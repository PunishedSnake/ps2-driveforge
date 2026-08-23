[CmdletBinding()]
param(
    [string]$Version = '0.6.0-dev',
    [string]$Codename = 'Frieren',
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

if ([string]::IsNullOrWhiteSpace($Version) -or [string]::IsNullOrWhiteSpace($Codename)) {
    throw 'Version and Codename must both be non-empty.'
}
if ($Version.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
    $Codename.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) {
    throw 'Version/Codename contains characters that are invalid in package filenames.'
}

$Root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$CanonicalRoot = [System.IO.Path]::GetFullPath($CanonicalRoot)
$BuildBin = [System.IO.Path]::GetFullPath($BuildBin)
$DistRoot = Join-Path $Root 'dist'
$UserRoot = Join-Path $DistRoot 'user\windows-x64'
$PortableZip = Join-Path $DistRoot "PS2-DriveForge-$Version-$Codename-Portable-x64.zip"
$SetupExe = Join-Path $DistRoot "PS2-DriveForge-$Version-$Codename-Setup-x64.exe"

if (-not (Test-Path -LiteralPath $CanonicalRoot -PathType Container)) {
    throw "Canonical Windows staging was not found: $CanonicalRoot"
}

if (Test-Path -LiteralPath $UserRoot) {
    Remove-Item -LiteralPath $UserRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $UserRoot | Out-Null

$Launcher = Join-Path $BuildBin 'PS2-DriveForge-Launcher.exe'
Copy-RequiredFile $Launcher (Join-Path $UserRoot 'PS2-DriveForge.exe')

foreach ($Doc in @('README.md', 'CHANGELOG.md')) {
    Copy-RequiredFile (Join-Path $CanonicalRoot $Doc) (Join-Path $UserRoot $Doc)
}
foreach ($Doc in @('LICENSE', 'CREDITS.md', 'THIRD_PARTY_NOTICES.md')) {
    Copy-RequiredFile (Join-Path $Root $Doc) (Join-Path $UserRoot $Doc)
}

$CanonicalWinUI = Join-Path $CanonicalRoot 'WinUI'
if (-not (Test-Path -LiteralPath $CanonicalWinUI -PathType Container)) {
    throw "Canonical WinUI payload is missing: $CanonicalWinUI"
}
$WinUIDestination = Join-Path $UserRoot 'app\winui'
New-Item -ItemType Directory -Force -Path $WinUIDestination | Out-Null
Copy-Item -Path (Join-Path $CanonicalWinUI '*') -Destination $WinUIDestination -Recurse -Force

Copy-RequiredFile (Join-Path $CanonicalRoot 'PS2-DriveForge.exe') `
    (Join-Path $UserRoot 'legacy\PS2-DriveForge-Win32.exe')

$Tools = Join-Path $UserRoot 'tools'
New-Item -ItemType Directory -Force -Path $Tools | Out-Null
foreach ($Tool in @(
    'PS2-DriveForge-Mount.exe',
    'ps2-driveforge-inspect.exe',
    'ps2-driveforge-benchmark.exe',
    'ps2-driveforge-hdl-tools.exe',
    'ps2-driveforge-physical-tools.exe'
)) {
    $Source = Join-Path $CanonicalRoot $Tool
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        Copy-Item -LiteralPath $Source -Destination (Join-Path $Tools $Tool) -Force
    }
}

$CanonicalDocs = Join-Path $CanonicalRoot 'docs'
if (Test-Path -LiteralPath $CanonicalDocs -PathType Container) {
    $DocsDestination = Join-Path $UserRoot 'docs'
    New-Item -ItemType Directory -Force -Path $DocsDestination | Out-Null
    Copy-Item -Path (Join-Path $CanonicalDocs '*') -Destination $DocsDestination -Recurse -Force
}

$requiredUserFiles = @(
    'PS2-DriveForge.exe',
    'legacy\PS2-DriveForge-Win32.exe',
    'app\winui\PS2-DriveForge-WinUI.exe',
    'app\winui\Microsoft.UI.Xaml.dll',
    'tools\ps2-driveforge-hdl-tools.exe',
    'tools\ps2-driveforge-physical-tools.exe',
    'README.md',
    'CHANGELOG.md',
    'LICENSE',
    'CREDITS.md',
    'THIRD_PARTY_NOTICES.md'
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

$StagedLauncher = Join-Path $UserRoot 'PS2-DriveForge.exe'
$LauncherSmoke = Start-Process -FilePath $StagedLauncher -ArgumentList '--self-test' -Wait -PassThru
if ($LauncherSmoke.ExitCode -ne 0) {
    throw "Staged launcher smoke-test failed with exit code $($LauncherSmoke.ExitCode)."
}
Write-Host 'Staged launcher smoke-test: PASS' -ForegroundColor Green

if (Test-Path -LiteralPath $PortableZip) {
    Remove-Item -LiteralPath $PortableZip -Force
}
Compress-Archive -Path (Join-Path $UserRoot '*') -DestinationPath $PortableZip -CompressionLevel Optimal
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
        "/DAppVersion=$Version",
        "/DAppCodename=$Codename"
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

Write-Host "User-facing $Version $Codename staging verified: $UserRoot" -ForegroundColor Green
Write-Host "WinUI runtime files hidden under app\\winui: $(@(Get-ChildItem -LiteralPath (Join-Path $UserRoot 'app\winui') -Recurse -File).Count)"
