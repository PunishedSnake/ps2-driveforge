# Building PS2 DriveForge

PS2 DriveForge is a C++20 project with a portable APA/PFS/HDL core, a shared host layer and Windows-only desktop frontends. The normal Windows build is driven by `build-windows.ps1`; WinUI remains an MSBuild/C++/WinRT project because Windows App SDK XAML generation and deployment targets are MSBuild-oriented.

## Supported build shapes

| Build | Core/host | Tests | Win32 | Launcher | WinUI 3 | Dokany |
| --- | --- | --- | --- | --- | --- | --- |
| Windows x64 release/RC | yes | yes | yes | yes | yes | yes |
| Windows developer | yes | optional | yes | yes | optional | optional |
| Linux developer/CI | yes | yes | no | no | no | no |
| Clang sanitizer CI | yes | yes | no | no | no | no |

The private RC5 WinUI project resolves Windows App SDK **2.3.1** and Microsoft.Windows.CppWinRT **3.0.260715.1**. Explorer mounting is validated against Dokany **2.3.1**. Before a public 0.5 candidate, Windows App SDK must be upgraded to **2.4.0 or later with an unaffected resolved WinUI package**, as documented under the publication gate below.

## Windows prerequisites

Install or provide:

- Windows 10 2004 (19041) or newer; Windows 11 is the primary validation target;
- Visual Studio 2022 with **Desktop development with C++** and a current Windows SDK;
- CMake 3.24+;
- PowerShell;
- NuGet CLI in `PATH`;
- MSBuild in `PATH` (Developer PowerShell/Command Prompt is sufficient);
- Dokany 2.3.1 SDK/runtime for full Explorer-mount builds.

For user-facing installer generation, CI also uses **Inno Setup 6.7.3**. The workflow downloads the official installer and verifies its SHA-256 before running it.

## Full Windows build

From the repository root in a Visual Studio developer shell:

```powershell
.\build-windows.ps1 `
  -Configuration Release `
  -Clean `
  -WithDokany `
  -DokanyRoot 'C:\Program Files\Dokan\DokanLibrary-2.3.1'
```

The script:

1. configures the native CMake targets;
2. builds core, host, Win32, launcher, CLI/benchmark and Dokany targets;
3. runs all 14 normal regression executables;
4. restores pinned Windows App SDK/C++WinRT packages;
5. builds the unpackaged WinUI frontend;
6. verifies and stages the canonical developer/release payload.

The WinUI output directory is discovered from the actual MSBuild result below `src\winui\x64\<Configuration>` rather than assuming a flat output layout.

Verify canonical staging with:

```powershell
.\scripts\verify-windows-package.ps1
```

Canonical staging intentionally contains test executables and is **not** the clean end-user package.

## User-facing portable/setup packages

End-user staging is produced by:

```powershell
.\scripts\make-user-release.ps1 `
  -Version '0.5.0-rc5' `
  -InnoCompiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' `
  -DokanyMsi 'C:\path\to\Dokan_x64.msi'
```

The release script creates a clean tree:

```text
PS2-DriveForge.exe              native launcher
README.md
CHANGELOG.md
LICENSE
CREDITS.md
THIRD_PARTY_NOTICES.md
app\winui\                     modern frontend/runtime
legacy\PS2-DriveForge-Win32.exe
tools\
docs\
```

Regression executables remain in canonical CI staging but are deliberately excluded from the user package.

The Windows CI additionally executes the **exact staged launcher** with `--self-test` before ZIP/setup creation. This is a loader-level smoke test: a binary that links successfully but cannot start because of an invalid DLL/ordinal import fails packaging.

The launcher also supports:

```powershell
.\PS2-DriveForge.exe --legacy
```

which starts the supported Win32 fallback without entering WinUI.

## WinUI deployment and public-publication gate

The private RC5 branch uses `WindowsAppSDKSelfContained=true` so WinUI runtime files are copied below the app payload. The release verifier requires actual runtime DLLs (including `Microsoft.UI.Xaml.dll` and Windows App Runtime components), not merely the frontend EXE.

RC5 resolves Windows App SDK **2.3.1** and transitive `Microsoft.WindowsAppSDK.WinUI` **2.3.0**. Microsoft has confirmed that WinUI 2.3.0 is among the package versions affected by an incorrect Engineering Preview license and states that the issue is fixed in **Windows App SDK 2.4.0**, which resolves WinUI 2.3.6. Affected applications are instructed to upgrade to 2.4.0 before publishing.

Therefore the 2.3.1 self-contained RC5 payload is for **private hardware/UI validation only**. Before a public 0.5 candidate:

1. update the pinned Windows App SDK dependency to **2.4.0 or later**;
2. verify the resolved WinUI package is not one of the affected versions;
3. rebuild the self-contained/runtime layout from scratch;
4. re-run standalone WinUI and full Windows package CI;
5. re-run real-machine WinUI startup/launcher tests on the resulting artifact.

See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for the upstream issue/resolution and dependency notices.

## Useful Windows variants

Native/storage iteration without WinUI:

```powershell
.\build-windows.ps1 -Configuration Release -SkipWinUI
```

UI/packaging iteration without running the regression suite:

```powershell
.\build-windows.ps1 -Configuration Release -SkipTests
```

Build without Dokany:

```powershell
.\build-windows.ps1 -Configuration Release
```

Reduced package verification must explicitly match the intentionally omitted components:

```powershell
.\scripts\verify-windows-package.ps1 -WithoutDokany
.\scripts\verify-windows-package.ps1 -WithoutWinUI
```

A release candidate uses the full Windows build with tests, Dokany, both frontends, launcher smoke-test and user-package verification.

## Linux / portable build

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2DF_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows platform code, Dokany and WinUI must not become dependencies of `ps2driveforge_core` or portable host tests.

## Sanitizers

Representative Clang configuration:

```bash
cmake -S . -B build/sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DPS2DF_BUILD_TESTS=ON \
  -DPS2DF_WARNINGS_AS_ERRORS=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/sanitize --parallel
ctest --test-dir build/sanitize --output-on-failure
```

## Optional APA fuzzer

```bash
cmake -S . -B build/fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPS2DF_BUILD_TESTS=OFF \
  -DPS2DF_BUILD_FUZZERS=ON
cmake --build build/fuzz --target ps2-driveforge-fuzz-apa
./build/fuzz/ps2-driveforge-fuzz-apa
```

Turn useful fuzzer/hardware failures into deterministic regression tests before considering a parser bug fixed.

## Build failures that must not be bypassed

Treat all of these as real failures:

- missing Windows App SDK/C++WinRT imports;
- missing WinUI runtime payload in a self-contained build;
- missing Dokany headers/import library;
- any of the 14 regression targets failing;
- Clang sanitizer or `-Werror` failures;
- canonical or user-package verifier failures;
- staged launcher `--self-test` failure;
- generated-image SHA-256 mismatches;
- corruption fixtures unexpectedly being accepted.

A storage-tool release is not validated because an EXE exists. Build verification, package verification, dependency/publication review and real-HDD testing are separate gates by design.
