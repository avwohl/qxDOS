#ifndef OPL_H
#define OPL_H

#include <cstdint>
#include "audio_device.h"

// AdLib-compatible OPL2 (YM3812) / OPL3 (YMF262) FM synthesizer.
//
// Self-contained module: depends only on audio_device.h and <cstdint>.
// State lives entirely in the class — no globals, no I/O.
//
// Port model (AdLib base 0x388, 2 ports):
//   write_port(0, v) -> set the register index (low byte).
//   write_port(1, v) -> write value v to the currently-indexed register.
//   read_port(0)     -> status byte: bit7 = either timer expired (IRQ),
//                       bit6 = timer1 expired, bit5 = timer2 expired.
//   read_port(1)     -> 0xFF (open bus).
//
// OPL3 adds a second register bank at ports 2/3 (base+2, base+3); the main
// agent maps rel 2/3 there. Internally write_reg() takes a 9-bit register:
//   0x000-0x0FF = bank 0, 0x100-0x1FF = bank 1 (OPL3 only).
//
// Timers (regs 0x02/0x03/0x04) advance inside render() so the AdLib detection
// sequence (which polls the status register after starting timer 1) works.
class OPL : public AudioDevice {
public:
  explicit OPL(bool opl3 = false);
  ~OPL() override;

  void reset() override;
  void render(int32_t *out, int frames, int rate) override;

  // Direct register access (used by both ports and unit tests).
  void    write_reg(int reg, uint8_t val);   // reg 0x000..0x1FF
  uint8_t read_status() const;               // status byte (timer flags)

  // Port interface (rel = absolute_port - base).
  void    write_port(uint16_t rel, uint8_t val);
  uint8_t read_port(uint16_t rel);

  bool is_opl3() const { return opl3_; }

private:
  static constexpr int NUM_CHANS = 18;   // OPL3 max; OPL2 uses first 9
  static constexpr int NUM_OPS   = 36;   // 2 ops per channel

  enum EnvState { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

  struct Operator {
    // Register-derived parameters.
    uint8_t am_vib_eg_ksr_mult; // reg 0x20
    uint8_t ksl_tl;             // reg 0x40
    uint8_t ar_dr;              // reg 0x60
    uint8_t sl_rr;              // reg 0x80
    uint8_t waveform;           // reg 0xE0 (0..7)

    // Decoded.
    int     mult2;              // multiplier * 2 (so we keep .5 steps)
    int     attack, decay, sustain, release; // rates 0..15 / sustain level
    int     total_level;        // 0..63 (attenuation, 0=loudest)
    int     ksl;                // key-scale-level select 0..3
    int     ksr;                // key-scale-rate flag
    int     am, vib, eg_type;   // tremolo/vibrato/sustaining flags

    // Live state.
    int      env_state;
    double   env_level;         // attenuation in dB-ish (0 loud .. 96 silent)
    uint32_t phase;             // 22.10 phase accumulator (top 10 bits index)
  };

  struct Channel {
    int     fnum;               // 10-bit frequency number
    int     block;              // 0..7 octave
    int     keyon;
    int     fb;                 // feedback 0..7
    int     algo;               // 0 = FM (op0->op1), 1 = additive
    int     left, right;        // OPL3 panning (OPL2: both on)
    // 4-op (OPL3): if this channel is the primary of a 4-op pair.
    int     fourop_prim;        // 1 if primary of a 4-op voice
    int     fourop_sec;         // 1 if secondary (driven by primary)
    int     fourop_algo;        // combined algorithm 0..3
    double  fb_out1, fb_out2;   // feedback history (last two op0 outputs)
  };

  bool    opl3_ = false;
  bool    opl3_enabled_ = false; // reg 0x105 bit0 (NEW)
  int     num_chans_ = 9;

  Operator ops_[NUM_OPS];
  Channel  chans_[NUM_CHANS];

  // Timers / status.
  uint8_t reg_index_[2] = {0, 0}; // current index per bank (for ports)
  int     reg_bank_sel_ = 0;       // which bank the index port last targeted
  uint8_t timer1_data_ = 0;        // reg 0x02 preset
  uint8_t timer2_data_ = 0;        // reg 0x03 preset
  uint8_t timer_ctrl_  = 0;        // reg 0x04
  double  timer1_acc_  = 0.0;      // microsecond accumulators
  double  timer2_acc_  = 0.0;
  uint8_t status_      = 0;        // bit7 IRQ, bit6 T1, bit5 T2

  // Rhythm mode (reg 0xBD).
  uint8_t rhythm_ = 0;

  // OPL3 4-operator connection mask (reg 0x104, bits 0..5).
  uint8_t fourop_mask_ = 0;

  // Tremolo/vibrato LFO phase.
  uint32_t lfo_am_phase_ = 0;
  uint32_t lfo_vib_phase_ = 0;

  void    decode_op(int opi);
  void    key_on(int ch);
  void    key_off(int ch);
  int     op_index(int reg_lo, int bank) const; // 0x20/0x40.. -> operator idx
  int     chan_index(int reg_lo, int bank) const;
  void    advance_timers(double micros);
  void    update_fourop();
};

#endif // OPL_H
