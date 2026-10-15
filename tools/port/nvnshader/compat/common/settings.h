// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal replacement for suyu's common/settings.h to build shader_recompiler without
// video_core/core. Only exposes the fields the recompiler reads. Values are set
// from nvnshader (there is no config file).
#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace Settings {

template <typename T>
struct Value {
    T value{};
    constexpr Value() = default;
    constexpr Value(T v) : value{v} {}
    [[nodiscard]] T GetValue() const {
        return value;
    }
    void SetValue(T v) {
        value = v;
    }
    operator T() const {
        return value;
    }
};

struct ResolutionScalingInfo {
    u32 up_scale{1};
    u32 down_shift{0};
    f32 up_factor{1.0f};
    f32 down_factor{1.0f};
    bool active{};
    bool downscale{};

    s32 ScaleUp(s32 v) const {
        return v == 0 ? 0 : (std::max)((v * static_cast<s32>(up_scale)) >> static_cast<s32>(down_shift), 1);
    }
    u32 ScaleUp(u32 v) const {
        return v == 0U ? 0U : (std::max)((v * up_scale) >> down_shift, 1U);
    }
};

struct Values {
    /// Loops without the iteration safeguard (suyu: off by default).
    Value<bool> disable_shader_loop_safety_checks{false};
    /// Android tweak, no effect on desktop.
    Value<bool> fix_bloom_effects{false};
    /// Runs VerificationPass on the IR (slow; useful for debugging).
    bool renderer_debug{false};
    /// suyu resolution rescaling: in the port the backend controls resolution,
    /// so the recompiler never emits rescaling code.
    Value<bool> rescale_hack{false};
    ResolutionScalingInfo resolution_info{};
    bool use_debug_asserts{false};
};

extern Values values;

} // namespace Settings
