#include "ps2hdd/hdl_export.hpp"

#include "ps2hdd/pfs_export.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <future>
#include <system_error>
#include <utility>

namespace ps2hdd {
namespace {

constexpr std::uint64_t kPvdOffset = 16ULL * hdl::kIsoSectorSize;
constexpr std::array<char, 5> kIso9660Id{'C', 'D', '0', '0', '1'};

struct ChunkRead {
    hdl::IsoReadResult result;
    std::uint64_t offset{};
};

void remove_quietly(const std::filesystem::path& path)
{
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

std::string default_hdl_iso_filename(const hdl::GameInfo& game)
{
    std::string title = game.title.empty() ? game.partition_id : game.title;
    std::string name = title;
    if (!game.startup.empty()) {
        name += " [" + game.startup + "]";
    }
    return pfs::sanitize_host_filename(name) + ".iso";
}

HdlIsoExportResult export_hdl_iso(BlockDevice& device, const hdl::GameLayout& layout,
                                  const std::filesystem::path& destination,
                                  const HdlIsoExportOptions& options,
                                  HdlIsoExportProgress progress, std::stop_token stop)
{
    HdlIsoExportResult result;
    const std::uint64_t total = layout.iso_size_bytes;
    const std::size_t chunk_bytes =
        std::max<std::size_t>(hdl::kIsoSectorSize,
                              options.chunk_bytes - options.chunk_bytes % hdl::kIsoSectorSize);

    std::error_code ec;
    if (std::filesystem::exists(destination, ec) && !options.overwrite) {
        result.error = "Destination already exists: " + destination.string();
        return result;
    }
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            result.error = "Could not create destination directory: " + ec.message();
            return result;
        }
    }

    auto partial = destination;
    partial += ".partial";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.error = "Could not create host file: " + partial.string();
        return result;
    }

    // Two buffers: one being filled by the async reader, one being written.
    std::vector<std::byte> buffers[2]{std::vector<std::byte>(chunk_bytes),
                                      std::vector<std::byte>(chunk_bytes)};
    const auto start_read = [&](int which, std::uint64_t offset) {
        return std::async(std::launch::async, [&device, &layout, &buffers, which, offset] {
            ChunkRead chunk;
            chunk.offset = offset;
            chunk.result = hdl::read_iso(device, layout, offset, buffers[which]);
            return chunk;
        });
    };

    const auto fail = [&](std::string message) {
        out.close();
        remove_quietly(partial);
        result.error = std::move(message);
        return result;
    };

    int current = 0;
    std::future<ChunkRead> pending;
    if (total > 0) {
        pending = start_read(current, 0);
    }
    std::uint64_t written = 0;
    while (pending.valid()) {
        ChunkRead chunk = pending.get();
        if (!chunk.result.ok) {
            return fail(chunk.result.error);
        }
        const std::uint64_t next = chunk.offset + chunk.result.bytes_read;
        if (stop.stop_requested()) {
            out.close();
            remove_quietly(partial);
            result.cancelled = true;
            result.error = "Export cancelled";
            return result;
        }
        if (next < total && chunk.result.bytes_read > 0) {
            pending = start_read(current ^ 1, next);
        }

        const auto& data = buffers[current];
        if (chunk.offset <= kPvdOffset && kPvdOffset + 6 <= next) {
            const auto* id = data.data() + (kPvdOffset - chunk.offset) + 1;
            result.iso9660_signature =
                std::memcmp(id, kIso9660Id.data(), kIso9660Id.size()) == 0;
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(chunk.result.bytes_read));
        if (!out) {
            if (pending.valid()) {
                pending.wait();
            }
            return fail("Could not write host file (disk full?): " + partial.string());
        }
        written = next;
        result.bytes_written = written;
        if (progress) {
            progress(written, total);
        }
        current ^= 1;
    }

    out.flush();
    out.close();
    if (!out) {
        return fail("Could not flush host file: " + partial.string());
    }
    if (written != total) {
        return fail("HDL export ended early");
    }
    if (options.overwrite) {
        std::filesystem::remove(destination, ec);
    }
    std::filesystem::rename(partial, destination, ec);
    if (ec) {
        return fail("Could not move completed export into place: " + ec.message());
    }

    if (!result.iso9660_signature) {
        result.warnings.push_back(
            "No ISO9660 volume descriptor at sector 16; the image may be damaged or non-standard");
    }
    result.ok = true;
    return result;
}

} // namespace ps2hdd
