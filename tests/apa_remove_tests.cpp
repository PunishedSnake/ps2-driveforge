#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_remove.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Disk final : public ps2hdd::WritableBlockDevice {
public:
    explicit Disk(std::size_t bytes) : bytes_(bytes) {}
    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "apa-remove-fixture.img"; }
    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(out.data(), bytes_.data() + static_cast<std::size_t>(offset), out.size());
        return true;
    }
    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        if (offset > bytes_.size() || in.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(bytes_.data() + static_cast<std::size_t>(offset), in.data(), in.size());
        ++writes;
        written_bytes += in.size();
        return true;
    }
    bool flush() override { ++flushes; return true; }

    template <typename T>
    void put(std::uint32_t lba, const T& value)
    {
        check(write(static_cast<std::uint64_t>(lba) * ps2hdd::apa::kSectorSize,
                    std::as_bytes(std::span{&value, 1})),
              "fixture header write");
    }

    void reset_counters()
    {
        writes = 0;
        flushes = 0;
        written_bytes = 0;
    }

    std::size_t writes{};
    std::size_t flushes{};
    std::uint64_t written_bytes{};

private:
    std::vector<std::byte> bytes_;
};

ps2hdd::apa::Header header(const char* id, std::uint16_t type,
                           std::uint32_t start, std::uint32_t length,
                           std::uint32_t prev, std::uint32_t next)
{
    ps2hdd::apa::Header h{};
    h.magic = ps2hdd::apa::kMagic;
    std::strncpy(h.id, id, sizeof(h.id) - 1);
    h.type = type;
    h.start = start;
    h.length = length;
    h.prev = prev;
    h.next = next;
    h.mbr.version = 2;
    h.checksum = ps2hdd::apa::checksum(h);
    return h;
}

void remove_main_and_sub_without_touching_payload()
{
    constexpr std::uint32_t game = 0x40000;
    constexpr std::uint32_t sub = 0x80000;
    constexpr std::uint32_t opl = 0xC0000;
    constexpr std::uint32_t extent = 0x40000;

    Disk disk(static_cast<std::size_t>(opl + extent + 0x1000) * ps2hdd::apa::kSectorSize);
    auto mbr = header("__mbr", ps2hdd::apa::kTypeMbr, 0, extent, opl, game);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr);

    auto main = header("PP.SLUS-12345.Game", ps2hdd::apa::kTypeHdl,
                       game, extent, 0, sub);
    main.nsub = 1;
    main.subs[0] = {sub, extent};
    main.checksum = ps2hdd::apa::checksum(main);

    auto child = header("PP.SLUS-12345.Game", ps2hdd::apa::kTypeHdl,
                        sub, extent, game, opl);
    child.flags = ps2hdd::apa::kFlagSub;
    child.main = game;
    child.number = 1;
    child.checksum = ps2hdd::apa::checksum(child);

    auto pfs = header("+OPL", ps2hdd::apa::kTypePfs, opl, extent, sub, 0);

    disk.put(0, mbr);
    disk.put(game, main);
    disk.put(sub, child);
    disk.put(opl, pfs);

    ps2hdd::apa::Reader reader(disk);
    const auto before = reader.scan();
    check(before.ok() && before.partitions.size() == 4, "fixture chain before removal");

    const auto plan = ps2hdd::apa::plan_remove_main_partition(before, game);
    check(plan.ok, "APA removal plan");
    check(plan.removed_lbas.size() == 2, "main plus one sub planned for removal");
    check(plan.link_rewrites.size() == 2, "only MBR and surviving +OPL need relinking");
    check(plan.freed_bytes == 2ULL * extent * ps2hdd::apa::kSectorSize,
          "freed byte accounting");

    auto cached = before;
    disk.reset_counters();
    check(ps2hdd::apa::apply_remove_plan_to_scan(cached, plan),
          "cached APA snapshot accepts validated removal plan");
    check(disk.writes == 0 && disk.flushes == 0,
          "cached APA snapshot update performs zero device I/O");
    check(cached.partitions.size() == 2,
          "cached APA snapshot drops main and sub immediately");
    check(cached.partitions[0].next_lba == opl && cached.partitions[0].prev_lba == opl &&
              cached.partitions[1].prev_lba == 0 && cached.partitions[1].next_lba == 0,
          "cached APA snapshot gets the same canonical links as disk mutation");

    disk.reset_counters();
    const auto result = ps2hdd::apa::remove_main_partition_from_image(disk, game);
    check(result.ok, "transactional APA removal succeeds");
    check(result.removed_headers == 2 && result.rewritten_headers == 2,
          "removal result counts");
    check(disk.writes == plan.link_rewrites.size(),
          "fast removal writes only surviving headers whose links change");
    check(disk.written_bytes == plan.link_rewrites.size() * sizeof(ps2hdd::apa::Header),
          "fast removal write volume is independent of removed payload size");
    check(disk.flushes == 1, "fast removal uses one transaction flush");

    const auto after = reader.scan();
    check(after.ok() && after.partitions.size() == 2, "chain contains only MBR + +OPL");
    check(after.partitions[0].next_lba == opl && after.partitions[0].prev_lba == opl,
          "MBR links directly to surviving partition");
    check(after.partitions[1].prev_lba == 0 && after.partitions[1].next_lba == 0,
          "surviving +OPL links are canonical");
    check(cached.partitions[0].prev_lba == after.partitions[0].prev_lba &&
              cached.partitions[0].next_lba == after.partitions[0].next_lba &&
              cached.partitions[1].prev_lba == after.partitions[1].prev_lba &&
              cached.partitions[1].next_lba == after.partitions[1].next_lba,
          "zero-I/O cached snapshot matches conservative disk rescan");

    ps2hdd::apa::Header stale_main{};
    ps2hdd::apa::Header stale_sub{};
    check(disk.read(static_cast<std::uint64_t>(game) * ps2hdd::apa::kSectorSize,
                    std::as_writable_bytes(std::span{&stale_main, 1})),
          "read unreachable main header");
    check(disk.read(static_cast<std::uint64_t>(sub) * ps2hdd::apa::kSectorSize,
                    std::as_writable_bytes(std::span{&stale_sub, 1})),
          "read unreachable sub header");
    check(stale_main.magic == ps2hdd::apa::kMagic && stale_sub.magic == ps2hdd::apa::kMagic,
          "fast removal does not waste time zero-filling detached extents");
}

void remove_tail_updates_mbr_prev()
{
    constexpr std::uint32_t first = 0x40000;
    constexpr std::uint32_t tail = 0x80000;
    constexpr std::uint32_t extent = 0x40000;
    Disk disk(static_cast<std::size_t>(tail + extent + 0x1000) * ps2hdd::apa::kSectorSize);

    auto mbr = header("__mbr", ps2hdd::apa::kTypeMbr, 0, extent, tail, first);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    auto a = header("+OPL", ps2hdd::apa::kTypePfs, first, extent, 0, tail);
    auto b = header("PP.TAIL", ps2hdd::apa::kTypeHdl, tail, extent, first, 0);
    disk.put(0, mbr);
    disk.put(first, a);
    disk.put(tail, b);

    disk.reset_counters();
    const auto result = ps2hdd::apa::remove_main_partition_from_image(disk, tail);
    check(result.ok, "tail removal succeeds");
    check(disk.writes == 2 && disk.written_bytes == 2 * sizeof(ps2hdd::apa::Header),
          "tail removal remains two-header metadata work");

    ps2hdd::apa::Reader reader(disk);
    const auto scan = reader.scan();
    check(scan.ok() && scan.partitions.size() == 2, "tail removed from reachable chain");
    check(scan.partitions[0].prev_lba == first && scan.partitions[0].next_lba == first,
          "MBR last pointer updated to new tail");
    check(scan.partitions[1].prev_lba == 0 && scan.partitions[1].next_lba == 0,
          "new tail terminates chain");
}

} // namespace

int main()
{
    try {
        remove_main_and_sub_without_touching_payload();
        remove_tail_updates_mbr_prev();
        std::cout << "APA fast removal tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "APA fast removal test failure: " << error.what() << '\n';
        return 1;
    }
}
