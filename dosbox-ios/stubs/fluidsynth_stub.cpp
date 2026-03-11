/*
 * fluidsynth_stub.cpp - Stub implementation for FluidSynth MIDI device
 *
 * Replaces the real fluidsynth.cpp when FluidSynth library is not available.
 * The constructor throws so the MIDI system falls back to other devices.
 */

#include "midi/private/fluidsynth.h"
#include <stdexcept>

MidiDeviceFluidSynth::MidiDeviceFluidSynth()
{
    throw std::runtime_error("FluidSynth not available on this platform");
}

MidiDeviceFluidSynth::~MidiDeviceFluidSynth() = default;

void MidiDeviceFluidSynth::PrintStats() {}
void MidiDeviceFluidSynth::SendMidiMessage(const MidiMessage&) {}
void MidiDeviceFluidSynth::SendSysExMessage(uint8_t*, size_t) {}
std_fs::path MidiDeviceFluidSynth::GetSoundFontPath() { return {}; }
void MidiDeviceFluidSynth::SetChorus() {}
void MidiDeviceFluidSynth::SetReverb() {}
void MidiDeviceFluidSynth::SetFilter() {}
void MidiDeviceFluidSynth::SetVolume(const int) {}
void MidiDeviceFluidSynth::SetChorusParams(const ChorusParameters&) {}
void MidiDeviceFluidSynth::SetReverbParams(const ReverbParameters&) {}
void MidiDeviceFluidSynth::ApplyChannelMessage(const std::vector<uint8_t>&) {}
void MidiDeviceFluidSynth::ApplySysExMessage(const std::vector<uint8_t>&) {}
void MidiDeviceFluidSynth::MixerCallback(const int) {}
void MidiDeviceFluidSynth::ProcessWorkFromFifo() {}
int MidiDeviceFluidSynth::GetNumPendingAudioFrames() { return 0; }
void MidiDeviceFluidSynth::RenderAudioFramesToFifo(const int) {}
void MidiDeviceFluidSynth::Render() {}

void FSYNTH_ListDevices(MidiDeviceFluidSynth*, Program*) {}

// FSYNTH_AddConfigSection is called by dosbox.cpp
#include "config/config.h"
void FSYNTH_AddConfigSection([[maybe_unused]] const ConfigPtr& conf) {}
