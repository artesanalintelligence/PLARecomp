// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal logging, assertion and Settings implementations for shader_recompiler
// outside suyu. Messages go to a configurable callback (nvnshader::SetLogCallback).

#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "nvnshader/log.h"

namespace Settings {
Values values{};
} // namespace Settings

namespace nvnshader {
namespace {
LogCallback g_log_callback = nullptr;
void* g_log_user = nullptr;
int g_min_level = 3; // Warning
} // namespace

namespace detail {
thread_local std::string* g_tls_sink = nullptr;
void SetThreadLogSink(std::string* sink) {
    g_tls_sink = sink;
}
} // namespace detail

void SetLogCallback(LogCallback cb, void* user, int min_level) {
    g_log_callback = cb;
    g_log_user = user;
    g_min_level = min_level;
}
} // namespace nvnshader

namespace Common::Log {

void FmtLogMessageImpl(Class, Level log_level, const char* filename, unsigned int line_num,
                       const char*, fmt::string_view format, const fmt::format_args& args) {
    const int level = static_cast<int>(log_level);
    std::string* sink = nvnshader::detail::g_tls_sink;
    if (level < nvnshader::g_min_level && !(sink && level >= 3)) {
        return;
    }
    std::string msg;
    try {
        msg = fmt::vformat(format, args);
    } catch (...) {
        msg = std::string(format.data(), format.size());
    }
    if (sink && level >= 3) {
        // Warnings during a translation are stored in ShaderModule::warnings.
        if (!sink->empty()) {
            *sink += '\n';
        }
        *sink += msg;
        return;
    }
    if (nvnshader::g_log_callback) {
        nvnshader::g_log_callback(nvnshader::g_log_user, level, msg.c_str());
        return;
    }
    static std::mutex mtx;
    std::scoped_lock lk{mtx};
    const char* base = filename;
    for (const char* p = filename; p && *p; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    std::fprintf(stderr, "[nvnshader %d] %s:%u: %s\n", level, base ? base : "?", line_num,
                 msg.c_str());
}

void Stop() {}

} // namespace Common::Log

// In suyu a "soft" assert only logs and continues. Same here: the recompiler already reports with
// LOG_CRITICAL before calling this function.
void AssertFailSoftImpl() {}

// UNREACHABLE: suyu aborts the process. That is not acceptable in a library, so throw an
// exception that the nvnshader API turns into a translation error.
void AssertFatalImpl() {
    throw std::logic_error("shader_recompiler: UNREACHABLE");
}
