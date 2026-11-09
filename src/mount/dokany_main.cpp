#include "ps2hdd/dokany_mount.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_mount_writer.hpp"
#include "ps2hdd/version.hpp"
#include "ps2hdd/writable_file_block_device.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
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
                  L"  PS2-DriveForge-Mount.exe --image <disk.img> --mount <P:> --writable\n"
                  L"      [--allow <partition>]... [--replay-journal] [--debug]\n"
                  L"  PS2-DriveForge-Mount.exe --physical <index> --mount <P:> --writable\n"
                  L"      --confirm PhysicalDrive<index> [--allow <partition>]... [--replay-journal]\n"
                  L"  PS2-DriveForge-Mount.exe --unmount <P:>\n\n"
                  L"Mounts are read-only unless --writable is given. Writable mounts (disk images\n"
                  L"for now) accept Explorer writes into PFS partitions through the DriveForge\n"
                  L"write-back engine: data is staged on the host, coalesced and committed in\n"
                  L"batches; __system/__sysconf/__net stay protected unless named with --allow.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::optional<std::filesystem::path> image;
    std::optional<unsigned> physical;
    std::optional<std::wstring> mount_point;
    std::optional<std::wstring> unmount_point;
    bool debug = false;
    bool writable = false;
    bool replay_journal = false;
    std::wstring confirm;
    std::set<std::string> allowed;

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
        } else if (arg == L"--writable") {
            writable = true;
        } else if (arg == L"--confirm" && i + 1 < argc) {
            confirm = argv[++i];
        } else if (arg == L"--replay-journal") {
            replay_journal = true;
        } else if (arg == L"--allow" && i + 1 < argc) {
            const std::wstring id = argv[++i];
            allowed.insert(std::string(id.begin(), id.end()));
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

    if (writable && physical && confirm != L"PhysicalDrive" + std::to_wstring(*physical)) {
        std::wcerr << L"A writable mount of a physical drive needs the literal target confirmation:\n"
                   << L"  --confirm PhysicalDrive" << *physical << L"\n";
        return 2;
    }

    ps2hdd::DokanyMountController controller;
    bool started = false;
    if (writable) {
        wchar_t local[MAX_PATH]{};
        const DWORD chars = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        const std::filesystem::path base = chars > 0 && chars < MAX_PATH ? std::filesystem::path(local)
                                                                       : std::filesystem::temp_directory_path();
        ps2hdd::WriteBackOptions options;
        options.spool_directory = base / L"PS2 DriveForge" / L"Spool" /
                                  (image ? image->filename().wstring() : L"PhysicalDrive" + std::to_wstring(*physical));
        if (!allowed.empty()) {
            options.writable_partitions = allowed;
            for (const auto& id : allowed) options.protected_partitions.erase(id);
        }
        const auto orphans = ps2hdd::PfsWriteBack::scan_journal(options.spool_directory);
        if (!orphans.empty() && !replay_journal) {
            std::wcout << orphans.size() << L" uncommitted file(s) from a previous session are kept in\n  "
                       << options.spool_directory.wstring()
                       << L"\nRe-run with --replay-journal to write them to the disk.\n";
        }
        ps2hdd::WriterFactory factory;
        if (image) {
            const auto image_path = *image;
            factory = [image_path](std::string& error) -> std::unique_ptr<ps2hdd::WritableBlockDevice> {
                auto device = std::make_unique<ps2hdd::WritableFileBlockDevice>(image_path);
                if (!device->is_open()) {
                    error = "Could not reopen the disk image for writing";
                    return nullptr;
                }
                return device;
            };
        } else {
            auto guarded = ps2hdd::make_physical_mount_writer(*physical, base / L"PS2 DriveForge" / L"Recovery");
            if (!guarded.ok) {
                std::cerr << "Writable physical mount refused: " << guarded.error << '\n';
                return 1;
            }
            std::wcout << L"Safety backup of the APA master: " << guarded.hddmbr_path.wstring() << L"\n";
            factory = std::move(guarded.factory);
        }
        started = controller.start_writable(std::move(source), std::move(factory), options, *mount_point, debug);
        if (started && replay_journal && !orphans.empty()) {
            if (auto* engine = controller.write_back()) {
                std::wcout << L"Replaying " << engine->replay_orphans(orphans) << L" journaled file(s).\n";
            }
        }
    } else {
        started = controller.start(std::move(source), *mount_point, debug);
    }
    if (!started) {
        std::cerr << "Mount startup failed: " << controller.last_error() << '\n';
        return 1;
    }

    g_controller = &controller;
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::wcout << L"Mounting PS2 DriveForge " << (writable ? L"with write-back" : L"read-only") << L" at "
               << *mount_point << L"...\n"
               << L"Namespace root: " << *mount_point << L"Partitions\\\n"
               << L"Press Ctrl+C or run --unmount " << *mount_point
               << L" from another terminal.\n";

    for (int attempt = 0; attempt < 200 && controller.is_running() && !controller.is_mounted(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (controller.is_mounted()) {
        std::wcout << L"Mounted PS2 DriveForge " << (writable ? L"writable" : L"read-only") << L" at "
                   << *mount_point << L"\n";
    }

    controller.wait();
    if (auto* engine = controller.write_back()) {
        const auto stats = engine->stats();
        std::wcout << L"Write-back: " << stats.application_writes << L" application writes -> "
                   << stats.committed_ops << L" PFS operations in " << stats.disk_write_sessions
                   << L" disk session(s); " << stats.coalesced_ops << L" coalesced, " << stats.failed_ops
                   << L" failed.\n";
    }
    SetConsoleCtrlHandler(console_handler, FALSE);
    g_controller = nullptr;

    if (controller.last_status() != 0) {
        std::cerr << controller.last_error() << '\n';
        return 1;
    }
    return 0;
}