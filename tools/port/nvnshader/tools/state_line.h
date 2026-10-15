// SPDX-License-Identifier: GPL-3.0-or-later
// Text format of TranslationState used by nvnshader_tool --states and nvnshader_vkcheck
// (prewarm lists). Tools only.
#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "nvnshader/nvnshader.h"

namespace nvnshader::tools {

// --states line: "<hash> key=value ..." (written by scripts/states_from_suyu.py).
//   attribs=<32 letters: F float, S sint, U uint, s sscaled, u uscaled, - disabled>
//   topo=<points|lines|triangles> psize=<f> xfb=<0|1> dual=<0|1> earlyz=<0|1> ynegate=<0|1>
//   ndc=<0|1> alpha=<0..7> aref=<f> vt=<0|1> tex=c:o:t:i,... xfbx=attr:buf:stride:off:comps,...
//   prev=<8 hex words separated by ':'> tag=<text>
inline bool ParseStateLine(const std::string& line, std::string& hash, TranslationState& st, std::string& tag) {
    std::istringstream is(line);
    if (!(is >> hash)) {
        return false;
    }
    std::string kv;
    while (is >> kv) {
        const auto eq = kv.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string k = kv.substr(0, eq);
        const std::string v = kv.substr(eq + 1);
        auto split = [](const std::string& x, char sep) {
            std::vector<std::string> out;
            std::string cur;
            for (char ch : x) {
                if (ch == sep) {
                    out.push_back(cur);
                    cur.clear();
                } else {
                    cur += ch;
                }
            }
            if (!cur.empty()) {
                out.push_back(cur);
            }
            return out;
        };
        if (k == "attribs") {
            for (size_t i = 0; i < v.size() && i < st.attrib_types.size(); ++i) {
                switch (v[i]) {
                case 'F': st.attrib_types[i] = AttribType::Float; break;
                case 'S': st.attrib_types[i] = AttribType::SInt; break;
                case 'U': st.attrib_types[i] = AttribType::UInt; break;
                case 's': st.attrib_types[i] = AttribType::SScaled; break;
                case 'u': st.attrib_types[i] = AttribType::UScaled; break;
                default: st.attrib_types[i] = AttribType::Disabled; break;
                }
            }
        } else if (k == "topo") {
            st.topology = v == "points" ? Topology::Points : v == "lines" ? Topology::Lines : Topology::Triangles;
        } else if (k == "psize") {
            st.point_size = std::stof(v);
        } else if (k == "xfb") {
            st.xfb_active = v == "1";
        } else if (k == "dual") {
            st.dual_source_blend = v == "1";
        } else if (k == "earlyz") {
            st.force_early_z = v == "1";
        } else if (k == "ynegate") {
            st.y_negate = v == "1";
        } else if (k == "ndc") {
            st.depth_minus_one_to_one = v == "1";
        } else if (k == "alpha") {
            st.alpha_test = static_cast<CompareOp>(std::stoi(v));
        } else if (k == "aref") {
            st.alpha_ref = std::stof(v);
        } else if (k == "vt") {
            st.viewport_transform = v == "1";
        } else if (k == "tex") {
            for (const auto& t : split(v, ',')) {
                const auto f = split(t, ':');
                if (f.size() == 4) {
                    st.texture_hints.push_back(TextureHint{
                        static_cast<uint32_t>(std::stoul(f[0])), static_cast<uint32_t>(std::stoul(f[1])),
                        static_cast<TextureType>(std::stoi(f[2])), f[3] == "1"});
                }
            }
        } else if (k == "xfbx") {
            st.xfb.assign(256, {});
            for (const auto& t : split(v, ',')) {
                const auto f = split(t, ':');
                if (f.size() == 5) {
                    const size_t a = std::stoul(f[0]);
                    if (a < 256) {
                        st.xfb[a] = {static_cast<uint8_t>(std::stoul(f[1])), 0,
                                     static_cast<uint16_t>(std::stoul(f[2])),
                                     static_cast<uint16_t>(std::stoul(f[3])),
                                     static_cast<uint8_t>(std::stoul(f[4]))};
                    }
                }
            }
        } else if (k == "prev") {
            for (const auto& w : split(v, ':')) {
                st.previous_stage_stores.push_back(std::stoull(w, nullptr, 16));
            }
        } else if (k == "tag") {
            tag = v;
        }
    }
    return true;
}

} // namespace nvnshader::tools
