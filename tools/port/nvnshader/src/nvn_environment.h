// SPDX-License-Identifier: GPL-3.0-or-later
// shader_recompiler Environment for NVN shaders: reads code from a buffer (not from emulated
// GPU memory) and answers the queries that suyu makes against the Maxwell state.
#pragma once

#include <cstring>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "nvnshader/nvnshader.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"

namespace nvnshader {

class NvnEnvironment final : public Shader::Environment {
public:
    /// code: shader data from byte 0 (program base); start: offset of the SPH
    /// (graphics) or of the first instruction (compute). consts: c1 bank.
    NvnEnvironment(Shader::Stage stage_, const uint8_t* code_, size_t code_size_, u32 start,
                   const uint8_t* consts_, size_t const_size_, const ShaderHeader& header,
                   const TranslationState& state_);

    u64 ReadInstruction(u32 address) override;
    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;
    Shader::TextureType ReadTextureType(u32 raw_handle) override;
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 raw_handle) override;
    bool IsTexturePixelFormatInteger(u32 raw_handle) override;
    u32 ReadViewportTransformState() override;
    u32 TextureBoundBuffer() const override {
        return 2; // NVN: texture handles bound in c2 (suyu: "proprietary driver")
    }
    u32 LocalMemorySize() const override {
        return local_memory_size;
    }
    u32 SharedMemorySize() const override {
        return shared_memory_size;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return workgroup_size;
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

    const std::vector<TextureQuery>& Queries() const {
        return queries;
    }

private:
    struct Slot {
        u32 index;
        u32 offset;
    };
    const TextureHint* FindHint(u32 index, u32 offset) const;
    TextureQuery& Query(u32 raw_handle);

    const uint8_t* code;
    size_t code_size;
    const uint8_t* consts;
    size_t const_size;
    const TranslationState& state;
    u32 local_memory_size{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};
    std::vector<Slot> synthetic; ///< id - 1 -> (cbuf, offset)
    std::vector<TextureQuery> queries;
};

} // namespace nvnshader
