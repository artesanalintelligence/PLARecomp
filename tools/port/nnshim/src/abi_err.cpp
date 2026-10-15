// nnshim: adapters for nn::err / nn::diag (28 imports; 13 native, 15 PASS).
//
// Native:
//   diag::detail::AbortImpl (2 overloads, 8,305 call sites) and nndetailDiagAbortFormatImpl:
//     formatted message (guest printf) -> suyu log + dump in <dump_dir> with the
//     registers and the call stack (X29 chain) + host dialog. Then, depending on
//     "err.abort_action": "hle" (default: the original aborts and suyu stops the game as
//     before) or "exit" (terminates the process; for the standalone executable).
//   result::detail::OnUnhandledResult: same as an abort.
//   diag::GetBacktrace: frame chain (X29/X30) read from guest memory.
//   err::ShowError (3) and ShowApplicationError: log + host dialog (the error applet).
//   err::ApplicationErrorArg (constructor and 4 setters): public 0x1014-byte structure.
// PASS (SDK user code with no IPC; works the same recompiled in v1.5):
//   ErrorResultVariant (8), ErrorCode::IsValid, nnResult* (4), NplnContext::Make and
//   UpdateResultWithContext<NplnContext> (network).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "abi_common.hpp"
#include "nnshim/services.hpp"

using namespace nnshim;

namespace {

struct ErrConfig {
    bool dialog = true;
    std::string abort_action = "hle";   // "hle" | "exit"
    std::string dump_dir;
} g_err;
std::mutex g_err_mutex;

void InitErr(const cfg::Value& c) {
    g_err.dialog = c.GetBool("err.dialog", true);
    g_err.abort_action = c.GetString("err.abort_action", "hle");
    g_err.dump_dir = c.GetString("err.dump_dir");
}

ModuleHooks g_err_module{"err", &InitErr, nullptr};

// AAPCS64 variadic arguments: X<first>..X7, V0..V7, then the stack (8 bytes each).
class GuestArgs final : public diag::ArgSource {
public:
    GuestArgs(PlaGuest& g, unsigned first_gp) : g_(g), gp_(first_gp), stack_(g.sp()) {}
    uint64_t NextGp() override {
        if (gp_ < 8) return g_.x(gp_++);
        uint64_t v = 0;
        g_.load(stack_, v);
        stack_ += 8;
        return v;
    }
    double NextFp() override {
        double d = 0;
        if (fp_ < 8) {
            uint64_t v[2];
            g_.get_v(fp_++, v);
            std::memcpy(&d, &v[0], 8);
            return d;
        }
        g_.load(stack_, d);
        stack_ += 8;
        return d;
    }
    std::string Str(uint64_t va) override {
        std::string s;
        if (!va || !g_.cstr(va, s, 1024)) return "(null)";
        return s;
    }

private:
    PlaGuest& g_;
    unsigned gp_;
    unsigned fp_ = 0;
    uint64_t stack_;
};

std::string GuestStr(PlaGuest& g, uint64_t va) {
    std::string s;
    if (!va || !g.cstr(va, s, 1024)) return {};
    return s;
}

std::string ModuleOffset(uint64_t va) {
    char buf[64];
    uint64_t best = 0;
    const char* best_name = nullptr;
    for (const char* m : {"main", "sdk", "subsdk0", "subsdk1", "rtld"}) {
        const uint64_t b = g_api->module_base(m);
        if (b && va >= b && b > best) { best = b; best_name = m; }
    }
    if (best_name) std::snprintf(buf, sizeof buf, "%s+0x%llx", best_name, static_cast<unsigned long long>(va - best));
    else std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(va));
    return buf;
}

// Frame chain: [X29] = previous X29, [X29+8] = return address.
int Backtrace(PlaGuest& g, uint64_t* out, int max) {
    int n = 0;
    if (n < max) out[n++] = g.x(30);
    uint64_t fp = g.x(29);
    while (n < max && fp && (fp & 0xF) == 0) {
        uint64_t frame[2] = {};
        if (g.read(fp, frame, sizeof frame) != sizeof frame || !frame[1]) break;
        out[n++] = frame[1];
        if (frame[0] <= fp) break;   // the stack grows down: older frames are higher up
        fp = frame[0];
    }
    return n;
}

std::string DumpPath() {
    std::filesystem::path dir = g_err.dump_dir.empty() ? std::filesystem::temp_directory_path() / "nnshim"
                                                       : std::filesystem::u8path(g_err.dump_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[64];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(name, sizeof name, "abort-%Y%m%d-%H%M%S.txt", &tm);
    return (dir / name).u8string();
}

// Log + dump + dialog. Returns the adapter action.
pla_action Abort(PlaGuest& g, const std::string& kind, const std::string& message) {
    std::lock_guard lk(g_err_mutex);
    uint64_t bt[32];
    const int n = Backtrace(g, bt, 32);
    std::string text = kind + ": " + message + "\n\nCall stack:\n";
    for (int i = 0; i < n; ++i) text += "  #" + std::to_string(i) + " " + ModuleOffset(bt[i]) + "\n";
    Log(PLA_LOG_ERROR, "nnshim: %s", (kind + ": " + message).c_str());
    for (int i = 0; i < n && i < 8; ++i) Log(PLA_LOG_ERROR, "nnshim:   #%d %s", i, ModuleOffset(bt[i]).c_str());
    const std::string path = DumpPath();
    if (std::FILE* f = std::fopen(path.c_str(), "w")) {
        std::fprintf(f, "%s\nRegisters:\n", text.c_str());
        for (unsigned r = 0; r <= 30; ++r)
            std::fprintf(f, "  X%-2u = 0x%016llx%s", r, static_cast<unsigned long long>(g.x(r)), r % 2 ? "\n" : "");
        std::fprintf(f, "\n  SP  = 0x%016llx\n", static_cast<unsigned long long>(g.sp()));
        std::fclose(f);
        Log(PLA_LOG_ERROR, "nnshim: dump at %s", path.c_str());
    }
    CounterAdd("err.aborts");
    Trace("abort %s", message.c_str());
    if (g_err.dialog)
        diag::ShowDialog("PLARecomp - fatal error",
                         text + "\nDump: " + path + "\n\nThe game will now close.", true);
#ifdef _WIN32
    if (g_err.abort_action == "exit") {
        WriteStatsFile();
        TerminateProcess(GetCurrentProcess(), 3);
    }
#endif
    return PLA_CONTINUE;   // the original aborts (svcBreak) and suyu stops emulation
}

std::string Where(PlaGuest& g, uint64_t cond, uint64_t func, uint64_t file, uint32_t line) {
    std::string s;
    const std::string c = GuestStr(g, cond), fn = GuestStr(g, func), fl = GuestStr(g, file);
    if (!c.empty()) s += "condition \"" + c + "\"";
    if (!fn.empty()) s += (s.empty() ? "" : ", ") + std::string("in ") + fn;
    if (!fl.empty()) s += " (" + fl + ":" + std::to_string(line) + ")";
    return s.empty() ? std::string("no information") : s;
}

void ShowErrorText(const std::string& title, const std::string& text) {
    Log(PLA_LOG_WARNING, "nnshim: %s: %s", title.c_str(), text.c_str());
    Trace("error %s", text.c_str());
    CounterAdd("err.shown");
    if (g_err.dialog) diag::ShowDialog(title, text, false);
}

std::string ResultText(uint32_t r) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04u-%04u (0x%x)", 2000 + (r & 0x1FF), (r >> 9) & 0x1FFF, r);
    return buf;
}

} // namespace

// void AbortImpl(const char* cond, const char* func, const char* file, int line)
NNSHIM_ADAPTER(Err, nnshim_diag_AbortImpl) {
    PlaGuest g(ctx);
    return Abort(g, "Abort", Where(g, g.x(0), g.x(1), g.x(2), g.w(3)));
}

// void AbortImpl(cond, func, file, int line, const Result* r, const char* fmt, ...)
NNSHIM_ADAPTER(Err, nnshim_diag_AbortImplFormat) {
    PlaGuest g(ctx);
    std::string msg = Where(g, g.x(0), g.x(1), g.x(2), g.w(3));
    if (const uint64_t rp = g.x(4)) {
        uint32_t r = 0;
        g.load(rp, r);
        msg += ", result " + ResultText(r);
    }
    if (const std::string fmt = GuestStr(g, g.x(5)); !fmt.empty()) {
        GuestArgs args(g, 6);
        msg += ": " + diag::Format(fmt, args);
    }
    return Abort(g, "Abort", msg);
}

// void nndetailDiagAbortFormatImpl(cond, func, file, int line, const char* fmt, ...)
NNSHIM_ADAPTER(Err, nnshim_diag_AbortFormatImplC) {
    PlaGuest g(ctx);
    std::string msg = Where(g, g.x(0), g.x(1), g.x(2), g.w(3));
    if (const std::string fmt = GuestStr(g, g.x(4)); !fmt.empty()) {
        GuestArgs args(g, 5);
        msg += ": " + diag::Format(fmt, args);
    }
    return Abort(g, "Abort", msg);
}

// void result::detail::OnUnhandledResult(Result)
NNSHIM_ADAPTER(Err, nnshim_result_OnUnhandledResult) {
    PlaGuest g(ctx);
    return Abort(g, "Unhandled result", ResultText(g.w(0)));
}

// int diag::GetBacktrace(uintptr_t* out, int count)
NNSHIM_ADAPTER(Err, nnshim_diag_GetBacktrace) {
    PlaGuest g(ctx);
    uint64_t bt[64];
    const int max = std::min(std::max(static_cast<int>(g.w(1)), 0), 64);
    const int n = Backtrace(g, bt, max);
    if (n) g.write(g.x(0), bt, static_cast<size_t>(n) * 8);
    g.ret(static_cast<uint32_t>(n));
    return PLA_SKIP;
}

// ApplicationErrorArg (0x1014): +0 u16 version (0x102), +8 u32 code, +0xC LanguageCode,
// +0x14 dialog message [0x800], +0x814 full-screen message [0x800].
NNSHIM_ADAPTER(Err, nnshim_err_ApplicationErrorArgCtor) {
    PlaGuest g(ctx);
    const uint64_t a = g.x(0);
    g.store<uint16_t>(a, 0x102);
    g.store<uint32_t>(a + 8, 0);
    g.store<uint8_t>(a + 0x14, 0);
    g.store<uint8_t>(a + 0x814, 0);
    return PLA_SKIP;
}

NNSHIM_ADAPTER(Err, nnshim_err_SetApplicationErrorCodeNumber) {
    PlaGuest g(ctx);
    g.store<uint32_t>(g.x(0) + 8, g.w(1));
    return PLA_SKIP;
}

static void CopyMessage(PlaGuest& g, uint64_t dst, uint64_t src) {
    std::string s;
    g.cstr(src, s, 0x7FF);
    if (s.size() > 0x7FF) s.resize(0x7FF);
    g.write(dst, s.c_str(), s.size() + 1);
}

NNSHIM_ADAPTER(Err, nnshim_err_SetDialogMessage) {
    PlaGuest g(ctx);
    CopyMessage(g, g.x(0) + 0x14, g.x(1));
    return PLA_SKIP;
}

NNSHIM_ADAPTER(Err, nnshim_err_SetFullScreenMessage) {
    PlaGuest g(ctx);
    CopyMessage(g, g.x(0) + 0x814, g.x(1));
    return PLA_SKIP;
}

NNSHIM_ADAPTER(Err, nnshim_err_SetLanguageCode) {
    PlaGuest g(ctx);
    char code[8] = {};
    g.read(g.x(1), code, 8);
    g.write(g.x(0) + 0xC, code, 8);
    return PLA_SKIP;
}

// void ShowApplicationError(const ApplicationErrorArg&)
NNSHIM_ADAPTER(Err, nnshim_err_ShowApplicationError) {
    PlaGuest g(ctx);
    const uint64_t a = g.x(0);
    uint32_t code = 0;
    g.load(a + 8, code);
    std::string dialog = GuestStr(g, a + 0x14), full = GuestStr(g, a + 0x814);
    char head[48];
    std::snprintf(head, sizeof head, "Application error code %u", code);
    ShowErrorText("PLARecomp", std::string(head) + "\n\n" + (full.empty() ? dialog : full));
    return PLA_SKIP;
}

// void ShowError(Result)
NNSHIM_ADAPTER(Err, nnshim_err_ShowErrorResult) {
    PlaGuest g(ctx);
    ShowErrorText("PLARecomp", "Error " + ResultText(g.w(0)));
    return PLA_SKIP;
}

// void ShowError(ErrorCode)   ErrorCode = { u32 category; u32 number } in X0
NNSHIM_ADAPTER(Err, nnshim_err_ShowErrorCode) {
    PlaGuest g(ctx);
    char buf[48];
    std::snprintf(buf, sizeof buf, "Error %04u-%04u", static_cast<uint32_t>(g.x(0)), static_cast<uint32_t>(g.x(0) >> 32));
    ShowErrorText("PLARecomp", buf);
    return PLA_SKIP;
}

// void ShowError(const ErrorResultVariant&)   = { u32 state (1 Result, 2 ErrorCode); u64 value @+4 }
NNSHIM_ADAPTER(Err, nnshim_err_ShowErrorVariant) {
    PlaGuest g(ctx);
    uint32_t state = 0;
    uint64_t v = 0;
    g.load(g.x(0), state);
    g.load(g.x(0) + 4, v);
    char buf[64];
    if (state == 2)
        std::snprintf(buf, sizeof buf, "Error %04u-%04u", static_cast<uint32_t>(v), static_cast<uint32_t>(v >> 32));
    else
        std::snprintf(buf, sizeof buf, "Error %s", ResultText(static_cast<uint32_t>(v)).c_str());
    ShowErrorText("PLARecomp", buf);
    return PLA_SKIP;
}
