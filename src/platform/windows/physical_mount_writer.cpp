#ifdef _WIN32

#include "ps2hdd/physical_mount_writer.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_fingerprint.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/writable_physical_drive.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <system_error>

namespace ps2hdd {

PhysicalMountWriter make_physical_mount_writer(unsigned index, const std::filesystem::path& artifact_directory)
{
    PhysicalMountWriter result;
    if (artifact_directory.empty()) {
        result.error = "a writable physical mount requires a host-side recovery artifact directory";
        return result;
    }
    PhysicalDrive read_only(index);
    if (!read_only.is_open()) {
        result.error = "could not open PhysicalDrive read-only for writable-mount admission";
        return result;
    }
    const auto admission = physical_write::authorize(read_only);
    if (!admission.ok) {
        result.error = "physical write admission failed: " + admission.error;
        return result;
    }
    apa::Reader reader(read_only);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        result.error = "APA diagnostics are not clean; refusing a writable mount";
        return result;
    }
    const auto fingerprint = apa_layout_fingerprint(scan, read_only.size_bytes());

    std::error_code ec;
    std::filesystem::create_directories(artifact_directory, ec);
    if (ec) {
        result.error = "could not create recovery artifact directory: " + ec.message();
        return result;
    }
    std::array<std::byte, fhdb::kRescueApaHeaderBytes> master{};
    if (!read_only.read(0, master)) {
        result.error = "could not capture the APA master before enabling writes";
        return result;
    }
    const auto backup = fhdb::save_hddmbr(artifact_directory, master);
    if (!backup.ok) {
        result.error = "could not persist HDDMBR backup before enabling writes: " + backup.error;
        return result;
    }
    result.hddmbr_path = backup.path;

    result.factory = [index, fingerprint](std::string& error) -> std::unique_ptr<WritableBlockDevice> {
        PhysicalDrive check(index);
        if (!check.is_open()) {
            error = "PhysicalDrive is no longer readable (disconnected or asleep?)";
            return nullptr;
        }
        apa::Reader batch_reader(check);
        const auto current = batch_reader.scan();
        if (!current.ok() || apa_layout_fingerprint(current, check.size_bytes()) != fingerprint) {
            error = "target disk no longer matches the mounted APA layout; refusing to write";
            return nullptr;
        }
        const auto batch_admission = physical_write::authorize(check);
        if (!batch_admission.ok) {
            error = "per-batch write admission failed: " + batch_admission.error;
            return nullptr;
        }
        WritablePhysicalDriveOptions options;
        options.authorization = batch_admission.authorization;
        auto device = std::make_unique<WritablePhysicalDrive>(index, options);
        if (!device->is_open()) {
            error = "could not acquire guarded write lease: " + device->admission_error();
            return nullptr;
        }
        return device;
    };
    result.ok = true;
    return result;
}

} // namespace ps2hdd

#endif
