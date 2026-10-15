// NVN objects tracked by the backend: programs and windows.
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "nvnvk.h"

namespace nvnvk {

struct ProgramStage {
    uint64_t data_gpu = 0;     // ShaderData.data
    uint64_t control_cpu = 0;  // ShaderData.control (guest VA)
};

struct ProgramInfo {
    uint64_t obj = 0;
    uint64_t version = 0;      // bumped by each SetShaders on the same object
    std::vector<ProgramStage> stages;
};

struct WindowInfo {
    uint64_t obj = 0;
    std::vector<uint64_t> textures;  // game Texture*
    std::array<int, 4> crop{0, 0, 0, 0};
};

class Objects {
public:
    void SetProgram(const ProgramInfo& p);
    std::shared_ptr<const ProgramInfo> Program(uint64_t obj);
    void SetWindow(const WindowInfo& w);
    bool Window(uint64_t obj, WindowInfo* out);
    void SetCrop(uint64_t obj, int x, int y, int w, int h);

private:
    std::mutex mu_;
    std::unordered_map<uint64_t, std::shared_ptr<const ProgramInfo>> programs_;
    std::unordered_map<uint64_t, WindowInfo> windows_;
};
extern Objects g_obj;

}  // namespace nvnvk
