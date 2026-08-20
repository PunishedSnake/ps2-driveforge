#include "ps2hdd/pfs_export.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    try {
        using ps2hdd::pfs::sanitize_host_filename;
        check(sanitize_host_filename("CFG") == "CFG", "normal filename preserved");
        check(sanitize_host_filename("bad:name?.cfg") == "bad_name_.cfg", "illegal characters replaced");
        check(sanitize_host_filename("CON") == "_CON", "reserved device name escaped");
        check(sanitize_host_filename("LPT1.txt") == "_LPT1.txt", "reserved LPT device name escaped");
        check(sanitize_host_filename("trailing. ") == "trailing__", "trailing dot/space escaped");
        check(sanitize_host_filename("..") == "_", "dot-dot cannot escape destination");
        std::cout << "All host export tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
