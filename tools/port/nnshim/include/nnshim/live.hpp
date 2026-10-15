// nnshim::live: live settings channel of nvnvk's in-game menu (docs/port/LIVE-SETTINGS.md).
//
// nvnvk.dll exports nvnvk_live_version() and nvnvk_live_get("nnshim.audio.volume", ...). A small thread polls the
// version every 100 ms and calls the subscribed listeners; listeners read the keys they care about with Get().
// Without nvnvk (suyu renderer) Version() stays 0 and the listeners still get their periodic call (e.g. to watch
// controls.json for changes made by the launcher).
#pragma once

#include <cstdint>
#include <string>

namespace nnshim::live {

uint32_t Version();                                // 0 if nvnvk is not loaded
bool Get(const char* key, std::string& out);       // false if the menu has not published the key
// `changed`: the version changed since the previous call. Called from the live thread every ~100 ms.
using Listener = void (*)(bool changed);
void Subscribe(Listener fn);                       // starts the thread on first use
void Stop();                                       // stops and joins the thread (idempotent)

} // namespace nnshim::live
