[CmdletBinding()]
param(
    [string]$Root = (Join-Path $PSScriptRoot '..\dist\windows-x64'),
    [switch]$WithoutDokany,
    [switch]$WithoutWinUI
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RegressionManifest = Join-Path $PSScriptRoot 'frieren-regression-tests.ps1'
if (-not (Test-Path -LiteralPath $RegressionManifest -PathType Leaf)) {
    throw "Frieren regression manifest is missing: $RegressionManifest"
}
. $RegressionManifest

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
    'ps2-driveforge-physical-tools.exe',
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
    'docs\fhdb-bootstrap-parity.md',
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

$expectedTests = @($FrierenRegressionTests)
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
if ($packagedDocs.Count -lt 14) {
    throw "Expected the complete release documentation set, found only $($packagedDocs.Count) Markdown files in staging."
}

Write-Host "Canonical Frieren Windows package staging verified: $Root" -ForegroundColor Green
Write-Host "Required release files: $($required.Count)"
Write-Host "Regression executables: $($packagedTests.Count)"
Write-Host "Packaged Markdown docs: $($packagedDocs.Count)"
if (-not $WithoutWinUI) {
    Write-Host "WinUI payload files: $(@(Get-ChildItem -LiteralPath (Join-Path $Root 'WinUI') -Recurse -File).Count)"
}
