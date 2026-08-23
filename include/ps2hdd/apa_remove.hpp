#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/writable_block_device.hpp"
#include "ps2hdd/write_transaction.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ps2hdd::apa {

struct RemoveLinkRewrite {
    std::uint32_t header_lba{};
    std::uint32_t old_prev_lba{};
    std::uint32_t old_next_lba{};
    std::uint32_t new_prev_lba{};
    std::uint32_t new_next_lba{};
};

struct RemovePlan {
    bool ok{};
    std::string error;
    std::uint32_t main_lba{};
    std::string partition_id;
    std::vector<std::uint32_t> removed_lbas;
    std::vector<RemoveLinkRewrite> link_rewrites;
    std::uint64_t freed_bytes{};
};

struct RemoveResult {
    bool ok{};
    std::string error;
    std::string partition_id;
    std::size_t removed_headers{};
    std::size_t rewritten_headers{};
    std::uint64_t freed_bytes{};
};

// Pure metadata planner. Removing an APA partition does not require rebuilding
// an HDL catalog or touching the partition payload. The planner unlinks the main
// header plus all of its sub headers and rewrites only surviving chain links.
[[nodiscard]] RemovePlan plan_remove_main_partition(const ScanResult& scan,
                                                    std::uint32_t main_lba);

// Stages only surviving-header link rewrites. Removed headers are intentionally
// left untouched and become unreachable free space; a future allocation may
// overwrite them. This avoids turning deletion into a giant zero-fill operation.
[[nodiscard]] std::string stage_remove_main_partition(WritableBlockDevice& device,
                                                      const RemovePlan& plan,
                                                      WriteTransaction& transaction);

// Image mutation convenience wrapper with rescan verification and rollback.
[[nodiscard]] RemoveResult remove_main_partition_from_image(WritableBlockDevice& device,
                                                            std::uint32_t main_lba);

} // namespace ps2hdd::apa
