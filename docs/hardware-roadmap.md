# emu88 DOS-era hardware roadmap

Hardware peripherals to emulate so the **emu88** backend can fully replace
DOSBox. emu88 is the default backend as of 932af28 — `MachineConfig.swift` has
`var backend: EmulatorBackend = .emu88` and decodes a missing key as `?? .emu88`
— so this list is the gap between what the app ships and what DOS software
expects, not a wish list for an alternate engine.

**Status measured 2026-08-27**, against commit `91963f6` (committed
`Thu Aug 27 09:37:12 2026 -0400`) plus the uncommitted work described under
Unreleased in [`CHANGELOG.md`](../CHANGELOG.md), on Linux x86_64 with g++
15.2.0. Every row below was read out of `emu88/` or out of a harness in
`tests/` that was run for this pass; the harness output quoted is from that
run.

**What could not be measured on that machine**, stated once so no row is
mistaken for a result:

- **Nothing on the host side.** There is no Xcode and no Mac here, so
  `qxDOS/Bridge/` and `qxDOS/Views/` cannot be compiled, let alone run. The
  CoreAudio sink, the gamepad poll, the serial transport and the Settings
  toggles are read as source, and are marked 🖥️.
- **Nothing about the DOSBox backend.** `git submodule status` prints a leading
  `-` for `dosbox-staging`, so the submodule is uninitialized and no comparison
  against DOSBox was made.
- **Nothing audible.** The audio rows are register-level and PCM-level results
  from the unit harnesses. No one listened to any of it here.

Legend: ✅ done · 🟡 partial / detection-only · ❌ not started · 🖥️ host-side
code that is in the tree but cannot be built or run on the machine that
measured this.

Ports/interface and the rough effort/priority toward "real DOS software just
works" are noted. Priority: **P1** = biggest gaps blocking everyday DOS use,
**P2** = important, **P3** = nice-to-have / niche.

---

## Status update (core implemented)

The optional-hardware **foundation** plus a first wave of devices are in the
emu88 core, each unit-tested (`tests/hardware_test.cc` + per-module
`tests/{opl,sb,uart}_unit.cc`). All are **opt-in** via `dos_machine::Config`, so
a machine that wants none of them attaches none of them; audio is a pull model
(`dos_machine::audio_render(buf, frames, rate)`, which the host drives).

Done in the core: **AdLib/OPL2 + OPL3 FM**, **Sound Blaster** (DSP + the card's
own 8237 DMA channels + mixer + block-end IRQ), **PC speaker** (real PIT-ch2
tone), **joystick** (game port `0x201`), **16550 UART** (one port, COM1), **LPT1**
output, **Hercules 720×348** graphics.

Run for this pass, on the prebuilt binaries in `tests/build/`:

```
$ tests/build/hardware_test        # 28 "ok" lines, exit 0
=== ALL HARDWARE TESTS PASS ===
$ tests/build/opl_unit
  [OPL2] keyed channel measured ~438.0 Hz (target 440)
ALL OPL TESTS PASS
$ tests/build/sb_unit
ALL SB TESTS PASS
$ tests/build/uart_unit
ALL UART TESTS PASS (43 checks)
```

Scope of that: those four harnesses drive the device modules and the
`dos_machine` port dispatch in-process. They score register behaviour, IRQ
delivery and the *shape* of the rendered PCM. They do not score fidelity against
real silicon, and they never touch the host audio, gamepad or serial paths.

### What this file said before, and why it was wrong

Corrected 2026-08-27, in place rather than edited away, because the wrong
version is the argument for reading the rest of this file against the source.

Everything above the tables was already right. Every table below it was written
before 992c7fc, ea990a9 and ac72ee6 and was never revisited, so the file
asserted both halves of a contradiction under a heading that said "Status
reflects the current tree". Specifically, these were false as of `91963f6`:

- "**the largest gap (nothing currently produces sound)**" and "There is **no
  host audio sink** for emu88 yet". A CoreAudio sink landed in ea990a9 and is
  in `qxDOS/Bridge/Emu88Emulator.mm`. It is unbuildable here, which is a
  different statement from absent, and the file now makes that distinction with
  the 🖥️ marker instead of scoring it ❌.
- AdLib marked "🟡 detect-only" and Sound Blaster "🟡 DSP-detect-only". Both are
  full implementations in `emu88/opl.cc` and `emu88/sound_blaster.cc`. The
  detect-only stubs the old rows described *do still exist* — see the note on
  the AdLib row — but they are the fallback the guest sees when no sound card is
  configured, not the whole story.
- PC speaker marked "🟡 plumbed, silent" on the grounds that
  `dos_io::speaker_beep` is a no-op in the bridge. That is still literally true
  and no longer the point: `speaker_beep` is only the INT 10h BEL path
  (`dos_bios.cc:227`). The speaker's actual tone comes from `PCSpeaker::render`
  through `audio_render`.
- OPL3, joystick, Hercules, the 16550 UART, LPT and the 8237 DMA controller all
  marked ❌ "not started". All six are implemented and covered by a harness.
  The 8237 comes with a real qualification, which the row below now states.

Two things the old tables got right and this one keeps: EGA/VGA 16-colour planar
modes are not composited, and MPU-401, Gravis Ultrasound, CGA-composite and
Tandy do not exist.

---

## Audio — implemented; the limits are per row

Audio is a **pull** model. `dos_machine::audio_render(buf, frames, rate)` sums
every attached `AudioDevice` into an interleaved stereo `int16` buffer and
returns false when nothing is attached, so a host that never pulls costs
nothing and a headless build stays silent by construction
(`hardware_test`: "headless: no audio devices", "headless: audio_render returns
false"). The 4096-frame mixing scratch buffer used to be a caller-visible limit
- a longer request was clamped and the tail of `out` left untouched, which was a
latent trap rather than a live bug only because the CoreAudio bridge never asks
for more. It fills in chunks now, and `hardware_test` asserts a 5000-frame
request writes its tail.

The chips below are real synthesis and real DMA, not detection stubs. What they
are *not* is bit-exact.

| Device | Interface | Status | Pri | Notes |
|---|---|---|---|---|
| **Host audio output** | CoreAudio | 🖥️ | **P1** | ea990a9: `AVAudioEngine` + `AVAudioSourceNode` at 44.1 kHz stereo, whose render block drains a lock-free SPSC ring the emu thread refills to ~120 ms per batch (≤4096 frames per `audio_render` call). `audio_render` and the device port writes therefore both run on the emu thread; only the ring crosses. Not buildable here; the commit itself claims "compiles clean", not "was heard". |
| **AdLib / OPL2 (YM3812)** | ports `0x388/0x389` | ✅ | **P1** | `emu88/opl.cc`, 9 channels, 2-op, 4 waveforms, feedback, the timer/status detection path. `opl_unit` measured a channel keyed for 440 Hz at **~438.0 Hz**. Clean-room, deliberately not bit-exact: envelope rates are a `dB/s` approximation, the rhythm register `0xBD` is latched but never rendered (so the 5-voice percussion mode is silent), KSL is decoded but never applied to attenuation, and the AM/vibrato LFO phases are reset but never advanced. **Also still present:** the pre-992c7fc detect-only stub in `dos_machine.cc`'s port switch, reachable only when `sound_card == 0`, which fakes an AdLib timer at `0x388`. A guest with no sound card configured will "detect" an AdLib that produces nothing. |
| **Sound Blaster / SB Pro / SB16** | base `0x220`, IRQ 5, DMA 1/5 | ✅ | **P1** | `emu88/sound_blaster.cc`: DSP command interpreter, mixer at base+`0x04/0x05`, 8- and 16-bit streamed PCM fetched from guest physical memory over its own 8237 channels, single-cycle and auto-init, block-end IRQ on the configured line, resampling to the host rate. Reports SB16 4.05 to `0xE1`. `sb_unit` covers 11 areas including "IRQ fires exactly once at block end" and speaker-off muting. **No ADPCM** — commands `0x16`/`0x17`/`0x74`–`0x77` are absent, so a game using compressed DSP output gets silence. Same stub caveat as AdLib: with `sound_card == 0` the old detect-only DSP at `0x220` still answers `0xAA` to a reset. |
| **PC speaker** | port `0x61` + PIT ch 2 | ✅ | **P2** | `emu88/pc_speaker.h`: a square wave at `1193182 / reload`, gated by `0x61` bits 0 **and** 1. `hardware_test` asserts a gated tone is non-silent and an ungated one is silent. **No PWM / sample mode.** `audio_render` re-samples the tone from the live PIT reload and `port_b` once *per host buffer* (~120 ms in the bridge), so a program that produces digitized speaker audio by bit-banging `0x61` faster than that is invisible to the model. `dos_io::speaker_beep`, the INT 10h BEL path, is a separate no-op in the bridge. |
| **OPL3 (YMF262)** | `0x388`–`0x38B`, SB base+`0x00`–`0x03` | ✅ | P2 | Second register bank; reg `0x105` (NEW) switches to 18 channels, reg `0x104` pairs channels into 4-op voices, and reg `0xC0`+ carries L/R panning. `opl_unit` checks detection, enable and panning. Attached automatically with the Sound Blaster (`sound_card == 2`); the SB FM alias at base+`0x00`–`0x03` routes to the same object. Inherits every OPL2 fidelity limit above. |
| **MPU-401 (MIDI)** | port `0x330`, UART mode | ❌ | P3 | Nothing in `emu88/` answers `0x330`. External/General-MIDI music; pair with a host SoundFont or GM synth. |
| **Roland MT-32 / General MIDI** | via MPU-401 | ❌ | P3 | High-end DOS music; large effort (LA synthesis / GM map). |
| **Gravis Ultrasound (GF1)** | base `0x240`, RAM | ❌ | P3 | Wavetable; niche but beloved (demoscene). |
| **Tandy/PCjr 3-voice, Game Blaster (CMS)** | `0xC0`, `0x220` | ❌ | P3 | Early/again niche. |

## Input

| Device | Interface | Status | Pri | Notes |
|---|---|---|---|---|
| **Keyboard (8042)** | ports `0x60/0x64`, INT 9/16h | ✅ | — | Scancode queue, IRQ 1 raised through the master PIC, INT 16h including the enhanced `AH=10h/11h/12h` calls. `tests/bios_test.cc` covers the INT 16h side as of 2026-08-27 - `AH=00`/`01`/`02`/`03`/`05`/`09`/`10`/`11`/`12`, the BDA ring wrap and the peek that must not consume. The **8042 port model** at `0x60`/`0x64` and IRQ 1 delivery have no harness and are exercised only incidentally, every time a suite boots. |
| **Mouse** | INT 33h (+ host) | ✅ | — | Functions `0x00`–`0x08`, `0x0A`–`0x0C`, `0x0F`, `0x15`, `0x1A`, `0x21` and `0x24`. **`0x09` (define graphics cursor) is absent** - the switch goes from `0x0008` straight to `0x000A`, so a guest that sets a custom cursor shape silently gets nothing. Coordinate scaling across all video modes including SVGA is covered end-to-end by `tests/vesa_test.cc`. |
| **Joystick / game port** | port `0x201` | ✅ | **P1** | Analog 4-axis / 4-button read via the resistive-timing protocol: a write to `0x201` arms four monostables whose expiry is scaled from the host axis value, and buttons read back active-low. Absent when not configured — reads `0xFF`. Four `hardware_test` checks. Host half (ac72ee6, 🖥️): `GCController.current.extendedGamepad` polled on the emu thread every 32 batches, with a SwiftUI on-screen pad able to override through atomics the poll consumes, plus a Settings toggle. |
| **PS/2 wheel / 3-button mouse** | INT 33h ext | 🟡 | P3 | Function `0x00` reports `BX = 3` buttons and press/release counters exist for all three. Scroll-wheel (function `0x11`) is not implemented. |

## Display / Video

| Device | Status | Pri | Notes |
|---|---|---|---|
| **MDA / CGA / EGA text** | ✅ | — | Text modes composited. `tests/bios_test.cc` covers the INT 10h side as of 2026-08-27 - mode set and the BDA fields it writes, cursor, scroll with all four window edges, write-char and TTY including the scroll at the bottom line, and `AH=1Ah` for all five display configurations. One divergence it pins: the default configuration reports display combination code `02h` (CGA) from `AH=1Ah` while the same BIOS implements mode 13h and VESA VBE 2.0. |
| **VGA — text, mode 13h, Mode X** | ✅ | — | 320×200×256 linear (chain-4) plus unchained planar, both composited in `emit_video_frame`. |
| **SVGA / VESA VBE 2.0** | ✅ | — | S3-class, 8 MB LFB at `0xE0000000` plus the bank-switched window at `0xA0000`, a 25-entry mode table: 640×400 at 8 bpp, 320×200 at 15/16/24/32 bpp, and 640×480, 800×600, 1024×768 and 1280×1024 at all five depths. (The row said "640×480 to 1280×1024" before, inherited from the pre-rewrite version; six of the 25 are below 640×480.) `tests/vesa_test.cc` PASSes, and it executes the emitted `4F0A` protected-mode routines rather than trusting them. |
| **Hercules graphics (720×348 mono)** | ✅ | P2 | `herc_composite()` reads the four interleaved 8 KB banks at `0xB0000`/`0xB8000` and emits a 720×348 frame through the existing video pipeline. `hardware_test` composites one and checks a lit pixel. Three conditions must all hold: `Config::display == DISPLAY_HERCULES`, `0x3BF` bit 0 (graphics allowed), `0x3B8` bit 1 (graphics mode). Reachable from the app — `machineType == 4` maps to `.hercules` — though that path is 🖥️ and unverified here. The palette is hard-coded amber-on-black. |
| **EGA/VGA 16-color planar modes (0x0D–0x12)** | ❌ | P2 | `video_set_mode` has no case for them: they fall through to the `default:` branch, which sets 80×25 text metrics, and `emit_video_frame` then composites them as text. What exists is not this mode — the 4×64 KB plane store, the sequencer map-mask and the GC read-map in `emu88_mem` are Mode X's, and there are no GC write modes 1/2/3, no set/reset and no bit-mask register. Mode 12h is the last common mode DOS applications assume. |
| **CGA composite / Tandy graphics** | ❌ | P3 | Color-artifact NTSC and Tandy 16-color. Nothing in `emu88/` mentions either. |
| **VESA Linear-only modes / VBE 3.0 / hi-res palette** | 🟡 | P3 | VBE 2.0 covers the field; 3.0 (CRTC/refresh control) optional. |

## Serial / Parallel / Comms

| Device | Interface | Status | Pri | Notes |
|---|---|---|---|---|
| **Serial UART (16550)** | one port, default `0x3F8` / IRQ 4 | ✅ | **P2** | `emu88/uart16550.cc`: RX/TX FIFOs with programmable trigger level, DLAB-banked divisor latch, line/modem control and status, local loopback, and RX/THRE interrupts polled every 1024 instructions in `run_batch`. `uart_unit` passes 43 checks. **Two real limits.** It is **one** port, not COM1–4: `dos_machine` holds a single `UART16550*` at `Config::serial_iobase`, so COM2/3/4 do not exist. And there is **no baud throttle** — the divisor is stored and readable but never used for timing, and `transmit()` hands the byte to the host immediately, with THR-empty and transmitter-empty true again on return. Host half (ac72ee6, 🖥️): `serial_tx`/`serial_rx` over thread-safe FIFOs, surfaced as `-emulatorSerialOutput:` and `-sendSerialData:`, plus a Settings toggle. |
| **Parallel port (LPT)** | LPT1, default `0x378` | 🟡 | P3 | Core is done and tested: data latch, control register, host output on the strobe rising edge, status hardwired to `0xDF` (ready, no error). `hardware_test` asserts "LPT emitted 'A' on strobe". **Nothing consumes it in the app.** `dos_io::lpt_output` is not overridden in `qxDOS/Bridge/` — the base no-op in `emu88/dos_io.h` is what runs — and there is no Settings toggle for `parallel_enabled`, so on the shipping app LPT is switched off and would discard bytes if it were not. Printing to a host file/PDF is the missing half. |
| **Covox Speech Thing / Disney** | LPT data | ❌ | P3 | 8-bit DAC over LPT. Needs the LPT host sink above *and* a change of shape: a Covox writes the data port with no strobe, and `port_out` only calls `lpt_output` on a strobe rising edge, so today a Covox write is latched and dropped. |
| **NE2000 Ethernet** | base `0x300`, IRQ 3, libslirp | ✅ | — | `emu88/ne2000.cc`, 32 ports dispatched at the configured base, with 16-bit data-port access; host side is `qxDOS/Bridge/Emu88SlirpNet.mm`. `tests/ne2000_test.cc` covers the card itself as of 2026-08-27 - 220 assertions over the DP8390 register model, remote DMA, transmit, the receive ring and its filter, which found and fixed two defects including a received frame overwriting the card's own MAC PROM. **Scope:** that harness is the module in isolation; the port decode in `dos_machine.cc` and the IRQ delivery are not exercised by it, and mTCP/FDNET were not run on this machine. The end-to-end working claim is still inherited from the commit that added it. |

## System board / chipset

| Device | Interface | Status | Pri | Notes |
|---|---|---|---|---|
| **8259 PIC** | `0x20/0x21` | 🟡 | P2 | Master only, and the old row's "should be confirmed" is now answered both ways. **There is no slave PIC at all** — `0xA0`/`0xA1` have no case in `port_out`/`port_in`, so writes are dropped, reads return `0xFF`, and IRQ 8–15 cannot be delivered. The RTC therefore has no IRQ 8; it is INT 1Ah and CMOS only. SB16 on IRQ 5 does work — it is a master line, and `sb_unit` asserts the block-end IRQ fires once on the configured line. What is modelled is one IMR, one vector base and the ICW1–ICW4 init sequence; **EOI is parsed and discarded**, there is no in-service register, and there is no priority or nesting. |
| **8254 PIT** | `0x40–0x43` | ✅ | — | Timer tick + ch 2 gate (which is what drives the PC speaker). |
| **8237 DMA** | `0x00–0x0F`, `0x80–0x8F`, `0xC0–0xDF` | 🟡 | P3 | Implemented, and no longer blocking anything, but it is **not a system DMA controller**. It lives inside `emu88/sound_blaster.cc` and the ports are only routed to it when a Sound Blaster is attached (`if (sb)` in both `port_out` and `port_in`). Only the card's two jumpered channels are driven — 8-bit ch 1 and 16-bit ch 5 by default; other channels are accepted and ignored so the dispatch stays mechanical. Address/count/page registers, the low and high flip-flops and the 16-bit word-addressing quirk are all modelled for those two. |
| **CMOS / RTC** | `0x70/0x71`, INT 1Ah | ✅ | — | Clock/config. No periodic-interrupt path (see the PIC row). |
| **A20 gate** | `0x92` / 8042 | ✅ | — | Real, unreal and protected mode. `tests/dpmi_test.cc` asserts A20 is forced on across the DPMI mode switch. |
| **FPU (x87)** | — | ✅ | — | 387-class, in `emu88_fpu.cc`. `tests/fpu_test.cc` PASSes 470 assertions over ~74 mnemonics. **Scope, because "✅" oversells it:** the register stack is `double regs[8]`, 53 mantissa bits rather than 80-bit extended, so 31 assertions pin values that provably differ from a real 387 — no denormal class, precision control ignored entirely, `F2XM1` computed as `pow(2,x)-1`. Nine conformance defects this harness recorded were fixed on 2026-08-27 and the baseline is 0; the register format is not a defect and is not fixed. `tests/README.md` §4 lists both. |

## Storage

| Device | Status | Pri | Notes |
|---|---|---|---|
| **Floppy (INT 13h), HDD** | ✅ | — | mmap-backed images, emulated at the BIOS level rather than at the controller. `tests/bios_test.cc` covers the INT 13h side as of 2026-08-27 against RAM-backed drives - `AH=00/02/03/04/08/15/41/42/43/48`, a verified read/write round trip and the error paths - which found and fixed two defects, including a CHS sector number of 0 underflowing the LBA to 2^64-1 and asking `dos_io` to read at byte offset 2^64-512. |
| **CD-ROM / MSCDEX** | 🟡 | P3 | The old row said "confirm MSCDEX INT 2Fh coverage". Confirmed, and the answer is that there is none: `bios_int2fh` handles `1680h`, `1687h`, `4300h` and `4310h` and nothing else, so the whole MSCDEX `15xx` family is absent and a program that probes for MSCDEX finds nothing. What does work is the INT 13h side — an ISO mounts as drive `≥0xE0`, LBA only, no CHS geometry, plus the El Torito status call `AH=4Bh`. Redbook audio is separate and also absent. |
| **8237-driven floppy DMA** | ❌ | P3 | Does *not* "come with the DMA controller above" — that controller is inside the Sound Blaster and only exists when one is attached. Nothing needs this today, because INT 13h is emulated at BIOS level against mmap'd images and no guest ever programs a floppy DMA channel that emu88 would have to honour. It becomes real only if a controller-level FDC is ever emulated. |

---

## Suggested order (to "drop DOSBox")

The audio foundation, the 8237 channels the Sound Blaster needs, AdLib, Sound
Blaster, the joystick, the PC speaker tone, Hercules, OPL3, the UART and LPT
were the first six steps of the old version of this list. They are done. What is
genuinely unstarted, in the order that buys the most compatibility:

1. **EGA/VGA 16-color planar modes `0x0D`–`0x12`** (P2). The largest remaining
   hole in everyday DOS use: mode 12h (640×480×16) is what a great deal of
   non-game DOS software assumes. Needs the mode table entries, GC write modes
   1/2/3 with set/reset and the bit-mask register, and a planar compositor.
2. **Slave PIC and a real EOI** (P2). `0xA0`/`0xA1` are unhandled, so IRQ 8–15
   do not exist, and EOI is discarded, so there is no priority or nesting. This
   is small, and it is what any device wanting IRQ 8+ (including a periodic RTC)
   will hit first.
3. **An LPT host sink** (P3), then **Covox** (P3) on top of it. The core LPT is
   done; the bridge overrides nothing and has no toggle, so bytes go nowhere.
   Covox additionally needs the strobe-less data-port write to reach the host.
4. **MPU-401 / General MIDI** (P3), then MT-32 if ever. Nothing answers `0x330`.
5. **Gravis Ultrasound** (P3). Nothing answers `0x240`.
6. **CGA composite / Tandy graphics** (P3). Neither exists in any form.

Separately, and **only closable on a Mac**: everything marked 🖥️. The CoreAudio
sink, the gamepad poll and the serial transport are written and compile-checked
against the macOS and iOS SDKs per their commits, and have never been run.
Confirming them is a device run, not a code change.

Each device should land with a focused harness in `tests/` (the way
`vesa_test.cc` drives the VESA BIOS, and the way `opl_unit`/`sb_unit`/`uart_unit`
drive the modules directly) so behavior is regression-checked without the iOS
app.

## Not covered by anything in this repo

A gap named is worth more than a feature listed.

- **The host side has no tests and cannot get any here.** `tests/build.sh`
  builds eleven harnesses; none of them links a line of Objective-C or Swift.
  Every 🖥️ row is source review.
- **Nothing compares emu88 against DOSBox.** The submodule is uninitialized on
  this machine, so "emu88 can replace DOSBox" is an argument from feature lists
  in this file, not a measured equivalence.
- **Audio fidelity is untested by construction.** `opl_unit` measures pitch
  (~438.0 Hz against a 440 Hz target) and asserts that a keyed channel is
  audible and a released one is not; `sb_unit` asserts PCM shape, IRQ timing and
  muting. Neither compares a rendered buffer against a reference recording, and
  the OPL core is explicitly not bit-exact, so "correct" here means "a DOS
  program's music plays at the right pitch with the right envelopes", not "the
  same samples a YM3812 would emit".
- **The keyboard has no dedicated harness beyond the BIOS layer.**
  `tests/bios_test.cc` covers INT 16h and the BDA ring; the 8042 port model at
  `0x60`/`0x64` and IRQ 1 delivery are exercised only incidentally by whatever
  boots.
- **The detect-only stubs are untested, and reachable.** The pre-992c7fc AdLib
  timer and SB DSP stubs still sit in `dos_machine.cc`'s port switch, reachable
  whenever `sound_card == 0` - which is the default `Config`. No harness covers
  that configuration, and a guest that probes finds hardware that produces
  nothing: a game that detects an AdLib configures itself for FM music and then
  plays none. `todo.txt` carries it, open, with the reason it was not just
  deleted.
- **The NE2000's guest-facing side is untested.** `tests/ne2000_test.cc` drives
  the card in isolation; the port decode in `dos_machine.cc`, the 16-bit port
  paths and the IRQ delivery are not exercised by it, and mTCP and FDNET have
  never been run on this machine.

## Downstream consumer: dosiz

Changes in this file's subject matter do not stay in this repository. The
sibling **dosiz** project (`/home/wohl/src/dosiz`) compiles six emu88 files
straight out of this working tree by relative path — `EMU88_DIR` in
`dosiz/src/CMakeLists.txt` defaults to `${CMAKE_SOURCE_DIR}/../../qxDOS/emu88`
and the list is `emu88.cc`, `emu88_pmode.cc`, `emu88_fpu.cc`, `emu88_mem.cc`,
**`opl.cc`** and **`sound_blaster.cc`**. No submodule, no vendored copy, no
pinned SHA.

Two consequences for anyone working through the list above:

- **The OPL and Sound Blaster modules have a second consumer.** dosiz does not
  use `dos_machine`'s port dispatch at all; `dosiz/src/hardware.cc` constructs
  `OPL` and `SoundBlaster` itself and drives their public API. That API is
  exactly what `opl_unit` and `sb_unit` exercise, which is the reason those
  harnesses drive the modules directly instead of going through the machine.
  Keep it that way.
- **The obligation runs one direction.** `dosiz/CLAUDE.md` states it: "emu88
  belongs to qxDOS. Do not fix emu88 bugs from this repo", pointing at
  `qxDOS/tests/` as the gate. Fixes land here and are validated here.
