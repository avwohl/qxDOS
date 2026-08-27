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

- **The x87 FPU and the DPMI host have harnesses**, covering 2580 lines that
  every existing harness compiled and none of them ever executed. 437
  assertions in `tests/fpu_test.cc` and 378 in `tests/dpmi_test.cc`, both wired
  into `tests/build.sh` and `tests/run_suites.sh` so CI runs them: seven
  harnesses became nine.

  `fpu_test` drives real opcode bytes through `emu88::execute()` rather than
  calling `execute_fpu()` behind the decoder, so the `D8`-`DF` escape dispatch
  and the modrm path are exercised too. Because `emu88.h` declares
  `double regs[8]` - 53 mantissa bits, not the 387's 64 - it separates three
  things instead of asserting whatever the code happens to do: `check()` for
  behaviour that is correct, `diverge()` (31 sites) pinning a value that
  provably differs from real hardware with a comment naming the gap, and
  `bug()` (9 sites) asserting the correct 387 answer against a defect the
  double design does not explain. `dpmi_test` arrives the way a client does -
  `INT 2Fh AX=1687h`, a `FAR CALL` to the returned entry, then `INT 31h` from a
  stub inside the client's own protected-mode code segment - and asserts the
  descriptor bytes that land in the LDT rather than "`CF` clear", with the
  error paths held as hard as the happy ones.

  **Both were shown to fail before they were trusted**, which is the standard
  `f265310` set after POST `0xFF` turned out to be a gate that could not fail.
  56 single-point mutations of `emu88_fpu.cc` killed 45; of the eleven
  survivors two were provably equivalent mutants in unreachable clamps and nine
  were real coverage holes, closed with 48 more assertions, after which all 21
  re-applied mutations died. One assertion was thrown out during that work
  because it expected a condition code of all-bits-clear - exactly what a
  decode that ignored the opcode leaves behind - and it had passed against a
  mutant. 20 mutations of `dos_dpmi.cc` each turned at least one check red.

  **13 defects came out of it**, none of them fixed here. They are held to a
  baseline the way SingleStepTests is held to `SST_BASELINE`, so fixing one
  fails the harness and says to lower the number rather than quietly going
  green; `tests/README.md` sections 4 and 5 describe each. The two that reach
  an ordinary client are both in the DPMI host. `0002h`'s descriptor cache is
  dead code: the cache-hit path runs `break`, which leaves the enclosing `for`
  loop rather than the `switch` case, so every repeat lookup of one real-mode
  segment allocates a fresh selector and DJGPP's
  `__dpmi_segment_to_descriptor` in a loop walks through all 2047 LDT entries.
  And every reflected real-mode interrupt pushes its frame at physical
  `171FAh`, 64 KB above the `7000h`-`8000h` locked stack reserved for it,
  because `rm_sp = stack_top & 0x0F` is 0 at every 512-byte-aligned level and
  the first push wraps `SP` to `FFFEh`. Of the nine x87 defects the one a
  compiled program is likeliest to notice is `FCOMI` leaving `OF`, `SF` and
  `AF` untouched, so a following `JL`/`JLE`/`JG`/`JGE` reads whatever the last
  integer instruction left behind.

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
  checking both forms over the full input space. **The headline carried no
  compiler qualifier**: it was true of clang, and under
  `g++ -std=c++20 -O2 -Wall -Wextra -I emu88 -c` the 11 `emu88/*.cc` files still
  gave **10** warnings, with 27 removed-trace husks across six files, three of
  which that commit did not name. Both are closed by the sweep below.
- **The dead-code sweep** that closes both: 152 deletions against 17
  insertions across eight `emu88` files, again with nothing suppressed. Three
  pieces of it were not free, and all three sit on paths dosiz compiles out of
  this tree. Four debug blocks in `emu88_mem::store_mem`, which every guest
  byte write paid for, one of them setting `ivt21_trap` on any write that
  changed `IVT[21h]` - `git grep ivt21_trap` across `emu88/` at f265310
  returns exactly two hits, that write and its declaration, so nothing had
  ever read the flag. An 80-entry LDT dump on every intercepted `#NP`: 640
  `fetch_mem` calls to decide whether to execute a bare `;`, on the
  segment-not-present path a DPMI client takes routinely, which ad01cd0's own
  closing paragraph had flagged and no commit since had touched. And the `char
  vendor[32]` filled but never compared in DPMI `0x0A00`, Get Vendor-Specific
  API Entry Point - a real unimplemented service rather than a dead variable,
  so what stands in its place is a comment saying what implementing it would
  need. The three `-Wclass-memaccess` `memset`s over `MemBlock`, `DosBlock`
  and `SegMap` became per-element `= {}`. All eleven members are scalars
  initialized to zero, so the values are the ones `memset` left; the 3, 1 and
  1 bytes of tail padding it also cleared are already zero from the arrays'
  own `= {}` and are read by nothing. The three are non-trivial types, which
  is what the warning is about - they are trivially copyable.
  `gp_trace_count`, `watchpoint_addr` and `ivt21_trap` went with the code that
  wrote them. The 11 `emu88/*.cc` files now give **zero** warnings under `g++
  -std=c++20 -O2 -Wall -Wextra -I emu88 -c`, and 27 bare-semicolon husks
  across six files became none.
- **What that sweep deliberately left**, because deleting it is a decision
  rather than a cleanup, is in [`todo.txt`](todo.txt): eight `{ }`-bodied
  husks that no warning finds, since `-Wempty-body` fires on the semicolon
  form and not the brace form - two of them guard a runaway real-mode loop
  that now exits with no report at all, and three are `else { }` branches in
  `emu88_fpu.cc` that each once named an unhandled FPU opcode, so one is now
  silently ignored; four members still written and never read, whose names
  still read like a feature; and `-Wextra` still absent from `tests/build.sh`,
  so the zero above is a measurement and not a gate. Held to
  `tests/run_suites.sh` throughout: SingleStepTests 1,758,402 of 1,758,699,
  matching the recorded baseline exactly, and `test386.asm` reaching POST 0xFF
  with its arithmetic output identical to the reference across all 44,926
  lines.
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
