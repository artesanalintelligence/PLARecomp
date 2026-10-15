// SPDX-License-Identifier: GPL-3.0-or-later
// NVN (Maxwell) -> SPIR-V translation on top of suyu's shader_recompiler.

#include "nvnshader/nvnshader.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "common/settings.h"
#include "control.h"
#include "nvn_environment.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace nvnshader::detail {
void SetThreadLogSink(std::string* sink);
}

namespace nvnshader {

namespace {

uint32_t Rd32(const uint8_t* p, size_t off) {
    uint32_t v;
    std::memcpy(&v, p + off, sizeof(v));
    return v;
}

struct Hasher {
    uint64_t h = 0xcbf29ce484222325ULL;
    void Bytes(const void* d, size_t n) {
        h = Hash64(d, n, h);
    }
    template <typename T>
    void Pod(const T& v) {
        Bytes(&v, sizeof(v));
    }
};

Shader::Stage ToShaderStage(Stage s) {
    switch (s) {
    case Stage::Vertex:
        return Shader::Stage::VertexB;
    case Stage::TessControl:
        return Shader::Stage::TessellationControl;
    case Stage::TessEval:
        return Shader::Stage::TessellationEval;
    case Stage::Geometry:
        return Shader::Stage::Geometry;
    case Stage::Fragment:
        return Shader::Stage::Fragment;
    case Stage::Compute:
        return Shader::Stage::Compute;
    }
    return Shader::Stage::VertexB;
}

Shader::AttributeType ToShader(AttribType t) {
    return static_cast<Shader::AttributeType>(static_cast<uint8_t>(t));
}

Shader::Profile MakeProfile(const HostCaps& c) {
    Shader::Profile p{};
    p.supported_spirv = c.spirv_version;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = c.int8;
    p.support_uniform_and_storage_buffer_8bit = c.storage_8bit;
    p.support_int16 = c.int16;
    p.support_uniform_and_storage_buffer_16bit = c.storage_16bit;
    p.support_int64 = c.int64;
    p.support_vertex_instance_id = false;
    p.support_float_controls = c.float_controls;
    p.support_separate_denorm_behavior = c.float_controls && c.separate_denorm_behavior;
    p.support_separate_rounding_mode = c.float_controls && c.separate_rounding_mode;
    p.support_fp16_denorm_preserve = c.float_controls && c.denorm_preserve_fp16;
    p.support_fp32_denorm_preserve = c.float_controls && c.denorm_preserve_fp32;
    p.support_fp16_denorm_flush = c.float_controls && c.denorm_flush_fp16;
    p.support_fp32_denorm_flush = c.float_controls && c.denorm_flush_fp32;
    p.support_fp16_signed_zero_nan_preserve = c.float_controls && c.signed_zero_inf_nan_preserve_fp16;
    p.support_fp32_signed_zero_nan_preserve = c.float_controls && c.signed_zero_inf_nan_preserve_fp32;
    p.support_fp64_signed_zero_nan_preserve = c.float_controls && c.signed_zero_inf_nan_preserve_fp64;
    p.support_explicit_workgroup_layout = c.explicit_workgroup_layout;
    p.support_vote = c.subgroup_vote;
    p.supported_subgroup_stages = 0x7F;
    p.support_viewport_index_layer_non_geometry = c.viewport_index_layer_any_stage;
    p.support_viewport_mask = c.nv_viewport_array2;
    p.support_typeless_image_loads = c.typeless_image_loads;
    p.support_demote_to_helper_invocation = c.demote_to_helper;
    p.support_int64_atomics = c.int64_atomics;
    p.support_derivative_control = true;
    p.support_geometry_shader_passthrough = c.nv_geometry_passthrough;
    p.support_native_ndc = c.native_ndc;
    p.support_scaled_attributes = c.scaled_attributes;
    p.support_multi_viewport = true;
    p.support_geometry_streams = c.geometry_streams;
    p.support_sampled_image_array_nonuniform_indexing = c.sampled_image_array_nonuniform;
    p.warp_size_potentially_larger_than_guest = !c.is_nvidia;
    p.lower_left_origin_mode = false;
    p.need_declared_frag_colors = false;
    p.need_gather_subpixel_offset = !c.is_nvidia;
    p.has_broken_spirv_clamp = false;
    p.has_broken_spirv_position_input = false;
    p.has_broken_unsigned_image_offsets = false;
    p.has_broken_signed_operations = false;
    p.has_broken_fp16_float_controls = c.is_nvidia; // same as suyu on NVIDIA
    p.ignore_nan_fp_comparisons = false;
    p.has_broken_spirv_subgroup_mask_vector_extract_dynamic = false;
    // cbuf reads with a dynamic offset: explicit range check (returns 0 like the hardware).
    // Without it, an offset of -1 read 4 GiB past the bound UBO: with robustBufferAccess off that
    // is an access anywhere in the process's GPU address space (GPU-AV in intro7b) and it
    // crashed suyu's Present in shadow mode.
    p.has_broken_robust = true;
    p.min_ssbo_alignment = c.min_ssbo_alignment;
    p.max_user_clip_distances = c.max_user_clip_distances;
    return p;
}

Shader::HostTranslateInfo MakeHostInfo(const HostCaps& c) {
    Shader::HostTranslateInfo h{};
    h.min_ssbo_alignment = c.min_ssbo_alignment;
    h.support_float64 = c.float64;
    h.support_float16 = c.float16;
    h.support_int64 = c.int64;
    h.needs_demote_reorder = !c.is_nvidia;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = c.viewport_index_layer_any_stage;
    h.support_geometry_shader_passthrough = c.nv_geometry_passthrough;
    h.support_conditional_barrier = c.is_nvidia;
    h.ApplyDescriptorLimitPolicy();
    return h;
}

// Transform feedback layout from the control section (see control.h). Returns 256 entries
// indexed by suyu attribute, like VideoCommon::MakeTransformFeedbackVaryings.
std::vector<nvnvk::shader::XfbVarying> XfbFromControl(const uint8_t* c) {
    std::vector<nvnvk::shader::XfbVarying> xfb(256);
    const uint32_t n = std::min<uint32_t>(c[ctrl::kXfbCount], 4);
    for (uint32_t b = 0; b < n; ++b) {
        if (!c[ctrl::kXfbEnabled + b]) {
            continue;
        }
        const uint32_t vec = Rd32(c, ctrl::kXfbVector + b * ctrl::kXfbBufferStride);
        const uint32_t comps = Rd32(c, ctrl::kXfbComponents + b * ctrl::kXfbBufferStride);
        uint16_t stride;
        std::memcpy(&stride, c + ctrl::kXfbStrides + 2 * b, sizeof(stride));
        if (vec < 0x20 || vec >= 0x40 || comps == 0 || comps > 4) {
            continue; // [S] only the "one generic per buffer" case has been seen
        }
        const uint32_t attr = 0x20 + (vec - 0x20) * 4;
        xfb[attr] = nvnvk::shader::XfbVarying{static_cast<uint8_t>(b), 0, stride, 0,
                                              static_cast<uint8_t>(comps)};
    }
    return xfb;
}

Shader::RuntimeInfo MakeRuntimeInfo(const Shader::IR::Program& program, const TranslationState& st) {
    Shader::RuntimeInfo info{};
    if (st.previous_stage_stores.empty()) {
        info.previous_stage_stores.mask.set();
    } else {
        for (size_t w = 0; w < st.previous_stage_stores.size() && w < 8; ++w) {
            for (size_t b = 0; b < 64; ++b) {
                if ((st.previous_stage_stores[w] >> b) & 1) {
                    info.previous_stage_stores.mask.set(w * 64 + b);
                }
            }
        }
    }
    switch (program.stage) {
    case Shader::Stage::VertexB:
        if (st.topology == Topology::Points) {
            info.fixed_state_point_size = st.point_size;
        }
        info.convert_depth_mode = st.depth_minus_one_to_one;
        for (size_t i = 0; i < info.generic_input_types.size(); ++i) {
            info.generic_input_types[i] = ToShader(st.attrib_types[i]);
        }
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = st.tess_clockwise;
        info.tess_primitive = static_cast<Shader::TessPrimitive>(st.tess_domain);
        info.tess_spacing = static_cast<Shader::TessSpacing>(st.tess_spacing);
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = st.point_size;
        }
        info.convert_depth_mode = st.depth_minus_one_to_one;
        break;
    case Shader::Stage::Fragment:
        info.alpha_test_func = static_cast<Shader::CompareFunction>(st.alpha_test);
        info.alpha_test_reference = st.alpha_ref;
        info.dual_source_blend = st.dual_source_blend;
        for (size_t i = 0; i < 8; ++i) {
            info.color_output_types[i] = ToShader(st.color_output_types[i]);
        }
        break;
    default:
        break;
    }
    if ((program.stage == Shader::Stage::VertexB || program.stage == Shader::Stage::Geometry) &&
        !st.xfb.empty()) {
        const size_t n = std::min<size_t>(st.xfb.size(), info.xfb_varyings.size());
        u32 count = 0;
        for (size_t i = 0; i < n; ++i) {
            const auto& x = st.xfb[i];
            info.xfb_varyings[i] = Shader::TransformFeedbackVarying{
                .buffer = x.buffer, .stream = x.stream, .stride = x.stride,
                .offset = x.offset, .components = x.components};
            if (x.components) {
                count = static_cast<u32>(i + 1);
            }
        }
        info.xfb_count = count;
    }
    switch (st.topology) {
    case Topology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Topology::Lines:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Topology::LinesAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Topology::TrianglesAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    default:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    }
    info.force_early_z = st.force_early_z;
    info.y_negate = st.y_negate;
    return info;
}

void FillModule(const Shader::IR::Program& program, ShaderModule& m) {
    const Shader::Info& info = program.info;
    u32 binding = 0;
    for (const auto& d : info.constant_buffer_descriptors) {
        Descriptor o{};
        o.type = DescriptorType::UniformBuffer;
        o.binding = binding++;
        o.count = d.count;
        o.cbuf_index = d.index;
        o.size = info.uses_global_memory ? 65536u : info.constant_buffer_used_sizes[d.index];
        o.is_read = true;
        m.descriptors.push_back(o);
    }
    for (const auto& d : info.storage_buffers_descriptors) {
        Descriptor o{};
        o.type = DescriptorType::StorageBuffer;
        o.binding = binding;
        binding += d.count;
        o.count = d.count;
        o.cbuf_index = d.cbuf_index;
        o.cbuf_offset = d.cbuf_offset;
        o.is_written = d.is_written;
        o.is_read = true;
        m.descriptors.push_back(o);
    }
    for (const auto& d : info.texture_buffer_descriptors) {
        Descriptor o{};
        o.type = DescriptorType::TextureBuffer;
        o.binding = binding++;
        o.count = d.count;
        o.cbuf_index = d.cbuf_index;
        o.cbuf_offset = d.cbuf_offset;
        o.texture_type = TextureType::Buffer;
        o.has_secondary = d.has_secondary;
        o.shift_left = d.shift_left;
        o.secondary_cbuf_index = d.secondary_cbuf_index;
        o.secondary_cbuf_offset = d.secondary_cbuf_offset;
        o.secondary_shift_left = d.secondary_shift_left;
        o.size_shift = d.size_shift;
        o.is_read = true;
        m.descriptors.push_back(o);
    }
    for (const auto& d : info.image_buffer_descriptors) {
        Descriptor o{};
        o.type = DescriptorType::ImageBuffer;
        o.binding = binding++;
        o.count = d.count;
        o.cbuf_index = d.cbuf_index;
        o.cbuf_offset = d.cbuf_offset;
        o.texture_type = TextureType::Buffer;
        o.image_format = static_cast<uint32_t>(d.format);
        o.is_written = d.is_written;
        o.is_read = d.is_read;
        o.is_integer = d.is_integer;
        o.size_shift = d.size_shift;
        m.descriptors.push_back(o);
    }
    for (const auto& d : info.texture_descriptors) {
        Descriptor o{};
        o.type = DescriptorType::SampledTexture;
        o.binding = binding++;
        o.count = d.count;
        o.cbuf_index = d.cbuf_index;
        o.cbuf_offset = d.cbuf_offset;
        o.texture_type = static_cast<TextureType>(static_cast<u32>(d.type));
        o.is_depth = d.is_depth;
        o.is_multisample = d.is_multisample;
        o.is_integer = d.is_integer;
        o.has_secondary = d.has_secondary;
        o.shift_left = d.shift_left;
        o.secondary_cbuf_index = d.secondary_cbuf_index;
        o.secondary_cbuf_offset = d.secondary_cbuf_offset;
        o.secondary_shift_left = d.secondary_shift_left;
        o.size_shift = d.size_shift;
        o.is_read = true;
        m.descriptors.push_back(o);
    }
    for (const auto& d : info.image_descriptors) {
        Descriptor o{};
        o.type = DescriptorType::StorageImage;
        o.binding = binding++;
        o.count = d.count;
        o.cbuf_index = d.cbuf_index;
        o.cbuf_offset = d.cbuf_offset;
        o.texture_type = static_cast<TextureType>(static_cast<u32>(d.type));
        o.image_format = static_cast<uint32_t>(d.format);
        o.is_written = d.is_written;
        o.is_read = d.is_read;
        o.is_integer = d.is_integer;
        o.size_shift = d.size_shift;
        m.descriptors.push_back(o);
    }
    for (size_t i = 0; i < 32; ++i) {
        for (size_t c = 0; c < 4; ++c) {
            const size_t bit = i * 4 + c;
            if (info.loads.Generic(i, c)) {
                m.input_mask[bit / 64] |= 1ULL << (bit % 64);
            }
            if (info.stores.Generic(i, c)) {
                m.output_mask[bit / 64] |= 1ULL << (bit % 64);
            }
        }
    }
    for (size_t b = 0; b < 512; ++b) {
        if (info.stores.mask[b]) {
            m.stores[b / 64] |= 1ULL << (b % 64);
        }
    }
    for (size_t i = 0; i < 8; ++i) {
        if (info.stores_frag_color[i]) {
            m.color_outputs_mask |= 1u << i;
        }
    }
    m.writes_depth = info.stores_frag_depth;
    m.writes_sample_mask = info.stores_sample_mask;
    m.uses_render_area = info.uses_render_area;
    m.uses_layer = info.stores[Shader::IR::Attribute::Layer];
    m.uses_viewport_index = info.stores[Shader::IR::Attribute::ViewportIndex];
    m.clip_distances_mask = info.used_clip_distances;
    m.local_memory_size = program.local_memory_size;
    m.shared_memory_size = program.shared_memory_size;
    m.workgroup_size = program.workgroup_size;
}

} // namespace

uint64_t Hash64(const void* data, size_t size, uint64_t seed) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool ParseControl(const ShaderBinary& b, ShaderHeader* out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };
    if (!b.control) {
        return fail("null control");
    }
    const auto* c = static_cast<const uint8_t*>(b.control);
    if (b.control_size != 0 && b.control_size < ctrl::kMinControlSize) {
        return fail("control too short");
    }
    if (Rd32(c, ctrl::kMagic) != ctrl::kControlMagic) {
        return fail("bad control magic (not an NVN shader)");
    }
    ShaderHeader h{};
    const uint32_t stage = Rd32(c, ctrl::kStage);
    if (stage > 5) {
        return fail("unknown stage in control");
    }
    h.stage = static_cast<Stage>(stage);
    h.code_offset = Rd32(c, ctrl::kHeaderSize);
    h.code_size = Rd32(c, ctrl::kCodeSize);
    h.const_size = Rd32(c, ctrl::kConstSize);
    h.const_offset = Rd32(c, ctrl::kConstOffset);
    h.data_size = Rd32(c, ctrl::kDataSize);
    h.num_registers = Rd32(c, ctrl::kNumRegisters);
    if (h.stage == Stage::Vertex) {
        h.xfb_buffers = c[ctrl::kXfbCount];
    }
    if (h.stage == Stage::Compute) {
        h.workgroup_size = {Rd32(c, ctrl::kWorkgroupX), Rd32(c, ctrl::kWorkgroupY),
                            Rd32(c, ctrl::kWorkgroupZ)};
        h.shared_memory_size = Rd32(c, ctrl::kSharedMemory);
    }
    if (b.data_size != 0) {
        if (h.data_size == 0 || h.data_size > b.data_size) {
            h.data_size = static_cast<uint32_t>(b.data_size);
        }
    }
    if (h.code_offset != 0 && h.code_offset != 0x30) {
        return fail("unexpected data header");
    }
    if (h.code_offset + h.code_size > h.data_size) {
        return fail("code outside data");
    }
    if (h.const_size != 0 && h.const_offset + h.const_size > h.data_size) {
        return fail("c1 bank outside data");
    }
    if (b.data) {
        const auto* d = static_cast<const uint8_t*>(b.data);
        if (h.code_offset == 0x30 && Rd32(d, 0) != ctrl::kDataMagic) {
            return fail("bad data magic");
        }
        h.code_hash = Hash64(d, h.data_size);
    }
    *out = h;
    return true;
}

bool Translate(const ShaderBinary& binary, const TranslationState& state, const HostCaps& caps,
               const TranslateOptions& options, TranslateResult* out, std::string* error) {
    ShaderHeader header{};
    if (!ParseControl(binary, &header, error)) {
        return false;
    }
    if (!binary.data) {
        if (error) {
            *error = "null data";
        }
        return false;
    }
    const auto* data = static_cast<const uint8_t*>(binary.data);
    // The Environment sees all of data: the SPH starts at code_offset (0x30) and instructions
    // at code_offset + 0x50 = 0x80, 32-byte aligned as Maxwell requires (each 32-byte group
    // starts with a scheduling word). Readable region: up to the c1 bank (which follows the
    // code) or to the end of data.
    size_t code_end = header.data_size;
    if (header.const_size != 0 && header.const_offset > header.code_offset) {
        code_end = header.const_offset;
    }
    const uint8_t* consts = header.const_size ? data + header.const_offset : nullptr;

    const Shader::Stage sstage = ToShaderStage(header.stage);
    const Shader::Profile profile = MakeProfile(caps);
    // XFB active without an explicit layout: take it from the control section.
    TranslationState xfb_state;
    const TranslationState* effective = &state;
    if (state.xfb_active && state.xfb.empty() && header.xfb_buffers != 0) {
        xfb_state = state;
        xfb_state.xfb = XfbFromControl(static_cast<const uint8_t*>(binary.control));
        effective = &xfb_state;
    }
    const Shader::HostTranslateInfo host_info = MakeHostInfo(caps);

    // One translation pass with the given options. uses_atomic_images reports whether the
    // shader does image atomics (they need an integer format: see below).
    auto run = [&](const TranslationState& st, TranslateResult& result, bool& uses_atomic_images) {
        NvnEnvironment env(sstage, data, code_end, header.code_offset, consts, header.const_size,
                           header, st);
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
        Shader::ObjectPool<Shader::IR::Inst> insts;
        Shader::ObjectPool<Shader::IR::Block> blocks;
        ShaderModule& m = result.module;
        m.stage = header.stage;
        m.code_hash = header.code_hash;
        m.state_hash = state.Hash();
        if (sstage == Shader::Stage::Compute) {
            Shader::Maxwell::Flow::CFG cfg{env, flow_blocks, env.StartAddress()};
            Shader::IR::Program program{
                Shader::Maxwell::TranslateProgram(insts, blocks, env, cfg, host_info)};
            m.spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
            FillModule(program, m);
            uses_atomic_images = program.info.uses_atomic_image_u32;
        } else {
            const u32 cfg_offset{
                static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
            Shader::Maxwell::Flow::CFG cfg(env, flow_blocks, cfg_offset, false);
            Shader::IR::Program program{
                Shader::Maxwell::TranslateProgram(insts, blocks, env, cfg, host_info)};
            const Shader::RuntimeInfo rt{MakeRuntimeInfo(program, st)};
            Shader::Maxwell::ConvertLegacyToGeneric(program, rt);
            Shader::Backend::Bindings bindings{};
            m.spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, program, bindings);
            FillModule(program, m);
            uses_atomic_images = program.info.uses_atomic_image_u32;
        }
        m.texture_queries = env.Queries();
    };

    std::string warnings;
    detail::SetThreadLogSink(&warnings);
    try {
        TranslateResult result{};
        bool atomic_images = false;
        if (options.verify_ir) {
            Settings::values.renderer_debug = true; // global: debugging only
        }
        run(*effective, result, atomic_images);
        if (atomic_images) {
            // Image atomics require an integer sampled type. Without a hint the format is
            // assumed non-integer (the bound texture is unknown) and the SPIR-V would be
            // invalid: retry with the unhinted images marked as integer.
            TranslationState retry = *effective;
            bool changed = false;
            for (const TextureQuery& q : result.module.texture_queries) {
                const bool is_image = std::any_of(
                    result.module.descriptors.begin(), result.module.descriptors.end(),
                    [&](const Descriptor& d) {
                        return (d.type == DescriptorType::StorageImage ||
                                d.type == DescriptorType::ImageBuffer) &&
                               d.cbuf_index == q.cbuf_index && d.cbuf_offset == q.cbuf_offset;
                    });
                const bool hinted = std::any_of(
                    state.texture_hints.begin(), state.texture_hints.end(),
                    [&](const TextureHint& h) {
                        return h.cbuf_index == q.cbuf_index && h.cbuf_offset == q.cbuf_offset;
                    });
                if (is_image && !hinted && !q.assumed_integer) {
                    retry.texture_hints.push_back(
                        TextureHint{q.cbuf_index, q.cbuf_offset, q.assumed_type, true});
                    changed = true;
                }
            }
            if (changed) {
                warnings.clear();
                result = TranslateResult{};
                run(retry, result, atomic_images);
                result.module.state_hash = state.Hash();
            }
        }
        detail::SetThreadLogSink(nullptr);
        result.module.warnings = std::move(warnings);
        if (options.optimize != OptLevel::None) {
            std::string opt_error;
            if (!OptimizeSpirv(result.module.spirv, options.optimize, &opt_error)) {
                if (!result.module.warnings.empty()) {
                    result.module.warnings += '\n';
                }
                result.module.warnings += opt_error;
            }
        }
        *out = std::move(result);
        return true;
    } catch (const std::exception& e) {
        detail::SetThreadLogSink(nullptr);
        if (error) {
            *error = e.what();
            if (!warnings.empty()) {
                *error += " | ";
                *error += warnings;
            }
        }
        return false;
    }
}

uint64_t CacheKey(const ShaderHeader& header, const TranslationState& state,
                  const TranslateOptions& options, const HostCaps& caps) {
    Hasher h;
    h.Pod(kTranslatorVersion);
    h.Pod(header.code_hash);
    h.Pod(header.data_size);
    h.Pod(state.Hash());
    h.Pod(caps.Hash());
    h.Pod(options.verify_ir);
    h.Pod(options.optimize);
    return h.h;
}

// ---------------------------------------------------------------------------------------------
// Serialization (on-disk cache format)

namespace {
constexpr uint32_t kCacheMagic = 0x48535656; // "VVSH"

struct Writer {
    std::vector<uint8_t> out;
    template <typename T>
    void Pod(const T& v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void Str(const std::string& s) {
        Pod(static_cast<uint32_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t o = 0;
    bool ok = true;
    template <typename T>
    void Pod(T& v) {
        if (o + sizeof(T) > n) {
            ok = false;
            return;
        }
        std::memcpy(&v, p + o, sizeof(T));
        o += sizeof(T);
    }
    void Str(std::string& s) {
        uint32_t len = 0;
        Pod(len);
        if (!ok || o + len > n) {
            ok = false;
            return;
        }
        s.assign(reinterpret_cast<const char*>(p + o), len);
        o += len;
    }
};
} // namespace

std::vector<uint8_t> Serialize(const TranslateResult& r) {
    Writer w;
    const ShaderModule& m = r.module;
    w.Pod(kCacheMagic);
    w.Pod(kTranslatorVersion);
    w.Pod(nvnvk::shader::kShaderIfaceVersion);
    w.Pod(m.stage);
    w.Pod(m.code_hash);
    w.Pod(m.state_hash);
    w.Pod(static_cast<uint32_t>(m.spirv.size()));
    for (uint32_t word : m.spirv) {
        w.Pod(word);
    }
    w.Pod(static_cast<uint32_t>(m.descriptors.size()));
    for (const Descriptor& d : m.descriptors) {
        w.Pod(d); // POD without pointers
    }
    w.Pod(m.input_mask);
    w.Pod(m.output_mask);
    w.Pod(m.stores);
    w.Pod(m.color_outputs_mask);
    w.Pod(m.writes_depth);
    w.Pod(m.writes_sample_mask);
    w.Pod(m.uses_render_area);
    w.Pod(m.uses_layer);
    w.Pod(m.uses_viewport_index);
    w.Pod(m.clip_distances_mask);
    w.Pod(m.local_memory_size);
    w.Pod(m.shared_memory_size);
    w.Pod(m.workgroup_size);
    w.Str(m.warnings);
    w.Pod(static_cast<uint32_t>(m.texture_queries.size()));
    for (const TextureQuery& q : m.texture_queries) {
        w.Pod(q);
    }
    return std::move(w.out);
}

bool Deserialize(const uint8_t* data, size_t size, TranslateResult* out) {
    Reader r{data, size};
    uint32_t magic = 0, ver = 0, iver = 0;
    r.Pod(magic);
    r.Pod(ver);
    r.Pod(iver);
    if (!r.ok || magic != kCacheMagic || ver != kTranslatorVersion ||
        iver != nvnvk::shader::kShaderIfaceVersion) {
        return false;
    }
    TranslateResult res{};
    ShaderModule& m = res.module;
    r.Pod(m.stage);
    r.Pod(m.code_hash);
    r.Pod(m.state_hash);
    uint32_t n = 0;
    r.Pod(n);
    if (!r.ok || n > (64u << 20)) {
        return false;
    }
    m.spirv.resize(n);
    for (auto& word : m.spirv) {
        r.Pod(word);
    }
    r.Pod(n);
    if (!r.ok || n > 4096) {
        return false;
    }
    m.descriptors.resize(n);
    for (auto& d : m.descriptors) {
        r.Pod(d);
    }
    r.Pod(m.input_mask);
    r.Pod(m.output_mask);
    r.Pod(m.stores);
    r.Pod(m.color_outputs_mask);
    r.Pod(m.writes_depth);
    r.Pod(m.writes_sample_mask);
    r.Pod(m.uses_render_area);
    r.Pod(m.uses_layer);
    r.Pod(m.uses_viewport_index);
    r.Pod(m.clip_distances_mask);
    r.Pod(m.local_memory_size);
    r.Pod(m.shared_memory_size);
    r.Pod(m.workgroup_size);
    r.Str(m.warnings);
    r.Pod(n);
    if (!r.ok || n > 4096) {
        return false;
    }
    m.texture_queries.resize(n);
    for (auto& q : m.texture_queries) {
        r.Pod(q);
    }
    if (!r.ok) {
        return false;
    }
    *out = std::move(res);
    return true;
}

DiskCache::DiskCache(std::string dir) : dir_{std::move(dir)} {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
}

namespace {
std::filesystem::path CachePath(const std::string& dir, uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.nvsh", static_cast<unsigned long long>(key));
    return std::filesystem::path(dir) / name;
}
} // namespace

bool DiskCache::Load(uint64_t key, TranslateResult* out) const {
    std::ifstream f(CachePath(dir_, key), std::ios::binary);
    if (!f) {
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return Deserialize(buf.data(), buf.size(), out);
}

bool DiskCache::Store(uint64_t key, const TranslateResult& result) const {
    const auto path = CachePath(dir_, key);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        const auto buf = Serialize(result);
        f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        if (!f) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

// ---------------------------------------------------------------------------------------------
// ITranslator with cache

namespace {
class Translator final : public ITranslator {
public:
    Translator(const HostCaps& caps_, std::string cache_dir, TranslateOptions options_)
        : caps{caps_}, options{options_} {
        if (!cache_dir.empty()) {
            cache.emplace(std::move(cache_dir));
        }
    }
    bool Parse(const ShaderBinary& binary, ShaderHeader* out, std::string* error) override {
        return ParseControl(binary, out, error);
    }
    bool Translate(const ShaderBinary& binary, const TranslationState& state, ShaderModule* out,
                   std::string* error) override {
        ShaderHeader header{};
        if (!ParseControl(binary, &header, error)) {
            return false;
        }
        const uint64_t key = CacheKey(header, state, options, caps);
        TranslateResult result;
        if (cache && cache->Load(key, &result)) {
            *out = std::move(result.module);
            return true;
        }
        if (!nvnshader::Translate(binary, state, caps, options, &result, error)) {
            return false;
        }
        if (cache) {
            cache->Store(key, result);
        }
        *out = std::move(result.module);
        return true;
    }

    std::shared_ptr<const ShaderModule> TranslateBound(const ShaderBinary& binary,
                                                       const TranslationState& state,
                                                       const TextureResolver& resolver,
                                                       std::string* error) override {
        ShaderHeader header{};
        if (!ParseControl(binary, &header, error)) {
            return nullptr;
        }
        TranslationState base = state;
        base.texture_hints.clear();
        const uint64_t base_key = CacheKey(header, base, options, caps);
        {
            std::scoped_lock lk{mutex};
            if (auto it = variants.find(base_key); it != variants.end()) {
                for (const auto& v : it->second) {
                    if (TextureQueriesMatch(*v, resolver)) {
                        ++stats.hits;
                        return v;
                    }
                }
            }
        }
        // No valid variant: start from the hint-less translation (or an existing one) and add
        // the real hints for the queried slots until they match (usually once).
        TranslationState st = base;
        std::shared_ptr<const ShaderModule> result;
        for (int round = 0; round < 4; ++round) {
            auto m = std::make_shared<ShaderModule>();
            if (!Translate(binary, st, m.get(), error)) {
                return nullptr;
            }
            ++stats.translations;
            result = m;
            if (TextureQueriesMatch(*m, resolver)) {
                break;
            }
            // Merge the new hints with the existing ones.
            for (const TextureHint& h : HintsFromResolver(*m, resolver)) {
                auto same = [&](const TextureHint& x) {
                    return x.cbuf_index == h.cbuf_index && x.cbuf_offset == h.cbuf_offset;
                };
                auto it = std::find_if(st.texture_hints.begin(), st.texture_hints.end(), same);
                if (it != st.texture_hints.end()) {
                    *it = h;
                } else {
                    st.texture_hints.push_back(h);
                }
            }
        }
        std::scoped_lock lk{mutex};
        auto& list = variants[base_key];
        for (const auto& v : list) {
            if (v->state_hash == result->state_hash) {
                return v; // another thread got there first with the same variant
            }
        }
        list.push_back(result);
        ++stats.variants;
        return result;
    }

private:
    HostCaps caps;
    TranslateOptions options;
    std::optional<DiskCache> cache;
    std::mutex mutex;
    std::unordered_map<uint64_t, std::vector<std::shared_ptr<const ShaderModule>>> variants;
    struct {
        std::atomic<uint64_t> hits{0}, translations{0}, variants{0};
    } stats;
};
} // namespace

std::unique_ptr<ITranslator> CreateTranslator(const HostCaps& caps, std::string cache_dir,
                                              TranslateOptions options) {
    return std::make_unique<Translator>(caps, std::move(cache_dir), options);
}

} // namespace nvnshader

// ---------------------------------------------------------------------------------------------
// Functions declared in shader_iface.h

namespace nvnvk::shader {

uint64_t TranslationState::Hash() const {
    nvnshader::Hasher h;
    h.Pod(attrib_types);
    h.Pod(topology);
    if (topology == Topology::Points) {
        h.Pod(point_size);
    }
    h.Pod(depth_minus_one_to_one);
    h.Pod(y_negate);
    h.Pod(viewport_transform);
    h.Pod(xfb_active);
    h.Pod(static_cast<uint32_t>(xfb.size()));
    for (const XfbVarying& x : xfb) {
        h.Pod(x.buffer);
        h.Pod(x.stream);
        h.Pod(x.stride);
        h.Pod(x.offset);
        h.Pod(x.components);
    }
    h.Pod(tess_domain);
    h.Pod(tess_spacing);
    h.Pod(tess_clockwise);
    h.Pod(alpha_test);
    if (alpha_test != CompareOp::Always) {
        h.Pod(alpha_ref);
    }
    h.Pod(dual_source_blend);
    h.Pod(force_early_z);
    h.Pod(color_output_types);
    h.Pod(static_cast<uint32_t>(previous_stage_stores.size()));
    for (uint64_t w : previous_stage_stores) {
        h.Pod(w);
    }
    h.Pod(static_cast<uint32_t>(texture_hints.size()));
    for (const TextureHint& t : texture_hints) {
        h.Pod(t.cbuf_index);
        h.Pod(t.cbuf_offset);
        h.Pod(t.type);
        h.Pod(t.is_integer);
    }
    return h.h;
}

uint64_t HostCaps::Hash() const {
    nvnshader::Hasher h;
    h.Pod(spirv_version);
    const bool flags[] = {float_controls, separate_denorm_behavior, separate_rounding_mode,
                          denorm_preserve_fp16, denorm_preserve_fp32, denorm_flush_fp16,
                          denorm_flush_fp32, signed_zero_inf_nan_preserve_fp16,
                          signed_zero_inf_nan_preserve_fp32, signed_zero_inf_nan_preserve_fp64, int8, int16, int64, float16, float64, storage_8bit,
                          storage_16bit, demote_to_helper, int64_atomics, subgroup_vote,
                          viewport_index_layer_any_stage, typeless_image_loads, native_ndc,
                          scaled_attributes, geometry_streams, sampled_image_array_nonuniform,
                          explicit_workgroup_layout, nv_viewport_array2, nv_geometry_passthrough,
                          transform_feedback, is_nvidia};
    h.Bytes(flags, sizeof(flags));
    h.Pod(min_ssbo_alignment);
    h.Pod(max_user_clip_distances);
    return h.h;
}

bool TextureQueriesMatch(const ShaderModule& m, const TextureResolver& resolver) {
    for (const TextureQuery& q : m.texture_queries) {
        TextureHint bound{};
        if (!resolver || !resolver(q.cbuf_index, q.cbuf_offset, &bound)) {
            continue; // unknown: accept the assumption
        }
        if (q.asked_type && bound.type != q.assumed_type) {
            return false;
        }
        if (q.asked_integer && bound.is_integer != q.assumed_integer) {
            return false;
        }
    }
    return true;
}

std::vector<TextureHint> HintsFromResolver(const ShaderModule& m, const TextureResolver& resolver) {
    std::vector<TextureHint> out;
    for (const TextureQuery& q : m.texture_queries) {
        TextureHint bound{};
        if (resolver && resolver(q.cbuf_index, q.cbuf_offset, &bound)) {
            bound.cbuf_index = q.cbuf_index;
            bound.cbuf_offset = q.cbuf_offset;
            out.push_back(bound);
        }
    }
    return out;
}

uint64_t TextureSignature(const std::vector<TextureHint>& hints) {
    std::vector<TextureHint> sorted = hints;
    std::sort(sorted.begin(), sorted.end(), [](const TextureHint& a, const TextureHint& b) {
        return a.cbuf_index != b.cbuf_index ? a.cbuf_index < b.cbuf_index : a.cbuf_offset < b.cbuf_offset;
    });
    nvnshader::Hasher h;
    for (const TextureHint& t : sorted) {
        h.Pod(t.cbuf_index);
        h.Pod(t.cbuf_offset);
        h.Pod(t.type);
        h.Pod(t.is_integer);
    }
    return h.h;
}

TextureType TextureTypeFromNvnTarget(uint32_t t) {
    switch (t) {
    case 0:
        return TextureType::Color1D;
    case 2:
        return TextureType::Color3D;
    case 3:
        return TextureType::ColorArray1D;
    case 4:
    case 6:
        return TextureType::ColorArray2D;
    case 7:
        return TextureType::Color2DRect;
    case 8:
        return TextureType::ColorCube;
    case 9:
        return TextureType::ColorArrayCube;
    case 10:
        return TextureType::Buffer;
    default:
        return TextureType::Color2D; // 1 2D, 5 2D MS
    }
}

size_t RelocateDescriptors(std::vector<uint32_t>& spirv, uint32_t new_set, uint32_t binding_base) {
    constexpr uint32_t kOpDecorate = 71;
    constexpr uint32_t kDecorationBinding = 33;
    constexpr uint32_t kDecorationDescriptorSet = 34;
    size_t changed = 0;
    size_t i = 5; // SPIR-V header
    while (i < spirv.size()) {
        const uint32_t word = spirv[i];
        const uint32_t count = word >> 16;
        const uint32_t op = word & 0xFFFF;
        if (count == 0 || i + count > spirv.size()) {
            break;
        }
        if (op == kOpDecorate && count >= 4) {
            if (spirv[i + 2] == kDecorationDescriptorSet) {
                spirv[i + 3] = new_set;
                ++changed;
            } else if (spirv[i + 2] == kDecorationBinding) {
                spirv[i + 3] += binding_base;
                ++changed;
            }
        }
        i += count;
    }
    return changed;
}

} // namespace nvnvk::shader
