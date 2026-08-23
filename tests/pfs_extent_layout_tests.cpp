#include "ps2hdd/pfs_extent_layout.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ps2hdd::pfs::Inode base_inode()
{
    ps2hdd::pfs::Inode inode{};
    inode.magic = ps2hdd::pfs::kSegdMagic;
    inode.inode_block = {100, 0, 1};
    inode.last_segment = inode.inode_block;
    inode.data[0] = inode.inode_block;
    inode.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeRegular | 0x01B6U);
    inode.size = 1234;
    inode.number_blocks = 1;
    inode.number_data = 1;
    inode.number_segdesg = 1;
    return inode;
}

std::vector<ps2hdd::pfs::BlockInfo> extents(std::size_t count)
{
    std::vector<ps2hdd::pfs::BlockInfo> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back({static_cast<std::uint32_t>(1000 + index * 2U), 0, 1});
    }
    return result;
}

void direct_layout_needs_no_segi()
{
    auto base = base_inode();
    const auto data = extents(113);
    const auto layout = ps2hdd::pfs::build_file_extent_layout(base, data, {});
    check(layout.ok, "113 direct extents serialize");
    check(layout.segments.empty(), "113 direct extents need no SEGI");
    check(layout.inode.number_data == 114, "direct number_data includes inode self descriptor");
    check(layout.inode.next_segment.number == 0 && layout.inode.last_segment.number == 100,
          "direct layout keeps segment links on base inode");
    check(layout.inode.number_blocks == 114, "direct number_blocks counts inode plus payload zones");
    check(ps2hdd::pfs::inode_checksum(layout.inode) == layout.inode.checksum,
          "direct layout checksum verifies");
}

void one_segi_matches_reader_indexing_contract()
{
    auto base = base_inode();
    const auto data = extents(114);
    const std::vector<ps2hdd::pfs::BlockInfo> segi{{500, 0, 1}};
    const auto layout = ps2hdd::pfs::build_file_extent_layout(base, data, segi);
    check(layout.ok && layout.segments.size() == 1, "114 extents require one SEGI");
    check(layout.inode.number_data == 116,
          "number_data includes inode self, 114 payload descriptors and SEGI self slot");
    check(layout.inode.data[113].number == data[112].number,
          "last direct data extent occupies inode data[113]");
    const auto& descriptor = layout.segments.front().descriptor;
    check(descriptor.data[0].number == 500, "SEGI data[0] describes its metadata location");
    check(descriptor.data[1].number == data[113].number,
          "first indirect payload extent occupies SEGI data[1]");
    check(descriptor.last_segment.number == base.inode_block.number,
          "first SEGI points back to base SEGD inode");
    check(descriptor.next_segment.number == 0, "single SEGI terminates chain");
    check(layout.inode.next_segment.number == 500 && layout.inode.last_segment.number == 500,
          "base inode identifies first and last SEGI");
    check(ps2hdd::pfs::segment_checksum(descriptor) == descriptor.checksum,
          "SEGI checksum verifies");
}

void multiple_segi_are_chained_and_bounded()
{
    auto base = base_inode();
    const auto data = extents(113 + 122 + 5);
    const std::vector<ps2hdd::pfs::BlockInfo> segi{{500, 0, 1}, {700, 0, 1}};
    const auto layout = ps2hdd::pfs::build_file_extent_layout(base, data, segi);
    check(layout.ok && layout.segments.size() == 2, "large extent graph uses two SEGI records");
    check(layout.segments[0].descriptor.next_segment.number == 700,
          "first SEGI points to second");
    check(layout.segments[1].descriptor.last_segment.number == 500,
          "second SEGI points back to first");
    check(layout.segments[1].descriptor.data[5].number == data.back().number,
          "tail SEGI carries remaining data descriptors");
    check(layout.inode.last_segment.number == 700, "inode tracks final SEGI");
    check(layout.inode.number_segdesg == 2, "inode records traversable SEGI count");
}

void mismatched_segi_count_is_refused()
{
    auto base = base_inode();
    const auto data = extents(114);
    const auto layout = ps2hdd::pfs::build_file_extent_layout(base, data, {});
    check(!layout.ok, "serializer refuses missing SEGI location");
}

} // namespace

int main()
{
    try {
        direct_layout_needs_no_segi();
        one_segi_matches_reader_indexing_contract();
        multiple_segi_are_chained_and_bounded();
        mismatched_segi_count_is_refused();
        std::cout << "PFS extent layout tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PFS extent layout test failure: " << error.what() << '\n';
        return 1;
    }
}
