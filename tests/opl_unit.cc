// Standalone unit test for emu88/opl.{h,cc} — the AdLib OPL2/OPL3 FM synth.
//
// Build & run:
//   clang++ -std=c++20 -I emu88 tests/opl_unit.cc emu88/opl.cc -o /tmp/opl_unit
//   /tmp/opl_unit
//
// Exercises the module directly (no DOS machine): the AdLib detection sequence,
// register round-trips, timer/status behaviour, key-on tone generation, pitch,
// key-off silence, and a little OPL3 panning.

#include "opl.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); g_fail++; } \
} while (0)

// Render `frames` and return peak absolute sample (left channel) + RMS.
static void render_stats(OPL &opl, int frames, int rate,
                         double &peakL, double &peakR, double &rms) {
  std::vector<int32_t> buf(frames * 2, 0);
  opl.render(buf.data(), frames, rate);
  peakL = peakR = 0.0;
  double sum = 0.0;
  for (int i = 0; i < frames; i++) {
    double l = std::fabs((double)buf[2 * i + 0]);
    double r = std::fabs((double)buf[2 * i + 1]);
    if (l > peakL) peakL = l;
    if (r > peakR) peakR = r;
    sum += (double)buf[2 * i] * buf[2 * i];
  }
  rms = std::sqrt(sum / frames);
}

// Count zero-crossings on the left channel to estimate fundamental frequency.
static double estimate_freq(OPL &opl, int frames, int rate) {
  std::vector<int32_t> buf(frames * 2, 0);
  opl.render(buf.data(), frames, rate);
  int crossings = 0;
  int prev = 0;
  for (int i = 0; i < frames; i++) {
    int s = buf[2 * i];
    if (prev <= 0 && s > 0) crossings++;
    prev = s;
  }
  // crossings (rising) over the time window = cycles.
  double seconds = (double)frames / rate;
  return crossings / seconds;
}

//-----------------------------------------------------------------------------
// AdLib detection sequence (the classic probe used by every DOS game/lib):
//   1. Reset both timers (reg 0x04 = 0x60: mask T1+T2).
//   2. Reset IRQ           (reg 0x04 = 0x80).
//   3. Read status -> expect 0 in the timer bits.
//   4. Start timer 1 with preset 0xFF (reg 0x02=0xFF), enable+unmask (reg 0x04=0x21).
//   5. Wait ~80us, read status -> expect bit7 and bit6 set.
//   6. Reset (reg 0x04=0x60 then 0x80); status timer bits clear again.
//-----------------------------------------------------------------------------
static void test_adlib_detection(OPL &opl, const char *tag) {
  // Step 1+2: mask + reset.
  opl.write_port(0, 0x04); opl.write_port(1, 0x60);   // mask both timers
  opl.write_port(0, 0x04); opl.write_port(1, 0x80);   // IRQ reset
  uint8_t st = opl.read_port(0);
  CHECK((st & 0xE0) == 0x00, "detect: status timer bits clear after reset");

  // Step 4: program timer 1, preset 0xFF (=> 1 tick of 80us), start unmasked.
  opl.write_port(0, 0x02); opl.write_port(1, 0xFF);   // timer-1 data
  opl.write_port(0, 0x04); opl.write_port(1, 0x21);   // start T1, unmask T1
  // Status should not yet show the flag (no time has elapsed / render run).
  st = opl.read_port(0);
  CHECK((st & 0x80) == 0x00, "detect: no IRQ before time elapses");

  // Step 5: advance time by rendering ~100us worth of audio at 49716 Hz.
  // 100us at 49716 Hz ~= 5 frames; render a comfortable margin.
  double pL, pR, rms;
  render_stats(opl, 64, 49716, pL, pR, rms); // ~1.3ms -> plenty for an 80us tick
  st = opl.read_port(0);
  CHECK((st & 0x80) != 0, "detect: IRQ (bit7) set after timer 1 expires");
  CHECK((st & 0x40) != 0, "detect: timer-1 flag (bit6) set after expiry");
  CHECK((st & 0x20) == 0, "detect: timer-2 flag (bit5) stays clear");

  // Step 6: reset clears the flags again.
  opl.write_port(0, 0x04); opl.write_port(1, 0x80);   // IRQ reset
  st = opl.read_port(0);
  CHECK((st & 0xE0) == 0x00, "detect: status clears after second reset");

  std::printf("  [%s] AdLib detection sequence OK\n", tag);
}

//-----------------------------------------------------------------------------
int main() {
  // ---- OPL2 path ----------------------------------------------------------
  {
    OPL opl(false);  // OPL2
    CHECK(!opl.is_opl3(), "ctor: OPL2 default");

    // read_port(1) is open bus.
    CHECK(opl.read_port(1) == 0xFF, "read_port(1) == 0xFF");

    test_adlib_detection(opl, "OPL2");

    // ---- Register round-trip via write_reg (operator params decode) -------
    // We can't read operator registers back (write-only chip), but we can
    // verify the *effect*: program a channel and confirm it makes sound.
    opl.reset();

    // Set up channel 0, operator 0 (modulator) + operator 1 (carrier).
    // Modulator: reg 0x20 (mult), 0x40 (TL), 0x60 (AR/DR), 0x80 (SL/RR), 0xE0 wave
    // Carrier:   regs +0x03.
    auto wr = [&](int reg, uint8_t v){ opl.write_port(0, reg); opl.write_port(1, v); };

    // Carrier loud, fast attack, no decay, full sustain.
    // bit5 (0x20) = EG-type "sustaining" so the tone holds while keyed on.
    wr(0x23, 0x21);  // carrier mult=1, sustaining
    wr(0x20, 0x21);  // modulator mult=1, sustaining
    wr(0x43, 0x00);  // carrier TL=0 (loudest)
    wr(0x40, 0x3F);  // modulator TL=63 (quietest) so carrier dominates
    wr(0x63, 0xF0);  // carrier AR=15 (instant), DR=0
    wr(0x60, 0xF0);  // modulator AR=15, DR=0
    wr(0x83, 0x0F);  // carrier SL=0, RR=15
    wr(0x80, 0x0F);  // modulator SL=0, RR=15
    wr(0xE3, 0x00);  // carrier waveform = sine
    wr(0xE0, 0x00);  // modulator waveform = sine
    wr(0xC0, 0x01);  // feedback=0, algo=1 (additive) so carrier audible directly

    // Frequency: target ~440 Hz. f = Fnum * 49716 / 2^(20-block).
    // Choose block=4 -> denom = 2^16 = 65536. Fnum = 440*65536/49716 ~= 580.
    int block = 4;
    int fnum = (int)std::lround(440.0 * 65536.0 / 49716.0);
    wr(0xA0, fnum & 0xFF);
    wr(0xB0, ((fnum >> 8) & 0x03) | (block << 2));    // key OFF still

    // Key OFF -> silence.
    double pL, pR, rms;
    render_stats(opl, 1024, 49716, pL, pR, rms);
    CHECK(pL < 1.0, "key-off: silent (left)");
    CHECK(pR < 1.0, "key-off: silent (right)");

    // Key ON -> audible.
    wr(0xB0, ((fnum >> 8) & 0x03) | (block << 2) | 0x20);  // key-on bit
    // Discard a warm-up block (attack), then measure.
    render_stats(opl, 256, 49716, pL, pR, rms);
    render_stats(opl, 4096, 49716, pL, pR, rms);
    CHECK(pL > 100.0, "key-on: produces audible output (left)");
    CHECK(pR > 100.0, "key-on: produces audible output (right) [OPL2 dup mono]");
    CHECK(std::fabs(pL - pR) < 1.0, "OPL2: L and R identical (mono duplicated)");

    // Pitch check: estimated frequency near 440 Hz (loose: zero-cross method).
    double f = estimate_freq(opl, 49716, 49716);  // 1 second window
    std::printf("  [OPL2] keyed channel measured ~%.1f Hz (target 440)\n", f);
    CHECK(f > 380.0 && f < 500.0, "key-on: pitch near target 440 Hz");

    // Key OFF again -> decays to silence (RR=15 is fast).
    wr(0xB0, ((fnum >> 8) & 0x03) | (block << 2));   // clear key-on
    render_stats(opl, 8192, 49716, pL, pR, rms);     // let release finish
    render_stats(opl, 1024, 49716, pL, pR, rms);
    CHECK(pL < 50.0, "key-off after key-on: returns to (near) silence");

    std::printf("  [OPL2] tone generation / pitch / key gating OK\n");
  }

  // ---- OPL3 path ----------------------------------------------------------
  {
    OPL opl(true);   // OPL3
    CHECK(opl.is_opl3(), "ctor: OPL3");

    // Detection works on bank 0 just like OPL2.
    test_adlib_detection(opl, "OPL3");

    // Enable OPL3 (NEW bit, reg 0x105 via bank-1 port pair 2/3).
    opl.write_port(2, 0x05); opl.write_port(3, 0x01);

    auto wr0 = [&](int reg, uint8_t v){ opl.write_port(0, reg); opl.write_port(1, v); };

    // Program channel 0 to make a tone, then test L/R panning via reg 0xC0.
    wr0(0x43, 0x00);  // carrier TL loud
    wr0(0x40, 0x3F);  // modulator quiet
    wr0(0x63, 0xF0);  // carrier AR=15
    wr0(0x60, 0xF0);  // modulator AR=15
    wr0(0x83, 0x0F);  // carrier RR=15
    wr0(0x80, 0x0F);
    wr0(0xE3, 0x00);
    wr0(0xE0, 0x00);
    wr0(0x20, 0x21); wr0(0x23, 0x21);  // sustaining envelopes
    int block = 4, fnum = 580;
    wr0(0xA0, fnum & 0xFF);

    // Pan LEFT only: reg 0xC0 bit4 = left, bit5 = right. Left-only = 0x10.
    wr0(0xC0, 0x11);  // algo=1, left on, right off
    wr0(0xB0, ((fnum >> 8) & 0x03) | (block << 2) | 0x20);  // key-on
    double pL, pR, rms;
    render_stats(opl, 256, 49716, pL, pR, rms);
    render_stats(opl, 2048, 49716, pL, pR, rms);
    CHECK(pL > 100.0, "OPL3 pan-left: left audible");
    CHECK(pR < 1.0,   "OPL3 pan-left: right silent");

    // Switch to RIGHT only.
    wr0(0xC0, 0x21);  // right on, left off
    render_stats(opl, 2048, 49716, pL, pR, rms);
    CHECK(pR > 100.0, "OPL3 pan-right: right audible");
    CHECK(pL < 1.0,   "OPL3 pan-right: left silent");

    // Both.
    wr0(0xC0, 0x31);
    render_stats(opl, 2048, 49716, pL, pR, rms);
    CHECK(pL > 100.0 && pR > 100.0, "OPL3 pan-both: both audible");

    std::printf("  [OPL3] enable + L/R panning OK\n");
  }

  // ---- Timer 2 independent path ------------------------------------------
  {
    OPL opl(false);
    opl.write_port(0, 0x04); opl.write_port(1, 0x80);  // reset
    opl.write_port(0, 0x03); opl.write_port(1, 0xFF);  // timer-2 data (1 tick=320us)
    opl.write_port(0, 0x04); opl.write_port(1, 0x02);  // start T2, unmask T2 (bit5 clear)
    double a, b, c;
    render_stats(opl, 64, 49716, a, b, c);  // ~1.3ms >> 320us
    uint8_t st = opl.read_port(0);
    CHECK((st & 0x80) != 0, "timer2: IRQ bit set on expiry");
    CHECK((st & 0x20) != 0, "timer2: T2 flag (bit5) set");
    CHECK((st & 0x40) == 0, "timer2: T1 flag stays clear");
    std::printf("  [OPL2] timer-2 independent overflow OK\n");
  }

  // ---- Masked timer does not raise status --------------------------------
  {
    OPL opl(false);
    opl.write_port(0, 0x04); opl.write_port(1, 0x80);  // reset
    opl.write_port(0, 0x02); opl.write_port(1, 0xFF);  // T1 data
    opl.write_port(0, 0x04); opl.write_port(1, 0x41);  // start T1 but MASK T1 (bit6)
    double a, b, c;
    render_stats(opl, 64, 49716, a, b, c);
    uint8_t st = opl.read_port(0);
    CHECK((st & 0x80) == 0, "masked timer1: no IRQ flag");
    std::printf("  [OPL2] masked timer suppresses IRQ OK\n");
  }

  if (g_fail == 0) std::printf("ALL OPL TESTS PASS\n");
  else             std::printf("%d FAILURES\n", g_fail);
  return g_fail ? 1 : 0;
}
