# Third-party notices

PS2 DriveForge original source code is licensed under the MIT License in [`LICENSE`](LICENSE). This file documents independently licensed software that DriveForge uses, redistributes, or relies on as a format/interoperability reference.

This notice is informational and does not replace the license text shipped by each upstream project.

## Dokany 2.3.1

DriveForge uses Dokany to expose its read-only virtual filesystem to Windows Explorer.

Upstream: https://github.com/dokan-dev/dokany  
Version used by current Windows builds: **2.3.1.1000**

Dokany documents a mixed licensing model:

- `dokan2.dll` - GNU Lesser General Public License v3;
- `dokan2.sys` - GNU Lesser General Public License v3;
- `dokannp2.dll` - GNU Lesser General Public License v3;
- `dokanfuse2.dll` - GNU Lesser General Public License v3;
- Dokan installer components - GNU Lesser General Public License v3;
- `dokanctl.exe` and samples - MIT License.

DriveForge links to Dokany through its normal shared-library mechanism and does not modify the Dokany runtime/driver. The Windows installer may carry the official upstream x64 MSI as a prerequisite.

Corresponding source and licenses:

- https://github.com/dokan-dev/dokany/tree/v2.3.1.1000
- https://github.com/dokan-dev/dokany/blob/v2.3.1.1000/license.lgpl.txt
- https://github.com/dokan-dev/dokany/blob/v2.3.1.1000/license.mit.txt

The exact upstream MIT notice is also preserved at `third_party/licenses/dokany/license.mit.txt`. DriveForge does not hand-transcribe Dokany LGPL text; public packaging preserves the applicable upstream license material.

## Microsoft Windows App SDK / WinUI / C++/WinRT

The WinUI frontend is built with Microsoft Windows App SDK, WinUI 3 and C++/WinRT packages. The Windows App SDK source repository is MIT-licensed, while redistributed binary/NuGet payloads remain subject to the terms shipped with the relevant Microsoft packages.

Main upstream project:

https://github.com/microsoft/WindowsAppSDK

Current Frieren development uses Windows App SDK **2.4.0** and the corresponding resolved WinUI/runtime dependencies.

### Historical RC5 licensing issue

An earlier private Emilia RC5 build used Windows App SDK 2.3.1 and resolved `Microsoft.WindowsAppSDK.WinUI` 2.3.0. Microsoft later confirmed that several WinUI packages, including 2.3.0 and 2.3.2, carried an incorrect Engineering Preview license and directed affected applications to Windows App SDK 2.4.0, which resolves an unaffected WinUI package.

Upstream resolution:

https://github.com/microsoft/WindowsAppSDK/issues/6654#issuecomment-5287791268

That old RC remains historical validation evidence and is not the dependency state described by current Frieren builds. License notes should follow the package we actually ship, not the package we once argued with.

## Microsoft WebView2 / Windows SDK build components

Windows App SDK native dependencies may bring in Microsoft WebView2 metadata/build assets and Windows SDK build packages. These components remain subject to Microsoft package license terms. DriveForge does not relicense them under MIT.

## Inno Setup

DriveForge uses Inno Setup to build the recommended Windows installer.

Upstream: https://github.com/jrsoftware/issrc  
Project: https://jrsoftware.org/isinfo.php

Inno Setup is Copyright (C) 1997-2026 Jordan Russell, with portions Copyright (C) 2000-2026 Martijn Laan. The compiler is a build-time dependency; DriveForge does not claim authorship of it.

## PS2SDK

Upstream: https://github.com/ps2dev/ps2sdk

PS2SDK is a primary public reference for APA/PFS behavior used while implementing and validating DriveForge independent format code. The repository is distributed under the Academic Free License 2.0, with individual components potentially carrying additional notices.

DriveForge does not bundle PS2SDK in its Windows release.

## pfsshell

Upstream: https://github.com/ps2homebrew/pfsshell

pfsshell is an established host-side APA/PFS tool used as a compatibility/reference implementation. The project documents GPLv2 licensing for the project as a whole and AFLv2 licensing for its APA/PFS/iomanX libraries.

DriveForge does not bundle pfsshell and does not present pfsshell code as DriveForge code.

## hdl-dump / hdl_dumb

Upstream: https://github.com/ps2homebrew/hdl-dump

hdl-dump is a long-standing HDLoader/APA tool used as a format/interoperability reference. It contains GPL-licensed source and upstream copyright notices. DriveForge does not bundle hdl-dump.

## Open PS2 Loader

Upstream: https://github.com/ps2homebrew/Open-PS2-Loader

Open PS2 Loader is an actively maintained real-world consumer of APA/PFS and HDLoader HDD layouts. It is an interoperability/reference target and is not bundled with DriveForge.

## Runtime OPL metadata and asset providers

Frieren can fetch optional OPL metadata/assets at runtime from third-party public repositories/services. DriveForge stages provider data on demand rather than bundling whole databases into the application. This is deliberate: a URL is not a license grant, however convenient it would be if the Internet worked that way.

Current provider/reference projects include:

- `israpps/HDL-Batch-installer` title/Redump data;
- `israpps/PS2-OPL-CFG-Database` CFG data, GPL-3.0;
- `GDX-X/PS2-OPL-CFG-Compatibility-Database` compatibility CFG overlays;
- `PS2-Widescreen/OPL-Widescreen-Cheats` widescreen CHT data;
- `PS2-Widescreen/Bare-Mastercodes-bin` mastercode fallback data, Unlicense;
- `Luden02/psx-ps2-opl-art-database` artwork source;
- Archive.org OPL artwork collections used as fallback sources;
- `CosmicScale/HDD-OSD-Icon-Database` optional HDD-OSD icons.

Provider code preserves source/provenance in staged results. Data without a clear redistribution license is fetched at runtime rather than incorporated into DriveForge release archives. Artwork and game-related marks remain the property of their respective owners.

## FHDB Manager

FHDB Manager is the companion PS2-side recovery/rescue tool developed under the same project ownership in the historical `PunishedSnake/fhdb-bootstrap-manager` repository. It is listed here as an interoperability source rather than a third-party dependency.

DriveForge mirrors shared `PS2HBRC`, `HDDMBR`, `HDDRAW`, `APAMETA1/HDDMETA` and `FORENSIC.TXT` semantics. The DriveForge-private `PS2DFRC1` Mutation Journal is not an FHDB artifact.

## Trademarks

PlayStation, PlayStation 2, PS2, Windows and other names/logos mentioned by the project are trademarks of their respective owners. Reference to a project or trademark is for interoperability, attribution or dependency identification and does not imply endorsement.
