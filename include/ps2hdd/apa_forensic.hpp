#pragma once

#include "ps2hdd/block_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2hdd::forensic {

inline constexpr std::uint32_t kScanStep = 0x40000U;
inline constexpr std::size_t kMaxNodes = 2048;
inline constexpr std::size_t kMaxMaps = 3;
inline constexpr std::size_t kApaHeaderBytes = 1024;

enum Evidence : std::uint32_t {
    evidence_magic = 1U << 0,
    evidence_self_start = 1U << 1,
    evidence_checksum = 1U << 2,
    evidence_length = 1U << 3,
    evidence_nsub = 1U << 4,
    evidence_type = 1U << 5,
    evidence_id = 1U << 6,
    evidence_grid = 1U << 7,
    evidence_referenced = 1U << 8,
    evidence_dormant_free = 1U << 9,
};

enum class MapKind : std::uint32_t {
    forward = 1,
    reverse = 2,
    geometry = 3,
};

struct Node {
    std::uint32_t lba{};
    std::uint32_t stored_checksum{};
    std::uint32_t calculated_checksum{};
    std::uint32_t next{};
    std::uint32_t prev{};
    std::uint32_t start{};
    std::uint32_t length{};
    std::uint32_t main{};
    std::uint32_t nsub{};
    std::uint32_t number{};
    std::uint16_t type{};
    std::uint16_t flags{};
    std::uint32_t evidence{};
    unsigned confidence{};
    std::string id;
    std::array<std::byte, kApaHeaderBytes> header{};
};

struct Map {
    MapKind kind{MapKind::forward};
    std::vector<std::uint16_t> order;
    unsigned reciprocal_links{};
    unsigned inferred_links{};
    unsigned conflicts{};
    unsigned overlaps{};
    unsigned confidence{};
    bool repairable{};
};

struct ScanResult {
    bool ok{};
    std::string error;
    std::uint32_t total_sectors{};
    std::uint32_t grid_step{kScanStep};
    unsigned grid_reads{};
    unsigned reference_reads{};
    unsigned unreadable_reads{};
    unsigned dormant_free_nodes{};
    bool truncated{};
    std::vector<Node> nodes;
    std::vector<Map> maps;
};

struct Patch {
    std::uint16_t node_index{};
    std::uint32_t lba{};
    std::uint32_t old_next{};
    std::uint32_t old_prev{};
    std::uint32_t new_next{};
    std::uint32_t new_prev{};
    bool checksum_corroborated{};

    [[nodiscard]] unsigned bit_distance() const noexcept;
};

struct RepairPlan {
    std::size_t map_index{};
    std::vector<Patch> patches;
    unsigned corroborated_count{};
    unsigned speculative_count{};
    unsigned confidence{};
    bool automatic_safe{};
    bool manual_allowed{};
};

using ProgressCallback = std::function<void(std::uint32_t lba,
                                             std::uint32_t total_sectors,
                                             std::size_t nodes_found)>;

// Read-only raw reconstruction. Normal apa::Reader admission is deliberately
// not required; this is the degraded path for damaged headers/topology.
[[nodiscard]] ScanResult scan_apa(BlockDevice& device, ProgressCallback progress = {});

[[nodiscard]] RepairPlan build_repair_plan(const ScanResult& result,
                                           std::size_t map_index);

// Only prev/next/checksum are changed. The patch must still match the original
// scanned node byte-for-byte relationship before a caller may write it.
[[nodiscard]] bool build_patched_header(const ScanResult& result,
                                        const Patch& patch,
                                        std::array<std::byte, kApaHeaderBytes>& repaired,
                                        std::string& error) noexcept;

[[nodiscard]] const char* map_name(MapKind kind) noexcept;

// Canonical textual schema used by fhdb-bootstrap-manager FORENSIC.TXT.
[[nodiscard]] std::string render_report(const ScanResult& result);

} // namespace ps2hdd::forensic
