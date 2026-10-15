// SPDX-License-Identifier: GPL-3.0-or-later

#include "nvn_environment.h"

#include <algorithm>

#include "shader_recompiler/program_header.h"

namespace nvnshader {

namespace {
// The "synthetic" handles ReadCbufValue returns for banks other than c1 are small
// identifiers (1..N) that fit in the 20-bit texture index of a Maxwell handle
// (TexturePair: texture in [0,20), sampler in [20,32)). That way ReadTextureType(handle)
// can map back to the (cbuf, offset) pair it came from.
constexpr u32 kTextureIdMask = 0xFFFFF;

Shader::TextureType ToShader(TextureType t) {
    return static_cast<Shader::TextureType>(static_cast<u32>(t));
}
} // namespace

NvnEnvironment::NvnEnvironment(Shader::Stage stage_, const uint8_t* code_, size_t code_size_,
                               u32 start, const uint8_t* consts_, size_t const_size_,
                               const ShaderHeader& header, const TranslationState& state_)
    : code{code_}, code_size{code_size_}, consts{consts_}, const_size{const_size_},
      state{state_} {
    stage = stage_;
    start_address = start;
    is_proprietary_driver = true; // folds c1 reads (compiler constants)
    gp_passthrough_mask = {};
    if (stage != Shader::Stage::Compute) {
        if (code_size < start + sizeof(Shader::ProgramHeader)) {
            throw Shader::LogicError("code shorter than the SPH");
        }
        std::memcpy(&sph, code + start, sizeof(sph));
        const u64 local_size{sph.LocalMemorySize()};
        local_memory_size = static_cast<u32>(local_size) + sph.common3.shader_local_memory_crs_size;
    } else {
        workgroup_size = header.workgroup_size;
        shared_memory_size = header.shared_memory_size;
        local_memory_size = 0; // [S] the control does not expose local memory (or it was not located)
    }
}

u64 NvnEnvironment::ReadInstruction(u32 address) {
    if (static_cast<size_t>(address) + sizeof(u64) > code_size) {
        throw Shader::LogicError("instruction read outside the code: {:#x} (size {:#x})",
                                 address, code_size);
    }
    u64 inst;
    std::memcpy(&inst, code + address, sizeof(inst));
    return inst;
}

u32 NvnEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    if (cbuf_index == 1) {
        // c1: compiler constants, stored in the shader's own data.
        if (static_cast<size_t>(cbuf_offset) + sizeof(u32) <= const_size) {
            u32 v;
            std::memcpy(&v, consts + cbuf_offset, sizeof(v));
            return v;
        }
        return 0;
    }
    for (size_t i = 0; i < synthetic.size(); ++i) {
        if (synthetic[i].index == cbuf_index && synthetic[i].offset == cbuf_offset) {
            return static_cast<u32>(i + 1);
        }
    }
    synthetic.push_back({cbuf_index, cbuf_offset});
    return static_cast<u32>(synthetic.size());
}

const TextureHint* NvnEnvironment::FindHint(u32 index, u32 offset) const {
    for (const TextureHint& h : state.texture_hints) {
        if (h.cbuf_index == index && h.cbuf_offset == offset) {
            return &h;
        }
    }
    return nullptr;
}

TextureQuery& NvnEnvironment::Query(u32 raw_handle) {
    u32 id = raw_handle & kTextureIdMask;
    if (id == 0 || id > synthetic.size()) {
        id = raw_handle >> 20; // in case the handle came shifted
    }
    u32 index = 0xFFFFFFFF;
    u32 offset = 0xFFFFFFFF;
    if (id != 0 && id <= synthetic.size()) {
        index = synthetic[id - 1].index;
        offset = synthetic[id - 1].offset;
    }
    for (TextureQuery& q : queries) {
        if (q.cbuf_index == index && q.cbuf_offset == offset) {
            return q;
        }
    }
    TextureQuery q{};
    q.cbuf_index = index;
    q.cbuf_offset = offset;
    if (const TextureHint* h = FindHint(index, offset)) {
        q.assumed_type = h->type;
        q.assumed_integer = h->is_integer;
    }
    queries.push_back(q);
    return queries.back();
}

Shader::TextureType NvnEnvironment::ReadTextureType(u32 raw_handle) {
    TextureQuery& q = Query(raw_handle);
    q.asked_type = true;
    return ToShader(q.assumed_type);
}

Shader::TexturePixelFormat NvnEnvironment::ReadTexturePixelFormat(u32 raw_handle) {
    TextureQuery& q = Query(raw_handle);
    q.asked_integer = true;
    return q.assumed_integer ? Shader::TexturePixelFormat::A8B8G8R8_UINT
                             : Shader::TexturePixelFormat::A8B8G8R8_UNORM;
}

bool NvnEnvironment::IsTexturePixelFormatInteger(u32 raw_handle) {
    TextureQuery& q = Query(raw_handle);
    q.asked_integer = true;
    return q.assumed_integer;
}

u32 NvnEnvironment::ReadViewportTransformState() {
    return state.viewport_transform ? 1u : 0u;
}

} // namespace nvnshader
