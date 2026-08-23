[CmdletBinding()]
param(
    [string]$Root = (Join-Path $PSScriptRoot '..\dist\windows-x64'),
    [switch]$WithoutDokany,
    [switch]$WithoutWinUI
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = [System.IO.Path]::GetFullPath($Root)
if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
    throw "Canonical Windows staging directory does not exist: $Root"
}

$required = @(
    'PS2-DriveForge.exe',
    'ps2-driveforge-inspect.exe',
    'ps2-driveforge-benchmark.exe',
    'ps2-driveforge-hdl-tools.exe',
    'ps2-driveforge-pfs-tools.exe',
    'README.md',
    'CHANGELOG.md',
    'BUILDING.md',
    'CONTRIBUTING.md',
    'docs\architecture.md',
    'docs\testing.md',
    'docs\release-process.md',
    'docs\rc-hardware-checklist.md',
    'docs\REAL_HARDWARE_VALIDATION.md',
    'docs\frieren-plan.md',
    'docs\opl-asset-pipeline.md',
    'docs\frieren-pfs-write.md',
    'docs\emilia-benchmark-2026-08-22.md'
)
if (-not $WithoutDokany) {
    $required += 'PS2-DriveForge-Mount.exe'
}
if (-not $WithoutWinUI) {
    $required += 'WinUI\PS2-DriveForge-WinUI.exe'
}

$missing = @()
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $relative) -PathType Leaf)) {
        $missing += $relative
    }
}

if ($missing.Count -ne 0) {
    Write-Host 'Canonical package contents:'
    Get-ChildItem -LiteralPath $Root -Recurse -File |
        ForEach-Object { Write-Host ('  ' + $_.FullName.Substring($Root.Length).TrimStart('\')) }
    throw "Canonical Windows package is missing required files: $($missing -join ', ')"
}

if (-not $WithoutWinUI) {
    $winuiRoot = Join-Path $Root 'WinUI'
    $payload = @(Get-ChildItem -LiteralPath $winuiRoot -Recurse -File)

    $duplicateExecutables = @(
        Get-ChildItem -LiteralPath $winuiRoot -Recurse -File -Filter 'PS2-DriveForge-WinUI.exe'
    )
    if ($duplicateExecutables.Count -ne 1) {
        throw "Expected exactly one WinUI executable in canonical staging, found $($duplicateExecutables.Count)."
    }

    $xamlRuntime = @(
        Get-ChildItem -LiteralPath $winuiRoot -Recurse -File -Filter 'Microsoft.UI.Xaml.dll'
    )
    if ($xamlRuntime.Count -ne 1) {
        throw "WinUI staging is not self-contained: expected exactly one Microsoft.UI.Xaml.dll, found $($xamlRuntime.Count)."
    }

    $windowsAppRuntime = @(
        Get-ChildItem -LiteralPath $winuiRoot -Recurse -File |
            Where-Object { $_.Name -like 'Microsoft.WindowsAppRuntime*.dll' }
    )
    if ($windowsAppRuntime.Count -eq 0) {
        throw 'WinUI staging is missing Windows App SDK runtime DLLs (Microsoft.WindowsAppRuntime*.dll).'
    }

    if ($payload.Count -lt 10) {
        throw "WinUI staging is implausibly small for a self-contained deployment: only $($payload.Count) files."
    }
}

$expectedTests = @(
    'ps2-driveforge-tests.exe',
    'ps2-driveforge-pfs-file-tests.exe',
    'ps2-driveforge-pfs-segi-tests.exe',
    'ps2-driveforge-pfs-write-tests.exe',
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
    'ps2-driveforge-opl-partition-tests.exe',
    'ps2-driveforge-storage-profile-tests.exe',
    'ps2-driveforge-dokany-open-policy-tests.exe',
    'ps2-driveforge-darkness-policy-tests.exe'
)

$missingTests = @(
    $expectedTests | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $Root $_) -PathType Leaf)
    }
)
if ($missingTests.Count -ne 0) {
    throw "Canonical Windows package is missing regression executables: $($missingTests -join ', ')"
}

$packagedTests = @(
    Get-ChildItem -LiteralPath $Root -File |
        Where-Object { $_.Name -like 'ps2-driveforge*-tests.exe' }
)
$unexpectedTests = @(
    $packagedTests.Name | Where-Object { $_ -notin $expectedTests }
)
if ($unexpectedTests.Count -ne 0) {
    throw "Canonical Windows package contains unexpected regression executables: $($unexpectedTests -join ', ')"
}
if ($packagedTests.Count -ne $expectedTests.Count) {
    throw "Expected exactly $($expectedTests.Count) packaged regression executables, found $($packagedTests.Count)."
}

$packagedDocs = @(Get-ChildItem -LiteralPath (Join-Path $Root 'docs') -File -Filter '*.md')
if ($packagedDocs.Count -lt 13) {
    throw "Expected the complete release documentation set, found only $($packagedDocs.Count) Markdown files in staging."
}

Write-Host "Canonical Frieren Windows package staging verified: $Root" -ForegroundColor Green
Write-Host "Required release files: $($required.Count)"
Write-Host "Regression executables: $($packagedTests.Count)"
Write-Host "Packaged Markdown docs: $($packagedDocs.Count)"
if (-not $WithoutWinUI) {
    Write-Host "WinUI payload files: $(@(Get-ChildItem -LiteralPath (Join-Path $Root 'WinUI') -Recurse -File).Count)"
}
