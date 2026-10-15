// nnshim: A/B validation of the services (time, settings, oe, account) against the HLE.
//
// In observe mode the original (suyu's HLE) handles the call; with gen_mod_json.py --validate
// each validatable adapter also gets a "post" (<adapter>_post) that, when the original
// returns, computes the shim's answer for the same arguments and compares it with the HLE's.
// Counters: val.<name>.ok / val.<name>.diff (plus val.ok / val.diff totals); each
// difference goes to the trace ("val <name> diff hle=... shim=...") and the first 3 of each
// function also to the log.
#pragma once

#include <string>

namespace nnshim {

// Records a comparison result. `equal` decides; hle/shim are only for the report.
void ValidateResult(const char* name, bool equal, const std::string& hle, const std::string& shim);

// printf into a std::string (for ValidateResult reports).
std::string Fmt(const char* fmt, ...);

} // namespace nnshim
