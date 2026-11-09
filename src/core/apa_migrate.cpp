#include "ps2hdd/apa_migrate.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/disk_layout_guard.hpp"
#include "ps2hdd/fhdb_rescue.hpp"
#include "ps2hdd/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace ps2hdd::migrate {
namespace {

constexpr std::uint64_t kMasterBytes = 1024;

bool all_zero(std::span<const std::byte> data)
{
    return std::all_of(data.begin(), data.end(), [](std::byte b) { return b == std::byte{0}; });
}

} // namespace

Plan plan(BlockDevice& source, std::uint64_t target_bytes)
{
    Plan result;
    result.source_bytes = source.size_bytes();
    result.target_bytes = target_bytes;
    if (!source.read(0, result.master)) {
        result.error = "Could not read the source APA master";
        return result;
    }
    if (fhdb::is_apa_jail_master(result.master)) {
        result.error = "APA-Jail drives keep an exFAT partition at a fixed offset after the APA region; "
                       "migrate the exFAT side with a PC tool and use a fresh APA-Jail install on the new drive";
        return result;
    }
    const auto layout = inspect_disk_layout(source);
    if (!layout.ok || !layout.allows_ps2_mutation() || fhdb::is_pc_owned_master(result.master)) {
        result.error = "Source carries PC partition-map ownership; refusing to treat it as a PS2 HDD";
        return result;
    }
    apa::Reader reader(source);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        result.error = "Source APA chain is not clean; run the health check and repair it before migrating";
        return result;
    }

    std::vector<Range> ranges;
    for (const auto& partition : scan.partitions) {
        const auto offset = std::uint64_t{partition.start_lba} * apa::kSectorSize;
        const auto length = std::uint64_t{partition.length_sectors} * apa::kSectorSize;
        result.required_bytes = std::max(result.required_bytes, offset + length);
        const bool free_space = partition.type == apa::kTypeFree;
        ranges.push_back({offset, free_space ? std::uint64_t{apa::kHeaderSize} : length, partition.id});
        if (!free_space) ++result.partitions;
        // Sub extents normally appear as their own chain headers too; listing
        // them from the main as well guarantees they are copied either way.
        for (std::size_t i = 0; i < partition.sub_partitions.size(); ++i) {
            const auto& sub = partition.sub_partitions[i];
            const auto sub_offset = std::uint64_t{sub.start} * apa::kSectorSize;
            const auto sub_bytes = std::uint64_t{sub.length} * apa::kSectorSize;
            result.required_bytes = std::max(result.required_bytes, sub_offset + sub_bytes);
            ranges.push_back({sub_offset, sub_bytes, partition.id + " sub " + std::to_string(i + 1)});
        }
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
        return a.offset != b.offset ? a.offset < b.offset : a.bytes > b.bytes;
    });
    ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
        return a.offset == b.offset && a.bytes == b.bytes;
    }), ranges.end());
    for (auto& range : ranges) {
        if (!result.ranges.empty()) {
            auto& last = result.ranges.back();
            if (range.offset < last.offset + last.bytes) {
                result.error = "Overlapping APA partitions at " + range.what + "; refusing to migrate";
                return result;
            }
            if (range.offset == last.offset + last.bytes) {
                last.bytes += range.bytes;
                if (last.what.size() < 200) last.what += ", " + range.what;
                continue;
            }
        }
        result.ranges.push_back(range);
    }
    for (const auto& range : result.ranges) result.copy_bytes += range.bytes;

    if (target_bytes < result.required_bytes) {
        result.error = "Target is too small: the last APA partition ends at " + std::to_string(result.required_bytes) +
                       " bytes, target has " + std::to_string(target_bytes);
        return result;
    }
    if (target_bytes < result.source_bytes)
        result.warnings.push_back("Target is smaller than the source; everything allocated fits, but less free space remains");
    if (target_bytes > (std::uint64_t{1} << 41))
        result.warnings.push_back("Target is larger than 2 TiB: classic 28/48-bit APA tooling may not address all of it");
    result.ok = true;
    return result;
}

Result apply(BlockDevice& source, WritableBlockDevice& target, const Plan& plan, const Options& options,
             Progress progress, std::stop_token stop)
{
    Result result;
    if (!plan.ok) {
        result.error = "Migration plan is not valid";
        return result;
    }
    if (target.size_bytes() < plan.required_bytes) {
        result.error = "Target shrank below the planned size";
        return result;
    }
    std::array<std::byte, kMasterBytes> live{};
    if (!source.read(0, live) || live != plan.master) {
        result.error = "Source APA master changed after planning; refusing a stale migration plan";
        return result;
    }

    const std::size_t chunk = std::max<std::size_t>(options.chunk_bytes, 64U * 1024U);
    std::vector<std::byte> buffer(chunk);
    std::vector<std::byte> verify_buffer;
    std::vector<crypto::Sha256Digest> source_hashes;
    std::uint64_t done = 0;

    // Pass 1: everything except the master. The master's range starts at 0;
    // its first 1024 bytes are deferred.
    for (const auto& range : plan.ranges) {
        crypto::Sha256Context hash;
        crypto::sha256_init(hash);
        std::uint64_t position = 0;
        while (position < range.bytes) {
            if (stop.stop_requested()) {
                result.cancelled = true;
                result.error = "Migration cancelled; the target has no APA master and is not a usable PS2 disk yet";
                return result;
            }
            const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, range.bytes - position));
            const auto offset = range.offset + position;
            auto data = std::span(buffer).first(take);
            if (!source.read(offset, data)) {
                result.error = "Source read failed at byte " + std::to_string(offset);
                return result;
            }
            if (options.verify) crypto::sha256_update(hash, data);
            std::uint64_t write_offset = offset;
            auto write = std::span<const std::byte>(data);
            if (offset < kMasterBytes) {
                const auto skip = static_cast<std::size_t>(kMasterBytes - offset);
                write = write.subspan(std::min(skip, write.size()));
                write_offset = kMasterBytes;
            }
            if (!write.empty()) {
                if (options.target_is_zeroed && all_zero(write)) {
                    result.skipped_zero_bytes += write.size();
                } else if (!target.write(write_offset, write)) {
                    result.error = "Target write failed at byte " + std::to_string(write_offset);
                    return result;
                }
            }
            position += take;
            done += take;
            result.copied_bytes += take;
            if (progress) progress(done, plan.copy_bytes);
        }
        source_hashes.push_back(crypto::sha256_final(hash));
    }
    if (!target.flush()) {
        result.error = "Target flush failed before the APA master was written";
        return result;
    }

    if (options.verify) {
        verify_buffer.resize(chunk);
        for (std::size_t i = 0; i < plan.ranges.size(); ++i) {
            const auto& range = plan.ranges[i];
            crypto::Sha256Context hash;
            crypto::sha256_init(hash);
            std::uint64_t position = 0;
            while (position < range.bytes) {
                if (stop.stop_requested()) {
                    result.cancelled = true;
                    result.error = "Migration cancelled during verification; the APA master was not written";
                    return result;
                }
                const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, range.bytes - position));
                const auto offset = range.offset + position;
                auto data = std::span(verify_buffer).first(take);
                if (!target.read(offset, data)) {
                    result.error = "Target read-back failed at byte " + std::to_string(offset);
                    return result;
                }
                // The deferred master reads back whatever the target held;
                // substitute the planned bytes so the hash covers the final image.
                if (offset < kMasterBytes) {
                    const auto overlap = static_cast<std::size_t>(std::min<std::uint64_t>(kMasterBytes - offset, take));
                    std::copy_n(plan.master.begin() + static_cast<std::ptrdiff_t>(offset), overlap, data.begin());
                }
                crypto::sha256_update(hash, data);
                position += take;
            }
            if (crypto::sha256_final(hash) != source_hashes[i]) {
                result.error = "Target verification failed for " + range.what + "; the APA master was not written";
                return result;
            }
        }
        result.verified = true;
    }

    // Pass 2: publish the master last.
    if (!target.write(0, plan.master) || !target.flush()) {
        result.error = "Writing the APA master to the target failed; the target is not a usable PS2 disk";
        return result;
    }
    std::array<std::byte, kMasterBytes> readback{};
    if (!target.read(0, readback) || readback != plan.master) {
        result.error = "Target APA master failed read-back verification";
        return result;
    }
    result.master_written = true;
    apa::Reader reader(target);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        result.error = "The migrated disk failed a full APA parse";
        return result;
    }
    result.ok = true;
    return result;
}

bool create_sparse_image(const std::filesystem::path& path, std::uint64_t bytes, std::string& error)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        error = path.string() + " already exists; refusing to overwrite it";
        return false;
    }
    {
        std::ofstream create(path, std::ios::binary);
        if (!create) {
            error = "Could not create " + path.string();
            return false;
        }
    }
    std::filesystem::resize_file(path, bytes, ec);
    if (ec) {
        error = "Could not size " + path.string() + ": " + ec.message();
        std::filesystem::remove(path, ec);
        return false;
    }
    return true;
}

} // namespace ps2hdd::migrate
