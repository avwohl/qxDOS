# emu88 CPU test harnesses

Standalone validation of the **emu88** 386 CPU core (built without the iOS/DOSBox
app) against two industry-standard suites.

## Setup

```sh
bash tests/fetch_tests.sh   # downloads test data into tests/data/ (gitignored, ~600MB)
bash tests/build.sh         # builds tests/build/sst386
```

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

DIV/IDIV overflow-boundary microcode and the multi-prefix / load-far / far-jump
`#GP` corners that earlier dominated this list are now **100%** correct. Reaching
a literal 100% on the per-instruction suite would require bit-exact replication
of the 386's unpublished multiplier-array state and its prefetch queue.

## 2. test386.asm — full-system (all modes)

PCjs/barotto diagnostic ROM exercising real mode → protected mode → **paging** →
**V86 mode**, GDT/LDT, call gates, and TSS task switching. Diagnostic codes go to
POST port `0x190`; ASCII arithmetic results to port `0xE9`.

```sh
clang++ -std=c++20 -O2 -I emu88 tests/test386_run.cc emu88/emu88.cc \
    emu88/emu88_pmode.cc emu88/emu88_fpu.cc emu88/emu88_mem.cc -o tests/build/test386
tests/build/test386 2>/dev/null
```

**Result: PASS** — reaches POST `0xFF` and the `0xEE` arithmetic output matches
`test386-EE-reference.txt` exactly.
