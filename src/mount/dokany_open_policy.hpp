#pragma once

#include <cstdint>

namespace ps2hdd::dokany_policy {

// ZwCreateFile receives the NT kernel FILE_* create-disposition values, not the
// Win32 CREATE_NEW/OPEN_EXISTING constants used by CreateFileW. Several numeric
// values overlap while meaning different things (notably FILE_OPEN == 1 while
// CREATE_NEW == 1), so keeping this enum local and explicit prevents a very
// easy-to-miss class of mount failures.
enum class CreateDisposition : std::uint32_t {
    supersede = 0,   // FILE_SUPERSEDE
    open = 1,        // FILE_OPEN
    create = 2,      // FILE_CREATE
    open_if = 3,     // FILE_OPEN_IF
    overwrite = 4,   // FILE_OVERWRITE
    overwrite_if = 5 // FILE_OVERWRITE_IF
};

enum class OpenStatus {
    success,
    name_not_found,
    name_collision,
    write_protected,
    file_is_directory,
    not_a_directory,
    invalid_disposition,
};

struct OpenRequest {
    bool exists{};
    bool is_directory{};
    bool wants_write{};
    bool directory_only{};
    bool non_directory_only{};
    bool delete_on_close{};
    std::uint32_t disposition{};
};

constexpr OpenStatus evaluate(const OpenRequest& request)
{
    if (request.disposition > static_cast<std::uint32_t>(CreateDisposition::overwrite_if)) {
        return OpenStatus::invalid_disposition;
    }

    const auto disposition = static_cast<CreateDisposition>(request.disposition);

    if (!request.exists) {
        // FILE_OPEN and FILE_OVERWRITE require an existing object. Every other
        // missing-object disposition would create/supersede an object, which a
        // structurally read-only DriveForge mount must never attempt.
        if (disposition == CreateDisposition::open ||
            disposition == CreateDisposition::overwrite) {
            return OpenStatus::name_not_found;
        }
        return OpenStatus::write_protected;
    }

    if (request.non_directory_only && request.is_directory) {
        return OpenStatus::file_is_directory;
    }
    if (request.directory_only && !request.is_directory) {
        return OpenStatus::not_a_directory;
    }

    if (disposition == CreateDisposition::create) {
        return OpenStatus::name_collision;
    }

    if (request.wants_write || request.delete_on_close ||
        disposition == CreateDisposition::supersede ||
        disposition == CreateDisposition::overwrite ||
        disposition == CreateDisposition::overwrite_if) {
        return OpenStatus::write_protected;
    }

    // Existing object + FILE_OPEN or FILE_OPEN_IF is a normal read-only open.
    return OpenStatus::success;
}

} // namespace ps2hdd::dokany_policy
