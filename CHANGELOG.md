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
  does not explain - nine of those, all since fixed.

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
