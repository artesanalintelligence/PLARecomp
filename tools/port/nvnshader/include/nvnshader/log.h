// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace nvnshader {

/// level: 0 trace, 1 debug, 2 info, 3 warning, 4 error, 5 critical (= Common::Log::Level).
using LogCallback = void (*)(void* user, int level, const char* message);

/// Redirects recompiler messages (by default level 3+ goes to stderr).
void SetLogCallback(LogCallback cb, void* user, int min_level = 3);

} // namespace nvnshader
