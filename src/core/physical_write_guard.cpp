#include "ps2hdd/physical_write_guard.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2hdd::physical_write {
namespace {

constexpr std::uint64_t kSectorBytes = 512;
constexpr std::uint64_t kMinimumPs2DiskBytes = 8ULL * 1024ULL * 1024ULL;

void hash_u64(crypto::Sha256Context& context, std::uint64_t value) noexcept
{
    std::array<std::byte, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
    crypto::sha256_update(context, bytes);
}

std::uint64_t align_down_sector(std::uint64_t value) noexcept
{
    return value - (value % kSectorBytes);
}

std::array<std::uint64_t, kIdentityWindowCount>
make_offsets(std::uint64_t size, std::uint64_t window) noexcept
{
    const auto max_start = size - window;
    std::array<std::uint64_t, kIdentityWindowCount> offsets{
        0,
        size / 4U,
        size / 2U,
        (size / 4U) * 3U,
        max_start,
    };

    for (auto& offset : offsets) {
        offset = std::min(align_down_sector(offset), align_down_sector(max_start));
    }
    return offsets;
}

} // namespace

IdentityResult capture_identity(BlockDevice& device)
{
    IdentityResult result;
    const auto size = device.size_bytes();
    if (size < kSectorBytes) {
        result.error = "device is too small for a physical-write identity snapshot";
        return result;
    }

    const auto window = std::min<std::uint64_t>(kIdentityWindowBytes, size);
    const auto offsets = make_offsets(size, window);
    std::vector<std::byte> buffer(static_cast<std::size_t>(window));

    crypto::Sha256Context context;
    crypto::sha256_init(context);
    constexpr std::array<std::byte, 20> tag{
        std::byte{'P'}, std::byte{'S'}, std::byte{'2'}, std::byte{'D'}, std::byte{'F'},
        std::byte{'-'}, std::byte{'P'}, std::byte{'H'}, std::byte{'Y'}, std::byte{'S'},
        std::byte{'I'}, std::byte{'C'}, std::byte{'A'}, std::byte{'L'}, std::byte{'-'},
        std::byte{'I'}, std::byte{'D'}, std::byte{'-'}, std::byte{'V'}, std::byte{'1'},
    };
    crypto::sha256_update(context, tag);
    hash_u64(context, size);
    hash_u64(context, window);

    for (const auto offset : offsets) {
        if (!device.read(offset, buffer)) {
            result.error = "failed to read one of the physical-write identity windows";
            return result;
        }
        hash_u64(context, offset);
        crypto::sha256_update(context, buffer);
    }

    result.snapshot.size_bytes = size;
    result.snapshot.offsets = offsets;
    result.snapshot.digest = crypto::sha256_final(context);
    result.ok = true;
    return result;
}

bool verify_identity(BlockDevice& device,
                     const IdentitySnapshot& expected,
                     std::string& error)
{
    error.clear();
    if (device.size_bytes() != expected.size_bytes) {
        error = "physical device size changed since read-only preflight";
        return false;
    }

    const auto current = capture_identity(device);
    if (!current.ok) {
        error = current.error;
        return false;
    }
    if (current.snapshot.offsets != expected.offsets ||
        current.snapshot.digest != expected.digest) {
        error = "physical device fingerprint changed since read-only preflight";
        return false;
    }
    return true;
}

AuthorizationResult authorize(BlockDevice& device)
{
    AuthorizationResult result;
    if (device.size_bytes() < kMinimumPs2DiskBytes ||
        (device.size_bytes() % kSectorBytes) != 0) {
        result.error = "physical write requires a sector-aligned PS2-sized device";
        return result;
    }

    const auto layout = inspect_disk_layout(device);
    if (!layout.ok) {
        result.error = "PC partition-map admission failed: " + layout.error;
        return result;
    }
    if (!layout.allows_ps2_mutation()) {
        result.error = "physical write refused because GPT/protective ownership evidence is present";
        return result;
    }

    apa::Reader reader(device);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        result.error = "physical write refused because the APA scan is not clean";
        return result;
    }

    const auto identity = capture_identity(device);
    if (!identity.ok) {
        result.error = identity.error;
        return result;
    }

    result.authorization.identity = identity.snapshot;
    result.authorization.apa_version = scan.apa_version;
    result.authorization.apa_header_count = scan.partitions.size();
    result.authorization.pc_layout = layout.kind;
    result.ok = true;
    return result;
}

bool verify_authorization(BlockDevice& device,
                          const Authorization& expected,
                          std::string& error)
{
    error.clear();

    const auto layout = inspect_disk_layout(device);
    if (!layout.ok || !layout.allows_ps2_mutation()) {
        error = layout.ok
                    ? "PC partition-map ownership changed and no longer allows PS2 mutation"
                    : "PC partition-map recheck failed: " + layout.error;
        return false;
    }
    if (layout.kind != expected.pc_layout) {
        error = "PC partition-map classification changed since read-only preflight";
        return false;
    }

    apa::Reader reader(device);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        error = "APA recheck failed before opening physical write access";
        return false;
    }
    if (scan.apa_version != expected.apa_version ||
        scan.partitions.size() != expected.apa_header_count) {
        error = "APA topology changed since read-only physical-write preflight";
        return false;
    }

    return verify_identity(device, expected.identity, error);
}

} // namespace ps2hdd::physical_write
