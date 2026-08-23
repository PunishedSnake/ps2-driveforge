#include "ps2hdd/hdl_image_install.hpp"

#include "ps2hdd/apa_hdl_headers.hpp"
#include "ps2hdd/apa_mutation.hpp"
#include "ps2hdd/hdl_install_plan.hpp"
#include "ps2hdd/hdl_metadata_builder.hpp"
#include "ps2hdd/recovery_capsule.hpp"
#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace ps2hdd::hdl {
namespace {

[[nodiscard]] const apa::Partition* find_main_hdl(const apa::ScanResult& scan,
                                                   std::string_view id) noexcept
{
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.type == apa::kTypeHdl && partition.id == id) {
            return &partition;
        }
    }
    return nullptr;
}

[[nodiscard]] bool partition_id_exists(const apa::ScanResult& scan,
                                       std::string_view id) noexcept
{
    return std::any_of(scan.partitions.begin(), scan.partitions.end(), [&](const auto& partition) {
        return !partition.is_sub() && partition.id == id;
    });
}

void report(const ImageInstallProgressCallback& callback,
            std::uint64_t copied, std::uint64_t total, std::string_view phase)
{
    if (callback) {
        callback({copied, total, phase});
    }
}

[[nodiscard]] bool verify_staged_before_images(WritableBlockDevice& disk,
                                                std::span<const StagedWrite> writes,
                                                std::string& error)
{
    for (const auto& write : writes) {
        std::vector<std::byte> current(write.before.size());
        if (!disk.read(write.offset, current)) {
            error = "Could not re-read staged metadata before durable recovery prepare";
            return false;
        }
        if (current != write.before) {
            error = "Target metadata changed after transaction staging; refusing stale recovery prepare";
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool admit_recovery_path(WritableBlockDevice& disk,
                                       const std::filesystem::path& path,
                                       std::string& error)
{
    if (path.empty()) {
        return true;
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "Could not inspect existing HDL recovery capsule path: " + ec.message();
        return false;
    }
    if (!exists) {
        return true;
    }

    const auto previous = inspect_recovery_capsule(path, disk);
    if (!previous.ok) {
        error = "Existing HDL recovery capsule is unreadable and will not be overwritten: " +
                previous.error;
        return false;
    }
    if (previous.capsule_state == RecoveryCapsuleState::prepared) {
        error = "An unresolved PREPARED HDL recovery capsule already exists; recover it before a new install";
        return false;
    }
    if (previous.device_state == RecoveryDeviceState::foreign_or_corrupt) {
        error = "Existing terminal HDL recovery capsule does not match the current target device";
        return false;
    }
    return true;
}

} // namespace

ImageInstallResult install_to_image(WritableBlockDevice& disk,
                                    BlockDevice& game_iso,
                                    const ImageInstallOptions& options,
                                    ImageInstallProgressCallback progress)
{
    ImageInstallResult result;
    if (static_cast<BlockDevice*>(&disk) == &game_iso) {
        result.error = "HDL source ISO and writable target must be different block devices";
        return result;
    }
    if (options.copy_buffer_bytes < 2048 || options.copy_buffer_bytes % 2048 != 0) {
        result.error = "HDL copy buffer size must be a non-zero multiple of 2048 bytes";
        return result;
    }
    if (options.media != MediaType::cd && options.media != MediaType::dvd) {
        result.error = "HDL image install requires explicit CD or DVD media type";
        return result;
    }
    if (!admit_recovery_path(disk, options.recovery_capsule_path, result.error)) {
        return result;
    }

    report(progress, 0, game_iso.size_bytes(), "preflight");
    apa::Reader initial_reader(disk);
    const auto initial_scan = initial_reader.scan();
    if (!initial_scan.ok()) {
        result.error = "Refusing HDL install because the target APA scan is not clean";
        return result;
    }

    const auto install_plan = plan_install(
        initial_scan, disk.size_bytes(), game_iso, options.title, options.hidden);
    if (!install_plan.ok) {
        result.error = install_plan.error;
        return result;
    }
    result.partition_id = install_plan.partition_id;
    result.startup = install_plan.source.startup;
    result.main_start_lba = install_plan.allocation.main_start_lba();
    result.sub_count = install_plan.allocation.sub_count();
    result.payload_bytes = install_plan.source.image_bytes;
    result.allocated_bytes = install_plan.allocation.allocated_bytes;

    if (partition_id_exists(initial_scan, result.partition_id)) {
        result.error = "HDL APA partition ID already exists on the target";
        return result;
    }

    const auto header_plan = apa::build_hdl_headers(
        install_plan.allocation, result.partition_id, options.created);
    if (!header_plan.ok) {
        result.error = "Could not serialize planned HDL APA headers: " + header_plan.error;
        return result;
    }

    MetadataBuildRequest metadata_request;
    metadata_request.title = options.title;
    metadata_request.startup = result.startup;
    metadata_request.compat_flags = options.compat_flags;
    metadata_request.dma = options.dma;
    metadata_request.layer_break = options.layer_break;
    metadata_request.media = options.media;
    metadata_request.payload_bytes = result.payload_bytes;
    const auto metadata = build_install_metadata(install_plan.allocation, metadata_request);
    if (!metadata.ok) {
        result.error = "Could not build native HDL install metadata: " + metadata.error;
        return result;
    }

    // Payload is deliberately copied while the new APA headers remain absent.
    // Failed payload writes therefore leave at worst stale bytes in free space.
    std::vector<std::byte> buffer(options.copy_buffer_bytes);
    std::vector<std::byte> verify(options.copy_buffer_bytes);
    std::uint64_t copied = 0;
    for (const auto& extent : metadata.payload_extents) {
        std::uint64_t extent_done = 0;
        while (extent_done < extent.bytes) {
            const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
                extent.bytes - extent_done, buffer.size()));
            auto source = std::span<std::byte>(buffer).first(amount);
            auto readback = std::span<std::byte>(verify).first(amount);
            if (!game_iso.read(extent.source_offset_bytes + extent_done, source)) {
                result.error = "Could not read game ISO while streaming HDL payload";
                result.orphan_payload_bytes_on_failure = copied;
                return result;
            }
            const auto target_offset = extent.device_offset_bytes + extent_done;
            if (!disk.write(target_offset, std::span<const std::byte>(source))) {
                result.error = "Could not write game payload into planned free APA extent";
                result.orphan_payload_bytes_on_failure = copied;
                return result;
            }
            if (!disk.read(target_offset, readback) ||
                !std::equal(source.begin(), source.end(), readback.begin(), readback.end())) {
                result.error = "Game payload read-back verification failed before APA publication";
                result.orphan_payload_bytes_on_failure = copied + amount;
                return result;
            }
            extent_done += amount;
            copied += amount;
            report(progress, copied, result.payload_bytes, "payload");
        }
    }
    if (copied != result.payload_bytes) {
        result.error = "Internal HDL payload map did not consume the complete source ISO";
        result.orphan_payload_bytes_on_failure = copied;
        return result;
    }
    if (!disk.flush()) {
        result.error = "Could not durably flush HDL payload before metadata publication";
        result.orphan_payload_bytes_on_failure = copied;
        return result;
    }

    // Metadata and the entire APA publication are a small rollback-capable
    // transaction. The bulk payload stays outside it because before-imaging a
    // multi-gigabyte free extent would defeat the point of transactional metadata.
    WriteTransaction transaction(disk);
    const auto metadata_offset =
        static_cast<std::uint64_t>(result.main_start_lba) * apa::kSectorSize + kMetadataOffset;
    if (!transaction.stage(metadata_offset, metadata.metadata, "new HDL DEADFEED metadata")) {
        result.error = "Could not stage native HDL metadata: " + transaction.stage_error();
        result.orphan_payload_bytes_on_failure = copied;
        return result;
    }

    const auto publication_error = apa::stage_hdl_header_publication(
        disk, install_plan.allocation, header_plan, transaction);
    if (!publication_error.empty()) {
        result.error = "Could not stage APA publication: " + publication_error;
        result.orphan_payload_bytes_on_failure = copied;
        return result;
    }

    if (!options.recovery_capsule_path.empty()) {
        if (!verify_staged_before_images(disk, transaction.staged_writes(), result.error)) {
            result.orphan_payload_bytes_on_failure = copied;
            return result;
        }
        const auto capsule = create_recovery_capsule(
            options.recovery_capsule_path, disk, transaction.staged_writes());
        if (!capsule.ok) {
            result.error = "Could not durably prepare HDL recovery capsule: " + capsule.error;
            result.orphan_payload_bytes_on_failure = copied;
            return result;
        }
        result.recovery_capsule_created = true;
    }

    report(progress, copied, result.payload_bytes, "publish");
    const auto commit = transaction.commit([&]() -> std::string {
        apa::Reader reader(disk);
        const auto scan = reader.scan();
        if (!scan.ok()) {
            return "APA chain did not re-scan cleanly after publication";
        }
        const auto* partition = find_main_hdl(scan, result.partition_id);
        if (partition == nullptr) {
            return "new HDL main partition is not visible after APA publication";
        }
        if (partition->start_lba != result.main_start_lba ||
            partition->sub_count != result.sub_count) {
            return "published HDL APA extent relationship differs from the approved plan";
        }

        const auto game = read_game_info(disk, *partition);
        if (!game.ok) {
            return "normal HDL parser rejected the newly installed metadata: " + game.error;
        }
        if (game.game.title != options.title || game.game.startup != result.startup ||
            game.game.compat_flags != options.compat_flags || game.game.dma != options.dma ||
            game.game.layer_break != options.layer_break || game.game.media != options.media ||
            game.game.raw_size_bytes != result.payload_bytes ||
            !game.game.allocation_table_consistent) {
            return "new HDL metadata did not round-trip through the normal parser";
        }
        return {};
    });

    if (!commit.ok) {
        result.error = "HDL APA publication transaction failed: " + commit.error;
        result.orphan_payload_bytes_on_failure = copied;
        if (result.recovery_capsule_created) {
            if (commit.rollback_ok) {
                const auto restored = restore_prepared_recovery_capsule(
                    options.recovery_capsule_path, disk);
                if (!restored.ok) {
                    result.recovery_pending = true;
                    result.warning = "Automatic transaction rollback succeeded, but recovery capsule "
                                     "could not be finalized as RESTORED: " + restored.error;
                }
            } else {
                result.recovery_pending = true;
                result.warning = "In-memory rollback failed; PREPARED recovery capsule must be resolved";
            }
        }
        return result;
    }

    if (result.recovery_capsule_created) {
        const auto marked = mark_recovery_capsule_committed(options.recovery_capsule_path);
        if (!marked.ok) {
            result.recovery_pending = true;
            result.warning = "HDL install verified successfully, but recovery capsule could not be "
                             "marked COMMITTED: " + marked.error;
        }
    }

    report(progress, copied, result.payload_bytes, "complete");
    result.ok = true;
    result.orphan_payload_bytes_on_failure = 0;
    return result;
}

} // namespace ps2hdd::hdl
