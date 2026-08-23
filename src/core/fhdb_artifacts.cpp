#include "ps2hdd/fhdb_artifacts.hpp"

#include "ps2hdd/sha256.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ps2hdd::fhdb {
namespace {

constexpr std::size_t kMetaHeaderBytes = 64;
constexpr std::size_t kMetaEntryBytes = 4 + 32 + kRescueApaHeaderBytes;
constexpr std::size_t kMetaTrailerBytes = 32;
constexpr std::array<std::byte, 8> kMetaMagic{
    std::byte{'A'}, std::byte{'P'}, std::byte{'A'}, std::byte{'M'},
    std::byte{'E'}, std::byte{'T'}, std::byte{'A'}, std::byte{'1'}};
constexpr std::uint32_t kMetaVersion = 1;

void store_u32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

std::uint32_t load_u32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

bool read_file(const std::filesystem::path& path, std::vector<std::byte>& bytes)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > std::numeric_limits<std::size_t>::max()) return false;
    bytes.resize(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(input) || bytes.empty();
}

bool verify_file(const std::filesystem::path& path, std::span<const std::byte> expected)
{
    std::vector<std::byte> actual;
    return read_file(path, actual) &&
           std::equal(actual.begin(), actual.end(), expected.begin(), expected.end());
}

bool ensure_directory(const std::filesystem::path& directory, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "Could not create FHDB artifact directory: " + ec.message();
        return false;
    }
    return true;
}

bool write_exclusive(const std::filesystem::path& path,
                     std::span<const std::byte> bytes,
                     std::string& error)
{
#if defined(_WIN32)
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "Could not create non-overwriting FHDB artifact slot";
        return false;
    }
    bool ok = true;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - cursor, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + static_cast<std::ptrdiff_t>(cursor), amount,
                       &written, nullptr) || written != amount) {
            ok = false;
            break;
        }
        cursor += written;
    }
    if (ok) ok = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    if (!ok) {
        DeleteFileW(path.c_str());
        error = "Could not durably write FHDB artifact slot";
        return false;
    }
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        error = "Could not create non-overwriting FHDB artifact slot: " +
                std::string(std::strerror(errno));
        return false;
    }
    bool ok = true;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto written = ::write(fd,
            reinterpret_cast<const char*>(bytes.data()) + static_cast<std::ptrdiff_t>(cursor),
            bytes.size() - cursor);
        if (written <= 0) {
            ok = false;
            break;
        }
        cursor += static_cast<std::size_t>(written);
    }
    if (ok) ok = ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        ::unlink(path.c_str());
        error = "Could not durably write FHDB artifact slot: " +
                std::string(std::strerror(errno));
        return false;
    }
#endif
    if (!verify_file(path, bytes)) {
        error = "FHDB artifact read-back comparison failed";
        return false;
    }
    return true;
}

bool write_replace(const std::filesystem::path& path,
                   std::span<const std::byte> bytes,
                   std::string& error)
{
    auto temp = path;
    temp += ".tmp";
    std::error_code ec;
    std::filesystem::remove(temp, ec);
#if defined(_WIN32)
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "Could not create temporary FORENSIC.TXT";
        return false;
    }
    bool ok = true;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - cursor, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + static_cast<std::ptrdiff_t>(cursor), amount,
                       &written, nullptr) || written != amount) {
            ok = false;
            break;
        }
        cursor += written;
    }
    if (ok) ok = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        error = "Could not atomically publish FORENSIC.TXT";
        return false;
    }
#else
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        error = "Could not create temporary FORENSIC.TXT: " + std::string(std::strerror(errno));
        return false;
    }
    bool ok = true;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto written = ::write(fd,
            reinterpret_cast<const char*>(bytes.data()) + static_cast<std::ptrdiff_t>(cursor),
            bytes.size() - cursor);
        if (written <= 0) {
            ok = false;
            break;
        }
        cursor += static_cast<std::size_t>(written);
    }
    if (ok) ok = ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    if (!ok || ::rename(temp.c_str(), path.c_str()) != 0) {
        ::unlink(temp.c_str());
        error = "Could not atomically publish FORENSIC.TXT: " + std::string(std::strerror(errno));
        return false;
    }
#endif
    return verify_file(path, bytes);
}

using ExistingMatcher = std::function<bool(std::span<const std::byte>)>;

ArtifactSaveResult save_pair(const std::filesystem::path& directory,
                             const std::array<const char*, 2>& names,
                             std::span<const std::byte> bytes,
                             ExistingMatcher matcher = {})
{
    ArtifactSaveResult result;
    if (!ensure_directory(directory, result.error)) return result;
    for (const auto* name : names) {
        const auto path = directory / name;
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec) {
            result.error = "Could not inspect FHDB artifact slot: " + ec.message();
            return result;
        }
        if (exists) {
            std::vector<std::byte> current;
            if (read_file(path, current) &&
                ((matcher && matcher(current)) ||
                 (!matcher && current.size() == bytes.size() &&
                  std::equal(current.begin(), current.end(), bytes.begin(), bytes.end())))) {
                result.ok = true;
                result.reused = true;
                result.path = path;
                return result;
            }
            continue;
        }
        if (!write_exclusive(path, bytes, result.error)) return result;
        result.ok = true;
        result.path = path;
        return result;
    }
    result.error = "Both canonical FHDB artifact slots already contain different snapshots";
    return result;
}

bool rescue_state_matches(std::span<const std::byte> existing,
                          const RescueImageResult& wanted)
{
    const auto parsed = validate_rescue_image(existing);
    if (!parsed.ok) return false;
    const auto& a = parsed.info;
    const auto& b = wanted.info;
    return a.flags == b.flags && a.payload_start == b.payload_start &&
           a.payload_sectors == b.payload_sectors && a.payload_bytes == b.payload_bytes &&
           a.kelf_file_bytes == b.kelf_file_bytes &&
           a.apa_sha256 == b.apa_sha256 && a.payload_sha256 == b.payload_sha256;
}

} // namespace

std::vector<std::byte> build_hddmeta_image(const forensic::ScanResult& scan,
                                           const forensic::RepairPlan& plan,
                                           std::string& error)
{
    error.clear();
    if (!scan.ok || plan.patches.empty() || plan.map_index >= scan.maps.size()) {
        error = "HDDMETA requires one valid non-empty forensic repair plan";
        return {};
    }
    if (plan.patches.size() >
        (std::numeric_limits<std::uint32_t>::max() - kMetaHeaderBytes - kMetaTrailerBytes) /
            kMetaEntryBytes) {
        error = "HDDMETA patch set exceeds APAMETA1 size range";
        return {};
    }
    const auto size = kMetaHeaderBytes + plan.patches.size() * kMetaEntryBytes + kMetaTrailerBytes;
    std::vector<std::byte> image(size, std::byte{0});
    std::copy(kMetaMagic.begin(), kMetaMagic.end(), image.begin());
    store_u32(image.data() + 8, kMetaVersion);
    store_u32(image.data() + 12, scan.total_sectors);
    store_u32(image.data() + 16, static_cast<std::uint32_t>(plan.map_index));
    store_u32(image.data() + 20, plan.confidence);
    store_u32(image.data() + 24, static_cast<std::uint32_t>(plan.patches.size()));
    store_u32(image.data() + 28, plan.corroborated_count);
    store_u32(image.data() + 32, plan.speculative_count);
    unsigned one_or_two = 0;
    for (const auto& patch : plan.patches) {
        const auto distance = patch.bit_distance();
        if (distance == 1 || distance == 2) ++one_or_two;
    }
    store_u32(image.data() + 36, one_or_two);

    std::size_t offset = kMetaHeaderBytes;
    for (const auto& patch : plan.patches) {
        if (patch.node_index >= scan.nodes.size() || scan.nodes[patch.node_index].lba != patch.lba) {
            error = "HDDMETA patch does not resolve to its original scanned header";
            return {};
        }
        const auto& node = scan.nodes[patch.node_index];
        store_u32(image.data() + static_cast<std::ptrdiff_t>(offset), patch.lba);
        const auto digest = crypto::sha256(node.header);
        std::copy(digest.begin(), digest.end(),
                  image.begin() + static_cast<std::ptrdiff_t>(offset + 4));
        std::copy(node.header.begin(), node.header.end(),
                  image.begin() + static_cast<std::ptrdiff_t>(offset + 36));
        offset += kMetaEntryBytes;
    }
    const auto trailer = crypto::sha256(std::span<const std::byte>(image).first(size - kMetaTrailerBytes));
    std::copy(trailer.begin(), trailer.end(),
              image.end() - static_cast<std::ptrdiff_t>(kMetaTrailerBytes));
    return image;
}

HddMetaValidation validate_hddmeta_image(std::span<const std::byte> image)
{
    HddMetaValidation result;
    if (image.size() < kMetaHeaderBytes + kMetaEntryBytes + kMetaTrailerBytes ||
        !std::equal(kMetaMagic.begin(), kMetaMagic.end(), image.begin()) ||
        load_u32(image.data() + 8) != kMetaVersion) {
        result.error = "HDDMETA/APAMETA1 header is invalid";
        return result;
    }
    result.total_sectors = load_u32(image.data() + 12);
    result.map_index = load_u32(image.data() + 16);
    result.confidence = load_u32(image.data() + 20);
    result.patch_count = load_u32(image.data() + 24);
    result.corroborated_count = load_u32(image.data() + 28);
    result.speculative_count = load_u32(image.data() + 32);
    result.one_or_two_bit_count = load_u32(image.data() + 36);
    if (result.patch_count == 0 ||
        image.size() != kMetaHeaderBytes +
                            static_cast<std::size_t>(result.patch_count) * kMetaEntryBytes +
                            kMetaTrailerBytes) {
        result.error = "HDDMETA/APAMETA1 size relationship is invalid";
        return result;
    }
    std::size_t offset = kMetaHeaderBytes;
    for (std::uint32_t i = 0; i < result.patch_count; ++i) {
        const auto header = image.subspan(offset + 36, kRescueApaHeaderBytes);
        crypto::Sha256Digest stored{};
        std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(offset + 4), 32, stored.begin());
        if (crypto::sha256(header) != stored) {
            result.error = "HDDMETA entry SHA-256 does not match original APA header";
            return result;
        }
        offset += kMetaEntryBytes;
    }
    crypto::Sha256Digest stored_trailer{};
    std::copy_n(image.end() - static_cast<std::ptrdiff_t>(kMetaTrailerBytes),
                kMetaTrailerBytes, stored_trailer.begin());
    if (crypto::sha256(image.first(image.size() - kMetaTrailerBytes)) != stored_trailer) {
        result.error = "HDDMETA complete-image SHA-256 does not match";
        return result;
    }
    result.ok = true;
    return result;
}

ArtifactSaveResult save_hddraw(const std::filesystem::path& directory,
                               std::span<const std::byte, kRescueApaHeaderBytes> raw_header)
{
    return save_pair(directory, {"HDDRAW.BIN", "HDDRAW2.BIN"}, raw_header);
}

ArtifactSaveResult save_hddmbr(const std::filesystem::path& directory,
                               std::span<const std::byte, kRescueApaHeaderBytes> raw_header)
{
    return save_pair(directory, {"HDDMBR.BIN", "HDDMBR2.BIN"}, raw_header);
}

ArtifactSaveResult save_hddrescue(const std::filesystem::path& directory,
                                  const RescueImageResult& rescue)
{
    ArtifactSaveResult result;
    const auto bytes = serialize_rescue_image(rescue);
    if (bytes.empty()) {
        result.error = "Cannot save an invalid FHDB rescue image";
        return result;
    }
    return save_pair(directory, {"HDDRESCUE.BIN", "HDDRESCUE2.BIN"}, bytes,
                     [&](std::span<const std::byte> existing) {
                         return rescue_state_matches(existing, rescue);
                     });
}

ArtifactSaveResult save_hddmeta(const std::filesystem::path& directory,
                                const forensic::ScanResult& scan,
                                const forensic::RepairPlan& plan)
{
    ArtifactSaveResult result;
    std::string error;
    const auto image = build_hddmeta_image(scan, plan, error);
    if (image.empty()) {
        result.error = std::move(error);
        return result;
    }
    return save_pair(directory, {"HDDMETA.BIN", "HDDMETA2.BIN"}, image);
}

ArtifactSaveResult save_forensic_report(const std::filesystem::path& directory,
                                        const forensic::ScanResult& scan)
{
    ArtifactSaveResult result;
    if (!scan.ok) {
        result.error = "Cannot save FORENSIC.TXT from an invalid scan";
        return result;
    }
    if (!ensure_directory(directory, result.error)) return result;
    const auto text = forensic::render_report(scan);
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    result.path = directory / "FORENSIC.TXT";
    if (!write_replace(result.path, bytes, result.error)) return result;
    result.ok = true;
    return result;
}

} // namespace ps2hdd::fhdb
