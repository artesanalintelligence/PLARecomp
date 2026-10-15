// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace plahost::log {
namespace {

std::mutex g_lock;
FILE* g_file = nullptr;
const auto g_start = std::chrono::steady_clock::now();
int g_min_level = [] {
    const char* e = std::getenv("PLAHOST_LOG_LEVEL");
    return e && *e ? std::atoi(e) : static_cast<int>(Info);
}();

const char* LevelName(Level l) {
    switch (l) {
    case Trace: return "Trace";
    case Debug: return "Debug";
    case Info: return "Info";
    case Warning: return "Warning";
    case Error: return "Error";
    default: return "Critical";
    }
}

void Emit(Level level, const char* tag, const char* text) {
    char head[96];
    std::snprintf(head, sizeof head, "[%12.6f] %s <%s> ", Uptime(), tag, LevelName(level));
    std::scoped_lock lk{g_lock};
    if (g_file) {
        std::fputs(head, g_file);
        std::fputs(text, g_file);
        std::fputc('\n', g_file);
        if (level >= Warning) {
            std::fflush(g_file);
        }
    } else {
        std::fputs(head, stderr);
        std::fputs(text, stderr);
        std::fputc('\n', stderr);
    }
}

} // namespace

double Uptime() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

void Open(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(std::u8string(dir.begin(), dir.end())), ec);
    const auto path = std::filesystem::path(std::u8string(dir.begin(), dir.end())) / "pla.log";
    // The previous run's log is kept as pla.old.log (the same names as before, for the test scripts).
    std::filesystem::rename(path, std::filesystem::path(path).replace_filename("pla.old.log"), ec);
    std::scoped_lock lk{g_lock};
    if (g_file) {
        std::fclose(g_file);
    }
    g_file = _wfopen(path.wstring().c_str(), L"wb");
    if (g_file) {
        std::setvbuf(g_file, nullptr, _IOFBF, 1 << 16);
    }
}

void Flush() {
    std::scoped_lock lk{g_lock};
    if (g_file) {
        std::fflush(g_file);
    }
}

void WriteV(Level level, const char* tag, const char* fmt, va_list ap) {
    if (level < g_min_level) {
        return;
    }
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    Emit(level, tag, buf);
}

void Write(Level level, const char* tag, const char* fmt, ...) {
    if (level < g_min_level) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    WriteV(level, tag, fmt, ap);
    va_end(ap);
}

void Raw(Level level, const char* tag, const std::string& text) {
    if (level < g_min_level) {
        return;
    }
    Emit(level, tag, text.c_str());
}

} // namespace plahost::log
