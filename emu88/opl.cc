// AdLib-compatible OPL2 (YM3812) / OPL3 (YMF262) FM synth.
// Compact DBOPL/Nuked-OPL2-style core: not bit-exact, but the register model,
// AdLib detection / timer / status path, pitch, and ADSR are correct enough to
// drive real DOS music (AdLib/Sound Blaster FM) and to pass the detection probe.

#include "opl.h"
#include <cmath>
#include <cstring>

//=============================================================================
// Register-slot -> operator-index map.
//
// The OPL register file addresses operators by a non-contiguous offset within
// 0x00..0x15 (with gaps at 0x06,0x07,0x0E,0x0F,0x16..0x1F). The canonical AdLib
// layout pairs operators into the 9 channels as:
//   ch  modulator-offset  carrier-offset
//    0        0x00              0x03
//    1        0x01              0x04
//    2        0x02              0x05
//    3        0x08              0x0B
//    4        0x09              0x0C
//    5        0x0A              0x0D
//    6        0x10              0x13
//    7        0x11              0x14
//    8        0x12              0x15
// We flatten register offsets 0x00..0x15 into our 22 used operator slots and
// keep a reverse map. Internally we store 18 channels * 2 ops = 36 operators
// (slot index = chan*2 + slot), and translate register offsets to that.
//=============================================================================

// Register operator-offset (0x00..0x15) for [channel][modulator=0,carrier=1].
static const int kOpRegOff[9][2] = {
  {0x00, 0x03}, {0x01, 0x04}, {0x02, 0x05},
  {0x08, 0x0B}, {0x09, 0x0C}, {0x0A, 0x0D},
  {0x10, 0x13}, {0x11, 0x14}, {0x12, 0x15},
};

// Reverse: register offset (0x00..0x1F) -> (chan*2+slot) within a bank, or -1.
static int regoff_to_op(int off) {
  for (int c = 0; c < 9; c++) {
    if (off == kOpRegOff[c][0]) return c * 2 + 0;
    if (off == kOpRegOff[c][1]) return c * 2 + 1;
  }
  return -1;
}

//=============================================================================
// Sine / waveform tables. We use a 1024-entry sine and synthesize the 4 OPL2
// waveforms (and OPL3's 4 extra) on the fly from it.
//=============================================================================
static double g_sine[1024];
static bool   g_tables_ready = false;

static void init_tables() {
  if (g_tables_ready) return;
  for (int i = 0; i < 1024; i++)
    g_sine[i] = std::sin(2.0 * M_PI * (double)i / 1024.0);
  g_tables_ready = true;
}

// Waveform shaper: index is 10-bit (0..1023) phase.
static double waveform_sample(int wf, uint32_t idx) {
  idx &= 1023;
  double s = g_sine[idx];
  switch (wf & 7) {
    case 0: return s;                                  // full sine
    case 1: return (idx < 512) ? s : 0.0;              // half sine
    case 2: return std::fabs(s);                       // absolute sine
    case 3: {                                          // quarter sine (pulse)
      int q = idx & 511;
      return (q < 256) ? std::fabs(g_sine[idx & 1023]) : 0.0;
    }
    case 4: return (idx < 512) ? g_sine[(idx * 2) & 1023] : 0.0; // OPL3: alt sine
    case 5: return (idx < 512) ? std::fabs(g_sine[(idx * 2) & 1023]) : 0.0;
    case 6: return (idx < 512) ? 1.0 : -1.0;           // OPL3: square
    case 7: {                                          // OPL3: derived/log-saw
      double x = (idx < 512) ? (double)idx / 512.0
                             : (double)(1024 - idx) / 512.0;
      return s >= 0 ? x : -x;
    }
  }
  return s;
}

//=============================================================================
// Attenuation: total_level + ksl + envelope, expressed in "dB-ish" units.
// We keep env_level as attenuation in dB (0 = full, ~96 = silent) for clarity.
//=============================================================================
static double db_to_lin(double db) {
  if (db >= 96.0) return 0.0;
  return std::pow(10.0, -db / 20.0);
}

// Envelope rate -> dB per second. OPL rates 0..15 map roughly logarithmically.
// These are approximate but give audibly correct attack/decay behaviour.
static double rate_attack_per_sec(int r) {
  if (r <= 0) return 0.0;
  // Fastest (15) ~ a few ms; slowest ~ seconds. Use dB/s.
  return 96.0 * std::pow(2.0, (r - 4) / 2.0);
}
static double rate_decay_per_sec(int r) {
  if (r <= 0) return 0.0;
  return 96.0 * std::pow(2.0, (r - 8) / 2.0);
}

//=============================================================================
OPL::OPL(bool opl3) : opl3_(opl3) {
  init_tables();
  num_chans_ = opl3_ ? 18 : 9;
  reset();
}

OPL::~OPL() {}

void OPL::reset() {
  std::memset(ops_, 0, sizeof(ops_));
  std::memset(chans_, 0, sizeof(chans_));
  for (int c = 0; c < NUM_CHANS; c++) {
    chans_[c].left = 1;
    chans_[c].right = 1;
  }
  for (int o = 0; o < NUM_OPS; o++) {
    ops_[o].mult2 = 1;
    ops_[o].env_state = ENV_OFF;
    ops_[o].env_level = 96.0;  // silent
  }
  reg_index_[0] = reg_index_[1] = 0;
  reg_bank_sel_ = 0;
  timer1_data_ = timer2_data_ = timer_ctrl_ = 0;
  timer1_acc_ = timer2_acc_ = 0.0;
  status_ = 0;
  rhythm_ = 0;
  opl3_enabled_ = false;
  num_chans_ = opl3_ ? (opl3_enabled_ ? 18 : 9) : 9;
  lfo_am_phase_ = lfo_vib_phase_ = 0;
}

//=============================================================================
// Index helpers: a register's low byte (0x00..0xFF) plus its bank (0/1) maps
// onto an operator (for 0x20/0x40/0x60/0x80/0xE0) or a channel (0xA0/0xB0/0xC0).
//=============================================================================
int OPL::op_index(int reg_lo, int bank) const {
  int off = reg_lo & 0x1F;
  int o = regoff_to_op(off);
  if (o < 0) return -1;
  // Bank 1 (OPL3) channels are 9..17, operators 18..35.
  return o + bank * 18;
}

int OPL::chan_index(int reg_lo, int bank) const {
  int c = reg_lo & 0x0F;
  if (c > 8) return -1;
  return c + bank * 9;
}

//=============================================================================
// Decode register-derived fields into the live operator struct.
//=============================================================================
void OPL::decode_op(int opi) {
  Operator &op = ops_[opi];
  int m = op.am_vib_eg_ksr_mult & 0x0F;
  static const int mult_tbl[16] = {1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30};
  op.mult2 = mult_tbl[m];               // already *2 relative to .5 unit
  op.am     = (op.am_vib_eg_ksr_mult >> 7) & 1;
  op.vib    = (op.am_vib_eg_ksr_mult >> 6) & 1;
  op.eg_type= (op.am_vib_eg_ksr_mult >> 5) & 1;
  op.ksr    = (op.am_vib_eg_ksr_mult >> 4) & 1;

  op.ksl        = (op.ksl_tl >> 6) & 3;
  op.total_level= op.ksl_tl & 0x3F;

  op.attack = (op.ar_dr >> 4) & 0x0F;
  op.decay  = op.ar_dr & 0x0F;
  op.sustain= (op.sl_rr >> 4) & 0x0F;
  op.release= op.sl_rr & 0x0F;
}

//=============================================================================
void OPL::key_on(int ch) {
  if (ch < 0 || ch >= NUM_CHANS) return;
  chans_[ch].keyon = 1;
  for (int s = 0; s < 2; s++) {
    Operator &op = ops_[ch * 2 + s];
    op.env_state = ENV_ATTACK;
    op.phase = 0;
    // If attack rate is max, jump straight to full level.
    if (op.attack >= 15) { op.env_level = 0.0; op.env_state = ENV_DECAY; }
  }
  chans_[ch].fb_out1 = chans_[ch].fb_out2 = 0.0;
}

void OPL::key_off(int ch) {
  if (ch < 0 || ch >= NUM_CHANS) return;
  chans_[ch].keyon = 0;
  for (int s = 0; s < 2; s++) {
    Operator &op = ops_[ch * 2 + s];
    if (op.env_state != ENV_OFF) op.env_state = ENV_RELEASE;
  }
}

//=============================================================================
// write_reg: the heart of the register model.
//=============================================================================
void OPL::write_reg(int reg, uint8_t val) {
  int bank = (reg >> 8) & 1;        // bit 8 selects OPL3 bank
  if (!opl3_) bank = 0;
  int lo = reg & 0xFF;

  // --- Global / timer registers (bank 0 only; OPL3 mirrors at 0x100 too) ---
  if (lo == 0x01) {                 // test register / waveform-select enable
    // bit5 = waveform select enable (OPL2). We always allow waveforms.
    return;
  }
  if (lo == 0x02) { timer1_data_ = val; return; }
  if (lo == 0x03) { timer2_data_ = val; return; }
  if (lo == 0x04) {
    if (bank == 1 && opl3_) {
      // reg 0x104: 4-operator connection enable (bits 0..5).
      // Store and refresh 4-op pairing.
      // We stash it in rhythm_'s upper bits is messy; keep a dedicated field
      // via reuse of timer_ctrl high — instead recompute from a static.
      fourop_mask_ = val & 0x3F;
      update_fourop();
      return;
    }
    timer_ctrl_ = val;
    if (val & 0x80) {               // IRQ-RESET: clear all status flags
      status_ = 0;
      // (Mask bits 0x40/0x20 also clear specific flags, but 0x80 clears all.)
      return;                       // when bit7 set, other bits are ignored
    }
    if (val & 0x40) status_ &= ~0x40; // mask T1 -> clear its flag
    if (val & 0x20) status_ &= ~0x20; // mask T2 -> clear its flag
    // Restart accumulators when a timer is (re)started.
    if (val & 0x01) timer1_acc_ = 0.0;
    if (val & 0x02) timer2_acc_ = 0.0;
    return;
  }

  if (lo == 0x05 && opl3_ && bank == 1) {  // reg 0x105: OPL3 enable (NEW)
    opl3_enabled_ = val & 1;
    num_chans_ = opl3_enabled_ ? 18 : 9;
    update_fourop();
    return;
  }

  if (lo == 0x08) {                 // CSW / NOTE-SEL — affects ksr split; ignore
    return;
  }

  if (lo == 0xBD) {                 // rhythm / AM-depth / VIB-depth
    rhythm_ = val;
    return;
  }

  // --- Operator registers ---
  if (lo >= 0x20 && lo <= 0x35) {
    int o = op_index(lo, bank);
    if (o >= 0) { ops_[o].am_vib_eg_ksr_mult = val; decode_op(o); }
    return;
  }
  if (lo >= 0x40 && lo <= 0x55) {
    int o = op_index(lo, bank);
    if (o >= 0) { ops_[o].ksl_tl = val; decode_op(o); }
    return;
  }
  if (lo >= 0x60 && lo <= 0x75) {
    int o = op_index(lo, bank);
    if (o >= 0) { ops_[o].ar_dr = val; decode_op(o); }
    return;
  }
  if (lo >= 0x80 && lo <= 0x95) {
    int o = op_index(lo, bank);
    if (o >= 0) { ops_[o].sl_rr = val; decode_op(o); }
    return;
  }
  if (lo >= 0xE0 && lo <= 0xF5) {
    int o = op_index(lo, bank);
    if (o >= 0) { ops_[o].waveform = val & (opl3_ ? 7 : 3); }
    return;
  }

  // --- Channel registers ---
  if (lo >= 0xA0 && lo <= 0xA8) {
    int c = chan_index(lo, bank);
    if (c >= 0) chans_[c].fnum = (chans_[c].fnum & 0x300) | val;
    return;
  }
  if (lo >= 0xB0 && lo <= 0xB8) {
    int c = chan_index(lo, bank);
    if (c < 0) return;
    chans_[c].fnum  = (chans_[c].fnum & 0xFF) | ((val & 0x03) << 8);
    chans_[c].block = (val >> 2) & 0x07;
    int newkey = (val >> 5) & 1;
    if (newkey && !chans_[c].keyon) key_on(c);
    else if (!newkey && chans_[c].keyon) key_off(c);
    else chans_[c].keyon = newkey;
    return;
  }
  if (lo >= 0xC0 && lo <= 0xC8) {
    int c = chan_index(lo, bank);
    if (c < 0) return;
    chans_[c].fb   = (val >> 1) & 0x07;
    chans_[c].algo = val & 1;
    if (opl3_) {
      chans_[c].left  = (val >> 4) & 1;
      chans_[c].right = (val >> 5) & 1;
    } else {
      chans_[c].left = chans_[c].right = 1;
    }
    return;
  }
}

//=============================================================================
// OPL3 4-operator pairing. When reg 0x104 bit n is set (and NEW is on), the
// channel pair (n, n+3) within a bank form a 4-op voice.
//=============================================================================
void OPL::update_fourop() {
  for (int c = 0; c < NUM_CHANS; c++) {
    chans_[c].fourop_prim = chans_[c].fourop_sec = 0;
  }
  if (!opl3_ || !opl3_enabled_) return;
  // 4-op pairs: bank0 {0,3},{1,4},{2,5}; bank1 {9,12},{10,13},{11,14}.
  static const int prim[6] = {0, 1, 2, 9, 10, 11};
  for (int i = 0; i < 6; i++) {
    if (fourop_mask_ & (1 << i)) {
      int p = prim[i], s = p + 3;
      chans_[p].fourop_prim = 1;
      chans_[s].fourop_sec  = 1;
    }
  }
}

//=============================================================================
// Status register read.
//=============================================================================
uint8_t OPL::read_status() const {
  // Real chips return a fixed pattern in the low bits for ID; AdLib detection
  // only cares about the timer flags (bits 7/6/5). Low bits read as 0.
  return status_;
}

//=============================================================================
// Port interface.
//=============================================================================
void OPL::write_port(uint16_t rel, uint8_t val) {
  switch (rel) {
    case 0:  reg_index_[0] = val; reg_bank_sel_ = 0; break; // bank-0 index
    case 1:  write_reg(reg_index_[0], val); break;          // bank-0 data
    case 2:  reg_index_[1] = val; reg_bank_sel_ = 1; break; // OPL3 bank-1 index
    case 3:  write_reg(0x100 | reg_index_[1], val); break;  // OPL3 bank-1 data
    default: break;
  }
}

uint8_t OPL::read_port(uint16_t rel) {
  switch (rel) {
    case 0:  return read_status();
    case 2:  return read_status();
    default: return 0xFF;   // data ports / open bus
  }
}

//=============================================================================
// Timer advance. Timer 1 ticks every 80us, timer 2 every 320us. On overflow
// the preset is reloaded and (if unmasked) the status flag is set.
//=============================================================================
void OPL::advance_timers(double micros) {
  if (timer_ctrl_ & 0x01) {                 // timer 1 running
    timer1_acc_ += micros;
    double period = 80.0 * (256 - timer1_data_);
    while (timer1_acc_ >= period) {
      timer1_acc_ -= period;
      if (!(timer_ctrl_ & 0x40)) {          // not masked
        status_ |= 0x40 | 0x80;             // T1 flag + IRQ flag
      }
    }
  }
  if (timer_ctrl_ & 0x02) {                 // timer 2 running
    timer2_acc_ += micros;
    double period = 320.0 * (256 - timer2_data_);
    while (timer2_acc_ >= period) {
      timer2_acc_ -= period;
      if (!(timer_ctrl_ & 0x20)) {
        status_ |= 0x20 | 0x80;
      }
    }
  }
}

//=============================================================================
// render(): produce one 2-op FM voice per active channel and sum into out.
//=============================================================================
void OPL::render(int32_t *out, int frames, int rate) {
  if (rate <= 0 || frames <= 0) return;

  // Advance timers for the duration of this block (status polled between blocks
  // and within — but block granularity is fine for detection).
  double block_us = (double)frames * 1e6 / (double)rate;
  advance_timers(block_us);

  const double dt = 1.0 / (double)rate;
  // OPL master clock: 14.318 MHz / 288 = ~49716 Hz internal sample rate, but we
  // synthesize directly at the host rate. Fnum->Hz: f = Fnum * 2^(block-20) * fs
  // where fs = 49716. Equivalent: f = Fnum * 49716 / 2^(20-block).
  const double FS = 49716.0;

  for (int c = 0; c < num_chans_; c++) {
    Channel &ch = chans_[c];
    // Secondary half of a 4-op voice is driven by its primary; skip standalone.
    if (ch.fourop_sec) continue;

    // Any operator still producing? (OFF env => silent)
    bool active = false;
    for (int s = 0; s < 2; s++)
      if (ops_[c * 2 + s].env_state != ENV_OFF) active = true;
    if (ch.fourop_prim) {
      for (int s = 0; s < 2; s++)
        if (ops_[(c + 3) * 2 + s].env_state != ENV_OFF) active = true;
    }
    if (!active) continue;

    Operator &mod = ops_[c * 2 + 0];
    Operator &car = ops_[c * 2 + 1];

    // Base channel frequency in Hz.
    double base_hz = (double)ch.fnum * FS /
                     (double)(1u << (20 - ch.block));
    // Per-sample phase increment for the channel's fundamental (carrier with
    // mult=1); each operator multiplies by its own mult.
    double inc_base = base_hz * dt;   // cycles per sample

    int Lon = ch.left, Ron = ch.right;

    for (int i = 0; i < frames; i++) {
      // --- Modulator ---
      double mod_atten = mod.total_level / 0.75 + mod.env_level; // ~0.75dB/step
      double mod_out = 0.0;
      if (mod.env_state != ENV_OFF) {
        double fb = 0.0;
        if (ch.fb) {
          double avg = (ch.fb_out1 + ch.fb_out2) * 0.5;
          fb = avg * (double)(1 << ch.fb) / 16.0;  // feedback depth
        }
        uint32_t mphase = (uint32_t)((mod.phase >> 0)) & 1023;
        double mw = waveform_sample(mod.waveform,
                      (uint32_t)(mphase + (int)(fb * 256.0)) & 1023);
        mod_out = mw * db_to_lin(mod_atten);
        ch.fb_out2 = ch.fb_out1;
        ch.fb_out1 = mod_out;
      }

      // --- Carrier ---
      double sample = 0.0;
      if (car.env_state != ENV_OFF) {
        double car_atten = car.total_level / 0.75 + car.env_level;
        double phasemod;
        if (ch.algo == 0) {
          // FM: modulator phase-modulates carrier.
          phasemod = mod_out * 512.0;  // modulation index
        } else {
          phasemod = 0.0;              // additive: handled below
        }
        uint32_t cphase = (uint32_t)(car.phase >> 0) & 1023;
        double cw = waveform_sample(car.waveform,
                      (uint32_t)(cphase + (int)phasemod) & 1023);
        sample = cw * db_to_lin(car_atten);
        if (ch.algo == 1) {
          // Additive: carrier + modulator both go to output.
          sample += mod_out;
        }
      }

      // Mix to L/R.
      int32_t s = (int32_t)(sample * 9000.0);
      if (Lon) out[2 * i + 0] += s;
      if (Ron) out[2 * i + 1] += s;

      // --- Advance phases ---
      // Phase accumulator is 10-bit fixed; store as float-ish via integer.
      // We keep a high-resolution phase by scaling increments by 1024.
      mod.phase += (uint32_t)(inc_base * (mod.mult2 * 0.5) * 1024.0);
      car.phase += (uint32_t)(inc_base * (car.mult2 * 0.5) * 1024.0);

      // --- Advance envelopes (per sample) ---
      for (int s2 = 0; s2 < 2; s2++) {
        Operator &op = ops_[c * 2 + s2];
        switch (op.env_state) {
          case ENV_ATTACK: {
            double r = rate_attack_per_sec(op.attack) * dt;
            op.env_level -= r;
            if (op.env_level <= 0.0 || op.attack == 0) {
              op.env_level = 0.0;
              op.env_state = ENV_DECAY;
            }
            break;
          }
          case ENV_DECAY: {
            double r = rate_decay_per_sec(op.decay) * dt;
            op.env_level += r;
            double sl = op.sustain * 3.0; // sustain level in dB (3dB/step)
            if (op.env_level >= sl) {
              op.env_level = sl;
              op.env_state = ENV_SUSTAIN;
            }
            break;
          }
          case ENV_SUSTAIN:
            if (!op.eg_type) {
              // Non-sustaining: continue to decay like release.
              double r = rate_decay_per_sec(op.release) * dt;
              op.env_level += r;
              if (op.env_level >= 96.0) { op.env_level = 96.0; op.env_state = ENV_OFF; }
            }
            break;
          case ENV_RELEASE: {
            double r = rate_decay_per_sec(op.release) * dt;
            op.env_level += r;
            if (op.env_level >= 96.0) {
              op.env_level = 96.0;
              op.env_state = ENV_OFF;
            }
            break;
          }
          default: break;
        }
      }
    }
  }
}
