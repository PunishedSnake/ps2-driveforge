[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [switch]$Clean,
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Version = '0.2.0'
$Codename = 'Bocchi'

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

Write-Host "PS2 DriveForge $Version-dev ($Codename) - Windows x64 build" -ForegroundColor Cyan
Write-Host "Configuration: $Configuration"
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

Write-Host "`n[1/4] Configuring with Visual Studio 2022..." -ForegroundColor Yellow
Invoke-Native -FilePath cmake -Arguments @(
    '-S', $Root,
    '-B', $BuildDir,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-DPS2DF_BUILD_TESTS=$BuildTests"
)

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
if (-not (Test-Path $InspectorExe)) {
    throw "Expected executable was not produced: $InspectorExe"
}
Copy-Item $InspectorExe $DistDir -Force

$InspectorPdb = Join-Path $BinDir 'ps2-driveforge-inspect.pdb'
if (Test-Path $InspectorPdb) {
    Copy-Item $InspectorPdb $DistDir -Force
}

if (-not $SkipTests) {
    foreach ($TestBinary in @('ps2-driveforge-tests', 'ps2-driveforge-pfs-tests')) {
        foreach ($Extension in @('.exe', '.pdb')) {
            $Path = Join-Path $BinDir ($TestBinary + $Extension)
            if (Test-Path $Path) {
                Copy-Item $Path $DistDir -Force
            }
        }
    }
}

foreach ($Doc in @('README.md', 'CHANGELOG.md')) {
    $Source = Join-Path $Root $Doc
    if (Test-Path $Source) {
        Copy-Item $Source $DistDir -Force
    }
}

$ZipName = "PS2-DriveForge-$Version-$Codename-$Configuration-windows-x64.zip"
$ZipPath = Join-Path (Split-Path -Parent $DistDir) $ZipName
if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}
Compress-Archive -Path (Join-Path $DistDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Host "`nBuild completed successfully." -ForegroundColor Green
Write-Host "Executable: $InspectorExe"
Write-Host "Package:    $ZipPath"
Write-Host "`nPhysical-drive and PFS access remain read-only in this development version." -ForegroundColor Green
