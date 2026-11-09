#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <limits>

namespace ps2hdd {
namespace {

[[nodiscard]] bool range_fits(std::uint64_t offset, std::uint64_t bytes,
                              std::uint64_t size) noexcept
{
    return offset <= size && bytes <= size - offset;
}

} // namespace

bool WriteTransaction::ranges_overlap(std::uint64_t offset, std::uint64_t bytes) const noexcept
{
    const std::uint64_t end = offset + bytes;
    for (const auto& write : writes_) {
        const std::uint64_t existing_end = write.offset + write.after.size();
        if (offset < existing_end && write.offset < end) {
            return true;
        }
    }
    return false;
}

bool WriteTransaction::stage(std::uint64_t offset,
                             std::span<const std::byte> replacement,
                             std::string_view label)
{
    stage_error_.clear();
    if (commit_started_) {
        stage_error_ = "Cannot stage more writes after transaction commit has started";
        return false;
    }
    if (replacement.empty()) {
        stage_error_ = "Mutation range is empty";
        return false;
    }
    if (offset % kMutationSectorSize != 0 || replacement.size() % kMutationSectorSize != 0) {
        stage_error_ = "Mutation range must be aligned to complete 512-byte sectors";
        return false;
    }
    if (!range_fits(offset, replacement.size(), device_.size_bytes())) {
        stage_error_ = "Mutation range extends outside the backing device";
        return false;
    }
    if (replacement.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
        stage_error_ = "Mutation range overflows the host address space";
        return false;
    }
    if (ranges_overlap(offset, replacement.size())) {
        stage_error_ = "Mutation range overlaps an already staged write";
        return false;
    }

    StagedWrite write;
    write.offset = offset;
    write.before.resize(replacement.size());
    write.after.assign(replacement.begin(), replacement.end());
    write.label.assign(label.begin(), label.end());
    if (!device_.read(offset, write.before)) {
        stage_error_ = "Could not capture mutation before-image";
        return false;
    }

    writes_.push_back(std::move(write));
    return true;
}

bool WriteTransaction::rollback() noexcept
{
    bool write_ok = true;
    for (auto it = writes_.rbegin(); it != writes_.rend(); ++it) {
        if (!device_.write(it->offset, it->before)) {
            write_ok = false;
        }
    }
    const bool flush_ok = device_.flush();

    bool verify_ok = true;
    for (const auto& write : writes_) {
        std::vector<std::byte> readback(write.before.size());
        if (!device_.read(write.offset, readback) || readback != write.before) {
            verify_ok = false;
        }
    }
    return write_ok && flush_ok && verify_ok;
}

WriteTransactionResult WriteTransaction::commit(Verifier verifier)
{
    WriteTransactionResult result;
    result.staged_ranges = writes_.size();

    if (commit_started_) {
        result.error = "Write transaction commit was already attempted";
        return result;
    }
    commit_started_ = true;

    if (writes_.empty()) {
        result.error = "Write transaction has no staged ranges";
        return result;
    }

    auto fail = [&](std::string error) {
        result.error = std::move(error);
        result.rollback_attempted = true;
        result.rollback_ok = rollback();
        result.error += result.rollback_ok
                            ? "; all staged before-images restored"
                            : "; WARNING: automatic transaction rollback failed";
        return result;
    };

    for (const auto& write : writes_) {
        ++result.writes_started;
        if (!device_.write(write.offset, write.after)) {
            std::string error = "Could not write staged mutation range";
            if (!write.label.empty()) {
                error += " (" + write.label + ")";
            }
            return fail(std::move(error));
        }
    }

    if (!device_.flush()) {
        return fail("Could not flush staged mutation ranges");
    }

    for (const auto& write : writes_) {
        std::vector<std::byte> readback(write.after.size());
        if (!device_.read(write.offset, readback) || readback != write.after) {
            std::string error = "Mutation read-back verification failed";
            if (!write.label.empty()) {
                error += " (" + write.label + ")";
            }
            return fail(std::move(error));
        }
    }

    if (verifier) {
        const std::string verification_error = verifier();
        if (!verification_error.empty()) {
            return fail("Post-write parser verification failed: " + verification_error);
        }
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd
