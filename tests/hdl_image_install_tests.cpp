#include "ps2hdd/hdl_image_install.hpp"

#include "ps2hdd/apa_allocation.hpp"
#include "ps2hdd/recovery_capsule.hpp"
#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

class MemoryIso final : public ps2hdd::BlockDevice {
public:
    explicit MemoryIso(std::size_t bytes) : bytes_(bytes) {}
    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "synthetic-game.iso"; }
    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - offset) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }
    std::span<std::byte> bytes() { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

class SparseWritableDisk final : public ps2hdd::WritableBlockDevice {
public:
    explicit SparseWritableDisk(std::uint64_t size) : size_(size) {}
    std::uint64_t size_bytes() const override { return size_; }
    std::string display_name() const override { return "synthetic-ps2.img"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        const auto exact = chunks_.find({offset, out.size()});
        if (exact != chunks_.end()) {
            std::memcpy(out.data(), exact->second.data(), out.size());
            return true;
        }
        // Test fixture models untouched free space as zero-filled. This keeps
        // arbitrary transaction before-image reads deterministic without
        // allocating a multi-gigabyte dense buffer.
        if (offset <= size_ && out.size() <= size_ - offset) {
            std::fill(out.begin(), out.end(), std::byte{0});
            return true;
        }
        return false;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        ++write_calls;
        if (offset > size_ || in.size() > size_ - offset) {
            return false;
        }
        auto bytes = std::vector<std::byte>(in.begin(), in.end());
        if (corrupt_write_call != 0 && write_calls == corrupt_write_call && !bytes.empty()) {
            bytes[0] ^= std::byte{0x40};
        }
        chunks_[{offset, in.size()}] = std::move(bytes);
        return true;
    }

    bool flush() override
    {
        ++flush_calls;
        return true;
    }

    void put(std::uint64_t offset, std::span<const std::byte> bytes)
    {
        chunks_[{offset, bytes.size()}] = std::vector<std::byte>(bytes.begin(), bytes.end());
    }

    template <typename T>
    void put_object(std::uint64_t offset, const T& value)
    {
        put(offset, std::as_bytes(std::span{&value, 1}));
    }

    std::size_t write_calls{};
    std::size_t flush_calls{};
    std::size_t corrupt_write_call{};

private:
    struct KeyLess {
        bool operator()(const std::pair<std::uint64_t, std::size_t>& left,
                        const std::pair<std::uint64_t, std::size_t>& right) const noexcept
        {
            return left < right;
        }
    };
    std::uint64_t size_{};
    std::map<std::pair<std::uint64_t, std::size_t>, std::vector<std::byte>, KeyLess> chunks_;
};

void write_record(std::span<std::byte> target, std::uint32_t extent_lba,
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

MemoryIso make_game_iso()
{
    constexpr std::uint32_t root_lba = 20;
    constexpr std::uint32_t cnf_lba = 21;
    constexpr std::size_t sectors = 24;
    constexpr std::string_view cnf = "BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.00\r\n";
    MemoryIso image(sectors * 2048);

    // Fill non-filesystem sectors with a deterministic pattern too, so payload
    // verification covers real non-zero bytes rather than only metadata.
    for (std::size_t i = 0; i < image.bytes().size(); ++i) {
        image.bytes()[i] = static_cast<std::byte>((i * 17U + 3U) & 0xFFU);
    }

    auto pvd = image.bytes().subspan(16 * 2048, 2048);
    std::fill(pvd.begin(), pvd.end(), std::byte{0});
    pvd[0] = std::byte{1};
    std::memcpy(pvd.data() + 1, "CD001", 5);
    pvd[6] = std::byte{1};
    const std::string root_id(1, '\0');
    write_record(pvd.subspan(156), root_lba, 2048, 0x02, root_id);

    auto root = image.bytes().subspan(root_lba * 2048, 2048);
    std::fill(root.begin(), root.end(), std::byte{0});
    const std::string dot(1, '\0');
    write_record(root, root_lba, 2048, 0x02, dot);
    const auto used = std::to_integer<unsigned char>(root[0]);
    write_record(root.subspan(used), cnf_lba, static_cast<std::uint32_t>(cnf.size()), 0,
                 "SYSTEM.CNF;1");
    std::memcpy(image.bytes().data() + cnf_lba * 2048, cnf.data(), cnf.size());
    return image;
}

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

SparseWritableDisk make_disk()
{
    const auto chunk = ps2hdd::apa::kAllocationChunkSectors;
    SparseWritableDisk disk(static_cast<std::uint64_t>(64) * chunk * ps2hdd::apa::kSectorSize);
    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, chunk, chunk, chunk);
    constexpr char sony[] = "Sony Computer Entertainment Inc.";
    std::memcpy(mbr.mbr.magic, sony, sizeof(sony) - 1);
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    const auto opl = make_header("+OPL", ps2hdd::apa::kTypePfs, chunk, chunk, 0, 0);
    disk.put_object(0, mbr);
    disk.put_object(static_cast<std::uint64_t>(chunk) * ps2hdd::apa::kSectorSize, opl);
    return disk;
}

ps2hdd::hdl::ImageInstallOptions options()
{
    ps2hdd::hdl::ImageInstallOptions value;
    value.title = "Frieren Full Install";
    value.compat_flags = 0x04;
    value.dma = 0x0040;
    value.media = ps2hdd::hdl::MediaType::dvd;
    value.copy_buffer_bytes = 16 * 2048;
    value.created.year = 2026;
    value.created.month = 8;
    value.created.day = 23;
    return value;
}

std::filesystem::path recovery_path(std::string_view name)
{
    const auto root = std::filesystem::temp_directory_path() /
                      "ps2-driveforge-hdl-image-install-tests";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    check(!ec, "create HDL install recovery test directory");
    const auto path = root / std::string(name);
    std::filesystem::remove(path, ec);
    return path;
}

void test_full_install_publishes_verified_hdl_partition()
{
    auto disk = make_disk();
    auto iso = make_game_iso();
    std::vector<std::string> phases;
    const auto result = ps2hdd::hdl::install_to_image(
        disk, iso, options(), [&](const auto& progress) {
            if (phases.empty() || phases.back() != progress.phase) {
                phases.emplace_back(progress.phase);
            }
        });

    check(result.ok, "image-only HDL install should succeed");
    check(result.partition_id == "PP.SLUS-12345..FRIEREN_FULL_INS",
          "installed partition ID mismatch");
    check(result.startup == "SLUS_123.45", "installed startup mismatch");
    check(result.payload_bytes == iso.size_bytes(), "installed payload byte count mismatch");
    check(result.orphan_payload_bytes_on_failure == 0, "successful install must not report orphan bytes");
    check(!phases.empty() && phases.front() == "preflight" && phases.back() == "complete",
          "install progress phases are incomplete");

    ps2hdd::apa::Reader reader(disk);
    const auto scan = reader.scan();
    check(scan.ok(), "installed APA chain should scan cleanly");
    const ps2hdd::apa::Partition* installed = nullptr;
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.id == result.partition_id) {
            installed = &partition;
            break;
        }
    }
    check(installed != nullptr, "installed HDL partition not found after publication");
    const auto game = ps2hdd::hdl::read_game_info(disk, *installed);
    check(game.ok, "installed DEADFEED metadata should parse normally");
    check(game.game.title == "Frieren Full Install", "installed title mismatch");
    check(game.game.raw_size_bytes == iso.size_bytes(), "installed raw size mismatch");
}

void test_recovery_capsule_commits_after_verified_publication()
{
    auto disk = make_disk();
    auto iso = make_game_iso();
    auto install_options = options();
    const auto capsule = recovery_path("successful.rcap");
    install_options.recovery_capsule_path = capsule;

    const auto result = ps2hdd::hdl::install_to_image(disk, iso, install_options);
    check(result.ok, "recoverable HDL install should succeed");
    check(result.recovery_capsule_created && !result.recovery_pending,
          "successful recoverable install should create and finalize its capsule");
    check(result.warning.empty(), "successful recovery lifecycle should not warn");

    const auto inspection = ps2hdd::inspect_recovery_capsule(capsule, disk);
    check(inspection.ok &&
              inspection.capsule_state == ps2hdd::RecoveryCapsuleState::committed &&
              inspection.device_state == ps2hdd::RecoveryDeviceState::all_after,
          "verified HDL publication should leave a COMMITTED all-after capsule");

    std::error_code ec;
    std::filesystem::remove(capsule, ec);
}

void test_unresolved_prepared_capsule_refuses_before_payload_copy()
{
    auto disk = make_disk();
    auto iso = make_game_iso();
    const auto capsule = recovery_path("unresolved.rcap");

    std::array<std::byte, ps2hdd::apa::kHeaderSize> replacement{};
    ps2hdd::WriteTransaction pending(disk);
    check(pending.stage(0, replacement, "synthetic unresolved APA mutation"),
          "stage unresolved recovery fixture");
    check(ps2hdd::create_recovery_capsule(capsule, disk, pending.staged_writes()).ok,
          "create unresolved PREPARED recovery capsule");

    auto install_options = options();
    install_options.recovery_capsule_path = capsule;
    const auto writes_before = disk.write_calls;
    const auto result = ps2hdd::hdl::install_to_image(disk, iso, install_options);
    check(!result.ok && result.error.find("unresolved PREPARED") != std::string::npos,
          "new install should refuse an unresolved PREPARED recovery capsule");
    check(disk.write_calls == writes_before,
          "unresolved recovery refusal must happen before any ISO payload write");

    std::error_code ec;
    std::filesystem::remove(capsule, ec);
}

void test_failed_publication_leaves_old_chain_and_reports_orphan_payload()
{
    auto disk = make_disk();
    auto iso = make_game_iso();

    // One payload write is produced with this tiny ISO/buffer combination in
    // multiple chunks, so choose corruption dynamically after payload writes by
    // using a large buffer that makes payload one forward write. Publication is
    // then metadata=2, new main=3, MBR=4, old tail=5.
    auto install_options = options();
    install_options.copy_buffer_bytes = iso.size_bytes();
    disk.corrupt_write_call = 5;

    const auto result = ps2hdd::hdl::install_to_image(disk, iso, install_options);
    check(!result.ok, "corrupted APA publication must fail install");
    check(result.orphan_payload_bytes_on_failure == iso.size_bytes(),
          "failed publication should report the already copied orphan payload");

    ps2hdd::apa::Reader reader(disk);
    const auto scan = reader.scan();
    check(scan.ok(), "old APA chain should be restored after publication rollback");
    check(scan.partitions.size() == 2, "failed publication must not expose a new HDL partition");
}

void test_duplicate_partition_id_refuses_before_payload_write()
{
    auto disk = make_disk();
    auto iso = make_game_iso();
    const auto first = ps2hdd::hdl::install_to_image(disk, iso, options());
    check(first.ok, "fixture first install failed");
    const auto writes_after_first = disk.write_calls;

    const auto second = ps2hdd::hdl::install_to_image(disk, iso, options());
    check(!second.ok, "duplicate HDL partition ID should be refused");
    check(disk.write_calls == writes_after_first, "duplicate refusal must occur before payload writes");
}

} // namespace

int main()
{
    try {
        test_full_install_publishes_verified_hdl_partition();
        test_recovery_capsule_commits_after_verified_publication();
        test_unresolved_prepared_capsule_refuses_before_payload_copy();
        test_failed_publication_leaves_old_chain_and_reports_orphan_payload();
        test_duplicate_partition_id_refuses_before_payload_write();
        std::cout << "HDL image install tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HDL image install tests failed: " << error.what() << '\n';
        return 1;
    }
}
