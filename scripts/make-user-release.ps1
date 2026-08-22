[CmdletBinding()]
param(
    [string]$Version = '0.5.0-rc4',
    [string]$CanonicalRoot = (Join-Path $PSScriptRoot '..\dist\windows-x64'),
    [string]$BuildBin = (Join-Path $PSScriptRoot '..\build\windows-x64\Release'),
    [string]$InnoCompiler = '',
    [string]$DokanyMsi = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-File {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required release input is missing: $Path"
    }
}

function Copy-RequiredFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    Require-File $Source
    $parent = Split-Path -Parent $Destination
    if ($parent) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

$Root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$CanonicalRoot = [System.IO.Path]::GetFullPath($CanonicalRoot)
$BuildBin = [System.IO.Path]::GetFullPath($BuildBin)
$DistRoot = Join-Path $Root 'dist'
$UserRoot = Join-Path $DistRoot 'user\windows-x64'
$PortableZip = Join-Path $DistRoot "PS2-DriveForge-$Version-Emilia-Portable-x64.zip"
$SetupExe = Join-Path $DistRoot "PS2-DriveForge-$Version-Emilia-Setup-x64.exe"

if (-not (Test-Path -LiteralPath $CanonicalRoot -PathType Container)) {
    throw "Canonical Windows staging was not found: $CanonicalRoot"
}

if (Test-Path -LiteralPath $UserRoot) {
    Remove-Item -LiteralPath $UserRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $UserRoot | Out-Null

# The root contains only the normal entrypoint and human-facing release docs.
# Runtime/detail binaries are deliberately placed below app/legacy/tools.
$Launcher = Join-Path $BuildBin 'PS2-DriveForge-Launcher.exe'
Copy-RequiredFile $Launcher (Join-Path $UserRoot 'PS2-DriveForge.exe')

foreach ($Doc in @('README.md', 'CHANGELOG.md')) {
    Copy-RequiredFile (Join-Path $CanonicalRoot $Doc) (Join-Path $UserRoot $Doc)
}

# Modern frontend + Windows App SDK runtime. The self-contained payload remains
# intact but is hidden below app/winui instead of spilling hundreds of files into
# the directory a normal user opens.
$CanonicalWinUI = Join-Path $CanonicalRoot 'WinUI'
if (-not (Test-Path -LiteralPath $CanonicalWinUI -PathType Container)) {
    throw "Canonical WinUI payload is missing: $CanonicalWinUI"
}
$WinUIDestination = Join-Path $UserRoot 'app\winui'
New-Item -ItemType Directory -Force -Path $WinUIDestination | Out-Null
Copy-Item -LiteralPath (Join-Path $CanonicalWinUI '*') -Destination $WinUIDestination -Recurse -Force

# Proven Win32 frontend is an explicit supported fallback, not a deprecated copy.
Copy-RequiredFile (Join-Path $CanonicalRoot 'PS2-DriveForge.exe') `
    (Join-Path $UserRoot 'legacy\PS2-DriveForge-Win32.exe')

# Developer/advanced utilities are available but no longer clutter the root.
$Tools = Join-Path $UserRoot 'tools'
New-Item -ItemType Directory -Force -Path $Tools | Out-Null
foreach ($Tool in @(
    'PS2-DriveForge-Mount.exe',
    'ps2-driveforge-inspect.exe',
    'ps2-driveforge-benchmark.exe'
)) {
    $Source = Join-Path $CanonicalRoot $Tool
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        Copy-Item -LiteralPath $Source -Destination (Join-Path $Tools $Tool) -Force
    }
}

# Keep detailed documentation accessible without exposing regression executables.
$CanonicalDocs = Join-Path $CanonicalRoot 'docs'
if (Test-Path -LiteralPath $CanonicalDocs -PathType Container) {
    $DocsDestination = Join-Path $UserRoot 'docs'
    New-Item -ItemType Directory -Force -Path $DocsDestination | Out-Null
    Copy-Item -LiteralPath (Join-Path $CanonicalDocs '*') -Destination $DocsDestination -Recurse -Force
}

# Release-layout verification. This is intentionally separate from the canonical
# CI package verifier because the end-user package should NOT contain test EXEs.
$requiredUserFiles = @(
    'PS2-DriveForge.exe',
    'legacy\PS2-DriveForge-Win32.exe',
    'app\winui\PS2-DriveForge-WinUI.exe',
    'app\winui\Microsoft.UI.Xaml.dll',
    'README.md',
    'CHANGELOG.md'
)
foreach ($Relative in $requiredUserFiles) {
    Require-File (Join-Path $UserRoot $Relative)
}

$runtimeDlls = @(
    Get-ChildItem -LiteralPath (Join-Path $UserRoot 'app\winui') -Recurse -File |
        Where-Object { $_.Name -like 'Microsoft.WindowsAppRuntime*.dll' }
)
if ($runtimeDlls.Count -eq 0) {
    throw 'User release is missing Microsoft.WindowsAppRuntime*.dll in app\winui.'
}

$leakedTests = @(
    Get-ChildItem -LiteralPath $UserRoot -Recurse -File |
        Where-Object { $_.Name -like 'ps2-driveforge*-tests.exe' }
)
if ($leakedTests.Count -ne 0) {
    throw "User release contains regression test executables: $($leakedTests.Name -join ', ')"
}

if (Test-Path -LiteralPath $PortableZip) {
    Remove-Item -LiteralPath $PortableZip -Force
}
Compress-Archive -LiteralPath (Join-Path $UserRoot '*') -DestinationPath $PortableZip -CompressionLevel Optimal
Write-Host "Portable package: $PortableZip" -ForegroundColor Green
Write-Host "Portable SHA-256: $((Get-FileHash -LiteralPath $PortableZip -Algorithm SHA256).Hash)"

if ($InnoCompiler) {
    Require-File $InnoCompiler
    if ($DokanyMsi) {
        Require-File $DokanyMsi
    }

    $InnoScript = Join-Path $Root 'installer\PS2DriveForge.iss'
    Require-File $InnoScript

    if (Test-Path -LiteralPath $SetupExe) {
        Remove-Item -LiteralPath $SetupExe -Force
    }

    $Arguments = @(
        "/DReleaseRoot=$UserRoot",
        "/DOutputDir=$DistRoot",
        "/DAppVersion=$Version"
    )
    if ($DokanyMsi) {
        $Arguments += "/DDokanyMsi=$([System.IO.Path]::GetFullPath($DokanyMsi))"
    }
    $Arguments += $InnoScript

    & $InnoCompiler @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup compiler failed with exit code $LASTEXITCODE"
    }
    Require-File $SetupExe
    Write-Host "Installer: $SetupExe" -ForegroundColor Green
    Write-Host "Installer SHA-256: $((Get-FileHash -LiteralPath $SetupExe -Algorithm SHA256).Hash)"
}

Write-Host "User-facing release staging verified: $UserRoot" -ForegroundColor Green
Write-Host "WinUI runtime files hidden under app\\winui: $(@(Get-ChildItem -LiteralPath (Join-Path $UserRoot 'app\winui') -Recurse -File).Count)"
