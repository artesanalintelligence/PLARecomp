// nnshim: AAPCS64 adapters for nn::audio::AudioOut ("audio" family).
//
// Structures (own disassembly of the PLA 1.1.1 sdk module):
//   AudioOutParameter (8 bytes): +0 s32 sampleRate, +4 u16 channelCount, +6 u16 magic 0xCAFE
//                                (InitializeAudioOutParameter, sdk+0x3ac60, writes 0xCAFE000000000000)
//   AudioOutBuffer (0x28 bytes): +8 void* data, +0x10 u64 capacity, +0x18 u64 size
//                                (SetAudioOutBufferInfo sdk+0x3ac70, GetAudioOutBufferDataPointer sdk+0x3ac80)
//   AudioOut: +0 used by the game as "open" (the Wwise sink checks *(u32*)out != 0 before
//             closing), +4 u32 channels (GetAudioOutChannelCount sdk+0x54c70 reads [x0+4]),
//             +8 u32 sample rate.
// Those SDK accessors are pure (no IPC): they stay in the SDK (PASS) and the shim keeps the
// same layout, so they keep working with a native AudioOut.
//
// Per-resource routing: an AudioOut the shim did not open (not in the Device) -> PLA_CONTINUE.
// If the host backend cannot open the output, OpenDefaultAudioOut returns PLA_CONTINUE and
// that session stays entirely in the HLE.
//
// The game polls GetReleasedAudioOutBuffer (it does not use the SDK's buffer-released event), so
// no kernel object is needed. If another game needed it: pla_api 1.1
// (event_create + event_guest_handle; event_signal from the backend callback).
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "abi_common.hpp"
#include "nnshim/audio.hpp"
#include "nnshim/config.hpp"
#include "nnshim/live.hpp"

namespace nnshim {
namespace {

// When the process exits without pla_mod_shutdown the device is not destroyed (XAudio2/COM under
// the loader lock and an already-dead thread): it is simply leaked.
struct AudioHolder : std::unique_ptr<audio::Device> {
    using std::unique_ptr<audio::Device>::operator=;
    ~AudioHolder() {
        if (ProcessExiting()) (void)release();
    }
};
AudioHolder g_audio;
std::mutex g_audio_mutex;   // only for creating/destroying g_audio
audio::Options g_audio_opt;
std::atomic<uint32_t> g_next_session{1};

audio::Device* Dev() {
    std::lock_guard lk(g_audio_mutex);
    if (!g_audio) {
        g_audio = std::make_unique<audio::Device>(g_audio_opt);
        Log(PLA_LOG_INFO, "nnshim audio: backend %s", g_audio->backend_name());
    }
    return g_audio.get();
}

// Live volume / mute from nvnvk's in-game menu ("nnshim.audio.volume", "nnshim.audio.mute").
void OnLiveAudio(bool changed) {
    if (!changed) return;
    std::string v;
    double volume;
    bool mute;
    {
        std::lock_guard lk(g_audio_mutex);
        volume = g_audio_opt.volume;
        mute = g_audio_opt.mute;
    }
    const double old_volume = volume;
    const bool old_mute = mute;
    if (live::Get("nnshim.audio.volume", v) && !v.empty()) volume = std::atof(v.c_str());
    if (live::Get("nnshim.audio.mute", v) && !v.empty()) mute = v == "true" || v == "1";
    if (volume == old_volume && mute == old_mute) return;
    std::lock_guard lk(g_audio_mutex);
    g_audio_opt.volume = volume;
    g_audio_opt.mute = mute;
    if (g_audio) g_audio->SetLive(volume, mute);
    Log(PLA_LOG_INFO, "nnshim audio: live volume %.2f%s", volume, mute ? " (muted)" : "");
}

void InitAudio(const cfg::Value& config) {
    g_audio_opt.backend = config.GetString("audio.backend", "xaudio2");
    g_audio_opt.volume = config.GetNumber("audio.volume", 1.0);
    g_audio_opt.mute = config.GetBool("audio.mute", false);
    g_audio_opt.test_rebuild_sec = config.GetNumber("audio.test_rebuild_sec", 0.0);
    live::Subscribe(&OnLiveAudio);
}

void ShutdownAudio() {
    live::Stop();
    std::lock_guard lk(g_audio_mutex);
    if (g_audio) {
        Log(PLA_LOG_INFO, "nnshim audio: %llu buffers queued, %llu released, %llu underruns, %llu device rebuilds (%s)",
            static_cast<unsigned long long>(g_audio->appended()),
            static_cast<unsigned long long>(g_audio->released()),
            static_cast<unsigned long long>(g_audio->underruns()),
            static_cast<unsigned long long>(g_audio->rebuilds()), g_audio->backend_name());
        g_audio.reset();
    }
}

ModuleHooks g_audio_module{"audio", &InitAudio, &ShutdownAudio};

}  // namespace
}  // namespace nnshim

using namespace nnshim;

// Result nn::audio::OpenDefaultAudioOut(AudioOut* out, const AudioOutParameter& p)
NNSHIM_ADAPTER(Audio, nnshim_audio_OpenDefaultAudioOut) {
    PlaGuest g(ctx);
    const uint64_t out = g.x(0), param_va = g.x(1);
    uint8_t param[8] = {};
    if (!out || g.read(param_va, param, sizeof param) != sizeof param) return PLA_CONTINUE;
    int32_t rate;
    uint16_t channels;
    std::memcpy(&rate, param, 4);
    std::memcpy(&channels, param + 4, 2);
    audio::Device* dev = Dev();
    if (!dev->Open(out, rate > 0 ? static_cast<uint32_t>(rate) : 48000, channels)) {
        Log(PLA_LOG_WARNING, "nnshim audio: could not open the output (%d Hz, %u channels): left to the HLE",
            rate, channels);
        return PLA_CONTINUE;
    }
    const uint32_t tag = 0xA0D00000u | (g_next_session.fetch_add(1) & 0xFFFF);
    const uint32_t ch = dev->Channels(out), sr = dev->SampleRate(out);
    g.write(out + 0, &tag, 4);
    g.write(out + 4, &ch, 4);
    g.write(out + 8, &sr, 4);
    Log(PLA_LOG_INFO, "nnshim audio: OpenDefaultAudioOut(%#llx) %u Hz, %u channels (requested %d Hz, %u)",
        static_cast<unsigned long long>(out), sr, ch, rate, channels);
    g.set_x(0, 0);   // ResultSuccess
    return PLA_SKIP;
}

// Result nn::audio::StartAudioOut(AudioOut*)
NNSHIM_ADAPTER(Audio, nnshim_audio_StartAudioOut) {
    PlaGuest g(ctx);
    audio::Device* dev = Dev();
    if (!dev->IsOpen(g.x(0))) return PLA_CONTINUE;
    dev->Start(g.x(0));
    g.set_x(0, 0);
    return PLA_SKIP;
}

// void nn::audio::StopAudioOut(AudioOut*)
NNSHIM_ADAPTER(Audio, nnshim_audio_StopAudioOut) {
    PlaGuest g(ctx);
    audio::Device* dev = Dev();
    if (!dev->IsOpen(g.x(0))) return PLA_CONTINUE;
    dev->Stop(g.x(0));
    return PLA_SKIP;
}

// void nn::audio::CloseAudioOut(AudioOut*)
NNSHIM_ADAPTER(Audio, nnshim_audio_CloseAudioOut) {
    PlaGuest g(ctx);
    audio::Device* dev = Dev();
    if (!dev->IsOpen(g.x(0))) return PLA_CONTINUE;
    dev->Close(g.x(0));
    Log(PLA_LOG_INFO, "nnshim audio: CloseAudioOut(%#llx)", static_cast<unsigned long long>(g.x(0)));
    return PLA_SKIP;
}

// AudioOutState nn::audio::GetAudioOutState(const AudioOut*)   (0 = Started, 1 = Stopped)
NNSHIM_ADAPTER(Audio, nnshim_audio_GetAudioOutState) {
    PlaGuest g(ctx);
    audio::Device* dev = Dev();
    if (!dev->IsOpen(g.x(0))) return PLA_CONTINUE;
    g.set_x(0, static_cast<uint64_t>(dev->GetState(g.x(0))));
    return PLA_SKIP;
}

// bool nn::audio::AppendAudioOutBuffer(AudioOut*, AudioOutBuffer*)
NNSHIM_ADAPTER(Audio, nnshim_audio_AppendAudioOutBuffer) {
    PlaGuest g(ctx);
    audio::Device* dev = Dev();
    const uint64_t out = g.x(0), buf = g.x(1);
    if (!dev->IsOpen(out)) return PLA_CONTINUE;
    uint64_t hdr[4] = {};   // +0 (link), +8 data, +0x10 capacity, +0x18 size
    bool ok = buf && g.read(buf, hdr, sizeof hdr) == sizeof hdr && hdr[1] && hdr[3] <= hdr[2] &&
              hdr[3] <= (16u << 20);
    if (ok) {
        // No intermediate guest copy when a host pointer is available (pla_api 1.1).
        const void* pcm = PLA_API_HAS(g_api, host_pointer) ? g_api->host_pointer(hdr[1], hdr[3]) : nullptr;
        std::vector<uint8_t> tmp;
        if (!pcm && hdr[3]) {
            tmp.resize(hdr[3]);
            ok = g.read(hdr[1], tmp.data(), tmp.size()) == tmp.size();
            pcm = tmp.data();
        }
        if (ok) ok = dev->Append(out, buf, pcm, hdr[3]);
    }
    // One line every 5 s: buffer rate and level (proof that there is sound).
    static std::atomic<uint64_t> last_ms{0}, last_n{0};
    const uint64_t now = UptimeMs();
    uint64_t prev = last_ms.load();
    const uint64_t prev_ms = prev;
    if (now - prev >= 5000 && last_ms.compare_exchange_strong(prev, now)) {
        const uint64_t n = dev->appended();
        Log(PLA_LOG_INFO, "nnshim audio: %llu buffers/s, peak %u/32767, queued %llu released %llu underruns %llu (%u B/buffer) rebuilds %llu",
            static_cast<unsigned long long>((n - last_n.exchange(n)) * 1000 / (now - prev_ms ? now - prev_ms : 1)),
            dev->TakePeak(), static_cast<unsigned long long>(n), static_cast<unsigned long long>(dev->released()),
            static_cast<unsigned long long>(dev->underruns()), static_cast<unsigned>(hdr[3]),
            static_cast<unsigned long long>(dev->rebuilds()));
    }
    g.set_x(0, ok ? 1 : 0);
    return PLA_SKIP;
}

// AudioOutBuffer* nn::audio::GetReleasedAudioOutBuffer(AudioOut*)
NNSHIM_ADAPTER(Audio, nnshim_audio_GetReleasedAudioOutBuffer) {
    PlaGuest g(ctx);
    audio::Device* dev = Dev();
    if (!dev->IsOpen(g.x(0))) return PLA_CONTINUE;
    g.set_x(0, dev->PopReleased(g.x(0)));
    return PLA_SKIP;
}
