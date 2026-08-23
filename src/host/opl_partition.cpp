#include "ps2hdd/opl_partition.hpp"

#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <utility>

namespace ps2hdd::opl {
namespace {

std::string upper_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool is_pfs_main(const apa::Partition& partition) noexcept
{
    return partition.type == apa::kTypePfs && !partition.is_sub();
}

int name_score(std::string_view partition_id, std::vector<std::string>& reasons)
{
    const std::string upper = upper_ascii(std::string(partition_id));
    if (upper == "+OPL") {
        reasons.emplace_back("APA id is exactly +OPL");
        return 100;
    }
    if (upper.find("OPL") != std::string::npos) {
        reasons.emplace_back("APA id contains OPL");
        return 40;
    }
    return 0;
}

int root_score(pfs::Reader& reader, std::vector<std::string>& reasons)
{
    const auto root = reader.root();
    if (!root) {
        reasons.emplace_back("PFS root inode could not be read");
        return 0;
    }

    const auto entries = reader.list_directory(*root);
    const std::set<std::string> strong_dirs{"ART", "CFG", "CHT", "VMC"};
    const std::set<std::string> secondary_dirs{"THM", "APPS", "LNG"};
    int score = 0;
    for (const auto& entry : entries) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = upper_ascii(entry.name);
        if (strong_dirs.contains(name)) {
            score += 12;
            reasons.emplace_back("root contains " + name + "/");
        } else if (secondary_dirs.contains(name)) {
            score += 6;
            reasons.emplace_back("root contains " + name + "/");
        }
    }
    return score;
}

} // namespace

PartitionResolution resolve_data_partition(BlockDevice& device,
                                           const apa::ScanResult& scan,
                                           std::optional<std::string_view> configured_partition)
{
    PartitionResolution result;
    if (!scan.ok()) {
        result.error = "Cannot resolve OPL partition from a non-clean APA scan";
        return result;
    }

    if (configured_partition) {
        for (std::size_t i = 0; i < scan.partitions.size(); ++i) {
            const auto& partition = scan.partitions[i];
            if (partition.id != *configured_partition) {
                continue;
            }
            if (!is_pfs_main(partition)) {
                result.error = "Configured OPL data partition is not a PFS main partition";
                return result;
            }

            PartitionEvidence evidence;
            evidence.partition_id = partition.id;
            evidence.reasons.emplace_back("explicitly configured OPL data partition");
            ApaVolume volume(device, partition);
            pfs::Reader reader(volume);
            evidence.pfs_valid = reader.valid();
            if (!evidence.pfs_valid) {
                evidence.reasons.emplace_back("configured partition failed PFS validation");
                result.candidates.push_back(std::move(evidence));
                result.error = "Configured OPL data partition failed PFS validation";
                return result;
            }
            evidence.score = 1000;
            evidence.reasons.emplace_back("PFS probe is valid");
            result.candidates.push_back(std::move(evidence));
            result.ok = true;
            result.partition_id = partition.id;
            result.partition_index = i;
            return result;
        }
        result.error = "Configured OPL data partition was not found in the APA scan";
        return result;
    }

    struct Ranked {
        std::size_t index{};
        int score{};
    };
    std::vector<Ranked> ranked;

    for (std::size_t i = 0; i < scan.partitions.size(); ++i) {
        const auto& partition = scan.partitions[i];
        if (!is_pfs_main(partition)) {
            continue;
        }

        PartitionEvidence evidence;
        evidence.partition_id = partition.id;
        evidence.score += name_score(partition.id, evidence.reasons);

        ApaVolume volume(device, partition);
        pfs::Reader reader(volume);
        evidence.pfs_valid = reader.valid();
        if (evidence.pfs_valid) {
            evidence.score += 20;
            evidence.reasons.emplace_back("PFS probe is valid");
            evidence.score += root_score(reader, evidence.reasons);
            ranked.push_back({i, evidence.score});
        } else {
            evidence.reasons.emplace_back("PFS probe failed; candidate excluded");
        }
        result.candidates.push_back(std::move(evidence));
    }

    if (ranked.empty()) {
        result.error = "No valid PFS main partition is available for OPL assets";
        return result;
    }

    std::sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.index < right.index;
    });

    if (ranked.size() > 1 && ranked[0].score == ranked[1].score) {
        result.error = "OPL data partition resolution is ambiguous; configure a partition explicitly";
        return result;
    }

    result.ok = true;
    result.partition_index = ranked.front().index;
    result.partition_id = scan.partitions[result.partition_index].id;
    return result;
}

} // namespace ps2hdd::opl
