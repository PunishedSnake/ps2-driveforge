#include "ps2hdd/apa_remove.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_set>

namespace ps2hdd::apa {
namespace {

bool read_header(WritableBlockDevice& device, std::uint32_t lba, Header& header)
{
    const auto offset = static_cast<std::uint64_t>(lba) * kSectorSize;
    if (offset > device.size_bytes() || sizeof(header) > device.size_bytes() - offset) {
        return false;
    }
    return device.read(offset, std::as_writable_bytes(std::span{&header, 1}));
}

bool canonical_chain(const ScanResult& scan)
{
    if (!scan.ok() || scan.partitions.empty() || scan.partitions.front().start_lba != 0 ||
        scan.partitions.front().type != kTypeMbr) {
        return false;
    }
    const auto last_lba = scan.partitions.size() > 1 ? scan.partitions.back().start_lba : 0U;
    if (scan.partitions.front().prev_lba != last_lba) {
        return false;
    }
    for (std::size_t i = 0; i < scan.partitions.size(); ++i) {
        const auto expected_next = i + 1 < scan.partitions.size()
                                       ? scan.partitions[i + 1].start_lba
                                       : 0U;
        if (scan.partitions[i].next_lba != expected_next) {
            return false;
        }
        if (i != 0 && scan.partitions[i].prev_lba != scan.partitions[i - 1].start_lba) {
            return false;
        }
    }
    return true;
}

} // namespace

RemovePlan plan_remove_main_partition(const ScanResult& scan, std::uint32_t main_lba)
{
    RemovePlan plan;
    plan.main_lba = main_lba;
    if (!canonical_chain(scan)) {
        plan.error = "APA chain must be clean and canonical before removal";
        return plan;
    }
    if (main_lba == 0) {
        plan.error = "APA MBR cannot be removed";
        return plan;
    }

    const auto target = std::find_if(scan.partitions.begin(), scan.partitions.end(),
                                     [main_lba](const Partition& p) {
                                         return p.start_lba == main_lba;
                                     });
    if (target == scan.partitions.end()) {
        plan.error = "APA main partition was not found";
        return plan;
    }
    if (target->is_sub()) {
        plan.error = "APA removal must target a main partition, not a sub partition";
        return plan;
    }
    if (target->type == kTypeMbr) {
        plan.error = "APA MBR cannot be removed";
        return plan;
    }

    plan.partition_id = target->id;
    std::unordered_set<std::uint32_t> removed;
    removed.insert(target->start_lba);
    plan.removed_lbas.push_back(target->start_lba);
    plan.freed_bytes += static_cast<std::uint64_t>(target->length_sectors) * kSectorSize;

    for (const auto& sub : target->sub_partitions) {
        if (!removed.insert(sub.start).second) {
            plan.error = "APA main header contains a duplicate sub-partition extent";
            return plan;
        }
        const auto header = std::find_if(scan.partitions.begin(), scan.partitions.end(),
                                         [&](const Partition& p) {
                                             return p.start_lba == sub.start;
                                         });
        if (header == scan.partitions.end() || !header->is_sub() ||
            header->main_lba != target->start_lba || header->length_sectors != sub.length) {
            plan.error = "APA sub-partition header set does not match its main header";
            return plan;
        }
        plan.removed_lbas.push_back(sub.start);
        plan.freed_bytes += static_cast<std::uint64_t>(sub.length) * kSectorSize;
    }

    for (const auto& p : scan.partitions) {
        if (p.is_sub() && p.main_lba == target->start_lba && !removed.contains(p.start_lba)) {
            plan.error = "APA chain contains an unlisted sub partition for the removal target";
            return plan;
        }
    }

    std::vector<const Partition*> survivors;
    survivors.reserve(scan.partitions.size() - removed.size());
    for (const auto& p : scan.partitions) {
        if (!removed.contains(p.start_lba)) {
            survivors.push_back(&p);
        }
    }
    if (survivors.empty() || survivors.front()->start_lba != 0) {
        plan.error = "APA removal would detach the MBR";
        return plan;
    }

    const auto new_last = survivors.size() > 1 ? survivors.back()->start_lba : 0U;
    for (std::size_t i = 0; i < survivors.size(); ++i) {
        const auto& p = *survivors[i];
        const auto new_prev = i == 0 ? new_last : survivors[i - 1]->start_lba;
        const auto new_next = i + 1 < survivors.size() ? survivors[i + 1]->start_lba : 0U;
        if (p.prev_lba != new_prev || p.next_lba != new_next) {
            plan.link_rewrites.push_back({p.start_lba, p.prev_lba, p.next_lba,
                                          new_prev, new_next});
        }
    }

    plan.ok = true;
    return plan;
}

bool apply_remove_plan_to_scan(ScanResult& scan, const RemovePlan& plan)
{
    if (!plan.ok || plan.removed_lbas.empty() || !canonical_chain(scan)) {
        return false;
    }

    const std::unordered_set<std::uint32_t> removed(plan.removed_lbas.begin(),
                                                    plan.removed_lbas.end());
    if (removed.size() != plan.removed_lbas.size()) {
        return false;
    }
    for (const auto lba : removed) {
        if (std::none_of(scan.partitions.begin(), scan.partitions.end(),
                         [lba](const Partition& partition) {
                             return partition.start_lba == lba;
                         })) {
            return false;
        }
    }

    for (const auto& rewrite : plan.link_rewrites) {
        auto partition = std::find_if(scan.partitions.begin(), scan.partitions.end(),
                                      [&](const Partition& candidate) {
                                          return candidate.start_lba == rewrite.header_lba;
                                      });
        if (partition == scan.partitions.end() || removed.contains(rewrite.header_lba) ||
            partition->prev_lba != rewrite.old_prev_lba ||
            partition->next_lba != rewrite.old_next_lba) {
            return false;
        }
        partition->prev_lba = rewrite.new_prev_lba;
        partition->next_lba = rewrite.new_next_lba;
    }

    const auto old_size = scan.partitions.size();
    scan.partitions.erase(
        std::remove_if(scan.partitions.begin(), scan.partitions.end(),
                       [&](const Partition& partition) {
                           return removed.contains(partition.start_lba);
                       }),
        scan.partitions.end());
    if (scan.partitions.size() + removed.size() != old_size) {
        return false;
    }
    return canonical_chain(scan);
}

std::string stage_remove_main_partition(WritableBlockDevice& device,
                                        const RemovePlan& plan,
                                        WriteTransaction& transaction)
{
    if (!plan.ok || plan.removed_lbas.empty()) {
        return "Cannot stage APA removal from an invalid plan";
    }
    for (const auto& rewrite : plan.link_rewrites) {
        Header header{};
        if (!read_header(device, rewrite.header_lba, header)) {
            return "Could not read APA neighbour before removal";
        }
        if (header.magic != kMagic || header.start != rewrite.header_lba ||
            checksum(header) != header.checksum) {
            return "APA neighbour failed validation before removal";
        }
        if (header.prev != rewrite.old_prev_lba || header.next != rewrite.old_next_lba) {
            return "APA chain changed after removal planning; refusing stale plan";
        }

        header.prev = rewrite.new_prev_lba;
        header.next = rewrite.new_next_lba;
        header.checksum = checksum(header);
        const auto offset = static_cast<std::uint64_t>(rewrite.header_lba) * kSectorSize;
        if (!transaction.stage(offset, std::as_bytes(std::span{&header, 1}),
                               "unlink APA partition")) {
            return "Could not stage APA removal link update: " + transaction.stage_error();
        }
    }
    return {};
}

RemoveResult remove_main_partition_from_image(WritableBlockDevice& device,
                                              std::uint32_t main_lba)
{
    RemoveResult result;
    Reader reader(device);
    const auto before = reader.scan();
    const auto plan = plan_remove_main_partition(before, main_lba);
    if (!plan.ok) {
        result.error = plan.error;
        return result;
    }

    WriteTransaction transaction(device);
    if (const auto error = stage_remove_main_partition(device, plan, transaction); !error.empty()) {
        result.error = error;
        return result;
    }

    const auto committed = transaction.commit([&]() -> std::string {
        Reader verify_reader(device);
        const auto after = verify_reader.scan();
        if (!after.ok()) {
            return "APA rescan failed after removal";
        }
        for (const auto lba : plan.removed_lbas) {
            const auto still_visible = std::any_of(after.partitions.begin(), after.partitions.end(),
                                                   [lba](const Partition& p) {
                                                       return p.start_lba == lba;
                                                   });
            if (still_visible) {
                return "Removed APA header is still reachable after unlink";
            }
        }
        if (after.partitions.size() + plan.removed_lbas.size() != before.partitions.size()) {
            return "APA removal changed an unexpected number of reachable headers";
        }
        return {};
    });
    if (!committed.ok) {
        result.error = "APA removal transaction failed: " + committed.error;
        return result;
    }

    result.ok = true;
    result.partition_id = plan.partition_id;
    result.removed_headers = plan.removed_lbas.size();
    result.rewritten_headers = plan.link_rewrites.size();
    result.freed_bytes = plan.freed_bytes;
    return result;
}

} // namespace ps2hdd::apa
