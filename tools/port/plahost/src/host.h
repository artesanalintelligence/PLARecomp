// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process-wide frontend state shared by the pieces of plahost.

#pragma once

#include <atomic>
#include <string>

#include "common.h"

namespace plahost::host {

struct Options {
    bool fullscreen = false;
    bool mods = true;
    int fps_cap = 0;        // 0 = none
    bool fps_cap_set = false;  // fps_cap came from PLA.ini or --fps-cap
    int hang_report = 30;   // seconds without a frame before a thread report (0 = off)
    int window_width = 1280;
    int window_height = 720;
    u64 memory_mib = 3285;  // TotalMemorySize the game sees (the console's application pool)
    std::string app_name = "PLARecomp";
};

const Options& Opts();
/// The folder of the executable (UTF-16).
const std::wstring& ExeDir();
std::string ExeDirUtf8();
std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);

/// Ends the process after shutting the mods down (exit code `code`). Any thread.
[[noreturn]] void Exit(int code, const char* why);

/// Frames presented by the renderer (pla_api frame_presented).
extern std::atomic<u64> g_frames;
void* WindowHandle();

/// Writes user/log/threads-<time>.txt (all guest threads and native waits). Returns the path.
std::string WriteThreadReport(const char* reason);

} // namespace plahost::host
