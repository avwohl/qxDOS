#ifndef SOUND_BLASTER_H
#define SOUND_BLASTER_H

#include "audio_device.h"
#include <cstdint>
#include <functional>

// Sound Blaster (SB / SB Pro / SB16) digital-audio device for emu88.
//
// This module models the three pieces that make DMA-driven digital sound work:
//   1. the DSP command interpreter (ports base+0x06,0x0A,0x0C,0x0E),
//   2. the mixer (ports base+0x04,0x05),
//   3. the 8237 DMA controller channels that feed PCM bytes from guest memory.
//
// FM (OPL2/OPL3) is intentionally NOT handled here — the OPL module owns the
// 0x388/0x389 (and base+0x00..0x03) FM register ports. A SoundBlaster may be
// told an OPL exists for status/detection purposes, but it never touches FM.
//
// Threading model matches the rest of emu88: register writes happen on the CPU
// thread (write_port / dma_write), and the host pulls rendered PCM at its own
// output rate via render() (an AudioDevice). DMA fetches read guest physical
// memory through the mem_read callback; the block-end IRQ is delivered through
// raise_irq. Both are supplied by the main agent after construction.
//
// Wiring (done by the main agent, NOT in this file):
//   - Route I/O ports base..base+0x0F to write_port/read_port (rel = port-base).
//   - Route the 8-bit DMA controller ports (0x00-0x0F), the page registers
//     (0x80-0x8F), and the 16-bit DMA controller ports (0xC0-0xDF) to
//     dma_write/dma_read.
//   - Set mem_read to read one guest physical byte.
//   - Set raise_irq to assert the configured IRQ line (default 5).
//   - Add the SoundBlaster* to the machine's audio_dev[] list so render() runs.
class SoundBlaster : public AudioDevice {
public:
  // base = I/O base (default 0x220). irq = the ISA IRQ line asserted on block
  // completion (default 5). dma8/dma16 = the 8-bit and 16-bit DMA channels the
  // card is jumpered to (defaults: 1 and 5, the classic SB16 layout).
  explicit SoundBlaster(uint16_t base = 0x220, int irq = 5,
                        int dma8 = 1, int dma16 = 5);

  // --- Callbacks the main agent installs after construction ---
  std::function<uint8_t(uint32_t)> mem_read;   // read one guest physical byte
  std::function<void(int irq)>     raise_irq;  // assert the SB IRQ at block end

  // --- I/O port access (rel = absolute_port - base, 0..0x0F) ---
  void    write_port(uint16_t rel, uint8_t val);
  uint8_t read_port(uint16_t rel);

  // --- 8237 DMA controller port access ---
  // The main agent routes absolute ports 0x00-0x0F, 0x80-0x8F and 0xC0-0xDF
  // here. Only the channels this card uses (dma8_chan / dma16_chan) are modeled;
  // other channels are accepted and ignored so the dispatch stays mechanical.
  void    dma_write(uint16_t port, uint8_t v);
  uint8_t dma_read(uint16_t port);

  // --- AudioDevice ---
  void reset() override;
  void render(int32_t *out, int frames, int rate) override;

  // --- Introspection (used by the unit test; harmless in production) ---
  bool     is_playing()    const { return playing; }
  bool     speaker_on()    const { return speaker; }
  int      sample_rate()   const { return sample_rate_hz; }
  int      irq_line()      const { return irq_num; }
  uint16_t io_base()       const { return base_port; }
  bool     auto_init()     const { return auto_init_mode; }
  bool     is_16bit()      const { return mode_16bit; }
  uint64_t irq_count()     const { return irqs_raised; }

private:
  // ----- configuration -----
  uint16_t base_port;
  int      irq_num;
  int      dma8_chan;    // 8-bit DMA channel (0..3), classic = 1
  int      dma16_chan;   // 16-bit DMA channel (4..7), classic = 5

  // ----- DSP command-port state (base+0x0C / base+0x0A / base+0x0E) -----
  // The DSP is a tiny state machine: a command byte may be followed by 0..N
  // parameter bytes. We collect parameters then act. Replies (version bytes,
  // identify echo, etc.) are pushed into a small read FIFO drained at base+0x0A.
  uint8_t  cmd;                // command currently collecting parameters
  int      need_params;        // parameter bytes still expected for `cmd`
  uint8_t  params[8];          // collected parameter bytes
  int      n_params;

  // DSP reset handshake (base+0x06): write 1 then 0 -> 0xAA appears at read FIFO.
  bool     reset_pending;

  // Read-data FIFO (drained at base+0x0A; status at base+0x0E bit7).
  static constexpr int FIFO_SZ = 16;
  uint8_t  fifo[FIFO_SZ];
  int      fifo_head, fifo_tail, fifo_count;

  // Last byte written to E0 (DSP identify) — reply is its one's complement.
  // ----- mixer (base+0x04 index, base+0x05 data) -----
  uint8_t  mixer_index;
  uint8_t  mixer_regs[256];

  // ----- transfer / playback state -----
  bool     speaker;            // D1/D3 speaker enable (gates output)
  bool     playing;            // a DMA transfer is in progress
  bool     paused;             // D0 pause / D4 continue
  bool     auto_init_mode;     // 1C/1E/2C... auto-init vs single-cycle (14/16/24)
  bool     mode_16bit;         // 16-bit signed PCM vs 8-bit unsigned
  bool     stereo;             // stereo PCM (SB Pro / SB16)
  bool     signed_pcm;         // 16-bit programmed transfers are signed
  int      sample_rate_hz;     // programmed output rate in Hz

  // Block length. 0x48 (and the 14/1C/B0.. command params) set this in *samples
  // minus one* per the hardware convention; we store the +1 sample count.
  int      block_samples;      // samples per block (length+1)
  int      block_pos;          // samples emitted in the current block

  // DSP "set block size" (0x48) staging.
  uint16_t dsp_block_len;      // length-1 programmed via 0x48

  // Fractional resampler accumulator: how far we are between source samples,
  // in units of source-samples, scaled by 2^16 for integer math.
  uint32_t resamp_frac;

  uint64_t irqs_raised;        // diagnostics

  // ----- 8237 DMA channel state -----
  struct DMAChan {
    uint16_t base_addr;   // programmed start offset (within page)
    uint16_t cur_addr;    // current offset
    uint16_t base_count;  // programmed length-1
    uint16_t cur_count;   // remaining-1 (counts down; underflow => terminal)
    uint8_t  page;        // page register (high address bits)
    uint8_t  mode;        // 8237 mode byte
    bool     masked;      // channel masked (no transfers)
    bool     flipflop;    // address/count byte flip-flop (false=low next)
    bool     tc;          // terminal-count latched (read-clears in status)
  };
  DMAChan  dma[8];        // 8 channels; we only drive dma8_chan / dma16_chan
  bool     dma_ff_lo;     // low controller (ch0-3) flip-flop
  bool     dma_ff_hi;     // high controller (ch4-7) flip-flop

  // ----- helpers -----
  void  push_fifo(uint8_t b);
  void  dsp_command(uint8_t v);
  void  dsp_param(uint8_t v);
  void  start_playback(bool autoinit, bool sixteen, bool sgn);
  void  block_complete();      // assert IRQ + restart (auto-init) or stop
  int   active_dma() const { return mode_16bit ? dma16_chan : dma8_chan; }
  uint32_t dma_phys(int ch) const;   // current physical fetch address
  // Fetch one source frame (L,R) from DMA memory, advancing the channel; returns
  // false at terminal count (caller then runs block_complete()).
  bool  fetch_frame(int32_t &l, int32_t &r);
};

#endif // SOUND_BLASTER_H
