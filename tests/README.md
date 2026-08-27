# emu88 CPU test harnesses

Standalone validation of the **emu88** 386 CPU core (built without the iOS/DOSBox
app) against two industry-standard suites, plus hand-written harnesses for the
parts those suites do not reach: the VESA BIOS, the x87 FPU and the DPMI host.

## Setup

```sh
bash tests/fetch_tests.sh   # downloads test data into tests/data/ (gitignored, ~600MB)
bash tests/build.sh         # builds all nine harnesses into tests/build/
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

## 4. x87 FPU — instruction-level

`emu88_fpu.cc` is 870 lines of CPU core, so every harness above already compiled
it and not one of them ever executed an x87 opcode. `tests/fpu_test.cc` does,
by writing real encodings to `CS:0000` and running them through
`emu88::execute()` — the `D8`–`DF` escape dispatch, the modrm decoder and the
FPU handler are therefore exercised together, rather than by calling
`execute_fpu()` behind the decoder's back.

```sh
bash tests/build.sh          # builds tests/build/fpu_test
tests/build/fpu_test
```

**Result: PASS** — 437 assertions over ~74 mnemonics: stack discipline and
`TOP` wraparound, the tag word, all three memory real formats, the seven `FLD`
constants as exact bit patterns, every arithmetic form with the non-commutative
ones asserted in both directions, integer load/store rounding versus `FISTTP`'s
truncation, the `C0`/`C2`/`C3` codes for less-than, equal, greater-than and
NaN-unordered, `FXAM`'s full classification, the transcendentals, all four
rounding modes driving `fpu_round`, `FCMOVcc`, `FCOMI`, `CR0.EM`/`CR0.TS`
gating, and `FNSAVE`/`FRSTOR` in both the 94-byte and 108-byte forms.

**Three kinds of assertion, because the register stack is not a 387's.**
`emu88.h` declares `double regs[8]` — 53 mantissa bits, not 80-bit extended —
so a whole class of real-hardware results is not reproducible here and will not
be without a rewrite. Rather than quietly asserting whatever the code does:

- `check()` asserts behaviour that is correct.
- `diverge()` (31 sites) pins a value that provably differs from a real 387,
  each with a comment naming the gap: `FLD m80real` of 1+2^-53 collapsing to
  exactly 1.0, no denormal class, no stack-overflow detection, precision
  control ignored entirely, `F2XM1` computed as `pow(2,x)-1` and `FYL2XP1` as
  `log2(x+1)` so both lose the precision those instructions exist to preserve.
  The gap is documented and the test fails if it ever moves.
- `bug()` (9 sites) asserts the *correct* 387 behaviour for a defect that is
  **not** explained by the double design. These are red on purpose and held to
  `KNOWN_BUGS_EXPECTED` the way SingleStepTests is held to `SST_BASELINE`:
  fixing one **fails** the harness, which prints `FIXED (lower
  KNOWN_BUGS_EXPECTED)`, because a silent improvement means the number is
  stale.

**Shown to be able to fail.** 56 single-point mutations of a scratch copy of
`emu88_fpu.cc` — swapped `FSUB`/`FSUBR`, reversed `FPATAN` operands, `fpu_push`
incrementing `TOP`, `FISTTP` rounding instead of truncating, `FCOMIP` not
popping, and 51 more. 45 died. Of the 11 survivors two were provably equivalent
mutants in unreachable clamps; the other nine were real coverage holes and are
now closed (+48 assertions), after which all 21 re-applied mutations died and
none escaped. One first-draft assertion was thrown out during that work because
it expected a condition code of all-bits-clear, which is exactly what a decode
that ignored the opcode leaves behind — it passed against a mutant. A comment
in the file records why the fixture is built the other way up.

### Known defects (`bug()`, 9)

- **`fpu_write_m80real` mangles subnormals.** For `dexp == 0` it still rebiases
  as if normalised and still ORs in the explicit J bit, so `FSTP m80real` of
  5e-324 writes exponent `0x3C00` instead of `0x3BCD` and the value reads back
  as ~1.1e-308 — off by 2^51. A DJGPP long-double underflow that transits memory
  is silently corrupted.
- **`FIDIV`/`FIDIVR` by zero lose the sign** (`emu88_fpu.cc:438-439` and
  `:716-717`): a bare `INFINITY` instead of `copysign`, so `FIDIV` of −6 by 0
  gives +∞. The real-operand paths twenty lines away get this right, so it is an
  inconsistency inside the file rather than a design choice.
- **0/0 is treated as a zero-divide, not an invalid operation**, on every divide
  path: `ZE` and +∞ where a 387 raises `IE` and returns the indefinite QNaN.
- **`fpu_compare` never clears `C1`**, which Intel specifies for `FCOM`,
  `FCOMP`, `FCOMPP`, `FUCOM`, `FICOM` and `FTST`.
- **`FCOMI` leaves `OF`, `SF` and `AF` untouched.** Guest code that does `FCOMI`
  and then branches on a signed condition — `JL`/`JLE`/`JG`/`JGE` read `SF` and
  `OF` — sees whatever the last integer instruction left behind.
- **`FPREM1` rounds the quotient with `round()`** rather than ties-to-even, so
  10 rem 4 gives −2 where the answer is 2.
- **The 32-bit `FNSAVE` image is half-written.** The zero-fill loop reads
  `for (int i = 3; i < (op_size_32 ? 7 : 7); i++)` — both arms of the ternary
  are 7, a dead copy-paste — and the body only ever stores 16-bit words, so the
  108-byte form zeroes only the low half of each 32-bit `FIP`/`FDP` field.

## 5. DPMI host — end-to-end

`dos_dpmi.cc` is 1710 lines, is linked into every harness that pulls in the DOS
layer, and until now was executed by none of them. It is the DPMI server every
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

**Result: PASS** — 378 assertions, 4 known bugs held at baseline. Covered: the
mode switch itself (`CR0.PE`, A20 forced on, the 256-vector snapshot, the client
PSP, and all 8 raw bytes of GDT entries 0–6, the TSS and two IDT gates); LDT
allocation, freeing and exhaustion; every descriptor service asserted as the
actual bytes that land in the LDT rather than merely "`CF` clear"; DOS memory
blocks against the real `INT 21h` reflection; real-mode and protected-mode
vector installation; `0400h`; the `0500h` 48-byte block dword by dword;
`0501h`–`0503h` including page alignment, non-overlap and a growing realloc that
relocates and copies; the `0900h`–`0902h` trio each asserting the *previous*
state; real-mode callbacks; and `INT 21h AH=4Ch` tearing the session down and
restoring the IVT.

Error paths are asserted as hard as the happy paths — `CF` set *and* the
documented `AX` error code — because a harness that only walks the success path
is the kind that cannot fail. Twenty single-line mutations of `dos_dpmi.cc` each
turned at least one check red before the harness was trusted; two blind spots
found that way were closed.

Seven further divergences are pinned exactly rather than left implicit: `0001h`
accepts a double free, `000Ah` hard-codes DPL=3 on the alias, `0203h` ignores an
out-of-range exception number instead of returning `8021h`, `0305h` returns a
null real-mode save address, `0100h` reports success with selector `0000h` when
the LDT is full, and freed `0501h` blocks never reclaim linear address space.

### Known defects (`bug()`, 4)

- **`0002h`'s descriptor cache is dead code.** On a cache hit the handler runs
  `break`, which leaves the enclosing `for` loop rather than the `switch` case,
  so control falls straight into the allocate-a-new-descriptor path below. Every
  repeat lookup of the same real-mode segment burns another LDT entry, so a
  client that maps one segment in a loop — DJGPP's
  `__dpmi_segment_to_descriptor`, DOS4GW mapping the PSP — exhausts the 2047
  entries. DPMI 0.9 requires the same descriptor back.
- **Reflected interrupts push their frame 64 KB outside the reserved window.**
  `dpmi_reflect_to_rm` computes `rm_sp = stack_top & 0x0F`; every level's
  `stack_top` is 512-byte aligned, so `rm_sp` is always 0 and the first push
  wraps `SP` to `FFFEh`. The frame lands at physical `171FAh` instead of inside
  the `7000h`–`8000h` locked stack the comment reserves for it. In a real
  session that address is the DOS kernel, a driver, or the client's own image.
- **Callback slots are never reclaimed and the counter is process-global.**
  `next_callback` is a function-local `static int`, so it survives
  `dpmi_terminate`, a fresh mode switch, and destruction of the machine object;
  `0304h` only clears `CF`. Hook and unhook in a loop and the 17th allocation
  fails forever, and a second DPMI program starts with the first one's count.
- **`0400h` advertises virtual memory it does not have.** `BX` is `0005h`, and
  bit 2 means "virtual memory supported". There is no paging here at all —
  `CR0.PG` is never set, `0600h`–`0603h` are no-ops, `0500h` reports no swap
  file — and the comment on the line says "no virtual memory", so the value
  contradicts its own documentation.

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
  eight harnesses.
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

Sections 4 and 5 close two gaps this list never named, because until 2026-08-27
neither the x87 FPU nor the DPMI host had a single line of coverage - both were
compiled into every harness and executed by none. What those two still do not
reach, stated so it is not mistaken for coverage:

- **`FIST`/`FISTP` of an out-of-range value is deliberately untested.** The code
  casts a `double` straight to `int32_t`/`int16_t`, which is undefined behaviour
  rather than the 387's `#IA`-and-integer-indefinite, so a test there would be
  testing the compiler, not the emulator.
- **A few undefined or unreachable x87 encodings**: `DD C8`-`CF`, and `DE D8+i`
  for `i != 1`, where the handler does nothing at all - defensible, since only
  `DE D9` is architecturally defined, but it means neither a compare nor a pop
  happens. `FWAIT` (`0x9B`) lives in `emu88.cc`, not `emu88_fpu.cc`.
- **DPMI descriptor services do not validate the selector they are given.**
  `0006h`-`000Ch` accept a GDT selector or an index past the end of the LDT and
  read or write memory beyond it rather than returning `8022h`. This is recorded
  rather than asserted: a test could only pin the out-of-bounds access, not a
  behaviour worth keeping.
