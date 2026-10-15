// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native mods (recomp/mods/include/pla_mod_api.h): mod.json loading, block-index hooks and the
// pla_api the mods call (nnshim, nvnvk, engine_options, render_res, ...). Same contract and
// folder layout as the fork's runtime (core/arm/recomp/recomp_mods.cpp, PLARecomp code).

#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "loader.h"

namespace plahost::mods {

/// Loads <root>/<title id>/*/mod.json and patches the images' block indexes. Before any block runs.
void Activate(u64 title_id, const std::wstring& root, const std::string& main_build_id);
/// Calls every mod's pla_mod_shutdown (the libraries stay loaded).
void Shutdown();
bool Retired();
/// A post-hooked call returned to kReturnTrap.
bool OnReturnTrap(GuestContext* c);
std::string Report();

} // namespace plahost::mods
