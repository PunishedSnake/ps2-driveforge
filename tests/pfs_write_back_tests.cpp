// Frieren PFS write-back engine regressions ("driver layer").
//
// A real sparse image file backs both the read-only DriveSession and every
// writer the engine opens, exactly like a mounted image. The factory counts
// write-capability sessions so tests can prove application I/O never touches
// the disk directly.

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_allocation.hpp"
#include "ps2hdd/apa_fingerprint.hpp"
#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_write_back.hpp"
#include "ps2hdd/writable_file_block_device.hpp"
#include "ps2hdd/writable_mount_view.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

ps2hdd::apa::Header make_apa_header(const char* id, std::uint16_t type,
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

void put_pfs_dentry(std::span<std::byte> sector, std::size_t offset,
                    std::uint32_t inode, std::string_view name,
                    std::uint16_t mode, std::uint16_t allocated)
{
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset), &inode, sizeof(inode));
    sector[offset + 4] = std::byte{0};
    sector[offset + 5] = static_cast<std::byte>(name.size());
    const auto raw = static_cast<std::uint16_t>(mode | allocated);
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 6), &raw, sizeof(raw));
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 8), name.data(), name.size());
}


// Writes the same +OPL fixture as image_game_deploy_tests, into a sparse file.
class ImageBuilder {
public:
    explicit ImageBuilder(const std::filesystem::path& path) : path_(path)
    {
        { std::ofstream create(path, std::ios::binary | std::ios::trunc); }
        std::filesystem::resize_file(path, static_cast<std::uint64_t>(8) * chunk * ps2hdd::apa::kSectorSize);
        out_.open(path, std::ios::binary | std::ios::in | std::ios::out);
    }
    void put(std::uint64_t offset, std::span<const std::byte> bytes)
    {
        out_.seekp(static_cast<std::streamoff>(offset));
        out_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    template <typename T> void put_object(std::uint64_t offset, const T& value)
    {
        put(offset, std::as_bytes(std::span{&value, 1}));
    }

    static constexpr std::uint32_t chunk = ps2hdd::apa::kAllocationChunkSectors;
    std::filesystem::path path_;
    std::fstream out_;
};

void build_image(const std::filesystem::path& path)
{
    constexpr std::uint32_t chunk = ps2hdd::apa::kAllocationChunkSectors;
    constexpr std::uint32_t pfs_lba = chunk;
    constexpr std::uint32_t zone_size = 8192;
    constexpr std::uint32_t sectors_per_zone = zone_size / ps2hdd::apa::kSectorSize;
    constexpr std::uint32_t root_inode_zone = 100;
    constexpr std::uint32_t root_data_zone = 101;
    constexpr std::uint32_t bitmap_sector = 0x2010;
    ImageBuilder disk(path);

    auto mbr = make_apa_header("__mbr", ps2hdd::apa::kTypeMbr, 0, chunk, pfs_lba, pfs_lba);
    constexpr char sony[] = "Sony Computer Entertainment Inc.";
    std::memcpy(mbr.mbr.magic, sony, sizeof(sony) - 1);
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    disk.put_object(0, mbr);
    disk.put_object(static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize,
                    make_apa_header("+OPL", ps2hdd::apa::kTypePfs, pfs_lba, chunk, 0, 0));

    const auto base = static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize;
    ps2hdd::pfs::SuperBlock super{};
    super.magic = ps2hdd::pfs::kSuperMagic;
    super.version = ps2hdd::pfs::kFormatVersion;
    super.zone_size = zone_size;
    super.root = {root_inode_zone, 0, 1};
    super.log = {50, 0, 1};
    std::array<std::byte, ps2hdd::apa::kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &super, sizeof(super));
    disk.put(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize, super_sector);
    disk.put(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperBackupSector) * ps2hdd::apa::kSectorSize, super_sector);

    ps2hdd::pfs::Inode root{};
    root.magic = ps2hdd::pfs::kSegdMagic;
    root.inode_block = {root_inode_zone, 0, 1};
    root.last_segment = root.inode_block;
    root.data[0] = root.inode_block;
    root.data[1] = {root_data_zone, 0, 1};
    root.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 0x01FFU);
    root.attr = 0x00A0U;
    root.size = ps2hdd::apa::kSectorSize;
    root.number_blocks = 2;
    root.number_data = 2;
    root.number_segdesg = 1;
    root.checksum = ps2hdd::pfs::inode_checksum(root);
    disk.put_object(base + static_cast<std::uint64_t>(root_inode_zone) * zone_size, root);

    std::array<std::byte, ps2hdd::apa::kSectorSize> root_dir{};
    put_pfs_dentry(root_dir, 0, root_inode_zone, ".", ps2hdd::pfs::kModeDirectory, 12);
    put_pfs_dentry(root_dir, 12, root_inode_zone, "..", ps2hdd::pfs::kModeDirectory, 500);
    disk.put(base + static_cast<std::uint64_t>(root_data_zone) * zone_size, root_dir);

    std::array<std::byte, ps2hdd::pfs::kMetadataSize> bitmap{};
    const auto mark = [&](std::uint32_t zone) {
        bitmap[zone / 8U] = static_cast<std::byte>(std::to_integer<unsigned>(bitmap[zone / 8U]) | (1U << (zone % 8U)));
    };
    for (std::uint32_t zone = 0; zone <= 110; ++zone) mark(zone);
    mark(ps2hdd::pfs::kSuperSector / sectors_per_zone);
    mark(bitmap_sector / sectors_per_zone);
    disk.put(base + static_cast<std::uint64_t>(bitmap_sector) * ps2hdd::apa::kSectorSize, bitmap);
    std::array<std::byte, ps2hdd::pfs::kMetadataSize> second_bitmap{};
    disk.put(base + static_cast<std::uint64_t>(bitmap_sector + 2U) * ps2hdd::apa::kSectorSize, second_bitmap);
}

struct Rig {
    explicit Rig(std::string name, ps2hdd::WriteBackOptions options = {}, bool fresh = true)
        : root(std::filesystem::temp_directory_path() / ("ps2df-writeback-" + name))
    {
        if (fresh) {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root);
            build_image(root / "disk.img");
        }
        session = std::make_unique<ps2hdd::DriveSession>(std::make_unique<ps2hdd::FileBlockDevice>(root / "disk.img"));
        check(session->scan(), "fixture session scans");
        options.spool_directory = root / "spool";
        if (options.free_space_reserve_bytes == ps2hdd::WriteBackOptions{}.free_space_reserve_bytes) {
            options.free_space_reserve_bytes = 1024U * 1024U;
        }
        auto image = root / "disk.img";
        engine = std::make_unique<ps2hdd::PfsWriteBack>(
            *session,
            [this, image](std::string& error) -> std::unique_ptr<ps2hdd::WritableBlockDevice> {
                ++sessions_opened;
                if (fail_writes.load()) {
                    error = "simulated disk disappearance";
                    return nullptr;
                }
                auto device = std::make_unique<ps2hdd::WritableFileBlockDevice>(image);
                if (!device->is_open()) error = "cannot open image";
                return device->is_open() ? std::move(device) : nullptr;
            },
            options);
    }

    std::filesystem::path root;
    std::atomic<int> sessions_opened{0};
    std::atomic<bool> fail_writes{false};
    std::unique_ptr<ps2hdd::DriveSession> session;
    std::unique_ptr<ps2hdd::PfsWriteBack> engine;
};

std::vector<std::byte> payload(std::size_t size, unsigned seed)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::byte>((i * 31U + seed * 7U) & 0xFFU);
    return bytes;
}

// Explorer-style copy: open, 4 KiB writes, close.
ps2hdd::WriteStatus explorer_copy(ps2hdd::PfsWriteBack& engine, std::string_view path,
                                  const std::vector<std::byte>& bytes)
{
    ps2hdd::PfsWriteBack::Handle handle{};
    if (const auto status = engine.open_write("+OPL", path, true, false, handle); status != ps2hdd::WriteStatus::ok) {
        return status;
    }
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4096) {
        const auto n = std::min<std::size_t>(4096, bytes.size() - offset);
        if (const auto status = engine.write(handle, offset, std::span(bytes).subspan(offset, n));
            status != ps2hdd::WriteStatus::ok) {
            (void)engine.close(handle, true);
            return status;
        }
    }
    return engine.close(handle);
}

std::vector<std::byte> disk_read(ps2hdd::DriveSession& session, std::string_view path)
{
    const auto stat = session.stat("+OPL", path);
    if (!stat.ok) return {};
    std::vector<std::byte> bytes(static_cast<std::size_t>(stat.entry.size));
    const auto read = session.read_file("+OPL", path, 0, bytes);
    check(read.ok && read.bytes_read == bytes.size(), "disk read succeeds");
    return bytes;
}

void small_write_storm_is_batched()
{
    ps2hdd::WriteBackOptions options;
    options.commit_delay = std::chrono::hours(1); // only an explicit flush commits
    Rig rig("storm", options);
    auto& engine = *rig.engine;
    const int baseline = rig.sessions_opened.load();

    check(engine.make_directory("+OPL", "/ART") == ps2hdd::WriteStatus::ok, "mkdir ART");
    constexpr int kFiles = 600;
    for (int i = 0; i < kFiles; ++i) {
        const auto name = "/ART/SLUS_" + std::to_string(10000 + i) + "_ICO.png";
        check(explorer_copy(engine, name, payload(512U + static_cast<std::size_t>(i % 7) * 1500U,
                                                  static_cast<unsigned>(i))) == ps2hdd::WriteStatus::ok,
              "explorer copy accepted");
    }
    const auto before = engine.stats();
    check(rig.sessions_opened.load() == baseline, "application writes performed zero disk sessions");
    check(before.application_writes > static_cast<std::size_t>(kFiles), "many small application writes seen");
    check(before.pending_ops == static_cast<std::size_t>(kFiles) + 1, "every file is a single pending op");

    const auto listing = engine.pending_children("+OPL", "/ART");
    check(listing.size() == static_cast<std::size_t>(kFiles), "overlay lists uncommitted files");
    std::array<std::byte, 64> head{};
    std::size_t got = 0;
    check(engine.read_pending("+OPL", "/art/slus_10003_ico.PNG", 0, head, got) && got == head.size() &&
              std::memcmp(head.data(), payload(64, 3).data(), head.size()) == 0,
          "read-your-writes before commit, case-insensitively");

    check(engine.flush(std::chrono::minutes(5)), "flush commits the storm");
    const auto after = engine.stats();
    const auto batches = static_cast<std::size_t>(rig.sessions_opened.load() - baseline);
    check(after.committed_ops == static_cast<std::size_t>(kFiles) + 1 && after.failed_ops == 0, "all ops committed");
    check(batches <= (static_cast<std::size_t>(kFiles) + 1 + 255) / 256 + 1,
          "storm committed in a handful of batched write sessions");
    for (int i = 0; i < kFiles; i += 37) {
        const auto name = "/ART/SLUS_" + std::to_string(10000 + i) + "_ICO.png";
        check(disk_read(*rig.session, name) ==
                  payload(512U + static_cast<std::size_t>(i % 7) * 1500U, static_cast<unsigned>(i)),
              "committed file is byte exact on disk");
    }
}

void coalescing_and_overlay_semantics()
{
    ps2hdd::WriteBackOptions options;
    options.commit_delay = std::chrono::hours(1);
    Rig rig("coalesce", options);
    auto& engine = *rig.engine;
    using S = ps2hdd::WriteStatus;

    for (unsigned v = 0; v < 5; ++v) {
        check(explorer_copy(engine, "/conf_opl.cfg", payload(3000, v)) == S::ok, "rewrite accepted");
    }
    check(explorer_copy(engine, "/temp.tmp", payload(100, 9)) == S::ok, "temp file");
    check(engine.remove_file("+OPL", "/TEMP.TMP") == S::ok, "temp deleted before commit");
    check(!engine.pending("+OPL", "/temp.tmp"), "new+deleted file vanished from the queue");
    check(engine.make_directory("+OPL", "/New folder") == S::ok, "mkdir");
    check(engine.remove_directory("+OPL", "/New folder") == S::ok, "rmdir");
    check(engine.make_directory("+OPL", "/CFG") == S::ok, "mkdir CFG");
    check(engine.make_directory("+OPL", "/cfg") == S::exists, "case-insensitive collision");
    check(explorer_copy(engine, "/CFG/SLUS_123.45.cfg", payload(77, 1)) == S::ok, "cfg");
    check(engine.remove_directory("+OPL", "/CFG") == S::not_empty, "non-empty rmdir refused");
    check(explorer_copy(engine, "/missing/x.bin", payload(1, 1)) == S::not_found, "missing parent refused");

    const auto stats = engine.stats();
    check(stats.coalesced_ops >= 4 + 2, "rewrites, temp file and mkdir/rmdir coalesced");
    check(stats.pending_ops == 3, "only conf_opl.cfg, CFG and the cfg file remain");

    check(engine.flush(), "flush");
    check(disk_read(*rig.session, "/conf_opl.cfg") == payload(3000, 4), "last write wins on disk");
    check(!rig.session->stat("+OPL", "/temp.tmp").ok, "temp file never reached the disk");
    check(!rig.session->stat("+OPL", "/New folder").ok, "cancelled folder never reached the disk");
    check(disk_read(*rig.session, "/CFG/SLUS_123.45.cfg") == payload(77, 1), "nested file committed");
    check(engine.stats().committed_ops == 3, "exactly three disk operations");

    // Partial overwrite keeps the untouched bytes (seeded from disk).
    ps2hdd::PfsWriteBack::Handle handle{};
    check(engine.open_write("+OPL", "/conf_opl.cfg", false, false, handle) == S::ok, "reopen");
    const auto patch = payload(10, 99);
    check(engine.write(handle, 100, patch) == S::ok, "patch");
    check(engine.close(handle) == S::ok, "close");
    check(engine.flush(), "flush patch");
    auto expected = payload(3000, 4);
    std::memcpy(expected.data() + 100, patch.data(), patch.size());
    check(disk_read(*rig.session, "/conf_opl.cfg") == expected, "in-place patch preserved the rest");

    // Rename (copy-on-write move) and refused case-only rename.
    check(engine.rename("+OPL", "/conf_opl.cfg", "/CFG/conf_backup.cfg", false) == S::ok, "rename");
    check(engine.flush(), "flush rename");
    check(!rig.session->stat("+OPL", "/conf_opl.cfg").ok, "old name gone");
    check(disk_read(*rig.session, "/CFG/conf_backup.cfg") == expected, "new name has the data");

    // Delete of a committed file.
    check(engine.remove_file("+OPL", "/CFG/conf_backup.cfg") == S::ok, "delete committed");
    check(engine.pending("+OPL", "/CFG/conf_backup.cfg")->kind == ps2hdd::PendingNode::Kind::deleted,
          "overlay hides the deleted file immediately");
    check(engine.flush(), "flush delete");
    check(!rig.session->stat("+OPL", "/CFG/conf_backup.cfg").ok, "deleted on disk");
}

void failsafes_hold()
{
    using S = ps2hdd::WriteStatus;
    {
        ps2hdd::WriteBackOptions options;
        options.free_space_reserve_bytes = 1ULL << 40; // a reserve no fixture can satisfy
        Rig rig("reserve", options);
        check(rig.engine->available_bytes("+OPL") == 0U, "reserve hides all free space");
        check(explorer_copy(*rig.engine, "/big.bin", payload(64 * 1024, 1)) == S::no_space,
              "growth past the reserve is refused at write time");
    }
    {
        ps2hdd::WriteBackOptions options;
        options.max_pending_bytes = 32 * 1024;
        options.commit_delay = std::chrono::hours(1);
        Rig rig("backpressure", options);
        check(explorer_copy(*rig.engine, "/a.bin", payload(30 * 1024, 1)) == S::ok, "fits");
        check(explorer_copy(*rig.engine, "/b.bin", payload(30 * 1024, 2)) == S::busy, "queue full -> busy");
        check(rig.engine->flush(), "drains");
        check(explorer_copy(*rig.engine, "/b.bin", payload(30 * 1024, 2)) == S::ok, "accepted after drain");
    }
    {
        ps2hdd::WriteBackOptions options;
        options.max_file_bytes = 8192;
        Rig rig("too-large", options);
        check(explorer_copy(*rig.engine, "/x.bin", payload(9000, 1)) == S::too_large, "whole-file cap");
    }
    {
        ps2hdd::WriteBackOptions options;
        options.writable_partitions = {"__common"};
        Rig rig("protected", options);
        ps2hdd::PfsWriteBack::Handle handle{};
        check(!rig.engine->partition_writable("+OPL"), "allow-list excludes +OPL");
        check(rig.engine->open_write("+OPL", "/x", true, false, handle) == S::protected_partition,
              "protected partition refused");
        ps2hdd::WriteBackOptions defaults;
        check(defaults.protected_partitions.contains("__system"), "__system protected by default");
    }
}

void breaker_trips_and_journal_replays()
{
    using S = ps2hdd::WriteStatus;
    ps2hdd::WriteBackOptions options;
    options.commit_delay = std::chrono::milliseconds(10);
    options.max_ops_per_batch = 1;
    options.breaker_failed_batches = 1;
    {
        options.commit_delay = std::chrono::hours(1);
        Rig rig("breaker", options);
        rig.fail_writes = true;
        for (int i = 0; i < 3; ++i) {
            check(explorer_copy(*rig.engine, "/save" + std::to_string(i) + ".bin", payload(2048, i)) == S::ok,
                  "queued while disk is failing");
        }
        check(!rig.engine->flush(std::chrono::seconds(10)), "flush reports the failure");
        check(rig.engine->read_only(), "breaker tripped to read-only");
        check(!rig.engine->stats().last_error.empty(), "breaker explains why");
        check(explorer_copy(*rig.engine, "/later.bin", payload(10, 1)) == S::read_only,
              "no new writes once tripped");
        check(rig.engine->pending_children("+OPL", "/").empty(), "uncommitted changes no longer advertised");
        check(rig.sessions_opened.load() <= 2, "breaker stops hammering a failing disk");
    }
    // Next mount: the journal still holds the two writes that never ran.
    const auto spool = std::filesystem::temp_directory_path() / "ps2df-writeback-breaker" / "spool";
    const auto orphans = ps2hdd::PfsWriteBack::scan_journal(spool);
    check(orphans.size() == 2, "journal reports the uncommitted spool data");
    options.commit_delay = std::chrono::milliseconds(10);
    Rig rig("breaker", options, false);
    check(rig.engine->replay_orphans(orphans) == 2, "orphans replayed");
    check(rig.engine->flush(), "replayed writes commit");
    check(disk_read(*rig.session, "/save1.bin") == payload(2048, 1) &&
              disk_read(*rig.session, "/save2.bin") == payload(2048, 2),
          "recovered data is byte exact");
    check(ps2hdd::PfsWriteBack::scan_journal(spool).empty(), "journal clean after replay");
}

void debounced_background_commit()
{
    ps2hdd::WriteBackOptions options;
    options.commit_delay = std::chrono::milliseconds(50);
    Rig rig("debounce", options);
    check(explorer_copy(*rig.engine, "/auto.bin", payload(5000, 5)) == ps2hdd::WriteStatus::ok, "write");
    for (int i = 0; i < 100 && rig.engine->stats().committed_ops == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(rig.engine->stats().committed_ops == 1, "committer runs on its own after the quiet period");
    check(disk_read(*rig.session, "/auto.bin") == payload(5000, 5), "background commit is on disk");
}

void mount_view_overlays_pending_state()
{
    using S = ps2hdd::WriteStatus;
    ps2hdd::WriteBackOptions options;
    options.commit_delay = std::chrono::hours(1);
    Rig rig("mount", options);
    ps2hdd::WritableMountView view(*rig.session, *rig.engine);

    const auto root = view.resolve("\\Partitions\\+OPL");
    check(root.ok && root.partition == "+OPL" && root.pfs_path == "/" && root.is_directory && root.writable,
          "partition root resolves to a writable PFS directory");
    check(view.resolve("\\Partitions").partition.empty(), "namespace folders are never PFS targets");

    const auto art = view.resolve("\\Partitions\\+OPL\\ART");
    check(art.ok && !art.exists && art.pfs_path == "/ART", "missing child resolves through its parent");
    check(rig.engine->make_directory(art.partition, art.pfs_path) == S::ok, "mkdir via resolved target");
    const auto file = view.resolve("\\Partitions\\+OPL\\ART\\SLUS_200.01_COV.png");
    check(file.ok && !file.exists && file.pfs_path == "/ART/SLUS_200.01_COV.png", "file inside a pending folder");
    check(explorer_copy(*rig.engine, file.pfs_path, payload(9000, 3)) == S::ok, "copy into pending folder");

    const auto listing = view.list_directory("\\Partitions\\+OPL\\art");
    check(listing.ok && listing.entries.size() == 1 && listing.entries[0].name == "SLUS_200.01_COV.png" &&
              listing.entries[0].size == 9000,
          "Explorer sees the uncommitted file with its size");
    std::array<std::byte, 16> head{};
    const auto read = view.read_file("\\Partitions\\+OPL\\ART\\slus_200.01_cov.png", 0, head);
    check(read.ok && read.bytes_read == head.size() &&
              std::memcmp(head.data(), payload(16, 3).data(), head.size()) == 0,
          "Explorer reads uncommitted data");

    check(rig.engine->flush(), "flush");
    const auto after = view.list_directory("\\Partitions\\+OPL\\ART");
    check(after.ok && after.entries.size() == 1 && after.entries[0].size == 9000,
          "same listing after the commit (now from disk)");
    check(rig.engine->remove_file("+OPL", "/ART/SLUS_200.01_COV.png") == S::ok, "delete");
    check(!view.lookup("\\Partitions\\+OPL\\ART\\SLUS_200.01_COV.png").ok &&
              view.list_directory("\\Partitions\\+OPL\\ART").entries.empty(),
          "deleted file disappears from Explorer before the commit");
}

void layout_fingerprint_survives_pfs_commits()
{
    ps2hdd::WriteBackOptions options;
    options.commit_delay = std::chrono::milliseconds(5);
    Rig rig("fingerprint", options);
    const auto image = rig.root / "disk.img";
    const auto take = [&] {
        ps2hdd::FileBlockDevice device(image);
        ps2hdd::apa::Reader reader(device);
        return ps2hdd::apa_layout_fingerprint(reader.scan(), device.size_bytes());
    };
    const auto before = take();
    check(rig.engine->make_directory("+OPL", "/VMC") == ps2hdd::WriteStatus::ok, "mkdir");
    for (int i = 0; i < 40; ++i) {
        check(explorer_copy(*rig.engine, "/VMC/card" + std::to_string(i) + ".bin", payload(20000, i)) ==
                  ps2hdd::WriteStatus::ok,
              "write");
    }
    check(rig.engine->flush(), "flush");
    check(take() == before, "PFS commits never change the APA layout fingerprint");

    ps2hdd::FileBlockDevice device(image);
    ps2hdd::apa::Reader reader(device);
    auto scan = reader.scan();
    scan.partitions.back().length_sectors += 1;
    check(ps2hdd::apa_layout_fingerprint(scan, device.size_bytes()) != before,
          "a repartitioned disk changes the fingerprint");
    check(ps2hdd::apa_layout_fingerprint(reader.scan(), device.size_bytes() + 512) != before,
          "a different-size disk changes the fingerprint");
}

void large_file_commits_from_mapped_spool()
{
    // Larger than the fixture's 256 KiB default I/O batches by far and
    // spanning many zones, committed straight from the memory-mapped spool.
    ps2hdd::WriteBackOptions options;
    options.commit_delay = std::chrono::milliseconds(5);
    Rig rig("large", options);
    const auto big = payload(48U * 1024U * 1024U + 777U, 42);
    ps2hdd::PfsWriteBack::Handle handle{};
    check(rig.engine->open_write("+OPL", "/game.vcd", true, true, handle) == ps2hdd::WriteStatus::ok, "open");
    for (std::size_t offset = 0; offset < big.size(); offset += 1024U * 1024U) {
        const auto n = std::min<std::size_t>(1024U * 1024U, big.size() - offset);
        check(rig.engine->write(handle, offset, std::span(big).subspan(offset, n)) == ps2hdd::WriteStatus::ok,
              "1 MiB writes");
    }
    check(rig.engine->close(handle) == ps2hdd::WriteStatus::ok, "close");
    check(rig.engine->flush(std::chrono::minutes(5)), "large commit");
    check(disk_read(*rig.session, "/game.vcd") == big, "48 MiB file is byte exact on disk");
    check(ps2hdd::WriteBackOptions{}.max_file_bytes >= 4ULL * 1024ULL * 1024ULL * 1024ULL,
          "default whole-file limit allows VCD-sized and larger files");
}

void case_only_renames()
{
    using S = ps2hdd::WriteStatus;
    ps2hdd::WriteBackOptions options;
    options.commit_delay = std::chrono::hours(1);
    Rig rig("case-rename", options);
    auto& engine = *rig.engine;
    ps2hdd::WritableMountView view(*rig.session, engine);

    check(engine.make_directory("+OPL", "/art") == S::ok, "mkdir art");
    check(explorer_copy(engine, "/art/cover.png", payload(5000, 1)) == S::ok, "file");
    check(explorer_copy(engine, "/readme.txt", payload(300, 2)) == S::ok, "file");
    check(engine.flush(), "commit originals");

    // File on disk: in-place dentry rewrite.
    check(engine.rename("+OPL", "/readme.txt", "/README.TXT", false) == S::ok, "file case rename queued");
    const auto root = view.list_directory("\\Partitions\\+OPL");
    check(std::any_of(root.entries.begin(), root.entries.end(), [](const auto& e) { return e.name == "README.TXT"; }),
          "Explorer shows the new spelling before the commit");

    // Directory on disk, plus a change queued inside it with the OLD spelling
    // (exactly what Explorer does with a path it resolved a moment earlier).
    check(engine.rename("+OPL", "/art", "/ART", false) == S::ok, "dir case rename queued");
    check(explorer_copy(engine, "/art/icon.png", payload(700, 3)) == S::ok, "write under old spelling");
    check(view.list_directory("\\Partitions\\+OPL\\ART").entries.size() == 2, "renamed folder lists both files");

    check(engine.flush(), "commit renames");
    check(rig.session->stat("+OPL", "/README.TXT").ok && !rig.session->stat("+OPL", "/readme.txt").ok,
          "file renamed on disk, no duplicate");
    check(disk_read(*rig.session, "/README.TXT") == payload(300, 2), "renamed file content intact");
    check(rig.session->stat("+OPL", "/ART").ok && !rig.session->stat("+OPL", "/art").ok, "directory renamed");
    check(disk_read(*rig.session, "/ART/cover.png") == payload(5000, 1) &&
              disk_read(*rig.session, "/ART/icon.png") == payload(700, 3),
          "old and newly queued children live under the new spelling");

    // A queued, never-committed file renames without any disk work.
    check(explorer_copy(engine, "/new.cfg", payload(10, 4)) == S::ok, "pending file");
    const auto committed = engine.stats().committed_ops;
    check(engine.rename("+OPL", "/new.cfg", "/NEW.cfg", false) == S::ok, "rename pending file");
    check(engine.flush(), "flush");
    check(engine.stats().committed_ops == committed + 1 && rig.session->stat("+OPL", "/NEW.cfg").ok &&
              !rig.session->stat("+OPL", "/new.cfg").ok,
          "pending file created directly under the new spelling");

    // Renaming back before the commit cancels the operation entirely.
    check(engine.rename("+OPL", "/README.TXT", "/Readme.txt", false) == S::ok, "rename");
    check(engine.rename("+OPL", "/Readme.txt", "/README.TXT", false) == S::ok, "rename back");
    check(engine.stats().pending_ops == 0, "round-trip case rename cancels out");
}

} // namespace

int main()
{
    try {
        small_write_storm_is_batched();
        coalescing_and_overlay_semantics();
        failsafes_hold();
        breaker_trips_and_journal_replays();
        debounced_background_commit();
        mount_view_overlays_pending_state();
        layout_fingerprint_survives_pfs_commits();
        large_file_commits_from_mapped_spool();
        case_only_renames();
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PFS write-back tests passed\n";
    return 0;
}
