#include "ps2hdd/hdl_image_install.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_allocation.hpp"
#include "ps2hdd/hdl.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void store_u32_both(std::byte* p, std::uint32_t value)
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
    p[4] = p[3];
    p[5] = p[2];
    p[6] = p[1];
    p[7] = p[0];
}

void write_iso_record(std::span<std::byte> target, std::uint32_t extent_lba,
                      std::uint32_t data_bytes, std::uint8_t flags,
                      std::string_view identifier)
{
    const std::size_t padding = identifier.size() % 2 == 0 ? 1 : 0;
    const std::size_t length = 33 + identifier.size() + padding;
    target[0] = static_cast<std::byte>(length);
    store_u32_both(target.data() + 2, extent_lba);
    store_u32_both(target.data() + 10, data_bytes);
    target[25] = static_cast<std::byte>(flags);
    target[28] = std::byte{1};
    target[31] = std::byte{1};
    target[32] = static_cast<std::byte>(identifier.size());
    std::memcpy(target.data() + 33, identifier.data(), identifier.size());
}

class LargeSparseIso final : public ps2hdd::BlockDevice {
public:
    static constexpr std::uint64_t kBytes = 130ULL * 1024ULL * 1024ULL;

    LargeSparseIso() : prefix_(24U * 2048U)
    {
        constexpr std::uint32_t root_lba = 20;
        constexpr std::uint32_t cnf_lba = 21;
        constexpr std::string_view cnf =
            "BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.00\r\n";

        auto pvd = std::span<std::byte>(prefix_).subspan(16U * 2048U, 2048U);
        pvd[0] = std::byte{1};
        std::memcpy(pvd.data() + 1, "CD001", 5);
        pvd[6] = std::byte{1};
        const std::string root_id(1, '\0');
        write_iso_record(pvd.subspan(156), root_lba, 2048, 0x02, root_id);

        auto root = std::span<std::byte>(prefix_).subspan(root_lba * 2048U, 2048U);
        const std::string dot(1, '\0');
        write_iso_record(root, root_lba, 2048, 0x02, dot);
        const auto used = std::to_integer<unsigned char>(root[0]);
        write_iso_record(root.subspan(used), cnf_lba,
                         static_cast<std::uint32_t>(cnf.size()), 0, "SYSTEM.CNF;1");
        std::memcpy(prefix_.data() + cnf_lba * 2048U, cnf.data(), cnf.size());
    }

    std::uint64_t size_bytes() const override { return kBytes; }
    std::string display_name() const override { return "130mib-sparse-game.iso"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > kBytes || out.size() > kBytes - offset) {
            return false;
        }
        std::fill(out.begin(), out.end(), std::byte{0});
        if (offset >= prefix_.size() || out.empty()) {
            return true;
        }
        const auto available = prefix_.size() - static_cast<std::size_t>(offset);
        const auto amount = std::min(out.size(), available);
        std::copy_n(prefix_.begin() + static_cast<std::ptrdiff_t>(offset), amount, out.begin());
        return true;
    }

private:
    std::vector<std::byte> prefix_;
};

class StreamingSparseDisk final : public ps2hdd::WritableBlockDevice {
public:
    explicit StreamingSparseDisk(std::uint64_t size) : size_(size) {}

    std::uint64_t size_bytes() const override { return size_; }
    std::string display_name() const override { return "multipart-target.img"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > size_ || out.size() > size_ - offset) {
            return false;
        }
        if (offset == stream_offset_ && out.size() == stream_bytes_.size()) {
            std::copy(stream_bytes_.begin(), stream_bytes_.end(), out.begin());
            return true;
        }
        const auto exact = small_.find({offset, out.size()});
        if (exact != small_.end()) {
            std::copy(exact->second.begin(), exact->second.end(), out.begin());
            return true;
        }
        std::fill(out.begin(), out.end(), std::byte{0});
        return true;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        if (offset > size_ || in.size() > size_ - offset) {
            return false;
        }
        if (in.size() > 4096U) {
            stream_offset_ = offset;
            stream_bytes_.assign(in.begin(), in.end());
        } else {
            small_[{offset, in.size()}] = std::vector<std::byte>(in.begin(), in.end());
        }
        return true;
    }

    bool flush() override { return true; }

    template <typename T>
    void put_object(std::uint64_t offset, const T& value)
    {
        const auto bytes = std::as_bytes(std::span{&value, 1});
        small_[{offset, sizeof(T)}] = std::vector<std::byte>(bytes.begin(), bytes.end());
    }

private:
    struct KeyLess {
        bool operator()(const std::pair<std::uint64_t, std::size_t>& left,
                        const std::pair<std::uint64_t, std::size_t>& right) const noexcept
        {
            return left < right;
        }
    };

    std::uint64_t size_{};
    std::map<std::pair<std::uint64_t, std::size_t>, std::vector<std::byte>, KeyLess> small_;
    std::uint64_t stream_offset_{static_cast<std::uint64_t>(-1)};
    std::vector<std::byte> stream_bytes_;
};

ps2hdd::apa::Header make_header(const char* id, std::uint16_t type,
                                std::uint32_t start, std::uint32_t length,
                                std::uint32_t prev, std::uint32_t next)
{
    ps2hdd::apa::Header header{};
    header.magic = ps2hdd::apa::kMagic;
    std::strncpy(header.id, id, sizeof(header.id) - 1);
    header.type = type;
    header.start = start;
    header.length = length;
    header.prev = prev;
    header.next = next;
    header.mbr.version = 2;
    header.checksum = ps2hdd::apa::checksum(header);
    return header;
}

StreamingSparseDisk make_fragmented_disk()
{
    const auto chunk = ps2hdd::apa::kAllocationChunkSectors;
    StreamingSparseDisk disk(static_cast<std::uint64_t>(16) * chunk * ps2hdd::apa::kSectorSize);

    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, chunk, 3 * chunk, chunk);
    constexpr char sony[] = "Sony Computer Entertainment Inc.";
    std::memcpy(mbr.mbr.magic, sony, sizeof(sony) - 1);
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    const auto opl = make_header("+OPL", ps2hdd::apa::kTypePfs,
                                 chunk, chunk, 0, 3 * chunk);
    const auto blocker = make_header("+BLOCK", ps2hdd::apa::kTypePfs,
                                     3 * chunk, chunk, chunk, 0);
    disk.put_object(0, mbr);
    disk.put_object(static_cast<std::uint64_t>(chunk) * ps2hdd::apa::kSectorSize, opl);
    disk.put_object(static_cast<std::uint64_t>(3 * chunk) * ps2hdd::apa::kSectorSize, blocker);
    return disk;
}

void full_installer_publishes_main_and_sub_relationship()
{
    auto disk = make_fragmented_disk();
    LargeSparseIso iso;

    ps2hdd::hdl::ImageInstallOptions options;
    options.title = "Multipart Frieren";
    options.media = ps2hdd::hdl::MediaType::dvd;
    options.copy_buffer_bytes = 4U * 1024U * 1024U;
    options.created.year = 2026;
    options.created.month = 8;
    options.created.day = 23;

    const auto result = ps2hdd::hdl::install_to_image(disk, iso, options);
    check(result.ok, "130 MiB install should publish successfully");
    check(result.sub_count == 1, "130 MiB fragmented install should require exactly one sub partition");

    ps2hdd::apa::Reader reader(disk);
    const auto scan = reader.scan();
    check(scan.ok(), "multipart install APA chain should rescan cleanly");

    const ps2hdd::apa::Partition* main = nullptr;
    const ps2hdd::apa::Partition* sub = nullptr;
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.id == result.partition_id) {
            main = &partition;
        }
        if (partition.is_sub() && partition.main_lba == result.main_start_lba) {
            sub = &partition;
        }
    }
    check(main != nullptr, "multipart install main partition should be visible");
    check(sub != nullptr, "multipart install sub partition should be visible");
    check(main->sub_count == 1 && main->sub_partitions.size() == 1,
          "main header should declare exactly one sub extent");
    check(main->sub_partitions[0].start == sub->start_lba &&
              main->sub_partitions[0].length == sub->length_sectors,
          "main subs[] table should match the published sub header");
    check(sub->main_lba == main->start_lba && sub->number == 1,
          "sub header should point back to main and carry number 1");

    const auto game = ps2hdd::hdl::read_game_info(disk, *main);
    check(game.ok, "normal HDL parser should accept multipart install metadata");
    check(game.game.declared_parts == 2,
          "DEADFEED allocation table should declare main plus one sub");
    check(game.game.allocation_table_consistent,
          "DEADFEED part count should agree with APA nsub relationship");
    check(game.game.raw_size_bytes == LargeSparseIso::kBytes,
          "multipart DEADFEED entries should cover the complete 130 MiB ISO");
}

} // namespace

int main()
{
    try {
        full_installer_publishes_main_and_sub_relationship();
        std::cout << "HDL multipart install tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HDL multipart install tests failed: " << error.what() << '\n';
        return 1;
    }
}
