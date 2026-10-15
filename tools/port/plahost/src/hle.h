// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// FPS-3 (B1): host versions of the SDK's internal sync primitives (PLAHOST_HLE=1); see hle.cpp.

#pragma once

namespace plahost::hle {

/// Replaces the block-index slots of the SDK sync primitives (after recomp::Attach, before the mods hook).
void Install();

} // namespace plahost::hle
