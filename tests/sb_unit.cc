// Standalone unit test for emu88's SoundBlaster module. Drives the public API
// directly (no dos_machine, no port dispatch) and asserts DSP/DMA/render
// behavior. Build & run:
//   clang++ -std=c++20 -I emu88 tests/sb_unit.cc emu88/sound_blaster.cc
//       -o /tmp/sb_unit && /tmp/sb_unit
#include "sound_blaster.h"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cstring>

static int g_fails = 0;
static void CK(const char *name, bool ok) {
  if (!ok) { printf("FAIL: %s\n", name); g_fails++; }
}

// A flat fake guest memory the SB's mem_read callback reads from.
static std::vector<uint8_t> g_mem;

int main() {
  g_mem.assign(0x100000, 0);  // 1 MB

  SoundBlaster sb(0x220, 5, /*dma8*/1, /*dma16*/5);
  sb.mem_read = [](uint32_t a) -> uint8_t {
    return (a < g_mem.size()) ? g_mem[a] : 0;
  };
  int irq_fires = 0, last_irq = -1;
  sb.raise_irq = [&](int irq) { irq_fires++; last_irq = irq; };

  const uint16_t B = 0x220;
  auto W = [&](uint16_t abs, uint8_t v) { sb.write_port(abs - B, v); };
  auto R = [&](uint16_t abs) -> uint8_t { return sb.read_port(abs - B); };

  // ---- 1) DSP reset handshake: write 1 then 0 to 0x226, read 0xAA @ 0x22A ----
  W(0x226, 1);
  W(0x226, 0);
  CK("reset status shows data available", (R(0x22E) & 0x80) != 0);
  CK("reset handshake returns 0xAA", R(0x22A) == 0xAA);
  CK("FIFO empty after draining 0xAA", (R(0x22E) & 0x80) == 0);

  // ---- 2) Version query: 0xE1 -> two bytes (SB16 4.05) ----
  CK("write-buffer ready (bit7=0)", (R(0x22C) & 0x80) == 0);
  W(0x22C, 0xE1);
  uint8_t vmaj = R(0x22A);
  uint8_t vmin = R(0x22A);
  CK("DSP version major == 4", vmaj == 4);
  CK("DSP version minor == 5", vmin == 5);

  // ---- 2b) DSP identify (0xE0): reply is one's complement of param ----
  W(0x22C, 0xE0);
  W(0x22C, 0x55);
  CK("DSP identify echoes ~param", R(0x22A) == (uint8_t)~0x55);

  // ---- 2c) Mixer read/write round-trips ----
  W(0x224, 0x22);     // mixer index = master volume
  W(0x225, 0xAB);     // data
  W(0x224, 0x22);
  CK("mixer reg round-trips", R(0x225) == 0xAB);

  // ---- 3) Program a small DMA block in fake memory ----
  // 8-step ramp: 0,32,64,...,224 (8-bit unsigned). Place it at phys 0x12300.
  const uint32_t PHYS = 0x12300;
  const int N = 8;
  for (int i = 0; i < N; i++) g_mem[PHYS + i] = (uint8_t)(i * 32);

  // Channel 1: page=0x01, offset=0x2300 -> phys 0x12300, count = N-1.
  sb.dma_write(0x0C, 0);            // clear flip-flop (low controller)
  sb.dma_write(0x83, 0x01);        // ch1 page
  sb.dma_write(0x02, 0x00);        // ch1 addr low
  sb.dma_write(0x02, 0x23);        // ch1 addr high
  sb.dma_write(0x03, (N - 1) & 0xFF);   // ch1 count low  (length-1)
  sb.dma_write(0x03, ((N - 1) >> 8));   // ch1 count high
  sb.dma_write(0x0B, 0x49);        // mode: single, read, ch1
  sb.dma_write(0x0A, 0x01);        // unmask ch1 (bit2=0, ch=1)

  // ---- 4) Set a sample rate via time constant (0x40). tc for ~11025 Hz. ----
  // rate = 1e6 / (256 - tc). For tc=165 -> 1e6/91 = 10989 Hz.
  W(0x22C, 0x40);
  W(0x22C, 165);
  CK("sample rate programmed (~11kHz)", sb.sample_rate() > 9000 && sb.sample_rate() < 13000);

  // Speaker on so output is audible.
  W(0x22C, 0xD1);
  CK("speaker on", sb.speaker_on());

  // ---- 5) 0x14 single-cycle 8-bit playback, length = N-1 ----
  W(0x22C, 0x14);
  W(0x22C, (N - 1) & 0xFF);   // length low
  W(0x22C, (N - 1) >> 8);     // length high
  CK("playing after 0x14", sb.is_playing());
  CK("single-cycle (not auto-init)", !sb.auto_init());
  CK("8-bit mode", !sb.is_16bit());

  // Render at the SAME rate as programmed so 1 host frame == 1 source sample.
  // That makes the output track the source ramp exactly.
  const int RATE = sb.sample_rate();
  const int FR = 64;
  std::vector<int32_t> buf(2 * FR, 0);
  sb.render(buf.data(), FR, RATE);

  // Non-silent output.
  int nonzero = 0;
  for (int i = 0; i < FR; i++) if (buf[2 * i] != 0) nonzero++;
  CK("render produced non-silent output", nonzero > 0);

  // Samples track the buffer: unsigned 8-bit b -> (b-128)<<8. The first source
  // sample is 0 -> (0-128)<<8 = -32768. Sample i (i<N) is (i*32 - 128)<<8.
  bool tracks = true;
  for (int i = 0; i < N; i++) {
    int32_t expect = ((int32_t)(i * 32) - 128) << 8;
    if (buf[2 * i] != expect)   { tracks = false; }
    if (buf[2 * i + 1] != expect) { tracks = false; }  // mono duplicated to R
  }
  CK("output samples track the DMA buffer (8-bit unsigned->signed16)", tracks);

  // ---- 6) IRQ fires exactly once at block end, on the SB IRQ line ----
  CK("raise_irq fired exactly once at block end", irq_fires == 1);
  CK("raise_irq used the configured IRQ (5)", last_irq == 5);
  CK("playback stopped after single-cycle block", !sb.is_playing());

  // ---- 7) Stopped state is silent ----
  std::vector<int32_t> sbuf(2 * FR, 7777);   // pre-fill; render must ADD nothing
  // Re-zero to detect additions cleanly.
  std::fill(sbuf.begin(), sbuf.end(), 0);
  sb.render(sbuf.data(), FR, RATE);
  bool silent = true;
  for (size_t i = 0; i < sbuf.size(); i++) if (sbuf[i] != 0) silent = false;
  CK("stopped state renders silence", silent);

  // ---- 8) Speaker-off mutes even while playing ----
  // Re-arm a playback, then disable speaker; render must be silent.
  for (int i = 0; i < N; i++) g_mem[PHYS + i] = (uint8_t)(i * 32);
  sb.dma_write(0x0C, 0);
  sb.dma_write(0x83, 0x01);
  sb.dma_write(0x02, 0x00);
  sb.dma_write(0x02, 0x23);
  sb.dma_write(0x03, (N - 1) & 0xFF);
  sb.dma_write(0x03, ((N - 1) >> 8));
  sb.dma_write(0x0A, 0x01);
  W(0x22C, 0x14);
  W(0x22C, (N - 1) & 0xFF);
  W(0x22C, (N - 1) >> 8);
  W(0x22C, 0xD3);   // speaker OFF
  CK("speaker off flag", !sb.speaker_on());
  std::vector<int32_t> mbuf(2 * FR, 0);
  sb.render(mbuf.data(), FR, RATE);
  bool muted = true;
  for (auto s : mbuf) if (s != 0) muted = false;
  CK("speaker-off mutes output", muted);
  W(0x22C, 0xD1);   // speaker back on

  // ---- 9) Auto-init (0x1C) restarts and keeps firing IRQs each block ----
  {
    SoundBlaster a(0x220, 7, 1, 5);     // different IRQ to check routing
    a.mem_read = [](uint32_t x) -> uint8_t { return (x < g_mem.size()) ? g_mem[x] : 0; };
    int afires = 0, alast = -1;
    a.raise_irq = [&](int irq){ afires++; alast = irq; };
    for (int i = 0; i < N; i++) g_mem[PHYS + i] = (uint8_t)(i * 32);
    a.dma_write(0x0C, 0);
    a.dma_write(0x83, 0x01);
    a.dma_write(0x02, 0x00);
    a.dma_write(0x02, 0x23);
    a.dma_write(0x03, (N - 1) & 0xFF);
    a.dma_write(0x03, ((N - 1) >> 8));
    a.dma_write(0x0A, 0x01);
    a.write_port(0x0C, 0x40); a.write_port(0x0C, 165);   // rate
    a.write_port(0x0C, 0x48); a.write_port(0x0C, (N-1)&0xFF); a.write_port(0x0C, (N-1)>>8); // block size
    a.write_port(0x0C, 0xD1);     // speaker on
    a.write_port(0x0C, 0x1C);     // 8-bit auto-init
    CK("auto-init mode set", a.auto_init());
    CK("auto-init playing", a.is_playing());
    // Render ~3 blocks worth at native rate.
    std::vector<int32_t> ab(2 * (3 * N), 0);
    a.render(ab.data(), 3 * N, a.sample_rate());
    CK("auto-init still playing after blocks", a.is_playing());
    CK("auto-init raised multiple IRQs", afires >= 2);
    CK("auto-init IRQ routed to configured line (7)", alast == 7);
    // Exit auto-init: 0xDA stops at next block end.
    a.write_port(0x0C, 0xDA);
    CK("0xDA clears auto-init flag", !a.auto_init());
  }

  // ---- 10) 16-bit signed playback via SB16 command 0xB0 ----
  {
    SoundBlaster s16(0x220, 5, 1, 5);
    s16.mem_read = [](uint32_t x) -> uint8_t { return (x < g_mem.size()) ? g_mem[x] : 0; };
    int f16 = 0; s16.raise_irq = [&](int){ f16++; };
    // Build 4 signed-16 samples at phys 0x20000 on the 16-bit channel 5.
    const uint32_t P16 = 0x20000;
    int16_t s16data[4] = { -30000, -10000, 10000, 30000 };
    std::memcpy(&g_mem[P16], s16data, sizeof(s16data));
    // 16-bit channel 5 uses word addressing: offset = (phys>>1) within page.
    // page register holds bits 16.. ; with page=(0x20000>>16)=0x02 and
    // cur_addr = (0x20000 & 0xFFFF) >> 1 = 0. dma_phys: (page&0xFE)<<15 | addr<<1.
    // High-controller layout: each channel takes 4 bytes from 0xC0.
    //   ch4 addr=0xC0 count=0xC2 ; ch5 addr=0xC4 count=0xC6 ; ...
    s16.dma_write(0xD8, 0);              // clear hi flip-flop
    s16.dma_write(0x8B, 0x02);          // ch5 page
    s16.dma_write(0xC4, 0x00);          // ch5 addr low (word)
    s16.dma_write(0xC4, 0x00);          // ch5 addr high
    s16.dma_write(0xC6, 0x03);          // ch5 count low (4 samples -> len-1=3)
    s16.dma_write(0xC6, 0x00);          // ch5 count high
    s16.dma_write(0xD4, 0x01);          // unmask ch5 (bit2=0, ch index 1 => chan5)
    s16.write_port(0x0C, 0x41);         // set output sample rate
    s16.write_port(0x0C, 0x2B);         // 11025 hi
    s16.write_port(0x0C, 0x11);         // 11025 lo  (0x2B11 = 11025)
    CK("16-bit rate via 0x41 == 11025", s16.sample_rate() == 0x2B11);
    s16.write_port(0x0C, 0xD1);         // speaker on
    s16.write_port(0x0C, 0xB0);         // 16-bit single-cycle
    s16.write_port(0x0C, 0x10);         // mode: signed (bit4), mono
    s16.write_port(0x0C, 0x03);         // length low (len-1)
    s16.write_port(0x0C, 0x00);         // length high
    CK("16-bit mode flagged", s16.is_16bit());
    std::vector<int32_t> b16(2 * 16, 0);
    s16.render(b16.data(), 16, s16.sample_rate());
    bool ok16 = true;
    for (int i = 0; i < 4; i++) {
      if (b16[2 * i] != s16data[i]) ok16 = false;
    }
    CK("16-bit signed samples pass through unchanged", ok16);
    CK("16-bit block fired one IRQ", f16 == 1);
  }

  // ---- 11) Resampling: half host rate down-samples (each src sample held ~2) ---
  {
    SoundBlaster rs(0x220, 5, 1, 5);
    rs.mem_read = [](uint32_t x) -> uint8_t { return (x < g_mem.size()) ? g_mem[x] : 0; };
    int rf = 0; rs.raise_irq = [&](int){ rf++; };
    const int M = 4;
    const uint32_t PR = 0x30000;
    for (int i = 0; i < M; i++) g_mem[PR + i] = (uint8_t)(64 + i * 16);
    rs.dma_write(0x0C, 0);
    rs.dma_write(0x83, (PR >> 16) & 0xFF);
    rs.dma_write(0x02, PR & 0xFF);
    rs.dma_write(0x02, (PR >> 8) & 0xFF);
    rs.dma_write(0x03, (M - 1) & 0xFF);
    rs.dma_write(0x03, (M - 1) >> 8);
    rs.dma_write(0x0A, 0x01);
    rs.write_port(0x0C, 0x40); rs.write_port(0x0C, 165);  // ~11kHz source
    rs.write_port(0x0C, 0xD1);
    rs.write_port(0x0C, 0x14);
    rs.write_port(0x0C, (M - 1) & 0xFF);
    rs.write_port(0x0C, (M - 1) >> 8);
    // Render at DOUBLE the source rate: each source sample should appear ~twice.
    int hostrate = rs.sample_rate() * 2;
    std::vector<int32_t> rb(2 * 32, 0);
    rs.render(rb.data(), 32, hostrate);
    // First two host frames should equal source sample 0.
    int32_t e0 = ((int32_t)g_mem[PR] - 128) << 8;
    CK("upsample holds sample across frames", rb[0] == e0 && rb[2] == e0);
    CK("resample still completes one block", rf == 1);
  }

  if (g_fails == 0) printf("ALL SB TESTS PASS\n");
  else              printf("%d FAILURES\n", g_fails);
  return g_fails ? 1 : 0;
}
