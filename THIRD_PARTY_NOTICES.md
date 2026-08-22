# Third-party notices

PS2 DriveForge's original source code is licensed under the MIT License in [`LICENSE`](LICENSE). This file documents independently licensed software that DriveForge uses, redistributes, or relies on as a format/interoperability reference.

This notice is informational and does not replace the license text shipped by each upstream project.

## Dokany 2.3.1

DriveForge uses Dokany to expose its read-only virtual filesystem to Windows Explorer.

Upstream: https://github.com/dokan-dev/dokany  
Version used by the Emilia Windows build: **2.3.1.1000**

Dokany documents a mixed licensing model:

- `dokan2.dll` — GNU Lesser General Public License v3;
- `dokan2.sys` — GNU Lesser General Public License v3;
- `dokannp2.dll` — GNU Lesser General Public License v3;
- `dokanfuse2.dll` — GNU Lesser General Public License v3;
- Dokan installer components — GNU Lesser General Public License v3;
- `dokanctl.exe` and samples — MIT License.

DriveForge links to Dokany through its normal shared-library mechanism and does not modify the Dokany runtime/driver. The Windows installer may carry the official upstream x64 MSI as a prerequisite. Corresponding Dokany source for the exact release is available from:

https://github.com/dokan-dev/dokany/tree/v2.3.1.1000

Upstream license files:

- https://github.com/dokan-dev/dokany/blob/v2.3.1.1000/license.lgpl.txt
- https://github.com/dokan-dev/dokany/blob/v2.3.1.1000/license.mit.txt

A public DriveForge release must preserve the applicable Dokany notices/license material in the distributed package.

## Microsoft Windows App SDK / WinUI / C++/WinRT

The Emilia WinUI frontend is built with Microsoft Windows App SDK, WinUI 3 and C++/WinRT packages. The source repositories include MIT-licensed components, while binary/NuGet redistribution is governed by the license terms shipped with the relevant Microsoft packages.

Main upstream project:

https://github.com/microsoft/WindowsAppSDK

The current development configuration resolves Windows App SDK **2.3.1** and WinUI package **2.3.0** plus their component dependencies.

### 2.3.x redistribution review

As of August 2026, Microsoft has an open Windows App SDK issue reporting that the stable WinUI 2.3.x NuGet package contains an "Engineering Preview" `license.txt` whose wording conflicts with normal redistribution of the self-contained payload:

https://github.com/microsoft/WindowsAppSDK/issues/6654

Because DriveForge currently builds a self-contained WinUI test payload, **public 0.5 release sign-off must not assume that this mismatch is harmless**. Before publishing the self-contained runtime, the project must do one of the following:

1. use Microsoft package/runtime terms that clearly permit the files being redistributed;
2. switch the public build to the official Windows App Runtime prerequisite/framework-dependent deployment model; or
3. use a Windows App SDK version whose package terms unambiguously cover the chosen deployment.

Internal/private RC hardware testing can continue while this is resolved, but the license review is a release gate.

## Microsoft WebView2 / Windows SDK build components

Windows App SDK's native dependency graph brings in Microsoft WebView2 metadata/build assets and Windows SDK build packages. These components remain subject to Microsoft's own package license terms. DriveForge does not relicense them under MIT.

## Inno Setup

DriveForge uses Inno Setup to build the recommended Windows installer.

Upstream: https://github.com/jrsoftware/issrc  
Project: https://jrsoftware.org/isinfo.php

Inno Setup is Copyright (C) 1997-2026 Jordan Russell, with portions Copyright (C) 2000-2026 Martijn Laan. Its license permits use for distributing products and states that acknowledgement in product documentation is appreciated but not required. DriveForge nevertheless credits it explicitly.

The compiler itself is a build-time dependency; DriveForge does not claim authorship of Inno Setup.

## PS2SDK

Upstream: https://github.com/ps2dev/ps2sdk

PS2SDK is an important primary reference for APA/PFS behavior used while implementing and validating DriveForge's independent reader. The PS2SDK repository is distributed under the Academic Free License 2.0, with individual files/components potentially carrying their own notices.

DriveForge does not bundle PS2SDK in its Windows release.

## pfsshell

Upstream: https://github.com/ps2homebrew/pfsshell

pfsshell is an established host-side APA/PFS tool used as a compatibility/reference implementation. The pfsshell project documents GPLv2 licensing for the project as a whole and AFLv2 licensing for its APA/PFS/iomanX libraries.

DriveForge does not bundle pfsshell and does not present pfsshell code as DriveForge code.

## hdl-dump / hdl_dumb

Upstream: https://github.com/ps2homebrew/hdl-dump

hdl-dump is a long-standing HDLoader/APA tool used as a format/interoperability reference. It contains GPL-licensed source and upstream copyright notices. DriveForge does not bundle hdl-dump.

## Open PS2 Loader

Upstream: https://github.com/ps2homebrew/Open-PS2-Loader

Open PS2 Loader is an actively maintained real-world consumer of APA/PFS and HDLoader HDD layouts. It is used as an interoperability/reference target; it is not bundled with DriveForge.

## Trademarks

PlayStation, PlayStation 2, PS2, Windows and other names/logos mentioned by the project are trademarks of their respective owners. Reference to a project or trademark is for interoperability, attribution or dependency identification and does not imply endorsement.
