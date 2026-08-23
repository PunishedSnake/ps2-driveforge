cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

project(ps2_driveforge VERSION 0.6.0 LANGUAGES CXX)

set(PS2DF_CODENAME "Frieren" CACHE STRING "Release codename")
option(PS2DF_BUILD_TESTS "Build unit tests" ON)
option(PS2DF_BUILD_FUZZERS "Build libFuzzer parser targets (Clang only)" OFF)
option(PS2DF_BUILD_DOKANY "Build the Windows Dokany read-only mount frontend" OFF)
option(PS2DF_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ps2hdd/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ps2hdd/version.hpp
    @ONLY
)

add_library(ps2driveforge_core
    src/core/file_block_device.cpp
    src/core/writable_file_block_device.cpp
    src/core/disk_layout_guard.cpp
    src/core/physical_write_guard.cpp
    src/core/writable_apa_volume.cpp
    src/core/write_transaction.cpp
    src/core/mutation_journal.cpp
    src/core/sha256.cpp
    src/core/fhdb_rescue.cpp
    src/core/fhdb_artifacts.cpp
    src/core/fhdb_restore.cpp
    src/core/apa_repair.cpp
    src/core/apa_forensic.cpp
    src/core/apa_recovery_apply.cpp
    src/core/apa.cpp
    src/core/apa_allocation.cpp
    src/core/apa_hdl_headers.cpp
    src/core/apa_mutation.cpp
    src/core/apa_remove.cpp
    src/core/apa_volume.cpp
    src/core/pfs.cpp
    src/core/pfs_extent_map.cpp
    src/core/pfs_extent_layout.cpp
    src/core/pfs_write.cpp
    src/core/pfs_directory.cpp
    src/core/pfs_advanced_write.cpp
    src/core/pfs_segi_write.cpp
    src/core/pfs_tree_remove.cpp
    src/core/pfs_batch.cpp
    src/core/tar_archive.cpp
    src/core/hdl.cpp
    src/core/ps2_iso.cpp
    src/core/hdl_install_plan.cpp
    src/core/hdl_metadata_builder.cpp
    src/core/hdl_image_install.cpp
    src/core/opl_assets.cpp
    src/core/partition_catalog.cpp
)

target_include_directories(ps2driveforge_core
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_features(ps2driveforge_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ps2driveforge_core PRIVATE /W4 /permissive-)
    if(PS2DF_WARNINGS_AS_ERRORS)
        target_compile_options(ps2driveforge_core PRIVATE /WX)
    endif()
else()
    target_compile_options(ps2driveforge_core PRIVATE -Wall -Wextra -Wpedantic)
    if(PS2DF_WARNINGS_AS_ERRORS)
        target_compile_options(ps2driveforge_core PRIVATE -Werror)
    endif()
endif()

if(WIN32)
    target_sources(ps2driveforge_core PRIVATE
        src/platform/windows/physical_drive.cpp
        src/platform/windows/writable_physical_drive.cpp
    )
    target_compile_definitions(ps2driveforge_core PUBLIC
        PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE=1
        PS2DF_HAS_WINDOWS_PHYSICAL_WRITE=1
    )
endif()

add_library(ps2driveforge_host
    src/host/pfs_export.cpp
    src/host/drive_session.cpp
    src/host/mount_view.cpp
    src/host/hdl_enrichment.cpp
    src/host/management_model.cpp
    src/host/http_client.cpp
    src/host/opl_asset_pipeline.cpp
    src/host/opl_partition.cpp
    src/host/opl_pfs_import.cpp
    src/host/image_game_deploy.cpp
)
target_link_libraries(ps2driveforge_host PUBLIC ps2driveforge_core)
target_compile_features(ps2driveforge_host PUBLIC cxx_std_20)
if(WIN32)
    target_sources(ps2driveforge_host PRIVATE
        src/platform/windows/physical_discovery.cpp
        src/platform/windows/physical_game_deploy.cpp
        src/platform/windows/physical_partition_remove.cpp
    )
    target_compile_definitions(ps2driveforge_host PUBLIC PS2DF_HAS_WINDOWS_PHYSICAL_MUTATION=1)
    target_link_libraries(ps2driveforge_host PUBLIC setupapi winhttp)
endif()
if(MSVC)
    target_compile_options(ps2driveforge_host PRIVATE /W4 /permissive-)
    if(PS2DF_WARNINGS_AS_ERRORS)
        target_compile_options(ps2driveforge_host PRIVATE /WX)
    endif()
else()
    target_compile_options(ps2driveforge_host PRIVATE -Wall -Wextra -Wpedantic)
    if(PS2DF_WARNINGS_AS_ERRORS)
        target_compile_options(ps2driveforge_host PRIVATE -Werror)
    endif()
endif()

add_executable(ps2-driveforge-inspect src/cli/main.cpp)
target_link_libraries(ps2-driveforge-inspect PRIVATE ps2driveforge_host)
target_compile_features(ps2-driveforge-inspect PRIVATE cxx_std_20)

add_executable(ps2-driveforge-benchmark src/cli/benchmark.cpp)
target_link_libraries(ps2-driveforge-benchmark PRIVATE ps2driveforge_host)
target_compile_features(ps2-driveforge-benchmark PRIVATE cxx_std_20)

add_executable(ps2-driveforge-hdl-tools src/cli/hdl_tools.cpp)
target_link_libraries(ps2-driveforge-hdl-tools PRIVATE ps2driveforge_host)
target_compile_features(ps2-driveforge-hdl-tools PRIVATE cxx_std_20)

add_executable(ps2-driveforge-pfs-tools src/cli/pfs_tools.cpp)
target_link_libraries(ps2-driveforge-pfs-tools PRIVATE ps2driveforge_core)
target_compile_features(ps2-driveforge-pfs-tools PRIVATE cxx_std_20)

if(WIN32)
    # Keep raw physical mutation in a separate developer executable. The normal
    # inspector and image tools must not gain write behavior merely because a
    # Windows build happens to expose WritablePhysicalDrive below them.
    add_executable(ps2-driveforge-physical-tools src/cli/physical_tools.cpp)
    target_link_libraries(ps2-driveforge-physical-tools PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-physical-tools PRIVATE cxx_std_20)

    add_executable(ps2-driveforge-gui WIN32
        src/gui/windows_main.cpp
        src/gui/windows_theme.cpp
        src/gui/windows_elevation.cpp
    )
    set_target_properties(ps2-driveforge-gui PROPERTIES OUTPUT_NAME "PS2-DriveForge")
    target_link_libraries(ps2-driveforge-gui PRIVATE
        ps2driveforge_host
        comctl32
        comdlg32
        dwmapi
        uxtheme
        shell32
        ole32
    )
    target_compile_features(ps2-driveforge-gui PRIVATE cxx_std_20)
    target_compile_definitions(ps2-driveforge-gui PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(ps2-driveforge-gui PRIVATE /W4 /permissive- /utf-8)
        if(PS2DF_WARNINGS_AS_ERRORS)
            target_compile_options(ps2-driveforge-gui PRIVATE /WX)
        endif()
    endif()

    add_executable(ps2-driveforge-launcher WIN32 src/launcher/windows_launcher.cpp)
    set_target_properties(ps2-driveforge-launcher PROPERTIES OUTPUT_NAME "PS2-DriveForge-Launcher")
    target_link_libraries(ps2-driveforge-launcher PRIVATE comctl32 shell32)
    target_compile_features(ps2-driveforge-launcher PRIVATE cxx_std_20)
    target_compile_definitions(ps2-driveforge-launcher PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(ps2-driveforge-launcher PRIVATE /W4 /permissive- /utf-8)
        if(PS2DF_WARNINGS_AS_ERRORS)
            target_compile_options(ps2-driveforge-launcher PRIVATE /WX)
        endif()
    endif()
endif()

if(PS2DF_BUILD_DOKANY)
    if(NOT WIN32)
        message(FATAL_ERROR "PS2DF_BUILD_DOKANY is only supported on Windows")
    endif()

    find_package(Dokany REQUIRED)

    add_library(ps2driveforge_dokany
        src/mount/dokany_controller.cpp
    )
    target_include_directories(ps2driveforge_dokany PRIVATE src/mount)
    target_link_libraries(ps2driveforge_dokany PUBLIC ps2driveforge_host Dokany::Dokany)
    target_compile_features(ps2driveforge_dokany PUBLIC cxx_std_20)
    target_compile_definitions(ps2driveforge_dokany PUBLIC PS2DF_HAS_DOKANY=1 UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(ps2driveforge_dokany PRIVATE /W4 /permissive- /utf-8)
        if(PS2DF_WARNINGS_AS_ERRORS)
            target_compile_options(ps2driveforge_dokany PRIVATE /WX)
        endif()
    endif()

    target_link_libraries(ps2-driveforge-gui PRIVATE ps2driveforge_dokany)
    target_compile_definitions(ps2-driveforge-gui PRIVATE PS2DF_HAS_DOKANY=1)

    add_executable(ps2-driveforge-mount src/mount/dokany_main.cpp)
    set_target_properties(ps2-driveforge-mount PROPERTIES OUTPUT_NAME "PS2-DriveForge-Mount")
    target_link_libraries(ps2-driveforge-mount PRIVATE ps2driveforge_dokany)
    target_compile_features(ps2-driveforge-mount PRIVATE cxx_std_20)
    target_compile_definitions(ps2-driveforge-mount PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(ps2-driveforge-mount PRIVATE /W4 /permissive- /utf-8)
        if(PS2DF_WARNINGS_AS_ERRORS)
            target_compile_options(ps2-driveforge-mount PRIVATE /WX)
        endif()
    endif()
endif()

if(PS2DF_BUILD_TESTS)
    enable_testing()

    add_executable(ps2-driveforge-tests tests/apa_tests.cpp)
    target_link_libraries(ps2-driveforge-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-tests COMMAND ps2-driveforge-tests)

    add_executable(ps2-driveforge-disk-layout-guard-tests tests/disk_layout_guard_tests.cpp)
    target_link_libraries(ps2-driveforge-disk-layout-guard-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-disk-layout-guard-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-disk-layout-guard-tests COMMAND ps2-driveforge-disk-layout-guard-tests)

    add_executable(ps2-driveforge-physical-write-guard-tests tests/physical_write_guard_tests.cpp)
    target_link_libraries(ps2-driveforge-physical-write-guard-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-physical-write-guard-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-physical-write-guard-tests COMMAND ps2-driveforge-physical-write-guard-tests)

    add_executable(ps2-driveforge-pfs-file-tests tests/pfs_file_tests.cpp)
    target_link_libraries(ps2-driveforge-pfs-file-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-pfs-file-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-pfs-file-tests COMMAND ps2-driveforge-pfs-file-tests)

    add_executable(ps2-driveforge-pfs-segi-tests tests/pfs_segi_tests.cpp)
    target_link_libraries(ps2-driveforge-pfs-segi-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-pfs-segi-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-pfs-segi-tests COMMAND ps2-driveforge-pfs-segi-tests)

    add_executable(ps2-driveforge-pfs-extent-layout-tests tests/pfs_extent_layout_tests.cpp)
    target_link_libraries(ps2-driveforge-pfs-extent-layout-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-pfs-extent-layout-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-pfs-extent-layout-tests COMMAND ps2-driveforge-pfs-extent-layout-tests)

    add_executable(ps2-driveforge-pfs-segi-write-tests tests/pfs_segi_write_tests.cpp)
    target_link_libraries(ps2-driveforge-pfs-segi-write-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-pfs-segi-write-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-pfs-segi-write-tests COMMAND ps2-driveforge-pfs-segi-write-tests)

    add_executable(ps2-driveforge-pfs-write-tests tests/pfs_write_tests.cpp)
    target_link_libraries(ps2-driveforge-pfs-write-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-pfs-write-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-pfs-write-tests COMMAND ps2-driveforge-pfs-write-tests)

    add_executable(ps2-driveforge-pfs-directory-tests tests/pfs_directory_tests.cpp)
    target_link_libraries(ps2-driveforge-pfs-directory-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-pfs-directory-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-pfs-directory-tests COMMAND ps2-driveforge-pfs-directory-tests)

    add_executable(ps2-driveforge-pfs-advanced-write-tests tests/pfs_advanced_write_tests.cpp)
    target_link_libraries(ps2-driveforge-pfs-advanced-write-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-pfs-advanced-write-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-pfs-advanced-write-tests COMMAND ps2-driveforge-pfs-advanced-write-tests)

    add_executable(ps2-driveforge-pfs-tree-remove-tests tests/pfs_tree_remove_tests.cpp)
    target_link_libraries(ps2-driveforge-pfs-tree-remove-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-pfs-tree-remove-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-pfs-tree-remove-tests COMMAND ps2-driveforge-pfs-tree-remove-tests)

    add_executable(ps2-driveforge-pfs-batch-tests tests/pfs_batch_tests.cpp)
    target_link_libraries(ps2-driveforge-pfs-batch-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-pfs-batch-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-pfs-batch-tests COMMAND ps2-driveforge-pfs-batch-tests)

    add_executable(ps2-driveforge-tar-archive-tests tests/tar_archive_tests.cpp)
    target_link_libraries(ps2-driveforge-tar-archive-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-tar-archive-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-tar-archive-tests COMMAND ps2-driveforge-tar-archive-tests)

    add_executable(ps2-driveforge-host-tests tests/host_tests.cpp)
    target_link_libraries(ps2-driveforge-host-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-host-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-host-tests COMMAND ps2-driveforge-host-tests)

    add_executable(ps2-driveforge-e2e-image-tests tests/e2e_image_tests.cpp)
    target_link_libraries(ps2-driveforge-e2e-image-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-e2e-image-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-e2e-image-tests COMMAND ps2-driveforge-e2e-image-tests)

    add_executable(ps2-driveforge-corruption-tests tests/corruption_tests.cpp)
    target_link_libraries(ps2-driveforge-corruption-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-corruption-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-corruption-tests COMMAND ps2-driveforge-corruption-tests)

    add_executable(ps2-driveforge-session-tests tests/session_tests.cpp)
    target_link_libraries(ps2-driveforge-session-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-session-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-session-tests COMMAND ps2-driveforge-session-tests)

    add_executable(ps2-driveforge-read-cache-tests tests/read_cache_tests.cpp)
    target_link_libraries(ps2-driveforge-read-cache-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-read-cache-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-read-cache-tests COMMAND ps2-driveforge-read-cache-tests)

    add_executable(ps2-driveforge-read-ahead-tests tests/read_ahead_tests.cpp)
    target_link_libraries(ps2-driveforge-read-ahead-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-read-ahead-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-read-ahead-tests COMMAND ps2-driveforge-read-ahead-tests)

    add_executable(ps2-driveforge-partition-catalog-tests tests/partition_catalog_tests.cpp)
    target_link_libraries(ps2-driveforge-partition-catalog-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-partition-catalog-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-partition-catalog-tests COMMAND ps2-driveforge-partition-catalog-tests)

    add_executable(ps2-driveforge-partition-grouping-tests tests/partition_grouping_tests.cpp)
    target_link_libraries(ps2-driveforge-partition-grouping-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-partition-grouping-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-partition-grouping-tests COMMAND ps2-driveforge-partition-grouping-tests)

    add_executable(ps2-driveforge-hdl-enrichment-tests tests/hdl_enrichment_tests.cpp)
    target_link_libraries(ps2-driveforge-hdl-enrichment-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-hdl-enrichment-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-hdl-enrichment-tests COMMAND ps2-driveforge-hdl-enrichment-tests)

    add_executable(ps2-driveforge-hdl-write-tests tests/hdl_write_tests.cpp)
    target_link_libraries(ps2-driveforge-hdl-write-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-hdl-write-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-hdl-write-tests COMMAND ps2-driveforge-hdl-write-tests)

    add_executable(ps2-driveforge-write-transaction-tests tests/write_transaction_tests.cpp)
    target_link_libraries(ps2-driveforge-write-transaction-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-write-transaction-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-write-transaction-tests COMMAND ps2-driveforge-write-transaction-tests)

    add_executable(ps2-driveforge-mutation-journal-tests tests/mutation_journal_tests.cpp)
    target_link_libraries(ps2-driveforge-mutation-journal-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-mutation-journal-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-mutation-journal-tests COMMAND ps2-driveforge-mutation-journal-tests)

    add_executable(ps2-driveforge-fhdb-rescue-tests tests/fhdb_rescue_tests.cpp)
    target_link_libraries(ps2-driveforge-fhdb-rescue-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-fhdb-rescue-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-fhdb-rescue-tests COMMAND ps2-driveforge-fhdb-rescue-tests)

    add_executable(ps2-driveforge-fhdb-artifacts-tests tests/fhdb_artifacts_tests.cpp)
    target_link_libraries(ps2-driveforge-fhdb-artifacts-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-fhdb-artifacts-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-fhdb-artifacts-tests COMMAND ps2-driveforge-fhdb-artifacts-tests)

    add_executable(ps2-driveforge-fhdb-restore-tests tests/fhdb_restore_tests.cpp)
    target_link_libraries(ps2-driveforge-fhdb-restore-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-fhdb-restore-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-fhdb-restore-tests COMMAND ps2-driveforge-fhdb-restore-tests)

    add_executable(ps2-driveforge-apa-repair-tests tests/apa_repair_tests.cpp)
    target_link_libraries(ps2-driveforge-apa-repair-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-apa-repair-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-apa-repair-tests COMMAND ps2-driveforge-apa-repair-tests)

    add_executable(ps2-driveforge-apa-forensic-tests tests/apa_forensic_tests.cpp)
    target_link_libraries(ps2-driveforge-apa-forensic-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-apa-forensic-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-apa-forensic-tests COMMAND ps2-driveforge-apa-forensic-tests)

    add_executable(ps2-driveforge-apa-recovery-apply-tests tests/apa_recovery_apply_tests.cpp)
    target_link_libraries(ps2-driveforge-apa-recovery-apply-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-apa-recovery-apply-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-apa-recovery-apply-tests COMMAND ps2-driveforge-apa-recovery-apply-tests)

    add_executable(ps2-driveforge-writable-apa-volume-tests tests/writable_apa_volume_tests.cpp)
    target_link_libraries(ps2-driveforge-writable-apa-volume-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-writable-apa-volume-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-writable-apa-volume-tests COMMAND ps2-driveforge-writable-apa-volume-tests)

    add_executable(ps2-driveforge-apa-allocation-tests tests/apa_allocation_tests.cpp)
    target_link_libraries(ps2-driveforge-apa-allocation-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-apa-allocation-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-apa-allocation-tests COMMAND ps2-driveforge-apa-allocation-tests)

    add_executable(ps2-driveforge-apa-hdl-header-tests tests/apa_hdl_header_tests.cpp)
    target_link_libraries(ps2-driveforge-apa-hdl-header-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-apa-hdl-header-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-apa-hdl-header-tests COMMAND ps2-driveforge-apa-hdl-header-tests)

    add_executable(ps2-driveforge-apa-mutation-tests tests/apa_mutation_tests.cpp)
    target_link_libraries(ps2-driveforge-apa-mutation-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-apa-mutation-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-apa-mutation-tests COMMAND ps2-driveforge-apa-mutation-tests)

    add_executable(ps2-driveforge-apa-remove-tests tests/apa_remove_tests.cpp)
    target_link_libraries(ps2-driveforge-apa-remove-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-apa-remove-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-apa-remove-tests COMMAND ps2-driveforge-apa-remove-tests)

    add_executable(ps2-driveforge-ps2-iso-tests tests/ps2_iso_tests.cpp)
    target_link_libraries(ps2-driveforge-ps2-iso-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-ps2-iso-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-ps2-iso-tests COMMAND ps2-driveforge-ps2-iso-tests)

    add_executable(ps2-driveforge-hdl-install-plan-tests tests/hdl_install_plan_tests.cpp)
    target_link_libraries(ps2-driveforge-hdl-install-plan-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-hdl-install-plan-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-hdl-install-plan-tests COMMAND ps2-driveforge-hdl-install-plan-tests)

    add_executable(ps2-driveforge-hdl-metadata-builder-tests tests/hdl_metadata_builder_tests.cpp)
    target_link_libraries(ps2-driveforge-hdl-metadata-builder-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-hdl-metadata-builder-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-hdl-metadata-builder-tests COMMAND ps2-driveforge-hdl-metadata-builder-tests)

    add_executable(ps2-driveforge-hdl-image-install-tests tests/hdl_image_install_tests.cpp)
    target_link_libraries(ps2-driveforge-hdl-image-install-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-hdl-image-install-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-hdl-image-install-tests COMMAND ps2-driveforge-hdl-image-install-tests)

    add_executable(ps2-driveforge-hdl-multipart-install-tests tests/hdl_multipart_install_tests.cpp)
    target_link_libraries(ps2-driveforge-hdl-multipart-install-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-hdl-multipart-install-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-hdl-multipart-install-tests COMMAND ps2-driveforge-hdl-multipart-install-tests)

    add_executable(ps2-driveforge-opl-assets-tests tests/opl_assets_tests.cpp)
    target_link_libraries(ps2-driveforge-opl-assets-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-opl-assets-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-opl-assets-tests COMMAND ps2-driveforge-opl-assets-tests)

    add_executable(ps2-driveforge-opl-asset-pipeline-tests tests/opl_asset_pipeline_tests.cpp)
    target_link_libraries(ps2-driveforge-opl-asset-pipeline-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-opl-asset-pipeline-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-opl-asset-pipeline-tests COMMAND ps2-driveforge-opl-asset-pipeline-tests)

    add_executable(ps2-driveforge-opl-pfs-import-tests tests/opl_pfs_import_tests.cpp)
    target_link_libraries(ps2-driveforge-opl-pfs-import-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-opl-pfs-import-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-opl-pfs-import-tests COMMAND ps2-driveforge-opl-pfs-import-tests)

    add_executable(ps2-driveforge-opl-tar-pfs-import-tests tests/opl_tar_pfs_import_tests.cpp)
    target_link_libraries(ps2-driveforge-opl-tar-pfs-import-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-opl-tar-pfs-import-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-opl-tar-pfs-import-tests COMMAND ps2-driveforge-opl-tar-pfs-import-tests)

    add_executable(ps2-driveforge-image-game-deploy-tests tests/image_game_deploy_tests.cpp)
    target_link_libraries(ps2-driveforge-image-game-deploy-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-image-game-deploy-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-image-game-deploy-tests COMMAND ps2-driveforge-image-game-deploy-tests)

    add_executable(ps2-driveforge-opl-partition-tests tests/opl_partition_tests.cpp)
    target_link_libraries(ps2-driveforge-opl-partition-tests PRIVATE ps2driveforge_host)
    target_compile_features(ps2-driveforge-opl-partition-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-opl-partition-tests COMMAND ps2-driveforge-opl-partition-tests)

    add_executable(ps2-driveforge-storage-profile-tests tests/storage_profile_tests.cpp)
    target_link_libraries(ps2-driveforge-storage-profile-tests PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-storage-profile-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-storage-profile-tests COMMAND ps2-driveforge-storage-profile-tests)

    add_executable(ps2-driveforge-dokany-open-policy-tests tests/dokany_open_policy_tests.cpp)
    target_include_directories(ps2-driveforge-dokany-open-policy-tests PRIVATE src/mount)
    target_compile_features(ps2-driveforge-dokany-open-policy-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-dokany-open-policy-tests COMMAND ps2-driveforge-dokany-open-policy-tests)

    add_executable(ps2-driveforge-darkness-policy-tests tests/darkness_policy_tests.cpp)
    target_compile_features(ps2-driveforge-darkness-policy-tests PRIVATE cxx_std_20)
    add_test(NAME ps2-driveforge-darkness-policy-tests COMMAND ps2-driveforge-darkness-policy-tests)
endif()

if(PS2DF_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "PS2DF_BUILD_FUZZERS requires Clang/libFuzzer")
    endif()
    add_executable(ps2-driveforge-fuzz-apa fuzz/apa_fuzzer.cpp)
    target_link_libraries(ps2-driveforge-fuzz-apa PRIVATE ps2driveforge_core)
    target_compile_features(ps2-driveforge-fuzz-apa PRIVATE cxx_std_20)
    target_compile_options(ps2-driveforge-fuzz-apa PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(ps2-driveforge-fuzz-apa PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
