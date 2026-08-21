[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$WithDokany,
    [string]$DokanyRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $Root 'build\windows-x64'
$DistDir = Join-Path $Root 'dist\windows-x64'

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw @'
CMake was not found in PATH.
Install Visual Studio 2022 with the "Desktop development with C++" workload,
including MSVC, a Windows 10/11 SDK, and CMake tools for Windows.
'@
}

Write-Host 'PS2 DriveForge - Windows x64 build' -ForegroundColor Cyan
Write-Host "Configuration: $Configuration"
Write-Host "Dokany mount:  $WithDokany"
Write-Host "Build dir:     $BuildDir"
Write-Host "Output dir:    $DistDir"

if ($Clean) {
    if (Test-Path $BuildDir) {
        Write-Host 'Removing previous Windows build...'
        Remove-Item -Recurse -Force $BuildDir
    }
    if (Test-Path $DistDir) {
        Remove-Item -Recurse -Force $DistDir
    }
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$BuildTests = if ($SkipTests) { 'OFF' } else { 'ON' }
$BuildDokany = if ($WithDokany) { 'ON' } else { 'OFF' }
$ConfigureArgs = @(
    '-S', $Root,
    '-B', $BuildDir,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-DPS2DF_BUILD_TESTS=$BuildTests",
    "-DPS2DF_BUILD_DOKANY=$BuildDokany"
)
if ($DokanyRoot) {
    $ConfigureArgs += "-DDOKANY_ROOT=$DokanyRoot"
}

Write-Host "`n[1/4] Configuring with Visual Studio 2022..." -ForegroundColor Yellow
Invoke-Native -FilePath cmake -Arguments $ConfigureArgs

Write-Host "`n[2/4] Building..." -ForegroundColor Yellow
Invoke-Native -FilePath cmake -Arguments @('--build', $BuildDir, '--config', $Configuration, '--parallel')

if (-not $SkipTests) {
    Write-Host "`n[3/4] Running tests..." -ForegroundColor Yellow
    Invoke-Native -FilePath ctest -Arguments @('--test-dir', $BuildDir, '-C', $Configuration, '--output-on-failure')
} else {
    Write-Host "`n[3/4] Tests skipped." -ForegroundColor DarkYellow
}

Write-Host "`n[4/4] Packaging..." -ForegroundColor Yellow
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

$BinDir = Join-Path $BuildDir $Configuration
$InspectorExe = Join-Path $BinDir 'ps2-driveforge-inspect.exe'
$BenchmarkExe = Join-Path $BinDir 'ps2-driveforge-benchmark.exe'
$GuiExe = Join-Path $BinDir 'PS2-DriveForge.exe'
$RequiredExecutables = @($InspectorExe, $BenchmarkExe, $GuiExe)
$MountExe = Join-Path $BinDir 'PS2-DriveForge-Mount.exe'
if ($WithDokany) {
    $RequiredExecutables += $MountExe
}
foreach ($Required in $RequiredExecutables) {
    if (-not (Test-Path $Required)) {
        throw "Expected executable was not produced: $Required"
    }
    Copy-Item $Required $DistDir -Force
}

$PdbNames = @('ps2-driveforge-inspect.pdb', 'ps2-driveforge-benchmark.pdb', 'PS2-DriveForge.pdb')
if ($WithDokany) {
    $PdbNames += 'PS2-DriveForge-Mount.pdb'
}
foreach ($PdbName in $PdbNames) {
    $Pdb = Join-Path $BinDir $PdbName
    if (Test-Path $Pdb) {
        Copy-Item $Pdb $DistDir -Force
    }
}

if (-not $SkipTests) {
    foreach ($TestName in @(
        'ps2-driveforge-tests.exe',
        'ps2-driveforge-pfs-file-tests.exe',
        'ps2-driveforge-pfs-segi-tests.exe',
        'ps2-driveforge-host-tests.exe',
        'ps2-driveforge-e2e-image-tests.exe',
        'ps2-driveforge-corruption-tests.exe',
        'ps2-driveforge-session-tests.exe',
        'ps2-driveforge-read-cache-tests.exe',
        'ps2-driveforge-read-ahead-tests.exe',
        'ps2-driveforge-partition-catalog-tests.exe',
        'ps2-driveforge-storage-profile-tests.exe',
        'ps2-driveforge-dokany-open-policy-tests.exe',
        'ps2-driveforge-darkness-policy-tests.exe'
    )) {
        $TestExe = Join-Path $BinDir $TestName
        if (Test-Path $TestExe) {
            Copy-Item $TestExe $DistDir -Force
        }
    }
}

foreach ($Doc in @('README.md', 'CHANGELOG.md')) {
    $Source = Join-Path $Root $Doc
    if (Test-Path $Source) {
        Copy-Item $Source $DistDir -Force
    }
}

$ValidationDir = Join-Path $DistDir 'docs'
New-Item -ItemType Directory -Force -Path $ValidationDir | Out-Null
foreach ($Doc in @(
    'docs\testing.md',
    'docs\REAL_HARDWARE_VALIDATION.md',
    'docs\darkness-plan.md',
    'docs\emilia-plan.md',
    'docs\performance.md'
)) {
    $Source = Join-Path $Root $Doc
    if (Test-Path $Source) {
        Copy-Item $Source $ValidationDir -Force
    }
}

$ZipName = "PS2-DriveForge-0.5.0-Emilia-$Configuration-windows-x64.zip"
$ZipPath = Join-Path (Split-Path -Parent $DistDir) $ZipName
if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}
Compress-Archive -Path (Join-Path $DistDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Host "`nBuild completed successfully." -ForegroundColor Green
Write-Host "GUI:        $GuiExe"
Write-Host "Inspector:  $InspectorExe"
Write-Host "Benchmark:  $BenchmarkExe"
if ($WithDokany) {
    Write-Host "Mount:      $MountExe"
}
Write-Host "Package:    $ZipPath"
Write-Host "`nPhysical-drive access remains read-only in this development version." -ForegroundColor Green
if ($WithDokany) {
    Write-Host "Dokany 2.x runtime/driver must be installed on systems using the mount frontend." -ForegroundColor Green
}
