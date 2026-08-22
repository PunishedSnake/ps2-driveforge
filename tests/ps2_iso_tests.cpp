#include "ps2hdd/ps2_iso.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
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

class MemoryDevice final : public ps2hdd::BlockDevice {
public:
    explicit MemoryDevice(std::size_t bytes) : bytes_(bytes) {}

    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "synthetic-iso"; }

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

void write_record(std::span<std::byte> target, std::uint32_t extent_lba,
                  std::uint32_t data_bytes, std::uint8_t flags,
                  std::string_view identifier)
{
    const std::size_t padding = identifier.size() % 2 == 0 ? 1 : 0;
    const std::size_t length = 33 + identifier.size() + padding;
    check(length <= target.size(), "directory fixture record does not fit");
    target[0] = static_cast<std::byte>(length);
    target[1] = std::byte{0};
    store_u32_both(target.data() + 2, extent_lba);
    store_u32_both(target.data() + 10, data_bytes);
    target[25] = static_cast<std::byte>(flags);
    target[28] = std::byte{1};
    target[29] = std::byte{0};
    target[30] = std::byte{0};
    target[31] = std::byte{1};
    target[32] = static_cast<std::byte>(identifier.size());
    std::memcpy(target.data() + 33, identifier.data(), identifier.size());
}

MemoryDevice make_iso(std::string_view system_cnf, std::string_view system_name = "SYSTEM.CNF;1")
{
    constexpr std::uint32_t root_lba = 20;
    constexpr std::uint32_t system_lba = 21;
    constexpr std::size_t sectors = 24;
    MemoryDevice device(sectors * ps2hdd::iso::kSectorSize);

    auto pvd = device.bytes().subspan(
        ps2hdd::iso::kPrimaryVolumeDescriptorLba * ps2hdd::iso::kSectorSize,
        ps2hdd::iso::kSectorSize);
    pvd[0] = std::byte{1};
    std::memcpy(pvd.data() + 1, "CD001", 5);
    pvd[6] = std::byte{1};

    // Root directory record embedded in PVD.
    std::string root_identifier(1, '\0');
    write_record(pvd.subspan(156), root_lba, ps2hdd::iso::kSectorSize, 0x02, root_identifier);

    auto root = device.bytes().subspan(root_lba * ps2hdd::iso::kSectorSize,
                                       ps2hdd::iso::kSectorSize);
    std::string dot(1, '\0');
    std::string dotdot(1, '\1');
    write_record(root, root_lba, ps2hdd::iso::kSectorSize, 0x02, dot);
    const auto first_len = std::to_integer<unsigned char>(root[0]);
    write_record(root.subspan(first_len), root_lba, ps2hdd::iso::kSectorSize, 0x02, dotdot);
    const auto second_len = std::to_integer<unsigned char>(root[first_len]);
    write_record(root.subspan(first_len + second_len), system_lba,
                 static_cast<std::uint32_t>(system_cnf.size()), 0, system_name);

    auto cnf = device.bytes().subspan(system_lba * ps2hdd::iso::kSectorSize, system_cnf.size());
    std::memcpy(cnf.data(), system_cnf.data(), system_cnf.size());
    return device;
}

void test_boot2_startup_is_extracted()
{
    auto device = make_iso("BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n");
    const auto result = ps2hdd::iso::inspect_ps2_iso(device);
    check(result.ok, "valid synthetic PS2 ISO should inspect successfully");
    check(result.game.startup == "SLUS_123.45", "BOOT2 startup ID mismatch");
    check(result.game.system_cnf.find("VMODE") != std::string::npos,
          "SYSTEM.CNF contents should be retained");
}

void test_case_and_boot_key_are_tolerated()
{
    auto device = make_iso("boot = \"cdrom0:/sles_543.21;1\"\n", "system.cnf;1");
    const auto result = ps2hdd::iso::inspect_ps2_iso(device);
    check(result.ok, "case-insensitive ISO/SYSTEM.CNF parsing should succeed");
    check(result.game.startup == "SLES_543.21", "BOOT startup should normalize uppercase");
}

void test_missing_system_cnf_is_refused()
{
    auto device = make_iso("BOOT2 = cdrom0:\\SLUS_123.45;1\n", "NOTSYSTEM.TXT;1");
    const auto result = ps2hdd::iso::inspect_ps2_iso(device);
    check(!result.ok, "ISO without SYSTEM.CNF must be refused");
}

void test_malformed_startup_is_refused()
{
    auto device = make_iso("BOOT2 = cdrom0:\\GAME.ELF;1\n");
    const auto result = ps2hdd::iso::inspect_ps2_iso(device);
    check(!result.ok, "unsupported startup filename must be refused");
}

void test_missing_pvd_is_refused()
{
    MemoryDevice device(24 * ps2hdd::iso::kSectorSize);
    const auto result = ps2hdd::iso::inspect_ps2_iso(device);
    check(!result.ok, "image without ISO9660 PVD must be refused");
}

} // namespace

int main()
{
    try {
        test_boot2_startup_is_extracted();
        test_case_and_boot_key_are_tolerated();
        test_missing_system_cnf_is_refused();
        test_malformed_startup_is_refused();
        test_missing_pvd_is_refused();
        std::cout << "PS2 ISO tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PS2 ISO tests failed: " << error.what() << '\n';
        return 1;
    }
}
