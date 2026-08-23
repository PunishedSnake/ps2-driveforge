[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$SkipWinUI,
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

function Require-Command {
    param([Parameter(Mandatory = $true)][string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Required command was not found in PATH: $Name"
    }
    return $command.Source
}

function Resolve-WinUIOutput {
    param(
        [Parameter(Mandatory = $true)][string]$SearchRoot,
        [Parameter(Mandatory = $true)][string]$ExecutableName
    )

    if (-not (Test-Path $SearchRoot)) {
        throw "WinUI output root was not produced: $SearchRoot"
    }

    $candidates = @(
        Get-ChildItem -Path $SearchRoot -Recurse -File -Filter $ExecutableName |
            Sort-Object FullName
    )

    if ($candidates.Count -eq 0) {
        throw "Expected self-contained WinUI executable was not produced below: $SearchRoot"
    }
    if ($candidates.Count -gt 1) {
        $paths = $candidates.FullName -join [Environment]::NewLine
        throw "Multiple WinUI executable candidates were produced; staging would be ambiguous:`n$paths"
    }

    return $candidates[0]
}

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$RegressionManifest = Join-Path $Root 'scripts\frieren-regression-tests.ps1'
if (-not (Test-Path -LiteralPath $RegressionManifest -PathType Leaf)) {
    throw "Frieren regression manifest is missing: $RegressionManifest"
}
. $RegressionManifest

$BuildDir = Join-Path $Root 'build\windows-x64'
$DistDir = Join-Path $Root 'dist\windows-x64'
$WinUIRoot = Join-Path $Root 'src\winui'
$WinUIConfiguration = if ($Configuration -eq 'Debug') { 'Debug' } else { 'Release' }
$WinUIOutputRoot = Join-Path $WinUIRoot "x64\$WinUIConfiguration"
$WinUIPayloadDir = $null

$CMake = Require-Command 'cmake'
$CTest = Require-Command 'ctest'
if (-not $SkipWinUI) {
    $NuGet = Require-Command 'nuget'
    $MSBuild = Require-Command 'msbuild'
}

Write-Host 'PS2 DriveForge - Windows x64 build' -ForegroundColor Cyan
Write-Host "Configuration: $Configuration"
Write-Host "Dokany mount:  $WithDokany"
Write-Host "WinUI frontend: $(-not $SkipWinUI)"
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
    if (-not $SkipWinUI) {
        foreach ($Path in @(
            (Join-Path $WinUIRoot 'x64'),
            (Join-Path $WinUIRoot 'Generated Files')
        )) {
            if (Test-Path $Path) {
                Remove-Item -Recurse -Force $Path
            }
        }
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

Write-Host "`n[1/6] Configuring native DriveForge with Visual Studio 2022..." -ForegroundColor Yellow
Invoke-Native -FilePath $CMake -Arguments $ConfigureArgs

Write-Host "`n[2/6] Building native core/host/frontends..." -ForegroundColor Yellow
Invoke-Native -FilePath $CMake -Arguments @('--build', $BuildDir, '--config', $Configuration, '--parallel')

if (-not $SkipTests) {
    Write-Host "`n[3/6] Running native tests..." -ForegroundColor Yellow
    Invoke-Native -FilePath $CTest -Arguments @('--test-dir', $BuildDir, '-C', $Configuration, '--output-on-failure')
} else {
    Write-Host "`n[3/6] Native tests skipped." -ForegroundColor DarkYellow
}

if (-not $SkipWinUI) {
    Write-Host "`n[4/6] Restoring Windows App SDK / C++WinRT packages..." -ForegroundColor Yellow
    $WinUIPackages = Join-Path $WinUIRoot 'packages'
    New-Item -ItemType Directory -Force -Path $WinUIPackages | Out-Null
    Invoke-Native -FilePath $NuGet -Arguments @(
        'install', 'Microsoft.WindowsAppSDK',
        '-Version', '2.4.0',
        '-DependencyVersion', 'Lowest',
        '-OutputDirectory', $WinUIPackages,
        '-NonInteractive',
        '-Source', 'https://api.nuget.org/v3/index.json'
    )
    Invoke-Native -FilePath $NuGet -Arguments @(
        'install', 'Microsoft.Windows.CppWinRT',
        '-Version', '3.0.260715.1',
        '-OutputDirectory', $WinUIPackages,
        '-NonInteractive',
        '-Source', 'https://api.nuget.org/v3/index.json'
    )

    Write-Host "`n[5/6] Building self-contained WinUI frontend..." -ForegroundColor Yellow
    $WinUIProject = Join-Path $WinUIRoot 'PS2DriveForge.WinUI.vcxproj'
    Invoke-Native -FilePath $MSBuild -Arguments @(
        $WinUIProject,
        '/m',
        "/p:Configuration=$WinUIConfiguration",
        '/p:Platform=x64'
    )

    $WinUIExe = Resolve-WinUIOutput -SearchRoot $WinUIOutputRoot -ExecutableName 'PS2-DriveForge-WinUI.exe'
    $WinUIPayloadDir = Split-Path -Parent $WinUIExe.FullName
    Write-Host "WinUI payload: $WinUIPayloadDir"
} else {
    Write-Host "`n[4/6] WinUI package restore skipped." -ForegroundColor DarkYellow
    Write-Host "`n[5/6] WinUI build skipped." -ForegroundColor DarkYellow
}

Write-Host "`n[6/6] Packaging Frieren development payload..." -ForegroundColor Yellow
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

$BinDir = Join-Path $BuildDir $Configuration
$InspectorExe = Join-Path $BinDir 'ps2-driveforge-inspect.exe'
$BenchmarkExe = Join-Path $BinDir 'ps2-driveforge-benchmark.exe'
$HdlToolsExe = Join-Path $BinDir 'ps2-driveforge-hdl-tools.exe'
$PfsToolsExe = Join-Path $BinDir 'ps2-driveforge-pfs-tools.exe'
$PhysicalToolsExe = Join-Path $BinDir 'ps2-driveforge-physical-tools.exe'
$GuiExe = Join-Path $BinDir 'PS2-DriveForge.exe'
# Physical mutation is part of the Windows Frieren contract. Keep the developer
# tool mandatory in staging so a packaging regression cannot silently ship the
# low-level backend while omitting the only deliberately gated way to exercise it.
$RequiredExecutables = @(
    $InspectorExe,
    $BenchmarkExe,
    $HdlToolsExe,
    $PfsToolsExe,
    $PhysicalToolsExe,
    $GuiExe
)
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

if (-not $SkipWinUI) {
    if (-not $WinUIPayloadDir -or -not (Test-Path $WinUIPayloadDir)) {
        throw 'Resolved WinUI payload directory disappeared before packaging.'
    }
    $WinUIDist = Join-Path $DistDir 'WinUI'
    New-Item -ItemType Directory -Force -Path $WinUIDist | Out-Null
    Copy-Item (Join-Path $WinUIPayloadDir '*') $WinUIDist -Recurse -Force
}

$PdbNames = @(
    'ps2-driveforge-inspect.pdb',
    'ps2-driveforge-benchmark.pdb',
    'ps2-driveforge-hdl-tools.pdb',
    'ps2-driveforge-pfs-tools.pdb',
    'ps2-driveforge-physical-tools.pdb',
    'PS2-DriveForge.pdb'
)
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
    foreach ($TestName in $FrierenRegressionTests) {
        $TestExe = Join-Path $BinDir $TestName
        if (-not (Test-Path -LiteralPath $TestExe -PathType Leaf)) {
            throw "Canonical Frieren regression executable was not produced: $TestExe"
        }
        Copy-Item $TestExe $DistDir -Force
    }
}

foreach ($Doc in @('README.md', 'CHANGELOG.md', 'BUILDING.md', 'CONTRIBUTING.md')) {
    $Source = Join-Path $Root $Doc
    if (-not (Test-Path $Source -PathType Leaf)) {
        throw "Required release documentation is missing: $Source"
    }
    Copy-Item $Source $DistDir -Force
}

$DocsSource = Join-Path $Root 'docs'
$ValidationDir = Join-Path $DistDir 'docs'
if (-not (Test-Path $DocsSource -PathType Container)) {
    throw "Documentation directory is missing: $DocsSource"
}
New-Item -ItemType Directory -Force -Path $ValidationDir | Out-Null
$MarkdownDocs = @(Get-ChildItem -LiteralPath $DocsSource -File -Filter '*.md')
if ($MarkdownDocs.Count -eq 0) {
    throw "No Markdown documentation files were found in: $DocsSource"
}
foreach ($Doc in $MarkdownDocs) {
    Copy-Item $Doc.FullName $ValidationDir -Force
}

$ZipName = "PS2-DriveForge-0.6.0-Frieren-$Configuration-windows-x64.zip"
$ZipPath = Join-Path (Split-Path -Parent $DistDir) $ZipName
if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}
Compress-Archive -Path (Join-Path $DistDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal