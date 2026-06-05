# emu88 DOS-era hardware roadmap

Hardware peripherals to emulate so the **emu88** backend can fully replace
DOSBox. Status reflects the current tree.

Legend: ✅ done · 🟡 partial / detection-only · ❌ not started

Ports/interface and the rough effort/priority toward "real DOS software just
works" are noted. Priority: **P1** = biggest gaps blocking everyday DOS use,
**P2** = important, **P3** = nice-to-have / niche.

---

## Status update (core implemented)

The optional-hardware **foundation** plus a first wave of devices are now in the
emu88 core, each unit-tested (`tests/hardware_test.cc` + per-module
`tests/{opl,sb,uart}_unit.cc`). All are **opt-in** via `dos_machine::Config`, so a
text-only/compiler machine (dosiz) attaches none of them; audio is a pull model
(`dos_machine::audio_render(buf, frames, rate)` the host drives).

Done in the core: **AdLib/OPL2+OPL3 FM**, **Sound Blaster** (DSP + 8237 DMA +
mixer + IRQ), **PC speaker** (real PIT-ch2 tone), **joystick** (game port 0x201),
**16550 UART** (COM1), **LPT** output, **Hercules 720×348** graphics.

Remaining is mostly **host wiring** (qxDOS bridge): a CoreAudio sink that pulls
`audio_render`, gamepad→`set_joystick`, serial/LPT host hooks, and Settings
toggles. (Hercules already displays — it uses the existing video pipeline.)
Still unstarted: EGA/VGA planar 16-color modes, MPU-401/General-MIDI, Gravis
Ultrasound, CGA-composite. See the tables below for the full picture.

---

## Audio  — the largest gap (nothing currently produces sound)

There is **no host audio sink** for emu88 yet (the SwiftUI/CoreAudio side outputs
no PCM); the chips below are detection-only. The first task is an output path
(AVAudioEngine/AudioQueue ring buffer fed from a mixer), then the synths.

| Device | Interface | Status | Pri | Notes |
|---|---|---|---|---|
| **Host audio output** | CoreAudio | ❌ | **P1** | Prerequisite for everything below. ~44.1 kHz stereo ring buffer + a per-frame mixer tick. |
| **AdLib / OPL2 (YM3812)** | ports `0x388/0x389` | 🟡 detect-only | **P1** | FM synthesis (2-op, 9 ch). The single most-used DOS music chip. A known DSP (e.g. a Nuked-OPL2-style core) drives most game music. |
| **Sound Blaster / SB Pro / SB16** | base `0x220`, IRQ 5/7, DMA 1/5 | 🟡 DSP-detect-only | **P1** | Digitized audio (8/16-bit PCM via DMA), the DSP command set, the mixer, and OPL3 (SB Pro2/16). Needs the 8237 DMA controller (below). This + AdLib covers ~all DOS game audio. |
| **PC speaker** | port `0x61` + PIT ch 2 | 🟡 plumbed, silent | **P2** | `dos_io::speaker_beep` is a no-op in the bridge. Want real tone output (PIT-driven square wave) and PWM/sample mode for speaker "digitized" sound and music. |
| **OPL3 (YMF262)** | `0x388` / SB Pro2 | ❌ | P2 | 4-op / stereo superset of OPL2; ships with SB Pro 2 / SB16. |
| **MPU-401 (MIDI)** | port `0x330`, UART mode | ❌ | P3 | External/General-MIDI music; pair with a host SoundFont or GM synth. |
| **Roland MT-32 / General MIDI** | via MPU-401 | ❌ | P3 | High-end DOS music; large effort (LA synthesis / GM map). |
| **Gravis Ultrasound (GF1)** | base `0x240`, RAM | ❌ | P3 | Wavetable; niche but beloved (demoscene). |
| **Tandy/PCjr 3-voice, Game Blaster (CMS)** | `0xC0`, `0x220` | ❌ | P3 | Early/again niche. |

## Input

| Device | Interface | Status | Pri | Notes |
|---|---|---|---|---|
| **Keyboard (8042)** | ports `0x60/0x64`, INT 9/16h | ✅ | — | Working. |
| **Mouse** | INT 33h (+ host) | ✅ | — | Working; now coordinate-scaled across all video modes incl. SVGA. |
| **Joystick / game port** | port `0x201` | ❌ | **P1** | Analog 2-axis/2-button (and 4/4) read via the resistive-timing protocol on `0x201`. Many games need it; map to a host gamepad / on-screen pad. |
| **PS/2 wheel / 3-button mouse** | INT 33h ext | 🟡 | P3 | 3 buttons reported; scroll-wheel (func 0x11) not surfaced. |

## Display / Video

| Device | Status | Pri | Notes |
|---|---|---|---|
| **MDA / CGA / EGA text** | ✅ | — | Text modes composited. |
| **VGA — text, mode 13h, Mode X** | ✅ | — | 320×200×256 linear + unchained planar. |
| **SVGA / VESA VBE 2.0** | ✅ | — | S3-class, 8 MB LFB, 640×480–1280×1024 @ 8/15/16/24/32 bpp (just added). |
| **Hercules graphics (720×348 mono)** | ❌ | **P2** | MDA text works; the HGC `0x3B8` graphics-enable + the 720×348 page at `0xB0000` are not composited. Wanted for mono-CAD / early apps. |
| **EGA/VGA 16-color planar modes (0x0D–0x12)** | 🟡 | P2 | `video_set_mode` only really handles text + 13h; planar 640×480×16 (mode 12h) etc. aren't composited. |
| **CGA composite / Tandy graphics** | ❌ | P3 | Color-artifact NTSC and Tandy 16-color. |
| **VESA Linear-only modes / VBE 3.0 / hi-res palette** | 🟡 | P3 | VBE 2.0 covers the field; 3.0 (CRTC/refresh control) optional. |

## Serial / Parallel / Comms

| Device | Interface | Status | Pri | Notes |
|---|---|---|---|---|
| **Serial UART (16550)** | COM1–4 `0x3F8/0x2F8/…`, INT 4/3 | ❌ | **P2** | Null-modem/modem games, terminal apps, and SLIP/PPP. Map to a host socket / virtual modem. |
| **Parallel port (LPT)** | `0x378/0x278`, INT 7 | ❌ | P3 | Printing → host file/PDF; also some dongles/sound (Covox). |
| **Covox Speech Thing / Disney** | LPT data | ❌ | P3 | 8-bit DAC over LPT; trivial once LPT + audio exist. |
| **NE2000 Ethernet** | base `0x300`, libslirp | ✅ | — | mTCP/FDNET work (shared NAT with DOSBox path). |

## System board / chipset

| Device | Interface | Status | Pri | Notes |
|---|---|---|---|---|
| **8259 PIC** | `0x20/0x21` (+ `0xA0/0xA1`) | 🟡 | P2 | Master PIC works; **slave PIC** (IRQ 8–15) and full EOI/priority should be confirmed for SB16 (IRQ 5) and RTC (IRQ 8). |
| **8254 PIT** | `0x40–0x43` | ✅ | — | Timer tick + ch 2 gate. |
| **8237 DMA** | `0x00–0x0F`, `0xC0–0xDF` | ❌ | **P1** | Required for Sound Blaster digitized playback (and floppy DMA). Blocks SB audio. |
| **CMOS / RTC** | `0x70/0x71`, INT 1Ah | ✅ | — | Clock/config. |
| **A20 gate** | `0x92` / 8042 | ✅ | — | Working (real + unreal + PM). |
| **FPU (x87)** | — | ✅ | — | 387-class, in `emu88_fpu.cc`. |

## Storage

| Device | Status | Pri | Notes |
|---|---|---|---|
| **Floppy (INT 13h), HDD** | ✅ | — | mmap-backed images. |
| **CD-ROM / MSCDEX** | 🟡 | P3 | ISO mounts as drive `0xE0`; confirm MSCDEX INT 2Fh coverage for CD games (audio-CD redbook is separate). |
| **8237-driven floppy DMA** | ❌ | P3 | Comes with the DMA controller above. |

---

## Suggested order (to "drop DOSBox")

1. **Audio foundation** → host PCM sink + **8237 DMA** + **AdLib/OPL2** + **Sound Blaster** (P1). This is the headline gap: most DOS software currently runs silent.
2. **Joystick `0x201`** (P1) — small, high game-compat payoff.
3. **PC-speaker real tone/PWM** (P2) — falls out of the audio sink.
4. **Hercules 720×348 + EGA/VGA planar 16-color modes** (P2).
5. **16550 UART / COM** (P2) for modem/terminal/SLIP.
6. **OPL3, MPU-401/GM, LPT printing, slave PIC polish** (P3) as needed.

Each device should land with a focused harness in `tests/` (the way `vesa_test.cc`
drives the VESA BIOS) so behavior is regression-checked without the iOS app.
