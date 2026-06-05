#ifndef AUDIO_DEVICE_H
#define AUDIO_DEVICE_H

#include <cstdint>

// Portable audio-source interface for optionally-attached sound hardware
// (AdLib/OPL, Sound Blaster, PC speaker, ...). The emulator core owns the
// device state (registers written on the CPU thread); the host pulls rendered
// PCM at its output rate via dos_machine::audio_render(). A host with no audio
// (e.g. dosiz running a compiler) simply never pulls — the devices are inert.
//
// render() MIXES (adds) into a 32-bit interleaved stereo accumulator so several
// devices can sum into one buffer; the machine clamps to int16 at the end.
class AudioDevice {
public:
  virtual ~AudioDevice() = default;
  virtual void reset() = 0;
  // Add `frames` interleaved L,R samples at `rate` Hz into out[0..2*frames).
  virtual void render(int32_t *out, int frames, int rate) = 0;
};

#endif // AUDIO_DEVICE_H
