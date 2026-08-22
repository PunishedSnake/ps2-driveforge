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
    'README.md',
    'CHANGELOG.md'
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
    if ($payload.Count -lt 2) {
        throw "WinUI staging contains an EXE but no self-contained runtime/resource payload: $winuiRoot"
    }

    $duplicateExecutables = @(
        Get-ChildItem -LiteralPath $winuiRoot -Recurse -File -Filter 'PS2-DriveForge-WinUI.exe'
    )
    if ($duplicateExecutables.Count -ne 1) {
        throw "Expected exactly one WinUI executable in canonical staging, found $($duplicateExecutables.Count)."
    }
}

$testExecutables = @(
    Get-ChildItem -LiteralPath $Root -File -Filter 'ps2-driveforge-*-tests.exe'
)
if ($testExecutables.Count -ne 14) {
    throw "Expected 14 packaged regression executables, found $($testExecutables.Count)."
}

Write-Host "Canonical Windows package staging verified: $Root" -ForegroundColor Green
Write-Host "Required release files: $($required.Count)"
Write-Host "Regression executables: $($testExecutables.Count)"
if (-not $WithoutWinUI) {
    Write-Host "WinUI payload files: $(@(Get-ChildItem -LiteralPath (Join-Path $Root 'WinUI') -Recurse -File).Count)"
}
