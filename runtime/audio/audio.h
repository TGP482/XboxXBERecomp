#pragma once
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;

// the platform output pulls interleaved stereo float frames from render
void StartOutput(std::function<void(float* out, uint32_t frames)> render);
// the output's volume, 0 silent to 1 as mixed; any thread, takes effect within a buffer
void SetVolume(float volume);

}  // namespace audio
