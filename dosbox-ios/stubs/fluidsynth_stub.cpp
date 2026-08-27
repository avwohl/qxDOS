/*
 * fluidsynth_stub.cpp - Stub implementation for FluidSynth MIDI device
 *
 * Replaces the real fluidsynth.cpp when FluidSynth library is not available.
 * The constructor throws so the MIDI system falls back to other devices.
 *
 * Kept in step with DOSBox v0.83.0, which inserted a MidiSynth base class
 * (midi/private/midi_synth.h) between MidiDevice and MidiDeviceFluidSynth and
 * hoisted six members into it - MixerCallback, Render, SendMidiMessage,
 * SendSysExMessage, ProcessWorkFromFifo and GetNumPendingAudioFrames.  Defining
 * those on MidiDeviceFluidSynth here is now "out-of-line definition does not
 * match any declaration"; they belong to the base and are compiled from
 * midi/midi_synth.cpp, which the build compiles for mt32 and soundcanvas
 * anyway.  What is left below is exactly what fluidsynth.h still declares.
 */

#include "midi/private/fluidsynth.h"
#include <stdexcept>

MidiDeviceFluidSynth::MidiDeviceFluidSynth()
{
    throw std::runtime_error("FluidSynth not available on this platform");
}

// ~MidiDeviceFluidSynth is defined inline in the header at v0.83.0 - defining
// it here as well would be a redefinition.

void MidiDeviceFluidSynth::PrintStats() {}
std_fs::path MidiDeviceFluidSynth::GetSoundFontPath() { return {}; }
void MidiDeviceFluidSynth::SetChorus() {}
void MidiDeviceFluidSynth::SetReverb() {}
void MidiDeviceFluidSynth::SetFilter() {}
void MidiDeviceFluidSynth::SetVolume(const int) {}
void MidiDeviceFluidSynth::IdentifySoundFont() {}
void MidiDeviceFluidSynth::SetChorusParams(const ChorusParameters&) {}
void MidiDeviceFluidSynth::SetReverbParams(const ReverbParameters&) {}
void MidiDeviceFluidSynth::ApplyChannelMessage(const std::vector<uint8_t>&) {}
void MidiDeviceFluidSynth::ApplySysExMessage(const std::vector<uint8_t>&) {}

// The three MidiSynth pure virtuals this class still overrides.  CloseSynth is
// defined inline in the header; these two are not.
void MidiDeviceFluidSynth::ProcessWorkItem(const MidiWork&) {}
void MidiDeviceFluidSynth::RenderAudioFramesToFifo(const int) {}

// Second parameter changed from Program* to MoreOutputStrings& upstream, and
// midi.cpp calls the new form.
void FSYNTH_ListDevices(MidiDeviceFluidSynth*, MoreOutputStrings&) {}

// FSYNTH_AddConfigSection is called by dosbox.cpp
#include "config/config.h"
void FSYNTH_AddConfigSection([[maybe_unused]] const ConfigPtr& conf) {}
