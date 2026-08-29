# qxDOS — DOS for iOS and Mac

A DOS emulator for iOS and Mac. The default hardware backend is **emu88**, a
from-scratch 8088/286/386 interpreter written for this project and living in
[`emu88/`](emu88/). [DOSBox Staging](https://dosbox-staging.github.io/) is still
bundled and still selectable per machine profile.
Boots [FreeDOS](https://www.freedos.org/), [MS-DOS](https://github.com/microsoft/MS-DOS), or [DOSBox](https://dosbox-staging.github.io/)'s built-in DOS.

## What is this?

qxDOS is a SwiftUI front-end that lets you run DOS on iPad and Mac. It provides
the user interface, touch controls, disk management, the infrastructure to boot
different DOS operating systems, and — since commit `932af28`, "Make emu88 the
default hardware backend" — the PC emulation itself.

That last clause is a correction, recorded rather than edited away. This file
said "The qxDOS project provides only the iOS/Mac app shell" and "All PC
emulation is provided by DOSBox Staging". Both were false by the time you read
them. PC emulation now comes from `emu88/`: CPU, protected mode, x87, memory,
BIOS, VGA/VESA, DPMI host, NE2000, AdLib/OPL and Sound Blaster, 25 tracked files
and 19,072 lines. `qxDOS/Views/MachineConfig.swift` declares
`var backend: EmulatorBackend = .emu88` and decodes a missing key to `.emu88`,
so both a new profile and an old saved one land on emu88. DOSBox Staging is
still a git submodule at `dosbox-staging/` and still selectable; it is no longer
what runs unless you pick it.

The operating systems themselves — [FreeDOS](https://www.freedos.org/),
[MS-DOS](https://github.com/microsoft/MS-DOS), etc. — are independent projects
with their own developers and licenses, as is DOSBox Staging. qxDOS did not
write or contribute code to any of them.

Choose which DOS to run:

- **FreeDOS** — free, open-source DOS by the [FreeDOS Project](https://www.freedos.org/) (GPL v2+)
- **MS-DOS** — [Microsoft](https://github.com/microsoft/MS-DOS)'s original DOS; bring your own media, or use MS-DOS 4.0 (MIT license)
- **DOSBox DOS** — [DOSBox Staging](https://dosbox-staging.github.io/)'s built-in kernel and shell, no OS on disk needed. This one requires the DOSBox backend; emu88 has no built-in shell and boots a DOS from a disk image.

Use it to run classic DOS games, utilities, learn DOS programming, or
explore period software archives like Simtel and Walnut Creek.

## Emulation backends

One is chosen per machine profile.

**emu88 — the default.** A from-scratch interpreter for the 8088, 286 and 386,
plus the PC around it: an x87 FPU, a DPMI host, VGA and VESA VBE 2.0 (S3-class,
8 MB linear framebuffer), an NE2000 adapter, AdLib/OPL3, a Sound Blaster DSP
with 8237 DMA, a 16550 UART and a PC speaker. Written for qxDOS, GPL v3+, source
in `emu88/`. Its validation harnesses live in `tests/` and are described in
[tests/README.md](tests/README.md).

**DOSBox Staging — selectable.** Still a submodule at `dosbox-staging/`, and
still the only way to get the DOSBox built-in kernel and shell, custom cycle
counts, or a 486/Pentium CPU. It is no longer the default.

### What emu88 does not do

Named here rather than left to be discovered.

- **The x87's transcendentals are not correctly rounded.** The register file is
  80-bit extended (`f80 regs[8]`, a soft float in `emu88/emu88_f80.h`), and
  `tests/f80_unit.cc` grades its arithmetic against the host's own x87 bit for
  bit, flags included. The eight transcendentals are the exception: no 387
  rounds them correctly either, and `tests/f80_unit.cc` holds them to a
  measured ulp bound instead. *(Two things were listed here as missing and are not
  any more: delivery of unmasked exceptions, closed 2026-08-28, and the BIOS
  telling guests there was no coprocessor at all, closed 2026-08-29. Both are
  in [`CHANGELOG.md`](CHANGELOG.md).)*
- **Defects are recorded before they are fixed, and held at a baseline rather
  than hidden.** A harness that finds one asserts the architecturally correct
  answer and fails on purpose until somebody fixes it; fixing it then fails the
  harness the other way and says to lower the count, so a silent improvement
  cannot leave the number stale. Twenty-one were found this way in August 2026
  and all twenty-one are fixed, so every ledger currently reads zero.
  [tests/README.md](tests/README.md) sections 4 to 7 carry the account of each
  one, which is the point of the mechanism - the record, not the count.
- **No built-in DOS.** Boot FreeDOS or MS-DOS from a disk image.
- **No 486 or Pentium, and no custom cycle counts.** 8088, 286 and 386 only.
- **The validation has a scope.** SingleStepTests/80386 is run in real mode
  only and its corpus injects a `HALT` at the exception ISRs, so its pass rate
  says nothing about 32-bit protected-mode instruction execution or about
  whether a fault dispatched the right vector. Those are covered only by the
  one full-system ROM, `test386.asm`.

Use it to run classic DOS games, utilities, learn DOS programming, or
explore period software archives like Simtel and Walnut Creek.

## Features

- Two hardware backends: emu88 (default) and DOSBox Staging
- 386 CPU with an x87 FPU and a DPMI host. Under emu88 the FPU's register
  stack is 80-bit double extended precision, and its arithmetic is checked
  against a real x87 by [tests/f80_unit.cc](tests/f80_unit.cc). Unmasked
  exceptions are still not *delivered*; see "What emu88 does not do".
- VGA/SVGA graphics (S3 Trio64 under DOSBox; VESA VBE 2.0 with an 8 MB linear
  framebuffer under emu88)
- Sound Blaster 16 audio under DOSBox; Sound Blaster DSP with 8237 DMA plus
  OPL3 FM under emu88
- Mouse and keyboard input
- Configurable virtual touch controls (D-Pad, analog stick, buttons)
- NE2000 Ethernet networking with SLIRP (FTP, Telnet, HTTP downloads)
- Host file transfer (R and W commands to move files between DOS and your device)
- Disk image management (floppy, HDD, CD-ROM ISO)
- Downloadable disk catalog with FreeDOS images and archive.org collections
- Multiple machine configuration profiles
- Built-in help system with in-app documentation
- iPad and Mac (Catalyst) support

## Networking

The emulated NE2000 Ethernet adapter connects DOS to the internet through
your device's network connection via SLIRP (a virtual NAT router). The
FreeDOS disk comes with mTCP, a TCP/IP suite with FTP, Telnet, Ping, and
an HTTP downloader.

Type `FDNET` at the DOS prompt to get online:

```
C:\> FDNET
Loading NE2000 packet driver...
Getting IP address via DHCP...
Network is ready.

C:\> FTP ftp.example.com
C:\> HTGET http://example.com/GAME.ZIP GAME.ZIP
```

## File Transfer

Two built-in commands let you move files between DOS and your device:

- **R** (Read) — copies a file from your device into DOS
- **W** (Write) — copies a file from DOS to your device

```
C:\> R myfile.txt C:\MYFILE.TXT
C:\> W C:\DOCUMENT.TXT document.txt
```

Files are stored in the qxDOS folder in the Files app (iPad/iPhone) or
the app container's Documents directory (Mac).

## Disk Catalog

The app includes a downloadable catalog of disk images:

- **FreeDOS Hard Disk** — ~320 MB bootable FreeDOS with 230 utilities, full source under `C:\SOURCE\` and per-package `SOURCE/` trees
- **FreeDOS Starter** — ~32 MB minimal bootable FreeDOS, also with kernel/FreeCom/mTCP source
- **FreeDOS Boot Floppy (qxDOS)** — 1.44 MB FreeDOS boot floppy with attribution/credits
- **FreeDOS 1.4 / 1.3 LiveCD** — official installer ISOs (from freedos.org)
- **MS-DOS 4.0 Hard Disk** — 64 MB bootable Multitasking MS-DOS 4.0 BETA from microsoft/MS-DOS, with source ZIP on disk
- **MS-DOS 4.0 Boot Floppy** — 1.44 MB bootable Multitasking MS-DOS 4.0 BETA
- **Simtel MS-DOS Archive** — thousands of DOS utilities and shareware (from archive.org)
- **Walnut Creek CD-ROMs** — classic DOS software collections (from archive.org)

CD-ROM ISOs are downloaded directly from their original hosts. ZIP files
are automatically extracted.

### Source code on bundled disks

In accordance with section 3 of the GNU General Public License, every
GPL binary on a bundled FreeDOS disk image is shipped together with
its corresponding source code on the same disk:

- The kernel, FreeCom, and mTCP source archives live in `C:\SOURCE\`
- Each FreeDOS package directory (`C:\FREEDOS\<PKG>\`) contains a
  `SOURCE\` subdirectory with the package's original source as
  published by the FreeDOS Project
- License texts (GPL v2, GPL v3, MIT) live in `C:\LICENSE\`
- Credits and attribution: `C:\CREDITS.TXT`, `C:\README.TXT`,
  `C:\SOURCE.TXT`

The MS-DOS bundled disk ships with `C:\SOURCE\MSDOS40.ZIP` (the MIT-
licensed production MS-DOS 4.00/4.01 source from
github.com/microsoft/MS-DOS) plus `C:\LICENSE.TXT`.

## Building

### emu88 alone — no Xcode, no Mac

The core and its harnesses build on any machine with a C++20 compiler. This is
the whole development loop for `emu88/`, and it is what CI runs.

```bash
bash tests/fetch_tests.sh   # clones the two corpora into tests/data/ (gitignored, large)
bash tests/build.sh         # builds twelve harnesses into tests/build/
bash tests/run_suites.sh    # runs them and holds them to their recorded scores
```

`build.sh` uses `clang++` when it is present and `g++` otherwise. It needs no
SDL, no glib and no DOSBox submodule; only the SingleStepTests harness has an
external dependency, zlib. `run_suites.sh` is what
`.github/workflows/tests.yml` runs, so a green tick and a clean local run mean
the same thing.

Two more commands, neither in CI and both deliberately so:

```bash
ASAN=1 bash tests/build.sh   # everything again under -fsanitize=address,undefined
bash tests/check_dosiz.sh    # build the downstream consumer against this tree
```

The sanitized build lands in `tests/build-asan/` and is the thing to run by hand
after touching memory routing or a frame buffer; a sanitized SingleStepTests run
costs minutes rather than seconds, which is why CI does not. `check_dosiz.sh` is
described under *emu88 is compiled by a second project* below.

### The app — Mac and iOS

Requires a Mac. Nothing below this line can be built or checked on a Linux
checkout: the SwiftUI app, the Objective-C++ bridges, the CoreAudio path, the
joystick path and the DOSBox backend are all Xcode-only. `emu88/` and `tests/`
are the half that is portable.

#### Prerequisites

- Xcode 15+
- [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`)
- CMake (`brew install cmake`)
- SDL2 and dependencies (`brew install sdl2 sdl2_image libpng speexdsp iir1 opusfile`)

The DOSBox submodule is still required for an app build even though emu88 is the
default backend: the app links `dosbox-core`, and `Emu88SlirpNet.mm` uses the
libslirp symbols that library exports rather than linking libslirp itself. Which
makes the pin below a hard blocker on an app build, not a cosmetic problem —
see the warning under Steps.

#### Steps

> **`--recursive` does not work right now.** The `dosbox-staging` submodule is
> pinned at a commit that no longer exists upstream, so the submodule checkout
> fails - and it fails *quietly*, leaving you on upstream's default branch tip
> rather than stopping. Broken since 2026-03-23, measured 2026-08-27; the
> evidence and the three ways out are the first entry in [todo.txt](todo.txt).
> Everything in this section needs a Mac and none of it has been run on the
> machine that found this.

```bash
# Clone with submodules
git clone --recursive https://github.com/avwohl/qxDOS.git
cd qxDOS

# Configure CMake for Mac Catalyst
mkdir build-maccatalyst && cd build-maccatalyst
cmake ../dosbox-ios
cd ..

# Configure CMake for iOS (requires iOS toolchain)
mkdir build-ios && cd build-ios
cmake ../dosbox-ios -DCMAKE_SYSTEM_NAME=iOS
cd ..

# Generate Xcode project
xcodegen

# Open in Xcode and build
open qxDOS.xcodeproj
```

The Xcode pre-build script automatically runs `cmake --build` to
incrementally rebuild the DOSBox static libraries when sources change.

## Architecture

```
SwiftUI App (qxDOS/)
  ├─ Emu88Emulator.h/.mm — Objective-C++ bridge, DEFAULT backend
  │    ├─ emu88/ — the 386 core and the PC around it
  │    │    ├─ emu88.cc, emu88_pmode.cc, emu88_mem.cc — CPU, protected mode, memory
  │    │    ├─ emu88_fpu.cc, emu88_f80.h — x87 decode; 80-bit soft float
  │    │    ├─ dos_machine.cc, dos_bios.cc — machine, BIOS, VGA/VESA, INT 10h/13h/16h
  │    │    ├─ dos_dpmi.cc — DPMI host (INT 2Fh AX=1687h, INT 31h)
  │    │    └─ ne2000.cc, opl.cc, sound_blaster.cc, uart16550.cc — hardware
  │    └─ Emu88SlirpNet.h/.mm — libslirp NAT behind the NE2000, using the
  │         libslirp symbols the dosbox-core static library already exports
  └─ DOSEmulator.h/.mm — Objective-C++ bridge, selectable backend
       └─ dosbox-ios/ (C bridge layer)
            ├─ dosbox_bridge.cpp — config, lifecycle, input
            ├─ int_e0_hostio.cpp — INT E0h host file transfer
            └─ DOSBox-staging (git submodule)
                 ├─ CPU: 386/486/Pentium with FPU, dynamic recompiler
                 ├─ DOS: kernel, DPMI, drives, shell
                 ├─ Hardware: VGA, Sound Blaster, NE2000, keyboard, mouse
                 └─ SDL2: video output, audio, input events
```

Both bridges deliver frames through the same
`emulatorFrameReady:width:height:` selector, so the SwiftUI view model has one
render path rather than two.

## emu88 is compiled by a second project

`dosiz`, a separate DOS emulator, does not vendor emu88.
`dosiz/src/CMakeLists.txt` sets `EMU88_DIR` to
`${CMAKE_SOURCE_DIR}/../../qxDOS/emu88`
and compiles six files straight out of this working tree — `emu88.cc`,
`emu88_pmode.cc`, `emu88_fpu.cc`, `emu88_mem.cc`, `opl.cc` and
`sound_blaster.cc`, 9,065 lines as measured here, plus the headers they
include - among them the 1,852-line `emu88_f80.h`. There is no submodule, no
copy, no version constant and no checksum, so a local build there picks up
whatever is in this checkout at that moment; their CI is pinned separately, to a
full 40-character qxDOS SHA in `QXDOS_REF`, bumped by hand on their side.

A change under `emu88/` therefore changes a second product. [CLAUDE.md](CLAUDE.md)
states the rule that follows from that, including the command to run first.

There is a gate for it, and it is worth knowing which direction the coverage
runs. This repository owns emu88's validation suites and they are real-mode
per-instruction plus one full-system ROM; dosiz owns roughly thirty
**protected-mode** DPMI fixtures in its own CI. So the only automated check on
emu88's protected-mode behaviour lived in the repository that is forbidden to
fix emu88. `tests/check_dosiz.sh` runs those fixtures from here: it builds dosiz
from a sibling checkout against this tree, fails on any warning out of `emu88/`,
and runs every fixture its `ci.yml` asserts.

```sh
bash tests/check_dosiz.sh            # expects ../dosiz
DOSIZ_DIR=/path/to/dosiz bash tests/check_dosiz.sh
```

It exits 2, not 1, when there is no dosiz checkout — that is "not checked",
which is a different thing from a failure. It is deliberately not in CI here:
it needs a checkout this repository does not carry and must not depend on.

## License

qxDOS is licensed under GPL-3.0-or-later. See [LICENSE](LICENSE) for
the full license text.

This app distributes binaries from several independent open-source
projects. Each project's license is respected and its source code is
available — both directly on every disk image we ship (under
`C:\SOURCE\` and per-package `SOURCE/` trees) and from the upstream
sources listed below:

- **qxDOS** (GPL v3+) — https://github.com/avwohl/qxDOS
- **DOSBox Staging** (GPL v2+) — https://github.com/dosbox-staging/dosbox-staging
- **emu88** default hardware backend, written for qxDOS (GPL v3+) — `emu88/` in this repository
- **FreeDOS kernel** (GPL v2+) — https://github.com/FDOS/kernel
- **FreeCom (COMMAND.COM)** (GPL v2+) — https://github.com/FDOS/freecom
- **FreeDOS utilities** (GPL v2+ / BSD) — https://github.com/FDOS
- **CWSDPMI** (modified GPL v2) — http://sandmann.dotster.com/cwsdpmi/
- **mTCP** (GPL v3) — https://www.brutman.com/mTCP/
- **NE2000 packet driver** (Crynwr, GPL v2+) — http://crynwr.com/drivers/
- **libslirp** (BSD-3-Clause) — https://gitlab.freedesktop.org/slirp/libslirp
- **ZIPFoundation** (MIT) — https://github.com/weichsel/ZIPFoundation
- **MS-DOS** (MIT, © IBM and Microsoft) — https://github.com/microsoft/MS-DOS

qxDOS did not write any of the operating systems listed above, or
DOSBox Staging. All credit for FreeDOS, MS-DOS, DOSBox Staging, mTCP,
libslirp, and the rest belongs to their respective authors. emu88 is
the one entry in that list that is this project's own work; everything
else is repackaged into a SwiftUI app shell.

You may also request source code by opening an issue at
https://github.com/avwohl/qxDOS/issues. This offer is valid for at
least three years from the date of each release. See
[RIGHTS.md](RIGHTS.md) for the full attribution list, the trademark
notice, and the explicit GPL §3 source-on-disk statement.
