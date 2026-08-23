#include "ps2hdd/recovery_capsule.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

class MemoryDisk final : public ps2hdd::WritableBlockDevice {
public:
    explicit MemoryDisk(std::size_t bytes) : bytes_(bytes)
    {
        for (std::size_t index = 0; index < bytes_.size(); ++index) {
            bytes_[index] = static_cast<std::byte>((index * 13U + 9U) & 0xFFU);
        }
    }

    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "recovery-test.img"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        if (offset > bytes_.size() || in.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::copy(in.begin(), in.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        ++write_calls;
        return true;
    }

    bool flush() override
    {
        ++flush_calls;
        return true;
    }

    std::vector<std::byte> slice(std::size_t offset, std::size_t bytes) const
    {
        return {bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes_.begin() + static_cast<std::ptrdiff_t>(offset + bytes)};
    }

    std::size_t write_calls{};
    std::size_t flush_calls{};

private:
    std::vector<std::byte> bytes_;
};

std::vector<std::byte> filled(std::size_t bytes, unsigned char value)
{
    return std::vector<std::byte>(bytes, static_cast<std::byte>(value));
}

std::filesystem::path test_root()
{
    const auto root = std::filesystem::temp_directory_path() /
                      "ps2-driveforge-recovery-capsule-tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    check(!ec, "create recovery test directory");
    return root;
}

struct StagedFixture {
    MemoryDisk disk{4096};
    ps2hdd::WriteTransaction transaction{disk};
    std::vector<std::byte> first = filled(512, 0xA5);
    std::vector<std::byte> second = filled(1024, 0x5A);

    StagedFixture()
    {
        check(transaction.stage(512, first, "APA neighbour one"), "stage first recovery range");
        check(transaction.stage(2048, second, "APA neighbour two"), "stage second recovery range");
    }
};

void interrupted_transaction_restores_exact_before_images(const std::filesystem::path& root)
{
    StagedFixture fixture;
    const auto capsule = root / "interrupted.rcap";
    const auto original_first = fixture.disk.slice(512, 512);
    const auto original_second = fixture.disk.slice(2048, 1024);

    const auto created = ps2hdd::create_recovery_capsule(
        capsule, fixture.disk, fixture.transaction.staged_writes());
    check(created.ok && created.ranges == 2 && created.captured_bytes == 1536,
          "create prepared recovery capsule");

    const auto before = ps2hdd::inspect_recovery_capsule(capsule, fixture.disk);
    check(before.ok && before.capsule_state == ps2hdd::RecoveryCapsuleState::prepared &&
              before.device_state == ps2hdd::RecoveryDeviceState::all_before,
          "fresh capsule should observe all before-images");

    const auto& first = fixture.transaction.staged_writes()[0];
    check(fixture.disk.write(first.offset, first.after), "simulate first forward mutation");
    check(fixture.disk.flush(), "flush simulated interrupted mutation");

    const auto mixed = ps2hdd::inspect_recovery_capsule(capsule, fixture.disk);
    check(mixed.ok && mixed.device_state == ps2hdd::RecoveryDeviceState::mixed &&
              mixed.before_ranges == 1 && mixed.after_ranges == 1,
          "interrupted transaction should classify as mixed");

    const auto restored = ps2hdd::restore_prepared_recovery_capsule(capsule, fixture.disk);
    check(restored.ok && restored.writes_attempted == 2,
          "prepared mixed capsule should restore every before-image");
    check(fixture.disk.slice(512, 512) == original_first &&
              fixture.disk.slice(2048, 1024) == original_second,
          "restored device bytes should match exact staged before-images");

    const auto after = ps2hdd::inspect_recovery_capsule(capsule, fixture.disk);
    check(after.ok && after.capsule_state == ps2hdd::RecoveryCapsuleState::restored &&
              after.device_state == ps2hdd::RecoveryDeviceState::all_before,
          "restored capsule should persist its terminal state");
}

void committed_capsule_is_not_automatically_rolled_back(const std::filesystem::path& root)
{
    StagedFixture fixture;
    const auto capsule = root / "committed.rcap";
    check(ps2hdd::create_recovery_capsule(
              capsule, fixture.disk, fixture.transaction.staged_writes()).ok,
          "create capsule for committed test");

    for (const auto& write : fixture.transaction.staged_writes()) {
        check(fixture.disk.write(write.offset, write.after), "simulate complete forward mutation");
    }
    check(fixture.disk.flush(), "flush complete forward mutation");

    const auto applied = ps2hdd::inspect_recovery_capsule(capsule, fixture.disk);
    check(applied.ok && applied.device_state == ps2hdd::RecoveryDeviceState::all_after,
          "fully applied transaction should classify as all-after");
    check(ps2hdd::mark_recovery_capsule_committed(capsule).ok,
          "mark recovery capsule committed");

    const auto writes_before = fixture.disk.write_calls;
    const auto refused = ps2hdd::restore_prepared_recovery_capsule(capsule, fixture.disk);
    check(!refused.ok && fixture.disk.write_calls == writes_before,
          "committed capsule must not be automatically rolled back");
    const auto state = ps2hdd::inspect_recovery_capsule(capsule, fixture.disk);
    check(state.ok && state.capsule_state == ps2hdd::RecoveryCapsuleState::committed &&
              state.device_state == ps2hdd::RecoveryDeviceState::all_after,
          "committed capsule and target bytes should remain intact");
}

void foreign_range_refuses_automatic_recovery(const std::filesystem::path& root)
{
    StagedFixture fixture;
    const auto capsule = root / "foreign.rcap";
    check(ps2hdd::create_recovery_capsule(
              capsule, fixture.disk, fixture.transaction.staged_writes()).ok,
          "create capsule for foreign target test");

    const auto third_state = filled(512, 0x33);
    check(fixture.disk.write(512, third_state), "inject third-state target bytes");
    check(fixture.disk.flush(), "flush third-state target bytes");

    const auto inspection = ps2hdd::inspect_recovery_capsule(capsule, fixture.disk);
    check(inspection.ok &&
              inspection.device_state == ps2hdd::RecoveryDeviceState::foreign_or_corrupt,
          "range matching neither state should classify target as foreign/corrupt");
    const auto writes_before = fixture.disk.write_calls;
    const auto restored = ps2hdd::restore_prepared_recovery_capsule(capsule, fixture.disk);
    check(!restored.ok && fixture.disk.write_calls == writes_before,
          "foreign/corrupt target must be refused before any recovery write");
}

void corrupted_capsule_checksum_is_rejected(const std::filesystem::path& root)
{
    StagedFixture fixture;
    const auto capsule = root / "corrupt.rcap";
    check(ps2hdd::create_recovery_capsule(
              capsule, fixture.disk, fixture.transaction.staged_writes()).ok,
          "create capsule for checksum test");

    std::fstream file(capsule, std::ios::binary | std::ios::in | std::ios::out);
    check(static_cast<bool>(file), "open capsule for corruption injection");
    file.seekg(48);
    char value = 0;
    file.read(&value, 1);
    check(static_cast<bool>(file), "read capsule corruption byte");
    value ^= 0x40;
    file.seekp(48);
    file.write(&value, 1);
    file.flush();
    check(static_cast<bool>(file), "write capsule corruption byte");
    file.close();

    const auto inspection = ps2hdd::inspect_recovery_capsule(capsule, fixture.disk);
    check(!inspection.ok && inspection.error.find("checksum") != std::string::npos,
          "corrupted capsule checksum should fail closed");
}

} // namespace

int main()
{
    try {
        const auto root = test_root();
        interrupted_transaction_restores_exact_before_images(root);
        committed_capsule_is_not_automatically_rolled_back(root);
        foreign_range_refuses_automatic_recovery(root);
        corrupted_capsule_checksum_is_rejected(root);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::cout << "Recovery capsule tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Recovery capsule test failure: " << error.what() << '\n';
        return 1;
    }
}
