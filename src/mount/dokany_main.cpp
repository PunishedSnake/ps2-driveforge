#include "ps2hdd/dokany_mount.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/version.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

ps2hdd::DokanyMountController* g_controller = nullptr;

std::wstring utf8_to_wide(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (chars <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), chars);
    return result;
}

std::wstring normalize_cli_mount_point(std::wstring value)
{
    // RC6's WinUI helper command line quoted a root path as "C:\\". Windows
    // CRT parsing treats the final backslash as escaping the closing quote,
    // yielding C:" as argv. Accept that exact legacy form here so RC7 mounts
    // are robust, while the canonical normalized value remains C:\\.
    if (value.size() == 3 && value[1] == L':' && value[2] == L'"') {
        value.resize(2);
    }
    return ps2hdd::DokanyMountController::normalize_mount_point(std::move(value));
}

BOOL WINAPI console_handler(DWORD control_type)
{
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT || control_type == CTRL_SHUTDOWN_EVENT) {
        if (g_controller) {
            (void)g_controller->request_unmount();
        }
        return TRUE;
    }
    return FALSE;
}

void usage()
{
    std::wcout << L"PS2 DriveForge " << utf8_to_wide(ps2hdd::version::string)
               << L"-dev \"" << utf8_to_wide(ps2hdd::version::codename)
               << L"\" - read-only Dokany mount\n\n"
                  L"Usage:\n"
                  L"  PS2-DriveForge-Mount.exe --image <disk.img> --mount <P:> [--debug]\n"
                  L"  PS2-DriveForge-Mount.exe --physical <index> --mount <P:> [--debug]\n"
                  L"  PS2-DriveForge-Mount.exe --unmount <P:>\n\n"
                  L"The CLI remains a diagnostic/script frontend and packaged mount helper.\n"
                  L"Source devices are always opened read-only and mounted mutations are rejected.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::optional<std::filesystem::path> image;
    std::optional<unsigned> physical;
    std::optional<std::wstring> mount_point;
    std::optional<std::wstring> unmount_point;
    bool debug = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--image" && i + 1 < argc) {
            image = std::filesystem::path(argv[++i]);
        } else if (arg == L"--physical" && i + 1 < argc) {
            try {
                physical = static_cast<unsigned>(std::stoul(argv[++i]));
            } catch (...) {
                std::wcerr << L"Invalid PhysicalDrive index.\n";
                return 2;
            }
        } else if (arg == L"--mount" && i + 1 < argc) {
            mount_point = normalize_cli_mount_point(argv[++i]);
        } else if (arg == L"--unmount" && i + 1 < argc) {
            unmount_point = normalize_cli_mount_point(argv[++i]);
        } else if (arg == L"--debug") {
            debug = true;
        } else if (arg == L"--help" || arg == L"-h") {
            usage();
            return 0;
        } else {
            std::wcerr << L"Unknown or incomplete option: " << arg << L"\n";
            usage();
            return 2;
        }
    }

    if (unmount_point) {
        if (!ps2hdd::remove_dokany_mount_point(*unmount_point)) {
            std::wcerr << L"Could not unmount " << *unmount_point << L"\n";
            return 1;
        }
        std::wcout << L"Unmount requested for " << *unmount_point << L"\n";
        return 0;
    }

    if (!mount_point || mount_point->empty() || (image.has_value() == physical.has_value())) {
        usage();
        return 2;
    }

    std::unique_ptr<ps2hdd::BlockDevice> source;
    if (image) {
        auto device = std::make_unique<ps2hdd::FileBlockDevice>(*image);
        if (!device->is_open()) {
            std::wcerr << L"Could not open disk image.\n";
            return 1;
        }
        source = std::move(device);
    } else {
        auto device = std::make_unique<ps2hdd::PhysicalDrive>(*physical);
        if (!device->is_open()) {
            std::wcerr << L"Could not open PhysicalDrive" << *physical
                       << L" read-only (Win32 error " << device->open_error() << L").\n";
            return 1;
        }
        source = std::move(device);
    }

    ps2hdd::DokanyMountController controller;
    if (!controller.start(std::move(source), *mount_point, debug)) {
        std::cerr << "Mount startup failed: " << controller.last_error() << '\n';
        return 1;
    }

    g_controller = &controller;
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::wcout << L"Mounting PS2 DriveForge read-only at " << *mount_point << L"...\n"
               << L"Namespace root: " << *mount_point << L"Partitions\\\n"
               << L"Press Ctrl+C or run --unmount " << *mount_point
               << L" from another terminal.\n";

    for (int attempt = 0; attempt < 200 && controller.is_running() && !controller.is_mounted(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (controller.is_mounted()) {
        std::wcout << L"Mounted PS2 DriveForge read-only at " << *mount_point << L"\n";
    }

    controller.wait();
    SetConsoleCtrlHandler(console_handler, FALSE);
    g_controller = nullptr;

    if (controller.last_status() != 0) {
        std::cerr << controller.last_error() << '\n';
        return 1;
    }
    return 0;
}