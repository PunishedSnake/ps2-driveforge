#include "ps2hdd/opl_library_sync.hpp"

#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/hdl.hpp"
#include "ps2hdd/opl_partition.hpp"
#include "ps2hdd/pfs.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace ps2hdd::opl {
namespace {

constexpr std::size_t kTarRecord = 512;
// Inventory is metadata, not payload: refuse to slurp pathological archives.
constexpr std::uint64_t kMaxInventoryArchiveBytes = 512ULL * 1024ULL * 1024ULL;

std::string upper_ascii(std::string_view value)
{
    std::string result(value);
    for (char& ch : result) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return result;
}

std::string_view strip_extension(std::string_view name)
{
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string_view first_component(std::string_view path)
{
    while (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        path.remove_prefix(1);
    }
    const auto slash = path.find_first_of("/\\");
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

std::string_view last_component(std::string_view path)
{
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string c_field(const std::byte* p, std::size_t size)
{
    std::string value;
    for (std::size_t i = 0; i < size; ++i) {
        const auto ch = std::to_integer<char>(p[i]);
        if (ch == '\0') {
            break;
        }
        value.push_back(ch);
    }
    return value;
}

std::optional<std::uint64_t> octal_field(const std::byte* p, std::size_t size)
{
    std::uint64_t value = 0;
    bool any = false;
    for (std::size_t i = 0; i < size; ++i) {
        const auto ch = std::to_integer<char>(p[i]);
        if (ch == '\0' || ch == ' ') {
            if (any) {
                break;
            }
            continue;
        }
        if (ch < '0' || ch > '7' || value > (UINT64_MAX >> 3U)) {
            return std::nullopt;
        }
        value = value * 8U + static_cast<std::uint64_t>(ch - '0');
        any = true;
    }
    return value;
}

// Member names of a classic ustar archive. Returns nullopt on malformed input;
// the archive writer will refuse it later anyway, but sync must not guess.
std::optional<std::vector<std::string>> tar_member_names(std::span<const std::byte> archive)
{
    std::vector<std::string> names;
    std::size_t offset = 0;
    while (offset + kTarRecord <= archive.size()) {
        const std::byte* header = archive.data() + offset;
        if (std::all_of(header, header + kTarRecord, [](std::byte b) { return b == std::byte{0}; })) {
            return names;
        }
        const auto size = octal_field(header + 124, 12);
        if (!size) {
            return std::nullopt;
        }
        std::string name = c_field(header, 100);
        const std::string prefix = c_field(header + 345, 155);
        if (!prefix.empty()) {
            name = prefix + "/" + name;
        }
        const auto type = std::to_integer<char>(header[156]);
        if (type == '0' || type == '\0') {
            names.push_back(std::move(name));
        }
        const std::uint64_t payload = (*size + kTarRecord - 1) / kTarRecord * kTarRecord;
        if (payload > archive.size() - offset - kTarRecord) {
            return std::nullopt;
        }
        offset += kTarRecord + static_cast<std::size_t>(payload);
    }
    return offset == archive.size() ? std::optional(names) : std::nullopt;
}

const apa::Partition* main_partition(const apa::ScanResult& scan, std::string_view id)
{
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.id == id) {
            return &partition;
        }
    }
    return nullptr;
}

AssetOptions sync_asset_options(AssetOptions options)
{
    options.title_database = false;
    options.redump_database = false;
    options.hdd_osd_icon = false;
    return options;
}

} // namespace

std::size_t LibrarySyncPlan::games_with_updates() const noexcept
{
    return static_cast<std::size_t>(std::count_if(games.begin(), games.end(), [](const auto& game) {
        return game.state == LibrarySyncGameState::fetched;
    }));
}

const char* library_sync_state_name(LibrarySyncGameState state) noexcept
{
    switch (state) {
    case LibrarySyncGameState::up_to_date: return "up to date";
    case LibrarySyncGameState::fetched: return "assets staged";
    case LibrarySyncGameState::nothing_found: return "nothing found";
    case LibrarySyncGameState::skipped:
    default: return "skipped";
    }
}

std::string asset_inventory_key(std::string_view target_path, std::string_view archive_member)
{
    const auto directory = first_component(target_path);
    const auto name = archive_member.empty() ? last_component(target_path)
                                             : last_component(archive_member);
    return upper_ascii(directory) + "/" + upper_ascii(strip_extension(name));
}

std::set<std::string> inventory_opl_assets(BlockDevice& device, const apa::Partition& partition,
                                           std::vector<std::string>* warnings)
{
    std::set<std::string> keys;
    ApaVolume volume(device, partition);
    pfs::Reader reader(volume);
    if (!reader.valid()) {
        if (warnings) warnings->push_back("OPL partition failed PFS validation during inventory");
        return keys;
    }
    const auto root = reader.root();
    if (!root) {
        return keys;
    }
    for (const auto& top : reader.list_directory(*root)) {
        const std::string directory = upper_ascii(top.name);
        if (!top.is_directory() || (directory != "ART" && directory != "CFG" && directory != "CHT")) {
            continue;
        }
        const auto node = reader.read_inode(top.inode);
        if (!node) {
            continue;
        }
        for (const auto& entry : reader.list_directory(*node)) {
            if (!entry.is_regular()) {
                continue;
            }
            const std::string upper_name = upper_ascii(entry.name);
            const bool container = upper_name == directory + ".TAR";
            if (!container) {
                keys.insert(directory + "/" + upper_ascii(strip_extension(entry.name)));
                continue;
            }
            const auto archive_node = reader.read_inode(entry.inode);
            if (!archive_node || archive_node->inode.size > kMaxInventoryArchiveBytes) {
                if (warnings) warnings->push_back(directory + "/" + entry.name + " is unreadable or too large to inventory");
                continue;
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(archive_node->inode.size));
            if (!reader.read(*archive_node, 0, bytes)) {
                if (warnings) warnings->push_back("Could not read " + directory + "/" + entry.name);
                continue;
            }
            const auto members = tar_member_names(bytes);
            if (!members) {
                if (warnings) warnings->push_back(directory + "/" + entry.name + " is not a valid TAR archive");
                continue;
            }
            for (const auto& member : *members) {
                keys.insert(directory + "/" + upper_ascii(strip_extension(last_component(member))));
            }
        }
    }
    return keys;
}

LibrarySyncPlan plan_library_sync(BlockDevice& device, const apa::ScanResult& scan,
                                  HttpClient& http, const std::filesystem::path& cache_root,
                                  const LibrarySyncOptions& options,
                                  LibrarySyncProgress progress, std::stop_token stop)
{
    LibrarySyncPlan plan;
    const auto resolution = resolve_data_partition(
        device, scan,
        options.opl_partition ? std::optional<std::string_view>(*options.opl_partition) : std::nullopt);
    if (!resolution.ok) {
        plan.error = resolution.error.empty() ? "No OPL data partition could be resolved" : resolution.error;
        return plan;
    }
    const auto& opl_partition = scan.partitions[resolution.partition_index];
    plan.opl_partition_id = opl_partition.id;
    plan.opl_partition_start_lba = opl_partition.start_lba;

    std::vector<std::string> warnings;
    const auto present = inventory_opl_assets(device, opl_partition, &warnings);
    const auto asset_options = sync_asset_options(options.assets);

    const auto catalog = hdl::read_catalog(device, scan);
    std::map<std::string, std::string> claimed; // game_id -> partition
    std::size_t completed = 0;
    for (const auto& entry : catalog.entries) {
        if (stop.stop_requested()) {
            plan.cancelled = true;
            plan.error = "Library sync cancelled";
            return plan;
        }
        LibrarySyncGame game;
        game.partition_id = entry.game.partition_id;
        game.title = entry.game.title;

        const auto id = entry.ok ? canonical_game_id(entry.game.startup) : std::nullopt;
        if (!entry.ok) {
            game.issues.push_back("HDL header unreadable: " + entry.error);
        } else if (!id) {
            game.issues.push_back("Startup ID '" + entry.game.startup + "' is not an OPL serial");
        } else if (const auto other = claimed.find(*id); other != claimed.end()) {
            game.game_id = *id;
            game.issues.push_back("Same startup ID as " + other->second + "; assets are shared");
        } else {
            game.game_id = *id;
            claimed.emplace(*id, game.partition_id);

            auto asset_plan = plan_assets({*id, entry.game.title}, asset_options);
            if (!asset_plan.ok) {
                game.issues.push_back(asset_plan.error);
            } else {
                std::vector<AssetRequest> missing;
                for (auto& request : asset_plan.requests) {
                    if (request.placement != Placement::opl_data || request.candidates.empty()) {
                        continue;
                    }
                    const bool exists = std::any_of(
                        request.candidates.begin(), request.candidates.end(),
                        [&](const AssetCandidate& candidate) {
                            return present.contains(asset_inventory_key(candidate.target_path,
                                                                         candidate.archive_member));
                        });
                    if (exists && options.fill_missing_only) {
                        ++game.already_present;
                    } else {
                        missing.push_back(std::move(request));
                    }
                }
                game.missing = missing.size();
                if (missing.empty()) {
                    game.state = LibrarySyncGameState::up_to_date;
                } else {
                    asset_plan.requests = std::move(missing);
                    auto fetched = fetch_assets(asset_plan, http, cache_root, options.fetch);
                    for (const auto& issue : fetched.issues) {
                        game.issues.push_back(std::string(asset_kind_name(issue.kind)) + ": " + issue.message);
                    }
                    for (auto& asset : fetched.assets) {
                        if (asset.placement != Placement::opl_data) {
                            continue;
                        }
                        ++game.fetched;
                        plan.assets.push_back(std::move(asset));
                    }
                    game.state = game.fetched > 0 ? LibrarySyncGameState::fetched
                                                  : LibrarySyncGameState::nothing_found;
                }
            }
        }
        plan.games.push_back(std::move(game));
        ++completed;
        if (progress) {
            progress(completed, catalog.entries.size(), plan.games.back());
        }
    }

    if (!warnings.empty()) {
        LibrarySyncGame inventory;
        inventory.partition_id = plan.opl_partition_id;
        inventory.title = "OPL partition inventory";
        inventory.issues = std::move(warnings);
        plan.games.push_back(std::move(inventory));
    }
    plan.ok = true;
    return plan;
}

LibrarySyncResult apply_library_sync(WritableBlockDevice& device, const LibrarySyncPlan& plan,
                                     const PfsImportOptions& options)
{
    LibrarySyncResult result;
    if (!plan.ok) {
        result.error = plan.error.empty() ? "Library sync plan is not valid" : plan.error;
        return result;
    }
    if (plan.assets.empty()) {
        result.ok = true;
        return result;
    }

    // Stale-plan guard: the plan was built against one exact OPL partition.
    apa::Reader reader(device);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        result.error = "Refusing library sync: APA scan is no longer clean";
        return result;
    }
    const auto* partition = main_partition(scan, plan.opl_partition_id);
    if (!partition || partition->type != apa::kTypePfs ||
        partition->start_lba != plan.opl_partition_start_lba) {
        result.error = "Refusing library sync: OPL partition " + plan.opl_partition_id +
                       " changed since the plan was built";
        return result;
    }

    result.import = import_fetched_assets(device, *partition, plan.assets, options);
    result.files_written = result.import.batch.succeeded;
    for (const auto& archive : result.import.archives) {
        result.files_written += archive.replaced_members + archive.added_members;
    }
    result.ok = result.import.ok;
    if (!result.ok) {
        result.error = result.import.error.empty() ? "PFS import failed" : result.import.error;
    }
    return result;
}

} // namespace ps2hdd::opl
