#include "sound_blaster.h"
#include <cstring>

// ============================================================================
// Sound Blaster digital-audio device. See sound_blaster.h for the wiring model.
//
// DSP version reported is SB16 4.05 (0xE1 -> 0x04, 0x05). That makes detection
// code in games pick the SB16 code path: 16-bit DMA, high sample rates, the
// 0xB0../0xC0.. command family, and the mixer. The DSP command interpreter
// below covers the commands real games actually issue for streamed PCM.
// ============================================================================

SoundBlaster::SoundBlaster(uint16_t base, int irq, int dma8, int dma16)
    : base_port(base), irq_num(irq), dma8_chan(dma8), dma16_chan(dma16) {
  reset();
}

void SoundBlaster::reset() {
  cmd = 0;
  need_params = 0;
  n_params = 0;
  std::memset(params, 0, sizeof(params));
  reset_pending = false;

  fifo_head = fifo_tail = fifo_count = 0;
  std::memset(fifo, 0, sizeof(fifo));

  mixer_index = 0;
  std::memset(mixer_regs, 0, sizeof(mixer_regs));
  // Reasonable mixer defaults: master/voice mid-high, stereo off.
  mixer_regs[0x22] = 0xCC;  // master L/R
  mixer_regs[0x04] = 0xCC;  // voice  L/R
  mixer_regs[0x0E] = 0x00;  // output/stereo control

  speaker = false;
  playing = false;
  paused = false;
  auto_init_mode = false;
  mode_16bit = false;
  stereo = false;
  signed_pcm = false;
  sample_rate_hz = 11025;   // a sane default until the guest programs a rate

  block_samples = 0;
  block_pos = 0;
  dsp_block_len = 0;
  resamp_frac = 0;
  irqs_raised = 0;

  std::memset(dma, 0, sizeof(dma));
  for (int i = 0; i < 8; i++) dma[i].masked = true;
  dma_ff_lo = dma_ff_hi = false;
}

// ----------------------------------------------------------------------------
// Read-data FIFO
// ----------------------------------------------------------------------------
void SoundBlaster::push_fifo(uint8_t b) {
  if (fifo_count >= FIFO_SZ) return;  // drop on overflow (matches HW best-effort)
  fifo[fifo_tail] = b;
  fifo_tail = (fifo_tail + 1) % FIFO_SZ;
  fifo_count++;
}

// ----------------------------------------------------------------------------
// I/O ports (rel = port - base)
//   0x04 mixer index   0x05 mixer data
//   0x06 DSP reset     0x0A DSP read-data
//   0x0C DSP write cmd/data (read: write-buffer status, bit7=1 busy)
//   0x0E DSP read-buffer status (bit7=1 data available)
// ----------------------------------------------------------------------------
void SoundBlaster::write_port(uint16_t rel, uint8_t val) {
  switch (rel) {
    case 0x04:  // mixer index
      mixer_index = val;
      return;

    case 0x05:  // mixer data
      mixer_regs[mixer_index] = val;
      // Mixer reg 0x0E bit1 selects stereo on SB Pro; SB16 uses DSP commands.
      if (mixer_index == 0x0E) stereo = (val & 0x02) != 0;
      // Mixer reset (index 0x00, any write) restores defaults.
      if (mixer_index == 0x00) {
        mixer_regs[0x22] = 0xCC;
        mixer_regs[0x04] = 0xCC;
        mixer_regs[0x0E] = 0x00;
        stereo = false;
      }
      return;

    case 0x06:  // DSP reset
      if (val & 0x01) {
        reset_pending = true;
      } else if (reset_pending) {
        reset_pending = false;
        // Abort any transfer and signal "DSP ready" (0xAA).
        playing = false;
        paused = false;
        need_params = 0;
        n_params = 0;
        cmd = 0;
        fifo_head = fifo_tail = fifo_count = 0;
        push_fifo(0xAA);
      }
      return;

    case 0x0C:  // DSP write command / data
      if (need_params > 0) {
        dsp_param(val);
      } else {
        dsp_command(val);
      }
      return;

    default:
      return;  // FM ports (0x00-0x03) and others belong to the OPL module.
  }
}

uint8_t SoundBlaster::read_port(uint16_t rel) {
  switch (rel) {
    case 0x04:  // mixer index (some drivers read it back)
      return mixer_index;
    case 0x05:  // mixer data
      return mixer_regs[mixer_index];
    case 0x0A:  // DSP read-data
      if (fifo_count > 0) {
        uint8_t v = fifo[fifo_head];
        fifo_head = (fifo_head + 1) % FIFO_SZ;
        fifo_count--;
        return v;
      }
      return 0xFF;
    case 0x0C:  // DSP write-buffer status: bit7=1 => busy. We're always ready.
      return 0x00;
    case 0x0E:  // DSP read-buffer status: bit7=1 => data available.
      // Reading this port also acknowledges the 8-bit DMA IRQ on real HW.
      return fifo_count > 0 ? 0x80 : 0x00;
    case 0x0F:  // 16-bit DMA IRQ acknowledge (read-only)
      return 0xFF;
    default:
      return 0xFF;
  }
}

// ----------------------------------------------------------------------------
// DSP command interpreter
// ----------------------------------------------------------------------------
void SoundBlaster::dsp_command(uint8_t v) {
  cmd = v;
  n_params = 0;
  need_params = 0;

  switch (v) {
    // ---- identify / version ----
    case 0xE1:  // Get DSP version -> major, minor (SB16 = 4.05)
      push_fifo(0x04);
      push_fifo(0x05);
      break;
    case 0xE0:  // DSP identify: 1 param, reply = ~param
      need_params = 1;
      break;
    case 0xE3:  // DSP copyright string -> NUL-terminated; give a short one
      push_fifo('C'); push_fifo('O'); push_fifo('P'); push_fifo('Y');
      push_fifo('R'); push_fifo('I'); push_fifo('G'); push_fifo('H');
      push_fifo('T'); push_fifo(0x00);
      break;
    case 0xE8:  // read test register
      push_fifo(0x00);
      break;

    // ---- sample-rate programming ----
    case 0x40:  // set time constant: 1 param. rate = 1e6 / (256 - tc)
      need_params = 1;
      break;
    case 0x41:  // SB16 set output sample rate: 2 params (hi, lo)
    case 0x42:  // SB16 set input  sample rate: 2 params (hi, lo)
      need_params = 2;
      break;

    // ---- block size ----
    case 0x48:  // set DMA block size: 2 params (lo, hi) => length-1
      need_params = 2;
      break;

    // ---- 8-bit single-cycle / auto-init PCM output ----
    case 0x14:  // 8-bit single-cycle DAC, 2 length params (lo, hi)
    case 0x91:  // 8-bit high-speed single-cycle (no params; uses 0x48 length)
      if (v == 0x14) { need_params = 2; }
      else { start_playback(/*autoinit*/false, /*16*/false, /*sgn*/false); }
      break;
    case 0x1C:  // 8-bit auto-init DAC (uses 0x48 block length, no params)
    case 0x90:  // 8-bit high-speed auto-init
      start_playback(/*autoinit*/true, /*16*/false, /*sgn*/false);
      break;

    // ---- SB16 general DMA (0xB0-0xCF). low nibble bits encode auto-init etc.
    // 0xB0..0xBF = 16-bit, 0xC0..0xCF = 8-bit. We need 1 mode param + 2 length.
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
    case 0xC4: case 0xC5: case 0xC6: case 0xC7:
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF:
      need_params = 3;  // mode byte, length-lo, length-hi
      break;

    // ---- speaker control ----
    case 0xD1:  // speaker on
      speaker = true;
      break;
    case 0xD3:  // speaker off
      speaker = false;
      break;
    case 0xD8:  // get speaker status -> 0xFF on, 0x00 off
      push_fifo(speaker ? 0xFF : 0x00);
      break;

    // ---- transfer control ----
    case 0xD0:  // pause 8-bit DMA
    case 0xD5:  // pause 16-bit DMA
      paused = true;
      break;
    case 0xD4:  // continue 8-bit DMA
    case 0xD6:  // continue 16-bit DMA
      paused = false;
      break;
    case 0xDA:  // exit 8-bit auto-init DMA after current block
    case 0xD9:  // exit 16-bit auto-init DMA after current block
      auto_init_mode = false;
      break;

    case 0xF2:  // trigger 8-bit IRQ (diagnostic)
      block_complete();
      break;

    default:
      break;  // unknown / FM-adjacent commands: ignore
  }
}

void SoundBlaster::dsp_param(uint8_t v) {
  params[n_params++] = v;
  need_params--;
  if (need_params > 0) return;

  switch (cmd) {
    case 0xE0:  // identify: reply with one's complement
      push_fifo((uint8_t)~params[0]);
      break;

    case 0x40: {  // set time constant -> sample rate
      int tc = params[0];
      int denom = 256 - tc;
      if (denom < 1) denom = 1;
      sample_rate_hz = 1000000 / denom;
      break;
    }
    case 0x41:  // set output sample rate (hi, lo)
      sample_rate_hz = (params[0] << 8) | params[1];
      break;
    case 0x42:  // set input sample rate (hi, lo) — store the same field
      sample_rate_hz = (params[0] << 8) | params[1];
      break;

    case 0x48:  // set DMA block size (lo, hi) => length-1
      dsp_block_len = (uint16_t)(params[0] | (params[1] << 8));
      break;

    case 0x14:  // 8-bit single-cycle: params are length-1 (lo, hi)
      dsp_block_len = (uint16_t)(params[0] | (params[1] << 8));
      start_playback(/*autoinit*/false, /*16*/false, /*sgn*/false);
      break;

    default:
      // SB16 0xB0-0xCF family: mode byte + length-1 (lo, hi).
      if ((cmd >= 0xB0 && cmd <= 0xBF) || (cmd >= 0xC0 && cmd <= 0xCF)) {
        bool sixteen = (cmd >= 0xB0 && cmd <= 0xBF);
        uint8_t modebyte = params[0];
        stereo      = (modebyte & 0x20) != 0;    // bit5: stereo
        bool sgn    = (modebyte & 0x10) != 0;    // bit4: signed samples
        bool autoin = (cmd & 0x04) != 0;         // command bit2: auto-init
        dsp_block_len = (uint16_t)(params[1] | (params[2] << 8));
        start_playback(autoin, sixteen, sgn);
      }
      break;
  }
}

// ----------------------------------------------------------------------------
// Begin a DMA-fed playback. `dsp_block_len` holds length-1 in *samples* for
// 8-bit mono; for the SB16 family the length is in samples per the command.
// We convert to a +1 sample count and reset block position.
// ----------------------------------------------------------------------------
void SoundBlaster::start_playback(bool autoinit, bool sixteen, bool sgn) {
  auto_init_mode = autoinit;
  mode_16bit = sixteen;
  signed_pcm = sgn || sixteen;   // 16-bit transfers are signed
  block_samples = (int)dsp_block_len + 1;
  if (block_samples < 1) block_samples = 1;
  block_pos = 0;
  resamp_frac = 0;
  paused = false;
  playing = true;
}

// ----------------------------------------------------------------------------
// Block completion: assert the SB IRQ, then restart (auto-init) or stop.
// ----------------------------------------------------------------------------
void SoundBlaster::block_complete() {
  int ch = active_dma();
  if (ch >= 0 && ch < 8) dma[ch].tc = true;

  irqs_raised++;
  if (raise_irq) raise_irq(irq_num);

  if (auto_init_mode) {
    // Reload the DMA channel and the block counter; keep streaming.
    if (ch >= 0 && ch < 8) {
      dma[ch].cur_addr  = dma[ch].base_addr;
      dma[ch].cur_count = dma[ch].base_count;
    }
    block_pos = 0;
    // playing stays true
  } else {
    playing = false;
  }
}

// ----------------------------------------------------------------------------
// 8237 DMA controller
//   Low controller (8-bit, channels 0-3): ports 0x00-0x0F
//     0x00..0x07: even=addr, odd=count for channels 0..3
//     0x08 status(r)/command(w)  0x0A single-mask  0x0B mode
//     0x0C clear flip-flop       0x0F all-mask
//   High controller (16-bit, channels 4-7): ports 0xC0-0xDF (word-spaced)
//     0xC0..0xCF: addr/count for channels 4..7
//     0xD4 single-mask  0xD6 mode  0xD8 clear flip-flop
//   Page registers (0x80-0x8F): ch1=0x83 ch2=0x81 ch3=0x82 ch0=0x87
//                               ch5=0x8B ch6=0x89 ch7=0x8A
// ----------------------------------------------------------------------------
void SoundBlaster::dma_write(uint16_t port, uint8_t v) {
  // ---- page registers ----
  switch (port) {
    case 0x87: dma[0].page = v; return;
    case 0x83: dma[1].page = v; return;
    case 0x81: dma[2].page = v; return;
    case 0x82: dma[3].page = v; return;
    case 0x8B: dma[5].page = v; return;
    case 0x89: dma[6].page = v; return;
    case 0x8A: dma[7].page = v; return;
    default: break;
  }
  if (port >= 0x80 && port <= 0x8F) return;  // other page regs: ignore

  // ---- low controller (channels 0-3) ----
  if (port <= 0x0F) {
    switch (port) {
      case 0x00: case 0x02: case 0x04: case 0x06: {  // address, ch = port/2
        int ch = port >> 1;
        if (!dma_ff_lo) dma[ch].base_addr = (dma[ch].base_addr & 0xFF00) | v;
        else            dma[ch].base_addr = (dma[ch].base_addr & 0x00FF) | (v << 8);
        dma[ch].cur_addr = dma[ch].base_addr;
        dma_ff_lo = !dma_ff_lo;
        return;
      }
      case 0x01: case 0x03: case 0x05: case 0x07: {  // count, ch = port/2
        int ch = port >> 1;
        if (!dma_ff_lo) dma[ch].base_count = (dma[ch].base_count & 0xFF00) | v;
        else            dma[ch].base_count = (dma[ch].base_count & 0x00FF) | (v << 8);
        dma[ch].cur_count = dma[ch].base_count;
        dma_ff_lo = !dma_ff_lo;
        return;
      }
      case 0x0A: {  // single-channel mask: bit2=set/clear, bits0-1=channel
        int ch = v & 0x03;
        dma[ch].masked = (v & 0x04) != 0;
        return;
      }
      case 0x0B: {  // mode: bits0-1 = channel
        int ch = v & 0x03;
        dma[ch].mode = v;
        return;
      }
      case 0x0C:  // clear byte flip-flop
        dma_ff_lo = false;
        return;
      case 0x0D:  // master reset
        for (int i = 0; i < 4; i++) dma[i].masked = true;
        dma_ff_lo = false;
        return;
      case 0x0F:  // write all-mask
        for (int i = 0; i < 4; i++) dma[i].masked = (v & (1 << i)) != 0;
        return;
      default:    // 0x08 command, 0x09 request, 0x0E clear-mask: accept/ignore
        if (port == 0x0E) for (int i = 0; i < 4; i++) dma[i].masked = false;
        return;
    }
  }

  // ---- high controller (channels 4-7), word-addressed at 0xC0-0xDF ----
  if (port >= 0xC0 && port <= 0xDF) {
    if (port <= 0xCF) {
      int idx = (port - 0xC0) >> 2;     // 0..3 -> channels 4..7
      int ch = 4 + idx;
      bool is_count = (port - 0xC0) & 0x02;
      if (!is_count) {
        if (!dma_ff_hi) dma[ch].base_addr = (dma[ch].base_addr & 0xFF00) | v;
        else            dma[ch].base_addr = (dma[ch].base_addr & 0x00FF) | (v << 8);
        dma[ch].cur_addr = dma[ch].base_addr;
      } else {
        if (!dma_ff_hi) dma[ch].base_count = (dma[ch].base_count & 0xFF00) | v;
        else            dma[ch].base_count = (dma[ch].base_count & 0x00FF) | (v << 8);
        dma[ch].cur_count = dma[ch].base_count;
      }
      dma_ff_hi = !dma_ff_hi;
      return;
    }
    switch (port) {
      case 0xD4: {  // single mask
        int ch = 4 + (v & 0x03);
        dma[ch].masked = (v & 0x04) != 0;
        return;
      }
      case 0xD6: {  // mode
        int ch = 4 + (v & 0x03);
        dma[ch].mode = v;
        return;
      }
      case 0xD8:  // clear flip-flop
        dma_ff_hi = false;
        return;
      case 0xDA:  // master reset
        for (int i = 4; i < 8; i++) dma[i].masked = true;
        dma_ff_hi = false;
        return;
      case 0xDE:  // clear mask
        for (int i = 4; i < 8; i++) dma[i].masked = false;
        return;
      case 0xDF:  // all-mask
        for (int i = 0; i < 4; i++) dma[4 + i].masked = (v & (1 << i)) != 0;
        return;
      default:
        return;
    }
  }
}

uint8_t SoundBlaster::dma_read(uint16_t port) {
  // page registers
  switch (port) {
    case 0x87: return dma[0].page;
    case 0x83: return dma[1].page;
    case 0x81: return dma[2].page;
    case 0x82: return dma[3].page;
    case 0x8B: return dma[5].page;
    case 0x89: return dma[6].page;
    case 0x8A: return dma[7].page;
    default: break;
  }

  if (port <= 0x0F) {
    switch (port) {
      case 0x00: case 0x02: case 0x04: case 0x06: {
        int ch = port >> 1;
        uint8_t v = dma_ff_lo ? (dma[ch].cur_addr >> 8) : (dma[ch].cur_addr & 0xFF);
        dma_ff_lo = !dma_ff_lo;
        return v;
      }
      case 0x01: case 0x03: case 0x05: case 0x07: {
        int ch = port >> 1;
        uint8_t v = dma_ff_lo ? (dma[ch].cur_count >> 8) : (dma[ch].cur_count & 0xFF);
        dma_ff_lo = !dma_ff_lo;
        return v;
      }
      case 0x08: {  // status: bits0-3 = TC reached (read-clears)
        uint8_t s = 0;
        for (int i = 0; i < 4; i++) { if (dma[i].tc) s |= (1 << i); dma[i].tc = false; }
        return s;
      }
      default:
        return 0xFF;
    }
  }

  if (port >= 0xC0 && port <= 0xDF) {
    if (port <= 0xCF) {
      int idx = (port - 0xC0) >> 2;
      int ch = 4 + idx;
      bool is_count = (port - 0xC0) & 0x02;
      uint16_t val = is_count ? dma[ch].cur_count : dma[ch].cur_addr;
      uint8_t v = dma_ff_hi ? (val >> 8) : (val & 0xFF);
      dma_ff_hi = !dma_ff_hi;
      return v;
    }
    if (port == 0xD0) {  // high-controller status
      uint8_t s = 0;
      for (int i = 4; i < 8; i++) { if (dma[i].tc) s |= (1 << (i - 4)); dma[i].tc = false; }
      return s;
    }
    return 0xFF;
  }

  return 0xFF;
}

// Compute the physical fetch address for a channel. 8-bit channels: page<<16 |
// 16-bit offset. 16-bit channels: page<<16 | (offset<<1) (word addressing).
uint32_t SoundBlaster::dma_phys(int ch) const {
  if (ch >= 4) {
    // 16-bit channel: cur_addr counts 16-bit *words*; the physical byte address
    // is (page << 16) | (word_offset << 1). The page register's low bit is
    // ignored because offset bit 15 already supplies address bit 16 (the
    // classic 8237 16-bit DMA quirk, matching DOSBox's (page & 0xFE) << 16).
    return ((uint32_t)(dma[ch].page & 0xFE) << 16) | ((uint32_t)dma[ch].cur_addr << 1);
  }
  return ((uint32_t)dma[ch].page << 16) | dma[ch].cur_addr;
}

// ----------------------------------------------------------------------------
// Fetch one source frame (L,R) from DMA memory, advancing the active channel.
// Returns false once the channel hits terminal count (after delivering the
// final sample). Output is normalized to signed-16 range in l/r.
// ----------------------------------------------------------------------------
bool SoundBlaster::fetch_frame(int32_t &l, int32_t &r) {
  int ch = active_dma();
  if (ch < 0 || ch >= 8) { l = r = 0; return false; }
  if (!mem_read) { l = r = 0; return false; }

  auto read_one = [&](int32_t &out) {
    uint32_t addr = dma_phys(ch);
    if (mode_16bit) {
      uint8_t lo = mem_read(addr);
      uint8_t hi = mem_read(addr + 1);
      int16_t s = (int16_t)((hi << 8) | lo);
      out = s;  // already signed-16
      // advance one 16-bit word
      if (dma[ch].cur_addr == 0xFFFF) { /* wrap, harmless */ }
      dma[ch].cur_addr++;
    } else {
      uint8_t b = mem_read(addr);
      if (signed_pcm) out = (int32_t)((int8_t)b) << 8;   // signed 8 -> 16
      else            out = ((int32_t)b - 128) << 8;     // unsigned 8 -> signed16
      dma[ch].cur_addr++;
    }
  };

  // decrement count, detect terminal
  auto step_count = [&]() -> bool {
    if (dma[ch].cur_count == 0) { dma[ch].cur_count = 0xFFFF; return true; }
    dma[ch].cur_count--;
    return false;
  };

  bool terminal = false;
  read_one(l);
  terminal |= step_count();
  if (stereo) {
    read_one(r);
    terminal |= step_count();
  } else {
    r = l;
  }
  return !terminal;
}

// ----------------------------------------------------------------------------
// render(): resample the DMA stream to the host rate and ADD into out[].
//
// We advance through source samples at sample_rate_hz, emitting host frames at
// `rate`. A 16.16 fixed-point accumulator tracks the source position. Each time
// we cross a whole source sample we fetch the next DMA frame; when the DMA
// channel hits terminal count we run block_complete() (IRQ + auto-init reload
// or stop). Silent when stopped, paused, speaker-off, or DMA masked.
// ----------------------------------------------------------------------------
void SoundBlaster::render(int32_t *out, int frames, int rate) {
  if (rate <= 0 || frames <= 0) return;
  if (!playing || paused || !speaker) return;
  if (sample_rate_hz <= 0) return;

  int ch = active_dma();
  if (ch < 0 || ch >= 8 || dma[ch].masked) return;

  // step = source samples advanced per host frame, in 16.16 fixed point.
  uint64_t step = ((uint64_t)sample_rate_hz << 16) / (uint32_t)rate;

  // Current held source frame. Prime it with the first fetch.
  int32_t cur_l = 0, cur_r = 0;
  bool have = fetch_frame(cur_l, cur_r);
  if (!have && block_pos == 0) {
    // Single-sample block edge: still emit this one sample then complete.
  }
  block_pos++;

  for (int i = 0; i < frames; i++) {
    out[2 * i + 0] += cur_l;
    out[2 * i + 1] += cur_r;

    resamp_frac += step;
    while (resamp_frac >= (1u << 16)) {
      resamp_frac -= (1u << 16);
      if (!have) {
        // Previous fetch hit terminal count -> finish the block.
        block_complete();
        if (!playing) {
          // Stopped: zero-fill the rest (already silent since we add nothing).
          return;
        }
        // Auto-init restarted: ch may have changed semantics but stays the same.
        ch = active_dma();
        if (ch < 0 || ch >= 8 || dma[ch].masked) { playing = false; return; }
      }
      have = fetch_frame(cur_l, cur_r);
      block_pos++;
    }
  }
}
