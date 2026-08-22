# Building PS2 DriveForge

PS2 DriveForge is a C++20 project with a portable parser/host stack and Windows-only frontends. The normal Windows release build is intentionally driven by `build-windows.ps1`; WinUI remains an MSBuild/C++/WinRT project because Windows App SDK XAML generation is MSBuild-oriented.

## Supported build shapes

| Build | Core/host | Tests | Win32 GUI | WinUI 3 | Dokany mount |
| --- | --- | --- | --- | --- | --- |
| Windows x64 release | yes | yes | yes | yes | optional/normal release | 
| Linux developer/CI | yes | yes | no | no | no |
| Clang sanitizer CI | yes | yes | no | no | no |

The current WinUI target uses Windows App SDK **2.3.1** and Microsoft.Windows.CppWinRT **3.0.260715.1**. The read-only mount target is validated against Dokany **2.3.1**.

## Windows prerequisites

Install:

- Windows 10 2004 (19041) or newer; Windows 11 is the primary validation host;
- Visual Studio 2022 with Desktop development with C++ and a current Windows 10/11 SDK;
- CMake 3.24 or newer;
- PowerShell 7 or Windows PowerShell capable of running the build script;
- NuGet CLI available in `PATH`;
- MSBuild available in `PATH` (a Developer PowerShell/Command Prompt is sufficient);
- Dokany 2.3.1 development files when building the Explorer mount frontend.

The CI workflow installs the official Dokany 2.3.1 x64 MSI and verifies SHA-256 before using it. A local installation must expose `dokan.h` and `dokan2.lib` under its SDK root.

## Full Windows release build

From the repository root in an x64-capable Visual Studio developer shell:

```powershell
.\build-windows.ps1 `
  -Configuration Release `
  -Clean `
  -WithDokany `
  -DokanyRoot 'C:\Program Files\Dokan\DokanLibrary-2.3.1'
```

The script performs these stages:

1. configures the CMake native core/host/frontends;
2. builds native targets;
3. runs all normal regression tests;
4. restores pinned Windows App SDK/C++WinRT packages;
5. builds the self-contained unpackaged WinUI frontend;
6. stages the canonical Windows package and creates the release ZIP.

The script discovers the actual MSBuild WinUI output directory below `src\winui\x64\<Configuration>` rather than assuming a fixed project-subdirectory layout.

Expected release archive:

```text
dist\PS2-DriveForge-0.5.0-Emilia-Release-windows-x64.zip
```

Canonical staging directory:

```text
dist\windows-x64\
  PS2-DriveForge.exe
  PS2-DriveForge-Mount.exe
  ps2-driveforge-inspect.exe
  ps2-driveforge-benchmark.exe
  WinUI\
    PS2-DriveForge-WinUI.exe
    ... self-contained WinUI payload ...
  docs\
  README.md
  CHANGELOG.md
  ... regression executables ...
```

Verify the staging directory explicitly with:

```powershell
.\scripts\verify-windows-package.ps1
```

The verifier checks the required release binaries/docs, exactly 14 packaged regression executables, and a non-empty self-contained WinUI payload.

## Useful Windows variants

Skip tests while iterating on packaging/UI only:

```powershell
.\build-windows.ps1 -Configuration Release -SkipTests
```

Skip WinUI while iterating on portable/native code:

```powershell
.\build-windows.ps1 -Configuration Release -SkipWinUI
```

Build without Dokany:

```powershell
.\build-windows.ps1 -Configuration Release
```

If verifying such a deliberately reduced staging tree manually, use the matching verifier switches:

```powershell
.\scripts\verify-windows-package.ps1 -WithoutDokany
.\scripts\verify-windows-package.ps1 -WithoutWinUI
```

A release candidate must use the full build with tests, Dokany and WinUI enabled.

## Linux / portable build

Linux builds the storage/parser/host stack and portable regression tests only:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2DF_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Dokany and WinUI are Windows-only and must not become dependencies of `ps2driveforge_core` or portable host tests.

## Sanitizer configuration

The CI sanitizer job is the authoritative configuration. A representative local Clang build is:

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

Turn interesting fuzzer/hardware failures into deterministic tests before considering the bug fixed.

## Build failures worth treating as real failures

Do not bypass:

- missing Windows App SDK/C++WinRT imports;
- missing Dokany headers/import library in a mount build;
- package verifier failures;
- warnings-as-errors failures under Clang CI;
- any of the 14 regression targets;
- generated-image SHA-256 mismatches;
- corruption tests that unexpectedly accept malformed metadata.

A release package that merely contains an EXE is not sufficient. The canonical staging verifier exists specifically to catch incomplete or incorrectly staged frontend payloads.
