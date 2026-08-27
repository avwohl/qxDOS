# emu88 CPU test harnesses

Standalone validation of the **emu88** 386 CPU core (built without the iOS/DOSBox
app) against two industry-standard suites.

## Setup

```sh
bash tests/fetch_tests.sh   # downloads test data into tests/data/ (gitignored, ~600MB)
bash tests/build.sh         # builds all seven harnesses into tests/build/
bash tests/run_suites.sh    # runs them and holds them to their recorded scores
```

`run_suites.sh` is what `.github/workflows/tests.yml` runs, so a green tick and
a clean local run mean the same thing. `build.sh` uses `clang++` when it is
present and `g++` otherwise.

## 1. SingleStepTests/80386 — per-instruction (real mode)

Exhaustive per-instruction tests (1.76M cases): load CPU state, run one
instruction, compare every register + RAM byte (undefined flags masked via the
suite's per-file mask chunk).

```sh
tests/build/sst386 --summary --revoke tests/data/80386/revocation_list.txt \
    tests/data/80386/v1_ex_real_mode
# focused:  --only 0FAF --show 8 --diag <dir>
```

Current pass rate: **99.983%** (1,758,403 / 1,758,700). Every *architecturally
defined* behavior — results, defined flags, all addressing modes, all
addressing-/operand-size prefix combinations, segment-limit and 64KB-boundary
exceptions (`#GP`/`#SS` attributed to the correct effective segment), instruction
fetches crossing the code-segment limit, string-operation boundary faults, and
all mode transitions — is correct. The remaining ~0.017% (11 of 941 opcode files)
is exclusively *officially-undefined* or *environment-specific* behavior captured
from one particular 386EX test bench:

- **IMUL undefined flags** (`0FAF`, `69`, `6B` + prefixes; ~287): the 386
  multiplier latches SF/PF/AF deterministically out of its sequential signed
  add-and-shift datapath. Modelled bit-for-bit to **~96%** (a guard-bit
  accumulator with a sign-extended multiplicand reproduces the partial-sum sign
  across the loop); `CF`/`OF` and the numeric result are 100% correct. The last
  ~4% are gate-level carry-save-array artifacts — SF and PF residual failures are
  *statistically independent*, which proves no single input-derived rule remains:
  closing them needs die-level multiplier state, not yet publicly documented.
- **`IN` from peripheral ports** (`E5`, `66E5`; 6): the expected value is whatever
  device sat on that port on the capture bench (e.g. `7FFFFFFF` on port `0x1F`);
  it is peripheral data, not CPU behavior, and is not reproducible.
- **Self-modifying `REP` within the prefetch window** (`67AB`/`67A5`/`6766AB`/
  `6766A5`, one case each; 4): a 32-bit-address `REP STOSx`/`MOVSx` whose
  destination overlaps `CS` overwrites its own trailing `HLT`, but the 386 had
  already prefetched it, so real hardware still halts. Reproducing this needs a
  cycle-accurate prefetch-queue model. The architectural result (memory, `ECX`,
  `EDI`) is correct.

The multi-prefix / load-far / far-jump `#GP` corners that earlier dominated this
list are now **100%** correct. DIV/IDIV overflow-boundary microcode was claimed
here as 100% correct and was not: this suite scores identically whether the
`r/m16`/`r/m32` non-faulting band is present or absent — all four `F7.7`
variants are 2500/2500 either way — so it never had an opinion, and test386.asm
turned out to disagree with the band in four cases. See the correction under
test386.asm below. Reaching
a literal 100% on the per-instruction suite would require bit-exact replication
of the 386's unpublished multiplier-array state and its prefetch queue.

## 2. test386.asm — full-system (all modes)

PCjs/barotto diagnostic ROM exercising real mode → protected mode → **paging** →
**V86 mode**, GDT/LDT, call gates, and TSS task switching. Diagnostic codes go to
POST port `0x190`; ASCII arithmetic results to port `0xE9`.

```sh
bash tests/build.sh          # builds tests/build/test386 among the rest
tests/build/test386 tests/data/test386/test386.bin
```

**Result: PASS** — reaches POST `0xFF` and the `0xEE` arithmetic output matches
`test386-EE-reference.txt` exactly.

> Corrected 2026-08-26, in place rather than edited away. Both sentences above
> were false when written. The run reached POST `0xFF`, which is the harness's
> success flag and is what "PASS" was being read off — but the arithmetic output
> differed from the reference in **four** lines, every one of them an `IDIV`
> overflow that raised no `#DE`. The cause was the non-faulting band described
> under IDIV below, modelled correctly for `r/m8` and then extended to `r/m16`
> and `r/m32`, where the 386 does not behave that way. Nothing detected it
> because POST `0xFF` was the only thing checked; the reference comparison was
> never automated. It still is not — see "Not covered" below.

## 3. VESA / SVGA (VBE) — end-to-end

Drives the full machine (`emu88` + the DOS/BIOS layer) through the real INT 10h
dispatch to exercise the VESA VBE 2.0 SVGA implementation: `4F00` controller info,
`4F01` mode info (8/16-bpp masks), `4F02` set mode, `4F05` bank switch, `4F06`
logical-scanline set, `4F07` pan, and the `4F0A` protected-mode interface — whose
emitted `SetWindow`/`SetDisplayStart` routines are actually **executed** and
checked. Also covers the `0xA0000` window / `0xE0000000` linear-framebuffer VRAM
routing, the compositor's pan clamp, and the INT 33h mouse coordinate scaling.

```sh
bash tests/build.sh            # also builds tests/build/vesa_test
tests/build/vesa_test
```

**Result: PASS** — verifies the `VbeInfoBlock`/`ModeInfoBlock` byte layout, the
LFB and bank-switched window both address the 8 MB SVGA VRAM, a set-mode emits a
correctly-sized direct-color frame, a wider logical scanline (`4F06`) takes
effect, the executed `4F0A` routines update the bank/display-start, a pan past
the end of VRAM clamps to page 0 (no OOB; checked under AddressSanitizer), and
the mouse maps frame-pixel coordinates onto the guest range (SVGA 1:1; VGA mode
13h to the classic 640-wide virtual space).

## Not covered, and not automated

Written down 2026-08-26 after an `IDIV` bug sat in the gap between these suites
for as long as they have both existed.

- **SingleStepTests is real mode only.** `sst386.cc` loads every one of the
  1.76M cases with `base = sel<<4, limit 0xFFFF`. Its pass rate says nothing
  about 32-bit protected-mode instruction execution, which is where every DJGPP
  or PMODE/W client runs. Quote it with that scope attached.
- **It also never exercises exception delivery.** The corpus injects a `HALT`
  at the exception ISRs, so a fault is scored by the register and RAM state it
  leaves, not by whether the right vector was dispatched with the right frame.

Three of the five gaps this section opened with are closed as of 2026-08-27;
they are kept here, struck, because what they were is the argument for the two
that remain.

- ~~**`build.sh` does not build `test386`.**~~ It does now, alongside the other
  six harnesses.
- ~~**Nothing compares test386's output to the reference.**~~
  `tests/run_suites.sh` does, and that check is the reason it exists: the
  harness reports POST `0xFF` and stops, which is how four wrong `IDIV` lines
  read as a pass. The runner was shown to fail on exactly that regression before
  it was trusted — against a core built from before `7352fc5` it still says
  "ok reached POST 0xFF" and then fails the diff.
- ~~**There is no CI for any of this.**~~ `.github/workflows/tests.yml` builds
  the harnesses and runs `run_suites.sh` on every push and pull request, with
  the ~600 MB corpora cached.

What has NOT changed: the two gaps above these. Automating the suites does not
widen them — CI runs exactly what a person ran by hand, so 32-bit
protected-mode instruction execution and exception delivery are still covered
only by test386's full-system pass, which checks the machinery rather than every
instruction.
