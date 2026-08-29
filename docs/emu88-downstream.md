# What downstream needs to know about `emu88/`

A second product compiles these files out of this working tree. `dosiz`
(sibling checkout) does not vendor `emu88`; `dosiz/src/CMakeLists.txt` points
`EMU88_DIR` at `../../qxDOS/emu88` and compiles six `.cc` files from it, plus
every header those pull in. There is no submodule, no copy, no version
constant, no checksum and no build stamp between the two products.

That has two consequences, and they pull in opposite directions:

- **A local `make` in dosiz reads whatever is in this checkout at that
  moment.** Save a file here and their next build moves.
- **Their CI does not.** `QXDOS_REF` in `dosiz/.github/workflows/ci.yml` is a
  full 40-character qxDOS SHA, bumped by hand on their side.

So the further their pin drifts, the wider the gap between what their CI
validates and what a developer sitting in front of dosiz actually builds. This
file exists to make that gap legible, because `CHANGELOG.md` is otherwise the
only signal they get and it is written per-commit rather than per-pin.

**This is not a request to bump the pin.** When that happens is dosiz's call,
and `CLAUDE.md` says so deliberately. This is the note that says what bumping
it would bring.

## State as of 2026-08-29

| | |
|---|---|
| dosiz's pin (`QXDOS_REF`) | `64d8e7d27633c6a3bdeb533323dd2e0a28a6ee63` |
| qxDOS `HEAD` | `124445a` |
| commits between them | 29, of which **14 touch files dosiz compiles** |

Everything below was measured here, on this tree, rather than inferred.

### The headline: their CI is validating a different FPU

`emu88/emu88_f80.h` **did not exist at their pin.** At `64d8e7d` the x87
register file was

```c
double regs[8];       // ST(0)-ST(7) as host doubles
```

— 53 mantissa bits. It is now

```c
f80 regs[8];          // ST(0)-ST(7), 80-bit double extended precision
```

backed by a 2,343-line header-only soft float that dosiz now compiles. It is
header-only *on purpose*: their six-file list is fixed, not a glob, so a seventh
`.cc` under `emu88/` would build here, pass every suite here, and fail to
**link** there, with nothing in between to notice.

A dosiz developer building locally today gets the 80-bit FPU. Their CI gets the
`double` one. Any x87 result needing more than 53 mantissa bits *can* differ
between the two — many are exactly representable either way, so this is a
"can", not a "does" — and no test on either side compares one against the
other.

### Diffstat, pin → `HEAD`, restricted to what dosiz sees

```
 emu88/emu88.cc       |   75 +-
 emu88/emu88.h        |   78 +-
 emu88/emu88_f80.h    | 2343 ++++++++++++++++++++++++  (new file)
 emu88/emu88_fpu.cc   | 1462 ++++++++++++-------------
 emu88/emu88_mem.cc   |   36 -
 emu88/emu88_mem.h    |    2 -
 emu88/emu88_pmode.cc |   59 -
```

`opl.cc` and `sound_blaster.cc` — the other two of the six — are unchanged.

### Two new virtuals on `emu88`

```c
virtual bool fpu_signal_error();
virtual bool check_interrupts(void);
```

Both change the vtable. That is source-compatible and dosiz rebuilds from
source, so nothing breaks; it is listed because `dosiz/src/compat/dosbox_compat.cc`
subclasses `emu88` directly and someone reading that class should know the base
grew two overridable points. `EmuCpu` overrides neither, so both take their
default behaviour.

### What is inert for dosiz, and why

Two of the recent changes look alarming in the changelog and do nothing here:

- **x87 exception delivery (`#MF`).** The bare core honours `CR0.NE` and
  otherwise does not deliver. `grep -rn 'CR0_NE' ../dosiz/src/` finds nothing —
  dosiz never sets it — so `fpu_signal_error()` returns false and no exception
  is delivered that was not delivered before. This matters more than it looks:
  dosiz's default IDT gives vector 16 a *present* null-selector gate, so an
  unconditional `#MF` would have become `#GP(0)` and terminated the client. The
  `CR0.NE` gate is what keeps that from happening, and it was chosen for that
  reason.
- **The CPUID FPU bit** (`EDX` bit 0, leaf 1, `0x10` → `0x11`). This *is* in
  `emu88.cc`, which dosiz compiles. `grep -rni cpuid ../dosiz/src/` finds
  nothing, so dosiz's own code never reads it. Four **guest** binaries under
  `dosiz/tests/` contain the `0F A2` byte pair — `BC.EXE`, `BIGTEST.exe`,
  `COMMAND.COM`, `FIND.EXE` — and none of the four is among the fixtures
  `ci.yml` gates on (37, by `check_dosiz.sh`'s reading of it; `ci.yml` has 38
  `build/dosiz tests/…` lines, one of which carries no assertion to pair with),
  so the change is untested in either direction downstream.

### What did move values

`emu88_f80.h` changed x87 **results**, not just flags, twice in one day:

- `e831315` — `FYL2X`, `FYL2XP1` and `FPATAN` over much of their domain.
- `124445a` — `FSIN`, `FCOS` and `FPTAN` over most of theirs.

Both were measured as improvements against the host x87 and against a
higher-precision reference, and both are described with their numbers in
`CHANGELOG.md`. But "improvement" is not "no change": a dosiz test that pins an
x87 result bit-for-bit will move.

`F2XM1`, `FYL2X`, `FYL2XP1` and `FPATAN` were verified bit-identical across
`124445a` specifically — 960,000 results over twelve `RC`/`PC` control-word
settings, values, `C1` and flags — so that commit's value movement is confined
to the three trig forms.

### What this side verified

`tests/check_dosiz.sh` builds dosiz from a sibling checkout against **this
working tree**, fails on any warning out of `emu88/`, reads the fixture list out
of dosiz's own `ci.yml` rather than duplicating it, and runs every one. Against
`124445a`:

```
   ok  configured, built, 0 warnings from emu88/
   ok  37/37 dosiz fixtures passed
```

It exits **2**, not 1, when there is no dosiz checkout — "not checked" is a
different answer from a failure — so it is safe to run unconditionally. It is
deliberately not in CI here: it needs a checkout this repository does not carry
and must not depend on.

That 37/37 is the strongest statement available from this side, and it is worth
being clear about its limits. It says the *current* dosiz fixtures pass against
the *current* emu88. It does not say the two products agree on anything the
fixtures do not exercise — and the fixtures exercise no CPUID, no `CR0.NE`, and
no x87 result that a 53-bit mantissa would have got wrong.

## If the pin is bumped

The gate to run on their side is their own `ci.yml`. From this side, the check
is `bash tests/check_dosiz.sh` (or `DOSIZ_DIR=/path/to/dosiz bash
tests/check_dosiz.sh`), which is run here before every emu88 commit.

The one thing worth doing first is deciding whether any dosiz fixture should
pin an x87 value at all. Right now none does, which is why 29 commits of FPU
work — including a complete change of register format — moved none of them.
That is a gap in their coverage rather than a property of the code.
