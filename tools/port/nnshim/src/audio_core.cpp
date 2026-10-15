// nnshim::audio: host backends for nn::audio::AudioOut (see include/nnshim/audio.hpp).
//
//   xaudio2  one IXAudio2SourceVoice per session, interleaved s16 PCM (2 or 6 channels; 5.1 in
//            Switch order FL FR FC LFE RL RR = KSAUDIO_SPEAKER_5POINT1). OnBufferEnd marks the
//            buffer as released. XAudio2 2.9 ships with Windows 10/11 (nothing to redistribute).
//   null     no device: a thread "consumes" each buffer over its real duration. Used for tests
//            and as a fallback if XAudio2 fails.
//
// Device change: the XAudio2 mastering voice stays on the device it was created on. If Windows
// changes the default output (IMMNotificationClient::OnDefaultDeviceChanged) or the device goes
// away (IXAudio2EngineCallback::OnCriticalError, e.g. XAUDIO2_E_DEVICE_INVALIDATED), a watcher
// thread rebuilds the engine, the mastering voice and every session's voice on the new output.
// Whatever was queued is treated as played (the game gets its buffers back) and, while there is
// no device, sessions are consumed in real time as in the null backend (the game neither runs
// out of buffers nor hangs).
#include "nnshim/audio.hpp"
#include "nnshim/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <xaudio2.h>
#endif

namespace nnshim::audio {

namespace {

struct Pending {
    uint64_t cookie = 0;
    std::vector<uint8_t> data;
    uint64_t frames = 0;
};

}  // namespace

struct Session;

#ifdef _WIN32
struct VoiceCallback final : public IXAudio2VoiceCallback {
    Session* s = nullptr;
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) override;
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override;
};

// Critical engine error (device invalidated or removed): it must be rebuilt.
struct EngineCallback final : public IXAudio2EngineCallback {
    std::atomic<bool>* flag = nullptr;
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT) override {
        if (flag) flag->store(true);
    }
};

// Windows default output change (console or multimedia).
struct DeviceNotify final : public IMMNotificationClient {
    std::atomic<ULONG> refs{1};
    std::atomic<bool>* flag = nullptr;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole && flag) flag->store(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};
#endif

struct Session {
    uint64_t key = 0;
    uint32_t rate = 48000;
    uint32_t channels = 2;
    State state = State::Stopped;
    std::deque<std::unique_ptr<Pending>> queued;   // on the device, in order
    std::deque<uint64_t> released;
    // No voice (null backend or no device): time at which the first queued buffer finishes
    std::chrono::steady_clock::time_point null_due{};
#ifdef _WIN32
    IXAudio2SourceVoice* voice = nullptr;
    VoiceCallback cb;
#endif
};

struct Device::Impl {
    Options opt;
    mutable std::mutex m;   // sessions and queues; never held during an XAudio2 call
    // XAudio2 calls (shared) vs. rebuilding the engine (exclusive). XAudio2 callbacks only
    // take `m`, so there is no cycle.
    std::shared_mutex xa_m;
    std::map<uint64_t, std::unique_ptr<Session>> sessions;
    std::atomic<uint64_t> n_appended{0}, n_released{0}, n_underruns{0}, n_rebuilds{0};
    std::atomic<uint32_t> peak{0};
    std::atomic<bool> use_null{false};
    std::atomic<bool> want_rebuild{false};
    // Live volume (Device::SetLive): written by any thread, applied by the watcher (NullLoop).
    std::atomic<float> live_volume{1.0f};
    std::atomic<bool> live_mute{false};
    std::atomic<bool> live_dirty{false};
    float EffectiveVolume() const { return opt.mute ? 0.0f : static_cast<float>(opt.volume); }
#ifdef _WIN32
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    EngineCallback engine_cb;
    IMMDeviceEnumerator* enumerator = nullptr;
    DeviceNotify* notify = nullptr;
    bool com_ok = false;
#endif
    // Real-time thread (voiceless sessions) and device watcher.
    ExitSafeThread null_thread;
    std::condition_variable null_cv;
    bool null_stop = false;

    void OnReleased(Session* s, Pending* p) {
        // Called with m held.
        for (auto it = s->queued.begin(); it != s->queued.end(); ++it) {
            if (it->get() == p) {
                s->released.push_back(p->cookie);
                s->queued.erase(it);
                n_released.fetch_add(1, std::memory_order_relaxed);
                if (s->queued.empty() && s->state == State::Started)
                    n_underruns.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    bool CreateEngine() {
#ifdef _WIN32
        // With xa_m exclusive (or before any session exists).
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
            xa = nullptr;
            return false;
        }
        engine_cb.flag = &want_rebuild;
        xa->RegisterForCallbacks(&engine_cb);
        if (FAILED(xa->CreateMasteringVoice(&master))) {
            master = nullptr;
            xa->Release();
            xa = nullptr;
            return false;
        }
        if (EffectiveVolume() != 1.0f) master->SetVolume(EffectiveVolume());
        return true;
#else
        return false;
#endif
    }

    void DestroyEngine() {
#ifdef _WIN32
        if (master) master->DestroyVoice();
        master = nullptr;
        if (xa) {
            xa->UnregisterForCallbacks(&engine_cb);
            xa->StopEngine();
            xa->Release();
        }
        xa = nullptr;
#endif
    }

#ifdef _WIN32
    IXAudio2SourceVoice* CreateVoice(Session& s) {
        // With xa_m held (shared or exclusive) and without `m`. The caller publishes s.voice.
        if (!xa) return nullptr;
        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = static_cast<WORD>(s.channels);
        fmt.Format.nSamplesPerSec = s.rate;
        fmt.Format.wBitsPerSample = 16;
        fmt.Format.nBlockAlign = static_cast<WORD>(2 * s.channels);
        fmt.Format.nAvgBytesPerSec = s.rate * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 16;
        // FL FR FC LFE BL BR (= KSAUDIO_SPEAKER_5POINT1), FL FR, FC. No ks.h: it clashes with cguid.h.
        fmt.dwChannelMask = s.channels == 6 ? 0x3Fu : s.channels == 2 ? 0x3u : 0x4u;
        // KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00AA00389B71}
        fmt.SubFormat = GUID{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
        s.cb.s = &s;
        IXAudio2SourceVoice* v = nullptr;
        if (FAILED(xa->CreateSourceVoice(&v, &fmt.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &s.cb)))
            return nullptr;
        return v;
    }
#endif

    bool InitXAudio2() {
#ifdef _WIN32
        const HRESULT hr0 = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_ok = SUCCEEDED(hr0);
        if (!CreateEngine()) return false;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&enumerator)))) {
            notify = new DeviceNotify();
            notify->flag = &want_rebuild;
            if (FAILED(enumerator->RegisterEndpointNotificationCallback(notify))) {
                notify->Release();
                notify = nullptr;
            }
        }
        return true;
#else
        return false;
#endif
    }

    // Rebuilds the engine on the current default output. Returns false if there is no device.
    bool Rebuild() {
#ifdef _WIN32
        std::unique_lock xl(xa_m);
        // 1) Drop the voices (DestroyVoice waits for in-flight callbacks, which take `m`).
        std::vector<IXAudio2SourceVoice*> old;
        {
            std::lock_guard lk(m);
            for (auto& [k, s] : sessions) {
                if (s->voice) old.push_back(s->voice);
                s->voice = nullptr;
            }
        }
        for (auto* v : old) v->DestroyVoice();
        // 2) Whatever was queued counts as played; sessions switch to real time.
        {
            std::lock_guard lk(m);
            for (auto& [k, s] : sessions) {
                while (!s->queued.empty()) OnReleased(s.get(), s->queued.front().get());
                s->null_due = std::chrono::steady_clock::now();
            }
        }
        DestroyEngine();
        // 3) New engine and new voices.
        if (!CreateEngine()) return false;
        std::vector<Session*> list;
        {
            std::lock_guard lk(m);
            for (auto& [k, s] : sessions) list.push_back(s.get());
        }
        for (Session* s : list) {
            IXAudio2SourceVoice* v = CreateVoice(*s);
            if (!v) continue;
            bool started = false;
            {
                std::lock_guard lk(m);
                started = s->state == State::Started;
            }
            if (started) v->Start(0);
            std::lock_guard lk(m);
            s->voice = v;
        }
        n_rebuilds.fetch_add(1, std::memory_order_relaxed);
        return true;
#else
        return false;
#endif
    }

    void NullLoop() {
        using clock = std::chrono::steady_clock;
        auto next_check = clock::now();
        auto next_forced = opt.test_rebuild_sec > 0
                               ? clock::now() + std::chrono::milliseconds(static_cast<int64_t>(opt.test_rebuild_sec * 1000))
                               : clock::time_point::max();
        std::unique_lock lk(m);
        while (!null_stop) {
            auto next = clock::now() + std::chrono::milliseconds(50);
            const auto now = clock::now();
#ifdef _WIN32
            // Live volume / mute from the in-game menu (outside `m`: XAudio2 calls go under xa_m only).
            if (live_dirty.exchange(false)) {
                opt.volume = live_volume.load();
                opt.mute = live_mute.load();
                lk.unlock();
                {
                    std::shared_lock xl(xa_m);
                    if (master) master->SetVolume(EffectiveVolume());
                }
                lk.lock();
                continue;
            }
#endif
            for (auto& [k, s] : sessions) {
#ifdef _WIN32
                if (s->voice) continue;   // consumed by XAudio2
#endif
                while (s->state == State::Started && !s->queued.empty() && s->null_due <= now) {
                    Pending* p = s->queued.front().get();
                    OnReleased(s.get(), p);
                    if (!s->queued.empty()) {
                        const auto f = s->queued.front()->frames;
                        s->null_due += std::chrono::microseconds(f * 1000000 / s->rate);
                    }
                }
                if (s->state == State::Started && !s->queued.empty() && s->null_due < next)
                    next = s->null_due;
            }
#ifdef _WIN32
            // Watcher: output change, engine error or lost engine (retry every 1 s).
            if (!use_null && now >= next_check) {
                next_check = now + std::chrono::milliseconds(100);
                if (now >= next_forced) {
                    want_rebuild.store(true);
                    next_forced = now + std::chrono::milliseconds(static_cast<int64_t>(opt.test_rebuild_sec * 1000));
                }
                if (want_rebuild.exchange(false) || !xa) {
                    lk.unlock();
                    // Short wait: Windows notifies before the new output is ready.
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    if (!Rebuild()) next_check = clock::now() + std::chrono::seconds(1);
                    lk.lock();
                    continue;
                }
            }
#endif
            null_cv.wait_until(lk, next);
        }
    }
};

#ifdef _WIN32
// The callback does not know the Device: it resolves it via a global pointer (one per process).
static Device::Impl* g_impl_for_callbacks = nullptr;

void STDMETHODCALLTYPE VoiceCallback::OnBufferEnd(void* ctx) {
    Device::Impl* impl = g_impl_for_callbacks;
    if (!impl) return;
    std::lock_guard lk(impl->m);
    impl->OnReleased(s, static_cast<Pending*>(ctx));
}

void STDMETHODCALLTYPE VoiceCallback::OnVoiceError(void*, HRESULT) {
    if (Device::Impl* impl = g_impl_for_callbacks) impl->want_rebuild.store(true);
}
#endif

Device::Device(const Options& opt) : impl_(std::make_unique<Impl>()) {
    impl_->opt = opt;
    impl_->use_null = opt.backend == "null";
#ifdef _WIN32
    g_impl_for_callbacks = impl_.get();
#endif
    if (!impl_->use_null && !impl_->InitXAudio2()) impl_->use_null = true;
    // Always: consumes voiceless sessions and watches the device.
    impl_->null_thread = std::thread([this] { impl_->NullLoop(); });
}

Device::~Device() {
    if (impl_->null_thread.joinable()) {
        {
            std::lock_guard lk(impl_->m);
            impl_->null_stop = true;
        }
        impl_->null_cv.notify_all();
        impl_->null_thread.join();
    }
    std::vector<uint64_t> keys;
    {
        std::lock_guard lk(impl_->m);
        for (auto& [k, s] : impl_->sessions) keys.push_back(k);
    }
    for (uint64_t k : keys) Close(k);
#ifdef _WIN32
    if (impl_->enumerator) {
        if (impl_->notify) impl_->enumerator->UnregisterEndpointNotificationCallback(impl_->notify);
        impl_->enumerator->Release();
    }
    if (impl_->notify) impl_->notify->Release();
    impl_->DestroyEngine();
    g_impl_for_callbacks = nullptr;
    if (impl_->com_ok) CoUninitialize();
#endif
}

const char* Device::backend_name() const { return impl_->use_null ? "null" : "xaudio2"; }
uint64_t Device::appended() const { return impl_->n_appended.load(); }
uint64_t Device::released() const { return impl_->n_released.load(); }
uint64_t Device::underruns() const { return impl_->n_underruns.load(); }
uint64_t Device::rebuilds() const { return impl_->n_rebuilds.load(); }
void Device::RequestRebuild() { impl_->want_rebuild.store(true); }
void Device::SetLive(double volume, bool mute) {
    impl_->live_volume.store(static_cast<float>(std::clamp(volume, 0.0, 1.0)));
    impl_->live_mute.store(mute);
    impl_->live_dirty.store(true);
    {
        std::lock_guard lk(impl_->m);
    }
    impl_->null_cv.notify_all();
}
uint32_t Device::TakePeak() { return impl_->peak.exchange(0); }

bool Device::Open(uint64_t key, uint32_t sample_rate, uint32_t channels) {
    if (!sample_rate) sample_rate = 48000;
    if (!channels) channels = 2;
    if (channels != 1 && channels != 2 && channels != 6) return false;
    auto s = std::make_unique<Session>();
    s->key = key;
    s->rate = sample_rate;
    s->channels = channels;
#ifdef _WIN32
    if (!impl_->use_null) {
        std::shared_lock xl(impl_->xa_m);
        // No device right now: the session opens anyway and the watcher will give it a voice.
        if (impl_->xa) {
            s->voice = impl_->CreateVoice(*s);
            // Device just lost: no voice for now (real time) and the watcher rebuilds it.
            if (!s->voice) impl_->want_rebuild.store(true);
        }
        std::lock_guard lk(impl_->m);
        impl_->sessions[key] = std::move(s);
        return true;
    }
#endif
    std::lock_guard lk(impl_->m);
    impl_->sessions[key] = std::move(s);
    return true;
}

bool Device::IsOpen(uint64_t key) const {
    std::lock_guard lk(impl_->m);
    return impl_->sessions.count(key) != 0;
}

uint32_t Device::Channels(uint64_t key) const {
    std::lock_guard lk(impl_->m);
    auto it = impl_->sessions.find(key);
    return it == impl_->sessions.end() ? 0 : it->second->channels;
}

uint32_t Device::SampleRate(uint64_t key) const {
    std::lock_guard lk(impl_->m);
    auto it = impl_->sessions.find(key);
    return it == impl_->sessions.end() ? 0 : it->second->rate;
}

// XAudio2 calls are made without holding `m`: its thread takes `m` in OnBufferEnd and might
// hold an internal lock that SubmitSourceBuffer/Start/Stop need at the same time. They are made
// with `xa_m` shared, so the watcher cannot destroy the voice in between.
bool Device::Start(uint64_t key) {
    std::shared_lock xl(impl_->xa_m);
    void* voice = nullptr;
    {
        std::lock_guard lk(impl_->m);
        auto it = impl_->sessions.find(key);
        if (it == impl_->sessions.end()) return false;
        Session& s = *it->second;
        if (s.state == State::Started) return true;
        s.state = State::Started;
#ifdef _WIN32
        voice = s.voice;
#endif
        if (!s.queued.empty())
            s.null_due = std::chrono::steady_clock::now() +
                         std::chrono::microseconds(s.queued.front()->frames * 1000000 / s.rate);
        impl_->null_cv.notify_all();
    }
#ifdef _WIN32
    if (voice) static_cast<IXAudio2SourceVoice*>(voice)->Start(0);
#endif
    return true;
}

void Device::Stop(uint64_t key) {
    std::shared_lock xl(impl_->xa_m);
    void* voice = nullptr;
    {
        std::lock_guard lk(impl_->m);
        auto it = impl_->sessions.find(key);
        if (it == impl_->sessions.end()) return;
        it->second->state = State::Stopped;
#ifdef _WIN32
        voice = it->second->voice;
#endif
    }
#ifdef _WIN32
    // Outside the lock: FlushSourceBuffers fires OnBufferEnd, which takes it.
    if (voice) {
        auto* v = static_cast<IXAudio2SourceVoice*>(voice);
        v->Stop(0);
        v->FlushSourceBuffers();
        // FlushSourceBuffers is asynchronous: wait for the OnBufferEnd calls to arrive.
        for (int i = 0; i < 200; ++i) {
            XAUDIO2_VOICE_STATE st{};
            v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued == 0) break;
            Sleep(1);
        }
    }
#endif
    // Whatever was still queued counts as released (as on the console when stopping).
    std::lock_guard lk(impl_->m);
    auto it = impl_->sessions.find(key);
    if (it == impl_->sessions.end()) return;
    while (!it->second->queued.empty()) impl_->OnReleased(it->second.get(), it->second->queued.front().get());
}

void Device::Close(uint64_t key) {
    Stop(key);
    std::shared_lock xl(impl_->xa_m);
    std::unique_ptr<Session> s;
    {
        std::lock_guard lk(impl_->m);
        auto it = impl_->sessions.find(key);
        if (it == impl_->sessions.end()) return;
        s = std::move(it->second);
        impl_->sessions.erase(it);
    }
#ifdef _WIN32
    if (s->voice) s->voice->DestroyVoice();   // waits for callbacks to finish
#endif
}

State Device::GetState(uint64_t key) const {
    std::lock_guard lk(impl_->m);
    auto it = impl_->sessions.find(key);
    return it == impl_->sessions.end() ? State::Stopped : it->second->state;
}

bool Device::Append(uint64_t key, uint64_t cookie, const void* pcm, size_t bytes) {
    std::shared_lock xl(impl_->xa_m);
    Pending* raw = nullptr;
    Session* sp = nullptr;
    void* voice = nullptr;
    {
        std::lock_guard lk(impl_->m);
        auto it = impl_->sessions.find(key);
        if (it == impl_->sessions.end()) return false;
        Session& s = *it->second;
        auto p = std::make_unique<Pending>();
        p->cookie = cookie;
        p->data.assign(static_cast<const uint8_t*>(pcm), static_cast<const uint8_t*>(pcm) + bytes);
        p->frames = bytes / (2 * s.channels);
        {
            uint32_t pk = 0;
            const auto* smp = reinterpret_cast<const int16_t*>(p->data.data());
            for (size_t i = 0, n = p->data.size() / 2; i < n; ++i) {
                const int v = smp[i] < 0 ? -static_cast<int>(smp[i]) : smp[i];
                if (static_cast<uint32_t>(v) > pk) pk = static_cast<uint32_t>(v);
            }
            uint32_t cur = impl_->peak.load(std::memory_order_relaxed);
            while (pk > cur && !impl_->peak.compare_exchange_weak(cur, pk)) {}
        }
        raw = p.get();
        sp = &s;
        const bool was_empty = s.queued.empty();
        s.queued.push_back(std::move(p));
        impl_->n_appended.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        voice = s.voice;
#endif
        if (!voice && s.state == State::Started) {
            const auto now = std::chrono::steady_clock::now();
            if (was_empty || s.null_due < now - std::chrono::milliseconds(200))
                s.null_due = now + std::chrono::microseconds(raw->frames * 1000000 / s.rate);
            impl_->null_cv.notify_all();
        }
    }
#ifdef _WIN32
    if (voice) {
        // `raw` is still alive: only OnReleased frees it (this buffer's callback, Stop, Close or Rebuild).
        XAUDIO2_BUFFER b{};
        b.AudioBytes = static_cast<UINT32>(raw->data.size());
        b.pAudioData = raw->data.data();
        b.pContext = raw;
        if (FAILED(static_cast<IXAudio2SourceVoice*>(voice)->SubmitSourceBuffer(&b))) {
            // It will not play: release it right away so the game does not run out of buffers.
            std::lock_guard lk(impl_->m);
            impl_->OnReleased(sp, raw);
            impl_->want_rebuild.store(true);
        }
    }
#endif
    (void)sp;
    return true;
}

uint64_t Device::PopReleased(uint64_t key) {
    std::lock_guard lk(impl_->m);
    auto it = impl_->sessions.find(key);
    if (it == impl_->sessions.end() || it->second->released.empty()) return 0;
    const uint64_t c = it->second->released.front();
    it->second->released.pop_front();
    return c;
}

}  // namespace nnshim::audio
