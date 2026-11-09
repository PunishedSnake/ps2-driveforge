#include "ps2hdd/apa_forensic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ps2hdd::forensic {
namespace {

constexpr std::size_t kSectorBytes = 512;
constexpr std::size_t kMagicOffset = 0x004;
constexpr std::size_t kNextOffset = 0x008;
constexpr std::size_t kPrevOffset = 0x00c;
constexpr std::size_t kIdOffset = 0x010;
constexpr std::size_t kIdBytes = 32;
constexpr std::size_t kStartOffset = 0x040;
constexpr std::size_t kLengthOffset = 0x044;
constexpr std::size_t kTypeOffset = 0x048;
constexpr std::size_t kFlagsOffset = 0x04a;
constexpr std::size_t kNsubOffset = 0x04c;
constexpr std::size_t kMainOffset = 0x058;
constexpr std::size_t kNumberOffset = 0x05c;
constexpr std::size_t kSubsOffset = 0x200;
constexpr std::size_t kSubEntryBytes = 8;
constexpr std::uint32_t kMaxSubs = 64;

struct Index {
    ScanResult* result{};
    std::vector<std::uint16_t> by_lba;
};

[[nodiscard]] std::uint16_t load_u16(const std::byte* p) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[0])) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[1])) << 8U);
}

[[nodiscard]] std::uint32_t load_u32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

void store_u32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::uint32_t checksum(std::span<const std::byte, kApaHeaderBytes> header) noexcept
{
    std::uint32_t sum = 0;
    for (std::size_t i = 1; i < 256; ++i) {
        sum += load_u32(header.data() + static_cast<std::ptrdiff_t>(i * 4));
    }
    return sum;
}

[[nodiscard]] bool plausible_type(std::uint16_t type) noexcept
{
    switch (type) {
    case 0x0000:
    case 0x0001:
    case 0x0082:
    case 0x0083:
    case 0x0088:
    case 0x0100:
    case 0x0101:
    case 0x1337:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool plausible_id(const std::byte* id) noexcept
{
    bool saw = false;
    for (std::size_t i = 0; i < kIdBytes; ++i) {
        const auto value = std::to_integer<unsigned char>(id[i]);
        if (value == 0) break;
        if (value < 0x20 || value > 0x7e) return false;
        saw = true;
    }
    return saw;
}

[[nodiscard]] unsigned clamp_confidence(int value) noexcept
{
    return static_cast<unsigned>(std::clamp(value, 0, 100));
}

[[nodiscard]] std::string read_id(const std::byte* id)
{
    std::string value;
    for (std::size_t i = 0; i < kIdBytes; ++i) {
        const auto ch = std::to_integer<unsigned char>(id[i]);
        if (ch == 0) break;
        value.push_back(static_cast<char>(ch));
    }
    return value;
}

[[nodiscard]] unsigned inspect_header(
    std::span<const std::byte, kApaHeaderBytes> header,
    std::uint32_t lba, std::uint32_t total_sectors,
    std::uint32_t source_evidence,
    std::uint32_t& evidence_out, std::uint32_t& calculated_checksum) noexcept
{
    constexpr std::uint32_t semantic_anchor = evidence_magic | evidence_length | evidence_id;
    constexpr std::uint32_t direct_anchor = evidence_magic | evidence_self_start;
    auto evidence = source_evidence;
    const auto start = load_u32(header.data() + kStartOffset);
    const auto length = load_u32(header.data() + kLengthOffset);
    const auto nsub = load_u32(header.data() + kNsubOffset);
    const auto type = load_u16(header.data() + kTypeOffset);
    const auto end = static_cast<std::uint64_t>(start) + length;
    int score = 0;
    calculated_checksum = 0;

    if (std::memcmp(header.data() + kMagicOffset, "APA\0", 4) == 0) {
        evidence |= evidence_magic;
        score += 25;
    }
    if (start == lba) {
        evidence |= evidence_self_start;
        score += 20;
    }
    if ((source_evidence & evidence_referenced) == 0 && (evidence & direct_anchor) == 0) {
        evidence_out = evidence;
        return 0;
    }

    calculated_checksum = checksum(header);
    if (load_u32(header.data()) == calculated_checksum) {
        evidence |= evidence_checksum;
        score += 20;
    }
    if (length != 0 && start < total_sectors && end <= total_sectors) {
        evidence |= evidence_length;
        score += 15;
    }
    if (nsub <= kMaxSubs) {
        evidence |= evidence_nsub;
        score += 5;
    }
    if (plausible_type(type)) {
        evidence |= evidence_type;
        score += 5;
    }
    if (plausible_id(header.data() + kIdOffset)) {
        evidence |= evidence_id;
        score += 10;
    }
    if ((evidence & semantic_anchor) == 0) score = 0;
    evidence_out = evidence;
    return clamp_confidence(score);
}

[[nodiscard]] int find_node(const Index& index, std::uint32_t lba) noexcept
{
    const auto it = std::lower_bound(index.by_lba.begin(), index.by_lba.end(), lba,
        [&](std::uint16_t node_index, std::uint32_t value) {
            return index.result->nodes[node_index].lba < value;
        });
    if (it != index.by_lba.end() && index.result->nodes[*it].lba == lba) return *it;
    return -1;
}

void index_node(Index& index, std::uint16_t node_index)
{
    const auto lba = index.result->nodes[node_index].lba;
    const auto it = std::lower_bound(index.by_lba.begin(), index.by_lba.end(), lba,
        [&](std::uint16_t existing, std::uint32_t value) {
            return index.result->nodes[existing].lba < value;
        });
    index.by_lba.insert(it, node_index);
}

int add_candidate(Index& index, BlockDevice& device,
                  std::uint32_t lba, std::uint32_t total_sectors,
                  std::uint32_t source_evidence)
{
    auto& result = *index.result;
    if (lba >= total_sectors || total_sectors - lba < 2) return 0;
    const auto existing = find_node(index, lba);
    if (existing >= 0) {
        result.nodes[static_cast<std::size_t>(existing)].evidence |= source_evidence;
        return 0;
    }

    std::array<std::byte, kApaHeaderBytes> header{};
    const auto offset = static_cast<std::uint64_t>(lba) * kSectorBytes;
    if (!device.read(offset, header)) {
        ++result.unreadable_reads;
        return -1;
    }
    std::uint32_t evidence = 0;
    std::uint32_t calculated = 0;
    const auto confidence = inspect_header(header, lba, total_sectors, source_evidence,
                                           evidence, calculated);
    if (confidence < 30 && !(lba == 0 && confidence >= 20)) return 0;
    if (result.nodes.size() >= kMaxNodes) {
        result.truncated = true;
        return 1;
    }

    Node node;
    node.lba = lba;
    node.stored_checksum = load_u32(header.data());
    node.calculated_checksum = calculated;
    node.next = load_u32(header.data() + kNextOffset);
    node.prev = load_u32(header.data() + kPrevOffset);
    node.start = load_u32(header.data() + kStartOffset);
    node.length = load_u32(header.data() + kLengthOffset);
    node.type = load_u16(header.data() + kTypeOffset);
    node.flags = load_u16(header.data() + kFlagsOffset);
    node.nsub = load_u32(header.data() + kNsubOffset);
    node.main = load_u32(header.data() + kMainOffset);
    node.number = load_u32(header.data() + kNumberOffset);
    node.evidence = evidence;
    node.confidence = confidence;
    node.id = read_id(header.data() + kIdOffset);
    node.header = header;
    result.nodes.push_back(std::move(node));
    index_node(index, static_cast<std::uint16_t>(result.nodes.size() - 1));
    return 1;
}

[[nodiscard]] bool referenced_candidate(const Node& node) noexcept
{
    constexpr auto required = evidence_magic | evidence_length;
    return node.confidence >= 50 && (node.evidence & required) == required;
}

void follow_reference(Index& index, BlockDevice& device,
                      std::uint32_t lba, std::uint32_t total_sectors)
{
    auto& result = *index.result;
    if (lba == 0 || lba >= total_sectors) return;
    const auto existing = find_node(index, lba);
    if (existing >= 0) {
        result.nodes[static_cast<std::size_t>(existing)].evidence |= evidence_referenced;
        return;
    }
    ++result.reference_reads;
    (void)add_candidate(index, device, lba, total_sectors, evidence_referenced);
}

void chase_references(Index& index, BlockDevice& device, std::uint32_t total_sectors)
{
    auto& result = *index.result;
    std::size_t cursor = 0;
    while (cursor < result.nodes.size() && !result.truncated) {
        const auto node = result.nodes[cursor++];
        if (!referenced_candidate(node)) continue;
        follow_reference(index, device, node.next, total_sectors);
        follow_reference(index, device, node.prev, total_sectors);
        follow_reference(index, device, node.main, total_sectors);
        if (node.nsub <= kMaxSubs && node.confidence >= 60) {
            for (std::size_t i = 0; i < node.nsub; ++i) {
                const auto sub = load_u32(node.header.data() + kSubsOffset + i * kSubEntryBytes);
                follow_reference(index, device, sub, total_sectors);
            }
        }
    }
}

[[nodiscard]] int unique_link_candidate(const Index& index,
                                        const std::array<bool, kMaxNodes>& excluded,
                                        std::uint32_t current_lba,
                                        bool match_prev) noexcept
{
    int found = -1;
    for (std::size_t i = 0; i < index.result->nodes.size(); ++i) {
        const auto& node = index.result->nodes[i];
        const auto link = match_prev ? node.prev : node.next;
        if (excluded[i] || node.confidence < 55 || link != current_lba) continue;
        if (found >= 0) return -1;
        found = static_cast<int>(i);
    }
    return found;
}

Map build_forward_map(const Index& index)
{
    Map map;
    map.kind = MapKind::forward;
    std::array<bool, kMaxNodes> in_map{};
    const auto master = find_node(index, 0);
    if (master < 0) return map;
    map.order.push_back(static_cast<std::uint16_t>(master));
    in_map[static_cast<std::size_t>(master)] = true;
    auto current = master;
    while (map.order.size() < kMaxNodes) {
        const auto next_lba = index.result->nodes[static_cast<std::size_t>(current)].next;
        auto next = next_lba != 0 ? find_node(index, next_lba) : -1;
        if (next_lba == 0) {
            if (current == master) next = unique_link_candidate(index, in_map, 0, true);
            else break;
        }
        if (next < 0 || in_map[static_cast<std::size_t>(next)]) {
            next = unique_link_candidate(index, in_map,
                                         index.result->nodes[static_cast<std::size_t>(current)].lba,
                                         true);
        }
        if (next < 0 || in_map[static_cast<std::size_t>(next)]) break;
        map.order.push_back(static_cast<std::uint16_t>(next));
        in_map[static_cast<std::size_t>(next)] = true;
        current = next;
    }
    return map;
}

[[nodiscard]] bool canonical_empty(const Node& node) noexcept
{
    constexpr auto required = evidence_magic | evidence_self_start | evidence_checksum |
                              evidence_length | evidence_type | evidence_id;
    return node.type == 0 && node.flags == 0 && node.length != 0 && node.id == "__empty" &&
           node.stored_checksum == node.calculated_checksum &&
           (node.evidence & required) == required;
}

void classify_dormant(ScanResult& result, const Map& forward)
{
    std::array<bool, kMaxNodes> in_forward{};
    for (const auto index : forward.order) in_forward[index] = true;
    result.dormant_free_nodes = 0;
    for (auto& node : result.nodes) node.evidence &= ~evidence_dormant_free;

    for (std::size_t i = 0; i < result.nodes.size(); ++i) {
        auto& node = result.nodes[i];
        if (in_forward[i] || !canonical_empty(node)) continue;
        const auto node_end = static_cast<std::uint64_t>(node.start) + node.length;
        for (const auto index : forward.order) {
            const auto& container = result.nodes[index];
            if (!canonical_empty(container) || node.start <= container.start) continue;
            const auto container_end = static_cast<std::uint64_t>(container.start) + container.length;
            if (node_end <= container_end) {
                node.evidence |= evidence_dormant_free;
                ++result.dormant_free_nodes;
                break;
            }
        }
    }
}

Map build_reverse_map(const Index& index)
{
    Map map;
    map.kind = MapKind::reverse;
    std::array<bool, kMaxNodes> seen{};
    const auto master = find_node(index, 0);
    if (master < 0) return map;
    std::vector<std::uint16_t> reverse;
    auto current = index.result->nodes[static_cast<std::size_t>(master)].prev != 0
                       ? find_node(index, index.result->nodes[static_cast<std::size_t>(master)].prev)
                       : -1;
    if (current < 0) current = unique_link_candidate(index, seen, 0, false);
    while (current >= 0 && reverse.size() < kMaxNodes - 1) {
        if (seen[static_cast<std::size_t>(current)]) break;
        reverse.push_back(static_cast<std::uint16_t>(current));
        seen[static_cast<std::size_t>(current)] = true;
        const auto prev_lba = index.result->nodes[static_cast<std::size_t>(current)].prev;
        if (prev_lba == 0) break;
        auto prev = find_node(index, prev_lba);
        if (prev < 0) {
            prev = unique_link_candidate(index, seen,
                                         index.result->nodes[static_cast<std::size_t>(current)].lba,
                                         false);
        }
        current = prev;
    }
    map.order.push_back(static_cast<std::uint16_t>(master));
    std::reverse(reverse.begin(), reverse.end());
    map.order.insert(map.order.end(), reverse.begin(), reverse.end());
    return map;
}

Map build_geometry_map(const ScanResult& result)
{
    Map map;
    map.kind = MapKind::geometry;
    for (std::size_t i = 0; i < result.nodes.size() && map.order.size() < kMaxNodes; ++i) {
        const auto& node = result.nodes[i];
        constexpr auto required = evidence_self_start | evidence_length;
        if ((node.evidence & evidence_dormant_free) != 0) continue;
        if (node.lba == 0 || (node.confidence >= 50 && (node.evidence & required) == required)) {
            map.order.push_back(static_cast<std::uint16_t>(i));
        }
    }
    std::sort(map.order.begin(), map.order.end(), [&](std::uint16_t a, std::uint16_t b) {
        return result.nodes[a].lba < result.nodes[b].lba;
    });
    return map;
}

[[nodiscard]] bool maps_equal(const Map& a, const Map& b) noexcept
{
    return a.order == b.order;
}

void evaluate_map(const Index& index, Map& map)
{
    auto& result = *index.result;
    if (map.order.empty()) {
        map.confidence = 0;
        return;
    }
    unsigned high_nodes = 0;
    for (const auto& node : result.nodes) {
        if (node.confidence >= 60 && (node.evidence & evidence_dormant_free) == 0) ++high_nodes;
    }
    unsigned weak_nodes = 0;
    int score = 100;
    for (std::size_t i = 0; i < map.order.size(); ++i) {
        const auto& node = result.nodes[map.order[i]];
        if (node.confidence < 60) ++weak_nodes;
        std::uint32_t desired_prev = 0;
        std::uint32_t desired_next = 0;
        if (i == 0) {
            desired_prev = map.order.size() > 1 ? result.nodes[map.order.back()].lba : 0;
            desired_next = map.order.size() > 1 ? result.nodes[map.order[1]].lba : 0;
        } else {
            desired_prev = i == 1 ? 0 : result.nodes[map.order[i - 1]].lba;
            desired_next = i + 1 < map.order.size() ? result.nodes[map.order[i + 1]].lba : 0;
        }
        if (node.prev != desired_prev) {
            const auto target = node.prev != 0 ? find_node(index, node.prev) : -1;
            ++map.inferred_links;
            if (target >= 0) ++map.conflicts;
        }
        if (node.next != desired_next) {
            const auto target = node.next != 0 ? find_node(index, node.next) : -1;
            ++map.inferred_links;
            if (target >= 0) ++map.conflicts;
        }
        if (i + 1 < map.order.size()) {
            const auto& next = result.nodes[map.order[i + 1]];
            if (node.next == next.lba && next.prev == node.lba) ++map.reciprocal_links;
            if (i > 0 && static_cast<std::uint64_t>(node.start) + node.length > next.start) {
                ++map.overlaps;
            }
        }
    }
    score -= static_cast<int>(map.conflicts) * 15;
    score -= static_cast<int>(map.overlaps) * 30;
    score -= static_cast<int>(map.inferred_links) * 2;
    score -= static_cast<int>(weak_nodes) * 4;
    if (high_nodes > map.order.size()) score -= static_cast<int>(high_nodes - map.order.size()) * 5;
    if (result.truncated) score -= 10;
    map.confidence = clamp_confidence(score);
    map.repairable = !result.truncated && map.order.size() >= 2 &&
                     result.nodes[map.order.front()].lba == 0 && map.confidence >= 85 &&
                     map.conflicts == 0 && map.overlaps == 0;
    if (map.repairable) {
        constexpr auto required = evidence_magic | evidence_self_start | evidence_length;
        for (const auto node_index : map.order) {
            if ((result.nodes[node_index].evidence & required) != required) {
                map.repairable = false;
                break;
            }
        }
    }
}

void add_unique_map(ScanResult& result, Map candidate)
{
    if (candidate.order.empty() || result.maps.size() >= kMaxMaps) return;
    for (const auto& existing : result.maps) {
        if (maps_equal(existing, candidate)) return;
    }
    result.maps.push_back(std::move(candidate));
}

[[nodiscard]] bool patch_checksum_corroborated(const Node& node,
                                                std::uint32_t new_next,
                                                std::uint32_t new_prev) noexcept
{
    auto repaired = node.header;
    store_u32(repaired.data() + kNextOffset, new_next);
    store_u32(repaired.data() + kPrevOffset, new_prev);
    return checksum(repaired) == node.stored_checksum;
}

} // namespace

unsigned Patch::bit_distance() const noexcept
{
    return std::popcount(old_next ^ new_next) + std::popcount(old_prev ^ new_prev);
}

ScanResult scan_apa(BlockDevice& device, ProgressCallback progress)
{
    ScanResult result;
    if (device.size_bytes() < kApaHeaderBytes || device.size_bytes() % kSectorBytes != 0) {
        result.error = "Forensic APA scan requires a sector-aligned device of at least 1024 bytes";
        return result;
    }
    const auto sectors64 = device.size_bytes() / kSectorBytes;
    if (sectors64 > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "Forensic APA scanner supports the same 32-bit LBA range as fhdb-bootstrap-manager";
        return result;
    }
    result.total_sectors = static_cast<std::uint32_t>(sectors64);
    result.nodes.reserve(kMaxNodes);
    result.maps.reserve(kMaxMaps);
    Index index{&result, {}};
    index.by_lba.reserve(kMaxNodes);

    ++result.grid_reads;
    (void)add_candidate(index, device, 0, result.total_sectors, evidence_grid);
    if (progress) progress(0, result.total_sectors, result.nodes.size());

    for (std::uint32_t lba = kScanStep; lba < result.total_sectors && !result.truncated;) {
        ++result.grid_reads;
        (void)add_candidate(index, device, lba, result.total_sectors, evidence_grid);
        if (progress && ((result.grid_reads & 31U) == 0U ||
                         result.total_sectors - lba <= kScanStep)) {
            progress(lba, result.total_sectors, result.nodes.size());
        }
        if (std::numeric_limits<std::uint32_t>::max() - lba < kScanStep) break;
        lba += kScanStep;
    }

    chase_references(index, device, result.total_sectors);
    auto forward = build_forward_map(index);
    classify_dormant(result, forward);
    evaluate_map(index, forward);
    add_unique_map(result, std::move(forward));
    auto reverse = build_reverse_map(index);
    evaluate_map(index, reverse);
    add_unique_map(result, std::move(reverse));
    auto geometry = build_geometry_map(result);
    evaluate_map(index, geometry);
    add_unique_map(result, std::move(geometry));
    if (progress) progress(result.total_sectors, result.total_sectors, result.nodes.size());
    result.ok = true;
    return result;
}

RepairPlan build_repair_plan(const ScanResult& result, std::size_t map_index)
{
    RepairPlan plan;
    plan.map_index = map_index;
    if (!result.ok || map_index >= result.maps.size()) return plan;
    const auto& map = result.maps[map_index];
    plan.confidence = map.confidence;
    if (result.truncated || !map.repairable) return plan;

    for (std::size_t i = 0; i < map.order.size(); ++i) {
        const auto node_index = map.order[i];
        const auto& node = result.nodes[node_index];
        std::uint32_t desired_prev = 0;
        std::uint32_t desired_next = 0;
        if (i == 0) {
            desired_prev = map.order.size() > 1 ? result.nodes[map.order.back()].lba : 0;
            desired_next = map.order.size() > 1 ? result.nodes[map.order[1]].lba : 0;
        } else {
            desired_prev = i == 1 ? 0 : result.nodes[map.order[i - 1]].lba;
            desired_next = i + 1 < map.order.size() ? result.nodes[map.order[i + 1]].lba : 0;
        }
        if (node.prev == desired_prev && node.next == desired_next) continue;
        Patch patch;
        patch.node_index = node_index;
        patch.lba = node.lba;
        patch.old_next = node.next;
        patch.old_prev = node.prev;
        patch.new_next = desired_next;
        patch.new_prev = desired_prev;
        patch.checksum_corroborated = patch_checksum_corroborated(node, desired_next, desired_prev);
        if (patch.checksum_corroborated) ++plan.corroborated_count;
        else ++plan.speculative_count;
        plan.patches.push_back(patch);
    }
    plan.automatic_safe = !result.truncated && !plan.patches.empty() &&
                          plan.speculative_count == 0 && map.confidence >= 90;
    plan.manual_allowed = !result.truncated && !plan.patches.empty() &&
                          map.confidence >= 90 && map.conflicts == 0 && map.overlaps == 0;
    return plan;
}

bool build_patched_header(const ScanResult& result, const Patch& patch,
                          std::array<std::byte, kApaHeaderBytes>& repaired,
                          std::string& error) noexcept
{
    error.clear();
    if (!result.ok || patch.node_index >= result.nodes.size()) {
        error = "Forensic patch references an invalid scan node";
        return false;
    }
    const auto& node = result.nodes[patch.node_index];
    if (node.lba != patch.lba || node.next != patch.old_next || node.prev != patch.old_prev) {
        error = "Forensic patch no longer matches the scanned topology";
        return false;
    }
    repaired = node.header;
    store_u32(repaired.data() + kNextOffset, patch.new_next);
    store_u32(repaired.data() + kPrevOffset, patch.new_prev);
    store_u32(repaired.data(), checksum(repaired));
    return true;
}

const char* map_name(MapKind kind) noexcept
{
    switch (kind) {
    case MapKind::forward: return "forward links";
    case MapKind::reverse: return "reverse links";
    case MapKind::geometry: return "geometry order";
    default: return "unknown";
    }
}

std::string render_report(const ScanResult& result)
{
    std::ostringstream out;
    out << "PS2 HDD Bootstrap Manager - APA FORENSIC REPORT\n\n";
    out << std::hex << std::setfill('0');
    out << "Disk sectors : 0x" << std::setw(8) << result.total_sectors << "\n";
    out << "Grid step    : 0x" << std::setw(8) << result.grid_step << "\n";
    out << std::dec << std::setfill(' ');
    out << "Grid reads   : " << result.grid_reads << "\n"
        << "Reference reads: " << result.reference_reads << "\n"
        << "Unreadable   : " << result.unreadable_reads << "\n"
        << "Nodes        : " << result.nodes.size() << " / " << kMaxNodes << " capacity\n"
        << "Dormant free : " << result.dormant_free_nodes << "\n"
        << "Maps         : " << result.maps.size() << "\n"
        << "Truncated    : " << (result.truncated ? "YES" : "no") << "\n";
    if (result.dormant_free_nodes != 0) {
        out << "Dormant free note: checksum-valid historical __empty headers wholly covered by an active coalesced __empty extent; retained as evidence, excluded from active-map confidence\n";
    }
    out << (result.truncated
                ? "Write planning: LOCKED - scan is incomplete; visible tail is not physical tail\n"
                : "Write planning: complete-scan policy applies\n");
    out << '\n';

    for (std::size_t i = 0; i < result.maps.size(); ++i) {
        const auto& map = result.maps[i];
        const auto plan = build_repair_plan(result, i);
        out << "MAP " << i + 1 << ": " << map_name(map.kind) << "\n"
            << " confidence=" << map.confidence
            << " nodes=" << map.order.size()
            << " reciprocal=" << map.reciprocal_links
            << " inferred=" << map.inferred_links
            << " conflicts=" << map.conflicts
            << " overlaps=" << map.overlaps
            << " repairable=" << (map.repairable ? "yes" : "no") << "\n"
            << " patches=" << plan.patches.size()
            << " corroborated=" << plan.corroborated_count
            << " speculative=" << plan.speculative_count
            << " automatic=" << (plan.automatic_safe ? "yes" : "no")
            << " manual=" << (plan.manual_allowed ? "yes" : "no") << "\n\n";
    }

    out << "DISCOVERED HEADERS\n\n";
    for (std::size_t i = 0; i < result.nodes.size(); ++i) {
        const auto& node = result.nodes[i];
        out << '[' << i << "] LBA=0x" << std::hex << std::setfill('0') << std::setw(8)
            << node.lba << std::dec << std::setfill(' ')
            << " id='" << node.id << "' confidence=" << node.confidence
            << " evidence=0x" << std::hex << std::setfill('0') << std::setw(8)
            << node.evidence << std::dec << std::setfill(' ')
            << ((node.evidence & evidence_dormant_free) ? " DORMANT_FREE" : "") << "\n";
        out << std::hex << std::setfill('0')
            << " start=0x" << std::setw(8) << node.start
            << " length=0x" << std::setw(8) << node.length
            << " prev=0x" << std::setw(8) << node.prev
            << " next=0x" << std::setw(8) << node.next
            << " type=0x" << std::setw(4) << node.type
            << " flags=0x" << std::setw(4) << node.flags << "\n"
            << " main=0x" << std::setw(8) << node.main
            << std::dec << std::setfill(' ')
            << " number=" << node.number << " nsub=" << node.nsub
            << " checksum=" << (node.stored_checksum == node.calculated_checksum ? "OK" : "BAD")
            << "\n\n";
    }
    return out.str();
}

} // namespace ps2hdd::forensic
