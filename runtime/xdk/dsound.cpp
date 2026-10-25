// DirectSound (XDK 4831 and 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "core/diag.h"
#include "xbe/xbe.h"
#include "xdk/hle.h"
#include "xdk/dsp_image.h"
#include "xdk/layout.h"

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}
namespace dispatch {
X86Func* Find(uint32_t guest);
}

// configs written before the SDK knew the name
#ifndef SYM_IDirectSound_SynchPlayback
#define SYM_IDirectSound_SynchPlayback FFFFFF00
#define HAS_IDirectSound_SynchPlayback 0
#endif
#ifndef SYM_IDirectSound_DownloadEffectsImage
#define SYM_IDirectSound_DownloadEffectsImage FFFFFF01
#define HAS_IDirectSound_DownloadEffectsImage 0
#endif
#ifndef SYM_IDirectSound_GetEffectData
#define SYM_IDirectSound_GetEffectData FFFFFF02
#define HAS_IDirectSound_GetEffectData 0
#endif
#ifndef SYM_IDirectSound_CommitEffectData
#define SYM_IDirectSound_CommitEffectData FFFFFF03
#define HAS_IDirectSound_CommitEffectData 0
#endif
#ifndef SYM_IDirectSound_GetOutputLevels
#define SYM_IDirectSound_GetOutputLevels FFFFFF04
#define HAS_IDirectSound_GetOutputLevels 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetPlayRegion
#define SYM_IDirectSoundBuffer_SetPlayRegion FFFFFF05
#define HAS_IDirectSoundBuffer_SetPlayRegion 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetAllParameters
#define SYM_IDirectSoundBuffer_SetAllParameters FFFFFF06
#define HAS_IDirectSoundBuffer_SetAllParameters 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetFormat
#define SYM_IDirectSoundBuffer_SetFormat FFFFFF07
#define HAS_IDirectSoundBuffer_SetFormat 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetOutputBuffer
#define SYM_IDirectSoundBuffer_SetOutputBuffer FFFFFF08
#define HAS_IDirectSoundBuffer_SetOutputBuffer 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetRolloffCurve
#define SYM_IDirectSoundBuffer_SetRolloffCurve FFFFFF09
#define HAS_IDirectSoundBuffer_SetRolloffCurve 0
#endif
#ifndef SYM_IDirectSoundBuffer_StopEx
#define SYM_IDirectSoundBuffer_StopEx FFFFFF0A
#define HAS_IDirectSoundBuffer_StopEx 0
#endif
#ifndef SYM_DirectSoundUseLightHRTF
#define SYM_DirectSoundUseLightHRTF FFFFFF0B
#define HAS_DirectSoundUseLightHRTF 0
#endif
#ifndef SYM_IDirectSoundStream_SetEG
#define SYM_IDirectSoundStream_SetEG FFFFFF0C
#define HAS_IDirectSoundStream_SetEG 0
#endif
#ifndef SYM_IDirectSoundBuffer_Pause
#define SYM_IDirectSoundBuffer_Pause FFFFFF0D
#define HAS_IDirectSoundBuffer_Pause 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetEG
#define SYM_IDirectSoundBuffer_SetEG FFFFFF0E
#define HAS_IDirectSoundBuffer_SetEG 0
#endif
#ifndef SYM_IDirectSoundBuffer_PlayEx
#define SYM_IDirectSoundBuffer_PlayEx FFFFFF0F
#define HAS_IDirectSoundBuffer_PlayEx 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetPitch
#define SYM_IDirectSoundBuffer_SetPitch FFFFFF10
#define HAS_IDirectSoundBuffer_SetPitch 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetLFO
#define SYM_IDirectSoundBuffer_SetLFO FFFFFF11
#define HAS_IDirectSoundBuffer_SetLFO 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetConeAngles
#define SYM_IDirectSoundBuffer_SetConeAngles FFFFFF12
#define HAS_IDirectSoundBuffer_SetConeAngles 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetConeOrientation
#define SYM_IDirectSoundBuffer_SetConeOrientation FFFFFF13
#define HAS_IDirectSoundBuffer_SetConeOrientation 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetConeOutsideVolume
#define SYM_IDirectSoundBuffer_SetConeOutsideVolume FFFFFF14
#define HAS_IDirectSoundBuffer_SetConeOutsideVolume 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetMode
#define SYM_IDirectSoundBuffer_SetMode FFFFFF15
#define HAS_IDirectSoundBuffer_SetMode 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetDopplerFactor
#define SYM_IDirectSoundBuffer_SetDopplerFactor FFFFFF16
#define HAS_IDirectSoundBuffer_SetDopplerFactor 0
#endif
#ifndef SYM_IDirectSound_CreateSoundStream
#define SYM_IDirectSound_CreateSoundStream FFFFFF17
#define HAS_IDirectSound_CreateSoundStream 0
#endif
#ifndef SYM_IDirectSoundBuffer_GetVoiceProperties
#define SYM_IDirectSoundBuffer_GetVoiceProperties FFFFFF18
#define HAS_IDirectSoundBuffer_GetVoiceProperties 0
#endif
#ifndef SYM_CDirectSoundStream_GetVoiceProperties
#define SYM_CDirectSoundStream_GetVoiceProperties FFFFFF19
#define HAS_CDirectSoundStream_GetVoiceProperties 0
#endif
#ifndef SYM_CDirectSoundStream_SetLFO
#define SYM_CDirectSoundStream_SetLFO FFFFFF1A
#define HAS_CDirectSoundStream_SetLFO 0
#endif
#ifndef SYM_CDirectSoundStream_SetOutputBuffer
#define SYM_CDirectSoundStream_SetOutputBuffer FFFFFF1B
#define HAS_CDirectSoundStream_SetOutputBuffer 0
#endif
#ifndef SYM_CDirectSoundStream_SetPitch
#define SYM_CDirectSoundStream_SetPitch FFFFFF1C
#define HAS_CDirectSoundStream_SetPitch 0
#endif
#ifndef SYM_IDirectSoundBuffer_Set3DVoiceData
#define SYM_IDirectSoundBuffer_Set3DVoiceData FFFFFF1D
#define HAS_IDirectSoundBuffer_Set3DVoiceData 0
#endif
#ifndef SYM_IDirectSoundBuffer_Use3DVoiceData
#define SYM_IDirectSoundBuffer_Use3DVoiceData FFFFFF1E
#define HAS_IDirectSoundBuffer_Use3DVoiceData 0
#endif
#ifndef SYM_IDirectSoundStream_Set3DVoiceData
#define SYM_IDirectSoundStream_Set3DVoiceData FFFFFF1F
#define HAS_IDirectSoundStream_Set3DVoiceData 0
#endif
#ifndef SYM_IDirectSoundStream_Use3DVoiceData
#define SYM_IDirectSoundStream_Use3DVoiceData FFFFFF20
#define HAS_IDirectSoundStream_Use3DVoiceData 0
#endif
#ifndef SYM_XAudioSetEffectData
#define SYM_XAudioSetEffectData FFFFFF21
#define HAS_XAudioSetEffectData 0
#endif

namespace {

constexpr uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
constexpr uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
constexpr uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;  // E_PENDING, E_ABORT

struct Packet {
    uint32_t data, size, completed_ptr, status_ptr, context;
    uint32_t used = 0;
};

// DSMIXBIN numbers the host mixes by name
enum MixBin : uint32_t {
    kFrontLeft,
    kFrontRight,
    kCentre,
    kSub,
    kBackLeft,
    kBackRight,
    kCrosstalkFrontLeft,
    kCrosstalkFrontRight,
    kCrosstalkBackLeft,
    kCrosstalkBackRight,
    kFirstSend,  // effect sends from here up
};
constexpr uint32_t kI3dl2Send = kFirstSend;  // DSMIXBIN_I3DL2, the reverb's
constexpr uint32_t kBins = dsp::kBins;
// A mix-in buffer (DSBCAPS_MIXIN) is a submix: the voice processor runs the voices playing into it first, sending to
// the last bin, then plays that bin through the submix's own bins. Effect-in buffers (DSBCAPS_FXIN, and 0x100000)
// play an effect send named in their descriptor instead, as the effects image left it.
constexpr uint32_t kMixIn = 0x2000, kFxIn = 0x80000, kInputFromDesc = 0x180000;
constexpr uint32_t kHrtfVoice = 0x200000;  // an HRTF voice without DS3D: XACT's 3D submixes
constexpr uint8_t kMultipassBin = kBins - 1;
constexpr uint8_t kNoInput = 0xFF;
constexpr uint32_t kInvalidCall = 0x88780032u;  // DSERR_INVALIDCALL
constexpr uint32_t kMaxChannels = 8;
// rows of a voice's mix: its channels, then a mono 3D voice's two ears
constexpr uint32_t kLeftEar = kMaxChannels;
constexpr uint32_t kRightEar = kMaxChannels + 1;
constexpr uint32_t kRows = kMaxChannels + 2;
constexpr uint32_t kHrirTaps = 31;   // an HRTF filter's taps an ear
constexpr uint32_t kEarLength = 128;  // past the longest interaural delay, 42 samples, and the taps

// gain of each row on each mix bin
struct Gains {
    float bin[kRows][kBins];
};

// The library's 3D data block of a voice made with DSBCAPS_CTRL3D or with 0x400000, the flag XACT makes its voices
// with: the 3D calculator's results, which DirectSound writes for its own 3D voices and XACT writes itself through
// Set3DVoiceData. Volumes in mB, added to the voice's own; zero changes nothing. The I3DL2 low-pass poles replace the
// voice filter's FCB, and the HRTF angles give a mono voice its interaural delay alone.
struct Data3d {
    int32_t distance = 0, cone = 0;
    int32_t front = 0, rear = 0;        // speaker pan: the front 3D bins, the back crosstalk pair
    int32_t crosstalk = 0, centre = 0;  // the front's share between the crosstalk front pair and the centre
    int32_t direct = 0, room = 0;       // I3DL2: the 3D bins, the reverb send
    int32_t doppler = 0;                // pitch, 4096 an octave
    float azimuth = 0, elevation = 0;   // degrees, the HRTF's
    uint32_t lowpass[2] = {};           // I3DL2 low-pass coefficients of the direct and room paths, 0xFFFF the most
};

// DSI3DL2BUFFER: a DS3D voice's I3DL2 source settings, mB unless named
struct I3dl2Source {
    int32_t direct = 0, direct_hf = 0, room = 0, room_hf = 0;
    float room_rolloff = 0;
    int32_t obstruction = 0;  // HF level
    float obstruction_lf = 0;  // the share of the HF level that also falls on low frequencies
    int32_t occlusion = 0;
    float occlusion_lf = 0;
};

// DSFILTERDESC as the voice processor takes it: the mode, the parametric EQ's Q and four 16 bit coefficient words, the
// first pair the hardware's FCA, the second its FCB
struct Filter {
    uint32_t mode = 0;  // DSFILTER_MODE_BYPASS, DLS2, PARAMEQ, MULTI
    uint32_t q = 0;     // 0 to 7
    uint16_t c[4] = {};
};

// a channel's filter memory: the DLS2 low-pass and the parametric EQ are state variable filters, the I3DL2 low-passes
// of the direct and room paths one-pole filters
struct FilterState {
    float low = 0, band = 0;
    float eq_low = 0, eq_band = 0;
    float direct = 0, room = 0;
};

struct Voice {
    uint32_t obj = 0;
    bool stream = false;
    int refs = 1;
    // format
    uint16_t tag = 1, channels = 1, bits = 16, align = 2;
    uint32_t rate = 48000;
    // buffer
    // play region in bytes from data, loop region in bytes from the play region
    uint32_t data = 0, bytes = 0, loop_start = 0, loop_len = 0, play_start = 0, play_len = 0;
    uint32_t owned = 0;  // memory the library made for the buffer
    bool playing = false, looping = false, paused = false;
    bool sync = false;  // held until IDirectSound_SynchPlayback
    bool active = false, ending = false;  // stream: took a packet since it went quiet, quiet again once they run out
    double pos = 0;  // frames
    uint32_t freq = 0;
    // stream
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> packets;
    // levels
    float volume = 1, headroom = 1;
    uint8_t nbins = 0, bin[8] = {};
    int32_t bin_mb[32] = {};  // by mix bin
    std::vector<float> curve;  // rolloff
    bool is3d = false;
    uint32_t mode = 0;  // DS3DMODE: 1 head relative, 2 disabled
    float p[3] = {}, min_dist = 1, max_dist = 1e9f;
    // sound cone: full angles in degrees, the volume outside it in mB and the direction it points
    uint32_t cone_inside = 360, cone_outside = 360;
    int32_t cone_volume = 0;
    float cone_dir[3] = {0, 0, 1};
    I3dl2Source i3dl2;
    uint32_t flags = 0;  // creation flags, DSBUFFERDESC's or DSSTREAMDESC's
    // the 3D data block, which the library makes switched on; the voice's levels and pitch use it while it is on
    bool has_data3d = false, data3d_on = false;
    Data3d data3d;
    // submixes: the buffer this voice plays into (SetOutputBuffer), the bin a mix-in or effect-in buffer reads, and
    // a mix-in buffer's input for the block being mixed, with its peak since the last verbose report
    uint32_t output = 0;
    uint8_t input_bin = kNoInput;
    std::vector<float> input;
    float input_peak = 0;
    std::vector<float> effect_out;  // an effect-in buffer's send as the last block left it
    Filter filter;
    FilterState filtered[kMaxChannels];
    float source_peak = 0;  // the loudest sample read from its data since the last verbose report
    Gains gain = {};  // the last block's gains, the next block ramps from them
    bool has_gain = false;
    float out_peak = 0;  // its loudest sample on a bin since the last verbose report
    // a mono 3D voice's ears: the interaural delay in samples, each ear's HRTF filter, and its recent samples, written
    // twice so the filters read them in one run
    float itd = 0;
    float hrir[2][kHrirTaps] = {};
    float ear[kEarLength * 2] = {};
    uint32_t ear_pos = 0;
    // ADPCM block cache: the block read last and the one read before it, a frame between two blocks reads both
    int64_t block[2] = {-1, -1};
    uint8_t last = 0;  // decoded[last] holds block[last]
    int16_t decoded[2][65 * 8];
};

struct Done {
    uint32_t voice, callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
std::map<uint32_t, Voice> g_voices;
float g_listener[3] = {}, g_front[3] = {0, 0, 1}, g_top[3] = {0, 1, 0};
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
std::deque<Done> g_done;
std::condition_variable_any g_done_cv;
thread_local std::vector<Done> t_flushed;  // run by the STREAM call that flushed them, once g_lock is let go
std::atomic<int64_t> g_pumped{0};          // the last DirectSoundDoWork, steady ms
bool g_started = false;
uint32_t g_ds = 0;
// IDirectSound_SetMixBinHeadroom: the voice processor halves a bin per step, 1 for every bin but the last by default
uint8_t g_bin_headroom[kBins] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
struct Segment {
    uint32_t at;
    uint32_t bytes;
};
std::vector<Segment> g_effect_states;  // each effect's state segment of the downloaded image
// levels since the last verbose report: the output before it is clamped, each bin as the voice processor leaves it
// (the effects image reads it limited to full scale) and each speaker bin after the effects
struct Meters {
    float out_peak = 0;
    double out_squares = 0;
    uint64_t out_samples = 0;
    uint64_t clipped = 0;  // output samples past full scale
    uint64_t bins_clipped = 0;  // bin samples past full scale, which the effects image reads limited
    float bin[kBins] = {};
    float speaker[6] = {};
};
Meters g_meters;

void ForgetBlocks(Voice& v) {
    v.block[0] = -1;
    v.block[1] = -1;
}

Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

// a submix: a mix-in buffer that reads the multipass bin
bool IsSubmix(const Voice& v) { return !v.stream && v.input_bin == kMultipassBin; }

// a buffer that plays an input instead of data of its own: a mix-in buffer, or an effect-in buffer, which plays an
// effect send as the effects image left it, a block late (the console's output ring per send, 0x3FA85C)
bool TakesInput(const Voice& v) { return !v.stream && v.input_bin != kNoInput; }

// the submix the voice plays into, if it plays into one
Voice* SubmixOf(const Voice& v) {
    Voice* to = v.output ? Find(v.output) : nullptr;
    return to && IsSubmix(*to) ? to : nullptr;
}

// the mix-in or effect-in buffer the voice plays into, if any
Voice* InputBufferOf(const Voice& v) {
    Voice* to = v.output ? Find(v.output) : nullptr;
    return to && TakesInput(*to) ? to : nullptr;
}

constexpr int kMaxSubmixDepth = 8;  // submixes playing into submixes, a few deep at most

// a voice the voice processor runs: a buffer that plays, a stream that has packets
bool Running(const Voice& v) { return v.stream ? v.active : v.playing; }

// A voice starting a new sound: its levels do not ramp from the last sound's, and its filters start out empty.
void Restart(Voice& v) {
    v.has_gain = false;
    std::fill(std::begin(v.filtered), std::end(v.filtered), FilterState{});
    std::fill(std::begin(v.ear), std::end(v.ear), 0.0f);
}

// The library plays the mix-in or effect-in buffer a voice plays into when the voice starts, and the one that plays
// into, as a looping buffer with no data of its own.
void StartSubmixes(const Voice& v) {
    Voice* to = InputBufferOf(v);
    for (int hops = 0; to && !to->playing && hops < kMaxSubmixDepth; hops++) {
        to->playing = to->looping = true;
        Restart(*to);
        to = InputBufferOf(*to);
    }
}

// It stops a submix when the last voice playing into it stops, and so on down the chain.
void StopSubmix(Voice* to) {
    for (int hops = 0; to && to->playing && hops < kMaxSubmixDepth; to = InputBufferOf(*to), hops++) {
        for (auto& [obj, source] : g_voices) {
            if (source.output == to->obj && Running(source)) return;
        }
        to->playing = false;
    }
}

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }

float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * 64;  // WAVE_FORMAT_XBOX_ADPCM
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / 64 * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, the voice plays 64 of its 65 frames
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8), idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
    }
}

// the decoded ADPCM block at guest address at, decoded once while it stays in the cache
const int16_t* DecodedBlock(Voice& v, int64_t at) {
    for (uint8_t k = 0; k < 2; k++) {
        if (v.block[k] != at) continue;
        v.last = k;
        return v.decoded[k];
    }
    uint8_t k = v.last ^ 1;  // the slot not read last
    DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.decoded[k]);
    v.block[k] = at;
    v.last = k;
    return v.decoded[k];
}

// sample of channel c at frame f of memory starting at data
float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == 0x69) {
        int64_t at = int64_t(data) + int64_t(f / 64) * 36 * v.channels;
        return DecodedBlock(v, at)[(f % 64) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.obj, v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        if (ObjectRef e = FromHandle(pk.context)) SetEvent(*e);
    }
}

// the console's stereo downmix of the speaker bins: centre and back pair at -3 dB, the sub dropped
constexpr float kFold = 0.707f;

// A voice with no speaker bin and no effects image plays its loudest bin on the front pair; with speaker bins the
// crosstalk bins join their speakers and effect sends are dropped. Without the title's image nothing reads them.
// A voice playing into a submix keeps its send to it, and leaves the speakers to the submix.
void RouteWithoutImage(const Voice& v, Gains& g) {
    bool submix = SubmixOf(v) != nullptr;
    bool speakers = false;
    for (uint32_t s = 0; s < v.nbins; s++) speakers |= v.bin[s] < kFirstSend && v.bin[s] != kSub;
    if (!v.nbins) speakers = true;
    for (auto& row : g.bin) {
        float send = submix ? row[kMultipassBin] : 0.0f;
        if (!speakers) {
            float loudest = submix ? 0.0f : *std::max_element(row, row + kBins);
            std::fill(row, row + kBins, 0.0f);
            row[kFrontLeft] = loudest;
            row[kFrontRight] = loudest;
        } else {
            row[kFrontLeft] += row[kCrosstalkFrontLeft];
            row[kFrontRight] += row[kCrosstalkFrontRight];
            row[kBackLeft] += row[kCrosstalkBackLeft];
            row[kBackRight] += row[kCrosstalkBackRight];
            std::fill(row + kCrosstalkFrontLeft, row + kBins, 0.0f);
        }
        if (submix) row[kMultipassBin] = send;
    }
}

// The library gives every hardware voice (two channels) a run of n / voices slots. A stereo voice plays its left
// channel on even slots and its right on odd ones.
uint32_t ChannelOfSlot(const Voice& v, uint32_t voice, uint32_t slot) {
    uint32_t voice_channels = std::min<uint32_t>(v.channels - voice * 2, 2);
    return voice * 2 + slot % voice_channels;
}

// each channel's level on each bin; the voice processor divides a bin by 2^headroom, which the image may undo
Gains SlotGains(const Voice& v) {
    static const uint8_t kSpeakerBins[6] = {0, 1, 2, 3, 4, 5};  // no mix bins given: the speakers in order
    uint32_t channels = std::min<uint32_t>(v.channels, kMaxChannels);
    uint32_t voices = (channels - 1) / 2 + 1;
    const uint8_t* bin = v.nbins ? v.bin : kSpeakerBins;
    uint32_t n = v.nbins ? v.nbins : std::min<uint32_t>(voices * 2, 6);
    uint32_t per = std::max<uint32_t>(n / voices, 1);
    bool image = dsp::Loaded();
    Gains g = {};
    for (uint32_t k = 0; k < voices; k++) {
        for (uint32_t s = k * per; s < (k + 1) * per && s < n; s++) {
            uint32_t b = bin[s];
            float level = Db(v.bin_mb[b]) * v.volume * v.headroom;
            if (image) level = std::ldexp(level, -g_bin_headroom[b]);
            g.bin[ChannelOfSlot(v, k, s)][b] += level;
        }
    }
    return g;
}

// A 3D voice as the library's 3D pass sees it. Its levels have no left/right term: a mono voice's two ears differ
// by the interaural delay, and on a stereo console the back crosstalk pair is silent.
struct Place {
    bool on = false;
    bool ears = false;   // an HRTF voice: a mono one plays its crosstalk bins with the interaural delay
    int32_t att = 0;     // distance and cone, mB, every bin
    int32_t direct = 0;  // I3DL2 source levels, mB: the 3D bins
    int32_t room = 0;    // the reverb send
    float front = 1;     // crosstalk front pair
    float centre = 1;    // centre bin
    float azimuth = 0;   // degrees, plus to the right
    float elevation = 0;
};

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// the library's octant estimate of the angle of along from across: 45 degrees where they are equal
float OctantAngle(float along, float across) {
    float a = std::fabs(along);
    float s = std::fabs(across);
    if (a > s) return 45 * s / a;
    if (s > 0) return 90 - 45 * a / s;
    return 0;
}

float Azimuth(const float d[3], const float front[3], const float right[3]) {
    float ahead = Dot(d, front);
    float side = Dot(d, right);
    float deg = OctantAngle(ahead, side);
    if (ahead < 0) deg = 180 - deg;
    return side < 0 ? -deg : deg;
}

float Elevation(const float d[3], const float front[3], const float right[3], const float top[3]) {
    float flat = std::hypot(Dot(d, front), Dot(d, right));
    float up = Dot(d, top);
    float deg = OctantAngle(flat, up);
    return up < 0 ? -deg : deg;
}

// interaural delay in samples by azimuth in steps of 3 degrees at elevation 0, the far ear's HRTF delay
float Itd(float azimuth) {
    static const uint8_t kDelay[61] = {0,  2,  3,  4,  4,  5,  6,  9,  10, 11, 12, 12, 13, 14, 17, 18,
                                       19, 20, 21, 21, 22, 23, 24, 25, 26, 28, 29, 29, 31, 30, 30, 31,
                                       30, 28, 27, 28, 27, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15,
                                       14, 13, 13, 12, 11, 8,  7,  6,  6,  3,  2,  2,  0};
    constexpr int kStep = 3;
    if (!(std::fabs(azimuth) <= 180)) return 0;
    int deg = int(std::fabs(azimuth) + kStep / 2.0f) / kStep * kStep;
    float delay = kDelay[deg / kStep];
    return azimuth < 0 ? -delay : delay;
}

// A voice whose fifth bin is the centre shares the front with it below 45 degrees: 0.7071 cos^2(2a) and
// 0.8409 cos(2a) by whole degrees of azimuth and elevation
void ShareCentre(Place& at) {
    constexpr float kRadiansPerDegree = 0.017453293f;
    constexpr int kCentreLimit = 45;
    int az = int(std::fabs(at.azimuth));
    int el = int(std::fabs(at.elevation));
    if (az >= kCentreLimit || el >= kCentreLimit) {
        at.centre = 0;
        return;
    }
    float both = std::cos(2 * az * kRadiansPerDegree) * std::cos(2 * el * kRadiansPerDegree);
    at.centre *= 0.7071f * both;
    at.front *= 1 - 0.5f * both * both;
}

// distance attenuation, 1.0 inside the min distance: min / distance, or the voice's rolloff curve
float Attenuation(const Voice& v, float dist) {
    if (dist <= v.min_dist) return 1;
    if (v.curve.empty()) return v.min_dist / std::min(dist, v.max_dist);
    if (v.max_dist <= v.min_dist) return 1;
    // points spread evenly from min to max distance, 1.0 at min
    float at = (std::min(dist, v.max_dist) - v.min_dist) / (v.max_dist - v.min_dist) * v.curve.size();
    size_t i = std::min(size_t(at), v.curve.size() - 1);
    float from = i ? v.curve[i - 1] : 1.0f;
    return from + (v.curve[i] - from) * (at - i);
}

// a level in mB
int32_t Mb(float level) { return level <= 0 ? -10000 : int32_t(std::lround(2000.0 * std::log10(level))); }

int32_t Within(int64_t mb) { return int32_t(std::clamp<int64_t>(mb, -10000, 0)); }

// The cone's volume (0x3F59F7) at the listener's direction from the source: 0 within the inside angle, the outside
// volume past the outside angle, linear in mB between. The library measures the full angle as four times its octant
// estimate of half the angle between the cone's direction and the listener's (0x3F5805).
int32_t ConeVolume(const Voice& v, const float d[3]) {
    if (v.cone_inside >= 360 && v.cone_outside >= 360) return 0;
    float to_listener[3] = {-d[0], -d[1], -d[2]};
    float length = std::sqrt(Dot(to_listener, to_listener));
    float dir_length = std::sqrt(Dot(v.cone_dir, v.cone_dir));
    if (length <= 0 || dir_length <= 0) return 0;
    float diff = 0, sum = 0;
    for (int i = 0; i < 3; i++) {
        float a = v.cone_dir[i] / dir_length, b = to_listener[i] / length;
        diff += (a - b) * (a - b);
        sum += (a + b) * (a + b);
    }
    float angle = 4 * OctantAngle(std::sqrt(sum), std::sqrt(diff));
    if (angle <= float(v.cone_inside)) return 0;
    if (v.cone_inside < v.cone_outside && float(v.cone_outside) <= angle) return v.cone_volume;
    uint32_t span = std::max(v.cone_outside - std::min(v.cone_inside, v.cone_outside), 1u);
    return int32_t((angle - float(v.cone_inside)) * float(v.cone_volume) / float(span));
}

// The I3DL2 source's direct and room levels (0x3FB3AF): the obstruction and occlusion take their low frequency share
// off the direct path, the occlusion off the room path as well. The room rolloff by distance is left out (Forza sets
// no rolloff factor), and the high frequency remainder is the low-pass the host does not play.
void I3dl2Levels(const Voice& v, Place& at) {
    const I3dl2Source& s = v.i3dl2;
    float occluded = float(s.occlusion) * s.occlusion_lf;
    at.direct = s.direct + int32_t(float(s.obstruction) * s.obstruction_lf + occluded);
    at.room = s.room + int32_t(occluded);
}

Place Locate(const Voice& v) {
    static const float kAhead[3] = {0, 0, 1};
    static const float kRight[3] = {1, 0, 0};
    static const float kUp[3] = {0, 1, 0};
    Place at;
    if (!v.is3d) {
        // The library also sets up the HRTF of a voice made with 0x200000 (XACT's 3D submixes), placed by the angles
        // in its 3D data; the data's own levels place it otherwise.
        if (v.flags & kHrtfVoice && v.has_data3d) {
            at.ears = true;
            at.azimuth = v.data3d.azimuth;
            at.elevation = v.data3d.elevation;
        }
        return at;
    }
    if (v.mode == 2) return at;
    at.on = at.ears = true;
    bool relative = v.mode == 1;  // the position is already in listener space
    float d[3] = {v.p[0], v.p[1], v.p[2]};
    for (int i = 0; i < 3 && !relative; i++) d[i] -= g_listener[i];
    at.att = Within(int64_t(Mb(Attenuation(v, std::sqrt(Dot(d, d))))) + ConeVolume(v, d));
    I3dl2Levels(v, at);
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    const float* front = relative ? kAhead : g_front;
    const float* side = relative ? kRight : right;
    const float* top = relative ? kUp : g_top;
    at.azimuth = Azimuth(d, front, side);
    at.elevation = Elevation(d, front, side, top);
    at.centre = at.front;
    if (v.nbins >= 5 && v.bin[4] == kCentre) ShareCentre(at);
    return at;
}

// A DS3D voice's level on a bin as the library's volume pass (0x3F8814) adds it: distance and cone on every bin, the
// I3DL2 direct level on the 3D bins and the room level on the reverb send, each sum within -10000 and 0 mB
float BinScale(const Place& at, uint32_t b) {
    if (!at.on) return 1;
    int32_t direct = Within(int64_t(at.att) + at.direct);
    if (b == kCentre) return Db(direct) * at.centre;
    if (b == kCrosstalkFrontLeft || b == kCrosstalkFrontRight) return Db(direct) * at.front;
    if (b == kCrosstalkBackLeft || b == kCrosstalkBackRight) return 0;
    if (b == kI3dl2Send) return Db(Within(int64_t(at.att) + at.room));
    return Db(at.att);
}

// the 3D data in use, none while it is switched off
const Data3d* Data3dOf(const Voice& v) { return v.has_data3d && v.data3d_on ? &v.data3d : nullptr; }

// The 3D data's level on a bin as the library adds it to the voice's volume: distance and cone everywhere, then the
// pan, the centre share and the I3DL2 direct level on the 3D bins and the room level on the reverb send. Each sum
// stays within -10000 and 0 mB.
float Data3dScale(const Voice& v, uint32_t b) {
    const Data3d* d = Data3dOf(v);
    if (!d) return 1;
    int64_t mb = Within(int64_t(d->distance) + d->cone);
    if (b == kCentre) {
        mb += int64_t(d->direct) + d->centre + d->front;
    } else if (b == kCrosstalkFrontLeft || b == kCrosstalkFrontRight) {
        mb += int64_t(d->direct) + d->crosstalk + d->front;
    } else if (b == kCrosstalkBackLeft || b == kCrosstalkBackRight) {
        mb += int64_t(d->direct) + d->rear;
    } else if (b == kI3dl2Send) {
        mb += d->room;
    }
    return Db(Within(mb));
}

// the pitch the voice plays at, 4096 an octave from 48 kHz: its frequency's, plus the 3D data's Doppler pitch, which
// the library adds within -32767 and 8191
double Pitch(const Voice& v) {
    double hz = v.freq ? v.freq : v.rate;
    double pitch = 4096.0 * std::log2(std::max(hz, 1.0) / 48000.0);
    const Data3d* d = Data3dOf(v);
    if (!d || !d->doppler) return pitch;
    return std::clamp(pitch + d->doppler, -32767.0, 8191.0);
}

// a mono 3D voice's crosstalk bins play its ears: the left bins the left ear, the right bins the right one
void SplitEars(Gains& g) {
    static const uint32_t kLeft[2] = {kCrosstalkFrontLeft, kCrosstalkBackLeft};
    static const uint32_t kRight[2] = {kCrosstalkFrontRight, kCrosstalkBackRight};
    for (int k = 0; k < 2; k++) {
        std::swap(g.bin[0][kLeft[k]], g.bin[kLeftEar][kLeft[k]]);
        std::swap(g.bin[0][kRight[k]], g.bin[kRightEar][kRight[k]]);
    }
}

Gains Levels(const Voice& v, const Place& at) {
    Gains g = SlotGains(v);
    for (auto& row : g.bin) {
        for (uint32_t b = 0; b < kBins; b++) row[b] *= BinScale(at, b) * Data3dScale(v, b);
    }
    if (at.ears && v.channels == 1) SplitEars(g);
    if (!dsp::Loaded()) RouteWithoutImage(v, g);
    // a sound placed at NaN goes silent alone; its NaN gain would silence the whole mix
    for (auto& row : g.bin) {
        for (float& gain : row) {
            if (!std::isfinite(gain)) gain = 0;
        }
    }
    return g;
}

// The voice's filters for a block, from its filter descriptor (FCA, FCB) and, on a 3D voice, the I3DL2 low-passes.
// The voice processor filters after resampling to 48 kHz. The DLS2 low-pass and the parametric EQ are state variable
// filters: a frequency word is 4096 log2 of the frequency term 2 sin(pi fc / 48000) (Forza's 0x2EF10 writes them), so
// 0 is 8 kHz and 0x8000 about 30 Hz; the DLS2 resonance word is the damping 32768 10^(-Q / 20 dB) (0x2EF70); the EQ
// gain word is G - 1 in signed 2.14 (0x2EFD0). The I3DL2 low-passes are one-pole filters, the pole 1/65536 a step
// (0x3FB217), one on the direct path and one on the room path (the reverb and other effect sends).
struct VoiceFilter {
    bool lowpass = false, eq = false, i3dl2 = false;
    float f = 1, damping = 1, in = 1;          // DLS2, its input scaled by in
    float eq_f = 1, eq_damping = 1, eq_gain = 0;  // parametric EQ: G - 1 at its centre
    float direct = 0, room = 0;                // I3DL2 poles
};

float SvfFrequency(uint16_t word) { return std::clamp(std::exp2(float(int16_t(word)) / 4096.0f), 1.0f / 256, 1.0f); }
// below 1.5 the filter stays stable for every frequency term up to 1
float SvfDamping(uint16_t word) { return std::clamp(float(word) / 32768.0f, 0.01f, 1.4f); }
// The EQ's Q field (0 to 7) is not established; read here as halving the damping every two steps, a one octave band
// at 1, the value Forza sets.
float EqDamping(uint32_t q) { return std::exp2(-float(q & 7) / 2); }

// The I3DL2 low-pass for an HF level in mB at the listener's HF reference (0x3FB217): the pole of a one-pole filter
// that is that much down there, 65536 a unit, 0 when there is nothing to take off
uint16_t I3dl2Pole(int32_t hf, float reference) {
    if (hf >= 0 || hf <= -10000) return hf <= -10000 ? 0xFFFF : 0;
    double g = std::pow(10.0, 2.0 * hf / 2000.0);  // the HF gain squared
    double c = std::cos(6.283185307 * reference / audio::kRate);
    double a = (1 - g * c - std::sqrt(std::max(2 * g * (1 - c) - g * g * (1 - c * c), 0.0))) / (1 - g);
    if (!(a < 1)) return 0xFFFF;
    return uint16_t(std::clamp(a * 65536.0, 0.0, 65535.0));
}

float g_hf_reference = 5000;  // DSI3DL2LISTENER's flHFReference, the library's default until the title sets one

// A DS3D voice's I3DL2 low-passes from its source settings (0x3FB3AF): what the obstruction and occlusion take off
// above their low frequency share, plus the direct and room HF levels
void I3dl2Lowpass(const Voice& v, uint16_t pole[2]) {
    const I3dl2Source& s = v.i3dl2;
    float occluded = float(s.occlusion) * (1 - s.occlusion_lf);
    int32_t direct = Within(s.direct_hf + int64_t(float(s.obstruction) * (1 - s.obstruction_lf) + occluded));
    int32_t room = Within(s.room_hf + int64_t(occluded));
    pole[0] = I3dl2Pole(direct, g_hf_reference);
    pole[1] = I3dl2Pole(room, g_hf_reference);
}

// The modes: on a 3D voice (DS3D or 0x200000, among the HRTF voices) 1 is DLS2 and I3DL2, 2 the EQ and I3DL2, 3 I3DL2
// alone, and the library runs one left in mode 0 in mode 3 (0x3F938D); FCB then holds the I3DL2 poles, which the 3D
// data replaces while it is on, and the EQ, if any, comes from FCA. On any other voice 1 is DLS2, 2 the EQ (FCB and the
// Q field), 3 both. Mode names from xemu's reading of the hardware; the EQ itself is this file's reading of Forza's
// coefficients, a peak or notch of G at its frequency.
VoiceFilter FilterOf(const Voice& v, const Place& at) {
    VoiceFilter vf;
    const Filter& fd = v.filter;
    uint32_t mode = fd.mode & 3;
    if (v.flags & (kHrtfVoice | 0x10)) {
        uint16_t pole[2] = {fd.c[2], fd.c[3]};
        if (const Data3d* d = Data3dOf(v)) {
            pole[0] = uint16_t(d->lowpass[0]);
            pole[1] = uint16_t(d->lowpass[1]);
        }
        if (at.on) I3dl2Lowpass(v, pole);
        if (!mode) mode = 3;
        vf.i3dl2 = pole[0] || pole[1];
        vf.direct = pole[0] / 65536.0f;
        vf.room = pole[1] / 65536.0f;
        vf.lowpass = mode == 1;
        if (mode == 2) {
            vf.eq = true;
            vf.eq_f = SvfFrequency(fd.c[0]);
            vf.eq_gain = float(int16_t(fd.c[1])) / 16384;
        }
    } else {
        vf.lowpass = mode & 1;
        vf.eq = mode & 2;
        vf.eq_f = SvfFrequency(fd.c[2]);
        vf.eq_gain = float(int16_t(fd.c[3])) / 16384;
    }
    vf.f = SvfFrequency(fd.c[0]);
    vf.damping = SvfDamping(fd.c[1]);
    // the input scaled down as the damping falls, so the resonance stays within range: the usual normalisation of
    // this filter, sqrt(r / 2 + 0.01), which xemu uses as well; the hardware's own is not known (0.71 at Forza's r 1)
    vf.in = std::sqrt(vf.damping / 2 + 0.01f);
    vf.eq_damping = EqDamping(fd.q);
    vf.eq = vf.eq && vf.eq_gain != 0;
    return vf;
}

// One step of a Chamberlin state variable filter with frequency term f and damping r; returns the low-pass output and
// leaves the band-pass in band, which peaks at 1/r. The offset keeps silence out of denormals.
float Svf(float& low, float& band, float f, float r, float x) {
    low += f * band;
    float high = x + 1e-20f - low - r * band;
    band += f * high;
    return low;
}

// a channel through the voice's filters: the direct path's sample, and the room path's in room
float Filtered(const VoiceFilter& vf, FilterState& s, float x, float& room) {
    if (vf.lowpass) x = Svf(s.low, s.band, vf.f, vf.damping, x * vf.in);
    if (vf.eq) {
        Svf(s.eq_low, s.eq_band, vf.eq_f, vf.eq_damping, x);
        x += vf.eq_gain * vf.eq_damping * s.eq_band;
    }
    room = x;
    if (!vf.i3dl2) return x;
    s.direct = vf.direct * s.direct + (1 - vf.direct) * x + 1e-20f;
    s.room = vf.room * s.room + (1 - vf.room) * x + 1e-20f;
    room = s.room;
    return s.direct;
}

// a row's level on one bin, ramped across the block: levels follow the game once a frame, stepped a fast turn clicks.
// A lane on an effect send plays the room path.
struct Lane {
    uint32_t row;
    uint32_t bin;
    float gain;
    float step;
    bool room;
};

// the levels the block can hear; one zero at both ends of its ramp adds nothing to the mix
std::vector<Lane> AudibleLanes(const Gains& from, const Gains& to, uint32_t frames) {
    std::vector<Lane> lanes;
    for (uint32_t r = 0; r < kRows; r++) {
        for (uint32_t b = 0; b < kBins; b++) {
            float a = from.bin[r][b];
            float z = to.bin[r][b];
            bool room = r < kMaxChannels && b >= kFirstSend && b != kMultipassBin;
            if (a != 0 || z != 0) lanes.push_back({r, b, a, (z - a) / float(frames), room});
        }
    }
    return lanes;
}

// The library's full HRTF set, read from the title's own image. DirectSoundUseFullHRTF calls the routine that fills
// DirectSound's table of 3D routines (Forza's 0x3F5D8D), whose tenth entry is the HRTF filter selector (0x3F5C71).
// The selector reads a table of record pair numbers, 61 azimuths by 31 elevations (0x40EF48), and hands back a pair of
// 32 byte records (0x3FD988): 31 taps in sign and magnitude bytes, then the far ear's delay in samples. Per voice the
// library sends both ears' taps and the delay to the hardware (0x3F8F5D) unchanged, so the hardware gives every
// record one gain, which nothing in the library states. At 1/128, the magnitude as a 7 bit fraction, the set's low
// end would run 7 to 13 dB over the delay alone and a race's 3D mix far past full scale. At 1/512 the two ears' power
// below 500 Hz lies within 3.4 dB under to 0.7 dB over the delay alone for nine directions in ten (median 1.4 under),
// close to what a head does to low frequencies, and the front, the set's quietest direction, 5 dB under. 1/512 is
// taken: inferred from the set, not measured on a console. Without the set (a title that never calls
// DirectSoundUseFullHRTF), or with XBE_NO_HRTF, a mono 3D voice gets the interaural delay alone.
struct HrtfTable {
    bool tried = false;
    uint32_t slot = 0;      // the 3D routine table's entry holding the selector
    uint32_t selector = 0;  // the selector the tables were read from
    uint32_t index = 0;
    uint32_t records = 0;
    bool ok = false;
};
HrtfTable g_hrtf;

bool InImage(uint32_t va, uint32_t bytes) {
    for (const auto& s : g_image->sections) {
        if (va >= s.va && bytes <= s.vsize && va - s.va <= s.vsize - bytes) return true;
    }
    return false;
}

bool CodeIs(uint32_t va, std::initializer_list<uint8_t> bytes) {
    if (!InImage(va, uint32_t(bytes.size()))) return false;
    for (uint8_t b : bytes) {
        if (G<uint8_t>(va++) != b) return false;
    }
    return true;
}

constexpr float kHrirGain = 1.0f / 512;  // every record's, see above
// a tap byte, sign and magnitude: so read, the records' tails run smoothly, as two's complement they jump about
float HrirTap(uint8_t b) { return (b & 0x80 ? -float(b & 0x7F) : float(b)) * kHrirGain; }

// the selector the title installed, and its tables; false while there is none
bool HrtfReady() {
    if (!g_hrtf.tried) {
        g_hrtf.tried = true;
        uint32_t f = HAS_DirectSoundUseFullHRTF ? SYM(DirectSoundUseFullHRTF) : 0;
        // push esi; call; movzx esi, al; call the installer
        if (!f || !CodeIs(f, {0x56, 0xE8}) || !CodeIs(f + 6, {0x0F, 0xB6, 0xF0, 0xE8})) return false;
        uint32_t installer = f + 14 + R32(f + 10);
        // mov dword ptr [entry], routine, ten times
        for (uint32_t k = 0; k < 10; k++) {
            if (!CodeIs(installer + k * 10, {0xC7, 0x05})) return false;
        }
        g_hrtf.slot = R32(installer + 9 * 10 + 2);
        if (!InImage(g_hrtf.slot, 4)) g_hrtf.slot = 0;
    }
    uint32_t selector = g_hrtf.slot ? R32(g_hrtf.slot) : 0;
    if (!selector || selector == g_hrtf.selector) return selector && g_hrtf.ok;
    g_hrtf.selector = selector;
    g_hrtf.index = g_hrtf.records = 0;
    g_hrtf.ok = false;
    constexpr uint32_t kSearch = 0x140;  // the selector's length, about
    for (uint32_t at = selector; at < selector + kSearch; at++) {
        if (!g_hrtf.index && CodeIs(at, {0x0F, 0xB7, 0x0C, 0x75})) g_hrtf.index = R32(at + 4);  // movzx ecx, [esi*2+]
        if (!g_hrtf.records && CodeIs(at, {0x8D, 0x91})) g_hrtf.records = R32(at + 2);          // lea edx, [ecx+]
    }
    constexpr uint32_t kAzimuths = 61, kElevations = 31;
    if (!InImage(g_hrtf.index, kAzimuths * kElevations * 2)) return false;
    uint32_t last = 0;
    for (uint32_t i = 0; i < kAzimuths * kElevations; i++) last = std::max<uint32_t>(last, R16(g_hrtf.index + i * 2));
    if (!InImage(g_hrtf.records, (last + 1) * 32 + 32)) return false;
    g_hrtf.ok = true;
    LOGI("dsound: HRTF set from the selector at %08X, %u records at %08X", selector, last + 2, g_hrtf.records);
    return true;
}

// Both ears' filters and the interaural delay for a direction, as the full HRTF selector picks them: elevation to
// 6 degrees, azimuth to 3, 6 or 12 by elevation (its fold of the back onto the front is for surround speakers and left
// out). From the right (positive azimuth) the left ear gets the first record, the far ear's, and the delay.
bool Hrir(float azimuth, float elevation, float taps[2][kHrirTaps], float& itd) {
    if (diag::NoHrtf() || !HrtfReady()) return false;
    int el = int(elevation >= 0 ? elevation + 3 : elevation - 3) / 6 * 6;
    int az = 0;
    float flat = std::fabs(azimuth);
    if (std::abs(el) == 90) {
        az = 0;
    } else if (std::abs(el) > 60) {
        az = int(flat + 6) / 12 * 12;
    } else if (std::abs(el) > 30) {
        az = int(flat + 3) / 6 * 6;
    } else {
        az = int(flat + 1.5f) / 3 * 3;
    }
    uint32_t ai = uint32_t(az / 3), ei = uint32_t(el + 90) / 6;
    if (ai >= 61) ai = 0;
    if (ei >= 31) ei = 0;
    uint32_t far_ear = g_hrtf.records + uint32_t(R16(g_hrtf.index + (ai * 31 + ei) * 2)) * 32;
    uint32_t near_ear = far_ear + 32;
    bool right = azimuth >= 0;
    for (uint32_t k = 0; k < kHrirTaps; k++) {
        taps[0][k] = HrirTap(G<uint8_t>((right ? far_ear : near_ear) + k));
        taps[1][k] = HrirTap(G<uint8_t>((right ? near_ear : far_ear) + k));
    }
    float delay = float(G<uint8_t>(far_ear + kHrirTaps));
    itd = right ? delay : -delay;
    return true;
}

// the voice's recent samples, the ear further from the sound reads them later
void PushEar(Voice& v, float x) {
    v.ear[v.ear_pos] = x;
    v.ear[v.ear_pos + kEarLength] = x;
    v.ear_pos = (v.ear_pos + 1) % kEarLength;
}
// an ear's filter on the samples delay back
float EarAt(const Voice& v, const float* taps, float delay) {
    float back = std::clamp(delay, 0.0f, float(kEarLength - kHrirTaps - 2));
    uint32_t whole = uint32_t(back);
    float part = back - float(whole);
    const float* now = &v.ear[v.ear_pos + kEarLength - 1 - whole];
    float y0 = 0, y1 = 0;
    for (uint32_t k = 0; k < kHrirTaps; k++) {
        y0 += taps[k] * now[-int(k)];
        y1 += taps[k] * now[-int(k) - 1];
    }
    return y0 + (y1 - y0) * part;
}

// a stream goes quiet: the synch hold ends, a started one loses its pause as well
void Deactivate(Voice& v) {
    v.sync = false;
    if (!v.active) return;
    v.active = v.paused = v.ending = false;
    StopSubmix(InputBufferOf(v));
}

// where a frame and the one after it are read from, for interpolation
struct Cursor {
    uint32_t data;
    uint32_t f;
    uint32_t next_data;
    uint32_t next_f;
};

// the stream's frame at its position, none once its packets ran out
std::optional<Cursor> StreamFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
        Packet pk = v.packets.front();
        v.packets.pop_front();
        pk.used = pk.size;
        Complete(v, pk, 0);
        v.pos -= FramesOf(v, pk.size);
        f = uint32_t(v.pos);
    }
    if (v.packets.empty()) {
        v.pos = 0;
        if (v.ending) Deactivate(v);
        return std::nullopt;
    }
    Packet& pk = v.packets.front();
    Cursor at = {pk.data, f, pk.data, f};
    if (f + 1 < FramesOf(v, pk.size)) {
        at.next_f = f + 1;
    } else if (v.packets.size() > 1) {
        at.next_data = v.packets[1].data;
        at.next_f = 0;
    }
    return at;
}

// the buffer's frame at its position, looping or stopping at the end of its region; none once stopped
std::optional<Cursor> BufferFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    uint32_t start = v.looping ? FramesOf(v, v.loop_start) : 0;
    uint32_t end = FramesOf(v, v.looping ? v.loop_start + v.loop_len : v.play_len);
    if (f >= end) {
        if (!v.looping || end <= start) {
            v.playing = false;
            v.pos = 0;
            StopSubmix(InputBufferOf(v));
            return std::nullopt;
        }
        v.pos = start + std::fmod(v.pos - start, double(end - start));
        f = uint32_t(v.pos);
    }
    if (!v.data) return std::nullopt;
    uint32_t base = v.data + v.play_start;
    Cursor at = {base, f, base, f};
    if (f + 1 < end) {
        at.next_f = f + 1;
    } else if (v.looping) {
        at.next_f = start;
    }
    return at;
}

// Adds the voice to the bins, frames of kBins interleaved, its levels ramped across the block. What it sends to the
// submix it plays into goes to the submix's input instead, and a mix-in or effect-in buffer plays its input.
void MixVoice(Voice& v, float* bins, uint32_t frames) {
    bool submix = TakesInput(v);
    if (submix && v.input.size() < frames) return;
    Voice* to = SubmixOf(v);
    float* send = to && to->input.size() >= frames ? to->input.data() : nullptr;
    Place at = Locate(v);
    Gains target = Levels(v, at);
    bool ears = at.ears && v.channels == 1;
    // the ears' filters and delay, ramped across the block like the levels
    float hrir[2][kHrirTaps] = {};
    float itd = 0;
    if (ears && !Hrir(at.azimuth, at.elevation, hrir, itd)) {
        hrir[0][0] = hrir[1][0] = 1;
        itd = Itd(at.azimuth);
    }
    if (!v.has_gain) {
        v.gain = target;
        v.itd = itd;
        std::memcpy(v.hrir, hrir, sizeof hrir);
    }
    v.has_gain = true;
    std::vector<Lane> lanes = AudibleLanes(v.gain, target, frames);
    float itd_step = (itd - v.itd) / float(frames);
    float hrir_step[2][kHrirTaps];
    for (uint32_t e = 0; e < 2; e++) {
        for (uint32_t k = 0; k < kHrirTaps; k++) hrir_step[e][k] = (hrir[e][k] - v.hrir[e][k]) / float(frames);
    }
    uint32_t channels = std::min<uint32_t>(v.channels, kMaxChannels);
    uint32_t read = ears ? 1u : 0u;  // the channels a lane plays, a bit each
    for (const Lane& lane : lanes) {
        if (lane.row < kMaxChannels) read |= 1u << lane.row;
    }
    double rate = double(v.freq ? v.freq : v.rate) / audio::kRate;
    const Data3d* d3 = Data3dOf(v);
    if (d3 && d3->doppler) rate = 48000.0 * std::exp2(Pitch(v) / 4096.0) / audio::kRate;
    VoiceFilter vf = FilterOf(v, at);
    bool filtering = vf.lowpass || vf.eq || vf.i3dl2;
    v.block[v.last ^ 1] = -1;  // the title may have rewritten that block's memory since the last mix
    float peak = 0;
    for (uint32_t i = 0; i < frames; i++) {
        float row[kRows] = {};
        float room[kMaxChannels] = {};  // the room path's samples
        if (submix) {
            row[0] = v.input[i];
        } else {
            std::optional<Cursor> cursor = v.stream ? StreamFrame(v) : BufferFrame(v);
            if (!cursor) break;
            float t = float(v.pos - cursor->f);
            for (uint32_t c = 0; c < channels; c++) {
                if (!(read >> c & 1)) continue;
                float x = Sample(v, cursor->data, cursor->f, int(c));
                row[c] = x + (Sample(v, cursor->next_data, cursor->next_f, int(c)) - x) * t;
                v.source_peak = std::max(v.source_peak, std::fabs(row[c]));
            }
            v.pos += rate;
        }
        for (uint32_t c = 0; c < channels; c++) {
            if (!(read >> c & 1)) continue;
            if (filtering) {
                row[c] = Filtered(vf, v.filtered[c], row[c], room[c]);
            } else {
                room[c] = row[c];
            }
        }
        if (ears) {
            v.itd += itd_step;
            for (uint32_t e = 0; e < 2; e++) {
                for (uint32_t k = 0; k < kHrirTaps; k++) v.hrir[e][k] += hrir_step[e][k];
            }
            PushEar(v, row[0]);
            row[kLeftEar] = EarAt(v, v.hrir[0], v.itd);
            row[kRightEar] = EarAt(v, v.hrir[1], -v.itd);
        }
        float* frame = bins + size_t(i) * kBins;
        for (Lane& lane : lanes) {
            lane.gain += lane.step;
            float x = (lane.room ? room[lane.row] : row[lane.row]) * lane.gain;
            peak = std::max(peak, std::fabs(x));
            if (send && lane.bin == kMultipassBin) {
                send[i] += x;
            } else {
                frame[lane.bin] += x;
            }
        }
    }
    v.gain = target;
    v.itd = itd;
    std::memcpy(v.hrir, hrir, sizeof hrir);
    v.out_peak = std::max(v.out_peak, peak);
}

bool Sounding(const Voice& v) { return !v.paused && !v.sync && (v.stream || v.playing); }

// how many submixes the voice plays through: the deeper submix plays first
int Depth(const Voice& v) {
    int depth = 0;
    for (const Voice* at = SubmixOf(v); at && depth < kMaxSubmixDepth; at = SubmixOf(*at)) depth++;
    return depth;
}

// XBE_AUDIO_DUMP: the output before clamping, as a float WAV whose sizes are brought up to date every second so a
// stopped run leaves a file that plays
void DumpOutput(const float* out, uint32_t frames) {
    static FILE* file = nullptr;
    static bool tried = false;
    static uint32_t bytes = 0, since_header = 0;
    if (!tried) {
        tried = true;
        if (diag::AudioDump().empty()) return;
#ifdef _WIN32
        file = _wfopen(diag::AudioDump().c_str(), L"wb");
#else
        file = std::fopen(diag::AudioDump().c_str(), "wb");
#endif
        if (!file) LOGE("dsound: cannot write %s", diag::AudioDump().string().c_str());
    }
    if (!file) return;
    constexpr uint32_t kMaxBytes = 0xFFFFFF00u - 44;  // a WAV's sizes are 32 bit
    uint32_t n = std::min(frames * 8, kMaxBytes - bytes);
    if (!n) return;
    if (!bytes) std::fseek(file, 44, SEEK_SET);
    std::fwrite(out, 1, n, file);
    bytes += n;
    since_header += frames;
    if (since_header < audio::kRate) return;
    since_header = 0;
    // RIFF, fmt (WAVE_FORMAT_IEEE_FLOAT, 2 channels, 48 kHz, 32 bits), data
    auto u32 = [](uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); };
    auto u16 = [](uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); };
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0};
    u32(h + 4, 36 + bytes);
    u16(h + 20, 3);
    u16(h + 22, 2);
    u32(h + 24, audio::kRate);
    u32(h + 28, audio::kRate * 8);
    u16(h + 32, 8);
    u16(h + 34, 32);
    std::memcpy(h + 36, "data", 4);
    u32(h + 40, bytes);
    std::fseek(file, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, file);
    std::fseek(file, 0, SEEK_END);
    std::fflush(file);
}

// The voice processor's passes: the voices playing into submixes fill their inputs, then each submix plays before
// any submix it plays into. Effect-in buffers play the last block's sends.
void Render(float* out, uint32_t frames) {
    static std::vector<float> bins;  // the output thread alone renders
    bins.assign(size_t(frames) * kBins, 0.0f);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::vector<std::pair<int, Voice*>> submixes;
    for (auto& [obj, v] : g_voices) {
        if (IsSubmix(v)) {
            v.input.assign(frames, 0.0f);
            submixes.push_back({Depth(v), &v});
        } else if (TakesInput(v)) {
            v.input.swap(v.effect_out);
            v.input.resize(frames);
        }
    }
    for (auto& [obj, v] : g_voices) {
        if (!IsSubmix(v) && Sounding(v)) MixVoice(v, bins.data(), frames);
    }
    std::stable_sort(submixes.begin(), submixes.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [depth, v] : submixes) {
        float peak = 0;
        for (float x : v->input) peak = std::max(peak, std::fabs(x));
        v->input_peak = std::max(v->input_peak, peak);
        // the multipass bin is the GP's 24 bit memory, which saturates
        for (float& x : v->input) x = std::clamp(x, -1.0f, 1.0f);
        // the block its last voice stopped in still plays
        if (Sounding(*v) || peak > 0) MixVoice(*v, bins.data(), frames);
    }
    Meters& m = g_meters;
    for (uint32_t i = 0; i < frames; i++) {
        const float* b = &bins[size_t(i) * kBins];
        for (uint32_t k = 0; k < kBins; k++) {
            m.bin[k] = std::max(m.bin[k], std::fabs(b[k]));
            m.bins_clipped += std::fabs(b[k]) > 1.0f;
        }
    }
    if (dsp::Loaded()) dsp::Run(bins.data(), frames);
    for (auto& [obj, v] : g_voices) {
        if (!TakesInput(v) || IsSubmix(v)) continue;
        v.effect_out.resize(frames);
        for (uint32_t i = 0; i < frames; i++) v.effect_out[i] = bins[size_t(i) * kBins + v.input_bin];
    }
    for (uint32_t i = 0; i < frames; i++) {
        const float* b = &bins[size_t(i) * kBins];
        for (uint32_t k = 0; k < 6; k++) m.speaker[k] = std::max(m.speaker[k], std::fabs(b[k]));
        float left = b[kFrontLeft] + kFold * (b[kCentre] + b[kBackLeft]);
        float right = b[kFrontRight] + kFold * (b[kCentre] + b[kBackRight]);
        m.out_peak = std::max({m.out_peak, std::fabs(left), std::fabs(right)});
        m.out_squares += double(left) * left + double(right) * right;
        m.clipped += (std::fabs(left) > 1.0f) + (std::fabs(right) > 1.0f);
        out[i * 2] = left;
        out[i * 2 + 1] = right;
    }
    m.out_samples += uint64_t(frames) * 2;
    DumpOutput(out, frames);
    for (uint32_t i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Run(X86Context& ctx, const std::vector<Done>& done) {
    for (const Done& d : done) CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
}

// The MCPX APU's global sample counter (NV_PAPU_XGSCNT, 0xFE80200C), which DirectSound and XACT read directly as
// their clock: its register page is committed and the counter advances at 48 kHz from the host clock.
constexpr uint32_t kApuSampleCount = 0xFE80200C;

void StartApuClock() {
    static bool started = false;
    if (started) return;
    started = true;
    mem::Commit(kApuSampleCount & ~0xFFFu, 0x1000);
    std::thread([] {
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
            W32(kApuSampleCount, uint32_t(us.count() * 48 / 1000));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

void Start() {
    if (g_started) return;
    g_started = true;
    StartApuClock();
    audio::StartOutput(Render);
    // for a title that never calls DirectSoundDoWork: its callbacks come from here instead
    StartHostThread("dsound", [](X86Context& ctx) {
        constexpr int64_t kPumpMs = 250;
        for (;;) {
            std::vector<Done> done;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait_for(lock, std::chrono::milliseconds(50));
                if (NowMs() - g_pumped < kPumpMs) continue;
                done.assign(g_done.begin(), g_done.end());
                g_done.clear();
            }
            Run(ctx, done);
        }
    });
}

// WAVEFORMATEX
void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// The bins a voice gets with no list (SetMixBins, 0x3F32A8, its tables at 0x3FD494 and on), all at 0 mB: a 3D voice
// the crosstalk bins and the reverb send, a voice that plays into another buffer none (its input bin comes after),
// any other the speakers by its channel count.
void DefaultBins(Voice& v) {
    static const uint8_t k3d[] = {kCrosstalkFrontLeft, kCrosstalkBackLeft, kCrosstalkFrontRight, kCrosstalkBackRight,
                                  kI3dl2Send};
    static const uint8_t kStereo[] = {kFrontLeft, kFrontRight};
    static const uint8_t kQuad[] = {kFrontLeft, kFrontRight, kBackLeft, kBackRight};
    static const uint8_t kSix[] = {kFrontLeft, kFrontRight, kCentre, kSub, kBackLeft, kBackRight};
    v.nbins = 0;
    const uint8_t* bins = kStereo;
    uint8_t n = 2;
    if (v.flags & (kHrtfVoice | 0x10)) {
        bins = k3d;
        n = 5;
    } else if (v.output) {
        return;
    } else if (v.channels / 2 == 2) {
        bins = kQuad;
        n = 4;
    } else if (v.channels / 2 >= 3) {
        bins = kSix;
        n = 6;
    }
    for (uint8_t i = 0; i < n; i++) {
        v.bin[i] = bins[i];
        v.bin_mb[bins[i]] = 0;
    }
    v.nbins = n;
}

// DSMIXBINS: the voice's bins and their volumes
void SetBins(Voice& v, uint32_t mixbins) {
    if (!mixbins) {
        DefaultBins(v);
        return;
    }
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v.bin[i] = R32(pairs + i * 8) & 31;
        v.bin_mb[v.bin[i]] = int32_t(R32(pairs + i * 8 + 4));
    }
    v.nbins = uint8_t(n);
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v.bin_mb[R32(pairs + i * 8) & 31] = int32_t(R32(pairs + i * 8 + 4));
}
// 600 mB unless 3D or a mix in or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }

// creation flags: DSBCAPS_CTRL3D and 0x400000 give the voice the 3D data block, switched on
void SetFlags(Voice& v, uint32_t flags) {
    v.flags = flags;
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.has_data3d = v.data3d_on = flags & 0x400010;
    v.headroom = DefaultHeadroom(flags);
}

// Set3DVoiceData's parameters: dword 0 flags the fields that follow, each at its offset in the block. A voice with
// no block takes the HRTF angles alone.
void SetData3d(Voice& v, uint32_t data) {
    uint32_t flags = R32(data);
    auto mb = [&](uint32_t offset) { return int32_t(R32(data + offset)); };
    Data3d& d = v.data3d;
    if (flags & 0x40) {
        d.azimuth = F32(R32(data + 0x28));
        d.elevation = F32(R32(data + 0x2C));
    }
    if (!v.has_data3d) return;
    if (flags & 0x01) d.distance = mb(0x04);
    if (flags & 0x02) d.cone = mb(0x08);
    if (flags & 0x04) {
        d.front = mb(0x0C);
        d.rear = mb(0x10);
    }
    if (flags & 0x08) {
        d.crosstalk = mb(0x14);
        d.centre = mb(0x18);
    }
    if (flags & 0x10) {
        d.direct = mb(0x1C);
        d.room = mb(0x20);
    }
    if (flags & 0x20) d.doppler = mb(0x24);
    if (flags & 0x80) {
        d.lowpass[0] = R32(data + 0x30) & 0xFFFF;
        d.lowpass[1] = R32(data + 0x34) & 0xFFFF;
    }
}

// verbose: every few seconds, the levels at each stage of the mix, the loudest voices, the 3D data of the voices
// playing with it, what each submix takes in, and the streams playing without 3D data (music, movies) with the loudest
// samples they read and added
void ReportMix() {
    static int64_t next = 0;
    int64_t now = NowMs();
    if (now < next) return;
    next = now + 3000;
    Meters& m = g_meters;
    std::string bins_before, speakers;
    char item[32];
    for (uint32_t b = 0; b < kBins; b++) {
        if (m.bin[b] < 0.001f) continue;
        std::snprintf(item, sizeof item, " %02X %.3f", b, m.bin[b]);
        bins_before += item;
    }
    for (uint32_t b = 0; b < 6; b++) {
        std::snprintf(item, sizeof item, " %.3f", m.speaker[b]);
        speakers += item;
    }
    double rms = m.out_samples ? std::sqrt(m.out_squares / double(m.out_samples)) : 0.0;
    LOGI("dsound: output peak %.3f, RMS %.1f dBFS, %llu of %llu samples past full scale; bins before effects%s "
         "(%llu bin samples past full scale); speaker bins after%s",
         m.out_peak, rms > 0 ? 20 * std::log10(rms) : -120.0, (unsigned long long)m.clipped,
         (unsigned long long)m.out_samples, bins_before.c_str(), (unsigned long long)m.bins_clipped, speakers.c_str());
    std::vector<const Voice*> loud;
    for (auto& [obj, v] : g_voices) {
        if (v.out_peak > 0.001f) loud.push_back(&v);
    }
    std::sort(loud.begin(), loud.end(), [](const Voice* a, const Voice* b) { return a->out_peak > b->out_peak; });
    std::string voices;
    for (size_t i = 0; i < loud.size() && i < 10; i++) {
        const Voice& v = *loud[i];
        char line[112];
        std::snprintf(line, sizeof line, " %08X (flags %X, %u ch, %d mB, headroom %d, filter %u) %.3f;", v.obj,
                      v.flags, v.channels, Mb(v.volume), Mb(v.headroom), v.filter.mode, v.out_peak);
        voices += line;
    }
    LOGI("dsound: %zu voices on bins, loudest:%s", loud.size(), voices.c_str());
    std::string effects = dsp::Loaded() ? dsp::Report() : std::string();
    if (!effects.empty()) LOGI("dsound: effects image:%s", effects.c_str());
    m = Meters{};
    for (auto& [obj, v] : g_voices) {
        const Data3d* d = Data3dOf(v);
        if (v.stream && v.active && !d && !IsSubmix(v)) {
            LOGI("dsound: stream %08X (flags %X, %u channels, %u Hz, tag %X): source peak %.3f, mixed peak %.3f", obj,
                 v.flags, v.channels, v.freq ? v.freq : v.rate, v.tag, v.source_peak, v.out_peak);
        }
        v.source_peak = v.out_peak = 0;
        uint32_t sources = 0;
        if (IsSubmix(v)) {
            for (auto& [other, source] : g_voices) sources += source.output == obj;
        }
        bool sounding = v.stream ? v.active : v.playing;
        if (!(d && sounding) && !sources) continue;
        char bins[8 * 3 + 1] = {};
        for (uint32_t i = 0; i < v.nbins; i++) std::snprintf(bins + i * 3, 4, " %02X", v.bin[i]);
        char submix[96] = {};
        if (IsSubmix(v)) {
            std::snprintf(submix, sizeof submix, "; submix %s, %u voices in, peak %.3f",
                          sounding ? "playing" : "stopped", sources, v.input_peak);
            v.input_peak = 0;
        }
        Data3d none;
        if (!d) d = &none;
        LOGI("dsound: %08X (flags %X, %u channels, bins%s): 3D distance %d cone %d pan %d/%d centre %d/%d I3DL2 %d/%d "
             "Doppler %d, %.0f Hz; HRTF %.1f/%.1f, low-pass %X/%X%s",
             obj, v.flags, v.channels, bins, d->distance, d->cone, d->front, d->rear, d->crosstalk, d->centre,
             d->direct, d->room, d->doppler, 48000.0 * std::exp2(Pitch(v) / 4096.0), d->azimuth, d->elevation,
             d->lowpass[0], d->lowpass[1], submix);
    }
}

// The library adds the input bin of the buffer a voice plays into once for each of the voice's channels, after the
// voice's own bins, cutting them short to make room.
void AddInputBin(Voice& v, uint8_t bin) {
    uint32_t channels = std::min<uint32_t>(v.channels, 8);
    uint32_t have = uint32_t(std::count(v.bin, v.bin + v.nbins, bin));
    if (have >= channels) return;
    uint32_t need = channels - have;
    if (v.nbins + need > 8) v.nbins = uint8_t(8 - need);
    while (need--) v.bin[v.nbins++] = bin;
}

void RemoveBin(Voice& v, uint8_t bin) { v.nbins = uint8_t(std::remove(v.bin, v.bin + v.nbins, bin) - v.bin); }

uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    Voice& v = g_voices[obj];
    v.obj = obj;
    // DSBUFFERDESC: size, flags, bytes, format, mix bins, input mix bin
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);
    ReadFormat(v, R32(desc + 12));
    SetFlags(v, flags);
    // the bin a mix-in or effect-in buffer reads: an effect-in buffer's from the descriptor, a mix-in buffer's the
    // multipass bin, which is one channel
    if (flags & (kMixIn | kFxIn)) v.input_bin = flags & kInputFromDesc ? uint8_t(R32(desc + 20) & 31) : kMultipassBin;
    if (TakesInput(v)) v.channels = 1;
    if (TakesInput(v) && !IsSubmix(v)) LOGI("dsound: effect-in buffer %08X plays bin %02X", obj, v.input_bin);
    SetBins(v, R32(desc + 16));  // the defaults depend on the flags and the channels
    if (bytes) {
        v.data = v.owned = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 4);
        v.bytes = v.play_len = v.loop_len = bytes;
    }
    W32(out, obj);
    return 0;
}

// the effect's state segment when offset and size lie inside it, else 0
uint32_t EffectState(uint32_t effect, uint32_t offset, uint32_t size) {
    if (effect >= g_effect_states.size()) return 0;
    const Segment& state = g_effect_states[effect];
    if (offset > state.bytes || size > state.bytes - offset) return 0;
    return state.at;
}

}  // namespace

// device

// DSOUND objects are the host's; .XTLID names linked exports that would still run console code on them
static void AuditExports() {
    static const uint32_t handled[] = {
        SYM(DirectSoundCreate), SYM(IDirectSound_Release), SYM(DirectSoundUseFullHRTF), SYM(DirectSoundUseLightHRTF),
        SYM(IDirectSound_GetSpeakerConfig),
        SYM(IDirectSound_SetEffectData), SYM(IDirectSound_EnableHeadphones), SYM(IDirectSound_SetMixBinHeadroom),
        SYM(IDirectSound_CommitDeferredSettings), SYM(IDirectSound_SetDopplerFactor), SYM(IDirectSound_SetRolloffFactor),
        SYM(IDirectSound_SetDistanceFactor), SYM(IDirectSound_SetVelocity), SYM(IDirectSound_SetI3DL2Listener),
        SYM(IDirectSound_SetPosition), SYM(IDirectSound_SetOrientation), SYM(IDirectSound_SynchPlayback), SYM(DirectSoundDoWork),
        SYM(IDirectSound_DownloadEffectsImage), SYM(IDirectSound_GetEffectData), SYM(IDirectSound_CommitEffectData),
        SYM(IDirectSound_GetOutputLevels), SYM(IDirectSoundBuffer_SetPlayRegion), SYM(IDirectSoundBuffer_SetAllParameters),
        SYM(IDirectSoundBuffer_SetFormat), SYM(IDirectSoundBuffer_SetOutputBuffer), SYM(IDirectSoundBuffer_SetRolloffCurve),
        SYM(IDirectSoundBuffer_StopEx), SYM(IDirectSoundBuffer_Pause), SYM(IDirectSoundBuffer_SetEG),
        SYM(XAudioDownloadEffectsImage), SYM(DirectSoundCreateBuffer), SYM(IDirectSound_CreateSoundBuffer),
        SYM(IDirectSoundBuffer_Release), SYM(IDirectSoundBuffer_Unlock), SYM(IDirectSoundBuffer_SetBufferData),
        SYM(IDirectSoundBuffer_Play), SYM(IDirectSoundBuffer_Stop), SYM(IDirectSoundBuffer_SetLoopRegion),
        SYM(IDirectSoundBuffer_GetStatus), SYM(IDirectSoundBuffer_GetCurrentPosition), SYM(IDirectSoundBuffer_SetCurrentPosition),
        SYM(IDirectSoundBuffer_Lock), SYM(IDirectSoundBuffer_SetVolume), SYM(IDirectSoundBuffer_SetFrequency),
        SYM(IDirectSoundBuffer_SetFilter), SYM(IDirectSoundBuffer_SetHeadroom), SYM(IDirectSoundBuffer_SetMixBinVolumes),
        SYM(IDirectSoundBuffer_SetMixBins), SYM(IDirectSoundBuffer_SetMaxDistance), SYM(IDirectSoundBuffer_SetMinDistance),
        SYM(IDirectSoundBuffer_SetPosition), SYM(IDirectSoundBuffer_SetVelocity), SYM(IDirectSoundBuffer_SetI3DL2Source),
        SYM(DirectSoundCreateStream), SYM(CDirectSoundStream_AddRef), SYM(CDirectSoundStream_SetFormat),
        SYM(CDirectSoundStream_Release), SYM(CDirectSoundStream_GetInfo), SYM(CDirectSoundStream_GetStatus),
        SYM(CDirectSoundStream_Process), SYM(CDirectSoundStream_Discontinuity), SYM(CDirectSoundStream_Flush),
        SYM(CDirectSoundStream_FlushEx), SYM(CDirectSoundStream_Pause), SYM(CDirectSoundStream_SetVolume),
        SYM(CDirectSoundStream_SetFrequency), SYM(CDirectSoundStream_SetFilter), SYM(CDirectSoundStream_SetHeadroom),
        SYM(CDirectSoundStream_SetMixBinVolumes), SYM(CDirectSoundStream_SetMixBins), SYM(CDirectSoundStream_SetMaxDistance),
        SYM(CDirectSoundStream_SetMinDistance), SYM(CDirectSoundStream_SetPosition), SYM(CDirectSoundStream_SetVelocity),
        SYM(CDirectSoundStream_SetI3DL2Source), SYM(IDirectSoundStream_SetEG), SYM(IDirectSoundBuffer_PlayEx),
        SYM(IDirectSoundBuffer_SetPitch), SYM(IDirectSoundBuffer_SetLFO), SYM(IDirectSoundBuffer_SetConeAngles),
        SYM(IDirectSoundBuffer_SetConeOrientation), SYM(IDirectSoundBuffer_SetConeOutsideVolume),
        SYM(IDirectSoundBuffer_SetMode), SYM(IDirectSoundBuffer_SetDopplerFactor), SYM(IDirectSound_CreateSoundStream),
        SYM(IDirectSoundBuffer_GetVoiceProperties), SYM(CDirectSoundStream_GetVoiceProperties),
        SYM(CDirectSoundStream_SetLFO), SYM(CDirectSoundStream_SetOutputBuffer), SYM(CDirectSoundStream_SetPitch),
        SYM(XAudioSetEffectData)};
    std::string left;
    // the library's own section: the WMA decoder shares the export ids and touches no DSOUND object
    uint32_t lo = 0, hi = 0;
    for (auto& s : g_image->sections)
        if (SYM(DirectSoundCreate) - s.va < s.vsize) lo = s.va, hi = s.va + s.vsize;
    for (auto& s : g_image->sections)
        for (uint32_t o = 0; s.name == ".XTLID" && o + 8 <= std::min(s.vsize, s.rsize); o += 8) {
            uint32_t id = g_image->U32(s.raw + o), va = g_image->U32(s.raw + o + 4);
            auto done = [&](uint32_t f) { return std::find(std::begin(handled), std::end(handled), f) != std::end(handled); };
            // a jmp to a host version is covered by it
            if (id >> 16 != 0xB || va < lo || va >= hi || !dispatch::Find(va) || done(va) ||  // 0xB: DSOUND's library id
                (G<uint8_t>(va) == 0xE9 && done(va + 5 + R32(va + 1))))
                continue;
            char b[24];
            std::snprintf(b, sizeof b, " %05X at %08X", id, va);
            left += b;
        }
    if (!left.empty()) LOGW("dsound: exports running their original code:%s", left.c_str());
}

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        AuditExports();
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
// DirectSoundUseFullHRTF and DirectSoundUseLightHRTF run their original code: they only store the HRTF routine the
// library's 3D calculator calls, which XACT uses on its own, and touch no DSOUND object
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    W32(a[1], 0);  // DSSPEAKER_STEREO
    return 0;
}
// (this, effect, offset, data, size, flags): writes the effect's state
HLE_STDCALL(IDirectSound_SetEffectData, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t state = EffectState(a[1], a[2], a[4]);
    if (!state || !a[3]) return kInvalidCall;
    std::memcpy(HostPtr(state + a[2]), HostPtr(a[3]), a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) { return 0; }
// (this, mix bin, headroom)
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (a[1] < kBins) g_bin_headroom[a[1]] = a[2] & 7;
    return 0;
}
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
// DSI3DL2LISTENER as 12 floats: room, room HF, reflections and reverb are integers
static void ReadListener(uint32_t at, float p[12]) {
    for (int i = 0; i < 12; i++) {
        uint32_t v = R32(at + i * 4);
        bool integer = i < 2 || i == 5 || i == 7;
        p[i] = integer ? float(int32_t(v)) : F32(v);
    }
}
// (this, DSI3DL2LISTENER, apply): for the reverb the effects image's location names, which the library refuses
// without one (0x3F4E7E), as for an image downloaded with the index DSFX_IMAGELOC_UNUSED
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) {
    float p[12];
    ReadListener(a[1], p);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!dsp::SetListener(p)) return kInvalidCall;
    g_hf_reference = p[11];
    return 0;
}
// (effect, parameters, the converted state out): the XAudio helper turns a typed parameter block into an effect's
// state and writes it with the library's own DirectSound object, which the host replaces, so it runs here. Type 12 is
// an I3DL2 reverb's DSI3DL2LISTENER for the reverb at that index: a title that runs more than one reverb (Forza two,
// its image locating none) sets them so. Types 4 and 10 (filters), and the state the library hands back, are not
// reproduced.
HLE_STDCALL(XAudioSetEffectData, 3) {
    constexpr uint32_t kI3dl2Reverb = 12;
    uint32_t type = a[1] ? R32(a[1]) : 0;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (type == kI3dl2Reverb) {
        float p[12];
        ReadListener(a[1] + 4, p);
        if (dsp::SetReverb(int32_t(a[0]), p)) {
            LOGI("dsound: effect %u, I3DL2 reverb: room %.0f, room HF %.0f, decay %.2f s, HF ratio %.2f, reflections "
                 "%.0f after %.3f s, reverb %.0f after %.3f s, diffusion %.0f, density %.0f, HF reference %.0f Hz",
                 a[0], p[0], p[1], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
            return 0;
        }
    }
    static uint32_t warned = 0;  // a bit a type
    if (type < 32 && !(warned >> type & 1)) {
        warned |= 1u << type;
        LOGW("dsound: XAudioSetEffectData of type %u for effect %u %s", type, a[0],
             type == kI3dl2Reverb ? "finds no I3DL2 reverb there" : "not reproduced");
    }
    return kInvalidCall;
}
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_listener[i] = F32(a[1 + i]);
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) {
        g_front[i] = F32(a[1 + i]);
        g_top[i] = F32(a[4 + i]);
    }
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        done.assign(g_done.begin(), g_done.end());
        g_done.clear();
    }
    Run(ctx, done);
    return 0;
}
// starts together every voice played or unpaused with the synch playback flag
HLE_STDCALL(IDirectSound_SynchPlayback, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) v.sync = false;
    return 0;
}

// The descriptor keeps the image's effect list so the title finds its indices. State segments hold the image's plain
// state, which the host effects read (xdk/dsp_image.cpp); code, Y memory and scratch point at zeroed memory.
// loc: DSEFFECTIMAGELOC, the I3DL2 reverb's index first
static uint32_t EffectsDesc(const std::vector<uint8_t>& img, const char* name, uint32_t loc) {
    constexpr int kStateSegment = 1;
    XDK_REQUIRE("dsound", XDK_KEY(EffectsImage_Skip0), XDK_KEY(EffectsImage_Skip1), XDK_KEY(EffectsImage_Desc));
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4); return v; };
    size_t t = XDK_EffectsImage_Desc + size_t(u32(XDK_EffectsImage_Skip0) + u32(XDK_EffectsImage_Skip1)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name, count);
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    std::vector<Segment> states;
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            uint32_t at = u32(m + k * 8);
            bool inside = at <= img.size() && size <= img.size() - at;
            if (k == kStateSegment && inside) std::memcpy(HostPtr(seg), img.data() + at, size);
            if (k == kStateSegment) states.push_back({seg, size});
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    int32_t reverb = loc ? int32_t(R32(loc)) : -1;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_effect_states = states;
    std::vector<uint32_t> addresses;
    for (const Segment& s : states) addresses.push_back(s.at);
    dsp::Load(img, t, addresses, reverb);
    return desc;
}

HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for (auto& sec : g_image->sections)
            if (sec.name == name) img.assign(static_cast<uint8_t*>(HostPtr(sec.va)), static_cast<uint8_t*>(HostPtr(sec.va)) + sec.vsize);
    } else {
        ReadGameFile(name, img);
    }
    uint32_t desc = EffectsDesc(img, name.c_str(), a[1]);
    if (a[3]) W32(a[3], desc);
    return 0;
}

// (this, image, size, location, descriptor out): the title loaded the image itself
HLE_STDCALL(IDirectSound_DownloadEffectsImage, 5) {
    std::vector<uint8_t> img(static_cast<uint8_t*>(HostPtr(a[1])), static_cast<uint8_t*>(HostPtr(a[1])) + a[2]);
    uint32_t desc = EffectsDesc(img, "from memory", a[3]);
    if (a[4]) W32(a[4], desc);
    return 0;
}
// (this, effect, offset, data, size): reads the effect's state
HLE_STDCALL(IDirectSound_GetEffectData, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t state = EffectState(a[1], a[2], a[4]);
    if (!state || !a[3]) return kInvalidCall;
    std::memcpy(HostPtr(a[3]), HostPtr(state + a[2]), a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_CommitEffectData, 1) { return 0; }
// (this, DSOUTPUTLEVELS out, reset peaks): 16 dwords of meters, silent
HLE_STDCALL(IDirectSound_GetOutputLevels, 3) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 64);
    return 0;
}

// buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }

// DSFILTERDESC: mode, Q, four coefficients
static void ReadFilter(Voice& v, uint32_t p) {
    if (!p) return;
    v.filter.mode = R32(p);
    v.filter.q = R32(p + 4);
    for (int i = 0; i < 4; i++) v.filter.c[i] = uint16_t(R32(p + 8 + i * 4));
}

// DSI3DL2BUFFER: direct, direct HF, room, room HF, room rolloff factor, then obstruction and occlusion, each an HF
// level and its low frequency ratio
static void ReadI3dl2Source(Voice& v, uint32_t p) {
    if (!p) return;
    I3dl2Source& s = v.i3dl2;
    s.direct = int32_t(R32(p));
    s.direct_hf = int32_t(R32(p + 4));
    s.room = int32_t(R32(p + 8));
    s.room_hf = int32_t(R32(p + 12));
    s.room_rolloff = F32(R32(p + 16));
    s.obstruction = int32_t(R32(p + 20));
    s.obstruction_lf = F32(R32(p + 24));
    s.occlusion = int32_t(R32(p + 28));
    s.occlusion_lf = F32(R32(p + 32));
}

#define BUFFER(name, nargs)                                 \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(IDirectSoundBuffer_##name, nargs) {         \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        return v ? buf_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t buf_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

static uint32_t ReleaseBuffer(Voice& v);

// SetOutputBuffer (this, buffer): the voice plays into another buffer, which it holds a reference to. A mix-in or
// effect-in buffer takes the voice through its input bin, which replaces the voice's bins at 1/channels; SetMixBins
// adds it back after the bins it sets.
static void SetOutput(Voice& v, uint32_t to) {
    if (to == v.output) return;
    Voice* old = v.output ? Find(v.output) : nullptr;
    v.output = 0;
    if (old) {
        if (old->input_bin != kNoInput) RemoveBin(v, old->input_bin);
        if (TakesInput(*old)) StopSubmix(old);
        ReleaseBuffer(*old);
    }
    Voice* dest = to ? Find(to) : nullptr;
    if (!dest || dest->stream) return;
    dest->refs++;
    v.output = to;
    if (dest->input_bin == kNoInput) return;
    v.nbins = 0;
    AddInputBin(v, dest->input_bin);
    v.bin_mb[dest->input_bin] = Mb(1.0f / float(std::max<uint16_t>(v.channels, 1)));
    if (Running(v)) StartSubmixes(v);
}

// SetMixBins: the bins, then the input bin of the buffer the voice plays into
static void SetBinsKeepingInput(Voice& v, uint32_t mixbins) {
    SetBins(v, mixbins);
    if (Voice* to = v.output ? Find(v.output) : nullptr; to && to->input_bin != kNoInput) AddInputBin(v, to->input_bin);
}

// a buffer's last release lets go of the buffer it plays into, its memory and its object
static uint32_t ReleaseBuffer(Voice& v) {
    if (--v.refs > 0) return uint32_t(v.refs);
    SetOutput(v, 0);
    if (v.owned) mem::FreeContiguous(v.owned);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}

BUFFER(Release, 1) { return ReleaseBuffer(v); }
BUFFER(Unlock, 5) { return 0; }
BUFFER(SetBufferData, 3) {
    if (a[1] == v.data && a[2] == v.bytes) return 0;
    // the library frees its own memory, and makes it again for a size with no data
    if (v.owned && v.owned != a[1]) {
        mem::FreeContiguous(v.owned);
        v.owned = 0;
    }
    v.data = a[1];
    if (!a[1] && a[2]) v.data = v.owned = mem::AllocContiguous(a[2], 0, 0xFFFFFFFF, 4);
    v.bytes = v.play_len = v.loop_len = a[2];
    v.play_start = v.loop_start = 0;
    v.pos = 0;
    ForgetBlocks(v);
    return 0;
}
BUFFER(Play, 4) {
    Restart(v);
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    v.sync = a[3] & 4;
    StartSubmixes(v);
    return 0;
}
// (this, REFERENCE_TIME, flags): the start time is not honoured, it plays now
BUFFER(PlayEx, 4) {
    Restart(v);
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    v.sync = a[3] & 4;
    StartSubmixes(v);
    return 0;
}
BUFFER(Stop, 1) {
    v.playing = false;
    StopSubmix(InputBufferOf(v));
    return 0;
}
// (this, time low, time high, flags): stops now, the release envelope is not reproduced
BUFFER(StopEx, 4) {
    v.playing = false;
    StopSubmix(InputBufferOf(v));
    return 0;
}
// (this, flags): 0 resumes, 1 pauses, 2 holds until IDirectSound_SynchPlayback
BUFFER(Pause, 2) {
    v.paused = a[1] == 1;
    v.sync = a[1] == 2;
    return 0;
}
// envelopes are not played
BUFFER(SetEG, 2) { return 0; }
// (this, start, length) in bytes, 0 length plays to the end. The loop becomes the whole region.
BUFFER(SetPlayRegion, 3) {
    if (a[1] > v.bytes || a[2] > v.bytes - a[1]) return 0x88780032u;  // DSERR_INVALIDCALL
    v.play_start = a[1];
    v.play_len = v.loop_len = a[2] ? a[2] : v.bytes - a[1];
    v.loop_start = 0;
    v.pos = 0;
    ForgetBlocks(v);
    return 0;
}
BUFFER(SetLoopRegion, 3) {
    if (a[1] > v.play_len || a[2] > v.play_len - a[1]) return 0x88780032u;
    v.loop_start = a[1];
    v.loop_len = a[2] ? a[2] : v.play_len - a[1];
    return 0;
}
BUFFER(GetStatus, 2) {
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));  // DSBSTATUS_PLAYING, DSBSTATUS_LOOPING
    return 0;
}
// DSVOICEPROPS: mix bin count, 8 (bin, volume) pairs, then pitch, 3D distance volume, 3D cone volume, Doppler
// pitch, I3DL2 direct and room volumes. XACT reads it; the voice's own levels stand in for the hardware's. The pitch
// is the one playing, with the Doppler pitch in it; the 3D fields are the 3D data's while it is on, else zero.
static void PutVoiceProperties(const Voice& v, uint32_t out) {
    std::memset(HostPtr(out), 0, 4 + 8 * 8 + 6 * 4);
    W32(out, v.nbins);
    for (uint32_t i = 0; i < v.nbins && i < 8; i++) {
        W32(out + 4 + i * 8, v.bin[i]);
        W32(out + 8 + i * 8, uint32_t(v.bin_mb[v.bin[i] & 31]));
    }
    uint32_t at = out + 4 + 8 * 8;
    W32(at, uint32_t(int32_t(std::lround(Pitch(v)))));
    if (const Data3d* d = Data3dOf(v)) {
        W32(at + 4, uint32_t(d->distance));
        W32(at + 8, uint32_t(d->cone));
        W32(at + 12, uint32_t(d->doppler));
        W32(at + 16, uint32_t(d->direct));
        W32(at + 20, uint32_t(d->room));
    }
}
// pitch: 4096 units an octave from 48 kHz
static uint32_t PitchFrequency(uint32_t pitch) {
    return uint32_t(std::lround(48000.0 * std::exp2(double(int32_t(pitch)) / 4096.0)));
}
// (this, DSVOICEPROPS)
BUFFER(GetVoiceProperties, 2) {
    PutVoiceProperties(v, a[1]);
    return 0;
}
BUFFER(GetCurrentPosition, 3) {
    uint32_t p = BytesOf(v, uint32_t(v.pos));
    if (a[1]) W32(a[1], p);
    if (a[2]) W32(a[2], v.play_len ? (p + BytesOf(v, 64 * 4)) % v.play_len : 0);
    return 0;
}
BUFFER(SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;  // E_FAIL
    off %= v.bytes;
    uint32_t first = std::min(n, v.bytes - off);
    W32(a[3], v.data + off);
    W32(a[4], first);
    if (a[5]) W32(a[5], n > first ? v.data : 0);
    if (a[6]) W32(a[6], n > first ? n - first : 0);
    return 0;
}
BUFFER(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
BUFFER(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
// (this, DSFILTERDESC)
BUFFER(SetFilter, 2) {
    ReadFilter(v, a[1]);
    return 0;
}
BUFFER(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
BUFFER(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
BUFFER(SetMixBins, 2) {
    SetBinsKeepingInput(v, a[1]);
    return 0;
}
BUFFER(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
BUFFER(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
BUFFER(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
BUFFER(SetVelocity, 5) { return 0; }
// (this, mode, apply): DS3DMODE_NORMAL, HEADRELATIVE or DISABLE
BUFFER(SetMode, 3) {
    v.mode = a[1];
    return 0;
}
// (this, inside, outside, apply): full angles in degrees
BUFFER(SetConeAngles, 4) {
    v.cone_inside = a[1];
    v.cone_outside = a[2];
    return 0;
}
// (this, x, y, z, apply)
BUFFER(SetConeOrientation, 5) {
    for (int i = 0; i < 3; i++) v.cone_dir[i] = F32(a[1 + i]);
    return 0;
}
// (this, mB, apply)
BUFFER(SetConeOutsideVolume, 3) {
    v.cone_volume = int32_t(a[1]);
    return 0;
}
// Doppler and LFOs are not modelled
BUFFER(SetDopplerFactor, 3) { return 0; }
BUFFER(SetLFO, 2) { return 0; }
BUFFER(SetPitch, 2) {
    v.freq = PitchFrequency(a[1]);
    return 0;
}
// (this, DSI3DL2BUFFER, apply)
BUFFER(SetI3DL2Source, 3) {
    ReadI3dl2Source(v, a[1]);
    return 0;
}
// (this, DS3DBUFFER, apply): position at +4, the cone's angles at +0x1C and +0x20, its direction at +0x24 and its
// outside volume at +0x30, distances at +0x34 and +0x38, mode at +0x3C
BUFFER(SetAllParameters, 3) {
    uint32_t p = a[1];
    for (int i = 0; i < 3; i++) {
        v.p[i] = F32(R32(p + 4 + i * 4));
        v.cone_dir[i] = F32(R32(p + 0x24 + i * 4));
    }
    v.cone_inside = R32(p + 0x1C);
    v.cone_outside = R32(p + 0x20);
    v.cone_volume = int32_t(R32(p + 0x30));
    v.min_dist = F32(R32(p + 0x34));
    v.max_dist = F32(R32(p + 0x38));
    v.mode = R32(p + 0x3C);
    return 0;
}
// (this, points, count, apply)
BUFFER(SetRolloffCurve, 4) {
    v.curve.clear();
    for (uint32_t i = 0; a[1] && i < std::min<uint32_t>(a[2], 64); i++) v.curve.push_back(F32(R32(a[1] + i * 4)));
    return 0;
}
BUFFER(SetOutputBuffer, 2) {
    SetOutput(v, a[1]);
    ReportMix();
    return 0;
}
BUFFER(SetFormat, 2) {
    ReadFormat(v, a[1]);
    v.freq = 0;
    ForgetBlocks(v);
    return 0;
}
// Unexported methods XACT alone calls, buffer and stream alike, with the 3D calculator's results it worked out
// itself. (this, data): copies the flagged fields into the voice's 3D data block.
BUFFER(Set3DVoiceData, 2) {
    SetData3d(v, a[1]);
    ReportMix();
    return 0;
}
// (this, on): switches the 3D data block on or off; XACT turns it on for a 3D sound
BUFFER(Use3DVoiceData, 2) {
    v.data3d_on = a[1] != 0;
    return 0;
}

// streams, the object carries the XDK's own vtable

// (DSSTREAMDESC, out): DirectSoundCreateStream and IDirectSound_CreateSoundStream
static uint32_t NewStream(uint32_t desc, uint32_t out) {
    XDK_REQUIRE("dsound", XDK_KEY(CDirectSoundStream_Vtable), XDK_KEY(CDirectSoundStream_Vtable2),
                XDK_DATA(DSound_StreamVtable), XDK_DATA(DSound_StreamVtable2));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj + XDK_CDirectSoundStream_Vtable, kStreamVtable);
    W32(obj + XDK_CDirectSoundStream_Vtable2, kStreamVtable2);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.stream = true;
    uint32_t flags = R32(desc);
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    SetFlags(v, flags);
    SetBins(v, R32(desc + 20));
    W32(out, obj);
    return 0;
}

HLE_STDCALL(DirectSoundCreateStream, 2) { return NewStream(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundStream, 4) { return NewStream(a[1], a[2]); }

#define STREAM(name, nargs)                                     \
    static uint32_t str_##name(Voice& v, const Args& a);        \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {             \
        uint32_t r;                                             \
        std::vector<Done> flushed;                              \
        {                                                       \
            std::lock_guard<std::recursive_mutex> lock(g_lock); \
            Voice* v = Find(a[0]);                              \
            r = v ? str_##name(*v, a) : 0x80004005u;            \
            flushed.swap(t_flushed);                            \
        }                                                       \
        Run(ctx, flushed);                                      \
        return r;                                               \
    }                                                           \
    static uint32_t str_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

// the stream's callbacks still queued and those of its flushed packets, in order, go to the calling thread
static void FlushStream(Voice& v) {
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
    for (auto it = g_done.begin(); it != g_done.end();) {
        if (it->voice != v.obj) {
            ++it;
            continue;
        }
        t_flushed.push_back(*it);
        it = g_done.erase(it);
    }
    Deactivate(v);
}

STREAM(AddRef, 1) { return uint32_t(++v.refs); }
STREAM(SetFormat, 2) {
    FlushStream(v);
    ReadFormat(v, a[1]);
    v.freq = 0;  // the XDK sets the pitch back to the new format's own rate
    ForgetBlocks(v);
    return 0;
}
STREAM(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    FlushStream(v);
    SetOutput(v, 0);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
STREAM(GetInfo, 2) {
    uint32_t info = a[1];
    W32(info, 0x3);  // fixed sample size, fixed packet alignment
    W32(info + 4, v.tag == 0x69 ? 36u * v.channels : v.align);
    W32(info + 8, 0);
    W32(info + 12, 0);
    return 0;
}
STREAM(GetStatus, 2) {
    W32(a[1], v.packets.size() < v.max_packets ? 1u : 0u);  // XMEDIASTATUS_ACCEPT_INPUT
    return 0;
}
STREAM(Process, 3) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk{R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16)};
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    v.packets.push_back(pk);
    bool starting = !v.active;
    if (starting) Restart(v);  // after going quiet a stream starts fresh
    v.active = true;
    if (starting) StartSubmixes(v);
    v.ending = false;
    return 0;
}
STREAM(Discontinuity, 1) {
    if (!v.packets.empty()) v.ending = true;
    else if (v.active) FlushStream(v);
    return 0;
}
STREAM(Flush, 1) {
    FlushStream(v);
    return 0;
}
STREAM(FlushEx, 4) {
    if (a[3] && !v.active) return 0;
    FlushStream(v);
    return 0;
}
STREAM(Pause, 2) {
    v.paused = a[1] == 1;
    v.sync = a[1] == 2;
    return 0;
}
STREAM(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
STREAM(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
STREAM(SetFilter, 2) {
    ReadFilter(v, a[1]);
    return 0;
}
// the library's own writes voice registers a host stream does not have; envelopes are not played
HLE_STDCALL(IDirectSoundStream_SetEG, 2) { return 0; }
STREAM(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
STREAM(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
STREAM(SetMixBins, 2) {
    SetBinsKeepingInput(v, a[1]);
    return 0;
}
STREAM(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
STREAM(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
STREAM(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
STREAM(SetVelocity, 5) { return 0; }
STREAM(SetI3DL2Source, 3) {
    ReadI3dl2Source(v, a[1]);
    return 0;
}
// the library's own versions reach the hardware voice behind the stream (XACT calls them on its streams)
STREAM(GetVoiceProperties, 2) {
    PutVoiceProperties(v, a[1]);
    return 0;
}
STREAM(SetPitch, 2) {
    v.freq = PitchFrequency(a[1]);
    return 0;
}
STREAM(SetLFO, 2) { return 0; }
STREAM(SetOutputBuffer, 2) {
    SetOutput(v, a[1]);
    ReportMix();
    return 0;
}
// the stream's 3D data methods, bound at the exports: the bodies they jump to are no functions of their own
HLE_STDCALL(IDirectSoundStream_Set3DVoiceData, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Voice* v = Find(a[0]);
    if (!v) return 0x80004005u;
    SetData3d(*v, a[1]);
    ReportMix();
    return 0;
}
HLE_STDCALL(IDirectSoundStream_Use3DVoiceData, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Voice* v = Find(a[0]);
    if (!v) return 0x80004005u;
    v->data3d_on = a[1] != 0;
    return 0;
}
