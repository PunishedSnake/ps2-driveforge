#include "ps2hdd/bdm_library.hpp"

#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/opl_library_sync.hpp"
#include "ps2hdd/ps2_iso.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace ps2hdd::bdm {
namespace {

constexpr std::uint64_t kCdThreshold = 750ULL * 1024ULL * 1024ULL;

std::string upper(std::string_view value)
{
    std::string out(value);
    for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

std::string fat_safe(std::string_view value)
{
    std::string out;
    for (const unsigned char ch : value) {
        out.push_back(ch < 0x20 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(ch)) != std::string_view::npos
                          ? '_'
                          : static_cast<char>(ch));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? std::string("Game") : out;
}

// "SLUS_123.45.Title.iso" -> "SLUS_123.45"
std::optional<std::string> id_from_name(std::string_view name)
{
    if (name.size() < 12) return std::nullopt;
    return opl::canonical_game_id(name.substr(0, 11));
}

} // namespace

LayoutResult ensure_layout(const std::filesystem::path& root)
{
    LayoutResult result;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        result.error = "BDM library root is not a directory: " + root.string();
        return result;
    }
    for (const auto dir : kLayoutDirectories) {
        const auto path = root / std::string(dir);
        if (std::filesystem::is_directory(path, ec)) continue;
        if (!std::filesystem::create_directory(path, ec) || ec) {
            result.error = "Could not create " + path.string() + ": " + ec.message();
            return result;
        }
        result.created.emplace_back(dir);
    }
    result.ok = true;
    return result;
}

FragmentReport count_fragments(const std::filesystem::path& file)
{
    FragmentReport report;
#ifdef _WIN32
    HANDLE handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return report;
    STARTING_VCN_INPUT_BUFFER input{};
    std::vector<std::byte> buffer(64 * 1024);
    std::size_t extents = 0;
    while (true) {
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(handle, FSCTL_GET_RETRIEVAL_POINTERS, &input, sizeof(input), buffer.data(),
                                        static_cast<DWORD>(buffer.size()), &returned, nullptr);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error != ERROR_MORE_DATA) {
            // Empty/resident files have no extents; anything else is unknown.
            if (error == ERROR_HANDLE_EOF) report.known = true;
            CloseHandle(handle);
            return report;
        }
        const auto* pointers = reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(buffer.data());
        extents += pointers->ExtentCount;
        if (ok) break;
        input.StartingVcn = pointers->Extents[pointers->ExtentCount - 1].NextVcn;
    }
    CloseHandle(handle);
    report.known = true;
    report.extents = extents;
#else
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return report;
    // struct fiemap ends in a flexible array member; keep it in raw storage so
    // strict compilers do not see a zero-length-array object.
    alignas(alignof(std::uint64_t)) std::array<unsigned char, 64> storage{};
    static_assert(sizeof(storage) >= 32, "fiemap header");
    auto* query = reinterpret_cast<struct fiemap*>(storage.data());
    query->fm_start = 0;
    query->fm_length = FIEMAP_MAX_OFFSET;
    query->fm_flags = FIEMAP_FLAG_SYNC;
    query->fm_extent_count = 0; // count only
    if (::ioctl(fd, FS_IOC_FIEMAP, query) == 0) {
        report.known = true;
        report.extents = query->fm_mapped_extents;
    }
    ::close(fd);
#endif
    return report;
}

std::string game_file_name(std::string_view startup, std::string_view title, std::string_view extension)
{
    const std::string prefix = std::string(startup) + ".";
    std::string name = fat_safe(title);
    const auto budget = kMaxGameFileName > prefix.size() + extension.size()
                            ? kMaxGameFileName - prefix.size() - extension.size()
                            : 0;
    if (name.size() > budget) name.resize(budget);
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return prefix + name + std::string(extension);
}

InstallResult install_game(const std::filesystem::path& root, const std::filesystem::path& iso_path,
                           const InstallOptions& options, Progress progress, std::stop_token stop)
{
    InstallResult result;
    FileBlockDevice iso(iso_path);
    if (!iso.is_open()) {
        result.error = "Could not open " + iso_path.string();
        return result;
    }
    const auto info = iso::inspect_ps2_iso(iso);
    const auto id = info.ok ? opl::canonical_game_id(info.game.startup) : std::nullopt;
    if (!id) {
        result.error = info.ok ? "SYSTEM.CNF boot ID '" + info.game.startup + "' is not an OPL serial"
                               : "Not a PS2 game image: " + info.error;
        return result;
    }
    result.startup = *id;
    result.bytes = iso.size_bytes();
    result.media = options.media.value_or(result.bytes <= kCdThreshold ? hdl::MediaType::cd : hdl::MediaType::dvd);

    const auto layout = ensure_layout(root);
    if (!layout.ok) {
        result.error = layout.error;
        return result;
    }
    const auto title = options.title.empty() ? iso_path.stem().string() : options.title;
    result.path = root / (result.media == hdl::MediaType::cd ? "CD" : "DVD") / game_file_name(*id, title);

    std::error_code ec;
    for (const auto& game : scan_library(root, false)) {
        if (game.startup == *id && !options.overwrite) {
            result.error = "The library already contains " + *id + ": " + game.path.filename().string();
            return result;
        }
    }
    if (std::filesystem::exists(result.path, ec) && !options.overwrite) {
        result.error = "Destination already exists: " + result.path.string();
        return result;
    }

    auto partial = result.path;
    partial += ".partial";
    {
        // Reserve the whole image first so the filesystem can allocate one
        // contiguous run (exFAT and NTFS honour this; zero-fill is lazy).
        std::ofstream create(partial, std::ios::binary | std::ios::trunc);
        if (!create) {
            result.error = "Could not create " + partial.string();
            return result;
        }
    }
    std::filesystem::resize_file(partial, result.bytes, ec);
    if (ec) {
        std::filesystem::remove(partial, ec);
        result.error = "Not enough free space for " + result.path.filename().string();
        return result;
    }

    const auto fail = [&](std::string message) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        result.error = std::move(message);
        return result;
    };
    std::fstream out(partial, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) return fail("Could not open " + partial.string() + " for writing");
    std::vector<std::byte> buffer(std::max<std::size_t>(64U * 1024U, options.chunk_bytes));
    for (std::uint64_t offset = 0; offset < result.bytes;) {
        if (stop.stop_requested()) {
            out.close();
            result.cancelled = true;
            return fail("Install cancelled");
        }
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), result.bytes - offset));
        if (!iso.read(offset, std::span(buffer).first(n))) return fail("Read error in " + iso_path.string());
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(n));
        if (!out) return fail("Write error (library volume full or removed?)");
        offset += n;
        if (progress) progress(offset, result.bytes);
    }
    out.flush();
    out.close();
    if (!out) return fail("Could not flush " + partial.string());
    std::filesystem::rename(partial, result.path, ec);
    if (ec) return fail("Could not move the finished image into place: " + ec.message());

    result.fragments = count_fragments(result.path);
    if (result.fragments.known && result.fragments.extents > kOplMaxFragments) {
        result.warnings.push_back("Image has " + std::to_string(result.fragments.extents) +
                                  " fragments; OPL may refuse it. Defragment the volume and reinstall.");
    } else if (result.fragments.known && result.fragments.extents > 1) {
        result.warnings.push_back("Image has " + std::to_string(result.fragments.extents) +
                                  " fragments (OPL tolerates up to " + std::to_string(kOplMaxFragments) + ").");
    }
    result.ok = true;
    return result;
}

std::vector<LibraryGame> scan_library(const std::filesystem::path& root, bool measure_fragments)
{
    std::vector<LibraryGame> games;
    for (const auto* folder : {"DVD", "CD"}) {
        std::error_code ec;
        const auto dir = root / folder;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto ext = upper(entry.path().extension().string());
            if (ext != ".ISO" && ext != ".ZSO") continue;
            LibraryGame game;
            game.path = entry.path();
            game.cd = std::string_view(folder) == "CD";
            game.bytes = entry.file_size(ec);
            const auto name = entry.path().stem().string();
            if (const auto id = id_from_name(name)) {
                game.startup = *id;
                game.title = name.size() > 12 ? name.substr(12) : name;
            } else if (ext == ".ISO") {
                FileBlockDevice iso(entry.path());
                const auto info = iso.is_open() ? iso::inspect_ps2_iso(iso) : iso::GameSourceResult{};
                if (const auto parsed = info.ok ? opl::canonical_game_id(info.game.startup) : std::nullopt) {
                    game.startup = *parsed;
                } else {
                    game.error = info.ok ? "unrecognised boot ID" : info.error;
                }
                game.title = name;
            } else {
                game.error = "ZSO without an ID prefix in its file name";
                game.title = name;
            }
            if (measure_fragments) game.fragments = count_fragments(entry.path());
            games.push_back(std::move(game));
        }
    }
    std::sort(games.begin(), games.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return games;
}

AssetSyncResult sync_assets(const std::filesystem::path& root, HttpClient& http,
                            const std::filesystem::path& cache_root, bool apply, opl::AssetOptions assets,
                            opl::FetchOptions fetch)
{
    AssetSyncResult result;
    const auto layout = ensure_layout(root);
    if (!layout.ok) {
        result.error = layout.error;
        return result;
    }
    // BDM roots use plain files; catalogs and HDD-OSD icons do not apply.
    assets.layout = opl::LayoutMode::classic_files;
    assets.title_database = false;
    assets.redump_database = false;
    assets.hdd_osd_icon = false;

    std::set<std::string> present;
    for (const auto* dir : {"ART", "CFG", "CHT"}) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(root / dir, ec)) {
            if (entry.is_regular_file(ec)) {
                present.insert(opl::asset_inventory_key(std::string(dir) + "/" + entry.path().filename().string(), ""));
            }
        }
    }

    std::set<std::string> seen_ids;
    for (const auto& game : scan_library(root, false)) {
        if (game.startup.empty()) {
            result.issues.push_back(game.path.filename().string() + ": " + game.error);
            continue;
        }
        if (!seen_ids.insert(game.startup).second) continue;
        ++result.games;
        auto plan = opl::plan_assets({game.startup, game.title}, assets);
        if (!plan.ok) {
            result.issues.push_back(game.startup + ": " + plan.error);
            continue;
        }
        std::vector<opl::AssetRequest> missing;
        for (auto& request : plan.requests) {
            if (request.placement != opl::Placement::opl_data || request.candidates.empty()) continue;
            const bool exists = std::any_of(request.candidates.begin(), request.candidates.end(), [&](const auto& c) {
                return present.contains(opl::asset_inventory_key(c.target_path, c.archive_member));
            });
            if (exists) ++result.already_present;
            else missing.push_back(std::move(request));
        }
        if (missing.empty()) continue;
        plan.requests = std::move(missing);
        const auto fetched = opl::fetch_assets(plan, http, cache_root, fetch);
        for (const auto& issue : fetched.issues) result.issues.push_back(game.startup + ": " + issue.message);
        for (const auto& asset : fetched.assets) {
            if (asset.placement != opl::Placement::opl_data) continue;
            ++result.staged;
            if (!apply) continue;
            const auto target = root / std::filesystem::path(asset.target_path);
            std::error_code ec;
            if (std::filesystem::exists(target, ec)) continue; // user files always win
            auto temp = target;
            temp += ".partial";
            std::filesystem::copy_file(asset.staged_path, temp, std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) std::filesystem::rename(temp, target, ec);
            if (ec) {
                std::filesystem::remove(temp, ec);
                result.issues.push_back("Could not write " + asset.target_path);
                continue;
            }
            present.insert(opl::asset_inventory_key(asset.target_path, ""));
            ++result.written;
        }
    }
    result.ok = true;
    return result;
}

} // namespace ps2hdd::bdm
