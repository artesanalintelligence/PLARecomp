// Setup wizard: pages, state and the screenshot script.
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>

#include <string>

namespace setup {

struct Options {
    bool simulate = false;       // fake install phases with short timings (launcher.py is not run to install)
    std::string sim_fail;        // phase id that fails in the simulation
    double sim_scale = 1.0;      // multiplies simulated timings
    std::wstring capture;        // folder: walk through the wizard unattended and save a PNG per step
    std::wstring dump, dest, work, keys, title_keys;
    std::wstring data_dir;       // tests: settings, components and download cache under this folder
    std::wstring dev_repo;       // developer mode: use this scripts checkout and the system Python (no components)
    std::string url_base;        // download fallback base URL (overrides the bundled index)
    std::string only;            // tests: launcher --solo (install only these phases)
};

void AppInit(HWND hwnd, const Options& op);
void AppFrame(int w, int h);          // draws one frame (inside NewFrame/Render)
bool AppRequestClose();               // WM_CLOSE: true if the window can close now
bool AppWantsExit();
std::wstring AppCaptureRequest();     // image name to save after this frame ("" = none)
void AppShutdown();

}  // namespace setup
