# emu88 CPU test harnesses

Standalone validation of the **emu88** 386 CPU core (built without the iOS/DOSBox
app) against two industry-standard suites, plus hand-written harnesses for the
parts those suites do not reach: the VESA BIOS, the x87 FPU and the 80-bit soft
float underneath it, the DPMI host, the NE2000 network card and the PC BIOS.

One of those is different in kind from the rest and worth knowing about before
you read further. `f80_unit` does not test emu88 against a table of expected
values; it tests emu88's floating point against **the host's own x87**, because
on x86-64 `long double` is the same 80-bit format with the same control word.
See section 4b — including which parts of it use that oracle and which use
glibc's libm instead, which is not the same thing and was not distinguished
here until 2026-08-29.

## Setup

```sh
bash tests/fetch_tests.sh   # downloads test data into tests/data/ (gitignored, 585MB)
bash tests/build.sh         # builds all twelve harnesses into tests/build/
bash tests/run_suites.sh    # runs them and holds them to their recorded scores
```

`tests/data` is **585 MB** after fetching - 579 MB of SingleStepTests corpus,
6.2 MB of test386 - measured here on 2026-08-27. It used to be 1.2 GB: both
upstreams are shallow clones whose objects *are* the payload, and the 567 MB
`.git` each clone left behind was never reclaimed. `fetch_tests.sh` drops both
histories now, which is what CI had been doing by hand, so a local checkout and
a CI run hold the same bytes. Nothing the suites read was removed - they read
files and never git.

`run_suites.sh` is what `.github/workflows/tests.yml` runs, so a green tick and
a clean local run mean the same thing. `build.sh` uses `clang++` when it is
present and `g++` otherwise — **which matters, and was invisible here until
2026-08-28.** Every measurement in this file before that date was taken with
`g++` because no `clang++` was installed; once one was, `build.sh` silently
switched to it and immediately produced 44 warnings that `g++` had never
emitted (`-Wunused-const-variable`, which g++ does not raise for a namespace-
scope `static constexpr` in C++ and clang does). The tree is clean under both
now — `clang++ 21.1.8` and `g++ 15.2.0`, twelve harnesses each, zero warnings —
but the lesson is that "zero warnings" is a claim about a compiler, and this
script picks the compiler for you.

## 1. SingleStepTests/80386 — per-instruction (real mode)

Exhaustive per-instruction tests (1.76M cases): load CPU state, run one
instruction, compare every register + RAM byte (undefined flags masked via the
suite's per-file mask chunk).

```sh
tests/build/sst386 --summary --revoke tests/data/80386/revocation_list.txt \
    tests/data/80386/v1_ex_real_mode
# focused:  --only 0FAF --show 8 --diag <dir>
```

Current pass rate: **99.9831%** (1,758,402 / 1,758,699), re-measured 2026-08-27.
This pair was wrong here by one in both numerator and denominator until that
date; `tests/run_suites.sh` holds the correct 1758402 as `SST_BASELINE`, and
7352fc5's commit message has carried it since. Every *architecturally
defined* behavior — results, defined flags, all addressing modes, all
addressing-/operand-size prefix combinations, segment-limit and 64KB-boundary
exceptions (`#GP`/`#SS` attributed to the correct effective segment), instruction
fetches crossing the code-segment limit, string-operation boundary faults, and
all mode transitions — is correct. The remaining ~0.017% (11 of 941 opcode files)
is exclusively *officially-undefined* or *environment-specific* behavior captured
from one particular 386EX test bench:

- **IMUL undefined flags** (287 cases across exactly five opcode files -
  `0FAF`, `660FAF`, `670FAF`, `67660FAF` and `6769`; neither `6B` nor
  unprefixed `69` fails at all): the 386
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
> never automated. **It is now**: `tests/run_suites.sh` strips the POST and
> banner lines out of the harness's output and diffs the remainder against the
> reference, failing on any difference, and that check is the reason the runner
> exists. See "Not covered" below, where the gap this sentence opened is struck
> through.

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
the end of VRAM clamps to page 0 (no OOB), and the mouse maps frame-pixel
coordinates onto the guest range (SVGA 1:1; VGA mode 13h to the classic
640-wide virtual space).

> Corrected 2026-08-27. This paragraph said the pan clamp was "checked under
> AddressSanitizer". `fsanitize` appeared nowhere in the repository and the only
> hit for `sanitize` was that sentence, so whatever was done was a one-off that
> nothing committed could reproduce and no regression would have caught. It is a
> command now:
>
> ```sh
> ASAN=1 bash tests/build.sh && tests/build-asan/vesa_test
> ```
>
> which rebuilds every harness under `-fsanitize=address,undefined` into
> `tests/build-asan/`. Run on 2026-08-27: `vesa_test` passes with no sanitizer
> report. It is deliberately **not** in CI - the sanitized SingleStepTests run
> costs minutes rather than seconds. It is the thing to run by hand after
> touching memory routing or a frame buffer.

## 4. x87 FPU — instruction-level, and the arithmetic underneath it

Two harnesses, and the split between them matters. `tests/fpu_test.cc` owns the
DECODE, the register stack and the status word; `tests/f80_unit.cc` owns the
NUMBERS. Neither replaces the other, and until 2026-08-28 only the first
existed, which is why the arithmetic went ungraded for as long as it did.

### 4a. `tests/fpu_test.cc` — the decoder and the register file

`emu88_fpu.cc` is CPU core, so every harness above already compiled it and not
one of them ever executed an x87 opcode. This one does, by writing real
encodings to `CS:0000` and running them through `emu88::execute()` — the
`D8`–`DF` escape dispatch, the modrm decoder and the FPU handler are therefore
exercised together, rather than by calling `execute_fpu()` behind the decoder's
back.

```sh
bash tests/build.sh          # builds tests/build/fpu_test
tests/build/fpu_test
```

**Result: PASS** — 659 assertions over ~76 mnemonics: stack discipline and
`TOP` wraparound, stack overflow and underflow with the `IE`/`SF`/`C1` a 387
reports, the tag word, all three memory real formats, the seven `FLD`
constants as exact 80-bit bit patterns, every arithmetic form with the
non-commutative ones asserted in both directions, integer load/store rounding
versus `FISTTP`'s truncation, the `C0`/`C2`/`C3` codes for less-than, equal,
greater-than and NaN-unordered, `FXAM`'s full classification including the
denormal and unsupported classes, the transcendentals and their `C2`
out-of-range report, all four rounding modes, all three precision-control
settings, `FCMOVcc`, `FCOMI`, `FFREEP`, `CR0.EM`/`CR0.TS` gating,
`FNSTENV`/`FLDENV` with all seven environment fields, and `FNSAVE`/`FRSTOR` in
both the 94-byte and 108-byte forms.

Since 2026-08-28 it also covers **unmasked exceptions**, which it deliberately
did not before: section 17b that `ES` and `B` are a recomputed function of the
status and control words rather than a latch, and section 19b that an unmasked
exception is *delivered* — deferred to the next waiting instruction or `FWAIT`,
reported as a restartable fault, skipped by exactly ten no-wait encodings, and
outranking the reporting instruction's own operand fault. What this harness
cannot reach is the AT's IRQ13 route, because that lives in `dos_machine` and
this one builds a bare `emu88`; `bios_test` drives it end to end.

**Two kinds of assertion now, not three.** Until the register file was
rewritten, `emu88.h` declared `double regs[8]` — 53 mantissa bits, not 80-bit
extended — and this file carried **31 `diverge()` sites**, each pinning a value
that provably differed from a real 387 and each with a comment naming the gap.
There are none left. Every one is an ordinary `check()` on the 387's answer:

- `check()` asserts behaviour that is correct.
- `bug()` asserts the *correct* 387 behaviour for a defect recorded
  deliberately. These are red on purpose and held to `KNOWN_BUGS_EXPECTED` the
  way SingleStepTests is held to `SST_BASELINE`: fixing one **fails** the
  harness, which prints `FIXED (lower KNOWN_BUGS_EXPECTED)`, because a silent
  improvement means the number is stale. **`KNOWN_BUGS_EXPECTED` is 0.**
- `diverge()` is kept, unused, as the shape a future *deliberate* divergence
  should take. What it must not be used for again is a register format.

Section 21 of the file exists only for behaviour the 80-bit register file makes
reachable at all: gradual underflow to a denormal with `#U` and `#P`, overflow
to an infinity or to the largest finite value at the current precision, the
denormal and unsupported encoding classes, signalling NaNs, NaN propagation by
significand, and a `FNSAVE`/`FRSTOR` round trip over all eight encoding classes.

Section 22 covers something else again: what happens when the memory operand
**faults**. Real mode on a 286 or later enforces the 0xFFFF segment limit, so
an eight-byte operand at `DS:0xFFFE` runs off the end and raises `#GP` — which
is a fault, so the instruction restarts and the x87 state it re-enters with has
to be the state it left. Ten of that section's assertions fail against the
implementation as it stood before 2026-08-28: a faulting `FLD` pushed, a
faulting `FSTP` popped, a faulting `FISTP` left `#P` behind, and a faulting
`FNSAVE` re-initialised the whole FPU. One more asserts that `FBSTP` writes
nothing past the byte that faulted, which is not automatic —
`check_segment_write` lets an access through once an exception is pending, so
the rest of the field would otherwise land wherever the offset wrapped to.

### 4b. `tests/f80_unit.cc` — the soft float, against real hardware

Neither validation suite in this repository can grade an FPU. None of the 941
opcode files in `tests/data/80386/v1_ex_real_mode` begins `D8`..`DF` — the
capture bench was an 80386EX with no coprocessor — and `test386`'s reference
output has no x87 mnemonic in it. So when the register file was rewritten there
was nothing that could say whether the arithmetic underneath was right.

This is that thing. On x86-64, `long double` **is** the 80-bit format
`emu88/emu88_f80.h` implements, with the same control word, the same four
rounding modes, the same three precision-control settings and the same six
exception flags. So the host x87 is driven as an oracle: the same operation
runs both ways and the result **and the flags** are compared bit for bit.

```sh
tests/build/f80_unit          # ~3s at the default scale of 3
tests/build/f80_unit 50       # ~56s, tens of millions of cases
```

**Exact, with flags:** add, sub, mul, div, sqrt, the m32real/m64real
conversions both ways, the integer conversions both ways with their range
checks, packed BCD both ways, `FRNDINT`, `FSCALE`, `FXTRACT`, `FPREM`,
`FPREM1`, the comparisons and `FXAM`.

**Not exact:** the eight transcendentals. A real 387 does not round those
correctly either — Intel specifies about 1 ulp — so they are graded against the
host's long-double libm and held to `ULP_BOUND`, which is **6**. The worst
actually observed is **4** (`FYL2XP1`, and only at scale 50; the default run
reports 3), and the harness prints a figure per function on every run, so a
regression shows up as a number changing rather than as a check still passing.
`FSIN` of arguments up to 2^62 — where the argument reduction is the whole
difficulty — sits at 2. A spot check against an exact reference found some of
that remaining difference is glibc's rather than this file's, so the figure is
an upper bound on our error and not a measurement of it.

**Which oracle, and it is not the same one throughout.** This section opens by
saying the host x87 is driven as an oracle, and for the arithmetic and the flag
checks that is exactly what happens. `oracle_transcendental` is the exception:
it grades the eight against **glibc's long-double libm**, not against the host's
`F2XM1`/`FSIN`/`FPATAN` instructions. The distinction is not pedantic. glibc's
`atan2l` *is* the `FPATAN` instruction, so for that one the two oracles are the
same thing; its `expm1l` wraps `F2XM1` in further long-double arithmetic and is
measurably worse than the raw instruction, so part of the recorded `F2XM1`
figure is the oracle's error and not this file's. Above |x| ≈ 8 the two oracles
do not merely differ in tightness for `FSIN`/`FCOS`/`FPTAN` — they disagree
about which answer is correct. Any figure quoted from this harness has to say
which oracle it means, and any trig figure has to carry its argument range.

**What the oracle found**, none of which is in the manual in these words and
every one of which was a real defect when it was found:

- `#D` is raised for a denormal *operand* even when the other operand is an
  infinity and the denormal never reaches the arithmetic — but `#IA` and `#Z`
  **suppress** it, because they stop the operation first.
- The masked-overflow "largest finite value" is the largest finite value *at
  the current precision*, so under `PC=24` it is `0xFFFFFF0000000000`.
- Two NaNs with equal significands are separated by the smaller sign-exponent
  word, not by "the destination" as the manual says.
- Storing to a narrower format does **not** raise `#D` for a denormal source,
  because a store is not an arithmetic operation.

On anything that is not x86-64 with a 64-bit `long double` — which includes
every machine this emulator actually ships on — the oracle cannot run. The
harness still runs a table of golden vectors captured here, so it asserts
something real on ARM64 rather than silently passing, and says so on stdout.

### Shown to be able to fail

The earlier mutation record for this section — 56 mutations of `emu88_fpu.cc`,
45 dead, 11 survivors of which nine were real holes — was evidence about a file
that no longer exists, so it was re-earned rather than edited. On 2026-08-28,
**29 single-point mutations across both `emu88/emu88_f80.h` and
`emu88/emu88_fpu.cc`, rebuilt and run one at a time: 29 died, none survived.**

It did not start there. The first run of the three fault-atomicity mutations
left two alive, and both were real:

- the `FBSTP` write-suppression assertion was checking the wrong address. A
  real-mode effective address masks the offset to sixteen bits, so the writes
  after a fault do not run off the end of the segment — they **wrap to the
  start of it**, onto `DS:0001` and up. The test was looking past the end,
  where nothing ever lands.
- nothing asserted that a successful `FNSAVE` resets the instruction pointers
  as well as the registers, so a mutant that skipped that half survived.

The second, once `FNSAVE` was made to say `track = false` explicitly rather
than rely on an early return, became a provably equivalent mutant and was
replaced with one that is observable.

What is worth reading is not the score but which harness did the killing,
because it is the argument for why there are two:

| Killed by | Count | Examples |
|---|---|---|
| `f80_unit` only | 5 | subtraction dropping the sticky borrow; the alignment shift off by one past 128; the NaN tie-break picking the wrong operand; comparison forgetting that negatives order backwards; an exact cancellation always yielding `+0` |
| `fpu_test` only | 12 | `DC` `FSUB`/`FSUBR` swapped; stack overflow detected on the inverted condition; the saved tag word back in TOP-relative order; `FNSTENV` no longer masking; `C1` no longer telling overflow from underflow; `FFREEP` freeing without popping; the `FPREM` quotient bits in the wrong codes; `FXAM` losing the denormal class; a faulting memory operand no longer aborting the instruction; `FBSTP` writing on past the byte that faulted |
| both | 12 | round-to-nearest ignoring the tie rule; round-down and round-up swapped; `sqrt` computing one bit too few; the overflow and denormal exponent thresholds each off by one; precision control reading `PC=00` as 53 bits; multiplication dropping the normalising exponent bump |

Five mutations that no amount of opcode-level testing would have caught, and
twelve that no amount of arithmetic testing would have. Neither harness is
redundant, and neither would have been enough on its own.

**And one thing neither harness could have found.** Both were green, the oracle
had matched the host over millions of cases, and all 26 mutants were dead, when
a sanitized build

```sh
ASAN=1 bash tests/build.sh && tests/build-asan/fpu_test
```

reported `negation of -9223372036854775808 cannot be represented in type 'long
int'` out of `f80_to_int`. `FISTP m64int` of exactly -2^63 is an ordinary
in-range instruction; the answer it produced was correct on this compiler, and
the negation that produced it was undefined. That is the whole class of defect a
differential oracle cannot see — it compares answers, and the answer was right.
It is fixed (the negation is done in unsigned now), and section 8 asserts both
ends of the 64-bit range so the case has coverage as well as a fix. The lesson
is the one `build.sh` already records for the VESA pan clamp: run the sanitizer
by hand when you touch this, because CI does not.

An older note from the first pass is still worth keeping: one first-draft
assertion was thrown out because it expected a condition code of all-bits-clear,
which is exactly what a decode that ignored the opcode leaves behind — it passed
against a mutant. A comment in the file records why the fixture is built the
other way up.

### The defects this harness recorded, all fixed 2026-08-27

Each was found by this harness, each was recorded as a `bug()` first and fixed
second, and each assertion stayed exactly where it was and became a `check()`.
They are listed here because the harness's value is the record, not the count.

*(This heading said "the nine defects" and listed seven. The nine is the count
`CHANGELOG.md` and `todo.txt` use, and it counts two that were fixed in the
same pass without a separate bullet here. The heading is the thing that was
wrong, not the list, so the heading is what changed.)*

- **`fpu_write_m80real` mangled subnormals, and `fpu_read_m80real` mis-decoded
  them.** On the way out, a `dexp == 0` double was rebiased as if normalised and
  the explicit J bit was ORed in anyway, so `FSTP m80real` of 5e-324 wrote
  exponent `0x3C00` instead of `0x3BCD` and read back as ~1.1e-308 — off by
  2^51. A DJGPP long-double underflow that transited memory was silently
  corrupted. Fixed by normalising: find the highest set bit of the fraction,
  shift it up to the J bit, and bias the exponent from the true −1074. On the
  way in, an 80-bit denormal (`exp == 0`) has an effective exponent of 1, not 0,
  which `ldexp` was not being told.
- **`FIDIV`/`FIDIVR` by zero lost the sign**: a bare `INFINITY` instead of
  `copysign`, so `FIDIV` of −6 by 0 gave +∞. The real-operand paths twenty lines
  away got this right, so it was an inconsistency inside the file rather than a
  design choice. Two of the four integer branches were wrong.
- **0/0 was treated as a zero-divide, not an invalid operation**, on every
  divide path: `ZE` and +∞ where a 387 raises `IE` and returns the indefinite
  QNaN. All eight divide-by-zero sites now funnel through one helper, so they
  cannot disagree again: NaN numerator propagates with no flag, a zero numerator
  raises `IE` and returns a QNaN, and only a non-zero numerator raises `ZE` and
  returns an infinity signed by the XOR of the operands.
- **`fpu_compare` never cleared `C1`**, which Intel specifies for `FCOM`,
  `FCOMP`, `FCOMPP`, `FUCOM`, `FICOM` and `FTST`, so a `C1` left by `FXAM`
  survived into the next `FSTSW AX`.
- **`FCOMI` left `OF`, `SF` and `AF` untouched.** Guest code that did `FCOMI`
  and then branched on a signed condition — `JL`/`JLE`/`JG`/`JGE` read `SF` and
  `OF` — saw whatever the last integer instruction left behind. All four of
  `FCOMI`/`FUCOMI`/`FCOMIP`/`FUCOMIP` clear them now.
- **`FPREM1` rounded the quotient with `round()`** rather than ties-to-even, so
  10 rem 4 gave −2 where the answer is 2. It is `std::remainder` now, which is
  the IEEE remainder by definition; `rint`/`nearbyint` would have followed the
  host's rounding mode instead of the required round-to-nearest-even.
- **The 32-bit `FNSAVE` image was half-written.** The zero-fill loop read
  `for (int i = 3; i < (op_size_32 ? 7 : 7); i++)` — both arms of the ternary
  are 7, a dead copy-paste — and the body only ever stored 16-bit words, so the
  108-byte form zeroed only the low half of each 32-bit `FIP`/`FDP` field. The
  two environment layouts are now written separately: the 94-byte form clears
  `+6`..`+13`, the 108-byte form clears the reserved high halves of `CW`/`SW`/
  `TW` at `+2`/`+6`/`+10` and then `+12`..`+27`.

**The sentence that used to close this section is now false.** It read: *"None
of this makes the register stack 80-bit. The 31 `diverge()` sites are unchanged
and still name every place the `double` design shows through."* That was true
from the day the harness was written until 2026-08-28, when the register stack
became 80-bit and the 31 sites went to zero. It is quoted here rather than
deleted, because a reader who remembers it should be able to find out when it
stopped being true — and because everything above it, the seven defects
included, is still an accurate record of a file that has since been rewritten
around them.

## 5. DPMI host — end-to-end

`dos_dpmi.cc` was 1710 lines when this was written (1743 today), is linked into
every harness that pulls in the DOS layer, and until 2026-08-27 was executed by
none of them. It is the DPMI server every
DJGPP and DOS4GW client runs on top of.

```sh
bash tests/build.sh          # builds tests/build/dpmi_test
tests/build/dpmi_test
```

`tests/dpmi_test.cc` drives a client the way a real one arrives: `INT 2Fh`
`AX=1687h` in real mode, a `FAR CALL` to the returned entry point, then `INT
31h` issued from a `CD 31 / F4` stub inside the client's own protected-mode code
segment. Nothing calls into `dos_dpmi.cc` directly — `dpmi_int31h` is private.
The reflection tests (`0100h`/`0101h`/`0300h`–`0302h`) run against a
hand-assembled 8086 `INT 21h` handler installed in the IVT before the switch,
with swappable bodies that edit the pushed `FLAGS` image so `CF` propagates back
into protected mode the way a real DOS returns it.

**Result: PASS** — 429 assertions, 0 known bugs held at baseline. Covered: the
mode switch itself (`CR0.PE`, A20 forced on, the 256-vector snapshot, the client
PSP, and all 8 raw bytes of GDT entries 0–6, the TSS and two IDT gates); LDT
allocation, freeing and exhaustion; every descriptor service asserted as the
actual bytes that land in the LDT rather than merely "`CF` clear"; DOS memory
blocks against the real `INT 21h` reflection; real-mode and protected-mode
vector installation; `0400h`; the `0500h` 48-byte block dword by dword;
`0501h`–`0503h` including page alignment, non-overlap and a growing realloc that
relocates and copies; the `0900h`–`0902h` trio each asserting the *previous*
state; `0E00h`/`0E01h` coprocessor status, including that presence is `MPr`
(bit 2) rather than `MPv` (bit 0) and that a client-emulation request is
refused rather than silently accepted; real-mode callbacks; and
`INT 21h AH=4Ch` tearing the session down and restoring the IVT.

Error paths are asserted as hard as the happy paths — `CF` set *and* the
documented `AX` error code — because a harness that only walks the success path
is the kind that cannot fail. Twenty single-line mutations of `dos_dpmi.cc` each
turned at least one check red before the harness was trusted; two blind spots
found that way were closed.

Seven further divergences are pinned exactly rather than left implicit, and
`grep 'divergence:' tests/dpmi_test.cc` is the list: `0001h` accepts a double
free and `0001h` accepts a never-allocated selector, `000Ah` hard-codes DPL=3
on the alias, `0203h` ignores an out-of-range exception number instead of
returning `8021h`, `0305h` returns a null real-mode save address, and `0100h`
reports success *and* returns selector `0000h` when the LDT is full.

### The four defects, all fixed 2026-08-27

Each was found by this harness, recorded as a `bug()` first and fixed second,
with the assertion staying put and becoming a `check()`. `KNOWN_BUGS_EXPECTED`
is 0; the machinery stays for the next defect.

- **`0002h`'s descriptor cache was dead code.** On a cache hit the handler ran
  `break`, which leaves the enclosing `for` loop rather than the `switch` case,
  so control fell straight into the allocate-a-new-descriptor path below. Every
  repeat lookup of the same real-mode segment burned another LDT entry, so a
  client that maps one segment in a loop — DJGPP's
  `__dpmi_segment_to_descriptor`, DOS4GW mapping the PSP — exhausted the 2047
  entries. DPMI 0.9 requires the same descriptor back. The fix is `return`:
  nothing follows the `switch` in that function.
- **Reflected interrupts pushed their frame 64 KB outside the reserved window.**
  `dpmi_reflect_to_rm` computed `rm_sp = stack_top & 0x0F`; every level's
  `stack_top` is 512-byte aligned, so `rm_sp` was always 0 and the first push
  wrapped `SP` to `FFFEh`. The frame landed at physical `171FAh` instead of
  inside the `7000h`–`8000h` locked stack the comment reserves for it — in a
  real session, the DOS kernel, a driver, or the client's own image. `SS` now
  addresses the base of the window and `SP` is the offset within it. The harness
  asserts this by scanning both address ranges for a written byte, so it fails
  on a stray write anywhere in the 64 KB above the window.
- **Callback slots were never reclaimed and the counter was process-global.**
  `next_callback` was a function-local `static int`, so it survived
  `dpmi_terminate`, a fresh mode switch, and destruction of the machine object;
  `0304h` only cleared `CF`. Hook and unhook in a loop and the 17th allocation
  failed forever, and a second DPMI program in the same process started with the
  first one's count. The allocation record is a `bool[16]` in `DpmiState` now,
  cleared by `dpmi_mode_switch`, and `0304h` validates the address it is handed
  and gives the slot back — returning `CF` set and `AX=8024h`, the DPMI 0.9
  "invalid callback address", for a segment, an alignment or a slot it never
  handed out, and for a double free.
- **`0400h` advertised virtual memory it did not have.** `BX` was `0005h`, and
  bit 2 means "virtual memory supported". There is no paging here at all —
  `CR0.PG` is never set, `0600h`–`0603h` are no-ops, `0500h` reports no swap
  file — and the comment on the line said "no virtual memory", so the value
  contradicted its own documentation. `BX` is `0003h`: bit 0 for 32-bit clients,
  and bit 1 because this host really does return to **real** mode rather than
  V86 for a reflected interrupt (`dpmi_reflect_to_rm` and `dpmi_exec_rm` both
  clear `CR0.PE`). That bit was clear before and was also wrong; the harness
  now pins the whole word.

The seven divergences listed above are unchanged - they are design decisions in
this host, not defects, and each is still pinned exactly.

## 6. NE2000 / DP8390 network card — module-level

`emu88/ne2000.cc` was 425 lines before this harness existed, 438 after the two
fixes below. Until 2026-08-27 it was compiled by every
harness that links the DOS layer and executed by none of them - `grep -lni
ne2000 tests/*.cc` returned nothing at all. `tests/ne2000_test.cc` drives the
class through its public API the way `opl_unit`, `sb_unit` and `uart_unit`
drive theirs: `iowrite`/`ioread`/`iowrite16`/`ioread16` on the 32 I/O offsets,
`receive()`, and the `on_transmit` callback. Nothing reaches past the public
interface - the register file, the card RAM and the ring pointers are private
and are asserted only through the ports, which is how a driver sees them.

```sh
bash tests/build.sh          # builds tests/build/ne2000_test
tests/build/ne2000_test
```

**Result: PASS** - 220 assertions, 0 known bugs held at baseline. Covered:
reset state and register banking across all four pages through the command
register; `set_mac` read back both through `PAR0`-`PAR5` on page 1 and through
the doubled on-card PROM the way `NE2000.COM` reads it; remote DMA in both
directions with `RSAR`/`RBCR` setup, address auto-increment seen through
`CRDA0/1`, the `RDC` bit, count exhaustion, PROM write protection and the
`PSTOP`→`PSTART` wrap; transmit, asserting the exact bytes and length delivered
to `on_transmit` plus `TSR` and `ISR.PTX`; receive, asserting the 4-byte DP8390
header, the payload, `CURR` advancing and `BNRY`, read back through remote DMA
exactly as a driver does it; ring wrap; ring overflow with `BNRY` parked;
`IMR` masking, `irq_active()` and write-1-to-clear on the `ISR`; and receive
filtering across unicast, broadcast, multicast, promiscuous, monitor, stopped
and runt frames.

The same three-kind scheme as sections 4 and 5, for the same reason - this is a
functional model of a DP8390, not a gate-level one. **28 `diverge()` sites** pin
what this implementation does where a real card differs, each with a comment
naming the gap: no FCS is stored after the payload, transmission is
instantaneous so `CR.TXP` is never observably set, `TSR` has no `ABT`/`FU`/`CRS`
bits, the `CNTR0`-`CNTR2` error counters are hardwired to zero, `DCR.WTS` is
stored and never consulted so byte mode still moves a word, `receive()` tests
`CR.STP` and never `CR.STA`, and there is no maximum-frame-length check.

**Two defects came out of it, and both are fixed** - see the changelog.
`receive()` guarded its ring writes with `a < MEM_TOTAL` where `dma_write_byte`
guards with `a >= BUF_START && a < MEM_TOTAL`, so a card started before `CURR`
had been programmed wrote the incoming frame over its own read-only MAC PROM.
And `ioread16()` on a register port returned one 8-bit register zero-extended
while `iowrite16()` correctly split a word write into two byte writes, so an
`INW` of a register pair was half a read - the ISA bus decomposes a 16-bit
access to an 8-bit-decoded register file in both directions.

**Shown to be able to fail.** 22 single-point mutations of a scratch copy of
`emu88/ne2000.cc`. The first pass killed 18 of 20; both survivors were real
coverage holes - nothing read `CLDA0`/`CLDA1`, and nothing asserted that a
page-2 write is ignored - and both are closed, after which the re-applied
mutations died. Two further mutations added afterwards died as well.

Six more were applied against the two fixes above, and **one survived**:
changing the receive-path guard from `a >= BUF_START` to `a > BUF_START` changed
nothing, because every section used the conventional `PSTART = 0x46` and so
nothing ever wrote card address `4000h`. Section 6b of the harness exists to
close that - a three-page ring based at `PSTART = 0x40`, driven round until
`CURR` reaches the first page. With it, all six die. **Total: 28 applied, 28
killed.**

One more thing came out of that run and is worth recording, because it is about
the harness rather than the card. A mutant's corrupted ring drove a card-derived
length into the frame builder, which indexed a `std::vector` out of range and
aborted - and because `stdout` was fully buffered, the `FAIL` line that had
already diagnosed the mutant went with the buffer. The harness line-buffers now
and the frame builder rejects an impossible length with a message. Chasing that
also found a real out-of-bounds write in the harness itself: the frame builder
wrote the Ethernet type field at offsets 12 and 13 unconditionally, including
for the deliberate 13-byte runt in the receive-filter section.

## 7. PC BIOS services — end-to-end

`emu88/dos_bios.cc` was 2,178 lines before this harness existed, 2,254 after the
six fixes below. Every harness that links the DOS layer
compiled it and almost none of it ran: only whatever the INT 10h VESA path in
`vesa_test.cc` and the hand-assembled INT 21h handler in `dpmi_test.cc` happened
to touch. `tests/bios_test.cc` drives it the way sections 3 and 5 drive theirs -
it assembles `CD <vec> / F4` into guest memory, points `CS:IP` at it, and lets
`emu88::execute()` decode and dispatch. Every BIOS entry point in
`dos_machine.h` is private, so there is no short cut available even in
principle. The XMS driver is reached the way a real client reaches it, a
`CALL FAR F000:EFD8` into the ROM trap stub; INT 08h's chain to INT 1Ch is
proved by installing a hand-assembled real-mode `1Ch` handler in the IVT and
watching it run.

The disks are RAM images owned by the harness's `dos_io`. `dos_machine` reaches
every medium through `disk_read`/`disk_write`/`disk_size`/`disk_present` and has
no other notion of one, so a `std::vector<uint8_t>` per drive is a complete
drive as far as INT 13h is concerned: a 360 KB floppy at `00h`, a 1.44 MB floppy
at `01h`, and a 2 MB hard disk at `80h`. That `dos_io` also counts its calls,
which turns out to matter - see the CHS defect below.

```sh
bash tests/build.sh          # builds tests/build/bios_test
tests/build/bios_test
```

**Result: PASS** - 512 assertions, 0 known bugs held at baseline. Covered: INT
10h text services (`00` mode set and the seven BDA fields it writes, `01`/`02`/
`03` cursor, `05` page, `06`/`07` scroll with all four window edges asserted,
`08` read-back, `09`/`0A` write with and without an attribute and with a repeat
count, `0C`/`0D` mode-13h pixels, `0E` TTY including `CR`/`LF`/`BS`/`BEL` and
the scroll at the bottom line, `0F`, `10h` DAC single and block, `11h`/`30`,
`12h`/`BL=10`, `13h` write string, `1Ah` for all five display configurations);
INT 11h; INT 12h; INT 13h `00`/`02`/`03`/`04`/`08`/`15`/`41`/`42`/`43`/`48`
against those disks with a verified read/write round trip and the error paths;
INT 14h; INT 15h `24`/`41`/`4F`/`86`/`87`/`88`/`91`/`C0`; INT 16h
`00`/`01`/`02`/`03`/`05`/`09`/`10`/`11`/`12` including the ring wrap, the peek
that must not consume, and the blocking read's `IP` rewind; INT 17h; INT 19h
bootstrap from both drives and the no-bootable-medium path; INT 1Ah `00`-`05`;
INT 2Fh `1680`/`4300`/`4310`; and the XMS driver behind `4310`.

Since 2026-08-29 it also owns the two coprocessor paths, because both need a
whole machine rather than a bare core: **IRQ13 / INT 75h**, driven end to end -
unmask, divide by zero, `FWAIT`, the latch, the BIOS handler, the guest's
`INT 02h` handler - and asserting both that the video BIOS was *not* entered
(vector 16 is `INT 10h`) and that the machine does not wedge when nothing
retires the error; and **coprocessor presence**, that the BDA equipment word,
CMOS `0x14` and CPUID leaf 1 agree there is an x87, which they did not until
that date.

Deliberately not reached, with the reason in each case: the VESA `4Fxx` services
and INT 33h mouse, which section 3 already owns end to end; INT E0h host file
services, which need a real host filesystem behind `dos_io::host_file_*` (faking
it would test the fake); CD-ROM drives at `>= 0xE0`, which are attachable the
same way and were left out for size; `bios_int08h`'s protected-mode branch,
which needs a live DPMI session that section 5 owns; the keyboard idle heuristic,
which needs `run_batch`'s cycle counter that this harness deliberately does not
use; and the mode-13h sequencer/CRTC side effects, which live in `emu88_mem`.

**Six defects came out of it, and all six are fixed** - see the changelog. Two
are worth restating here because of what they took to catch:

- **INT 13h `AH=02`/`03` never checked that the CHS address exists.** `sector -
  1` with sector 0 made the unsigned LBA `2^64-1` and handed `dos_io` a byte
  offset of `2^64-512`; a host backend seeking with a signed `off_t` seeks
  backwards. Sector, head and cylinder are all bounded now. The assertion that
  matters is **not** the status code - a short read from `dos_io` produces the
  same `CF` and `AH=04h` as a rejection, so registers alone cannot tell them
  apart. It is `io.reads` staying at 0: the host is never asked.
- **XMS `AH=0Bh` validated neither offset nor length against the block**, so a
  move longer than the destination returned SUCCESS and wrote past the end of
  another allocation. It returns `A7h`/`A8h`/`A9h` now, bounded exactly - and
  the boundary assertions use an **odd** destination offset with an even length,
  because with the length required to be even that is the only shape that puts
  the end of a move exactly one byte past the block, and so the only shape that
  separates `> block` from `> block + 1`.

**Shown to be able to fail.** 18 single-point mutations of a scratch copy of
`dos_bios.cc` in the first pass, all 18 killed - after three earlier drafts had
survived, which is why the right-edge and top-edge scroll assertions, the
8-page cursor-reset assertion and the column-wrap TTY assertion exist. Eleven
more were applied against the six fixes and **four survived**: reverting only
the mode-13h half of the AL-bit-7 fix (the flag was tested in text mode only),
the `> block` / `> block + 1` off-by-one (every move tested asked for twice the
block), dropping the source offset from the source bound (every source move
started at 0), and re-allowing CHS sector 0 (the short read looked identical).
Each survivor was a real hole and each is the reason an assertion above exists.
With them added, 11 of 11 die: **29 applied, 29 killed**.

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
- **The FPU's exception DELIVERY is covered, but not by this corpus.** SST has
  no `D8`-`DF` file at all — 0 of 941 — and its only x87-adjacent opcode is
  `9B`. Delivery is graded instead by `fpu_test` section 19b (the deferral to
  the next waiting instruction, the ten no-wait encodings, `FIP` still naming
  the raising instruction, and a pending exception outranking an operand fault)
  and by `bios_test`'s "IRQ13 / INT 75h" section, which drives the whole PC
  path — unmask, divide by zero, `FWAIT`, IRQ13, `INT 75h`, the guest's
  `INT 02h` handler — and asserts the video BIOS was not entered.
  *(This bullet said there was no `#MF` dispatch and that "DOS software masks
  in practice" until 2026-08-28. Both were wrong: delivery exists now, and
  Borland's DOS runtimes leave invalid-operation, divide-by-zero and overflow
  unmasked. It is rewritten rather than deleted because the second half was a
  confident claim about the world that turned out to be false.)*

Three of the five gaps this section opened with were closed on 2026-08-27.  A
sixth, added that same day and closed on 2026-08-28 - the FPU's exception
delivery, rewritten above rather than struck because its second sentence was
wrong as well as stale - makes four closed.  They are kept here because what
they were is the argument for the two that remain.

- ~~**`build.sh` does not build `test386`.**~~ It does now, alongside the other
  ten harnesses.
- ~~**Nothing compares test386's output to the reference.**~~
  `tests/run_suites.sh` does, and that check is the reason it exists: the
  harness reports POST `0xFF` and stops, which is how four wrong `IDIV` lines
  read as a pass. The runner was shown to fail on exactly that regression before
  it was trusted — against a core built from before `7352fc5` it still says
  "ok reached POST 0xFF" and then fails the diff.
- ~~**There is no CI for any of this.**~~ `.github/workflows/tests.yml` builds
  the harnesses and runs `run_suites.sh` on every push and pull request, with
  the corpora cached.

What has NOT changed: the two gaps above these. Automating the suites does not
widen them — CI runs exactly what a person ran by hand, so 32-bit
protected-mode instruction execution is still covered only by test386's
full-system pass, which checks the machinery rather than every instruction.
*(This sentence also named x87 exception delivery until 2026-08-28. That half
is now covered — `fpu_test` section 19b and `bios_test` section 15 — and the
protected-mode half is not, so the count of two is unchanged but its second
member is different: see the DPMI note in section 3.)*

Sections 4 to 7 close four gaps this list never named, because until 2026-08-27
none of the x87 FPU, the DPMI host, the NE2000 or the PC BIOS had any coverage
at all - every one of them was compiled into harnesses and executed by none.
What those four still do not reach, stated so it is not mistaken for coverage:

- ~~**`FIST`/`FISTP` of an out-of-range value is deliberately untested.**~~
  Closed 2026-08-27. It was untestable for a real reason - the code cast a
  `double` straight to `int16_t`/`int32_t`/`int64_t`, which is *undefined
  behaviour* rather than the 387's `#IA`-and-integer-indefinite, so a test there
  would have been testing the compiler. All eight store paths - `FIST`
  m16/m32, `FISTP` m16/m32/m64, `FISTTP` m16/m32/m64 - now go through one range
  check that raises `IE` and stores the integer indefinite value, and 27
  assertions cover it: both boundaries at all three widths (including the
  64-bit pair, where -2^63 is exactly representable and in range while +2^63 is
  exactly representable and is not), a value that fits before rounding and not
  after, infinities and a NaN. Six single-point mutations of the range check
  each turn at least two of them red.
- **A few undefined or unreachable x87 encodings**: `DD C8`-`CF`, and `DE D8+i`
  for `i != 1`, where the handler does nothing at all - defensible, since only
  `DE D9` is architecturally defined, but it means neither a compare nor a pop
  happens. `FWAIT` (`0x9B`) lives in `emu88.cc`, not `emu88_fpu.cc`.
- **The NE2000 harness is the module in isolation.** It never runs guest code:
  the port decode in `dos_machine.cc` that routes `ne2000_base`..`+0x1F` to the
  card, the 16-bit port paths, and the IRQ delivery are all outside it. A
  guest-level test would need a packet driver and a host network back end, and
  mTCP and FDNET have never been run on this machine at all - the claim that
  networking works is inherited from the commit that added it.
- **The BIOS harness reaches maybe two thirds of `dos_bios.cc`, and section 7
  lists what it misses**: the VESA `4Fxx` block and INT 33h (section 3 owns
  them), INT E0h host file services, CD-ROM drives at `>= 0xE0`,
  `bios_int08h`'s protected-mode branch, and the keyboard idle heuristic. Two
  of those are size decisions rather than impossibilities and are the obvious
  next thing to add.
- ~~**DPMI descriptor services do not validate the selector they are given.**~~
  Closed 2026-08-27, and for the same reason as the entry above: it was
  unassertable while the behaviour was an out-of-bounds access rather than an
  answer. `0006h`-`000Ch` took a selector in `BX` and indexed the LDT with it
  unchecked, so a GDT selector (`TI` clear) indexed the LDT anyway and an index
  past the end read or wrote as much as 48 KB beyond the table - `sel >> 3` runs
  to 8191 where the table holds 2048 entries, the address is guest-controlled,
  and what sits past the LDT in this layout is the GDT, the IDT and the TSS.
  All seven return `CF` set and `AX=8022h` now, which is what DPMI 0.9
  specifies and what `0001h` had always done; `000Ah` no longer allocates an
  alias before deciding to reject. 32 assertions cover it, and five single-point
  mutations of the guard each turn at least one red.
