#include "ps2hdd/forensic_preview.hpp"

#include "ps2hdd/apa.hpp"

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>

namespace ps2hdd::forensic {
namespace {

std::string hex(std::uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string human(std::uint64_t bytes)
{
    std::ostringstream out;
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gib >= 1.0) out << std::fixed << std::setprecision(2) << gib << " GiB";
    else out << std::fixed << std::setprecision(0) << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB";
    return out.str();
}

PreviewRow row_for(const Node& node)
{
    PreviewRow row;
    row.lba = node.lba;
    row.id = node.id.empty() ? std::string("(no id)") : node.id;
    row.type = apa::type_name(node.type);
    row.bytes = std::uint64_t{node.length} * apa::kSectorSize;
    row.sub = (node.flags & apa::kFlagSub) != 0;
    row.confidence = node.confidence;
    return row;
}

} // namespace

MapPreview preview(const ScanResult& scan, std::size_t map_index)
{
    MapPreview result;
    result.map_index = map_index;
    if (map_index >= scan.maps.size()) return result;
    const auto& map = scan.maps[map_index];
    const auto plan = build_repair_plan(scan, map_index);
    result.kind = map_name(map.kind);
    result.confidence = plan.confidence;
    result.safety = plan.automatic_safe ? "automatic-safe" : plan.manual_allowed ? "manual-only" : "blocked";
    result.patches = plan.patches.size();

    std::set<std::uint16_t> in_map(map.order.begin(), map.order.end());
    for (const auto index : map.order) {
        if (index >= scan.nodes.size()) continue;
        const auto& node = scan.nodes[index];
        auto row = row_for(node);
        const auto patch = std::find_if(plan.patches.begin(), plan.patches.end(),
                                        [index](const Patch& p) { return p.node_index == index; });
        if (patch != plan.patches.end()) {
            if (patch->old_next != patch->new_next) row.change += "next " + hex(patch->old_next) + " -> " + hex(patch->new_next);
            if (patch->old_prev != patch->new_prev) {
                if (!row.change.empty()) row.change += ", ";
                row.change += "prev " + hex(patch->old_prev) + " -> " + hex(patch->new_prev);
            }
            if (!patch->checksum_corroborated) row.change += " (checksum not corroborated)";
        }
        if (!row.sub) {
            if (node.type == apa::kTypePfs) ++result.pfs_partitions;
            if (node.type == apa::kTypeHdl) ++result.hdl_games;
        }
        result.mapped_bytes += row.bytes;
        result.rows.push_back(std::move(row));
    }
    for (std::size_t i = 0; i < scan.nodes.size(); ++i) {
        if (in_map.contains(static_cast<std::uint16_t>(i))) continue;
        const auto& node = scan.nodes[i];
        if ((node.evidence & evidence_dormant_free) != 0) continue;  // free-space leftovers are expected
        result.orphaned.push_back(row_for(node));
    }
    return result;
}

std::string render_preview(const MapPreview& preview)
{
    std::ostringstream out;
    out << "Map " << preview.map_index << " (" << preview.kind << "): " << preview.safety << ", confidence "
        << preview.confidence << ", " << preview.patches << " header patch(es)\n"
        << "  " << preview.rows.size() << " partition header(s), " << preview.pfs_partitions << " PFS, "
        << preview.hdl_games << " HDL game(s), " << human(preview.mapped_bytes) << " mapped\n";
    for (const auto& row : preview.rows) {
        out << "  " << std::left << std::setw(12) << ("0x" + [&] { std::ostringstream h; h << std::hex << row.lba; return h.str(); }())
            << std::setw(6) << (row.sub ? "sub" : "main") << std::setw(10) << row.type << std::setw(11) << human(row.bytes)
            << row.id;
        if (!row.change.empty()) out << "   [" << row.change << ']';
        out << '\n';
    }
    if (!preview.orphaned.empty()) {
        out << "  Left unreachable by this map (" << preview.orphaned.size() << "):\n";
        for (const auto& row : preview.orphaned)
            out << "    0x" << std::hex << row.lba << std::dec << "  " << row.type << "  " << human(row.bytes) << "  " << row.id << '\n';
    }
    return out.str();
}

} // namespace ps2hdd::forensic
