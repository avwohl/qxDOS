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

- **The x87 FPU and the DPMI host have harnesses**, covering 2,580 lines - as
  `emu88_fpu.cc` and `dos_dpmi.cc` stood then, 2,710 after the fixes below -
  that every existing harness compiled and none of them ever executed. 473
  assertions in `tests/fpu_test.cc` and 420 in `tests/dpmi_test.cc`, both wired
  into `tests/build.sh` and `tests/run_suites.sh` so CI runs them: seven
  harnesses became nine.

  `fpu_test` drives real opcode bytes through `emu88::execute()` rather than
  calling `execute_fpu()` behind the decoder, so the `D8`-`DF` escape dispatch
  and the modrm path are exercised too. Because `emu88.h` declares
  `double regs[8]` - 53 mantissa bits, not the 387's 64 - it separates three
  things instead of asserting whatever the code happens to do: `check()` for
  behaviour that is correct, `diverge()` (31 sites) pinning a value that
  provably differs from real hardware with a comment naming the gap, and
  `bug()` asserting the correct 387 answer against a defect the double design
  does not explain - nine of those, all since fixed. *(The 31 divergences are
  also gone now, and so is the `double` design that caused them - see the
  80-bit register-file entry under Changed. This paragraph describes the
  harness as it was added, which is what a changelog entry is for.)*

  `dpmi_test` arrives the way a client does -
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

  **13 defects came out of it**, all of them fixed - see *Fixed* below. They
  were recorded first and fixed second, held to a baseline the way
  SingleStepTests is held to `SST_BASELINE`, so that fixing one failed the
  harness and said to lower the number rather than quietly going green. Both
  baselines are 0 now and every assertion that caught a defect is an ordinary
  `check()`; `tests/README.md` sections 4 and 5 carry the account of each. Two
  further gaps those files had recorded as **unassertable** are closed as well,
  and for the same reason in both cases: they were unassertable while the
  behaviour was undefined or out of bounds rather than an answer. `FIST`/`FISTP`
  of an out-of-range value cast a `double` straight to an integer, which is
  undefined behaviour rather than the 387's `#IA`-and-integer-indefinite, so a
  test there would have been testing the compiler; and the DPMI descriptor
  services read and wrote past the end of the LDT rather than returning
  `8022h`.

- **The NE2000 and the PC BIOS have harnesses**, which closes the last of the
  "compiled by everything, executed by nothing" entries in `todo.txt`. Nine
  harnesses became eleven.

  `tests/ne2000_test.cc` - 220 assertions. `emu88/ne2000.cc` was 425 lines and
  `grep -lni ne2000 tests/*.cc` returned **nothing at all**: it was linked into
  every harness that pulls in the DOS layer and executed by none of them. The
  card is driven through its 32 I/O ports the way a packet driver sees it -
  register banking across all four pages, the doubled on-card MAC PROM,
  remote DMA in both directions with its auto-increment and `RDC` bit, transmit
  asserted as the exact bytes delivered to `on_transmit`, the receive ring's
  4-byte DP8390 header and `CURR`/`BNRY` advance, ring wrap, overflow, `ISR`
  write-1-to-clear, and the `RCR` receive filter. 28 `diverge()` sites pin what
  a real DP8390 would do differently: no FCS, instantaneous transmission, no
  error counters, no multicast hash.

  `tests/bios_test.cc` - 498 assertions. `emu88/dos_bios.cc` was 2,178 lines,
  and the only parts of it that ran were whatever `vesa_test.cc`'s INT 10h path
  and `dpmi_test.cc`'s INT 21h handler happened to touch. This assembles
  `CD <vec> / F4` into guest memory and lets the decoder dispatch it, so the
  real interrupt entry path is exercised rather than the handlers being called
  behind it - every BIOS entry point in `dos_machine.h` is private, so there is
  no short cut even in principle. INT 10h text and mode-13h services, INT 11h,
  INT 12h, INT 13h against RAM-backed drives with a verified round trip, INT
  14h, INT 15h, INT 16h including the BDA ring wrap and the peek that must not
  consume, INT 17h, INT 19h, INT 1Ah, INT 2Fh, and the XMS driver behind
  `4310h`.

  **Both were shown to fail before they were trusted**, and in both cases the
  first pass left survivors that were real holes. 22 mutations of `ne2000.cc`,
  18 killed on the first pass; the two survivors - nothing read `CLDA0`/`CLDA1`,
  nothing asserted that a page-2 write is ignored - are why those assertions
  exist. 18 mutations of `dos_bios.cc`, all killed, after three earlier drafts
  had survived and produced the right-edge and top-edge scroll assertions, the
  8-page cursor-reset assertion and the column-wrap TTY assertion. Seventeen
  more mutations were applied against the fixes below and **five survived**,
  each one a coverage hole worth naming: a fix applied to only one of mode 13h's
  two clear paths, an off-by-one in a bound that no test approached, a bound
  that ignored its offset because every test used offset 0, a receive-path guard
  that no configuration ever reached, and a rejection that looked identical to a
  short read from the host. All five are closed. Final: 28 of 28 and 29 of 29.

- **A downstream gate, `tests/check_dosiz.sh`.** dosiz compiles six emu88 files
  out of this working tree and owns roughly thirty **protected-mode** DPMI
  fixtures in its own CI - so the only automated check on emu88's
  protected-mode behaviour lived in the repository that is forbidden to fix
  emu88. This script builds dosiz from a sibling checkout against this tree,
  fails on any warning out of `emu88/`, reads the fixture list out of dosiz's
  own `ci.yml` rather than duplicating it, and runs every one. It is
  deliberately not in CI here: it needs a checkout this repository does not
  carry and must not depend on. `7352fc5`'s commit message asserted dosiz
  "builds clean and behaves identically before and after"; that was checked by
  hand and nothing in either repository could reproduce it. Run on 2026-08-27
  against every emu88 change in this entry: configured, built, **0 warnings from
  `emu88/`, 37 of 37 fixtures passed** - 27 of those 37 being protected-mode
  DPMI fixtures, which is the coverage this repository does not have. Four of
  the six files dosiz compiles are touched here: `emu88.cc`, `emu88_mem.cc` and
  `emu88_pmode.cc` by the dead-code sweep, and `emu88_fpu.cc` by that plus nine
  of the twenty-one defect fixes and the `FIST` range check. The other twelve
  fixes are in `dos_bios.cc`, `dos_dpmi.cc` and `ne2000.cc`, which dosiz does
  not build.
- **`ASAN=1 bash tests/build.sh`** rebuilds every harness under
  `-fsanitize=address,undefined` into `tests/build-asan/`. `tests/README.md` had
  claimed the VESA pan clamp was "checked under AddressSanitizer" while
  `fsanitize` appeared nowhere in the repository - a one-off that nothing
  committed could reproduce and no regression would have caught. It is a command
  now, and not in CI on purpose: the sanitized SingleStepTests run costs minutes
  rather than seconds. `vesa_test` passes under it with no sanitizer report.
- **`tests/fetch_tests.sh` reclaims the clone histories.** Both upstreams are
  shallow clones whose objects *are* the payload; the suites read files and
  never git, so the 567 MB `.git` SingleStepTests left behind was pure
  duplication. `tests/data` goes from 1.2 GB to 585 MB, and a local checkout
  now holds what CI holds - `.github/workflows/tests.yml` had been doing this by
  hand after calling the script.
- **The suites run themselves now.**
  Nothing ran emu88's validation suites automatically. Both existed, both were
  green, and neither was wired to anything - the only workflow in the repository
  cuts releases and touches neither `emu88/` nor `tests/`. That is how the `IDIV`
  overflow bug fixed in `7352fc5` failed `test386.asm` for as long as both suites
  had existed and still read as a pass. It is automated now - the entry below
  is what did it.

  - **`.github/workflows/tests.yml`** builds every harness and runs the suites
    on every push and pull request - seven of them when it was written, eleven
    now. The corpora are cached, keyed on `fetch_tests.sh`; both upstreams are
    shallow clones whose objects *are* the payload, so their histories are
    dropped after fetching - 1.2 GB down to 585 MB, with nothing the suites read
    removed. That drop was done in this workflow at first and moved into
    `fetch_tests.sh` afterwards, so a local checkout holds what CI holds.
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
  files under `emu88/`, 17,244 lines. Practical consequences from 073605d worth
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

- **The two open transcendental counts, and a claim that was keeping one of
  them open.** THE CORE MOVED and **VALUES MOVED WITH IT**: this changes
  `emu88/emu88_f80.h`, which dosiz compiles, and it is not a flags-only change
  — `FYL2X`, `FYL2XP1` and `FPATAN` return different bits for a large fraction
  of their domain. Better bits, measured below, but different: a dosiz
  developer's local build moves the moment the file is saved, and there is no
  version, checksum or build stamp between the two products.

  **Count 1, `#U` from an intermediate — fixed, and it was seven forms wide,
  not one.** `todo.txt` and the previous entry scoped this to a single `F2XM1`
  input. An enumerated sweep of the bottom of the exponent range found the same
  miss in `F2XM1`, `FSIN`, `FPTAN`, `FSINCOS` (both outputs), `FYL2X`,
  `FYL2XP1` and `FPATAN`. `FCOS` is clean only because the host raises no `#U`
  for it.

  **The mechanism recorded for it was also wrong, and the fix it implied would
  not have worked.** The entry said the underflow "happens in an intermediate
  multiply that has already been denormalised in a throwaway context", implying
  the remedy was to forward that context's flags. Replayed: `f80_mul2(2^-16382,
  F80_LN2_HI)` raises **nothing at all** — `F80_LN2_HI` ends in two zero bits,
  so the single shift onto the denormal grid is exact. There was no flag to
  forward. Tininess had been spent by the throwaway and the inexactness lived
  in a flushed tail, so the condition had to be *reconstructed* rather than
  propagated: `f80_flag_tiny` asks the delivered result instead, ORing `#U` when
  the entry point has already recorded `#P` and the result has biased exponent
  zero. It ORs `#U` and nothing else — adding `#D` is the trap the previous
  entry recorded — and it is guarded on `#U` being masked, because with `#U`
  unmasked `f80_round_pack` already owns the response and has biased the
  exponent by +24576.

  Two things fell out of it. `FPTAN` never reported `#P` at all where its
  divide is exact — everything with |x| ≤ 2^-63, where the sine *is* x and the
  cosine *is* 1 — which had to be fixed first, because the `#U` rule is guarded
  on `#P`. And the committed claim that **`FYL2X` of a power of two raises
  nothing is false**: the host raises `#P` for every power of two except
  `x == 1.0`, and `x == 1.0` was the only input the check named after that claim
  tested. The check is renamed to say what it tests.

  **Count 2, `C1` — the recorded claim is false, and that was the finding.**
  The previous entry said "`C1` ... its agreement with hardware is unchanged at
  about chance". Re-measured against the host x87, that is wrong, and the
  probable cause of the error is reproducible: **`FSTPT` clears `C1`**, so a
  probe that reads `FNSTSW` after storing the result reports `C1 = 0` for
  everything — including for `FSQRT`, which is specified to set it. Such a probe
  yields 47.5–57.4% "agreement" for every function, which is "about chance"
  exactly. A live `FSQRT`/`FRNDINT` control catches it, and the checks added
  here carry one.

  A second reading error compounds it: `C1` means "the result grew in
  **magnitude**", not "moved toward +∞". Read the wrong way, every
  signed-result function scores exactly 50% and `C1` looks underivable in
  principle. `FRNDINT` over negative operands settles it — the host's `C1`
  matches |result| > |operand| 100% of the time and result > operand 0%.

  Measured properly, `C1` is derivable, and what decides it is whether the head
  of the last operation is exact or a 64-bit-rounded irrational. `FCOS` adds its
  correction to exactly 1.0 and was already at its ceiling. `FPATAN` added its
  to a rounded π/2 whose own half-ulp error swamps the rounding direction — and
  the tail constant it needed, `F80_PIO2_LO`, was already in the file, used only
  by the argument reduction. Carrying the logarithm and arctangent endings in
  double length fixes the value and the flag together (4,000 random inputs per
  function, against the host x87 instruction, before → after):

  | | value bit-identical | `C1` agrees |
  |---|---|---|
  | `FPATAN` | 80.3% → **94.1%** | 52.9% → **85.2%** |
  | `FYL2X` | 73.0% → **93.0%** | 51.7% → **86.8%** |
  | `FYL2XP1` | 66.3% → **78.0%** | 51.6% → **66.0%** |
  | `F2XM1` | 89.3% (unchanged) | 78.7% (unchanged) |
  | `FSIN` | 80.0% (unchanged) | 63.8% (unchanged) |
  | `FCOS` | 95.8% (unchanged) | 93.7% (unchanged) |

  `FPATAN`'s worst observed ulp drops from 2.0 to 1.0; no other recorded figure
  moves. **`C1` is not closed** — a ceiling of 88–97% is set by how often the
  host's own `C1` is architecturally correct, and `FSIN`/`FCOS`/`FPTAN` are
  untouched by this change — but "about chance" was never true of it and is the
  kind of claim that stops anyone looking.

  `f80_unit` 60 checks to 66. Four of the six new ones fail against the previous
  core; of the other two, one is the `FSQRT` probe control and one is the
  labelled guard on the `#D` trap — neither is evidence and both say so.
  `SingleStepTests` 1,758,402 and `test386` exact; `tests/check_dosiz.sh` 37/37
  with no warnings.

- **The machine stops denying it has a coprocessor.** THE CORE MOVED, but only
  just: `emu88/emu88.cc` (one CPUID feature bit) is the shared half; the rest is
  `emu88/dos_machine.cc` and `emu88/dos_dpmi.cc`, which dosiz does not compile.

  Three sources answered "is there an x87", and they did not agree. CMOS
  register `0x14` said yes (`0x2F`, bit 1) and has since it was written. The BDA
  equipment word at `40:10` said **no** — bit 1 was never set, so `INT 11h`,
  which returns that word verbatim, told every guest there was no coprocessor.
  CPUID leaf 1 said no as well (`EDX = 0x10`, bit 0 clear). DPMI `INT 31h`
  `AX=0E00h` said the question was unsupported (`AX=8001h`).

  emu88 has always emulated a complete 80-bit x87 whatever `config.cpu` says —
  there is no build without one — so "absent" was the lie, and an expensive
  one. Software that probes the documented way takes the answer seriously: a
  Borland-compiled program told there is no 387 uses its software emulator and
  never executes an x87 instruction, which means the `#MF` delivery path added
  the day before was dormant for exactly the software that motivated it. All
  four now say present: equipment word bit 1 (the default reads `0x0027`),
  CPUID `EDX` bit 0, and `0E00h` answering `0x35`.

  `0x35` rather than `0x31`, and the difference is the same bug in miniature.
  In `0E00h`'s reply, presence is bit 2 (`MPr`); bit 0 is `MPv`, "enabled for
  this client". `0x31` sets `MPv` and the type with `MPr` clear — a coprocessor
  that is enabled and absent — so the first cut of this change reproduced, in a
  third place, the contradiction it exists to remove. `0x35` is
  `MPv | MPr | type 3`. (dosiz's own host answers `0x31` here; its
  `DPMI_V10.COM` fixture asserts bit 0, which `0x35` also sets, so nothing
  downstream breaks — but the two hosts now disagree about `MPr`, and that is
  dosiz's call to make.)

  `0E01h` Set Coprocessor Emulation now succeeds for requests this host can
  meet and **refuses** the one it cannot. `EMv` asks the host to set `CR0.EM`
  while the client runs and reflect the resulting `#NM` to a client-supplied
  emulator; nothing here does that, so returning success would be the same
  species of lie as the equipment word — a client told "yes" waits forever for
  faults that never arrive. It returns `8021h` instead.

  `CR0.ET` and `CR0.MP` are set at POST in the same pass, as a real POST does
  on finding a coprocessor. Neither was ever *set* before — which is not the
  same as unread, since `emu88.cc`'s `WAIT` reads `MP` and a guest could always
  arm it itself with `MOV CR0` or `LMSW`, as a lazy-FPU-switching DOS extender
  does. What changed is that the machine arms it.
  They are set by the machine rather than by `emu88::reset()`, so a bare core —
  and therefore dosiz, `SingleStepTests` and `test386`, none of which construct
  a `dos_machine` — still comes up with `CR0 == 0`.

  **One consequence, stated because it is a real behaviour change and not a
  defect.** `CR0.MP` is what makes `WAIT` honour `CR0.TS`, so `WAIT` can now
  raise `#NM` where it never could. Nothing in this machine reaches it: `TS` is
  set in exactly one place, `emu88_pmode.cc`'s task switch, and neither the
  BIOS nor the DPMI host performs one — and the same `#NM` has always been
  raised by any ESC instruction with `TS` set, so this is a new trigger rather
  than a new failure class. Measured with `TS` forced and vector 7 left on an
  `IRET` stub that clears nothing: 999 dispatches, then `raise_exception`'s loop
  detector halts the CPU rather than hanging.

  `bios_test` 507 to 512 and `dpmi_test` 420 to 429. The `bios_test` accounting,
  since the obvious summary of it is wrong: the pass *adds* five assertions and
  *changes* six existing literals, eleven in all, of which ten fail against the
  previous core. The eleventh is the CMOS check, which passes before and after
  — it pins the half of the contradiction that was already right, which is what
  gives the "they agree" assertion something to agree with.

  Six of the nine `dpmi_test` additions fail against the previous core. One
  trap among them is worth naming because it nearly cost a real defect:
  the unsupported-function reply is `8001h`, and `8001h` happens to have bit 0
  set — so "is bit 0 set" passes against a host that has just said the question
  is unsupported. Combined with `MPv`/`MPr` being easy to transpose, an
  assertion on bit 0 alone would have passed both the old host and the wrong
  new value. `CF` is checked first and `AX` pinned exactly. `SingleStepTests` stays at 1,758,402 — CPUID is not in the
  corpus at all, which is why, rather than by luck — `test386` diffs clean, and
  `tests/check_dosiz.sh` is 37/37 with no warnings.

- **The x87 delivers its unmasked exceptions.** THE CORE MOVED: this changes
  `emu88/emu88.cc`, `emu88/emu88.h` and `emu88/emu88_fpu.cc`, which dosiz
  compiles, plus `emu88/dos_machine.cc`, `emu88/dos_machine.h`,
  `emu88/dos_bios.cc` and `emu88/dos_dpmi.cc`, which it does not.

  Before this, `emu88_fpu.cc` latched `ES` and `B` when a raised exception was
  unmasked and stopped there: no vector-16 dispatch, no `FERR#`, and `CR0_NE`
  declared in `emu88.h` and read nowhere. A guest that unmasked an exception
  and waited for a trap waited forever. `todo.txt` recorded it as needing "an
  IRQ13 path and a decision about `CR0.NE`", and both are here.

  Every behaviour below was reproduced on the host x87 before anything was
  changed, which is the only reason the directions are known:

  - **Delivery is deferred, and it is a fault.** An unmasked exception is
    reported not by the instruction that raises it but by the next *waiting*
    x87 instruction or by `WAIT`, with `CS:EIP` pointing at that reporting
    instruction so `IRET` re-executes it. Measured: an `FDIV` at `0x4018eb`
    raised nothing visible, and the `FLD1` 85 bytes later trapped with
    `RIP` exactly equal to its own first byte. `FIP`/`FCS` still name the
    instruction that raised, which is the only way a handler can find it.
  - **The check goes inside `execute_fpu`, after the modrm is decoded.** It
    cannot go in the escape dispatch in `emu88.cc`: the ten no-wait encodings
    are identified by opcode *and* modrm, and moved there it fails 12 of
    `fpu_test`'s 659 checks — every no-wait encoding reports, starting with
    `FNSTSW AX`. It also cannot go below
    the operand access - measured on the host, a pending exception outranks the
    reporting instruction's own page fault.
  - **The no-wait set is exactly ten encodings**: `D9 /6`, `D9 /7`, `DB E0`
    through `DB E4`, `DD /6`, `DD /7`, `DF E0`. The `mod!=3` qualifier is
    load-bearing, and `FNOP` is *waiting* despite the mnemonic - both verified
    against the host, where `FNOP`, `FXCH`, `FFREE`, `FDECSTP` and `FLDCW` all
    trapped and all ten above did not.
  - **`WAIT` was `break; // no FPU, just continue`.** Because `9B` is a whole
    instruction rather than a prefix, every waiting control form - `FSTSW`,
    `FCLEX`, `FINIT`, `FSTENV`, `FSAVE`, `FSTCW` - is `9B` followed by its
    no-wait encoding, so all of them became correct from that one line.

  **The `CR0.NE` decision, and why it is the board's rather than the CPU's.**
  `#MF` is vector 16 *decimal*, which is `INT 10h`, which in real mode is the
  video BIOS - and that is not a theoretical objection: raising vector 16 on
  this machine with `AX=0013h` was measured to reprogram the display to mode
  13h. No AT-compatible machine lets the CPU see the error. `ERROR#`/`FERR#`
  goes to a latch on IRQ13, vector `75h`, whose BIOS handler chains to
  `INT 02h` - which is where Borland's, Watcom's and DOS/4GW's floating-point
  handlers actually live, and why Turbo Pascal has runtime errors 205/206/207
  at all. So `fpu_signal_error()` is virtual: the bare core honours `CR0.NE`
  (raising `#MF` when set, and otherwise running on, because nothing is
  attached to the pin), and `dos_machine` overrides it with the AT wiring.
  dosiz, which subclasses `emu88` directly and sets `NE` nowhere, is unchanged
  by design - measured to matter, because its default IDT gives vector 16 a
  present null-selector gate that would have turned every `#MF` into `#GP(0)`
  and terminated the client.

  This also corrects `todo.txt`'s claim that "in practice DOS software masks".
  DJGPP 2.02+ and Watcom do; **Borland does not** - its DOS runtimes leave
  invalid-operation, divide-by-zero and overflow unmasked by default, and that
  is a large population of real-mode software.

  Four defects were found and fixed on the way, each of which had to be right
  before delivery could be:

  - **`ES` and `B` were treated as latched bits.** They are a *function* of the
    other two words - the OR of the currently-unmasked exception flags - so
    `FLDCW`, `FLDENV`, `FRSTOR` and `FNSTENV` all change them without any
    arithmetic happening. `emu88_fpu.cc` only ever OR-ed them in and never
    cleared, which was wrong in five measured cases: four `FLDENV` images and
    the live word after `FNSTENV`. Measured on the host, `FLDCW` unmasking an
    already-set flag takes `SW` from `0x3804` to `0xB884` out of nothing.
  - **`LMSW` wrote all sixteen low bits of `CR0`**, including `ET` and `NE`.
    Harmless while neither was read; now that `NE` selects between two delivery
    routes it means an `LMSW` could silently rewire the machine's x87. It is
    restricted to `PE`, `MP`, `EM` and `TS`, as on hardware.
  - **DPMI reflected every unhandled exception to real mode.** The comment said
    "terminate for CPU faults, reflect for others" and the code reflected
    unconditionally, so a `#MF` in a client with no handler installed would
    have reflected to real-mode `INT 10h` and called the video BIOS. DPMI 0.9
    4.5 reflects only exceptions 0-5 and 7 and terminates for the rest. This is
    deliberately wider than `#MF`: `#UD` and 8-1Fh now terminate the client too,
    where before they reflected to a real-mode vector that is not a handler for
    them - typically the BIOS's own no-op `IRET` stub, which resumed the client
    at the faulting instruction to fault again. `dpmi_test` is unmoved at 420.
  - **The report had to be RETIRABLE, or delivery is worse than no delivery.**
    `#MF` is a fault, so the reporting instruction re-executes after `IRET` -
    and if nothing clears the status word it reports again, forever. The
    default machine is exactly that case: `init_ivt` leaves `INT 02h` on an
    `IRET` stub that clears nothing. The first cut of this change livelocked a
    virgin machine on any unmasked exception, which is a worse failure than the
    silence it replaced. Hardware does not, because taking the interrupt clocks
    `IGNNE#` active and the instruction is let through on the retry; that is
    modelled now, cleared when `FERR#` deasserts. Four `bios_test` assertions
    pin it, and all four fail with `IGNNE#` disabled.
  - **A BIOS trap stub cannot chain to another interrupt.**
    `unimplemented_opcode` emulates the stub's `IRET` unconditionally after
    running the handler, so an `INT 75h` handler that called `do_interrupt(2)`
    had the frame it just pushed popped straight back off and the guest's
    `INT 02h` handler never ran. Under DPMI it failed differently and worse:
    the reflection path calls `dispatch_bios` and then restores `CS:IP`
    wholesale. `INT 75h` is therefore three bytes of real ROM code -
    `CD 02 / CF` - which goes through the ordinary interrupt machinery the way
    IBM's own handler does.
  - **The coprocessor line could not be serviced from `run_batch` alone.**
    `dos_dpmi`'s two nested real-mode execute loops call `check_interrupts()`
    and never `run_batch`, so an x87 error raised inside one of them was
    aborted with delivery arranged nowhere - the same instruction re-decoded
    until the loops' own 5M/10M safety counters tripped. `check_interrupts` is
    virtual now and `dos_machine` services the line there, which every
    execution loop reaches.
  - **The offer must not overwrite a queued interrupt.** `request_int` holds
    ONE vector, and the coprocessor offer sat after the timer, NE2000, UART and
    keyboard offers, destroying whichever was pending. `FERR#` is a level
    signal, so it now yields when the slot is taken and is re-offered on the
    first step it is free - which is what a level input on a real PIC does.
  - **IRQ13 could not have used the existing hardware-IRQ loop.** `pic_imr` is
    a `uint8_t`, so `pic_imr & (1 << 13)` is always 0 - IRQ 8-15 can never be
    masked - and `pic_vector_base + 13` is `INT 15h`, not `INT 75h`. The
    coprocessor latch therefore carries its own held flag and the AT's fixed
    vector `75h`, rather than joining a loop that would have mis-vectored it.
    The latent bug in that loop is untouched and is recorded in `todo.txt`.

  `fpu_test` gains 32 checks (627 to 659) and `bios_test` nine (498 to 507),
  the latter driving the whole path end to end - unmask, divide by zero,
  `FWAIT`, IRQ13, `INT 75h`, the guest's `INT 02h` handler - and asserting that
  the video BIOS was *not* entered.

  Both new sections were run against the unfixed core, and the split is worth
  stating exactly rather than quoting the totals. Of the 32 `fpu_test`
  additions, 11 fail there; of the 21 that pass, 17 pass **vacuously** - they
  assert that some encoding does *not* report, which is trivially true of a
  core that reports nothing - and the other four pass because the *setting*
  direction of `ES` was already right and only the clearing direction was
  broken. Of the nine `bios_test` additions, two fail. So the honest count of
  new assertions that discriminate is 13, not 41.

  Two of them discriminate against something else, and that is the point of
  keeping them: built deliberately with `#MF` routed to vector 16 the way a
  bare 386+387 would, `dos_io::video_mode_changed` went from 21 calls to 416.
  The numeric exception was calling `INT 10h` in a loop.

  `SingleStepTests` stays at 1,758,402 - the corpus contains no `D8`-`DF` file
  at all, 0 of 941, and its only x87-adjacent opcode is `9B` - and `test386`
  diffs clean; `tests/check_dosiz.sh` is 37/37 with no warnings.

- **All thirteen defects the new harnesses recorded**, and the harnesses that
  found them are the reason each one is described here rather than guessed at.
  All four harness baselines are `KNOWN_BUGS_EXPECTED = 0` now; every assertion that caught
  a defect stayed exactly where it was and became an ordinary `check()`, so the
  ledger reads as a record rather than a count. `tests/README.md` sections 4
  and 5 carry the full account.

  The two that reach an ordinary DPMI client:

  - **`0002h`'s descriptor cache was dead code.** On a cache hit the handler ran
    `break`, which leaves the enclosing `for` loop rather than the `switch`
    case, so control fell into the allocate-a-new-descriptor path below it.
    Every repeat lookup of one real-mode segment burned another LDT entry, and
    DJGPP's `__dpmi_segment_to_descriptor` in a loop walked through all 2047.
    The fix is `return` - nothing follows the `switch`.
  - **Reflected interrupts pushed their frame 64 KB outside the reserved
    window.** `rm_sp = stack_top & 0x0F` is 0 at every 512-byte-aligned level,
    so the first push wrapped `SP` to `FFFEh` and the frame landed at physical
    `171FAh` rather than in the `7000h`-`8000h` locked stack reserved for it -
    in a real session, the DOS kernel, a driver, or the client's own image.
    `SS` now addresses the base of the window and `SP` is the offset within it.

  The other two DPMI defects: **real-mode callback slots were never reclaimed
  and the counter was process-global** - a function-local `static int` that
  survived `dpmi_terminate`, a fresh mode switch and destruction of the machine,
  while `0304h` only cleared `CF`, so hook-and-unhook in a loop died at 16 and a
  second client in the same process started with the first one's count. The
  record is a `bool[16]` in `DpmiState` now, and `0304h` validates the address
  it is handed and returns `AX=8024h` for one it never issued or has already
  taken back. And **`0400h` advertised virtual memory it does not have**: `BX`
  was `0005h` with bit 2 set, where `CR0.PG` is never set, `0600h`-`0603h` are
  no-ops and `0500h` reports no swap file - the comment on the line said "no
  virtual memory". `BX` is `0003h`: bit 0 for 32-bit clients and bit 1 because
  this host really does return to **real** mode rather than V86 for a reflected
  interrupt, which was clear before and was also wrong.

  Nine x87 defects, none of them explained by the `double` register stack. The
  one a compiled program is likeliest to notice is **`FCOMI` leaving `OF`, `SF`
  and `AF` untouched**, so a following `JL`/`JLE`/`JG`/`JGE` read whatever the
  last integer instruction left behind; all four of
  `FCOMI`/`FUCOMI`/`FCOMIP`/`FUCOMIP` clear them now. Then: **`m80real`
  subnormals** mangled in both directions - `FSTP m80real` of 5e-324 wrote
  exponent `0x3C00` instead of `0x3BCD` and read back as ~1.1e-308, off by
  2^51, so a DJGPP long-double underflow that transited memory was silently
  corrupted; **0/0 taking the zero-divide path** (`ZE` and +∞) where a 387
  raises `IE` and returns the indefinite QNaN, on every divide path, now all
  eight funnelled through one helper so they cannot disagree again;
  **`FIDIV`/`FIDIVR` by zero losing the sign**, a bare `INFINITY` where the
  real-operand paths twenty lines away used `copysign`; **`fpu_compare` never
  clearing `C1`**, which Intel specifies for `FCOM`, `FCOMP`, `FCOMPP`,
  `FUCOM`, `FICOM` and `FTST`; **`FPREM1` rounding the quotient with `round()`**
  rather than ties-to-even, so 10 rem 4 gave −2 where the answer is 2, now
  `std::remainder`, which is the IEEE remainder by definition; and **the 32-bit
  `FNSAVE` image half-written**, its zero-fill loop reading `for (int i = 3; i
  < (op_size_32 ? 7 : 7); i++)` - both arms of the ternary 7, a dead copy-paste
  - with a body that only ever stored 16-bit words.

  **This ships to a second product with nothing in between.** Six emu88 files
  are compiled straight out of this tree by dosiz, `emu88_fpu.cc` among them,
  so nine of these thirteen reach it on its next build. `tests/check_dosiz.sh`
  is the check that was made by hand before and is a command now: dosiz
  configures, builds with zero warnings out of `emu88/`, and passes all 37
  fixtures its own CI asserts.
- **Eight more defects, from the two new harnesses**, fixed the same way: found
  first, recorded as a failing assertion, then fixed, with the assertion staying
  put and becoming a `check()`. Both ledgers are back to zero.

  In the NE2000: **a received frame could overwrite the card's own MAC PROM.**
  `receive()` guarded its ring writes with `a < MEM_TOTAL` where
  `dma_write_byte` guards with `a >= BUF_START && a < MEM_TOTAL`, so a card
  started before its driver had programmed `CURR` - and `CURR` comes up 0 from
  `reset()`, which is exactly the state a driver is in between `STA` and its
  first page-1 write - wrote the incoming frame over the read-only PROM at
  address 0. And **a 16-bit read of a register port was half a read**:
  `iowrite16()` correctly split a word write into two byte writes, because that
  is what the ISA bus does with an 8-bit-decoded register file, while
  `ioread16()` returned one register zero-extended, so an `INW` from `base+3`
  lost `TSR`.

  In the BIOS: **INT 13h `AH=02`/`03` never checked that the CHS address
  exists.** `sector - 1` with sector 0 made the unsigned LBA `2^64-1` and handed
  `dos_io` a byte offset of `2^64-512`; a host backend seeking with a signed
  `off_t` seeks backwards. Sector, head and cylinder are bounded now.
  **XMS `AH=0Bh` validated neither offset nor length against the block**, so a
  move longer than the destination reported SUCCESS and wrote past the end of
  another allocation - `A7h`/`A8h`/`A9h` now, bounded exactly. **INT 10h
  `AH=00` ignored AL bit 7**, "do not clear the display buffer", masking it off
  and clearing anyway, so a program that re-selects its current mode to reset
  the CRTC lost the screen. **INT 13h `AH=15h` returned `CF` set** for a drive
  that is not present, where `AH=00h` is the documented *success* answer, so a
  caller that branches on `CF` first read a missing drive as an I/O error. And
  **INT 1Ah `AH=01` left the 40:70 midnight flag set** when it set the tick
  count, so the very next `AH=00` reported a rollover that had already been
  consumed.

  Two of these are guest-controlled writes outside their bounds - the XMS move
  and the NE2000 ring - which is the class of defect worth finding a harness
  for, and neither was reachable by any suite that existed before this pass.
- **`audio_render` fills a buffer longer than 4096 frames instead of
  truncating it.** The 32-bit mixing accumulator is a fixed 4096-frame scratch
  buffer and a longer request was clamped to it, leaving the rest of `out`
  untouched - so a host that asked for more got its own stale buffer back for
  the tail, silently. It mixes in chunks now; each device's `render()` carries
  its own phase, so the joins are continuous. This was a latent trap rather
  than a live bug, because the CoreAudio bridge asks for at most 4096 - which
  is exactly the shape of thing that becomes a bug the first time somebody
  changes the host. `hardware_test` asserts a 5000-frame request writes its
  tail, and reverting the chunking turns it red.
- **`git submodule update --init` works again**, which it had not since
  `ebc7465` (2026-03-23). The `dosbox-staging` pin was `019bbfd5`, a commit that
  does not exist upstream: GitHub's API returns `422 No commit found`, no remote
  ref points at it, and a direct `git fetch` gets `upload-pack: not our ref`. It
  did not fail cleanly either - the upstream clone succeeded and only the
  checkout of the pin failed, leaving a fresh clone on upstream's default branch
  tip with `git submodule status` showing a leading `+`. Anyone who did not read
  the error built an arbitrary DOSBox believing they were on the pin.

  **The pin was never an upstream commit.** Walking the gitlink back, `a461494`'s
  `e8461f4` still resolves and all three later pins do not, and there is no fork
  of dosbox-staging under this account - so those commits lived only in one
  working copy and were never pushed. What they carried is in *Fixed* below.

  Nothing caught it because a working copy that already has the objects never
  notices, `tests.yml` never touches the submodule, and `release.yml` would but
  only runs on a release tag. What surfaced it was Dependabot, added an hour
  earlier for an unrelated reason: it clones with submodules unconditionally and
  so could not read its own config.

  The pin is `v0.83.0` (`7b40053b`, tagged 2026-08-26) - a release rather than
  `main`'s tip, which breaks five source paths where the tag breaks three. A
  fresh clone now runs `git submodule update --init` to exit 0 with no `+`.
- **The DPMI descriptor services validate the selector they are handed.**
  `0006h`-`000Ch` all took a selector in `BX` and indexed the LDT with it
  unchecked. `sel >> 3` runs to 8191 where the table holds 2048 entries, so a
  GDT selector (`TI` clear) indexed the LDT anyway and an out-of-range index
  read or wrote as much as 48 KB past the end of the table - a guest-controlled
  address, and what sits there in this machine's layout is the GDT, the IDT and
  the TSS. `0001h` had validated this way since it was written and the other
  seven had not. They return `CF` set and `AX=8022h` now, which is what DPMI 0.9
  specifies, and `0000Ah` no longer allocates an alias selector before deciding
  to reject. `tests/README.md` had recorded this as unassertable - "a test could
  only pin the out-of-bounds access, not a behaviour worth keeping" - which was
  true until the behaviour became an error code; `dpmi_test.cc` now puts 32
  assertions through it, and five single-point mutations of the guard each turn
  at least one red.
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

- **The x87 register stack is 80-bit extended precision, and every arithmetic
  path is rewritten against it.** `emu88.h` declared `double regs[8]` - 53
  mantissa bits against a 387's 64 - so a whole class of real-hardware results
  was not reproducible, and `tests/fpu_test.cc` carried **31 `diverge()`
  assertions** pinning exactly where the design showed through. There are none
  left. The register file is `f80 regs[8]`, and the arithmetic underneath it is
  a new header, `emu88/emu88_f80.h`.

  **This moves the core, and dosiz compiles it.** Changed: `emu88/emu88_fpu.cc`
  (rewritten), `emu88/emu88.h` (the `FPUState` struct, plus the six memory
  helpers whose types changed, two removed - `fpu_round` and `fpu_compare`, both
  of which took and returned `double` - and three added), one line of
  `emu88/emu88.cc` (`reset()` calls `fpu_power_on()` rather than `fpu_init()`,
  because RESET gives the data registers +0.0 and FNINIT leaves their contents
  alone - two different things that used to be one function), and one new
  header, `emu88/emu88_f80.h`. dosiz compiles
  `emu88_fpu.cc` as one of exactly six emu88 translation units, which is why
  the soft float is a HEADER: a seventh `.cc` would build here, pass everything
  here, and fail to link there with nothing in between to notice. Both gates
  were run - `tests/run_suites.sh` and `tests/check_dosiz.sh`.

  What the format change buys, beyond the eleven bits:

  - `FLD`/`FSTP m80real` are ten-byte moves, so NaN payloads, signalling NaNs,
    denormals and the unsupported encodings survive a round trip. The old
    reader rebuilt a `double` with `ldexp()` and lost the sign and payload of
    every NaN on the way in.
  - The seven `FLD` constants are the 387's ROM values to all 64 bits.
    `FLDPI` was `M_PI` widened - `C90FDAA22168C000`, the low eleven bits gone -
    and is `C90FDAA22168C235` now. The six that are not zero were computed by
    integer arithmetic to 800 bits and rounded; that they came out
    bit-identical to the published ROM values is the check that the generator
    was right.
  - Precision control works. Rounding a significand to 24 or 53 bits inside a
    15-bit exponent range is a thing only a soft float can do, and `CW` bits
    9:8 were read nowhere at all before.
  - `DE`, `OE`, `UE` and `PE` are set, in the order hardware sets them; none of
    the four was ever set before. `ES` and `B` latch when a raised exception is
    unmasked.
  - Pushing onto a live register is a stack overflow and reading an empty one
    is a stack underflow, each with the `IE`, `SF` and `C1` a 387 reports.
    Neither was detected before, so a desynchronised stack carried on with
    whatever stale value was in the slot.
  - `FPREM`/`FPREM1` reduce a large exponent difference a bite at a time and
    report `C2`, and leave the quotient bits in `C0`/`C3`/`C1`. The old code
    did one `double` divide and always reported "complete", which for
    `2^100 mod 3` is simply the wrong answer.
  - The 32-bit real-mode environment images mask their pointer fields. A
    real-mode linear address can need twenty-ONE bits - `(0xFFFF << 4) + 0xFFFF`
    is `0x10FFEF` - and only four of them belong in the high field; the rest of
    that dword is reserved. The 16-bit form got this free from its `uint16`
    field and the 32-bit one did not. *(This bullet is wrong, and is corrected
    under "The x87 defect pass" below rather than edited away: four bits at
    12:15 is the **16-bit** layout. The 32-bit real-address-mode image puts the
    pointer's high bits at bits 27:16, with twelve bits of room, so what this
    change did was move a truncation from one wrong place to another.)*
  - `FNSTENV`/`FLDENV` write and read all seven environment fields in the
    layout the operand size and the processor mode select - four layouts, and
    virtual-8086 mode takes the real-address one even though `CR0.PE` is set,
    which is the case testing `CR0.PE` alone gets wrong. They wrote two of the
    seven before. Separately, `FNSAVE` wrote its tag word TOP-relative where a
    387 writes it in **physical** register order, and `FRSTOR` read it back the
    same wrong way, so the image was self-consistent and interchangeable with
    nothing; both are fixed together.
  - The eight transcendentals are evaluated in this file's own arithmetic
    rather than through the host's `double` libm. `F2XM1` was `pow(2,x)-1` and
    `FYL2XP1` was `log2(x+1)`, which throws away precisely the precision those
    two encodings exist to keep: both returned exactly zero for `x = 2^-60`.
  - Encodings that did nothing are decoded. Three used to be *reported* as
    unhandled and then ignored: `FFREEP` (`DF C0+i`), which GCC and DJGPP emit
    as a cheap pop and whose absence left the stack one deeper than the
    compiler believed; the undocumented `FXCH` alias at `DD C8-CF`; and
    `FSETPM`/`FNENI`/`FNDISI`, which are 287 control instructions and no-ops on
    a 387 rather than errors. `DE D8` and `DE DA`-`DE DF` were worse - silent
    no-ops, with no report at all - and now reach the unhandled-opcode path.

  **The validation is the part worth reading.** Neither suite here can grade an
  FPU: no opcode file in the SingleStepTests corpus begins `D8`..`DF`, because
  the capture bench had no coprocessor, and `test386`'s reference output has no
  x87 mnemonic in it. So a new harness, `tests/f80_unit.cc`, drives the HOST's
  x87 as an oracle - on x86-64 `long double` is this exact format, with the
  same control word - and compares results **and exception flags** bit for bit
  across all four rounding modes and all three precision-control settings.
  Add, sub, mul, div, sqrt, every conversion, `FRNDINT`, `FSCALE`, `FXTRACT`,
  `FPREM`, `FPREM1`, the comparisons, `FXAM` and packed BCD all match exactly.
  The transcendentals cannot - no 387 rounds those correctly either. They are
  compared against the host's long-double libm and held to a bound of 6 ulp of
  a 64-bit significand; the worst actually observed is 4, and only at fifty
  times the default sample, with `FSIN` of arguments up to `2^62` at 2. The
  harness prints a figure per function on every run, so a regression shows up
  as a number changing rather than as a check still passing. A sweep of all 100
  zero/infinity/NaN quadrants of `FPATAN` is graded exactly.

  Two more defects came out of checks the oracle could not make. A sanitized
  build (`ASAN=1 bash tests/build.sh`) reported undefined behaviour in
  `f80_to_int`: `FISTP m64int` of exactly -2^63 is in range, and negating it as
  a signed `int64_t` is undefined - the answer was right, which is precisely
  why a differential oracle cannot see it. And feeding the transcendentals
  operands from the ENDS of the exponent range rather than the middle found
  `F2XM1` of 2^-16382 coming out at exactly twice the right value, because the
  helper that builds an `f80` from a significand and an exponent had no
  subnormal path and truncated a negative biased exponent into a `uint16_t`.
  Both are fixed and both now have assertions.

  Five things came out of the oracle itself that are not in the manual in those
  words, and each was a real defect when it was found: `#D` is reported for a
  denormal operand even against an infinity, but `#IA` and `#Z` suppress it;
  the masked-overflow "largest finite value" is the largest finite value *at
  the current precision*, so `0xFFFFFF0000000000` under `PC=24`; two NaNs with
  equal significands are separated by the smaller sign-exponent word, not by
  "the destination"; a narrowing store does not raise `#D` for a denormal
  source, because a store is not an arithmetic operation; and `FSCALE` does not
  honour precision control at all - PC reaches add, sub, mul, div and sqrt and
  nothing else - which this got wrong until the oracle's grid for the
  non-arithmetic operations was widened past `PC=64`. That last one is the
  argument for running the whole grid everywhere rather than only where the
  control word obviously applies.

  Two more things this pass turned up that are not about the FPU at all. A
  `clang++` appeared on the machine, and `tests/build.sh` prefers it over
  `g++` - so the "zero warnings under `-Wall -Wextra`" claim, which had only
  ever been checked with `g++`, was silently a claim about a compiler that was
  no longer being used. Clang produced 44 `-Wunused-const-variable` warnings
  that `g++` does not raise at all; they are fixed, and the tree is clean under
  both compilers now. And 29 single-point mutations of `emu88_f80.h` and
  `emu88_fpu.cc` were applied one at a time and all 29 died - five of them only
  to `f80_unit`, twelve only to `fpu_test`, which is the argument for there
  being two harnesses rather than one. Two of them survived the first run and
  both were real holes; `tests/README.md` §4 has the table and what they were.

  `tests/fpu_test.cc` goes from 473 assertions to 577, with its 31 divergences
  converted to ordinary checks and a new section for the classes the 80-bit
  file makes reachable at all - gradual underflow, overflow, denormals,
  unsupported encodings, signalling NaNs, and a `FNSAVE`/`FRSTOR` round trip
  over all eight of them. The suites are otherwise unchanged: SingleStepTests
  still at 1,758,402, `test386` still matching its reference line for line.

  **A faulting memory operand now aborts the instruction.** `#GP`, `#PF` and
  `#SS` are faults, not traps: the handler returns to the same instruction and
  it runs again from the start, so the x87 state it re-enters with has to be
  the state it left. Nothing enforced that - a faulting `FLD` still pushed, a
  faulting `FSTP` still popped, a faulting `FISTP` still left `#P` in the
  status word, and a faulting `FNSAVE` still re-initialised the whole FPU - so
  a guest that page-faulted on an x87 operand resumed with a register stack one
  deeper or one shallower than it had left. `execute_fpu` snapshots the state
  before a memory form runs and restores it if the instruction faulted; the
  snapshot is skipped entirely for the register forms, which cannot fault past
  the two checks now made before it. `FNSTSW AX` also went straight to
  `regs[reg_AX]`, the one FPU site writing a general-purpose register without
  the `fault_abort()` guard every other integer write in this core has; it goes
  through `set_reg16` now.

  There is a second half to that. `check_segment_write` deliberately lets an
  access through once an exception is already pending, so the loops that write
  a field a byte or a register at a time - `FNSAVE`'s eighty-byte register
  area, `FRSTOR`'s, `FBSTP`'s ten bytes - would keep writing *past* the fault,
  to wherever the offset had wrapped to. They stop at the first fault now, and
  `tests/fpu_test.cc` section 22 asserts that the bytes after it are untouched.
  *(The three LOOPS did. `fpu_store_env`'s seven fields are the same shape and
  were left unguarded, so `FNSTENV` and the environment half of `FNSAVE` kept
  writing past a fault - see "The x87 defect pass" below. "They" was too broad
  a word, and the sentence is corrected there rather than edited here.)*
  What is still not undone is memory the instruction had already written
  *before* the faulting access: a multi-dword store that faults halfway leaves
  its first half behind, here as on the integer side of this core.

  What this still does **not** do: deliver `#MF`. There is no exception
  delivery and no FERR path anywhere in emu88, so an unmasked exception is
  visible to a program that polls `FNSTSW`, and to nothing else.

- **The x87 defect pass**: nine defects found by a differential hunt against the
  host x87 after the 80-bit rewrite landed, fixed together rather than one at a
  time. **This moves the core and dosiz compiles it** - `emu88/emu88_f80.h` and
  `emu88/emu88_fpu.cc`, both on the six-file list.

  Two were wrong **answers**, four were wrong **reports**, and three were the
  masked stack-fault response - which turned out to be one rule applied in
  three places rather than three separate defects.

  - `FYL2XP1` was grossly wrong across `-1 < x <= -1/2`. It reduces with
    `t = x/(2+x)` and evaluates `atanh(t)` from a twenty-term table covering
    `|t| <= 1/3`, but that bound is `x >= -1/2`, not `|x| <= 1` as the table's
    own comment claimed - at `x = -0.9`, `t` is `-0.818`. The only guard bailed
    out to `log2(1+x)` at `|x| >= 1`, so the whole band in between ran a
    diverged series: 4.3e-11 relative error at `x = -0.75`, 6.7% at `-0.99` and
    **29% at `-0.999`**, against a real 387 that is correct across all of it.
    The fallback is exact there - `1 + x` is exact by Sterbenz over that band -
    so widening the guard costs nothing. The comment is corrected too.
  - The 32-bit **real-address-mode** environment image put the pointer's high
    bits at bits 12:15, which is the **16-bit** layout. The 32-bit one puts
    them at bits 27:16 with twelve bits of room. `FNSTENV`/`FNSAVE` wrote them
    in the wrong place and `FLDENV`/`FRSTOR` read them back from the same wrong
    place, so the image round-tripped self-consistently and matched no real
    387; a twenty-one-bit linear address also lost its top bits on the way
    through. The bullet above this one asserted the old behaviour was right,
    and is marked rather than deleted.
  - **Tininess was decided on the denormal grid**, in two separate places. The
    IEEE rule is tininess *after rounding as if the exponent range were
    unbounded*; reading the delivered result's J bit instead calls a value that
    denormal-grid rounding lifted back up to `2^-16382` normal, and loses `#U`.
    `f80_round_pack` had it, so `FMUL`, `FDIV` and `FSCALE` dropped the flag,
    and `f80_to_ieee` had it independently, so `FST`/`FSTP m32real`/`m64real`
    dropped it at the destination's own subnormal boundary. Both now re-round
    at the same precision with no exponent bound and test that. It matters that
    the second rounding is at the *working* precision: under `PC=24` and
    `PC=53` the unbounded rounding really does carry out to the smallest
    normal, so hardware reports no `#U` either, and a blanket "inexact down
    here implies `#U`" is wrong in the other direction.
  - `FCHS` and `FABS` on an empty `ST(0)` delivered a **positive QNaN**. They
    are the only two x87 instructions here that reach the sign bit without
    going through an `f80_*` routine, so they are the only two that could
    deform the `#IS` substitute: the masked response is the indefinite,
    `FFFF:C000000000000000`, and flipping or clearing its sign leaves
    `7FFF:C000000000000000`. The status word was already right, so a guest
    checking `FNSTSW` saw nothing and a guest checking the value saw a QNaN
    that is not the indefinite.
  - **Four more undocumented alias groups decoded to nothing**: `D9 D8-DF`
    (`FSTP1`), `DF C8-CF` (`FXCH7`), `DF D0-D7` (`FSTP8`) and `DF D8-DF`
    (`FSTP9`). Three of the four **pop**, which is the `FFREEP` failure mode
    this file already fixed once - a program using one to discard `ST(0)` left
    the stack one deeper every pass, and a stack overflow after eight. All four
    were measured on the host first, which is also how `D9 DA` was shown to be
    `FSTP ST(2)` and not a bare pop.

  - **`FNSTENV` kept writing past a fault.** `check_segment_write` lets an
    access through once an exception is already pending, which is why the
    `FNSAVE` register loop and the `FBSTP` byte loop carry
    `&& !fault_abort()`. `fpu_store_env`'s seven fields carried nothing, so
    once one field faulted the rest were let through with the offset wrapped to
    sixteen bits and landed at the **start of the segment** - memory the
    instruction never named. `FNSTENV [FFF8]` in real mode wrote four bytes to
    `DS:0002`. `execute_fpu` restores the FPU state on a fault but cannot
    un-write guest memory. Every field is guarded now, `FNSAVE` included,
    because its guarded register loop sat behind an unguarded environment.
  - **`FCMOVcc` ignored the masked `#IS` response.** Both operands are read
    whatever the condition says, so an empty one is a stack underflow either
    way - but the instruction then stored the *other* operand's real value.
    Hardware delivers the indefinite to `ST(0)` whichever operand was empty and
    whatever the condition decided, and tags it `SPECIAL`; measured at `CF=0`
    and `CF=1`, on both the `DA` and `DB` encodings.
  - **The two-result instructions left half a result behind.** `FXTRACT`,
    `FPTAN` and `FSINCOS` write one result and push the other, so an
    overflowing push replaced only the pushed value and left the first result
    standing beside the indefinite. `FPTAN` and `FSINCOS` were worse: the
    transcendental had already run and its inexactness was still in the
    context, so the status word got a `PE` the instruction never earned.
    Hardware gives **both** destinations the indefinite and reports `IE|SF`
    plus the `C1` direction bit alone - `SW=3A41` on a full stack, `3841` on an
    empty one, `PE` clear in both.

  **How they were found, and what that says about the harnesses.** A fan-out of
  differential probes against the host x87, each required to reproduce a wrong
  answer before reporting it. Every one of the six sits in a place the existing
  suites structurally could not reach, and two of those places are worth
  naming. The tininess cases need an exact result in the last half-ulp below
  the boundary, which random operands hit with probability near zero: a
  540,000-case random sweep at the denormal boundary passes against the
  **broken** code. And `f80_unit`'s `FYL2XP1` generator draws exponents in
  `[-70, -3]`, so it cannot produce `|x| >= 1/8` at all, let alone the band that
  was broken.

  So `tests/f80_unit.cc` gains an `oracle_boundaries()` that **enumerates**
  rather than samples - the last ulps below `2^-16382` for `FMUL`/`FDIV`/
  `FSCALE`, the same below both narrower destinations for the stores, and
  `FYL2XP1` across its whole domain - and `tests/fpu_test.cc` gains the four
  alias groups, the two stack-underflow sign cases and a 32-bit real-mode
  environment fixture whose pointers actually exceed 2^20. 50 checks to 53 and
  577 to 608. The whole set was run against the unfixed core first, which is
  the only reason to believe any of it: 3 of `f80_unit`'s 53 red and 19 of
  `fpu_test`'s 608, with 192 tininess mismatches and 3,072 store mismatches
  behind two of those three.

- **The x87 defect pass, second round**: three more from the same hunt, and one
  of them is the rule the first round applied in three places, applied in a
  fourth. Moves `emu88/emu88_fpu.cc`, `emu88/emu88.cc` and nothing else that
  dosiz compiles.

  - **A stack fault did not outrank a surviving NaN.** `fpu_get` substitutes
    the indefinite for an empty operand, but the `f80` primitive then ran its
    ordinary two-NaN tie-break *on the substitute*, so any QNaN in the live
    register with a significand above `C000000000000000` won and landed in the
    destination. The masked `#IS` response is the indefinite, unconditionally.
    44 arithmetic result-writes go through a new `WRR` macro that says so.

    It is deliberately **not** applied to `FXCH` or `FST ST(i)`, and that is the
    part worth recording: the host was measured first, and it exchanges *with*
    the substitute rather than flooding both registers - `FXCH ST(1)` with
    `ST(1)` empty leaves the indefinite in `ST(0)` and the live `1.0` in
    `ST(1)`. A blanket rule at the write site would have destroyed the live
    operand. The macro also evaluates its value into a temporary before testing
    the fault, because several call sites read their operands *inside* the
    argument - `WR(1, f80_yl2x(RD(1), RD(0), c))` - so a test written the
    obvious way would never have fired.
  - **`#D` was not suppressed by a higher-priority exception.** The `f80`
    primitives already do this within one call, but two paths reached the
    status word with a stale `DE` beside a higher exception: the memory-operand
    helpers raise `DE` into the same context *before* the arithmetic decides to
    raise `#IA` or `#Z`, and a stack fault discards the arithmetic's result
    while its `DE` stays behind. The host reports `IE` alone for `FLD m32` of a
    denormal onto a full stack and for `FADD m32` of one with `ST(0)` empty,
    and `DE|PE` for the same `FADD` against a live `ST(0)`.
  - **The constructor left the whole `FPUState` indeterminate.** `FPUState` has
    no default member initialisers and `f80` is a plain aggregate, so the
    control word, the status word, all eight tags and all eight registers held
    whatever was in the storage until `reset()` ran. Latent rather than live -
    `dos_machine`'s constructor resets, and every harness resets through
    `setup()` - but a poisoned `sw` alone decides `TOP` out of bits 13:11, so
    the first `FLD` would take the stack-overflow path against tags nobody set.
    `emu88::emu88()` calls `fpu_power_on()` now. This one carries no assertion:
    asserting it needs placement-new over poisoned storage, which is a test
    about the language rather than about the 387.

  `tests/fpu_test.cc` 608 checks to 616, and the three that can be asserted
  were red against the previous commit's core first.

- **The transcendentals answer to the control word now**, which closes three of
  the four counts against them and leaves the fourth open on purpose. Moves
  `emu88/emu88_f80.h`; dosiz compiles it.

  Every one of the eight computed through internal helpers that build a private
  context from a hard-coded `0x037F` and discard its flags, then hard-coded
  `c.flags |= F80_PE` on the way out. So rounding control reached none of them,
  `#U` and `#O` of the final result were never reported, `C1` was never set, and
  `#P` was reported even when the answer was exact. All four were measured
  against the host before anything was changed.

  The fix is one shared shape: each entry point's **last** arithmetic operation
  now rounds in a context built from the caller's control word, and everything
  before it stays nearest-even in a throwaway. Getting "last" right is most of
  the work - `f80_sincos` picks which of the sine and cosine series feeds which
  output **by quadrant**, so in quadrants 1 and 3 the sine comes out of the
  cosine series and the rounding context has to follow the value rather than the
  routine; `f80_patan`'s last step is one of three depending on the operands;
  and `f80_ptan`'s sine and cosine are intermediates, so only the divide rounds.
  Precision control is deliberately **not** applied - the host was measured and a
  387 ignores `PC` for these - so every one of these roundings is at an explicit
  64 bits.

  **What that buys, exactly:** `FYL2X` of a power of two is exact and now raises
  nothing where it used to raise `#P`; an overflowing `FYL2X` now reports `#O`;
  and all four rounding modes now produce different answers where they used to
  produce identical bits. The ulp figures are **unchanged** - 3.0, 2.0, 3.0,
  2.0, 2.0, 2.0, 3.0, 2.0 - which is the point: the internal helpers already
  rounded to nearest at 64 bits, so at the default control word this is a no-op
  on values and only the flags and the directed modes move.

  **What it does not buy, measured rather than assumed.** `#U` for a tiny result
  is still missing: `F2XM1` of `2^-16382` reports `#P` where the host reports
  `#U|#P`, because that underflow happens in an intermediate multiply that has
  already been denormalised in a throwaway context by the time the last step
  runs. And `C1` is derived from a real rounding now instead of being hard zero,
  but its agreement with hardware is unchanged at about chance - it depends on
  which way *our* approximation rounds, and ours is a couple of ulp from the
  host's. Neither is claimed as fixed.

  *(**Both sentences above are wrong** and are left standing rather than edited,
  because the way they were wrong is the useful part. The `#U` mechanism is not
  the intermediate multiply — that multiply raises nothing, so forwarding its
  flags would have changed nothing — and the miss covers seven forms, not one.
  And `C1` was never "about chance": that number came from a probe reading
  `FNSTSW` after an `FSTPT`, which clears `C1`, so it reported `C1 = 0` for
  everything including the `FSQRT` control. Both are corrected in the
  2026-08-29 transcendental entry above. A wrong measurement that says a defect
  is unfixable costs more than no measurement.)*

  Two things went wrong inside this change and are worth recording because both
  looked like progress. Moving the final rounding into the caller's context made
  it see the internal head/tail pieces, so `F2XM1` of the smallest **normal**
  started reporting `#D` - a denormal operand report for an operand that was not
  denormal. Only what a rounding can legitimately raise is merged back now.
  And removing the hard-coded `#P` lost it wherever the last step happens to be
  exact while the function is irrational, which is most of them; `#P` here means
  "this result is not representable", which no final rounding can derive. It is
  restored, with an exactness test on `FYL2X` for the one case - `x` a power of
  two - where it genuinely has to be suppressed.

  `tests/f80_unit.cc` gains a fourth section to `oracle_boundaries()` for what
  the transcendentals REPORT, which nothing graded before:
  `oracle_transcendental` measures ulps and runs only `cw=0x037F`, so no check
  in this repository could see a rounding-control bug or a wrong flag. 53 checks
  to 56, all three red against the previous commit's core first.

- **The logarithm shortcuts, `f80_mul2`'s tail, and a wrong warning withdrawn.**
  Moves `emu88/emu88_f80.h`, which dosiz compiles.

  Five shortcut returns in `FYL2X` and `FYL2XP1` - the paths that answer without
  going near the series - were wrong, and all five are graded against the host
  exactly now, value, sign of zero and flags:

  - `FYL2X(+0, 0.5)` returned `+0`. `log2(x)` is **negative** for `0 < x < 1`, so
    the product's sign follows the sign of the logarithm rather than the sign of
    `x`; the host returns `-0`.
  - `FYL2XP1(+inf, +0)` returned a signed zero. That is `0 * inf`; the host
    raises `#IA` and delivers the indefinite. The zero-`x` shortcut fired before
    anything looked at `y`.
  - `FYL2XP1` of a bottom-of-range denormal returned **zero**. The reduction
    `t = x/(2+x)` is about `x/2`, which for a denormal underflows to nothing and
    takes the whole result with it. There is a linear path for `|x| < 2^-66` now,
    where the series past its first term is under half an ulp of `y*x*log2(e)`
    anyway - the host returns the smallest denormal with `#U|#P`, and so does
    this.
  - `#D` was skipped whenever the other operand short-circuited the result. It
    belongs before those shortcuts and after the `#IA`/`#Z` paths, which is the
    priority rule the rest of the file already follows.

  **`f80_mul2`'s tail was wrong by exactly half an ulp of the head** whenever
  rounding the head carried out of 64 bits. The tail belongs to the scale the
  product had *before* the carry; the code adjusted the exponent first, then read
  the tail against the adjusted one and halved it as well. For
  `0xFFFFFFFFFFFFFFFE:3FFF x 0x8000000000000001:3FFF` that came back `2^62 + 1`
  times too large. Every transcendental that carries a head/tail pair uses this.
  It is graded against **exact 128-bit integer arithmetic** rather than against
  the host, because no instruction exposes a double-double product - the oracle
  has to be the product itself.

  **And a warning in `todo.txt` is withdrawn, which is the part worth reading.**
  That file told anyone picking up the remaining findings that up to seven of
  them were probably not defects, because the hunt that produced them returned
  seven refutations it had not attached to particular findings. A second pass
  built a probe per finding against the host and **confirmed every one of the
  seventeen at high confidence, with none refuted**. The seven refutations must
  have landed on findings that were fixed while that run was still going. The
  warning was wrong, it was discouraging work on real defects, and it is
  corrected in place rather than deleted.

  Two of those seventeen came back with the *report* corrected rather than
  merely confirmed, and that is now recorded with them: the two unmasked-
  exception entries are **one** change, because whether `#P` belongs on an
  unmasked overflow depends on the inexactness of the 24576-biased value and so
  cannot be decided without computing it - and the fix originally proposed for
  it, "do not set `#P` for an unmasked overflow", is wrong as stated. It breaks
  the 82 of 738 cases in that verifier's own sweep where the biased result
  really is inexact.

  `tests/f80_unit.cc` 56 checks to 58; both new ones red against the previous
  commit's core first.

- **Three more from the re-verified list**: the `C1` fault priority, the
  reserved halves of the 32-bit environment image, and the order in which a
  signalling NaN gets quieted. Moves `emu88/emu88_fpu.cc`, `emu88/emu88_f80.h`
  and `emu88/emu88.h`, all of which dosiz compiles.

  - **`C1` reported the second fault instead of the first.** One instruction can
    raise both: `FLD ST(i)` with the stack full and `ST(i)` empty reads an empty
    register and then pushes onto a full one, and so do `FXTRACT` and `FPTAN`.
    `fpu_get` already latched an underflow and refused to be overwritten;
    `fpu_push` overwrote it unconditionally, so the later overflow won and `C1`
    came out set. The host reports the underflow - `SW=3841`, `C1` clear.
  - **The reserved upper halves of the 32-bit environment image are ONES.**
    `FNINIT` then `FNSTENV32` gives `+00=FFFF037F`, `+04=FFFF0000`,
    `+08=FFFFFFFF`, `+18=FFFF0000` on the host; this wrote zeroes. Measured with
    the destination pre-poisoned with `0x00`, `0xAA` and `0x5A` and identical
    every time, so they are actively stored rather than left over. The three
    dwords that carry a full 32 bits - `FIP`, the selector-and-opcode, `FDP` -
    have no reserved half and get none. Only the protected form could be
    measured, because this host cannot leave protected mode; `CW`/`SW`/`TW` sit
    at the same offsets in both 32-bit layouts so the change covers both, and
    the real-address-mode pointer packing below is left alone rather than
    guessed at.
  - **A signalling NaN arriving as an m32/m64 operand was quieted too early.**
    The two-NaN tie-break compares significands, and setting the quiet bit first
    lifts the memory NaN above `ST(0)`: `FADD m32real` of the SNaN `7F800001`
    against an `ST(0)` of `C000000000000001` delivered the memory NaN where the
    host delivers `ST(0)`'s.

    Removing the quieting outright would have been wrong - `FLD m32real` of an
    SNaN has to deliver the QUIETED NaN, because nothing downstream quiets it
    there. The distinction is load versus operand, so `f80_from_ieee` takes a
    `quiet_snan` flag defaulting to the load behaviour and only the two
    arithmetic escapes pass `false`; `f80_prop_nan2` already quiets whichever
    NaN it selects, so the arithmetic path loses nothing by waiting. The control
    case - a *larger quiet* m32 NaN, which legitimately does win - is asserted
    beside it.

  `tests/fpu_test.cc` 616 checks to 626, six of them red against the previous
  commit's core first. (The other four are the control cases, which pass either
  way and are there to keep the fixes honest.)

- **`FLDENV`/`FRSTOR` regenerate the tag word, and `FPREM` normalises a
  pseudo-denormal.** Moves `emu88/emu88_fpu.cc` and `emu88/emu88_f80.h`.

  - **Only the EMPTY decision comes from a loaded tag word.** A real x87
    re-derives valid/zero/special from what is actually in each register, so an
    image whose tag word disagrees with its register data does not round-trip
    the way it was written. `fpu_load_env` copied all four encodings verbatim.
    All five cases were measured on the host with a hand-built 108-byte image
    rather than derived: tag word `0x0000`, `0x5555` and `0xAAAA` over registers
    holding `+0.0`, `1.0`, an SNaN and a denormal all come back **`0x55A1`**,
    while `0xFFFF` and `0x0003` come back unchanged because EMPTY is honoured.
    `FRSTOR` re-runs the retag after its eighty register bytes are in, since the
    data does not exist when the environment is read.

    The code carried a comment asserting the opposite - that the tags "must NOT
    be recomputed from the values". Its *reasoning* was right, and is kept: a
    saved EMPTY slot holds an arbitrary bit pattern and re-tagging it would
    resurrect it. That is precisely why the rule keeps EMPTY and regenerates
    only the other three.
  - **`FPREM`/`FPREM1` handed back a pseudo-denormal dividend unnormalised.**
    Exponent field 0 with the significand's J bit set has an exactly equal
    normalised form at biased exponent 1, and several paths `return a` straight
    back. `FPREM` of `0000:8000000000000000` by `1.0` gives
    `0001:8000000000000000` on the host. The value is identical either way,
    which is why the flags always agreed and only the encoding was wrong.

    The report defined the case as "significand MSB clear with a nonzero
    exponent". That is an **unnormal**, not a pseudo-denormal, and emu88 already
    sends those to `#IA` and the indefinite - so an unnormal row is asserted
    beside the fix, to catch an over-reaching version of it.

  `tests/f80_unit.cc` 58 checks to 59 and `tests/fpu_test.cc` 626 to 627, both
  red against the previous commit first.

  One thing went wrong writing the `FPREM` assertion and is worth recording,
  because it corrupted results rather than failing outright: the inline asm
  declared `st(1)` clobbered. `FPREM` does not pop, unlike `FYL2X`, so that told
  the compiler not to retire the divisor and leaked an x87 stack slot per
  iteration - enough, after six, to wreck every test that ran afterwards. It
  surfaced as the transcendental worst-case ulps jumping to 1e9. Those figures
  are printed on every run so that a regression shows up as a number changing
  rather than as a check still passing, and this is the first time that has
  actually earned its keep.

- **The unmasked `#O` and `#U` responses**, which closes the last finding on the
  re-verified list. Moves `emu88/emu88_f80.h`.

  An unmasked response is not the masked one with a flag added. A 387 delivers
  the result at **full destination precision** with the biased exponent moved by
  `-24576` for an overflow or `+24576` for an underflow, so no denormalisation
  happens and the delivered value is usually exact. Two consequences that both
  read as backwards until you see them: an unmasked `#U` carries **no** `#P`
  where the masked one does, and an **exact** tiny result still raises `#U` -
  tininess alone is enough, it does not also have to be inexact.

  Measured: `FSCALE` of `0001:8000000000000000` by `-1` gives
  `0000:4000000000000000` and raises nothing when masked, and
  `6000:8000000000000000` with `#U` alone when unmasked.

      band                     HEAD          fixed
      #U unmasked, tininess    800 diffs     0
      #O unmasked, overflow    320 diffs     0
      masked, both bands         0 diffs     0

  That last row is the one that matters. This lives in `f80_round_pack`, which
  every arithmetic path in the file funnels through, so a change that fixed the
  unmasked cases by disturbing the masked ones would be worse than the defect it
  cured. The masked rows are inside the committed assertion for that reason
  rather than as padding, and `tests/f80_unit.cc` goes from 59 checks to 60 with
  2,304 cases in the new one.

  **Scope, stated plainly:** emu88 delivers no `#MF` and has no FERR pin, so
  none of this is visible to a guest that does not unmask an exception and then
  poll `FNSTSW`. It is still what the guest is told when it looks.

  The first sweep written for this reported **zero** divergences in the tininess
  band and nearly had the finding recorded as overstated. `FSCALE` truncates its
  operand toward zero and the sweep was passing `2^-(n+1)` - 0.25, which
  truncates to **zero**, so nothing was ever scaled. Printing the actual values
  instead of the counts exposed it in one run. Counts hide that class of
  mistake; values do not.

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
- **The third and last dead-code sweep**, which closes what the second one
  deliberately left. That list was in `todo.txt` and it was incomplete: the
  count it was built from was `grep -c '^[[:space:]]*;[[:space:]]*$'`, which
  finds a deleted trace only where the leftover was a bare semicolon.

  - **Eight `{ }`-bodied husks**, which no warning finds - `-Wempty-body` fires
    on the semicolon form and not the brace form. **Five were restored, not
    deleted**, because an empty body there is worse than no code at all. The
    two `if (safety >= N) { }` guards in `dos_dpmi.cc` bounded a runaway
    real-mode loop and then reported nothing, so a client that hung came back
    silently; both print again, naming the vector, `CS:IP`, `SS:SP` and the
    handler. The three `else { }` chains in `emu88_fpu.cc` each once named an
    unhandled x87 opcode (`git show 073605d:emu88/emu88_fpu.cc`), so an
    encoding this file does not implement had become a silent no-op - the worst
    way to fail, because the program carries on with a stale `ST(0)`. They
    report again, capped at 16 so an unhandled opcode in an inner loop cannot
    outrun the program. Restoring a diagnostic is worth as little as the
    diagnostic it replaces if nobody checks it fires, so it was checked:
    executing `DB E0` through the decoder prints
    `[FPU] unhandled DB register op: E0`, and the eleven harnesses produce no
    such line between them. The other three were pure debug and went: the
    divide-by-zero log in `dos_machine.cc` under a comment promising "full
    context", and two in `emu88_pmode.cc` - one inside an `#ifdef PAGING_DEBUG`
    nothing defines, one the sole read of `exc_dispatch_trace`.
  - **Ten increment-only rate limiters** the husk count never looked for, of
    the shape `static int x = 0; if (x < N) { x++; }` - the counter for a trace
    that was deleted while its budget stayed. `exc_trace` and `dpmi31_log`
    (`dos_dpmi.cc`), `adlib_read_log` (`dos_machine.cc`), `retf_trace`,
    `v86_int21_count`, `iret_trace` and an `il` (`emu88.cc`), `pf_pte_log`,
    `idt_gp_log` and `esp_change_log` (`emu88_pmode.cc`). Three were not free:
    `retf_trace`'s guard compared `insn_ip` against a hard-coded DOS4GW address
    on **every** `RETF`, and `esp_change_log`'s read `get_esp()` twice around
    every protected-mode exception dispatch.
  - **An `IVT[21h]` watchpoint** at the head of `dpmi_int31h` that fetched two
    memory words on every DPMI call, compared them to a `static`, and did
    nothing with the answer.
  - **Ten members written and never read**, including four never written at
    all. `rm_trace_count`, `dpmi_trace_func`, `int2f_1687_trace_pending`,
    `int2f_trace_ret_cs`, `int2f_trace_ret_ip` and `exc_dispatch_trace`, plus
    `dpmi_trace_ret_cs`, `dpmi_trace_ret_eip`, `dpmi_trace_es_base` and
    `dpmi_trace_edi`, which existed only as declarations. The whole `if (ax ==
    0x1687)` block in `dos_machine::do_interrupt` went with them - its comment
    read "Set up post-return trace to log what the handler returns", and the
    post-return trace had been deleted while the setup stayed. `todo.txt` had
    called deleting these "a decision, not a cleanup" and left them; the
    decision is that a member that reads like a feature and is not one is worse
    than no member.
  - `emu88_mem.cc`'s `#include <cstdio>`, unused.

  Nothing suppressed - no `-Wno-*`, no `[[maybe_unused]]`, no `(void)` casts.
  After it, `grep` for all three husk shapes across `emu88/` returns nothing.
  One piece was deliberately left: `emu88_trace.h`'s four virtual hooks, the
  `trace` member and `set_trace()`, and the `debug` flag beside them. Nothing
  has ever called a hook or installed a tracer - but unlike the counters this
  is a coherent designed seam rather than a leftover, and removing a public
  virtual from a class a second product compiles should be somebody's decision.
  It is in `todo.txt` with that reasoning.
  Held to `tests/run_suites.sh` throughout, and to `tests/check_dosiz.sh`:
  SingleStepTests 1,758,402 of 1,758,699 matching the baseline exactly,
  `test386.asm` reaching POST `0xFF` with its arithmetic output identical to
  the reference across all 44,926 lines, and dosiz building with zero warnings
  out of `emu88/` and passing all 37 of its CI fixtures.
- **`-Wextra` is a gate now, not a measurement.** `tests/build.sh` passed
  `-Wall` only, so the "zero warnings under `-Wall -Wextra`" above was a
  command somebody ran once and nothing held. The harness side cost six
  warnings to add it, which is why it had not been done: three `-Wsign-compare`
  out of the vendored `tests/vendor/mooreader.h`, suppressed with a
  `#pragma GCC diagnostic` around the `#include` in `sst386.cc` rather than by
  patching the header, so the vendored copy stays byte-identical to upstream
  and re-vendoring needs no re-patching; a `-Wcomment` each from `sb_unit.cc`
  and `uart_unit.cc`, where a trailing backslash on a `//` line spliced the
  next line into the comment; and a `-Wunused-but-set-variable` in
  `test386_run.cc` for a liveness budget that was reset and never incremented
  or tested. All eleven `emu88/*.cc` and all eleven harnesses now build clean
  under `-Wall -Wextra`, and `.github/workflows/tests.yml` runs that build.
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

- **The repository stopped describing a product that no longer exists.**
  `README.md` opened with "using DOSBox Staging as the emulation engine" and
  "All PC emulation is provided by DOSBox Staging"; `CLAUDE.md`'s first line
  read "qxDOS - DOSBox-based DOS emulator for iOS/Mac". emu88 has been the
  default since 932af28. Both are rewritten, with the false sentences quoted
  and retracted in place rather than edited away, which is the standard
  `tests/README.md` set.

  - `README.md` gains an **Emulation backends** section that says which one is
    the default and what each is still for, a **What emu88 does not do**
    section naming the `double` x87 register stack and the rest rather than
    leaving them to be discovered, an Architecture block with the emu88 branch
    it never had, and a **Building** section that describes the standalone
    emu88 loop - the only half of this repository that builds without a Mac.
  - `CLAUDE.md` gains its **Key Directories** entries for `emu88/`, `tests/`,
    `scripts/` and `disk-content/`, which had been missing entirely: the
    17,244-line core and its eleven harnesses were absent from the file a
    newcomer reads to find their way.
  - `RIGHTS.md`, `qxDOS/Views/ContentView.swift` and
    `qxDOS/Resources/help_about_freedos.md` called emu88 "the alternate
    hardware backend", which is the reverse of the truth, in text a user can
    read in the app. The Swift and help-file edits are **string changes that
    were not compiled** - there is no Xcode here.
- **`CLAUDE.md` states the emu88 obligation as a rule**, which nothing in this
  repository had ever done. dosiz compiles six files - 9,102 lines - straight
  out of this working tree through a relative path in its own
  `src/CMakeLists.txt`: no submodule, no vendored copy, no version constant, no
  checksum, no build stamp. `dosiz/CLAUDE.md` says which way the obligation runs
  ("emu88 belongs to qxDOS. Do not fix emu88 bugs from this repo") and points at
  `qxDOS/tests/` as the gate, and this side said nothing at all. It now carries
  three numbered rules for any change under `emu88/`: validate with
  `tests/run_suites.sh` first, say in the changelog entry that the core moved
  and name the files, and do not fix an emu88 bug as a drive-by.
- **`docs/hardware-roadmap.md`** said "Status reflects the current tree" and
  then contradicted itself: its own summary paragraph listed AdLib/OPL2+OPL3,
  Sound Blaster, PC speaker, joystick, 16550 UART, LPT and Hercules as done,
  while every table below it still headed audio "the largest gap (nothing
  currently produces sound)", said "There is no host audio sink for emu88 yet",
  and marked six implemented devices as not started. All of that predated
  992c7fc, ea990a9 and ac72ee6. The tables are rewritten against the source and
  the harnesses, each row's evidence named; a new marker separates "implemented
  and tested here" from "in the tree and unbuildable on this machine", so the
  CoreAudio sink and the gamepad poll are no longer scored as if they had been
  run. The caveats that are real are kept and sharpened rather than dropped -
  the OPL core is clean-room and not bit-exact, Sound Blaster has no ADPCM, the
  UART transmits with no baud throttle.
- **`tests/README.md`** is the one document in the repo written to be checked
  rather than believed, and it is the model the rest now follows. Three numbers
  in it were wrong and are fixed: the SingleStepTests pass rate read
  1,758,403 / 1,758,700, one off in both halves; the VESA pan clamp was
  described as "checked under AddressSanitizer" when `fsanitize` appeared
  nowhere in the repository, so whatever was done was a one-off nothing could
  reproduce; and the corpus was described as ~600 MB where `tests/data`
  measured 1.2 GB, because `fetch_tests.sh` left 567 MB of clone history behind.
  All three corrections are made in place, below the sentence they retract. Its
  two defect ledgers are rewritten as a record of what was found and fixed.
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
- **`CLAUDE.md`** gained the build-bump procedure (81e44d5), and the emu88
  rewrite and the emu88-obligation rule described above.
- **`todo.txt`** was seventeen open items and is now ten. Nine closed outright
  and two were narrowed - the x87 entry lost its nine defects and kept its
  register format, the dosiz entry gained a gate and kept its pin. What is left
  is left because it needs a machine this one is not (a Mac, a real device, the
  DOSBox submodule) or a decision that is the owner's (the release tagging
  scheme), plus two standing statements of scope that exist so a pass rate is
  never quoted without them, and two things found while measuring the rest and
  deliberately left, both because closing them is a decision about what the
  product does rather than a cleanup: the pre-992c7fc detect-only AdLib and
  Sound Blaster stubs still answer when no sound card is configured, and the
  `emu88_trace` hook plus the `debug` flag are a designed extension point that
  nothing has ever installed a tracer into.

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
