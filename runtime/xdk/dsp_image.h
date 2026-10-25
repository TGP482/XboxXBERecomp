#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The title's DSP effects image (XAudioDownloadEffectsImage) run on the host: the effects the image chains after the
// voice processor, recognised by their code. Called under DirectSound's lock.
namespace dsp {

constexpr uint32_t kBins = 32;  // mix bins, DSMIXBIN 0..31

// table: offset of the image's effect descriptor table; states: each effect's state segment in guest memory, the
// image's state copied in; reverb: the I3DL2 reverb's effect index from DSEFFECTIMAGELOC, -1 for none
void Load(const std::vector<uint8_t>& img, size_t table, const std::vector<uint32_t>& states, int32_t reverb);
bool Loaded();
// DSI3DL2LISTENER as 12 floats for the reverb the image's location names (SetI3DL2Listener); false without one
bool SetListener(const float* p);
// the same for the reverb at an effect index (XAudioSetEffectData); false when that effect is no I3DL2 reverb
bool SetReverb(int32_t effect, const float* p);
// verbose: each reverb's largest output since the last report, or that it has no parameters yet, and each mixer's
// two gains
std::string Report();
// frames of kBins interleaved: the voice processor's mix in, every bin as the effects leave it out (the speakers, and
// the effect sends effect-in buffers read)
void Run(float* bins, uint32_t frames);

}  // namespace dsp
