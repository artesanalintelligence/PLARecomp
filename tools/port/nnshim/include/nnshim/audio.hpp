// nnshim::audio: native nn::audio::AudioOut.
//
// PLA mixes with Wwise and hands interleaved 16-bit PCM to nn::audio::AudioOut
// (main 0x7102e35b14..0x7102e360ec, Wwise's Switch "sink"):
//   OpenDefaultAudioOut(out, {sampleRate, channelCount 2|6, magic 0xCAFE})
//   N x SetAudioOutBufferInfo(buf, data, capacity, size)   (SDK, pure accessor)
//   StartAudioOut, AppendAudioOutBuffer(out, buf) ...
//   GetReleasedAudioOutBuffer(out) in a loop (polling: the game does not use the SDK event)
//   GetAudioOutState (0 = Started), StopAudioOut, CloseAudioOut
// This core knows nothing about the guest: the adapters (audio_abi.cpp) copy the PCM and pass
// the guest AudioOutBuffer address as a "cookie" that comes back from PopReleased.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nnshim::audio {

enum class State : int { Started = 0, Stopped = 1 };

class Backend;

struct Options {
    std::string backend = "xaudio2";   // "xaudio2" | "null" (no sound, real-time consumption)
    double volume = 1.0;
    bool mute = false;                 // volume 0 with the device open (can be undone live)
    // Testing: rebuild the engine every N s as if the default output changed (0 = never).
    double test_rebuild_sec = 0;
};

// One game AudioOut session, identified by the address of its nn::audio::AudioOut.
class Device {
public:
    explicit Device(const Options& opt);
    ~Device();

    // Opens an interleaved s16 PCM output. sample_rate 0 -> 48000, channels 0 -> 2.
    bool Open(uint64_t key, uint32_t sample_rate, uint32_t channels);
    bool IsOpen(uint64_t key) const;
    uint32_t Channels(uint64_t key) const;
    uint32_t SampleRate(uint64_t key) const;
    bool Start(uint64_t key);
    void Stop(uint64_t key);
    void Close(uint64_t key);
    State GetState(uint64_t key) const;
    // Copies `bytes` from `pcm` and queues them; `cookie` comes back from PopReleased once played.
    bool Append(uint64_t key, uint64_t cookie, const void* pcm, size_t bytes);
    // Next buffer already played (0 if none).
    uint64_t PopReleased(uint64_t key);

    // Statistics (for the log and tests).
    uint64_t appended() const;
    uint64_t released() const;
    uint64_t underruns() const;
    // Peak |sample| (0-32767) of what was queued since the last call (and resets it).
    uint32_t TakePeak();
    // Times the engine was rebuilt (default output change, device lost, or test).
    uint64_t rebuilds() const;
    // Requests an engine rebuild on the current default output (done by the watcher thread).
    void RequestRebuild();
    // Live volume / mute (in-game menu): applied by the watcher thread within ~50 ms.
    void SetLive(double volume, bool mute);
    const char* backend_name() const;

    struct Impl;   // public only for the XAudio2 callback (audio_core.cpp)

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nnshim::audio
