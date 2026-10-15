// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdarg>
#include <string>

namespace plahost::log {

enum Level { Trace = 0, Debug, Info, Warning, Error, Critical };

/// Opens <dir>/pla.log (truncated). Before this, lines go to stderr only.
void Open(const std::string& dir);
void Flush();
/// Seconds since the process started (the log's timestamp).
double Uptime();

void Write(Level level, const char* tag, const char* fmt, ...);
void WriteV(Level level, const char* tag, const char* fmt, va_list ap);
/// A line without formatting (mod messages).
void Raw(Level level, const char* tag, const std::string& text);

} // namespace plahost::log

#define LOGI(tag, ...) ::plahost::log::Write(::plahost::log::Info, tag, __VA_ARGS__)
#define LOGW(tag, ...) ::plahost::log::Write(::plahost::log::Warning, tag, __VA_ARGS__)
#define LOGE(tag, ...) ::plahost::log::Write(::plahost::log::Error, tag, __VA_ARGS__)
#define LOGC(tag, ...) ::plahost::log::Write(::plahost::log::Critical, tag, __VA_ARGS__)
#define LOGD(tag, ...) ::plahost::log::Write(::plahost::log::Debug, tag, __VA_ARGS__)
