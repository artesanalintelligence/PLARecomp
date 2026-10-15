// nnshim: adapters for nn::settings (3), nn::oe (19) and nn::account (24).
//
// settings/oe: language, operation mode and performance as config constants
// (docked by default), host window focus, capture/recording as no-ops.
// account: one fixed local user (Uid from config, by default suyu's profile one, so
// saves under <save_root> stay in the same folder) and the network account always
// "unavailable" (no network, like suyu's HLE).
//
// AsyncContext::GetSystemEvent is native for empty contexts (kernel event from
// pla_api 1.1). AsyncContext and AsyncNetworkServiceLicenseInfoContext constructors/destructors/
// assignments stay in the SDK (PASS) (user code with no IPC while the context is empty,
// which is what the native async functions leave).
#include <atomic>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "abi_common.hpp"
#include "nnshim/services.hpp"
#include "validate.hpp"
#include "os_event.hpp"

using namespace nnshim;

namespace {

// Public results (module 124 = account).
constexpr uint32_t kResultCancelledByUser = (1u << 9) | 124;          // 2124-0001
constexpr uint32_t kResultNetworkUnavailable = (59u << 9) | 124;      // 2124-0059 (no Internet)
constexpr uint32_t kResultNsaUnavailable = (200u << 9) | 124;         // 2124-0200 (network account unavailable)
constexpr uint64_t kHandleTag = 0x4E4E5348494D0001ull;                // "NNSHIM" in UserHandle::ptr

struct State {
    std::string language;          // empty = the host's, reduced to the game's languages
    bool docked = true;
    std::string focus = "always";  // "always" | "window"
    uint64_t uid[2] = {0x3ECA2F644C7C2144ull, 0x155E1F13CFBBD8C8ull};
    std::string nickname = "Player";
    std::atomic<int> last_focus{1};
    std::atomic<int> pending_focus_msg{0};
    // v1.5 stage 5: "applet.hle_backing" / "account.hle_backing" (default: the global "hle_backing")
    // decide whether oe::Initialize / account::Initialize also run in the HLE. oe is still needed
    // while vi (display) belongs to the HLE: it creates the layer with ISelfController (V2 will drop it).
    bool backing_applet = true;
    bool backing_account = true;
    std::atomic<bool> account_hle_inited{false};
} g;

uint64_t ParseHex64(const std::string& s, size_t pos) {
    return std::strtoull(s.substr(pos, 16).c_str(), nullptr, 16);
}

void InitApplet(const cfg::Value& c) {
    g.language = c.GetString("settings.language");
    g.docked = c.GetBool("oe.docked", true);
    g.focus = c.GetString("oe.focus", "always");
    const std::string uid = c.GetString("account.uid");
    if (uid.size() == 32) {
        g.uid[0] = ParseHex64(uid, 0);
        g.uid[1] = ParseHex64(uid, 16);
    }
    g.nickname = c.GetString("account.nickname", g.nickname);
    g.backing_applet = c.GetBool("applet.hle_backing", Runtime().hle_backing);
    g.backing_account = c.GetBool("account.hle_backing", Runtime().hle_backing);
    Log(PLA_LOG_INFO, "nnshim: settings language=%s, oe %s focus=%s, account uid=%016llX%016llX (%s)",
        settings::DesiredLanguage(g.language).c_str(), g.docked ? "docked" : "handheld", g.focus.c_str(),
        static_cast<unsigned long long>(g.uid[0]), static_cast<unsigned long long>(g.uid[1]), g.nickname.c_str());
}

ModuleHooks g_applet_module{"applet", &InitApplet, nullptr};

pla_action BackingApplet() { return g.backing_applet ? PLA_CONTINUE : PLA_SKIP; }

int HostFocus() {
#ifdef _WIN32
    if (g.focus != "window") return 1;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() ? 1 : 2;   // InFocus / OutOfFocus
#else
    return 1;
#endif
}

bool OurHandle(PlaGuest& gg, uint64_t handle_va, uint64_t uid[2]) {
    uint64_t h[3] = {};
    if (gg.read(handle_va, h, sizeof h) != sizeof h || h[2] != kHandleTag) return false;
    if (uid) { uid[0] = h[0]; uid[1] = h[1]; }
    return true;
}

} // namespace

// =========================== settings =====================================================
// LanguageCode LanguageCode::Make(Language)   (the SDK asks the set service for the table)
NNSHIM_ADAPTER(Settings, nnshim_settings_LanguageCodeMake) {
    PlaGuest gg(ctx);
    const char* code = settings::CodeForLanguage(static_cast<int>(gg.w(0)));
    gg.ret(code ? settings::PackCode(code) : 0);
    return PLA_SKIP;
}

// bool operator==(const Language&, const LanguageCode&) and the reverse
static pla_action LangEq(PlaGuest& gg, uint64_t lang_va, uint64_t code_va) {
    int32_t lang = -1;
    gg.load(lang_va, lang);
    char code[9] = {};
    gg.read(code_va, code, 8);
    const char* c = settings::CodeForLanguage(lang);
    gg.ret(c && std::strncmp(c, code, 8) == 0 ? 1 : 0);
    return PLA_SKIP;
}
NNSHIM_ADAPTER(Settings, nnshim_settings_LanguageEqCode) { PlaGuest gg(ctx); return LangEq(gg, gg.x(0), gg.x(1)); }
NNSHIM_ADAPTER(Settings, nnshim_settings_CodeEqLanguage) { PlaGuest gg(ctx); return LangEq(gg, gg.x(1), gg.x(0)); }

// =========================== oe ===========================================================
// void Initialize()
NNSHIM_ADAPTER(Applet, nnshim_oe_Initialize) { return BackingApplet(); }

// LanguageCode GetDesiredLanguage()
NNSHIM_ADAPTER(Applet, nnshim_oe_GetDesiredLanguage) {
    PlaGuest gg(ctx);
    gg.ret(settings::PackCode(settings::DesiredLanguage(g.language).c_str()));
    return PLA_SKIP;
}

// OperationMode GetOperationMode()   (0 = handheld, 1 = docked)
NNSHIM_ADAPTER(Applet, nnshim_oe_GetOperationMode) {
    PlaGuest gg(ctx);
    gg.ret(g.docked ? 1 : 0);
    return PLA_SKIP;
}

// PerformanceMode GetPerformanceMode()   (0 = normal, 1 = boost/docked)
NNSHIM_ADAPTER(Applet, nnshim_oe_GetPerformanceMode) {
    PlaGuest gg(ctx);
    gg.ret(g.docked ? 1 : 0);
    return PLA_SKIP;
}

// FocusState GetCurrentFocusState()   (1 = in focus, 2 = out of focus, 3 = background)
NNSHIM_ADAPTER(Applet, nnshim_oe_GetCurrentFocusState) {
    PlaGuest gg(ctx);
    const int f = HostFocus();
    if (f != g.last_focus.exchange(f)) g.pending_focus_msg = 1;
    gg.ret(static_cast<uint32_t>(f));
    return PLA_SKIP;
}

// bool TryPopNotificationMessage(Message* out)   (0xF = FocusStateChanged)
NNSHIM_ADAPTER(Applet, nnshim_oe_TryPopNotificationMessage) {
    PlaGuest gg(ctx);
    const int f = HostFocus();
    if (f != g.last_focus.exchange(f)) g.pending_focus_msg = 1;
    if (g.pending_focus_msg.exchange(0)) {
        gg.store<uint32_t>(gg.x(0), 0xF);
        gg.ret(1);
    } else {
        gg.ret(0);
    }
    return PLA_SKIP;
}

// Configuration functions with no effect on the host (void or no relevant result).
#define OE_NOP(NAME)                                    \
    NNSHIM_ADAPTER(Applet, NAME) { return PLA_SKIP; }
OE_NOP(nnshim_oe_EnableRecording)
OE_NOP(nnshim_oe_DisableRecording)
OE_NOP(nnshim_oe_EnableScreenShot)
OE_NOP(nnshim_oe_DisableScreenShot)
OE_NOP(nnshim_oe_SetCpuBoostMode)
OE_NOP(nnshim_oe_FinishStartupLogo)
OE_NOP(nnshim_oe_SetFocusHandlingMode)
OE_NOP(nnshim_oe_SetPerformanceConfiguration)
OE_NOP(nnshim_oe_EnterExitRequestHandlingSection)
OE_NOP(nnshim_oe_LeaveExitRequestHandlingSection)
OE_NOP(nnshim_oe_SetUserInactivityDetectionTimeExtended)
OE_NOP(nnshim_oe_SetOperationModeChangedNotificationEnabled)
OE_NOP(nnshim_oe_SetPerformanceModeChangedNotificationEnabled)

// =========================== account ======================================================
// void Initialize()
NNSHIM_ADAPTER(Account, nnshim_account_Initialize) {
    if (g.backing_account) {
        g.account_hle_inited = true;
        return PLA_CONTINUE;
    }
    return PLA_SKIP;
}

// Result GetUserCount(int* out) and Result ListOpenUsers(int* outCount, Uid* out, int max): without the HLE
// behind it (account.hle_backing=false) the SDK would have no session; the shim answers with its only user.
NNSHIM_ADAPTER(Account, nnshim_account_GetUserCount) {
    if (g.account_hle_inited) return PLA_CONTINUE;
    PlaGuest gg(ctx);
    if (gg.x(0)) gg.store<int32_t>(gg.x(0), 1);
    gg.ret(0);
    return PLA_SKIP;
}
NNSHIM_ADAPTER(Account, nnshim_account_ListOpenUsers) {
    if (g.account_hle_inited) return PLA_CONTINUE;
    PlaGuest gg(ctx);
    const int max = static_cast<int>(gg.w(2));
    int n = 0;
    if (max > 0 && gg.x(1)) {
        gg.write(gg.x(1), g.uid, sizeof g.uid);
        n = 1;
    }
    if (gg.x(0)) gg.store<int32_t>(gg.x(0), n);
    gg.ret(0);
    return PLA_SKIP;
}

// bool TryOpenPreselectedUser(UserHandle* out)   UserHandle = { Uid; void* ptr } (0x18)
NNSHIM_ADAPTER(Account, nnshim_account_TryOpenPreselectedUser) {
    PlaGuest gg(ctx);
    const uint64_t h[3] = {g.uid[0], g.uid[1], kHandleTag};
    gg.write(gg.x(0), h, sizeof h);
    gg.ret(1);
    return PLA_SKIP;
}

// Result GetUserId(Uid* out, const UserHandle&)
NNSHIM_ADAPTER(Account, nnshim_account_GetUserId) {
    PlaGuest gg(ctx);
    uint64_t uid[2];
    if (!OurHandle(gg, gg.x(1), uid)) return PLA_CONTINUE;
    gg.write(gg.x(0), uid, sizeof uid);
    gg.ret(0);
    return PLA_SKIP;
}

// Result GetNickname(Nickname* out, const Uid&)   Nickname = char[0x21]
NNSHIM_ADAPTER(Account, nnshim_account_GetNickname) {
    PlaGuest gg(ctx);
    char name[0x21] = {};
    std::strncpy(name, g.nickname.c_str(), 0x20);
    gg.write(gg.x(0), name, sizeof name);
    gg.ret(0);
    return PLA_SKIP;
}

// Result GetUserExistence(bool* out, const Uid&)
NNSHIM_ADAPTER(Account, nnshim_account_GetUserExistence) {
    PlaGuest gg(ctx);
    uint64_t uid[2] = {};
    gg.read(gg.x(1), uid, sizeof uid);
    gg.store<uint8_t>(gg.x(0), uid[0] == g.uid[0] && uid[1] == g.uid[1] ? 1 : 0);
    gg.ret(0);
    return PLA_SKIP;
}

// Result IsNetworkServiceAccountAvailable(bool* out, const UserHandle&)
NNSHIM_ADAPTER(Account, nnshim_account_IsNetworkServiceAccountAvailable) {
    PlaGuest gg(ctx);
    if (!OurHandle(gg, gg.x(1), nullptr)) return PLA_CONTINUE;
    gg.store<uint8_t>(gg.x(0), 0);
    gg.ret(0);
    return PLA_SKIP;
}

// Result EnsureNetworkServiceAccountAvailable(const UserHandle&)   (account linking applet)
NNSHIM_ADAPTER(Account, nnshim_account_EnsureNetworkServiceAccountAvailable) {
    PlaGuest gg(ctx);
    if (!OurHandle(gg, gg.x(0), nullptr)) return PLA_CONTINUE;
    gg.ret(kResultCancelledByUser);
    return PLA_SKIP;
}

// Result ShowLicenseRequirementsForNetworkService(const UserHandle&)   (applet)
NNSHIM_ADAPTER(Account, nnshim_account_ShowLicenseRequirementsForNetworkService) {
    PlaGuest gg(ctx);
    if (!OurHandle(gg, gg.x(0), nullptr)) return PLA_CONTINUE;
    gg.ret(kResultCancelledByUser);
    return PLA_SKIP;
}

// Result GetNetworkServiceAccountId(NetworkServiceAccountId* out, const UserHandle&)
NNSHIM_ADAPTER(Account, nnshim_account_GetNetworkServiceAccountId) {
    PlaGuest gg(ctx);
    if (!OurHandle(gg, gg.x(1), nullptr)) return PLA_CONTINUE;
    gg.ret(kResultNsaUnavailable);
    return PLA_SKIP;
}

// Result LoadNetworkServiceAccountIdTokenCache(size_t* outSize, char* buf, size_t size, const UserHandle&)
NNSHIM_ADAPTER(Account, nnshim_account_LoadNetworkServiceAccountIdTokenCache) {
    PlaGuest gg(ctx);
    if (!OurHandle(gg, gg.x(3), nullptr)) return PLA_CONTINUE;
    gg.ret(kResultNsaUnavailable);
    return PLA_SKIP;
}

// Result EnsureNetworkServiceAccountIdTokenCacheAsync(AsyncContext*, const UserHandle&)
// Without Internet the SDK fails before creating the task (CheckInternetAvailability): same here.
NNSHIM_ADAPTER(Account, nnshim_account_EnsureNetworkServiceAccountIdTokenCacheAsync) {
    PlaGuest gg(ctx);
    if (!OurHandle(gg, gg.x(1), nullptr)) return PLA_CONTINUE;
    gg.ret(kResultNetworkUnavailable);
    return PLA_SKIP;
}

// Result LoadNetworkServiceLicenseInfoAsync(AsyncNetworkServiceLicenseInfoContext*, const UserHandle&)
NNSHIM_ADAPTER(Account, nnshim_account_LoadNetworkServiceLicenseInfoAsync) {
    PlaGuest gg(ctx);
    if (!OurHandle(gg, gg.x(1), nullptr)) return PLA_CONTINUE;
    gg.ret(kResultNetworkUnavailable);
    return PLA_SKIP;
}

// AsyncContext = { vtable; Uid; IAsyncContext* obj (+0x18) }. With a null obj (all the
// native functions leave) the SDK would crash dereferencing it: the shim answers
// "done, no network". With a real obj (created by the HLE), go to the original.
static bool EmptyContext(PlaGuest& gg, uint64_t ctx_va) {
    uint64_t obj = 1;
    gg.load(ctx_va + 0x18, obj);
    return obj == 0;
}

// Result AsyncContext::HasDone(bool* out)
NNSHIM_ADAPTER(Account, nnshim_account_AsyncContextHasDone) {
    PlaGuest gg(ctx);
    if (!EmptyContext(gg, gg.x(0))) return PLA_CONTINUE;
    gg.store<uint8_t>(gg.x(1), 1);
    gg.ret(0);
    return PLA_SKIP;
}

// Result AsyncContext::GetResult()
NNSHIM_ADAPTER(Account, nnshim_account_AsyncContextGetResult) {
    PlaGuest gg(ctx);
    if (!EmptyContext(gg, gg.x(0))) return PLA_CONTINUE;
    gg.ret(kResultNetworkUnavailable);
    return PLA_SKIP;
}

// Result AsyncContext::GetSystemEvent(os::SystemEvent* out)
// Empty context (task the shim rejected: no network): an already signaled event (the task
// "finished"), with EventClearMode ManualClear and the handle managed by the game, like the SDK
// (sdk+0x4844). Requires pla_api 1.1; without it, go to the SDK.
NNSHIM_ADAPTER(Account, nnshim_account_AsyncContextGetSystemEvent) {
    PlaGuest gg(ctx);
    if (!EmptyContext(gg, gg.x(0))) return PLA_CONTINUE;
    const uint64_t ev_va = gg.x(1);
    if (!ev_va) return PLA_CONTINUE;
    const uint32_t h = osev::NewGuestEvent("nnshim:account:async", true, nullptr);
    if (!h || !osev::AttachReadableHandle(gg, ev_va, h, true, osev::kClearManual)) return PLA_CONTINUE;
    gg.ret(0);
    return PLA_SKIP;
}

// Result AsyncContext::Cancel()
NNSHIM_ADAPTER(Account, nnshim_account_AsyncContextCancel) {
    PlaGuest gg(ctx);
    if (!EmptyContext(gg, gg.x(0))) return PLA_CONTINUE;
    gg.ret(0);
    return PLA_SKIP;
}

// NetworkServiceLicenseKind AsyncNetworkServiceLicenseInfoContext::GetNetworkServiceLicenseKind()
NNSHIM_ADAPTER(Account, nnshim_account_GetNetworkServiceLicenseKind) {
    PlaGuest gg(ctx);
    if (!EmptyContext(gg, gg.x(0))) return PLA_CONTINUE;
    gg.ret(0);   // None
    return PLA_SKIP;
}

// =========================== A/B validation (observe mode, gen_mod_json.py --validate) =====
// The post hook compares what the HLE returned with what the shim would have returned (see validate.hpp).
// Expected config-driven differences: language (settings.language vs. suyu's profile),
// mode (oe.docked vs. use_docked_mode) and user (account.uid/nickname vs. the profile):
// for a clean A/B, set those values equal to the profile's.
namespace {
std::string CodeStr(uint64_t v) { return settings::UnpackCode(v); }
std::string UidStr(const uint64_t u[2]) {
    return Fmt("%016llX%016llX", static_cast<unsigned long long>(u[0]), static_cast<unsigned long long>(u[1]));
}
} // namespace

NNSHIM_OBSERVER(nnshim_settings_LanguageCodeMake) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0) & 0xffffffffu; }
NNSHIM_EXPORT void nnshim_settings_LanguageCodeMake_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    const int lang = static_cast<int>(gg.call_data()[0]);
    const char* code = settings::CodeForLanguage(lang);
    const uint64_t shim = code ? settings::PackCode(code) : 0;
    ValidateResult("settings.LanguageCodeMake", gg.x(0) == shim, Fmt("%d -> %s", lang, CodeStr(gg.x(0)).c_str()),
                   CodeStr(shim));
}

static void ObserveLangEq(PlaGuest& gg, uint64_t lang_va, uint64_t code_va) {
    int32_t lang = -1;
    gg.load(lang_va, lang);
    uint64_t code = 0;
    gg.load(code_va, code);
    uint64_t* cd = gg.call_data();
    cd[0] = static_cast<uint32_t>(lang);
    cd[1] = code;
}
static void PostLangEq(PlaGuest& gg, const char* name) {
    const uint64_t* cd = gg.call_data();
    const char* c = settings::CodeForLanguage(static_cast<int32_t>(cd[0]));
    const std::string code = CodeStr(cd[1]);
    const uint32_t shim = c && code == c ? 1 : 0;
    const uint32_t hle = gg.w(0) & 0xff;
    ValidateResult(name, hle == shim, Fmt("%d==%s -> %u", static_cast<int32_t>(cd[0]), code.c_str(), hle),
                   Fmt("%u", shim));
}
NNSHIM_OBSERVER(nnshim_settings_LanguageEqCode) { PlaGuest gg(ctx); ObserveLangEq(gg, gg.x(0), gg.x(1)); }
NNSHIM_EXPORT void nnshim_settings_LanguageEqCode_post(pla_ctx* ctx) { PlaGuest gg(ctx); PostLangEq(gg, "settings.LanguageEqCode"); }
NNSHIM_OBSERVER(nnshim_settings_CodeEqLanguage) { PlaGuest gg(ctx); ObserveLangEq(gg, gg.x(1), gg.x(0)); }
NNSHIM_EXPORT void nnshim_settings_CodeEqLanguage_post(pla_ctx* ctx) { PlaGuest gg(ctx); PostLangEq(gg, "settings.CodeEqLanguage"); }

NNSHIM_EXPORT void nnshim_oe_GetDesiredLanguage_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    const uint64_t shim = settings::PackCode(settings::DesiredLanguage(g.language).c_str());
    ValidateResult("oe.GetDesiredLanguage", gg.x(0) == shim, CodeStr(gg.x(0)), CodeStr(shim));
}
NNSHIM_EXPORT void nnshim_oe_GetOperationMode_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    const uint32_t shim = g.docked ? 1 : 0;
    ValidateResult("oe.GetOperationMode", (gg.w(0) & 0xff) == shim, Fmt("%u", gg.w(0) & 0xff), Fmt("%u", shim));
}
NNSHIM_EXPORT void nnshim_oe_GetPerformanceMode_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    const uint32_t shim = g.docked ? 1 : 0;
    ValidateResult("oe.GetPerformanceMode", gg.w(0) == shim, Fmt("%u", gg.w(0)), Fmt("%u", shim));
}
NNSHIM_EXPORT void nnshim_oe_GetCurrentFocusState_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    const uint32_t shim = static_cast<uint32_t>(HostFocus());
    ValidateResult("oe.GetCurrentFocusState", (gg.w(0) & 0xff) == shim, Fmt("%u", gg.w(0) & 0xff), Fmt("%u", shim));
}

NNSHIM_OBSERVER(nnshim_account_TryOpenPreselectedUser) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }
NNSHIM_EXPORT void nnshim_account_TryOpenPreselectedUser_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    uint64_t uid[2] = {};
    gg.read(gg.call_data()[0], uid, sizeof uid);
    const bool eq = (gg.w(0) & 0xff) == 1 && uid[0] == g.uid[0] && uid[1] == g.uid[1];
    ValidateResult("account.TryOpenPreselectedUser", eq, Fmt("%u %s", gg.w(0) & 0xff, UidStr(uid).c_str()),
                   "1 " + UidStr(g.uid));
}

NNSHIM_OBSERVER(nnshim_account_GetUserId) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }
NNSHIM_EXPORT void nnshim_account_GetUserId_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    uint64_t uid[2] = {};
    gg.read(gg.call_data()[0], uid, sizeof uid);
    const bool eq = gg.w(0) == 0 && uid[0] == g.uid[0] && uid[1] == g.uid[1];
    ValidateResult("account.GetUserId", eq, Fmt("r=%#x %s", gg.w(0), UidStr(uid).c_str()), UidStr(g.uid));
}

NNSHIM_OBSERVER(nnshim_account_GetNickname) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }
NNSHIM_EXPORT void nnshim_account_GetNickname_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    char name[0x22] = {};
    gg.read(gg.call_data()[0], name, 0x21);
    ValidateResult("account.GetNickname", gg.w(0) == 0 && g.nickname == name, Fmt("r=%#x %s", gg.w(0), name),
                   g.nickname);
}

NNSHIM_OBSERVER(nnshim_account_GetUserExistence) {
    PlaGuest gg(ctx);
    uint64_t* cd = gg.call_data();
    cd[0] = gg.x(0);
    uint64_t uid[2] = {};
    gg.read(gg.x(1), uid, sizeof uid);
    cd[1] = uid[0];
    cd[2] = uid[1];
}
NNSHIM_EXPORT void nnshim_account_GetUserExistence_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    const uint64_t* cd = gg.call_data();
    uint8_t hle = 0;
    gg.load(cd[0], hle);
    // The shim answers for ITS user; if the game asks about the HLE profile's user, the
    // expected answer matches the HLE's (1) only if the UIDs match: compare that way.
    const uint8_t shim = cd[1] == g.uid[0] && cd[2] == g.uid[1] ? 1 : 0;
    ValidateResult("account.GetUserExistence", gg.w(0) == 0 && hle == shim, Fmt("r=%#x %u", gg.w(0), hle),
                   Fmt("%u", shim));
}

NNSHIM_OBSERVER(nnshim_account_IsNetworkServiceAccountAvailable) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }
NNSHIM_EXPORT void nnshim_account_IsNetworkServiceAccountAvailable_post(pla_ctx* ctx) {
    PlaGuest gg(ctx);
    uint8_t hle = 0xff;
    gg.load(gg.call_data()[0], hle);
    ValidateResult("account.IsNetworkServiceAccountAvailable", gg.w(0) == 0 && hle == 0,
                   Fmt("r=%#x %u", gg.w(0), hle), "0");
}
