# Credits and acknowledgements

PS2 DriveForge is an independent homebrew project, but it exists in an ecosystem built by people who spent years documenting, implementing and testing PlayStation 2 storage formats.

## Project

- **PunishedSnake** — PS2 DriveForge project owner, design, implementation, hardware validation and release engineering.
- Contributors are credited through the repository's Git history and pull requests.

## PS2 storage ecosystem

The DriveForge APA/PFS/HDLoader implementation is original project code, but format behavior and compatibility have been cross-checked against established public implementations and documentation, including:

- **PS2SDK** (`ps2dev/ps2sdk`) — `libapa`, `libpfs` and the wider PS2 homebrew SDK ecosystem. PS2SDK is distributed under the Academic Free License 2.0.
- **pfsshell** (`ps2homebrew/pfsshell`) — long-standing host-side APA/PFS browsing and transfer tool. The project contains GPLv2 code and AFLv2 APA/PFS/iomanX libraries.
- **hdl-dump / hdl_dumb** (`ps2homebrew/hdl-dump`) — HDLoader/APA tooling and a useful compatibility reference for HDL partition metadata.
- **Open PS2 Loader (OPL)** (`ps2homebrew/Open-PS2-Loader`) — actively maintained PS2 loader and an important real-world consumer of APA/PFS/HDLoader HDD layouts.

These projects are references and interoperability targets. Their mention does not imply endorsement of DriveForge, and DriveForge does not claim ownership of their code, names or trademarks.

## Windows stack

- **Dokan / Dokany contributors** — Windows user-mode filesystem stack used for DriveForge's read-only Explorer mount.
- **Microsoft Windows App SDK / WinUI / C++/WinRT contributors** — modern Windows frontend/runtime technologies used by the Emilia WinUI interface.
- **Inno Setup** by Jordan Russell, Martijn Laan and contributors — installer toolchain used to build the normal Windows setup executable.
- **CMake, LLVM/Clang and Microsoft Visual C++ toolchains** — build, sanitizer and compiler infrastructure used by the project.

Legal/license details for redistributed dependencies live in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). This file is an acknowledgement page rather than a substitute for those license terms.

## Hardware validation

The project deliberately treats real-disk testing as part of engineering rather than an afterthought. Real PS2 APA/PFS/HDL disks, disk images and generated corruption fixtures have been used to verify assumptions that are easy to get wrong from source reading alone, especially APA sub-partition addressing, PFS zone/inode addressing, Explorer read patterns and Windows physical-disk discovery.

## Trademarks

PlayStation, PlayStation 2, PS2 and related marks are trademarks of their respective owners. PS2 DriveForge is not affiliated with or endorsed by Sony Interactive Entertainment.
