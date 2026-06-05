#ifndef PC_SPEAKER_H
#define PC_SPEAKER_H

#include "audio_device.h"

// PC speaker: a square wave whose frequency is PIT channel 2's output
// (1193182 / reload), gated by port 0x61 bits 0 (timer-2 gate) and 1 (speaker
// data enable). dos_machine calls set_tone() whenever the reload or 0x61 change.
class PCSpeaker : public AudioDevice {
  int freq_hz = 0;       // 0 => silent
  bool gated = false;    // both 0x61 enable bits set
  double phase = 0.0;    // 0..1 within the square-wave period
public:
  void set_tone(int hz, bool gate) { freq_hz = hz; gated = gate; }
  void reset() override { freq_hz = 0; gated = false; phase = 0.0; }
  void render(int32_t *out, int frames, int rate) override {
    if (!gated || freq_hz <= 0 || rate <= 0) { phase = 0.0; return; }
    const double inc = (double)freq_hz / (double)rate;
    const int amp = 7000;  // modest level so it mixes under digital audio
    for (int i = 0; i < frames; i++) {
      int s = (phase < 0.5) ? amp : -amp;
      out[2 * i + 0] += s;
      out[2 * i + 1] += s;
      phase += inc;
      if (phase >= 1.0) phase -= 1.0;
    }
  }
};

#endif // PC_SPEAKER_H
