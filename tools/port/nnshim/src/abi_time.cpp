// nnshim: adapters for nn::time (12 imports): host wall clock and time zone.
//
// os::GetSystemTick (frame pacing) stays with the guest; time::* is date and time.
// Initialize/Finalize: with "hle_backing" (v1, default) the shim keeps its own state and also
// lets the SDK initialization run, so SDK-internal code that uses the time service on its
// own finds it ready. With hle_backing=false (v1.5) they are purely native.
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "abi_common.hpp"
#include "nnshim/services.hpp"
#include "validate.hpp"

using namespace nnshim;

namespace {

std::atomic<int> g_init_count{0};
bool g_backing = true;   // "time.hle_backing" (default: the global "hle_backing")

void InitTime(const cfg::Value& c) {
    time::Config tc;
    g_backing = c.GetBool("time.hle_backing", Runtime().hle_backing);
    tc.timezone = c.GetString("time.timezone", tc.timezone);
    tc.offset_seconds = c.GetInt("time.offset_seconds", 0);
    tc.location_name = c.GetString("time.location_name");
    time::Configure(tc);
    g_init_count = 0;
    Log(PLA_LOG_INFO, "nnshim: time zone=%s location=%s offset=%lld s", tc.timezone.c_str(),
        time::DeviceLocationName().c_str(), static_cast<long long>(tc.offset_seconds));
}

ModuleHooks g_time_module{"time", &InitTime, nullptr};

pla_action Backing() { return g_backing ? PLA_CONTINUE : PLA_SKIP; }

} // namespace

// Result nn::time::Initialize()
NNSHIM_ADAPTER(Time, nnshim_time_Initialize) {
    PlaGuest g(ctx);
    g_init_count.fetch_add(1);
    g.ret(0);
    return Backing();
}

// bool nn::time::IsInitialized()
NNSHIM_ADAPTER(Time, nnshim_time_IsInitialized) {
    PlaGuest g(ctx);
    g.ret(g_init_count.load() > 0 ? 1 : 0);
    return PLA_SKIP;
}

// Result nn::time::Finalize()
NNSHIM_ADAPTER(Time, nnshim_time_Finalize) {
    PlaGuest g(ctx);
    if (g_init_count.load() > 0) g_init_count.fetch_sub(1);
    g.ret(0);
    return Backing();
}

// Result StandardUserSystemClock::GetCurrentTime(PosixTime* out)
NNSHIM_ADAPTER(Time, nnshim_time_UserGetCurrentTime) {
    PlaGuest g(ctx);
    g.store<int64_t>(g.x(0), time::Now());
    g.ret(0);
    return PLA_SKIP;
}

// Result StandardNetworkSystemClock::GetCurrentTime(PosixTime* out): the network clock is the
// same host clock (assumed to be synced by the OS).
NNSHIM_ADAPTER(Time, nnshim_time_NetworkGetCurrentTime) {
    PlaGuest g(ctx);
    g.store<int64_t>(g.x(0), time::Now());
    g.ret(0);
    return PLA_SKIP;
}

// Result ToCalendarTime(CalendarTime* out, CalendarAdditionalInfo* info, const PosixTime& t)
NNSHIM_ADAPTER(Time, nnshim_time_ToCalendarTime) {
    PlaGuest g(ctx);
    int64_t t = 0;
    g.load(g.x(2), t);
    time::CalendarTime cal{};
    time::CalendarAdditionalInfo info{};
    time::ToCalendar(t, nullptr, cal, info);
    if (g.x(0)) g.store(g.x(0), cal);
    if (g.x(1)) g.store(g.x(1), info);
    g.ret(0);
    return PLA_SKIP;
}

// CalendarTime ToCalendarTimeInUtc(const PosixTime& t)   (8 bytes, returned in X0)
NNSHIM_ADAPTER(Time, nnshim_time_ToCalendarTimeInUtc) {
    PlaGuest g(ctx);
    int64_t t = 0;
    g.load(g.x(0), t);
    time::CalendarTime cal{};
    time::CalendarAdditionalInfo info{};
    time::ToCalendarUtc(t, cal, info);
    uint64_t v = 0;
    std::memcpy(&v, &cal, 8);
    g.ret(v);
    return PLA_SKIP;
}

// Result ToPosixTime(int* outCount, PosixTime* out, int outMax, const CalendarTime& c, const TimeZoneRule& r)
NNSHIM_ADAPTER(Time, nnshim_time_ToPosixTime) {
    PlaGuest g(ctx);
    time::CalendarTime cal{};
    g.load(g.x(3), cal);
    time::ShimRule rule{};
    g.load(g.x(4), rule);
    int64_t out[2] = {};
    const int n = time::ToPosix(cal, time::IsShimRule(rule) ? &rule : nullptr, out);
    const int max = static_cast<int>(g.w(2));
    const int k = n < max ? n : max;
    for (int i = 0; i < k; ++i) g.store<int64_t>(g.x(1) + static_cast<uint64_t>(i) * 8, out[i]);
    if (g.x(0)) g.store<int32_t>(g.x(0), k);
    g.ret(0);
    return PLA_SKIP;
}

// Result LoadTimeZoneRule(TimeZoneRule* out, const LocationName& name)
NNSHIM_ADAPTER(Time, nnshim_time_LoadTimeZoneRule) {
    PlaGuest g(ctx);
    char name[0x25] = {};
    g.read(g.x(1), name, 0x24);
    time::ShimRule rule{};
    time::MakeRule(rule, name);
    g.store(g.x(0), rule);
    g.ret(0);
    return PLA_SKIP;
}

// Result GetDeviceLocationName(LocationName* out)
NNSHIM_ADAPTER(Time, nnshim_time_GetDeviceLocationName) {
    PlaGuest g(ctx);
    char name[0x24] = {};
    const std::string loc = time::DeviceLocationName();
    std::strncpy(name, loc.c_str(), sizeof name - 1);
    g.write(g.x(0), name, sizeof name);
    g.ret(0);
    return PLA_SKIP;
}

// ClockSnapshot::ClockSnapshot()
NNSHIM_ADAPTER(Time, nnshim_time_ClockSnapshotCtor) {
    PlaGuest g(ctx);
    time::ClockSnapshot s;
    time::DefaultSnapshot(s);
    g.store(g.x(0), s);
    return PLA_SKIP;
}

// static Result ClockSnapshot::CreateWithStandardSystemClock(ClockSnapshot* out)
NNSHIM_ADAPTER(Time, nnshim_time_CreateWithStandardSystemClock) {
    PlaGuest g(ctx);
    time::ClockSnapshot s;
    time::FillSnapshot(s);
    g.store(g.x(0), s);
    g.ret(0);
    return PLA_SKIP;
}

// =========================== A/B validation (observe mode, gen_mod_json.py --validate) =====
// Observers save the arguments (call_data: 4 values); the post hook compares what the
// HLE returned with what the shim would have returned. Clocks: 2 s tolerance.
namespace {

std::string CalStr(const time::CalendarTime& c) {
    return Fmt("%04d-%02d-%02d %02d:%02d:%02d", c.year, c.month, c.day, c.hour, c.minute, c.second);
}
bool CalEq(const time::CalendarTime& a, const time::CalendarTime& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour && a.minute == b.minute &&
           a.second == b.second;
}
std::string InfoStr(const time::CalendarAdditionalInfo& i) {
    char tz[9] = {};
    std::memcpy(tz, i.tz_name, 8);
    return Fmt("dow=%d doy=%d tz=%s dst=%d off=%d", i.day_of_week, i.day_of_year, tz, i.is_dst, i.utc_offset);
}
bool InfoEq(const time::CalendarAdditionalInfo& a, const time::CalendarAdditionalInfo& b) {
    return a.day_of_week == b.day_of_week && a.day_of_year == b.day_of_year && a.is_dst == b.is_dst &&
           a.utc_offset == b.utc_offset && std::memcmp(a.tz_name, b.tz_name, 8) == 0;
}
void ValidateClock(const char* name, PlaGuest& g) {
    const uint32_t r = g.w(0);
    int64_t hle = 0;
    if (r == 0) g.load(g.call_data()[0], hle);
    const int64_t shim = time::Now();
    const int64_t d = hle - shim;
    ValidateResult(name, r == 0 && d >= -2 && d <= 2, Fmt("r=%#x t=%lld", r, static_cast<long long>(hle)),
                   Fmt("t=%lld (diff %lld s)", static_cast<long long>(shim), static_cast<long long>(d)));
}

} // namespace

NNSHIM_OBSERVER(nnshim_time_UserGetCurrentTime) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }
NNSHIM_EXPORT void nnshim_time_UserGetCurrentTime_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    ValidateClock("time.UserGetCurrentTime", g);
}

NNSHIM_OBSERVER(nnshim_time_NetworkGetCurrentTime) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }
NNSHIM_EXPORT void nnshim_time_NetworkGetCurrentTime_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    ValidateClock("time.NetworkGetCurrentTime", g);
}

NNSHIM_OBSERVER(nnshim_time_ToCalendarTime) {
    PlaGuest g(ctx);
    uint64_t* cd = g.call_data();
    cd[0] = g.x(0);
    cd[1] = g.x(1);
    int64_t t = 0;
    g.load(g.x(2), t);
    cd[2] = static_cast<uint64_t>(t);
}
NNSHIM_EXPORT void nnshim_time_ToCalendarTime_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t* cd = g.call_data();
    time::CalendarTime hc{}, sc{};
    time::CalendarAdditionalInfo hi{}, si{};
    if (cd[0]) g.load(cd[0], hc);
    if (cd[1]) g.load(cd[1], hi);
    time::ToCalendar(static_cast<int64_t>(cd[2]), nullptr, sc, si);
    const bool eq = g.w(0) == 0 && (!cd[0] || CalEq(hc, sc)) && (!cd[1] || InfoEq(hi, si));
    ValidateResult("time.ToCalendarTime", eq, Fmt("r=%#x %s %s", g.w(0), CalStr(hc).c_str(), InfoStr(hi).c_str()),
                   CalStr(sc) + " " + InfoStr(si));
}

NNSHIM_OBSERVER(nnshim_time_ToCalendarTimeInUtc) {
    PlaGuest g(ctx);
    int64_t t = 0;
    g.load(g.x(0), t);
    g.call_data()[0] = static_cast<uint64_t>(t);
}
NNSHIM_EXPORT void nnshim_time_ToCalendarTimeInUtc_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t v = g.x(0);
    time::CalendarTime hc{}, sc{};
    std::memcpy(&hc, &v, 8);
    time::CalendarAdditionalInfo si{};
    time::ToCalendarUtc(static_cast<int64_t>(g.call_data()[0]), sc, si);
    ValidateResult("time.ToCalendarTimeInUtc", CalEq(hc, sc), CalStr(hc), CalStr(sc));
}

// The rule the HLE loads is opaque: LoadTimeZoneRule remembers which location was loaded at each
// address, and ToPosixTime uses it to compute the shim's answer with the same location.
namespace {
std::mutex g_rule_mutex;
std::map<uint64_t, std::string> g_rule_names;
} // namespace

NNSHIM_OBSERVER(nnshim_time_LoadTimeZoneRule) {
    PlaGuest g(ctx);
    char name[0x25] = {};
    g.read(g.x(1), name, 0x24);
    std::lock_guard lk(g_rule_mutex);
    g_rule_names[g.x(0)] = name;
}

NNSHIM_OBSERVER(nnshim_time_ToPosixTime) {
    PlaGuest g(ctx);
    uint64_t* cd = g.call_data();
    cd[0] = g.x(0);
    cd[1] = g.x(1);
    cd[2] = (g.x(4) & 0x00FFFFFFFFFFFFFFull) | (static_cast<uint64_t>(g.w(2) & 0xff) << 56);   // rule | max
    time::CalendarTime c{};
    g.load(g.x(3), c);
    std::memcpy(&cd[3], &c, 8);
}
NNSHIM_EXPORT void nnshim_time_ToPosixTime_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t* cd = g.call_data();
    time::CalendarTime c{};
    std::memcpy(&c, &cd[3], 8);
    int32_t hn = 0;
    int64_t hv[2] = {};
    if (cd[0]) g.load(cd[0], hn);
    const int max = static_cast<int>(cd[2] >> 56);
    const uint64_t rule_va = cd[2] & 0x00FFFFFFFFFFFFFFull;
    for (int i = 0; i < hn && i < 2 && i < max; ++i) g.load(cd[1] + static_cast<uint64_t>(i) * 8, hv[i]);
    std::string loc;
    {
        std::lock_guard lk(g_rule_mutex);
        if (auto it = g_rule_names.find(rule_va); it != g_rule_names.end()) loc = it->second;
    }
    time::ShimRule rule{};
    if (!loc.empty()) time::MakeRule(rule, loc);
    int64_t sv[2] = {};
    int sn = time::ToPosix(c, loc.empty() ? nullptr : &rule, sv);
    if (sn > max) sn = max;
    bool eq = g.w(0) == 0 && hn == sn;
    for (int i = 0; eq && i < sn && i < 2; ++i) eq = hv[i] == sv[i];
    ValidateResult("time.ToPosixTime", eq,
                   Fmt("r=%#x n=%d %lld %lld (%s, rule %s)", g.w(0), hn, static_cast<long long>(hv[0]),
                       static_cast<long long>(hv[1]), CalStr(c).c_str(), loc.empty() ? "?" : loc.c_str()),
                   Fmt("n=%d %lld %lld", sn, static_cast<long long>(sv[0]), static_cast<long long>(sv[1])));
}

NNSHIM_OBSERVER(nnshim_time_GetDeviceLocationName) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }
NNSHIM_EXPORT void nnshim_time_GetDeviceLocationName_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    char name[0x25] = {};
    g.read(g.call_data()[0], name, 0x24);
    const std::string shim = time::DeviceLocationName();
    ValidateResult("time.GetDeviceLocationName", g.w(0) == 0 && shim == name, Fmt("r=%#x %s", g.w(0), name), shim);
}

NNSHIM_OBSERVER(nnshim_time_CreateWithStandardSystemClock) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }
NNSHIM_EXPORT void nnshim_time_CreateWithStandardSystemClock_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    time::ClockSnapshot h{}, s{};
    g.load(g.call_data()[0], h);
    time::FillSnapshot(s);
    const int64_t d = h.user_time - s.user_time;
    char hl[0x25] = {}, sl[0x25] = {};
    std::memcpy(hl, h.location_name, 0x24);
    std::memcpy(sl, s.location_name, 0x24);
    const bool eq = g.w(0) == 0 && d >= -2 && d <= 2 && std::strcmp(hl, sl) == 0 &&
                    h.user_info.utc_offset == s.user_info.utc_offset && h.type == s.type;
    ValidateResult("time.ClockSnapshot", eq,
                   Fmt("r=%#x user=%lld loc=%s off=%d type=%u", g.w(0), static_cast<long long>(h.user_time), hl,
                       h.user_info.utc_offset, h.type),
                   Fmt("user=%lld loc=%s off=%d type=%u", static_cast<long long>(s.user_time), sl,
                       s.user_info.utc_offset, s.type));
}
