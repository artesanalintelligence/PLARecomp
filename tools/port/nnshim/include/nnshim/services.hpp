// nnshim: native core for time, settings/oe, account, err and swkbd (no guest ABI).
//
// Public structures (switchbrew / libnx; layout verified against the game's SDK):
//   PosixTime              int64 (seconds since 1970, UTC)
//   CalendarTime           { int16 year; int8 month (1-12), day, hour, minute, second; pad } = 8
//   CalendarAdditionalInfo { int32 dayOfWeek (0 = Sunday); int32 dayOfYear (0-365);
//                            char timeZone[8]; int32 isDst; int32 utcOffset (s) } = 0x18
//   LocationName           char[0x24]
//   TimeZoneRule           opaque 0x4000-byte block: the shim stores its own rule there
//   ClockSnapshot          0xD0 bytes (see time::ClockSnapshot)
//   LanguageCode           char[8] ("en-US", "es", ...), returned in X0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nnshim {

namespace time {

#pragma pack(push, 1)
struct CalendarTime {
    int16_t year;
    int8_t month, day, hour, minute, second;
    int8_t pad;
};
struct CalendarAdditionalInfo {
    int32_t day_of_week;
    int32_t day_of_year;
    char tz_name[8];
    int32_t is_dst;
    int32_t utc_offset;
};
struct SteadyClockTimePoint {
    int64_t time_point;          // seconds
    uint8_t source_id[16];
};
struct SystemClockContext {
    int64_t offset;
    SteadyClockTimePoint steady;
};
struct ClockSnapshot {
    SystemClockContext user_context;
    SystemClockContext network_context;
    int64_t user_time;
    int64_t network_time;
    CalendarTime user_calendar;
    CalendarTime network_calendar;
    CalendarAdditionalInfo user_info;
    CalendarAdditionalInfo network_info;
    SteadyClockTimePoint steady;
    char location_name[0x24];
    uint8_t automatic_correction;
    uint8_t type;
    uint16_t unk;
};
// The shim's time zone rule, stored inside the game's opaque TimeZoneRule.
struct ShimRule {
    char magic[8];               // "NNSHIMTZ"
    char location[0x24];
    int32_t mode;                // 0 = host zone, 1 = UTC, 2 = fixed offset
    int32_t fixed_offset;        // seconds (mode 2)
};
#pragma pack(pop)
static_assert(sizeof(CalendarTime) == 8);
static_assert(sizeof(CalendarAdditionalInfo) == 0x18);
static_assert(sizeof(ClockSnapshot) == 0xD0);

struct Config {
    std::string timezone = "host";     // "host" | "utc" | "+HH:MM" / "-HH:MM"
    int64_t offset_seconds = 0;        // added to the host clock (useful for testing daily events)
    std::string location_name;         // empty = host IANA name (Windows ICU) or "UTC"
};
void Configure(const Config& c);
int64_t Now();                                          // current PosixTime (with offset)
std::string DeviceLocationName();
void MakeRule(ShimRule& r, const std::string& location);
bool IsShimRule(const ShimRule& r);
// Conversion using the device rule (rule = nullptr) or a shim rule.
void ToCalendar(int64_t posix, const ShimRule* rule, CalendarTime& cal, CalendarAdditionalInfo& info);
void ToCalendarUtc(int64_t posix, CalendarTime& cal, CalendarAdditionalInfo& info);
// Returns how many PosixTimes map to `cal` (0, 1, or 2 across a DST change).
int ToPosix(const CalendarTime& cal, const ShimRule* rule, int64_t out[2]);
int64_t PosixFromUtc(const CalendarTime& cal);
void FillSnapshot(ClockSnapshot& s);
void DefaultSnapshot(ClockSnapshot& s);                  // = ClockSnapshot::ClockSnapshot()

} // namespace time

namespace settings {
// Language (public nn::settings enum) <-> LanguageCode.
const char* CodeForLanguage(int language);              // nullptr if it does not exist
int LanguageForCode(const char* code);                   // -1 if it does not exist
uint64_t PackCode(const char* code);                     // char[8] packed into a uint64 (X0)
std::string UnpackCode(uint64_t v);
// The application's "desired" language: config "settings.language" or the host's, reduced to
// the set the game supports (ja, en-US, en-GB, fr, de, it, es, ko, zh-Hans, zh-Hant).
std::string DesiredLanguage(const std::string& configured);
} // namespace settings

namespace diag {
// Formats a guest printf. `next_gp` = first X register holding variadic arguments,
// `stack` = address of those that do not fit in registers. Reads strings from guest memory.
class ArgSource {
public:
    virtual ~ArgSource() = default;
    virtual uint64_t NextGp() = 0;
    virtual double NextFp() = 0;
    virtual std::string Str(uint64_t va) = 0;
};
std::string Format(const std::string& fmt, ArgSource& args);
// Blocking host dialog (MessageBox). Does nothing if dialogs are disabled.
void ShowDialog(const std::string& title, const std::string& text, bool error);
} // namespace diag

namespace swkbd {
struct Request {
    std::u16string header, sub, guide, initial;
    int max_length = 500;
    int min_length = 0;
    bool password = false;
    void* owner = nullptr;   // game window (HWND), if the runtime provides it
};
// true = accepted (text = result), false = cancelled.
bool Prompt(const Request& r, std::u16string& text);
} // namespace swkbd

namespace font {
// Region of one font inside the shared font memory (like the pl:u service).
struct Region {
    uint32_t offset = 0;   // from the start of the memory; points to the data, past the header
    uint32_t size = 0;     // font bytes, excluding the header
};
// Builds the shared font memory contents the same way as suyu's HLE
// (IPlatformServiceManager): for each kSlots font present in `dir` (the firmware .bfttf or a
// .ttf with the same name), header {0x18029a7f, size ^ key} + plaintext data.
// Missing ones are skipped (like the HLE). Returns false if none is found.
bool BuildSharedFontImage(const std::string& dir, std::vector<uint8_t>& image, std::vector<Region>& regions,
                          std::string* log = nullptr);
} // namespace font

} // namespace nnshim
