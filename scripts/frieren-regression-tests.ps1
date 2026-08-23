# Canonical native Frieren regression executable manifest.
#
# Keep build staging and package verification on one source of truth. New CMake
# regression targets should be added here in the same change that wires them,
# otherwise the Windows development artifact is intentionally considered
# incomplete instead of silently omitting the new test.
$FrierenRegressionTests = @(
    'ps2-driveforge-tests.exe',
    'ps2-driveforge-disk-layout-guard-tests.exe',
    'ps2-driveforge-pfs-file-tests.exe',
    'ps2-driveforge-pfs-segi-tests.exe',
    'ps2-driveforge-pfs-write-tests.exe',
    'ps2-driveforge-pfs-directory-tests.exe',
    'ps2-driveforge-pfs-advanced-write-tests.exe',
    'ps2-driveforge-pfs-batch-tests.exe',
    'ps2-driveforge-host-tests.exe',
    'ps2-driveforge-e2e-image-tests.exe',
    'ps2-driveforge-corruption-tests.exe',
    'ps2-driveforge-session-tests.exe',
    'ps2-driveforge-read-cache-tests.exe',
    'ps2-driveforge-read-ahead-tests.exe',
    'ps2-driveforge-partition-catalog-tests.exe',
    'ps2-driveforge-hdl-enrichment-tests.exe',
    'ps2-driveforge-hdl-write-tests.exe',
    'ps2-driveforge-write-transaction-tests.exe',
    'ps2-driveforge-writable-apa-volume-tests.exe',
    'ps2-driveforge-apa-allocation-tests.exe',
    'ps2-driveforge-apa-hdl-header-tests.exe',
    'ps2-driveforge-apa-mutation-tests.exe',
    'ps2-driveforge-apa-remove-tests.exe',
    'ps2-driveforge-ps2-iso-tests.exe',
    'ps2-driveforge-hdl-install-plan-tests.exe',
    'ps2-driveforge-hdl-metadata-builder-tests.exe',
    'ps2-driveforge-hdl-image-install-tests.exe',
    'ps2-driveforge-opl-assets-tests.exe',
    'ps2-driveforge-opl-asset-pipeline-tests.exe',
    'ps2-driveforge-opl-pfs-import-tests.exe',
    'ps2-driveforge-opl-partition-tests.exe',
    'ps2-driveforge-storage-profile-tests.exe',
    'ps2-driveforge-dokany-open-policy-tests.exe',
    'ps2-driveforge-darkness-policy-tests.exe'
)
