// nnshim: native core for time, settings, diag (formatting and dialogs) and swkbd (Win32 window).
#include "nnshim/services.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace nnshim {

// =========================== time ==============================================================
namespace time {
namespace {

std::mutex g_mutex;
Config g_cfg;
int g_mode = 0;          // 0 host, 1 utc, 2 fixed
int32_t g_fixed = 0;

// Host UTC offset and DST for an instant (seconds).
// tzdata-style zone abbreviation for zones without a proper name ("-04", "+0530"), which is
// what the HLE (and the console) gives for America/Caracas and similar; 0 = "UTC". Zones with
// a lettered tzdata abbreviation (EST, CET...) cannot be reproduced without tzdata: they get the numeric one.
std::string NumericAbbrev(int32_t offset) {
    if (offset == 0) return "UTC";
    char buf[16];
    const int a = std::abs(offset), h = a / 3600, m = (a / 60) % 60;
    if (m) std::snprintf(buf, sizeof buf, "%c%02d%02d", offset < 0 ? '-' : '+', h, m);
    else std::snprintf(buf, sizeof buf, "%c%02d", offset < 0 ? '-' : '+', h);
    return buf;
}

void HostOffset(int64_t posix, int32_t& offset, bool& dst, std::string& name) {
#ifdef _WIN32
    const __time64_t t = posix;
    std::tm lt{}, gt{};
    _localtime64_s(&lt, &t);
    _gmtime64_s(&gt, &t);
    const __time64_t as_utc = _mkgmtime64(&lt);
    offset = static_cast<int32_t>(as_utc - t);
    dst = lt.tm_isdst > 0;
#else
    std::tm lt{};
    const time_t t = static_cast<time_t>(posix);
    localtime_r(&t, &lt);
    offset = static_cast<int32_t>(lt.tm_gmtoff);
    dst = lt.tm_isdst > 0;
#endif
    name = NumericAbbrev(offset);
}

int32_t RuleOffset(const ShimRule* rule, int64_t posix, bool& dst, std::string& name) {
    int mode = g_mode;
    int32_t fixed = g_fixed;
    if (rule && IsShimRule(*rule)) {
        mode = rule->mode;
        fixed = rule->fixed_offset;
    }
    dst = false;
    if (mode == 1) {
        // tzdata: "GMT" for GMT/Etc/GMT/Greenwich, "UTC" for the rest.
        const std::string loc = rule && IsShimRule(*rule) ? std::string(rule->location, strnlen(rule->location, sizeof rule->location)) : "";
        name = (loc.find("GMT") != std::string::npos || loc.find("Greenwich") != std::string::npos) ? "GMT" : "UTC";
        return 0;
    }
    if (mode == 2) {
        name = NumericAbbrev(fixed);
        return fixed;
    }
    int32_t off;
    HostOffset(posix, off, dst, name);
    return off;
}

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {   // H. Hinnant's algorithm (public domain)
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}

void Split(int64_t local, CalendarTime& cal, CalendarAdditionalInfo& info) {
    int64_t days = local / 86400;
    int64_t secs = local % 86400;
    if (secs < 0) { secs += 86400; --days; }
    int64_t y;
    unsigned m, d;
    CivilFromDays(days, y, m, d);
    cal.year = static_cast<int16_t>(y);
    cal.month = static_cast<int8_t>(m);
    cal.day = static_cast<int8_t>(d);
    cal.hour = static_cast<int8_t>(secs / 3600);
    cal.minute = static_cast<int8_t>((secs / 60) % 60);
    cal.second = static_cast<int8_t>(secs % 60);
    cal.pad = 0;
    info.day_of_week = static_cast<int32_t>(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday (4)
    info.day_of_year = static_cast<int32_t>(days - DaysFromCivil(y, 1, 1));
}

std::string g_host_location;

std::string HostIanaName() {
#ifdef _WIN32
    // System ICU (Windows 10 1903+): ucal_getDefaultTimeZone gives the IANA name.
    HMODULE icu = LoadLibraryW(L"icu.dll");
    if (!icu) return {};
    using Fn = int32_t (*)(char16_t*, int32_t, int*);
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(icu, "ucal_getDefaultTimeZone")));
    std::string out;
    if (fn) {
        char16_t buf[64] = {};
        int status = 0;
        const int32_t n = fn(buf, 63, &status);
        if (status <= 0 && n > 0)
            for (int32_t i = 0; i < n && i < 0x23; ++i) out.push_back(buf[i] < 0x80 ? static_cast<char>(buf[i]) : '?');
    }
    FreeLibrary(icu);
    return out;
#else
    return {};
#endif
}

} // namespace

void Configure(const Config& c) {
    std::lock_guard lk(g_mutex);
    g_cfg = c;
    g_mode = 0;
    if (c.timezone == "utc" || c.timezone == "UTC") g_mode = 1;
    else if (!c.timezone.empty() && (c.timezone[0] == '+' || c.timezone[0] == '-')) {
        int h = 0, m = 0;
        std::sscanf(c.timezone.c_str() + 1, "%d:%d", &h, &m);
        g_fixed = (h * 3600 + m * 60) * (c.timezone[0] == '-' ? -1 : 1);
        g_mode = 2;
    }
    if (g_host_location.empty()) g_host_location = HostIanaName();
}

int64_t Now() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() + g_cfg.offset_seconds;
}

std::string DeviceLocationName() {
    std::lock_guard lk(g_mutex);
    if (!g_cfg.location_name.empty()) return g_cfg.location_name;
    if (g_mode == 1) return "UTC";
    if (g_mode == 0 && !g_host_location.empty()) return g_host_location;
    return "UTC";
}

void MakeRule(ShimRule& r, const std::string& location) {
    std::memset(&r, 0, sizeof r);
    std::memcpy(r.magic, "NNSHIMTZ", 8);
    std::strncpy(r.location, location.c_str(), sizeof r.location - 1);
    std::string loc = location;
    if (loc.rfind("Etc/", 0) == 0) loc = loc.substr(4);
    // Fixed tzdata zones the game may request regardless of the device (the game loads "GMT"
    // for ToPosixTime: seen in the A/B against the HLE). Etc/GMT+N is UTC-N (inverted POSIX sign).
    static const char* const kZero[] = {"UTC", "UCT", "Universal", "Zulu", "GMT", "GMT0", "GMT+0", "GMT-0", "Greenwich"};
    for (const char* z : kZero) {
        if (loc == z) { r.mode = 1; return; }
    }
    if (location.rfind("Etc/GMT", 0) == 0 && location.size() > 7 && (location[7] == '+' || location[7] == '-')) {
        const int n = std::atoi(location.c_str() + 8);
        if (n >= 0 && n <= 14) {
            r.mode = 2;
            r.fixed_offset = (location[7] == '+' ? -n : n) * 3600;
            return;
        }
    }
    std::lock_guard lk(g_mutex);
    r.mode = g_mode;
    r.fixed_offset = g_fixed;
}

bool IsShimRule(const ShimRule& r) { return std::memcmp(r.magic, "NNSHIMTZ", 8) == 0; }

void ToCalendar(int64_t posix, const ShimRule* rule, CalendarTime& cal, CalendarAdditionalInfo& info) {
    bool dst = false;
    std::string name;
    const int32_t off = RuleOffset(rule, posix, dst, name);
    info = CalendarAdditionalInfo{};
    Split(posix + off, cal, info);
    std::strncpy(info.tz_name, name.c_str(), sizeof info.tz_name);
    info.is_dst = dst ? 1 : 0;
    info.utc_offset = off;
}

void ToCalendarUtc(int64_t posix, CalendarTime& cal, CalendarAdditionalInfo& info) {
    info = CalendarAdditionalInfo{};
    Split(posix, cal, info);
    std::memcpy(info.tz_name, "UTC", 4);
}

int64_t PosixFromUtc(const CalendarTime& c) {
    return DaysFromCivil(c.year, static_cast<unsigned>(c.month), static_cast<unsigned>(c.day)) * 86400 +
           c.hour * 3600 + c.minute * 60 + c.second;
}

int ToPosix(const CalendarTime& cal, const ShimRule* rule, int64_t out[2]) {
    const int64_t as_utc = PosixFromUtc(cal);
    // Candidates: the offset a few hours before and after (covers DST changes).
    int n = 0;
    for (const int64_t probe : {as_utc - 86400, as_utc + 86400}) {
        bool dst;
        std::string name;
        const int64_t cand = as_utc - RuleOffset(rule, probe, dst, name);
        CalendarTime back{};
        CalendarAdditionalInfo info{};
        ToCalendar(cand, rule, back, info);
        if (std::memcmp(&back, &cal, 7) == 0 && (n == 0 || out[0] != cand)) out[n++] = cand;
    }
    if (n == 2 && out[0] > out[1]) std::swap(out[0], out[1]);
    return n;
}

void DefaultSnapshot(ClockSnapshot& s) {
    std::memset(&s, 0, sizeof s);
    for (CalendarTime* c : {&s.user_calendar, &s.network_calendar}) {
        c->year = 1970;
        c->month = 1;
        c->day = 1;
    }
    for (CalendarAdditionalInfo* i : {&s.user_info, &s.network_info}) {
        i->day_of_week = 4;
        std::memcpy(i->tz_name, "UTC", 4);
    }
}

void FillSnapshot(ClockSnapshot& s) {
    DefaultSnapshot(s);
    static const uint8_t kSource[16] = {'n', 'n', 's', 'h', 'i', 'm', '-', 's', 't', 'e', 'a', 'd', 'y', 0, 0, 1};
    const int64_t steady = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const int64_t now = Now();
    s.steady.time_point = steady;
    std::memcpy(s.steady.source_id, kSource, 16);
    for (SystemClockContext* c : {&s.user_context, &s.network_context}) {
        c->offset = now - steady;
        c->steady = s.steady;
    }
    s.user_time = s.network_time = now;
    ToCalendar(now, nullptr, s.user_calendar, s.user_info);
    ToCalendar(now, nullptr, s.network_calendar, s.network_info);
    const std::string loc = DeviceLocationName();
    std::strncpy(s.location_name, loc.c_str(), sizeof s.location_name - 1);
    s.automatic_correction = 1;
    // TimeType that SDK 11.4 requests in CreateWithStandardSystemClock (the HLE returns it as is: 1).
    s.type = 1;
}

} // namespace time

// =========================== settings ==========================================================
namespace settings {
namespace {
// Public order of the nn::settings::Language enum (switchbrew, SetLanguage).
const char* const kCodes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                              "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
constexpr int kCount = sizeof(kCodes) / sizeof(kCodes[0]);
} // namespace

const char* CodeForLanguage(int language) { return (language >= 0 && language < kCount) ? kCodes[language] : nullptr; }

int LanguageForCode(const char* code) {
    for (int i = 0; i < kCount; ++i)
        if (!std::strncmp(code, kCodes[i], 8)) return i;
    return -1;
}

uint64_t PackCode(const char* code) {
    uint64_t v = 0;
    std::memcpy(&v, code, std::min<size_t>(std::strlen(code), 8));
    return v;
}

std::string UnpackCode(uint64_t v) {
    char buf[9] = {};
    std::memcpy(buf, &v, 8);
    return buf;
}

std::string DesiredLanguage(const std::string& configured) {
    std::string c = configured;
#ifdef _WIN32
    if (c.empty() || c == "host") {
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) {
            c.clear();
            for (const wchar_t* p = name; *p; ++p) c.push_back(static_cast<char>(*p));
        }
    }
#endif
    if (c.empty()) return "en-US";
    // Reduce to the game's language set (as the system does with the NACP ones).
    auto starts = [&](const char* p) { return c.rfind(p, 0) == 0; };
    if (starts("ja")) return "ja";
    if (starts("en-GB") || starts("en-AU") || starts("en-NZ") || starts("en-IE")) return "en-GB";
    if (starts("en")) return "en-US";
    if (starts("fr")) return "fr";
    if (starts("de")) return "de";
    if (starts("it")) return "it";
    if (starts("es")) return "es";
    if (starts("ko")) return "ko";
    if (starts("zh-TW") || starts("zh-HK") || starts("zh-Hant")) return "zh-Hant";
    if (starts("zh")) return "zh-Hans";
    return "en-US";
}
} // namespace settings

// =========================== diag ==============================================================
namespace diag {

std::string Format(const std::string& fmt, ArgSource& args) {
    std::string out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) spec.push_back(fmt[j++]);
        auto num = [&]() {
            if (j < fmt.size() && fmt[j] == '*') {
                spec += std::to_string(static_cast<int>(args.NextGp()));
                ++j;
            } else {
                while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) spec.push_back(fmt[j++]);
            }
        };
        num();
        if (j < fmt.size() && fmt[j] == '.') { spec.push_back('.'); ++j; num(); }
        int longs = 0;
        bool half = false;
        while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j])) {
            if (fmt[j] == 'l' || fmt[j] == 'q' || fmt[j] == 'j' || fmt[j] == 'z' || fmt[j] == 't') ++longs;
            if (fmt[j] == 'h') half = true;
            ++j;
        }
        if (j >= fmt.size()) break;
        const char conv = fmt[j];
        char buf[512];
        buf[0] = 0;
        switch (conv) {
        case '%': out.push_back('%'); break;
        case 'd': case 'i': {
            const uint64_t v = args.NextGp();
            const long long s = longs ? static_cast<long long>(v) : (half ? static_cast<short>(v) : static_cast<int>(v));
            std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), s);
            out += buf;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            const uint64_t v = args.NextGp();
            const unsigned long long u = longs ? v : static_cast<uint32_t>(v);
            std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), u);
            out += buf;
            break;
        }
        case 'c': out.push_back(static_cast<char>(args.NextGp())); break;
        case 'p': {
            std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(args.NextGp()));
            out += buf;
            break;
        }
        case 's': {
            const std::string s = args.Str(args.NextGp());
            std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), args.NextFp());
            out += buf;
            break;
        }
        default: out += spec; out.push_back(conv); break;
        }
        i = j;
    }
    return out;
}

void ShowDialog(const std::string& title, const std::string& text, bool error) {
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    MessageBoxW(nullptr, wide(text).c_str(), wide(title).c_str(),
                MB_OK | MB_TOPMOST | MB_SETFOREGROUND | (error ? MB_ICONERROR : MB_ICONINFORMATION));
#else
    std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
#endif
}

} // namespace diag

// =========================== swkbd: Win32 text window ==========================================
namespace swkbd {
#ifdef _WIN32
namespace {

struct DlgState {
    const Request* req;
    std::wstring result;
};

void Align4(std::vector<WORD>& t) {
    if (t.size() % 2) t.push_back(0);
}

void PushStr(std::vector<WORD>& t, const std::wstring& s) {
    for (wchar_t c : s) t.push_back(static_cast<WORD>(c));
    t.push_back(0);
}

void PushItem(std::vector<WORD>& t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls,
              const std::wstring& text) {
    Align4(t);
    DLGITEMTEMPLATE it{};
    it.style = style | WS_CHILD | WS_VISIBLE;
    it.x = x; it.y = y; it.cx = cx; it.cy = cy; it.id = id;
    const WORD* p = reinterpret_cast<const WORD*>(&it);
    t.insert(t.end(), p, p + sizeof(it) / 2);
    t.push_back(0xFFFF);
    t.push_back(cls);         // 0x80 button, 0x81 edit, 0x82 static
    PushStr(t, text);
    t.push_back(0);           // no creation data
}

INT_PTR CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<DlgState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(h, GWLP_USERDATA, lp);
        st = reinterpret_cast<DlgState*>(lp);
        HWND edit = GetDlgItem(h, 100);
        SendMessageW(edit, EM_LIMITTEXT, static_cast<WPARAM>(std::max(1, st->req->max_length)), 0);
        const std::wstring init(st->req->initial.begin(), st->req->initial.end());
        SetWindowTextW(edit, init.c_str());
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SetForegroundWindow(h);
        SetFocus(edit);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[1024] = {};
            GetWindowTextW(GetDlgItem(h, 100), buf, 1023);
            if (static_cast<int>(wcslen(buf)) < st->req->min_length) {
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }
            st->result = buf;
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

} // namespace

bool Prompt(const Request& r, std::u16string& text) {
    std::wstring header(r.header.begin(), r.header.end());
    std::wstring sub(r.sub.begin(), r.sub.end());
    std::wstring guide(r.guide.begin(), r.guide.end());
    std::wstring label = header;
    if (!sub.empty()) label += (label.empty() ? L"" : L"\n") + sub;
    if (label.empty()) label = guide.empty() ? L"Enter text:" : guide;
    std::vector<WORD> t;
    DLGTEMPLATE d{};
    d.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d.cdit = 4;
    d.cx = 240;
    d.cy = 78;
    const WORD* p = reinterpret_cast<const WORD*>(&d);
    t.insert(t.end(), p, p + sizeof(d) / 2);
    t.push_back(0);   // no menu
    t.push_back(0);   // default class
    PushStr(t, L"PLARecomp");
    t.push_back(9);   // font size
    PushStr(t, L"Segoe UI");
    PushItem(t, SS_LEFT, 7, 6, 226, 22, 101, 0x82, label);
    PushItem(t, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (r.password ? ES_PASSWORD : 0), 7, 32, 226, 14, 100, 0x81, L"");
    PushItem(t, BS_DEFPUSHBUTTON | WS_TABSTOP, 129, 56, 50, 14, IDOK, 0x80, L"OK");
    PushItem(t, BS_PUSHBUTTON | WS_TABSTOP, 183, 56, 50, 14, IDCANCEL, 0x80, L"Cancel");
    DlgState st{&r, {}};
    const INT_PTR rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(t.data()),
                                               static_cast<HWND>(r.owner), DlgProc, reinterpret_cast<LPARAM>(&st));
    if (rc != IDOK) return false;
    text.assign(st.result.begin(), st.result.end());
    return true;
}
#else
bool Prompt(const Request&, std::u16string&) { return false; }
#endif
} // namespace swkbd

} // namespace nnshim
