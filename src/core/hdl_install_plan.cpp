#include "ps2hdd/hdl_install_plan.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace ps2hdd::hdl {
namespace {

[[nodiscard]] char upper_ascii(char c) noexcept
{
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool startup_shape(std::string_view startup) noexcept
{
    if (startup.size() != 11 || (startup[4] != '_' && startup[4] != '-') || startup[8] != '.') {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isalpha(static_cast<unsigned char>(startup[i]))) {
            return false;
        }
    }
    for (const std::size_t i : {5U, 6U, 7U, 9U, 10U}) {
        if (!std::isdigit(static_cast<unsigned char>(startup[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string sanitized_title(std::string_view title)
{
    constexpr std::size_t kTitleInPartitionId = 16;
    const auto count = std::min(title.size(), kTitleInPartitionId);
    std::string out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto c = static_cast<unsigned char>(title[i]);
        if (std::isalnum(c)) {
            out.push_back(upper_ascii(static_cast<char>(c)));
        } else {
            out.push_back('_');
        }
    }
    return out;
}

} // namespace

std::string make_partition_id(const std::string& startup,
                              const std::string& title,
                              bool hidden)
{
    if (!startup_shape(startup) || title.empty()) {
        return {};
    }

    std::string result = hidden ? "__." : "PP.";
    result.reserve(31);
    for (std::size_t i = 0; i < 4; ++i) {
        result.push_back(upper_ascii(startup[i]));
    }
    result.push_back('-');
    result.push_back(startup[5]);
    result.push_back(startup[6]);
    result.push_back(startup[7]);
    result.push_back(startup[9]);
    result.push_back(startup[10]);
    result.append("..");
    result.append(sanitized_title(title));
    return result;
}

InstallPlan plan_install(const apa::ScanResult& disk_scan,
                         std::uint64_t disk_size_bytes,
                         BlockDevice& game_iso,
                         std::string title,
                         bool hidden)
{
    InstallPlan plan;
    plan.title = std::move(title);
    if (plan.title.empty()) {
        plan.error = "HDL install title must not be empty";
        return plan;
    }

    const auto source = iso::inspect_ps2_iso(game_iso);
    if (!source.ok) {
        plan.error = "PS2 ISO preflight failed: " + source.error;
        return plan;
    }
    plan.source = source.game;

    plan.partition_id = make_partition_id(plan.source.startup, plan.title, hidden);
    if (plan.partition_id.empty()) {
        plan.error = "Could not derive a valid HDL APA partition ID";
        return plan;
    }

    plan.allocation = apa::plan_hdl_allocation(
        disk_scan, disk_size_bytes, plan.source.image_bytes);
    if (!plan.allocation.ok) {
        plan.error = "APA allocation preflight failed: " + plan.allocation.error;
        return plan;
    }

    plan.ok = true;
    return plan;
}

} // namespace ps2hdd::hdl
