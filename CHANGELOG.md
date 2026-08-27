# Changelog

All notable changes to **qxDOS** are documented here.

For most of this project's life the record of a change has been the commit
message that made it, and those messages are longer and more specific than any
changelog entry - they carry the measurements, the counter-examples and the
things that were deliberately *not* done. This file summarises and points;
`git log` is the detail. Open work is in [`todo.txt`](todo.txt); the owner's two
original briefs, which used to sit at the top of `todo.txt`, are in
[`docs/brief.md`](docs/brief.md).

Two things to know before reading further.

**There are no version tags.** `git tag -l` returns exactly one tag, `disks`,
which is an asset tag rather than a version, so nothing below can be sliced into
releases. `project.yml` says `MARKETING_VERSION: "2.0"` and
`CURRENT_PROJECT_VERSION: 34`. The `v1.0.0 (Build 25)` entry at the bottom was
itself written in ebc7465 (2026-03-23), which set build **27**; no commit in the
history ever set build 25, and the same commit replaced an earlier `v2.0.0 -
DOSBox Migration` entry written at a461494. So the boundary below is
approximate: everything from b2b818c (2026-03-23) forward is in `Unreleased`.
Most of ebc7465's own additions are already in the v1.0.0 feature list below -
the touch controls, the presets and the disk catalog are - so they are not
repeated; the vendored libslirp and the mTCP DOS utilities it added are in
neither list, and are described here only where the emu88 bullet touches
them.

**Almost none of this was checked by machine.** Until 2026-08-27
`.github/workflows/release.yml` was the only workflow in the repo and it builds
disk images; no commit before that had ever been compile-checked or
test-checked by CI, and every measurement quoted below was produced by somebody
deciding to produce it. `tests.yml` runs the emu88 suites from that date on -
see the entry under Unreleased - so the statement holds for the history here
and not for what comes after it.


## [Unreleased]

### Added

- **The suites run themselves now.**
  Nothing ran emu88's validation suites automatically. Both existed, both were
  green, and neither was wired to anything - the only workflow in the repository
  cuts releases and touches neither `emu88/` nor `tests/`. That is how the `IDIV`
  overflow bug fixed in `7352fc5` failed `test386.asm` for as long as both suites
  had existed and still read as a pass.

  - **`.github/workflows/tests.yml`** builds all seven harnesses and runs the
    suites on every push and pull request. The ~600 MB corpora are cached, keyed
    on `fetch_tests.sh`; both upstreams are shallow clones whose objects *are* the
    payload, so their histories are dropped after fetching - 1.2 GB down to
    585 MB, with nothing the suites read removed.
  - **`tests/run_suites.sh`** is the gate, and is what CI runs, so a green tick
    and a clean local run mean the same thing. It holds SingleStepTests to
    1,758,402 / 1,758,699 - no worse, and *no better* without raising the
    baseline, because a silent improvement means the number is stale - and it
    does the comparison the test386 harness never did: its arithmetic output
    against the reference the upstream diagnostic ships.
  - That comparison was shown to fail before it was trusted. Against a core built
    from before `7352fc5` the runner still reports `ok reached POST 0xFF` and then
    fails the diff on the four `IDIV` lines. A gate that cannot fail is worth
    nothing, and POST `0xFF` alone was exactly such a gate.
  - **`tests/build.sh`** builds `test386` now, alongside the other six. Its
    command had lived only in prose in `tests/README.md`, which is a large part of
    why that suite was the one nobody ran. The script also falls back to `g++`
    when `clang++` is absent instead of failing with `command not found` - the
    same Clang-only assumption that had the sibling `cpmemu`'s release build
    broken on two architectures for two days.

  What this does not buy, and `tests/README.md` now says so where the gaps were
  listed: SingleStepTests as run is real mode only, and its corpus injects a
  `HALT` at the exception ISRs. 32-bit protected-mode instruction execution and
  exception delivery are still covered only by test386's full-system pass, which
  checks the machinery rather than every instruction. Automating a suite does not
  widen it.

- **emu88 is the hardware backend now, and it is written here.** 073605d
  brought the custom 8088/286/386+FPU+DPMI interpreter in from the archived
  iosFreeDOS project as a second selectable backend, and 932af28 made it the
  default - `qxDOS/Views/MachineConfig.swift` has `var backend: EmulatorBackend
  = .emu88` and decodes a saved config with no backend key as `?? .emu88`.
  DOSBox Staging stays in the tree and stays selectable. The core is 24 tracked
  files under `emu88/`, 17226 lines. Practical consequences from 073605d worth
  keeping in view: disks are mmap-backed (`MAP_SHARED` writable, `MAP_PRIVATE`
  read-only fallback) so a several-hundred-MB HDD image does not blow iOS RAM;
  networking goes through `Emu88SlirpNet` to the same statically linked libslirp
  NAT instance DOSBox uses, so mTCP and FDNET work identically on both backends;
  and emu88 stops with a real in-process teardown, where the DOSBox path still
  ends the process. **`README.md` and `CLAUDE.md` have not caught up with any of
  this** - both still describe DOSBox Staging as the engine. See `todo.txt`.
- **Two industry-standard suites drive the emu88 core, with harnesses and docs
  in `tests/`** (f2392f0). Re-measured on 2026-08-26 against HEAD, on linux
  x86_64 with g++ 15.2 (the harnesses were built with `CXX=g++ bash
  tests/build.sh`; `test386` was compiled by hand, see below):
    - SingleStepTests/80386, per-instruction: **1758402/1758699 passed
      (99.9831%)**, 1 skipped, 11 of 941 opcode files with failures, 43s. The
      297 failing cases decompose exactly: 287 IMUL undefined-SF/PF/AF cases
      across `0FAF`/`660FAF`/`670FAF`/`67660FAF`/`6769`, 6 `IN`-from-peripheral-
      port cases across `E5`/`66E5`, and 4 self-modifying-`REP`-inside-the-
      prefetch-window cases across `67A5`/`67AB`/`6766A5`/`6766AB`.
    - test386.asm, full-system: `insns=79670374 halted=0 last_POST=0xFF (33
      writes) done=1`, exit 0, and the `0xEE` ASCII block compared line for line
      against `tests/data/test386/test386-EE-reference.txt` - 44926 lines each,
      zero differing lines.
  **State the scope with the number.** SingleStepTests as run here is *real mode
  only*: `tests/sst386.cc` loads every segment register for all 1.76M cases
  through `load_segment_real` (`base = sel<<4, limit 0xFFFF`), and the corpus
  directory is named `v1_ex_real_mode`. It also never scores exception
  *delivery*, only the state a fault leaves behind. 32-bit protected-mode
  execution, which is where every DJGPP and PMODE/W client runs, is covered by
  test386's single full-system pass and nothing else. Leaving that scope off is
  what let the `IDIV` bug below sit unnoticed.
- **The CPU fixes f2392f0 made to reach that number**, each cleared against the
  corpus: `fetch_ip_byte` aborts the instruction on a mid-decode CS-limit `#GP`,
  so a prefixed instruction crossing the `0xFFFF` code limit no longer
  double-executes (that one alone cleared about 126 opcode files); `ea_add()`
  wraps multi-word-operand offsets in 16-bit address mode (`LES`/`LDS`/`LSS`/
  `LFS`/`LGS`, `BOUND`, far indirect `CALL`/`JMP`); string ops publish
  `pending_seg_idx` so segment-limit faults attribute to `#GP` rather than
  `#SS`; the EA far-jump no longer double-pushes after a limit-crossing fetch;
  and IMUL's undefined `SF`/`PF`/`AF` are modelled with a guard-bit add-and-
  shift accumulator, which took that family from about 76% to about 96%.
- **VESA VBE 2.0 (SVGA)** (18b597d): an S3 Trio64-class card with an 8 MB linear
  framebuffer reached two ways - the LFB aperture at physical `0xE0000000` and
  the bank-switched 64 KB window at `0xA0000`, both routed in
  `emu88_mem::fetch_mem`/`store_mem` ahead of the RAM and A20 logic. INT 10h
  `4F00`-`4F09` plus a real `4F0A` protected-mode interface whose emitted
  `SetWindow`/`SetDisplayStart` machine code is actually executed, over a
  25-mode table (640x480 to 1280x1024 at 8/15/16/24/32 bpp). INT 33h now scales
  frame-pixel mouse coordinates onto the guest range using the rendered
  resolution. Covered end to end by `tests/vesa_test.cc`, which drives the real
  INT 10h dispatch rather than calling the implementation directly.
- **An opt-in DOS-era hardware layer** (992c7fc), every device gated by
  `dos_machine::Config` so a text-only or compiler workload attaches none of it,
  and audio as a pull model - the host drives `dos_machine::audio_render(buf,
  frames, rate)`, so a host that never pulls costs nothing. AdLib/OPL2 + OPL3 FM
  (`emu88/opl.*`), Sound Blaster with the DSP command set, an 8237 DMA channel,
  the mixer and block-end IRQ (`emu88/sound_blaster.*`), a real PIT-ch2 PC
  speaker, the analog game port at `0x201`, a 16550 UART with FIFOs and modem
  and line status (`emu88/uart16550.*`), LPT1 output, and Hercules 720x348 mono
  graphics. Its own caveats, from the commit: the OPL core is clean-room and
  correct on pitch, ADSR and detection rather than bit-exact silicon; SB is
  streamed PCM with no ADPCM; the UART transmits with no baud throttle. Tested
  by `tests/hardware_test.cc` (26 checks, including "headless attaches nothing")
  and by `tests/{opl,sb,uart}_unit.cc`; all four pass here today.
- **Host wiring for that hardware**: a CoreAudio sink (ea990a9) - an
  `AVAudioEngine` + `AVAudioSourceNode` at 44.1 kHz stereo whose render block
  drains a lock-free SPSC ring that the emu thread refills to about 120 ms per
  batch, so `audio_render` and the device port writes stay on the emu thread and
  only the ring crosses to the audio thread - and gamepad plus COM1 hooks
  (ac72ee6), polling `GCController.current` on the emu thread into
  `set_joystick`, with a SwiftUI on-screen pad able to override it through
  atomics the poll consumes, and `serial_tx`/`serial_rx` over thread-safe FIFOs
  surfaced as `-emulatorSerialOutput:` and `-sendSerialData:`. **Both commits
  say what they were: compiled clean against the macOS and iOS SDKs, with the
  PCM unit-tested and no device run.** Nothing in this repo can run them - there
  is no Xcode here and the `dosbox-staging` submodule is uninitialized - so
  audible output, a real controller and a real serial peer remain unmeasured.
- **A selectable DOS layer** (8cec5b7): DOSBox DOS (built-in kernel and shell,
  `imgmount -fs fat`), FreeDOS, or MS-DOS (both booted from disk with `imgmount
  -fs none` + `boot`, which wired up the `boot_drive` field the bridge had been
  ignoring). Existing configs default to FreeDOS.
- **MS-DOS disks, and GPL section 3 compliance for the FreeDOS ones** (d63a077).
  The FreeDOS community pointed out that the shipped disks carried binaries
  without the corresponding source. The build scripts stop stripping `SOURCE/`
  out of extracted LiveCD packages, stage the kernel, FreeCom and mTCP source
  archives in `C:\SOURCE\`, and add `C:\LICENSE\` with the GPL2, GPL3 and MIT
  texts; the HDD grew 200 MB to 320 MB and the starter disk 22 MB to 32 MB to
  fit them. The two MS-DOS images are built from `microsoft/MS-DOS`
  `v4.0-ozzie`, the 1985-86 Multitasking MS-DOS 4.0 BETA released by Microsoft
  under MIT in 2024 - the only fully bootable layout in that release - and ship
  both the BETA boot-disk source and the 1988 production source.

### Fixed

- **`IDIV`'s non-faulting divider band is 8-bit only** (7352fc5), which is the
  bug that prompted everything above being written down. The band that `IDIV
  r/m8` genuinely needs had been extended to `r/m16` and `r/m32`, where the 386
  does not behave that way, so four `IDIV` overflow lines raised no `#DE` and
  differed from test386's reference output. Nothing detected it, because the
  harness checked POST `0xFF` and nothing else and the reference comparison was
  never automated - it still is not. With the band removed from the two wider
  forms, SingleStepTests is unchanged at 1758402/1758699 and the `0xEE` output
  matches the reference exactly.
- **Four CPU fixes made downstream in dosiz, adopted back here** (e89af9c),
  after the two copies of emu88 had drifted 129 lines. `FUCOMPP` (`DA E9`) was
  missing - the only register-form `DA` opcode that is not an `FCMOV`, and GCC
  emits it for long-double relational operators, so the compare was a no-op with
  stale `C0..C3` and the two pops were skipped, desyncing the FPU stack.
  `check_segment_access`/`check_segment_write` select the segment cache by
  `pending_seg_idx` instead of scanning `sregs[]` for a matching selector value,
  which picked the wrong hidden descriptor cache whenever DS/ES/SS alias one
  selector - common in DPMI clients. The data and stack accessors pass the raw
  linear address to `fetch_mem`/`store_mem` when paging is off rather than
  pre-masking, because those entry points test the high SVGA LFB aperture on the
  raw address, so pre-masking wrapped a VESA LFB address modulo `mem_size` into
  low RAM. And an undefined `0F` opcode raises `#UD` instead of calling
  `emu88_fatal`, so a guest's own handler gets a chance to run. Validated with
  identical before-and-after results across all five harnesses.
- **`-Wreorder-ctor`**: the constructor's init list now matches declaration
  order (e9645e9), ported from the same fix in dosiz while the two copies were
  still separate.

### Changed

- **The warning sweep** (ad01cd0): 20 warnings to none under `-Wall -Wextra`,
  with nothing suppressed - no `-Wno-*`, no pragma, no `[[maybe_unused]]`, no
  `(void)` casts, 13 insertions and 142 deletions, and
  `-Wno-unused-parameter` dropped from `tests/build.sh` because it was hiding
  nothing. Almost all of it was debug instrumentation whose `fprintf` calls had
  been removed while the scaffolding stayed, and intent was confirmed against
  073605d rather than guessed. One genuine correction came out of it: a bitwise
  OR between `FlagBits` and `EFlagBits` in the DPMI code, deprecated in C++20
  and slated to become an error, replaced with a named `uint16_t` constant after
  checking both forms over the full input space. **The headline carries no
  compiler qualifier and needs one.** Compiling the 11 `emu88/*.cc` files today
  with `g++ -std=c++20 -O2 -Wall -Wextra -I emu88 -c` gives **10** warnings: six
  `-Wempty-body`, three `-Wclass-memaccess` and one
  `-Wunused-but-set-variable`. 27 removed-trace husks remain across six files,
  three of which that commit did not name. In `todo.txt`.
- **The DOSBox backend quits by ending the process** (5d200dd), rather than
  attempting an in-process restart. DOSBox's static and global state makes
  reliable restart impractical; an `atexit` handler calling `_exit(0)` keeps
  static destructors from crashing on the way out, the
  `dosbox_request_shutdown` race was fixed, `HOSTIO` moved from static globals
  to a heap-allocated class, and the missing `GFX_Quit()` was added to teardown.
  emu88 does not do this - it restarts in place.
- **Renamed twice, for two different reasons.** `iosFreeDOS` became `qxDOS`
  (0de0dfe) - directory, bundle ID (`com.awohl.qxDOS`), bridging header,
  entitlements, scripts and docs - because the app boots DOSBox DOS, FreeDOS and
  MS-DOS, so "FreeDOS" should appear only where it means the actual OS. Before
  that, d797b37 and ef2002c took Apple's reserved terms out of user-visible
  strings. `NET.BAT` became `FDNET.BAT` (0f676a3) to stop colliding with other
  DOS `NET` commands, with a double-load guard and relaxed DHCP handling for
  read-only disks.
- **iPhone fixes and the catalog pipeline** (b2b818c): the iOS startup crash
  (an SDL `get_desktop_size()` assertion, bypassed with fullscreen plus
  `window_size` config and a patched `sdl_gui.cpp` fallback); the Mac quit
  deadlock between the main thread and the emulator queue on `GFX_Destroy`,
  fixed by making `DOSEmulator.stop` non-blocking; the 4:3 image constrained to
  the real UIKit safe-area height; and `scripts/update_catalog.sh`, which does
  the SHA comparison, version bump and sync between the two `disks.xml` copies
  instead of a person doing it.
- **App icon and attribution** (8c8b65e): the FreeDOS fish replaced by a green
  phosphor `C>` prompt, a GPL v2 section 3b three-year written source offer
  added to `README.md`, `RIGHTS.md`, the About view and the help files, and
  disclaimers making clear that qxDOS provides the app shell while the emulation
  and the DOS distributions are independent projects. 8434f1b says in
  `README.md` that this is an unendorsed port of FreeDOS.

### Documentation

- **`tests/README.md`** is the one document in the repo written to be checked
  rather than believed, and it is the model the rest should follow. Its "Not
  covered, and not automated" section states five real gaps - SingleStepTests
  being real mode only, its silence on exception delivery, `build.sh` not
  building `test386`, nothing comparing `test386` to its reference, and there
  being no CI - and all five check out against the source. Its section 2
  correction is left standing directly below the sentence it retracts instead
  of editing the false sentence away. Three numbers in it are still wrong;
  see `todo.txt`.
- **`docs/hardware-roadmap.md`** tracks the peripherals and the remaining host
  wiring. Its "Status update (core implemented)" paragraph is accurate; the
  tables below it still describe the tree as it was before 992c7fc, ea990a9 and
  ac72ee6, and contradict that paragraph directly. In `todo.txt`.
- **`docs/restart-static-state.md`** is the 2026-03-26 audit of DOSBox statics
  across an in-process restart. Three are marked WRONG (minor) and are still
  open; the rest are argued safe in place.
- **`docs/main-thread-checker.md`** documents `dosbox_run()` on a background
  queue calling `SDL_RenderPresent`, which touches UIView bounds, and records
  the 2026-03-26 decision to leave it as-is: debug-only, no effect on users or
  App Store review, revisit if real-device crashes appear. A deliberate open
  decision, not a fixed bug.
- **`docs/brief.md`** (2026-08-26) carries the owner's two briefs verbatim,
  moved out of `todo.txt` when that file was rewritten as open items only.
- **`CLAUDE.md`** gained the build-bump procedure (81e44d5). It has not been
  updated for emu88: its first line still calls this a DOSBox-based emulator and
  its Key Directories list omits `emu88/`, `tests/`, `scripts/` and
  `disk-content/`.

## v1.0.0 (Build 25) - Initial Release

First release of FreeDOS for iOS and Mac, powered by DOSBox Staging.

### Features
- Full 386 CPU with FPU and DPMI (runs protected-mode DOS programs)
- VGA/SVGA with S3 Trio64 emulation
- Sound Blaster 16 audio
- Configurable CPU type (386, 486, Pentium, Pentium MMX)
- Configurable CPU speed presets
- Mouse support with touch gestures (tap, long press, drag)
- Virtual touch controls: D-Pad, analog stick, action buttons
- Built-in touch control presets (DOOM, Duke Nukem 3D, General FPS, Arrow Keys)
- Custom touch control layout editor with per-game profiles
- Disk catalog with FreeDOS images and archive.org collections (Simtel, Walnut Creek)
- Automatic ZIP extraction for downloaded disk images
- Floppy, hard disk, and CD-ROM ISO support
- Multiple machine configuration profiles
- iPad and Mac Catalyst support
