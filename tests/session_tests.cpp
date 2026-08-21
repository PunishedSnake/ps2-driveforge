#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/mount_view.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kPfsLba = 0x4000;
constexpr std::uint32_t kPfsLength = 0x8000;
constexpr std::uint32_t kZoneSize = 8192;
constexpr std::uint32_t kRootZone = 700;
constexpr std::uint32_t kRootDataZone = 701;
constexpr std::uint32_t kFileZone = 702;
constexpr std::uint32_t kFileDataZone = 703;
constexpr std::size_t kBytes = 32U * 1024U * 1024U;
constexpr std::string_view kFileName = "hello.txt";
constexpr std::string_view kFileData = "Darkness says hi";

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class MemoryDevice final : public ps2hdd::BlockDevice {
public:
    MemoryDevice() : bytes_(kBytes) {}

    [[nodiscard]] std::uint64_t size_bytes() const override { return bytes_.size(); }
    [[nodiscard]] std::string display_name() const override { return "DriveSession fixture"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(out.data(), bytes_.data() + static_cast<std::ptrdiff_t>(offset), out.size());
        return true;
    }

    template <typename T>
    void put(std::uint64_t offset, const T& value)
    {
        const auto raw = std::as_bytes(std::span{&value, 1});
        check(offset <= bytes_.size() && raw.size() <= bytes_.size() - static_cast<std::size_t>(offset),
              "session fixture write fits");
        std::memcpy(bytes_.data() + static_cast<std::ptrdiff_t>(offset), raw.data(), raw.size());
    }

    void put_bytes(std::uint64_t offset, std::span<const std::byte> raw)
    {
        check(offset <= bytes_.size() && raw.size() <= bytes_.size() - static_cast<std::size_t>(offset),
              "session fixture bytes fit");
        std::memcpy(bytes_.data() + static_cast<std::ptrdiff_t>(offset), raw.data(), raw.size());
    }

private:
    std::vector<std::byte> bytes_;
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
    h.checksum = ps2hdd::apa::checksum(h);
    return h;
}

void put_dentry(std::span<std::byte> dir, std::size_t offset, std::uint32_t inode,
                std::uint8_t subpart, std::string_view name, std::uint16_t mode,
                std::uint16_t allocated)
{
    std::memcpy(dir.data() + static_cast<std::ptrdiff_t>(offset), &inode, sizeof(inode));
    dir[offset + 4] = static_cast<std::byte>(subpart);
    dir[offset + 5] = static_cast<std::byte>(name.size());
    const auto raw_len = static_cast<std::uint16_t>(mode | allocated);
    std::memcpy(dir.data() + static_cast<std::ptrdiff_t>(offset + 6), &raw_len, sizeof(raw_len));
    std::memcpy(dir.data() + static_cast<std::ptrdiff_t>(offset + 8), name.data(), name.size());
}

std::unique_ptr<MemoryDevice> fixture()
{
    auto dev = std::make_unique<MemoryDevice>();
    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, kPfsLba, 0, kPfsLba);
    constexpr char sony[] = "Sony Computer Entertainment Inc.";
    std::memcpy(mbr.mbr.magic, sony, std::min(sizeof(sony), sizeof(mbr.mbr.magic)));
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    const auto pfs_header = make_header("+TEST", ps2hdd::apa::kTypePfs, kPfsLba, kPfsLength, 0, 0);
    dev->put(0, mbr);
    dev->put(static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize, pfs_header);

    ps2hdd::pfs::SuperBlock super{};
    super.magic = ps2hdd::pfs::kSuperMagic;
    super.version = 3;
    super.zone_size = kZoneSize;
    super.root = {kRootZone, 0, 1};
    super.log = {650, 0, 1};
    std::array<std::byte, ps2hdd::apa::kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &super, sizeof(super));
    const auto base = static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize;
    dev->put_bytes(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize,
                   super_sector);
    dev->put_bytes(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperBackupSector) * ps2hdd::apa::kSectorSize,
                   super_sector);

    ps2hdd::pfs::Inode root{};
    root.magic = ps2hdd::pfs::kSegdMagic;
    root.inode_block = {kRootZone, 0, 1};
    root.last_segment = root.inode_block;
    root.data[0] = root.inode_block;
    root.data[1] = {kRootDataZone, 0, 1};
    root.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 0x01FF);
    root.size = ps2hdd::apa::kSectorSize;
    root.number_data = 2;
    root.number_blocks = 2;
    root.checksum = ps2hdd::pfs::inode_checksum(root);
    dev->put(base + static_cast<std::uint64_t>(kRootZone) * kZoneSize, root);

    ps2hdd::pfs::Inode file{};
    file.magic = ps2hdd::pfs::kSegdMagic;
    file.inode_block = {kFileZone, 0, 1};
    file.last_segment = file.inode_block;
    file.data[0] = file.inode_block;
    file.data[1] = {kFileDataZone, 0, 1};
    file.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeRegular | 0x01B6);
    file.size = kFileData.size();
    file.number_data = 2;
    file.number_blocks = 2;
    file.checksum = ps2hdd::pfs::inode_checksum(file);
    dev->put(base + static_cast<std::uint64_t>(kFileZone) * kZoneSize, file);

    std::array<std::byte, ps2hdd::apa::kSectorSize> dir{};
    put_dentry(dir, 0, kRootZone, 0, ".", ps2hdd::pfs::kModeDirectory, 12);
    put_dentry(dir, 12, kRootZone, 0, "..", ps2hdd::pfs::kModeDirectory, 12);
    put_dentry(dir, 24, kFileZone, 0, kFileName, ps2hdd::pfs::kModeRegular, 20);
    const std::uint16_t free_len = static_cast<std::uint16_t>(dir.size() - 44U);
    std::memcpy(dir.data() + 50, &free_len, sizeof(free_len));
    dev->put_bytes(base + static_cast<std::uint64_t>(kRootDataZone) * kZoneSize, dir);

    std::array<std::byte, kZoneSize> file_data{};
    std::memcpy(file_data.data(), kFileData.data(), kFileData.size());
    dev->put_bytes(base + static_cast<std::uint64_t>(kFileDataZone) * kZoneSize, file_data);
    return dev;
}

void session_roundtrip()
{
    ps2hdd::DriveSession session(fixture());
    check(session.scan(), "DriveSession scan succeeds");
    check(session.find_partition("+TEST") != nullptr, "DriveSession finds PFS partition");

    const auto browse = session.browse("+TEST", "/");
    check(browse.ok && browse.entries.size() == 1, "DriveSession browses root file");
    check(browse.entries[0].name == kFileName && browse.entries[0].size == kFileData.size(),
          "DriveSession reports file metadata");

    const auto stat = session.stat("+TEST", kFileName);
    check(stat.ok && stat.entry.is_regular() && stat.entry.size == kFileData.size(),
          "DriveSession stat resolves regular file");

    std::array<std::byte, 64> bytes{};
    const auto read = session.read_file("+TEST", kFileName, 9, bytes);
    check(read.ok && read.bytes_read == kFileData.size() - 9, "DriveSession clamps read at EOF");
    check(std::string(reinterpret_cast<const char*>(bytes.data()), read.bytes_read) == kFileData.substr(9),
          "DriveSession random read content");

    const auto out = std::filesystem::temp_directory_path() / "ps2-driveforge-session-export";
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
    const auto exported = session.export_to_host("+TEST", "/", out);
    check(exported.ok, "DriveSession exports root");
    check(std::filesystem::is_regular_file(out / std::string(kFileName)),
          "DriveSession creates host file");
    std::filesystem::remove_all(out, ec);

    const auto stats = session.stats();
    check(stats.apa_scans == 1, "session scan counter");
    check(stats.browse_operations == 1, "session browse counter");
    check(stats.stat_operations == 1, "session stat counter");
    check(stats.read_operations == 1, "session read counter");
    check(stats.export_operations == 1, "session export counter");
    check(stats.backing_io.read_calls > 0, "backing I/O calls are instrumented");

    session.reset_stats();
    const auto reset = session.stats();
    check(reset.backing_io.read_calls == 0 && reset.apa_scans == 0 && reset.browse_operations == 0 &&
              reset.stat_operations == 0 && reset.read_operations == 0,
          "session statistics reset");
}

void mount_view_roundtrip()
{
    ps2hdd::DriveSession session(fixture());
    check(session.scan(), "mount fixture scan succeeds");
    ps2hdd::ReadOnlyMountView view(session);

    const auto root = view.list_directory("\\");
    check(root.ok && root.entries.size() == 1 && root.entries[0].name == "Partitions",
          "mount root exposes Partitions namespace");

    const auto partitions = view.list_directory("\\Partitions");
    check(partitions.ok && partitions.entries.size() == 1 && partitions.entries[0].name == "+TEST",
          "mount namespace exposes PFS partition");

    const auto files = view.list_directory("\\Partitions\\+TEST");
    check(files.ok && files.entries.size() == 1 && files.entries[0].name == kFileName,
          "mount namespace exposes PFS file");

    const auto lookup = view.lookup("\\partitions\\+test\\HELLO.TXT");
    check(lookup.ok && lookup.kind == ps2hdd::MountNodeKind::pfs_file && lookup.size == kFileData.size(),
          "mount lookup follows Windows-style ASCII case-insensitive aliases");

    std::array<std::byte, 5> bytes{};
    const auto read = view.read_file("\\Partitions\\+TEST\\hello.txt", 5, bytes);
    check(read.ok && read.bytes_read == bytes.size(), "mount random read succeeds");
    check(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == kFileData.substr(5, 5),
          "mount random read returns correct bytes");

    const auto missing = view.lookup("\\Partitions\\+TEST\\missing.bin");
    check(!missing.ok, "mount lookup rejects missing files");
}

} // namespace

int main()
{
    try {
        session_roundtrip();
        mount_view_roundtrip();
        std::cout << "DriveSession / Darkness mount-view tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
