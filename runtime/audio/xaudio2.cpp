// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {
std::atomic<float> g_volume{1.0f};
}

void SetVolume(float volume) { g_volume = volume; }

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(hr)) hr = xa->CreateMasteringVoice(&master);
        if (SUCCEEDED(hr)) hr = xa->CreateSourceVoice(&voice, &wf);
        if (FAILED(hr)) {
            LOGE("audio: XAudio2 unavailable (%08X), no sound", unsigned(hr));
            return;
        }
        // XBE_MUTE=1 silences only the output: the mixer and the guest's sound state run exactly as audible, so a
        // capture or benchmark run cannot be heard and nothing in the game's audio timing changes
        const char* m = std::getenv("XBE_MUTE");
        const bool mute = m && m[0] && std::strcmp(m, "0") != 0;
        if (mute) {
            master->SetVolume(0.0f);
            LOGR("audio: XBE_MUTE, output silenced");
        }
        voice->Start();
        constexpr uint32_t kFrames = kRate / 100;
        constexpr uint32_t kMinQueued = 3;
        constexpr uint32_t kMaxQueued = 10;
        // one slot more than can be queued, so the one rendered is never one XAudio2 still reads
        constexpr uint32_t kSlots = kMaxQueued + 1;
        std::vector<float> bufs(kSlots * kFrames * 2);
        uint32_t depth = kMinQueued;
        float volume = 1.0f;
        for (uint32_t n = 0;;) {
            if (float v = g_volume; v != volume && !mute) {
                volume = v;
                master->SetVolume(v);
            }
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            // a starved voice crackles: queue one buffer more each time, up to kMaxQueued
            if (n && !st.BuffersQueued && depth < kMaxQueued) {
                depth++;
                LOGI("audio: output starved, queueing %u ms", depth * 10);
            }
            if (st.BuffersQueued >= depth) {
                Sleep(2);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * 2];
            render(b, kFrames);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 8;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio
