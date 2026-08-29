# qxDOS - DOS emulator for iOS/Mac on the in-house emu88 386 core

## Project Overview

DOS emulator for iOS and Mac. The hardware backend is **emu88**, a from-scratch
8088/286/386 + x87 + DPMI + PC-hardware interpreter written for this project and
living in `emu88/`. It has been the default since `932af28`:
`qxDOS/Views/MachineConfig.swift` declares `var backend: EmulatorBackend =
.emu88` and decodes a missing key to `.emu88`, so a new profile and an old saved
one both land on emu88.

DOSBox Staging is still a git submodule at `dosbox-staging/` and still selectable
per machine profile. It is not the engine any more.

This file said "DOSBox-based" and "powered by DOSBox Staging" until this pass.
That was false, and it is written down here rather than edited away, because it
is the kind of error a newcomer inherits without noticing.

Boots FreeDOS or MS-DOS from a disk image. The DOSBox built-in kernel and shell
require the DOSBox backend; emu88 has no built-in shell. 386 + FPU, DPMI,
VGA/VESA VBE 2.0, NE2000, AdLib/OPL3, Sound Blaster DSP with 8237 DMA.

## Architecture

```
SwiftUI Views
  ├─ Emu88Emulator.h/.mm (Objective-C++ bridge, DEFAULT backend)
  │    ├─ emu88/ - the 386 core and the PC around it
  │    │    ├─ emu88.cc, emu88_pmode.cc, emu88_mem.cc - CPU, protected mode, memory
  │    │    ├─ emu88_fpu.cc, emu88_f80.h - x87 decode; 80-bit soft float
  │    │    ├─ dos_machine.cc, dos_bios.cc - machine, BIOS, VGA/VESA
  │    │    ├─ dos_dpmi.cc - DPMI host (INT 2Fh AX=1687h, INT 31h)
  │    │    └─ ne2000.cc, opl.cc, sound_blaster.cc, uart16550.cc - hardware
  │    └─ Emu88SlirpNet.h/.mm - libslirp NAT behind the NE2000, using the
  │         libslirp symbols the dosbox-core static library already exports
  └─ DOSEmulator.h/.mm (Objective-C++ bridge, selectable backend)
       └─ DOSBox-staging (git submodule at dosbox-staging/)
            ├─ CPU: 386/486/Pentium with FPU, dynamic recompiler
            ├─ DOS: kernel, DPMI, drives, shell
            ├─ Hardware: VGA, Sound Blaster, keyboard, mouse
            └─ SDL2: video output, audio, input events
```

Both bridges deliver frames through the same
`emulatorFrameReady:width:height:` selector, so the view model has one render
path.

## Changing anything under `emu88/` - the rule

**A second product compiles these files out of this working tree.** `dosiz`
(sibling checkout, `../dosiz`) does not vendor emu88. `dosiz/src/CMakeLists.txt`
sets

```
EMU88_DIR = ${CMAKE_SOURCE_DIR}/../../qxDOS/emu88
```

and compiles six files from it: `emu88.cc`, `emu88_pmode.cc`, `emu88_fpu.cc`,
`emu88_mem.cc`, `opl.cc`, `sound_blaster.cc` - 9,065 lines, plus every header
those six pull in, which since 2026-08-28 includes the 1,852-line
`emu88_f80.h`.  **That six is a fixed list, not a glob.  A new emu88 `.cc`
would compile here, pass every suite here, and fail to LINK there, with nothing
in between to notice** - which is why the 80-bit soft float is a header.  No
submodule, no copy, no version constant, no checksum, no build stamp. A local `make` there
reads whatever is in this checkout at that moment. Their CI is pinned
separately: `QXDOS_REF` in `dosiz/.github/workflows/ci.yml` is a full
40-character qxDOS SHA, bumped by hand on their side. So CI moves when they
decide; a developer's local build there moves when you save. `dosiz/CLAUDE.md`
states which way the obligation runs - "emu88 belongs to qxDOS. Do not fix emu88
bugs from this repo" - and points at `qxDOS/tests/` as the gate.

For any change to `emu88/*.cc` or `emu88/*.h`:

1. **Validate here first: `bash tests/run_suites.sh`.** That is the gate
   `.github/workflows/tests.yml` runs, so a green tick and a clean local run
   mean the same thing. It needs `tests/data/`; run `bash tests/fetch_tests.sh`
   once first.
2. **Then run the downstream gate: `bash tests/check_dosiz.sh`.** See below.
3. **Say in the CHANGELOG entry that the core moved, and name the files.** The
   changelog is the only signal dosiz gets. There is no version to compare and
   no checksum to notice.
4. **Do not fix an emu88 bug as a drive-by.** It ships to a second product with
   nothing in between. The thirteen defects the FPU and DPMI harnesses recorded
   were fixed in one deliberate pass that ran both gates - not one at a time as
   they were noticed.

`run_suites.sh` passing is necessary and not sufficient, and the reason is an
asymmetry worth understanding. The suites here are real-mode per-instruction
plus one full-system ROM. The only automated coverage of emu88's
**protected-mode** DPMI behaviour is roughly thirty committed `.COM`/`.EXE`
fixtures in dosiz's own `ci.yml` - which is to say, in the repository that is
forbidden to fix emu88. So there is a second command:

```sh
bash tests/check_dosiz.sh            # expects ../dosiz
DOSIZ_DIR=/path/to/dosiz bash tests/check_dosiz.sh
```

It builds dosiz from a sibling checkout against this working tree, **fails on
any warning out of `emu88/`**, reads the fixture list out of dosiz's own
`ci.yml` rather than duplicating it, and runs every one. It exits **2**, not 1,
when there is no dosiz checkout - "not checked" is a different answer from a
failure - so it is safe to run unconditionally. It is not in CI here on
purpose: it needs a checkout this repository does not carry and must not depend
on. `todo.txt` carries what is still open about that pin, and
[`docs/emu88-downstream.md`](docs/emu88-downstream.md) is the standing note of
what their pin is currently missing - written per-pin rather than per-commit,
because the changelog is per-commit and the drift is not.

## Build

### emu88 alone - no Xcode, no Mac, no DOSBox submodule

```sh
bash tests/fetch_tests.sh   # corpora into tests/data/ (gitignored, large)
bash tests/build.sh         # twelve harnesses into tests/build/
bash tests/run_suites.sh    # runs them, held to recorded scores
```

`build.sh` uses `clang++` when present and `g++` otherwise. No SDL, no glib;
only the SingleStepTests harness has an external dependency, zlib. This is the
whole development loop for the core, and it is the only half of this repository
that can be exercised on a Linux machine.

### The app - requires a Mac

> **This block said "step 2 is broken" until 2026-08-29, and it was stale.**
> It described a pin of `019bbfd5` that did not exist upstream. That was fixed
> in `e4ade9d` (2026-08-27): the gitlink is `7b40053b` now,
> `git submodule status` shows it clean with no leading `+`, and the directory
> populates. The old text is summarised rather than kept because, unlike the
> wrong claims left standing elsewhere in this repository, nothing can be
> learned from it twice - it was simply out of date, and it was warning people
> away from a step that works. What IS still true is the last sentence: nothing
> below this line has been run on this machine - see the note under Build - and
> `todo.txt`'s first entry records that the v0.83.0 bump is statically clean and
> has never been compiled.

**Why the submodule is pinned at all**, since the rest of this repository pins
almost nothing: DOSBox Staging is a third-party upstream moving thousands of
commits a year, with no gate in either direction. Nothing here tests it -
there is no Mac on the machine that maintains this file, so an unpinned
submodule would drift untested and silently. That is the opposite of the
`emu88`/dosiz relationship, where the upstream *does* gate on the downstream
(`tests/check_dosiz.sh`) and a pin therefore buys much less than it costs. Pin
what you cannot test; float what something already checks.

1. Prerequisites: Xcode 15+, CMake, XcodeGen
2. `git submodule update --init` to fetch dosbox-staging. Still required even
   though emu88 is the default: the app links `dosbox-core`, and
   `Emu88SlirpNet.mm` uses the libslirp symbols that library exports rather
   than linking libslirp itself.
3. Build DOSBox as static library via cmake (see dosbox-ios/CMakeLists.txt)
4. `xcodegen` to generate Xcode project
5. Open qxDOS.xcodeproj and build

## Key Directories

Sizes measured with `git ls-files <dir> | wc -l` and `| xargs wc -l`.

- `emu88/` - the 386 core and the PC around it. The largest thing in the
  repository, the default backend, and shared with dosiz (see the rule above).
  `emu88_f80.h` is header-only ON PURPOSE: dosiz compiles a fixed list of six
  emu88 `.cc` files, so a seventh would build here and fail to link there
- `tests/` - emu88's validation harnesses. 20 tracked files, of which 9,011
  lines are the twelve harness `.cc` files. `tests/README.md` is the reference
  for what each suite covers and, as importantly, what it does not.
  `f80_unit.cc` is the one that grades the FPU's arithmetic, and it is the only
  harness that links no emu88 `.cc` at all
- `qxDOS/` - SwiftUI app (Views, Bridge, Assets). `Bridge/Emu88Emulator.mm` and
  `Bridge/Emu88SlirpNet.mm` are the emu88 side; `Bridge/DOSEmulator.mm` is the
  DOSBox side
- `dosbox-staging/` - DOSBox source (git submodule; empty, and **cannot
  currently be populated** - the pin does not exist upstream, see `todo.txt`)
- `dosbox-ios/` - iOS-specific DOSBox integration layer
- `scripts/` - disk image builders and catalog tooling (10 tracked files)
- `disk-content/` - files staged onto the built images (`freedos/`, `msdos/`,
  `licenses/`)
- `fd/` - FreeDOS disk images (gitignored, built by scripts/)
- `dos/` - DOS guest utility sources (`r.asm`, `w.asm`, `dpmitest.asm`) and the
  mTCP binaries under `dos/net/`. The built `.com` files are gitignored; only
  R.COM and W.COM are actually built, `dpmitest.asm` never has been
- `release_assets/` - Catalog XML, help files for GitHub releases
- `docs/` - Reference documentation

## Disk Catalog Consistency

Three files must stay in sync when disk images change:
- `fd/*.img` - the actual disk images (gitignored, built by scripts/)
- `qxDOS/Resources/disks.xml` - bundled catalog (fallback in app)
- `release_assets/disks.xml` - catalog published with GitHub releases

When a disk image is rebuilt, check that:
1. The `<size>` in both disks.xml files matches the actual file size
2. The `<sha256>` matches `shasum -a 256 fd/<image>`
3. Both disks.xml copies are identical
4. The `<catalog version>` is bumped so clients re-download

If any are stale, update them - but know which script does what, because this
paragraph used to say the wrong one. `scripts/build_starter_disk.sh` ends by
calling `scripts/update_catalog.sh`, which does **not** check and warn: if the
SHA differs it replaces the image, rewrites size and sha in both `disks.xml`,
bumps `<catalog version>` and syncs the two copies, silently. The script that
checks and reports is `scripts/check_disks.sh`, and **nothing calls it** - run
it by hand.

## DOS Text Files

Files written to DOS disk images (AUTOEXEC.BAT, FDCONFIG.SYS, .BAT, .TXT, etc.)
MUST use DOS line endings (`\r\n`, CR+LF). Shell heredocs produce Unix `\n` which
FreeDOS cannot parse — commands won't run, PATH won't be set, etc.
Use `printf 'line1\r\nline2\r\n'` instead of heredocs when writing DOS text files.

## Bumping the Build Number

1. Edit `CURRENT_PROJECT_VERSION` in `project.yml`
2. Run `xcodegen` to regenerate the Xcode project

Both steps are required — the xcodeproj is gitignored and built from project.yml.

## XcodeGen

After modifying `project.yml`, run `xcodegen` yourself — never ask the user to do it.

## Development Notes

- emu88 is C++20 with no external dependencies: no SDL, no glib, no DOSBox
  headers. Only the SingleStepTests harness links anything (zlib)
- DOSBox uses C++20, SDL2
- For iOS: disabled debugger, webserver, FluidSynth, MT32
- SDL2 provides iOS support (Metal rendering)
- The DOSBox bridge writes a temporary dosbox.conf and launches DOSBox; the
  emu88 bridge constructs a `dos_machine` in process, mmaps disk images
  (`MAP_SHARED` writable, `MAP_PRIVATE` read-only), and tears down cleanly on
  stop, so a restart-in-place works. DOSBox still terminates the process
- Frame capture via custom GFX callback → delegate → SwiftUI
- ZIPFoundation SPM dependency for extracting downloaded zip archives

## What cannot be checked from a Linux checkout

Stated so an unverifiable claim is not mistaken for a tested one. There is no
Xcode and no Mac on a Linux box, so nothing about the SwiftUI app, the
Objective-C++ bridges, the CoreAudio/`AVAudioSourceNode` path, the
GameController joystick path, or the DOSBox backend can be run or verified
there. `emu88/` and `tests/` can be, in full. Everything measured in this file
was measured on Linux x86_64 with g++ 15.2.
