#pragma once

#include "ps2hdd/block_device.hpp"

#include <cstdint>
#include <string>

namespace ps2hdd {

enum class LegacyPartitionMapKind {
    none,
    legacy_mbr,
    protective_gpt,
    hybrid_gpt,
    gpt_header_without_protective_mbr,
};

struct DiskLayoutGuardResult {
    bool ok{};
    std::string error;
    LegacyPartitionMapKind kind{LegacyPartitionMapKind::none};
    bool mbr_signature{};
    bool protective_entry{};
    bool other_mbr_entries{};
    bool gpt_header{};

    // This answers only the PC partition-map question. A PS2 write path must
    // still require a clean APA scan and its own mutation-specific validation.
    [[nodiscard]] bool allows_ps2_mutation() const noexcept
    {
        return ok && !protective_entry && !gpt_header;
    }
};

// Read the conventional PC MBR/GPT admission sectors without interpreting them
// as PS2 APA data. A protective or hybrid GPT is a hard write refusal even when
// an APA-looking header also exists. This mirrors the bootstrap manager safety
// rule and prevents DriveForge from treating a dual-owned disk as disposable.
[[nodiscard]] DiskLayoutGuardResult inspect_disk_layout(BlockDevice& device);
[[nodiscard]] const char* legacy_partition_map_name(LegacyPartitionMapKind kind) noexcept;

} // namespace ps2hdd
