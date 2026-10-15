// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// sm and the startup services the SDK still talks to with nnshim + nvnvk loaded: am (applet
// proxy), vi (display, layer and the binder of the native window), hid (applet resource),
// audren (device names), fsp-srv/lm/ectx (session setup only). Command numbers and layouts
// are the documented Horizon IPC interfaces (switchbrew).

#include "services.h"

#include <deque>
#include <mutex>

#include "ipc.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"

namespace plahost::services {
namespace {

using ipc::Request;
using ipc::Response;
using ipc::Service;

template <typename T, typename... A>
std::shared_ptr<Service> Make(A&&... a) {
    return std::make_shared<T>(std::forward<A>(a)...);
}

// A service object every command of which returns a sub-object (generic "Get*" interfaces).
class Stub final : public Service {
public:
    using Service::Service;
};

// ---- am ----

std::mutex g_applet_lock;
std::deque<u32> g_messages;
std::shared_ptr<kernel::Event> g_message_event;
std::shared_ptr<kernel::Event> g_misc_event;
constexpr u64 kAruid = 0x51;

std::shared_ptr<kernel::Event> MessageEvent() {
    std::scoped_lock lk{g_applet_lock};
    if (!g_message_event) g_message_event = kernel::NewEvent("am:message");
    return g_message_event;
}

std::shared_ptr<kernel::Event> MiscEvent(const char* name) {
    return kernel::NewEvent(name);
}

class CommonStateGetter final : public Service {
public:
    CommonStateGetter() : Service("ICommonStateGetter") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 0: rs.PushCopy(kernel::ReadableHandle(MessageEvent())); return;
        case 1: {
            std::scoped_lock lk{g_applet_lock};
            if (g_messages.empty()) {
                rs.result = 0x680; // am: no message
                if (g_message_event) kernel::Clear(*g_message_event);
                return;
            }
            rs.Push<u32>(g_messages.front());
            g_messages.pop_front();
            if (g_messages.empty() && g_message_event) kernel::Clear(*g_message_event);
            return;
        }
        case 5: rs.Push<u8>(1); return;  // GetOperationMode: console (docked)
        case 6: rs.Push<u32>(1); return; // GetPerformanceMode: boost
        case 8: rs.Push<u8>(0); return;  // GetBootMode
        case 9: rs.Push<u8>(1); return;  // GetCurrentFocusState: in focus
        case 13: rs.PushCopy(kernel::ReadableHandle(MiscEvent("am:sleeplock"))); return;
        case 50: rs.Push<u8>(0); return; // IsVrModeEnabled
        case 60: rs.Push<u32>(1920); rs.Push<u32>(1080); return; // GetDefaultDisplayResolution
        case 61: rs.PushCopy(kernel::ReadableHandle(MiscEvent("am:resolution"))); return;
        case 900: case 10: case 11: case 12: case 20: case 51: case 52: case 53: case 54:
            return;
        }
        Unknown(rq, rs);
    }
};

class SelfController final : public Service {
public:
    SelfController() : Service("ISelfController") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 40: rs.Push<u64>(1); return;               // CreateManagedDisplayLayer
        case 44: rs.Push<u64>(1); rs.Push<u64>(2); return; // CreateManagedDisplaySeparableLayer
        case 1: case 0: case 2: case 3: case 4: case 9: case 10: case 11: case 12: case 13: case 14: case 16:
        case 19: case 50: case 62: case 68: case 69: case 72: case 100: case 110: case 120:
            return;
        case 41: rs.Push<u64>(1); rs.Push<u64>(1); return; // IsSystemBufferSharingEnabled-ish
        case 63: rs.Push<u64>(0); return;
        case 90: rs.Push<u32>(0); return;
        case 91: rs.PushCopy(kernel::ReadableHandle(MiscEvent("am:suspended-tick"))); return;
        }
        Unknown(rq, rs);
    }
};

class WindowController final : public Service {
public:
    WindowController() : Service("IWindowController") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 1: rs.Push<u64>(kAruid); return;
        case 2: rs.Push<u64>(kAruid); return;
        case 10: case 11: case 12: return;
        }
        Unknown(rq, rs);
    }
};

class ApplicationFunctions final : public Service {
public:
    ApplicationFunctions() : Service("IApplicationFunctions") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 1: rs.result = 0x2A2; return;               // PopLaunchParameter: none
        case 21: rs.Push<u64>(0x0000006E652D7365ULL); return; // GetDesiredLanguage ("es-...") unused
        case 40: return;                                  // NotifyRunning
        case 130: rs.PushCopy(kernel::ReadableHandle(MiscEvent("am:gpu-error"))); return;
        case 50: rs.Push<u8>(0); return;
        case 66: case 67: case 68: case 90: case 100: case 101: case 102: case 120: case 121: case 122:
            return;
        }
        Unknown(rq, rs);
    }
};

class ApplicationProxy final : public Service {
public:
    ApplicationProxy() : Service("IApplicationProxy") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 0: rs.PushObject(Make<CommonStateGetter>()); return;
        case 1: rs.PushObject(Make<SelfController>()); return;
        case 2: rs.PushObject(Make<WindowController>()); return;
        case 3: rs.PushObject(Make<Stub>("IAudioController")); return;
        case 4: rs.PushObject(Make<Stub>("IDisplayController")); return;
        case 10: rs.PushObject(Make<Stub>("IProcessWindingController")); return;
        case 11: rs.PushObject(Make<Stub>("ILibraryAppletCreator")); return;
        case 20: rs.PushObject(Make<ApplicationFunctions>()); return;
        case 1000: rs.PushObject(Make<Stub>("IDebugFunctions")); return;
        }
        Unknown(rq, rs);
    }
};

class AppletOE final : public Service {
public:
    AppletOE() : Service("appletOE") {}
    void Handle(Request& rq, Response& rs) override {
        if (rq.cmd == 0) {
            rs.PushObject(Make<ApplicationProxy>());
            return;
        }
        Unknown(rq, rs);
    }
};

// ---- vi ----

struct ParcelOut {
    std::vector<u8> data;
    template <typename T>
    void Write(const T& v) {
        const size_t at = data.size();
        data.resize(at + AlignUp(sizeof(T), 4));
        std::memcpy(data.data() + at, &v, sizeof(T));
    }
    std::vector<u8> Serialize(size_t objects = 0) const {
        std::vector<u8> out(16 + data.size() + objects * 4);
        const u32 hdr[4] = {static_cast<u32>(data.size()), 16, static_cast<u32>(objects * 4),
                            static_cast<u32>(16 + data.size())};
        std::memcpy(out.data(), hdr, 16);
        std::memcpy(out.data() + 16, data.data(), data.size());
        return out;
    }
};

struct NativeWindowParcel {
    u32 magic = 2;
    u32 process_id = 1;
    u64 id = 0;
    u32 pad0[2]{};
    char dispdrv[8] = {'d', 'i', 's', 'p', 'd', 'r', 'v', '\0'};
    u32 pad1[2]{};
};
static_assert(sizeof(NativeWindowParcel) == 0x28);

constexpr s32 kBinderId = 1;
constexpr u32 kWidth = 1280, kHeight = 720;

class HosBinderDriver final : public Service {
public:
    HosBinderDriver() : Service("IHOSBinderDriver") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 0:   // TransactParcel
        case 3: { // TransactParcelAuto
            const s32 id = rq.Pop<s32>();
            const u32 code = rq.Pop<u32>();
            const auto in = rq.ReadBuffer(0);
            Transact(id, code, in, rq);
            return;
        }
        case 1: return; // AdjustRefcount
        case 2:         // GetNativeHandle
            if (!buffer_event_) buffer_event_ = kernel::NewEvent("vi:buffer");
            kernel::Signal(*buffer_event_);
            rs.PushCopy(kernel::ReadableHandle(buffer_event_));
            return;
        }
        Unknown(rq, rs);
    }

private:
    void Transact(s32 id, u32 code, const std::vector<u8>& in, Request& rq) {
        (void)id;
        // Input parcel: header, interface token (u32 strict-mode, u32 length, UTF-16), then the arguments.
        size_t pos = 0;
        if (in.size() >= 16) {
            u32 off;
            std::memcpy(&off, in.data() + 4, 4);
            pos = off;
            u32 len = 0;
            if (pos + 8 <= in.size()) std::memcpy(&len, in.data() + pos + 4, 4);
            pos += 8 + AlignUp((u64{len} + 1) * 2, 4);
        }
        const auto rd32 = [&]() -> u32 {
            u32 v = 0;
            if (pos + 4 <= in.size()) std::memcpy(&v, in.data() + pos, 4);
            pos += 4;
            return v;
        };
        ParcelOut out;
        s32 status = 0;
        switch (code) {
        case 10: { // Connect
            rd32(); // listener
            const u32 api = rd32();
            (void)api;
            const u32 q[4] = {kWidth, kHeight, 0, 0};
            out.Write(q);
            break;
        }
        case 14: // SetPreallocatedBuffer
        case 11: // Disconnect
        case 2:  // SetBufferCount
        case 8:  // CancelBuffer
        case 4:  // DetachBuffer
            break;
        case 9: { // Query
            const u32 what = rd32();
            s32 v = 0;
            switch (what) {
            case 0: v = kWidth; break;
            case 1: v = kHeight; break;
            case 2: v = 1; break; // RGBA8888
            case 3: v = 2; break; // min undequeued buffers
            default: v = 0; break;
            }
            out.Write(v);
            break;
        }
        case 3: { // DequeueBuffer
            out.Write<s32>(slot_);
            slot_ = (slot_ + 1) % 3;
            out.Write<u32>(1);         // flattened fence present
            out.Write<s64>(0x24);
            u32 fence[9]{};
            out.Write(fence);
            break;
        }
        case 7: { // QueueBuffer
            const u32 q[4] = {kWidth, kHeight, 0, 0};
            out.Write(q);
            break;
        }
        case 1: // RequestBuffer
            out.Write<u32>(0);
            break;
        default: {
            static std::mutex lk;
            std::scoped_lock l{lk};
            static std::vector<u32> seen;
            if (std::find(seen.begin(), seen.end(), code) == seen.end()) {
                seen.push_back(code);
                LOGW("vi", "binder transaction %u answered with an empty parcel", code);
            }
            break;
        }
        }
        out.Write(status);
        LOGI("vi", "binder %d transaction %u (in %zu bytes) -> %zu bytes, out buffer %llu", id, code, in.size(), out.data.size() + 16, static_cast<unsigned long long>(rq.OutBuffer(0).size));
        const auto bytes = out.Serialize();
        rq.WriteBuffer(bytes.data(), bytes.size(), 0);
    }

    std::shared_ptr<kernel::Event> buffer_event_;
    s32 slot_ = 0;
};

class ApplicationDisplayService final : public Service {
public:
    ApplicationDisplayService() : Service("IApplicationDisplayService") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 100: rs.PushObject(Make<HosBinderDriver>()); return;
        case 101: rs.PushObject(Make<Stub>("ISystemDisplayService")); return;
        case 102: rs.PushObject(Make<Stub>("IManagerDisplayService")); return;
        case 103: rs.PushObject(Make<HosBinderDriver>()); return;
        case 1000: { // ListDisplays
            struct Info { char name[0x40]; u8 has_limit; u8 pad[7]; u64 max_layers, width, height; } info{};
            std::strcpy(info.name, "Default");
            info.has_limit = 1;
            info.max_layers = 1;
            info.width = 1920;
            info.height = 1080;
            rq.WriteBuffer(&info, sizeof info, 0);
            rs.Push<u64>(1);
            return;
        }
        case 1010: case 1011: rs.Push<u64>(1); return; // OpenDisplay / OpenDefaultDisplay
        case 1020: return;
        case 1101: return;
        case 1102: rs.Push<u64>(1920); rs.Push<u64>(1080); return; // GetDisplayResolution
        case 2020:   // OpenLayer
        case 2030: { // CreateStrayLayer
            if (rq.cmd == 2030) rs.Push<u64>(1); // layer id
            ParcelOut p;
            NativeWindowParcel w;
            w.id = kBinderId;
            p.Write(w);
            const auto bytes = p.Serialize(1);
            rq.WriteBuffer(bytes.data(), bytes.size(), 0);
            rs.Push<u64>(bytes.size());
            return;
        }
        case 2021: case 2031: case 2101: return;
        case 2102: rs.Push<u64>(0); return;
        case 2450: rs.Push<u64>(kWidth * kHeight * 4); rs.Push<u64>(0x1000); return;
        case 5202: // GetDisplayVsyncEvent
            if (!vsync_) vsync_ = kernel::NewEvent("vi:vsync");
            rs.PushCopy(kernel::ReadableHandle(vsync_));
            return;
        }
        Unknown(rq, rs);
    }

private:
    std::shared_ptr<kernel::Event> vsync_;
};

class ViRoot final : public Service {
public:
    using Service::Service;
    void Handle(Request& rq, Response& rs) override {
        if (rq.cmd <= 3) {
            rs.PushObject(Make<ApplicationDisplayService>());
            return;
        }
        Unknown(rq, rs);
    }
};

// ---- hid ----

std::shared_ptr<kernel::SharedMemory> g_hid_shmem;

class AppletResource final : public Service {
public:
    AppletResource() : Service("IAppletResource") {}
    void Handle(Request& rq, Response& rs) override {
        if (rq.cmd == 0) {
            if (!g_hid_shmem) {
                g_hid_shmem = std::make_shared<kernel::SharedMemory>();
                g_hid_shmem->size = 0x40000;
                g_hid_shmem->backing.assign(0x40000, 0);
                g_hid_shmem->name = "hid";
            }
            rs.PushCopy(kernel::AddHandle(g_hid_shmem));
            return;
        }
        Unknown(rq, rs);
    }
};

class Hid final : public Service {
public:
    Hid() : Service("hid") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 0: rs.PushObject(Make<AppletResource>()); return;
        case 106: // AcquireNpadStyleSetUpdateEventHandle
        case 40:
            rs.PushCopy(kernel::ReadableHandle(kernel::NewEvent("hid:event")));
            return;
        case 203: rs.PushObject(Make<Stub>("IActiveVibrationDeviceList")); return;
        }
        // Setters (style sets, hold type, activation...) succeed with nothing to return.
        if (rq.cmd >= 1 && rq.cmd < 2000) {
            rs.data.assign(0x10, 0);
            return;
        }
        Unknown(rq, rs);
    }
};

// ---- audren ----

class AudioDevice final : public Service {
public:
    AudioDevice() : Service("IAudioDevice") {}
    void Handle(Request& rq, Response& rs) override {
        static const char* const kNames[] = {"AudioStereoJackOutput", "AudioBuiltInSpeakerOutput", "AudioTvOutput",
                                             "AudioUsbDeviceOutput"};
        switch (rq.cmd) {
        case 0:   // ListAudioDeviceName
        case 6: { // ListAudioDeviceNameAuto
            const auto out = rq.OutBuffer(0);
            const u32 n = static_cast<u32>(std::min<u64>(4, out.size / 0x100));
            std::vector<u8> buf(n * 0x100, 0);
            for (u32 i = 0; i < n; ++i) std::strcpy(reinterpret_cast<char*>(buf.data() + i * 0x100), kNames[i]);
            rq.WriteBuffer(buf.data(), buf.size(), 0);
            rs.Push<u32>(n);
            return;
        }
        case 3:    // GetActiveAudioDeviceName
        case 10: { // GetActiveAudioDeviceNameAuto
            char name[0x100]{};
            std::strcpy(name, "AudioTvOutput");
            rq.WriteBuffer(name, sizeof name, 0);
            return;
        }
        case 1: case 7: return;              // SetAudioDeviceOutputVolume(Auto)
        case 2: case 8: rs.Push<float>(1.0f); return; // GetAudioDeviceOutputVolume(Auto)
        case 5: rs.Push<u32>(2); return;     // GetActiveChannelCount
        case 4: case 11: case 12:            // Query*Event
            rs.PushCopy(kernel::ReadableHandle(kernel::NewEvent("audio:device")));
            return;
        case 13: rs.Push<u32>(2); return;
        }
        Unknown(rq, rs);
    }
};

class AudRen final : public Service {
public:
    AudRen() : Service("audren:u") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 0: rs.PushObject(Make<Stub>("IAudioRenderer")); return;
        case 1: rs.Push<u64>(0x4000); return; // GetWorkBufferSize
        case 2: case 4: rs.PushObject(Make<AudioDevice>()); return;
        }
        Unknown(rq, rs);
    }
};

// ---- fsp-srv / lm / ectx (session setup only; nnshim serves the file systems) ----

class Fsp final : public Service {
public:
    Fsp() : Service("fsp-srv") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 1: return;                         // SetCurrentProcess
        case 1005: rs.Push<u32>(0); return;     // GetGlobalAccessLogMode
        case 1006: return;                      // OutputAccessLogToSdCard
        }
        if (rq.cmd < 1000) { // Open*: an object nobody uses (logged once)
            static std::mutex lk;
            std::scoped_lock l{lk};
            LOGI("fsp", "command %u returns an unused object (the nnshim serves file systems)", rq.cmd);
            rs.PushObject(Make<Stub>("fsp-object"));
            return;
        }
        Unknown(rq, rs);
    }
};

class ObjectFactory final : public Service {
public:
    ObjectFactory(std::string name, std::string child) : Service(std::move(name)), child_(std::move(child)) {}
    void Handle(Request& rq, Response& rs) override {
        if (rq.cmd == 0) {
            rs.PushObject(Make<Stub>(child_));
            return;
        }
        Unknown(rq, rs);
    }

private:
    std::string child_;
};

class Sm final : public Service {
public:
    Sm() : Service("sm:") {}
    void Handle(Request& rq, Response& rs) override {
        switch (rq.cmd) {
        case 0: return; // RegisterClient
        case 1: {       // GetServiceHandle
            char name[9]{};
            const u64 raw = rq.Pop<u64>();
            std::memcpy(name, &raw, 8);
            rs.PushObject(ipc::GetService(name));
            return;
        }
        }
        Unknown(rq, rs);
    }
};

} // namespace

void InitApplet() {
    MessageEvent();
    PushAppletMessage(15); // FocusStateChanged (the app starts in focus)
}

void PushAppletMessage(unsigned message) {
    auto ev = MessageEvent();
    {
        std::scoped_lock lk{g_applet_lock};
        g_messages.push_back(message);
    }
    kernel::Signal(*ev);
}

} // namespace plahost::services

namespace plahost::ipc {

std::shared_ptr<Service> ConnectNamedPort(const std::string& name) {
    if (name == "sm:") {
        return std::make_shared<services::Sm>();
    }
    return nullptr;
}

std::shared_ptr<Service> GetService(const std::string& name) {
    using namespace services;
    LOGI("IPC", "sm GetService(%s)", name.c_str());
    if (name.rfind("nvdrv", 0) == 0) return MakeNvdrv(name);
    if (name == "vi:m" || name == "vi:s" || name == "vi:u") return std::make_shared<ViRoot>(name);
    if (name == "appletOE") return std::make_shared<AppletOE>();
    if (name == "hid") return std::make_shared<Hid>();
    if (name == "audren:u") return std::make_shared<AudRen>();
    if (name == "fsp-srv") return std::make_shared<Fsp>();
    if (name == "lm") return std::make_shared<ObjectFactory>("lm", "ILogger");
    if (name == "ectx:aw") return std::make_shared<ObjectFactory>("ectx:aw", "IContextRegistrar");
    LOGW("IPC", "service %s has no native implementation: stub session", name.c_str());
    return std::make_shared<services::Stub>(name);
}

} // namespace plahost::ipc
