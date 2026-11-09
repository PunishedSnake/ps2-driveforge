#include "ps2hdd/apa_mutation.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace ps2hdd::apa {
namespace {

[[nodiscard]] bool read_header(WritableBlockDevice& device, std::uint32_t lba, Header& header)
{
    const auto offset = static_cast<std::uint64_t>(lba) * kSectorSize;
    if (offset > device.size_bytes() || sizeof(header) > device.size_bytes() - offset) {
        return false;
    }
    return device.read(offset, std::as_writable_bytes(std::span{&header, 1}));
}

} // namespace

std::string stage_hdl_header_publication(WritableBlockDevice& device,
                                         const AllocationPlan& allocation,
                                         const HdlHeaderPlan& new_headers,
                                         WriteTransaction& transaction)
{
    if (!allocation.ok) {
        return "Cannot stage APA publication from an invalid allocation plan";
    }
    if (!new_headers.ok || new_headers.headers.size() != allocation.extents.size()) {
        return "Serialized HDL header set does not match the allocation plan";
    }

    // Stage every currently-unreachable new header first. A later commit still
    // writes through WriteTransaction, but this ordering means an ordinary
    // write failure cannot link to a header that was never attempted.
    for (std::size_t i = 0; i < new_headers.headers.size(); ++i) {
        const auto& header = new_headers.headers[i];
        const auto& extent = allocation.extents[i];
        if (header.start != extent.start_lba || header.prev != extent.prev_lba ||
            header.next != extent.next_lba || header.magic != kMagic ||
            checksum(header) != header.checksum) {
            return "Serialized HDL header no longer matches the approved allocation plan";
        }
        const auto offset = static_cast<std::uint64_t>(header.start) * kSectorSize;
        const auto bytes = std::as_bytes(std::span{&header, 1});
        const std::string label = i == 0 ? "new HDL main APA header" : "new HDL sub APA header";
        if (!transaction.stage(offset, bytes, label)) {
            return "Could not stage new APA header: " + transaction.stage_error();
        }
    }

    // Existing-neighbour rewrites are accepted only if the disk still contains
    // exactly the links observed by the allocation planner. A rescan/replan is
    // required if anything changed after planning.
    for (const auto& update : allocation.existing_link_updates) {
        Header current{};
        if (!read_header(device, update.header_lba, current)) {
            return "Could not read an existing APA neighbour header before publication";
        }
        if (current.magic != kMagic || current.start != update.header_lba ||
            checksum(current) != current.checksum) {
            return "Existing APA neighbour header failed validation before publication";
        }
        if (current.prev != update.old_prev_lba || current.next != update.old_next_lba) {
            return "APA neighbour links changed after planning; refusing stale publication plan";
        }

        current.prev = update.new_prev_lba;
        current.next = update.new_next_lba;
        current.checksum = checksum(current);
        const auto offset = static_cast<std::uint64_t>(update.header_lba) * kSectorSize;
        if (!transaction.stage(offset, std::as_bytes(std::span{&current, 1}),
                               "existing APA link update")) {
            return "Could not stage existing APA link update: " + transaction.stage_error();
        }
    }

    return {};
}

} // namespace ps2hdd::apa
