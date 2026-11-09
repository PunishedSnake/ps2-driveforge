#include "ps2hdd/pfs.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <unordered_set>

namespace ps2hdd::pfs {

std::optional<ExtentMap> Reader::extent_map(const Node& node)
{
    last_error_.clear();
    ExtentMap result;
    if (!valid()) {
        fail("PFS filesystem is not valid");
        return std::nullopt;
    }
    if (node.inode.number_data == 0) {
        fail("PFS inode has no block descriptors");
        return std::nullopt;
    }

    result.data.reserve(node.inode.number_data > 1 ? node.inode.number_data - 1U : 0U);
    BlockInfo next_segment = node.inode.next_segment;
    std::optional<SegmentDescriptor> indirect;
    std::size_t segi_loaded = 0;
    std::unordered_set<std::uint64_t> seen_segi;

    for (std::size_t global = 1; global < node.inode.number_data; ++global) {
        BlockInfo block{};
        if (global < kInodeMaxBlocks) {
            block = node.inode.data[global];
        } else {
            const std::size_t local = (global - kInodeMaxBlocks) % kIndirectMaxBlocks;
            if (local == 0) {
                if (next_segment.number == 0) {
                    fail("PFS inode ends before the required SEGI descriptor");
                    return std::nullopt;
                }
                const auto key = (static_cast<std::uint64_t>(next_segment.subpart) << 32U) |
                                 next_segment.number;
                if (!seen_segi.insert(key).second) {
                    fail("PFS SEGI chain contains a cycle or duplicate descriptor");
                    return std::nullopt;
                }
                auto loaded = read_segment_descriptor(next_segment);
                if (!loaded) {
                    return std::nullopt;
                }
                result.indirect_descriptors.push_back(next_segment);
                indirect = *loaded;
                next_segment = indirect->next_segment;
                ++segi_loaded;
                continue;
            }
            if (!indirect) {
                fail("PFS indirect data encountered before a SEGI descriptor");
                return std::nullopt;
            }
            block = indirect->data[local];
        }

        if (block.count == 0) {
            fail("PFS contains an empty block descriptor inside the used descriptor range");
            return std::nullopt;
        }
        if (block.subpart > probe_.super.num_subs) {
            fail("PFS data extent points to a missing sub-partition");
            return std::nullopt;
        }
        result.data.push_back(block);
    }

    if (node.inode.number_data > kInodeMaxBlocks && result.indirect_descriptors.empty()) {
        fail("PFS inode references indirect data but no SEGI descriptor was traversed");
        return std::nullopt;
    }
    if (node.inode.number_segdesg != 0 && segi_loaded > node.inode.number_segdesg) {
        fail("PFS SEGI chain is longer than inode metadata reports");
        return std::nullopt;
    }
    return result;
}

} // namespace ps2hdd::pfs
