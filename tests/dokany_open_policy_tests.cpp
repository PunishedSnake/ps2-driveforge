#include "dokany_open_policy.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using ps2hdd::dokany_policy::CreateDisposition;
using ps2hdd::dokany_policy::OpenRequest;
using ps2hdd::dokany_policy::OpenStatus;

void expect(OpenStatus actual, OpenStatus expected, const char* label)
{
    if (actual != expected) {
        std::cerr << "FAILED: " << label << '\n';
        std::exit(1);
    }
}

OpenRequest existing_directory(CreateDisposition disposition)
{
    OpenRequest request{};
    request.exists = true;
    request.is_directory = true;
    request.disposition = static_cast<unsigned>(disposition);
    return request;
}

} // namespace

int main()
{
    // Regression for Darkness' first real Explorer mount: ZwCreateFile receives
    // FILE_OPEN == 1 for a normal existing root-directory open. Win32
    // CREATE_NEW is also numerically 1, but it is a different API contract.
    expect(ps2hdd::dokany_policy::evaluate(existing_directory(CreateDisposition::open)),
           OpenStatus::success,
           "existing root + FILE_OPEN must succeed");

    expect(ps2hdd::dokany_policy::evaluate(existing_directory(CreateDisposition::open_if)),
           OpenStatus::success,
           "existing directory + FILE_OPEN_IF must succeed read-only");

    expect(ps2hdd::dokany_policy::evaluate(existing_directory(CreateDisposition::create)),
           OpenStatus::name_collision,
           "existing directory + FILE_CREATE must collide");

    auto missing = OpenRequest{};
    missing.disposition = static_cast<unsigned>(CreateDisposition::open);
    expect(ps2hdd::dokany_policy::evaluate(missing), OpenStatus::name_not_found,
           "missing object + FILE_OPEN must be not found");

    missing.disposition = static_cast<unsigned>(CreateDisposition::open_if);
    expect(ps2hdd::dokany_policy::evaluate(missing), OpenStatus::write_protected,
           "missing object + FILE_OPEN_IF would create and must be blocked");

    auto overwrite = existing_directory(CreateDisposition::overwrite_if);
    expect(ps2hdd::dokany_policy::evaluate(overwrite), OpenStatus::write_protected,
           "overwrite dispositions must be blocked");

    auto write_access = existing_directory(CreateDisposition::open);
    write_access.wants_write = true;
    expect(ps2hdd::dokany_policy::evaluate(write_access), OpenStatus::write_protected,
           "write access on existing object must be blocked");

    auto delete_on_close = existing_directory(CreateDisposition::open);
    delete_on_close.delete_on_close = true;
    expect(ps2hdd::dokany_policy::evaluate(delete_on_close), OpenStatus::write_protected,
           "delete-on-close must be blocked");

    auto directory_as_file = existing_directory(CreateDisposition::open);
    directory_as_file.non_directory_only = true;
    expect(ps2hdd::dokany_policy::evaluate(directory_as_file), OpenStatus::file_is_directory,
           "directory opened with FILE_NON_DIRECTORY_FILE must fail correctly");

    OpenRequest file_as_directory{};
    file_as_directory.exists = true;
    file_as_directory.is_directory = false;
    file_as_directory.directory_only = true;
    file_as_directory.disposition = static_cast<unsigned>(CreateDisposition::open);
    expect(ps2hdd::dokany_policy::evaluate(file_as_directory), OpenStatus::not_a_directory,
           "file opened with FILE_DIRECTORY_FILE must fail correctly");

    std::cout << "Dokany read-only open policy tests passed.\n";
    return 0;
}
