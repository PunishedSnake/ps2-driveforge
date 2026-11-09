// Frieren: HDLoader game image (virtual ISO) regression tests.
//
// The fixture writes game 1's allocation table exactly the way hdl-dump does
// (shifted u32 writes at +0x0F5) and game 2/3's table in the loader-canonical
// form (+0x0F4). Both must decode to the same byte-exact ISO view.

#include "ps2hdd/apa.hpp"
#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/hdl.hpp"
#include "ps2hdd/hdl_export.hpp"
#include "ps2hdd/mount_view.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ps2hdd::apa::kSectorSize;

constexpr std::uint32_t kGame1Lba = 0x2000;
constexpr std::uint32_t kGame1Length = 0x3000; // 6 MiB main -> 2 MiB data
constexpr std::uint32_t kGame1SubLba = 0x5000;
constexpr std::uint32_t kGame1SubLength = 0x1000; // 2 MiB sub -> 1 MiB data
constexpr std::uint64_t kGame1SubData = 512U * 1024U;  // only half of the sub is used
constexpr std::uint32_t kGame2Lba = 0x6000;
constexpr std::uint32_t kGame2Length = 0x2800; // 5 MiB -> 1 MiB data
constexpr std::uint32_t kGame3Lba = 0x8800;
constexpr std::uint32_t kGame3Length = 0x2800;
constexpr std::uint32_t kEndLba = 0xB000;
constexpr std::uint64_t kGame1Iso = 2U * 1024U * 1024U + kGame1SubData;
constexpr std::uint64_t kGame2Iso = 1024U * 1024U - 2048U;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class MemoryDevice final : public ps2hdd::BlockDevice {
public:
    explicit MemoryDevice(std::size_t size) : bytes_(size) {}

    [[nodiscard]] std::uint64_t size_bytes() const override { return bytes_.size(); }
    [[nodiscard]] std::string display_name() const override { return "Frieren HDL fixture"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (fail_reads_ || offset > bytes_.size() ||
            out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(out.data(), bytes_.data() + static_cast<std::size_t>(offset), out.size());
        return true;
    }

    void put_bytes(std::uint64_t offset, std::span<const std::byte> raw)
    {
        check(offset + raw.size() <= bytes_.size(), "fixture write fits");
        std::memcpy(bytes_.data() + static_cast<std::size_t>(offset), raw.data(), raw.size());
    }

    template <typename T>
    void put(std::uint64_t offset, const T& value)
    {
        put_bytes(offset, std::as_bytes(std::span{&value, 1}));
    }

    void put_u32(std::uint64_t offset, std::uint32_t value)
    {
        const std::array<std::byte, 4> raw{
            static_cast<std::byte>(value & 0xFFU), static_cast<std::byte>((value >> 8U) & 0xFFU),
            static_cast<std::byte>((value >> 16U) & 0xFFU), static_cast<std::byte>(value >> 24U)};
        put_bytes(offset, raw);
    }

    [[nodiscard]] std::span<const std::byte> view(std::uint64_t offset, std::size_t size) const
    {
        return std::span<const std::byte>(bytes_).subspan(static_cast<std::size_t>(offset), size);
    }

    void fail_reads(bool value) { fail_reads_ = value; }

private:
    std::vector<std::byte> bytes_;
    bool fail_reads_{};
};

ps2hdd::apa::Header make_header(std::string_view id, std::uint16_t type, std::uint32_t start,
                                std::uint32_t length, std::uint32_t prev, std::uint32_t next)
{
    ps2hdd::apa::Header h{};
    h.magic = ps2hdd::apa::kMagic;
    std::memcpy(h.id, id.data(), std::min(id.size(), sizeof(h.id) - 1U));
    h.type = type;
    h.start = start;
    h.length = length;
    h.prev = prev;
    h.next = next;
    h.mbr.version = 2;
    return h;
}

void seal(ps2hdd::apa::Header& h) { h.checksum = ps2hdd::apa::checksum(h); }

// Deterministic per-ISO-offset pattern so slice ordering bugs are visible.
std::byte pattern(unsigned game, std::uint64_t iso_offset)
{
    const auto v = (iso_offset * 131U + (iso_offset >> 11U) * 7U + game * 97U) & 0xFFU;
    return static_cast<std::byte>(v);
}

std::vector<std::byte> expected_iso(unsigned game, std::uint64_t size)
{
    std::vector<std::byte> iso(static_cast<std::size_t>(size));
    for (std::uint64_t i = 0; i < size; ++i) {
        iso[static_cast<std::size_t>(i)] = pattern(game, i);
    }
    // ISO9660 primary volume descriptor at sector 16.
    constexpr std::array<char, 6> pvd{'\x01', 'C', 'D', '0', '0', '1'};
    std::memcpy(iso.data() + 16U * 2048U, pvd.data(), pvd.size());
    return iso;
}

void write_metadata(MemoryDevice& dev, std::uint32_t main_lba, std::string_view title,
                    std::string_view startup, std::uint8_t parts)
{
    const std::uint64_t base = static_cast<std::uint64_t>(main_lba) * kSectorSize +
                               ps2hdd::hdl::kMetadataOffset;
    dev.put_u32(base, ps2hdd::hdl::kMagic);
    dev.put_bytes(base + ps2hdd::hdl::kTitleOffset, std::as_bytes(std::span{title}));
    dev.put_bytes(base + ps2hdd::hdl::kStartupOffset, std::as_bytes(std::span{startup}));
    dev.put_u32(base + ps2hdd::hdl::kMediaOffset, 0x14U);
    dev.put_u32(base + ps2hdd::hdl::kPartCountOffset, parts);
}

std::uint64_t spec_offset(std::uint32_t main_lba, std::size_t index)
{
    return static_cast<std::uint64_t>(main_lba) * kSectorSize + ps2hdd::hdl::kMetadataOffset +
           ps2hdd::hdl::kPartSpecsOffset + index * ps2hdd::hdl::kPartSpecBytes;
}

// hdl-dump's writer: u32 pointer at +0x0F5, {offset in 1024-sector units,
// sector >> 8, size_in_kb * 4}.
void write_hdl_dump_spec(MemoryDevice& dev, std::uint32_t main_lba, std::size_t index,
                         std::uint32_t offset_units, std::uint32_t sector, std::uint32_t kb)
{
    const std::uint64_t base = static_cast<std::uint64_t>(main_lba) * kSectorSize +
                               ps2hdd::hdl::kMetadataOffset + ps2hdd::hdl::kAllocTableOffset +
                               index * ps2hdd::hdl::kAllocEntryBytes;
    dev.put_u32(base, offset_units);
    dev.put_u32(base + 4, sector >> 8U);
    dev.put_u32(base + 8, kb * 4U);
}

void write_canonical_spec(MemoryDevice& dev, std::uint32_t main_lba, std::size_t index,
                          std::uint32_t lsn, std::uint32_t sector, std::uint32_t bytes)
{
    const auto base = spec_offset(main_lba, index);
    dev.put_u32(base, lsn);
    dev.put_u32(base + 4, sector);
    dev.put_u32(base + 8, bytes);
}

void place_iso(MemoryDevice& dev, const std::vector<std::byte>& iso, std::uint64_t iso_offset,
               std::uint64_t device_offset, std::uint64_t length)
{
    dev.put_bytes(device_offset,
                  std::span<const std::byte>(iso).subspan(static_cast<std::size_t>(iso_offset),
                                                          static_cast<std::size_t>(length)));
}

std::unique_ptr<MemoryDevice> fixture()
{
    auto dev = std::make_unique<MemoryDevice>(static_cast<std::size_t>(kEndLba) * kSectorSize);

    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, kGame1Lba, kGame3Lba, kGame1Lba);
    constexpr char sony[] = "Sony Computer Entertainment Inc.";
    std::memcpy(mbr.mbr.magic, sony, std::min(sizeof(sony), sizeof(mbr.mbr.magic)));
    seal(mbr);

    auto main1 = make_header("PP.SLUS-20001..FIRST", ps2hdd::apa::kTypeHdl, kGame1Lba,
                             kGame1Length, 0, kGame1SubLba);
    main1.nsub = 1;
    main1.subs[0] = {kGame1SubLba, kGame1SubLength};
    seal(main1);
    auto sub1 = make_header("", ps2hdd::apa::kTypeHdl, kGame1SubLba, kGame1SubLength, kGame1Lba,
                            kGame2Lba);
    sub1.flags = ps2hdd::apa::kFlagSub;
    sub1.main = kGame1Lba;
    sub1.number = 1;
    seal(sub1);
    auto main2 = make_header("PP.SLES-50002..SECOND", ps2hdd::apa::kTypeHdl, kGame2Lba,
                             kGame2Length, kGame1SubLba, kGame3Lba);
    seal(main2);
    auto main3 = make_header("PP.SLES-50003..THIRD", ps2hdd::apa::kTypeHdl, kGame3Lba,
                             kGame3Length, kGame2Lba, 0);
    seal(main3);
    dev->put(0, mbr);
    dev->put(static_cast<std::uint64_t>(kGame1Lba) * kSectorSize, main1);
    dev->put(static_cast<std::uint64_t>(kGame1SubLba) * kSectorSize, sub1);
    dev->put(static_cast<std::uint64_t>(kGame2Lba) * kSectorSize, main2);
    dev->put(static_cast<std::uint64_t>(kGame3Lba) * kSectorSize, main3);

    // Game 1: written exactly like hdl-dump, two slices (main + sub).
    write_metadata(*dev, kGame1Lba, "First Game", "SLUS_200.01", 2);
    const std::uint32_t g1_main_data = kGame1Lba + 0x2000;
    const std::uint32_t g1_sub_data = kGame1SubLba + 0x0800;
    write_hdl_dump_spec(*dev, kGame1Lba, 0, 0, g1_main_data, 2048);
    write_hdl_dump_spec(*dev, kGame1Lba, 1, (kGame1Length - 0x2000) / 1024, g1_sub_data,
                        static_cast<std::uint32_t>(kGame1SubData / 1024U));
    const auto iso1 = expected_iso(1, kGame1Iso);
    place_iso(*dev, iso1, 0, static_cast<std::uint64_t>(g1_main_data) * kSectorSize,
              2U * 1024U * 1024U);
    place_iso(*dev, iso1, 2U * 1024U * 1024U, static_cast<std::uint64_t>(g1_sub_data) * kSectorSize,
              kGame1SubData);

    // Games 2 and 3: canonical table, identical titles to exercise alias suffixes.
    for (const auto [lba, game] : {std::pair{kGame2Lba, 2U}, std::pair{kGame3Lba, 3U}}) {
        write_metadata(*dev, lba, "Second Game", "SLES_500.02", 1);
        write_canonical_spec(*dev, lba, 0, 0, lba + 0x2000, static_cast<std::uint32_t>(kGame2Iso));
        place_iso(*dev, expected_iso(game, kGame2Iso), 0,
                  static_cast<std::uint64_t>(lba + 0x2000) * kSectorSize, kGame2Iso);
    }
    return dev;
}

ps2hdd::apa::ScanResult scan(MemoryDevice& dev)
{
    ps2hdd::apa::Reader reader(dev);
    auto result = reader.scan();
    check(result.ok(), "fixture APA scan is clean");
    return result;
}

const ps2hdd::apa::Partition& partition(const ps2hdd::apa::ScanResult& result, std::string_view id)
{
    for (const auto& p : result.partitions) {
        if (!p.is_sub() && p.id == id) {
            return p;
        }
    }
    throw std::runtime_error("fixture partition missing: " + std::string(id));
}

void layout_decodes_hdl_dump_tables()
{
    auto dev = fixture();
    const auto apa = scan(*dev);
    const auto layout = ps2hdd::hdl::read_game_layout(*dev, partition(apa, "PP.SLUS-20001..FIRST"));
    check(layout.ok, "hdl-dump-written layout decodes: " + layout.error);
    check(layout.layout.iso_size_bytes == kGame1Iso, "ISO size equals the sum of slices");
    check(layout.layout.game.raw_size_bytes == kGame1Iso,
          "legacy +0xF5 raw-size view agrees with canonical layout");
    check(layout.layout.extents.size() == 2, "two slices decoded");
    const auto& a = layout.layout.extents[0];
    const auto& b = layout.layout.extents[1];
    check(a.iso_offset == 0 && a.apa_extent == 0 &&
              a.device_offset == static_cast<std::uint64_t>(kGame1Lba + 0x2000) * kSectorSize,
          "slice 0 maps to main data area");
    check(b.iso_offset == a.length && b.apa_extent == 1 &&
              b.device_offset == static_cast<std::uint64_t>(kGame1SubLba + 0x800) * kSectorSize,
          "slice 1 maps to sub data area");

    const auto canonical = ps2hdd::hdl::read_game_layout(*dev, partition(apa, "PP.SLES-50002..SECOND"));
    check(canonical.ok && canonical.layout.iso_size_bytes == kGame2Iso,
          "canonical-only layout decodes");
}

void iso_reads_cross_slices_and_clamp()
{
    auto dev = fixture();
    const auto apa = scan(*dev);
    const auto layout = ps2hdd::hdl::read_game_layout(*dev, partition(apa, "PP.SLUS-20001..FIRST"));
    check(layout.ok, "layout for read test");
    const auto expected = expected_iso(1, kGame1Iso);

    // Straddle the main/sub boundary at 2 MiB with an odd offset/length.
    std::vector<std::byte> buffer(70001);
    const std::uint64_t offset = 2U * 1024U * 1024U - 33333U;
    auto read = ps2hdd::hdl::read_iso(*dev, layout.layout, offset, buffer);
    check(read.ok && read.bytes_read == buffer.size(), "cross-slice read succeeds");
    check(std::equal(buffer.begin(), buffer.end(), expected.begin() + static_cast<std::ptrdiff_t>(offset)),
          "cross-slice read is byte exact");

    read = ps2hdd::hdl::read_iso(*dev, layout.layout, kGame1Iso - 100, buffer);
    check(read.ok && read.bytes_read == 100, "read crossing EOF is clamped");
    read = ps2hdd::hdl::read_iso(*dev, layout.layout, kGame1Iso + 5, buffer);
    check(read.ok && read.bytes_read == 0, "read beyond EOF returns zero bytes");

    dev->fail_reads(true);
    read = ps2hdd::hdl::read_iso(*dev, layout.layout, 0, buffer);
    check(!read.ok && !read.error.empty(), "backing failure is reported");
}

void expect_layout_error(const std::function<void(MemoryDevice&)>& corrupt, std::string_view what)
{
    auto dev = fixture();
    const auto apa = scan(*dev);
    corrupt(*dev);
    const auto layout = ps2hdd::hdl::read_game_layout(*dev, partition(apa, "PP.SLUS-20001..FIRST"));
    check(!layout.ok && !layout.error.empty(), std::string("corrupted layout rejected: ") + std::string(what));
}

void corrupted_layouts_fail_closed()
{
    const std::uint32_t main_data = kGame1Lba + 0x2000;
    const std::uint32_t sub_data = kGame1SubLba + 0x800;
    const std::uint32_t lsn1 = 1024; // 2 MiB / 2048

    expect_layout_error([&](MemoryDevice& d) {
        write_canonical_spec(d, kGame1Lba, 0, 0, kGame1Lba, 2U * 1024U * 1024U);
    }, "slice inside the HDL reserved header window");
    expect_layout_error([&](MemoryDevice& d) {
        write_canonical_spec(d, kGame1Lba, 1, lsn1, kGame2Lba + 0x2000, 1024U * 1024U);
    }, "slice inside another game's partition");
    expect_layout_error([&](MemoryDevice& d) {
        write_canonical_spec(d, kGame1Lba, 1, lsn1 + 1, sub_data, 512U * 1024U);
    }, "non-contiguous ISO offset");
    expect_layout_error([&](MemoryDevice& d) {
        write_canonical_spec(d, kGame1Lba, 1, lsn1, sub_data, 512U * 1024U + 512U);
    }, "slice size not a whole ISO sector");
    expect_layout_error([&](MemoryDevice& d) {
        write_canonical_spec(d, kGame1Lba, 1, lsn1, sub_data, 0);
    }, "zero-length slice");
    expect_layout_error([&](MemoryDevice& d) {
        write_canonical_spec(d, kGame1Lba, 1, lsn1, main_data + 0x100, 512U * 1024U);
    }, "overlapping slices");
    expect_layout_error([&](MemoryDevice& d) {
        write_canonical_spec(d, kGame1Lba, 1, lsn1, sub_data, 2U * 1024U * 1024U);
    }, "slice running past its sub-partition");
    expect_layout_error([&](MemoryDevice& d) {
        write_canonical_spec(d, kGame1Lba, 1, lsn1, 0xFFFFFF00U, 512U * 1024U);
    }, "slice beyond the device");
    expect_layout_error([&](MemoryDevice& d) {
        d.put_u32(static_cast<std::uint64_t>(kGame1Lba) * kSectorSize + ps2hdd::hdl::kMetadataOffset, 0);
    }, "missing magic");
}

std::vector<std::byte> as_bytes_vector(std::vector<char> raw)
{
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

std::vector<std::byte> load(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>(in), {}};
    return as_bytes_vector(std::move(raw));
}

void export_streams_byte_exact_and_is_atomic()
{
    auto dev = fixture();
    const auto apa = scan(*dev);
    const auto layout = ps2hdd::hdl::read_game_layout(*dev, partition(apa, "PP.SLUS-20001..FIRST"));
    check(layout.ok, "layout for export");

    const auto dir = std::filesystem::temp_directory_path() / "ps2df-frieren-iso-tests";
    std::filesystem::remove_all(dir);
    const auto target = dir / ps2hdd::default_hdl_iso_filename(layout.layout.game);
    check(target.filename() == "First Game [SLUS_200.01].iso", "default filename format");

    std::uint64_t last = 0;
    std::size_t calls = 0;
    bool monotonic = true;
    ps2hdd::HdlIsoExportOptions options;
    options.chunk_bytes = 64U * 1024U + 1000U; // rounded down to whole sectors, many chunks
    const auto result = ps2hdd::export_hdl_iso(*dev, layout.layout, target, options,
                                               [&](std::uint64_t done, std::uint64_t total) {
                                                   monotonic = monotonic && done > last && total == kGame1Iso;
                                                   last = done;
                                                   ++calls;
                                               });
    check(result.ok, "export succeeds: " + result.error);
    check(result.iso9660_signature && result.warnings.empty(), "ISO9660 signature detected");
    check(monotonic && last == kGame1Iso && calls > 30, "progress is monotonic and complete");
    check(load(target) == expected_iso(1, kGame1Iso), "exported ISO is byte exact");
    check(!std::filesystem::exists(target.string() + ".partial"), "no partial file left behind");

    const auto again = ps2hdd::export_hdl_iso(*dev, layout.layout, target);
    check(!again.ok && again.error.find("exists") != std::string::npos,
          "existing destination is never overwritten by default");

    std::stop_source stop;
    stop.request_stop();
    const auto cancelled_target = dir / "cancelled.iso";
    const auto cancelled = ps2hdd::export_hdl_iso(*dev, layout.layout, cancelled_target, options, {},
                                                  stop.get_token());
    check(!cancelled.ok && cancelled.cancelled, "cancellation is reported");
    check(!std::filesystem::exists(cancelled_target) &&
              !std::filesystem::exists(cancelled_target.string() + ".partial"),
          "cancelled export leaves no files");

    dev->fail_reads(true);
    const auto failed_target = dir / "failed.iso";
    const auto failed = ps2hdd::export_hdl_iso(*dev, layout.layout, failed_target);
    check(!failed.ok && !std::filesystem::exists(failed_target) &&
              !std::filesystem::exists(failed_target.string() + ".partial"),
          "failed export leaves no files");
    std::filesystem::remove_all(dir);
}

void session_and_mount_expose_games()
{
    ps2hdd::DriveSession session(fixture());
    check(session.scan(), "session scan");

    const auto layouts = session.hdl_layouts();
    check(layouts.size() == 3 && layouts[0].ok &&
              layouts[0].layout.game.partition_id == "PP.SLUS-20001..FIRST",
          "session lists games in physical order");

    ps2hdd::ReadOnlyMountView view(session);
    const auto root = view.list_directory("\\");
    check(root.ok && root.entries.size() == 2 && root.entries[1].name == "Games",
          "mount root exposes Games when HDL partitions exist");

    const auto games = view.list_directory("\\Games");
    check(games.ok && games.entries.size() == 3, "Games lists every valid game");
    check(games.entries[0].name == "First Game [SLUS_200.01].iso" &&
              games.entries[1].name == "Second Game [SLES_500.02].iso" &&
              games.entries[2].name == "Second Game [SLES_500.02] (2).iso",
          "game aliases are deterministic and de-duplicated");
    check(!games.entries[0].is_directory() && games.entries[0].size == kGame1Iso,
          "virtual ISO is a sized regular file");

    const auto node = view.lookup("\\games\\first game [slus_200.01].ISO");
    check(node.ok && node.kind == ps2hdd::MountNodeKind::hdl_iso && !node.is_directory(),
          "case-insensitive lookup resolves the virtual ISO");
    check(!view.lookup("\\Games\\First Game [SLUS_200.01].iso\\x").ok,
          "cannot traverse through a virtual ISO");
    check(!view.lookup("\\Games\\missing.iso").ok, "missing game rejected");

    std::vector<std::byte> bytes(4096);
    const std::uint64_t offset = 2U * 1024U * 1024U - 2048U;
    const auto read = view.read_file("\\Games\\First Game [SLUS_200.01].iso", offset, bytes);
    const auto expected = expected_iso(1, kGame1Iso);
    check(read.ok && read.bytes_read == bytes.size() &&
              std::equal(bytes.begin(), bytes.end(), expected.begin() + static_cast<std::ptrdiff_t>(offset)),
          "mount read of virtual ISO is byte exact across slices");

    const auto dup = view.read_file("\\Games\\Second Game [SLES_500.02] (2).iso", 0, bytes);
    const auto third = expected_iso(3, kGame2Iso);
    check(dup.ok && std::equal(bytes.begin(), bytes.end(), third.begin()),
          "suffixed alias maps to the right partition");

    session.reset_stats();
    const auto dir = std::filesystem::temp_directory_path() / "ps2df-frieren-session";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto exported = session.export_hdl_iso("PP.SLUS-20001..FIRST", dir / "first.iso");
    check(exported.ok, "session export: " + exported.error);
    const auto stats = session.stats();
    check(stats.export_operations == 1 && stats.backing_io.bytes_requested >= kGame1Iso &&
              stats.read_ahead.prefetches == 0,
          "bulk export is counted as backing I/O and bypasses read-ahead");
    check(load(dir / "first.iso") == expected, "session export is byte exact");
    check(!session.export_hdl_iso("NOPE", dir / "x.iso").ok, "unknown partition rejected");
    std::filesystem::remove_all(dir);
}

void pfs_only_disks_have_no_games_folder()
{
    // Turn every HDL partition into a PFS type: the Games namespace must vanish.
    auto dev = fixture();
    auto header = make_header("+PFS", ps2hdd::apa::kTypePfs, kGame1Lba, kGame1Length, 0, 0);
    seal(header);
    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, kGame1Lba, kGame1Lba, kGame1Lba);
    constexpr char sony[] = "Sony Computer Entertainment Inc.";
    std::memcpy(mbr.mbr.magic, sony, std::min(sizeof(sony), sizeof(mbr.mbr.magic)));
    seal(mbr);
    dev->put(0, mbr);
    dev->put(static_cast<std::uint64_t>(kGame1Lba) * kSectorSize, header);
    ps2hdd::DriveSession session(std::move(dev));
    (void)session.scan();
    ps2hdd::ReadOnlyMountView view(session);
    const auto root = view.list_directory("\\");
    check(root.ok && root.entries.size() == 1 && root.entries[0].name == "Partitions",
          "PFS-only disk keeps the Darkness root namespace");
    check(!view.lookup("\\Games").ok, "Games namespace absent without HDL partitions");
}

} // namespace

int main()
{
    try {
        layout_decodes_hdl_dump_tables();
        iso_reads_cross_slices_and_clamp();
        corrupted_layouts_fail_closed();
        export_streams_byte_exact_and_is_atomic();
        session_and_mount_expose_games();
        pfs_only_disks_have_no_games_folder();
    } catch (const std::exception& ex) {
        std::cerr << "Test failure: " << ex.what() << '\n';
        return 1;
    }
    std::cout << "Frieren HDL ISO tests passed\n";
    return 0;
}
