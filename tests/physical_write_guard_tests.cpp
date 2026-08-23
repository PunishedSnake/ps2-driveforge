#include "ps2hdd/physical_write_guard.hpp"

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

class MemoryDisk final : public ps2hdd::BlockDevice {
public:
    explicit MemoryDisk(std::size_t bytes) : bytes_(bytes) {}

    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "physical-write-guard-fixture"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(out.data(), bytes_.data() + static_cast<std::size_t>(offset), out.size());
        return true;
    }

    void install_minimal_apa()
    {
        ps2hdd::apa::Header header{};
        header.magic = ps2hdd::apa::kMagic;
        header.start = 0;
        header.length = static_cast<std::uint32_t>(bytes_.size() / ps2hdd::apa::kSectorSize);
        header.type = ps2hdd::apa::kTypeMbr;
        std::memcpy(header.id, "__mbr", 5);
        constexpr char sony[] = "Sony Computer Entertainment Inc.";
        std::memcpy(header.mbr.magic, sony, sizeof(sony) - 1);
        header.mbr.version = 2;
        header.checksum = ps2hdd::apa::checksum(header);
        std::memcpy(bytes_.data(), &header, sizeof(header));
    }

    void flip(std::size_t offset)
    {
        bytes_.at(offset) ^= std::byte{0x01};
    }

    void install_gpt_signature()
    {
        constexpr char signature[] = "EFI PART";
        std::memcpy(bytes_.data() + 512, signature, sizeof(signature) - 1);
    }

private:
    std::vector<std::byte> bytes_;
};

void identity_detects_media_change()
{
    MemoryDisk disk(16U * 1024U * 1024U);
    disk.install_minimal_apa();

    const auto captured = ps2hdd::physical_write::capture_identity(disk);
    check(captured.ok, "identity snapshot should be captured");

    std::string error;
    check(ps2hdd::physical_write::verify_identity(disk, captured.snapshot, error),
          "unchanged media should retain its identity");

    disk.flip(8U * 1024U * 1024U);
    check(!ps2hdd::physical_write::verify_identity(disk, captured.snapshot, error),
          "changed sampled media must fail identity verification");
}

void clean_apa_can_be_authorized_and_rechecked()
{
    MemoryDisk disk(16U * 1024U * 1024U);
    disk.install_minimal_apa();

    const auto admitted = ps2hdd::physical_write::authorize(disk);
    check(admitted.ok, "clean APA image should pass portable physical-write admission");
    check(admitted.authorization.apa_version == 2, "APA version should be frozen in authorization");
    check(admitted.authorization.apa_header_count == 1, "minimal fixture should expose one APA header");

    std::string error;
    check(ps2hdd::physical_write::verify_authorization(disk, admitted.authorization, error),
          "unchanged admitted disk should pass immediate recheck");

    disk.flip(8U * 1024U * 1024U);
    check(!ps2hdd::physical_write::verify_authorization(disk, admitted.authorization, error),
          "media changed after preflight must lose physical-write authorization");
}

void gpt_evidence_blocks_authorization_before_apa_optimism()
{
    MemoryDisk disk(16U * 1024U * 1024U);
    disk.install_minimal_apa();
    disk.install_gpt_signature();

    const auto admitted = ps2hdd::physical_write::authorize(disk);
    check(!admitted.ok, "GPT evidence must block physical-write admission");
}

} // namespace

int main()
{
    try {
        identity_detects_media_change();
        clean_apa_can_be_authorized_and_rechecked();
        gpt_evidence_blocks_authorization_before_apa_optimism();
        std::cout << "Physical write guard tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Physical write guard test failure: " << error.what() << '\n';
        return 1;
    }
}
