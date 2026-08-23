# Host-side MagicGate in DriveForge

DriveForge contains an experimental host-side implementation of the disk-KELF
parts of PlayStation 2 MagicGate. The goal is not to emulate a memory card or a
whole MechaCon session. The goal is narrower and considerably more useful for
DriveForge: inspect, verify and eventually generate the KELF payloads used by
PS2 HDD bootstrap software entirely on the PC.

This distinction matters. A memory-card binding protocol, disk KELF signing and
raw HDD installation are three different problems. Combining them into one
`MagicGateThing` class would save several filenames and cost several years of
maintainer sanity.

## What came before

PC-side MagicGate work is not new. In particular:

- `ps3mca_tool` performs host-side KELF key transformations together with PS3
  Memory Card Adapter authentication and card storage-key operations;
- KELFTool implements PC-side PS2/PSX KELF encryption, decryption and signing.

DriveForge does not claim to be the first PC KELF signer. Its useful difference
is architectural: the MagicGate layer is being built as an in-process component
of a complete PS2 HDD management, recovery and bootstrap pipeline.

DriveForge is MIT licensed. KELFTool and several emulator/reference projects use
other licenses, including GPL. Their behavior and public format knowledge are
valuable verification sources, but their implementation code is not copied
into DriveForge. The DES/TDES, KELF parsing and signing code here is independently
implemented.

## Sources of truth

The implementation is cross-checked against several independent sources rather
than trusting one twenty-year-old comment forever:

1. PS2SDK SECRMAN is the primary oracle for KELF layout, MechaCon command flow,
   BIT geometry, Kbit/Kc placement and ICVPS2 placement.
2. `ps3mca_tool` is useful for separating disk Kbit/Kc wrapping from the later
   memory-card authentication/storage-key protocol.
3. KELFTool is a behavioral oracle for host-side signatures, BIT encryption and
   content transformations.
4. Standards-based DES known-answer vectors validate the primitive independently
   of the PS2 format code.

When sources disagree, DriveForge records the disagreement instead of choosing
whichever comment has the most confident punctuation.

## Layering

The host implementation is intentionally split into small pieces:

- `magicgate_cipher.hpp` - standard DES and two-key TDES block primitives;
- `magicgate_cbc.hpp` - bounded DES/TDES CBC over complete 8-byte blocks;
- `magicgate_kelf.hpp` - key-free KELF structure and offset inspection;
- `magicgate_disk_keys.hpp` / `magicgate_disk.hpp` - disk Kbit/Kc derivation,
  wrapping and unwrapping;
- `magicgate_keyset.hpp` - strict user-supplied keyset parser;
- `magicgate_signatures.hpp` - plaintext BIT model plus header, BIT and root
  signature primitives;
- `magicgate_content.hpp` - per-block content signature primitives;
- `magicgate_verify.hpp` - cryptographic verification of the KELF header
  envelope;
- `magicgate_payload.hpp` - payload decryption and per-block signature
  verification after the envelope has established flag semantics;
- `magicgate_sign.hpp` - self-verifying low-layout disk KELF construction.

No layer above the keyset parser owns Sony key material. DriveForge deliberately
ships no production MagicGate keys in source code, test data or release binaries.
A real keyset is an explicit local capability supplied by the user.

## KELF geometry used by SECRMAN

The fixed KELF header is 32 bytes:

```text
UserHeader[16]
ELF_size             u32
KELF_header_size     u16
system/application   2 bytes
flags                u16
BIT_count            u16
mg_zones             u32
```

SECRMAN locates Kbit/Kc using:

```text
32-byte fixed header
+ BIT_count * 16-byte optional pre-key descriptors
+ optional variable field when flags bit 0 is set
+ 8 bytes when (flags & 0xF000) == 0
= Kbit offset

Kbit 16 bytes
Kc   16 bytes
= encrypted runtime BIT table offset
```

For the common low-layout form supported by the current signer, the extra
8-byte area immediately before Kbit is the header signature.

The decrypted runtime BIT table contains an 8-byte table header followed by up
to 63 descriptors. Each descriptor contains a 32-bit size, 32-bit flags and an
8-byte block signature.

## The 0x01 / 0x02 BIT flag disagreement

Public sources disagree on the names of the low BIT flag bits. Some describe
0x01 as signed and 0x02 as encrypted; another established PC implementation
uses the opposite naming.

DriveForge therefore does not make the verifier trust either label globally.
The root signature is calculated under both possible signed-block selections.
A KELF envelope reports one of:

```text
bit_0x01
bit_0x02
ambiguous
unresolved
```

Payload crypto proceeds only when the root signature chooses one interpretation
unambiguously. The opposite low bit is then treated as the encrypted selector.

This is slightly more work than writing two constants. It also has the pleasant
property of being evidence rather than folklore.

## Disk Kbit/Kc

For a disk KELF, the first and second 8-byte halves of the 16-byte user header
are XORed. That value participates in deriving the file key through separate
Kbit and Kc master/material values.

The four 8-byte halves of Kbit/Kc are wrapped independently under the derived
16-byte file key. Each half starts with its own zero CBC IV. They are not one
32-byte CBC stream.

Memory-card SessionKey and StorageKey operations happen later in the card path
and do not belong in DriveForge's HDD KELF implementation.

## Signatures and content

The current host implementation covers:

- fixed-header signature;
- plaintext BIT signature;
- root signature;
- signed plaintext content blocks;
- signed and encrypted content blocks;
- DES and two-key TDES content encryption/decryption;
- encrypted BIT table round trips;
- complete low-layout sign -> verify -> decrypt self-checking.

`sign_low_layout_disk_kelf()` never returns a successful result merely because
it managed to serialize bytes. It immediately reparses and cryptographically
verifies its own output through the separate verifier and payload paths, then
requires the recovered plaintext to match the input byte-for-byte.

A signer disagreeing with its verifier is not a quirky compatibility mode. It
is a bug report with excellent timing.

## ICVPS2

KELF header flag bit 1 indicates ICVPS2. PS2SDK shows that ICVPS2 occupies the
last 8 bytes of `KELF_header_size`.

During the PS2 signing/download flow SECRMAN obtains those bytes from MechaCon
S-command `0x98` (`sceMgReadIcvPs2`). The currently reviewed public sources do
not provide a software implementation of the algorithm behind that command.

DriveForge therefore does **not** invent ICVPS2. The verifier can locate and
report an existing ICVPS2 field, but the current software signer refuses files
that require generating it. This restriction stays until a trustworthy
software oracle, documented algorithm or real hardware golden-vector set makes
the missing invariant testable.

## Current signing scope

The first signer deliberately accepts only the well-understood subset:

- common low-layout disk KELF;
- no variable pre-key field;
- no ICVPS2;
- one to 63 explicit BIT blocks;
- explicit caller-supplied Kbit/Kc;
- explicit signed/encrypted low-bit roles;
- content key count 1 or 2 when encrypted blocks exist;
- complete 8-byte alignment for signed/encrypted blocks;
- a block arrangement that lets the root signature prove which low flag bit is
  the signed selector.

The caller supplies segmentation rather than asking the format layer to guess
how an arbitrary ELF should be divided. Automatic FHDB/HDD-OSD/HOSD/PSBBN
strategies belong above this primitive and will choose known-good layouts for
those payload families.

## Testing

Synthetic tests intentionally contain no production Sony key material. They
cover:

- conventional DES known-answer vectors;
- two-key TDES round trips;
- strict keyset parsing;
- KELF layout and bounds rejection;
- disk Kbit/Kc wrap/unwrap;
- independent expected header/BIT/root/content signatures;
- encrypted BIT known vectors;
- complete synthetic KELF header-envelope verification;
- payload decryption and signature verification;
- corruption at header, BIT, root and payload layers;
- deterministic signing when the plan and content keys are frozen;
- signer refusal for ambiguous flag plans, ICVPS2 and unsupported content key
  counts.

A dedicated `MagicGate host verification` CI lane builds the header-only core
on Linux with ASan/UBSan and on MSVC. Crypto-format regressions should fail in
seconds instead of waiting for the entire Windows application stack to finish
assembling itself for emotional support.

## Before production bootstrap installation

The software signer is still experimental until it passes real artifact tests.
Before DriveForge uses it to create installable FHDB/HOSD/PSBBN payloads on a
physical HDD, we require:

1. golden disk KELFs produced or accepted by real PS2 hardware;
2. verification of those files by DriveForge with a user-local keyset;
3. DriveForge-generated equivalents accepted by the real PS2;
4. representative families/regions, not one conveniently cooperative sample;
5. Rescue Capsule and HDDMBR creation before the first physical bootstrap
   mutation;
6. payload-first, pointer-last installation and cold readback through the normal
   guarded physical pipeline.

MagicGate is a format/cryptography service. It does not receive a raw physical
write handle. The bootstrap installer consumes its verified output later. This
separation is intentional: a network downloader, a cryptographic signer and a
raw disk writer should never become one function simply because all three are
exciting.
