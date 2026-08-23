#include "ps2hdd/disk_layout_guard.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TinyDisk final : public ps2hdd::BlockDevice {
public:
    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "layout-guard-fixture"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(out.data(), bytes_.data() + static_cast<std::size_t>(offset), out.size());
        return true;
    }

    void mbr_signature()
    {
        bytes_[510] = std::byte{0x55};
        bytes_[511] = std::byte{0xAA};
    }

    void partition_type(std::size_t index, unsigned char type)
    {
        bytes_[446 + index * 16 + 4] = static_cast<std::byte>(type);
    }

    void gpt_signature()
    {
        constexpr char signature[] = "EFI PART";
        std::memcpy(bytes_.data() + 512, signature, sizeof(signature) - 1);
    }

private:
    std::array<std::byte, 1024> bytes_{};
};

void blank_disk_is_not_misidentified()
{
    TinyDisk disk;
    const auto result = ps2hdd::inspect_disk_layout(disk);
    check(result.ok, "blank admission sectors are readable");
    check(result.kind == ps2hdd::LegacyPartitionMapKind::none,
          "blank disk has no legacy/GPT map");
    check(result.allows_ps2_mutation(), "blank PC partition map itself does not block PS2 mutation");
}

void protective_gpt_is_hard_refusal()
{
    TinyDisk disk;
    disk.mbr_signature();
    disk.partition_type(0, 0xEE);
    disk.gpt_signature();

    const auto result = ps2hdd::inspect_disk_layout(disk);
    check(result.ok && result.protective_entry && result.gpt_header,
          "protective GPT evidence is detected");
    check(result.kind == ps2hdd::LegacyPartitionMapKind::protective_gpt,
          "protective GPT classified");
    check(!result.allows_ps2_mutation(), "protective GPT blocks PS2 mutation admission");
}

void hybrid_gpt_is_hard_refusal()
{
    TinyDisk disk;
    disk.mbr_signature();
    disk.partition_type(0, 0xEE);
    disk.partition_type(1, 0x07);
    disk.gpt_signature();

    const auto result = ps2hdd::inspect_disk_layout(disk);
    check(result.kind == ps2hdd::LegacyPartitionMapKind::hybrid_gpt,
          "hybrid GPT/MBR classified");
    check(result.other_mbr_entries, "hybrid map records non-protective MBR entry");
    check(!result.allows_ps2_mutation(), "hybrid GPT blocks PS2 mutation admission");
}

void orphan_gpt_header_is_still_refused()
{
    TinyDisk disk;
    disk.gpt_signature();

    const auto result = ps2hdd::inspect_disk_layout(disk);
    check(result.kind == ps2hdd::LegacyPartitionMapKind::gpt_header_without_protective_mbr,
          "orphan GPT header is classified suspiciously");
    check(!result.allows_ps2_mutation(), "GPT header without protective MBR still blocks mutation");
}

void ordinary_legacy_mbr_is_reported_but_not_the_gpt_gate()
{
    TinyDisk disk;
    disk.mbr_signature();
    disk.partition_type(0, 0x83);

    const auto result = ps2hdd::inspect_disk_layout(disk);
    check(result.kind == ps2hdd::LegacyPartitionMapKind::legacy_mbr,
          "legacy MBR classified separately");
    check(result.allows_ps2_mutation(),
          "legacy MBR alone is not this guard's GPT refusal; APA validation remains mandatory");
}

} // namespace

int main()
{
    try {
        blank_disk_is_not_misidentified();
        protective_gpt_is_hard_refusal();
        hybrid_gpt_is_hard_refusal();
        orphan_gpt_header_is_still_refused();
        ordinary_legacy_mbr_is_reported_but_not_the_gpt_gate();
        std::cout << "Disk layout guard tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Disk layout guard test failure: " << error.what() << '\n';
        return 1;
    }
}
