// SPDX-License-Identifier: MIT
//
// shader_iface.h — contract between the NVN→Vulkan backend (V2-CORE, tools/port/nvnvk) and the
// Maxwell→SPIR-V shader translator (V2-SHADERS, tools/port/nvnshader).
//
// Proposed by V2-SHADERS (2026-10-04). Version 3 (v2: texture hints and queries, detailed
// float controls in HostCaps; v3: TranslateBound / TextureResolver and XFB from the control). Changes: record them in
// docs/port/REQUESTS.md (V2-CORE / V2-SHADERS sections) and bump kShaderIfaceVersion.
//
// This header depends on neither Vulkan nor suyu: only the STL. The implementation lives in
// nvnshader (GPL-3.0 because of the suyu recompiler); the backend uses it through
// nvnshader::CreateTranslator() (tools/port/nvnshader/include/nvnshader/nvnshader.h).
//
// Intended backend flow:
//   nvnProgramSetShaders(program, n, NVNshaderData*)            (per stage)
//     -> ShaderBinary{control, data (pool+offset resolved to a host pointer)}
//     -> ITranslator::Parse()      (cheap: stage, sizes, workgroup; no translation)
//   on draw / when creating the VkPipeline:
//     -> ITranslator::Translate(binary, state)  (disk-cached; key = code hash +
//        TranslationState::Hash() + host profile)
//     -> VkShaderModule from ShaderModule::spirv, descriptor set layout from
//        ShaderModule::descriptors (one set per stage; see "Descriptor layout").
//
// Descriptor layout (details in docs/port/SHADERS.md §6):
//   * The SPIR-V comes out with DescriptorSet = 0 and consecutive bindings from 0 in this order:
//     uniform buffers, storage buffers, texture buffers, image buffers, textures (combined
//     image sampler), images. RelocateDescriptors() changes set and base without retranslating.
//   * Recommended: set = stage index (VS 0, TCS 1, TES 2, GS 3, FS 4); compute: set 0.
//   * Each Descriptor says where the resource comes from in NVN terms:
//       - UniformBuffer: Maxwell cbuf_index. NVN: c0 = driver constants, c1 = compiler
//         constants (already folded into the SPIR-V), c2 = texture handles; the game's UBOs
//         (nvnCommandBufferBindUniformBuffer(stage, i, ...)) are cbuf_index = i + 3 [V].
//         If cbuf_index = 0, the shader reads c0 (compute SSBO/UBO addresses): the backend
//         must provide c0 with the bound GPU addresses and sizes at those offsets.
//       - StorageBuffer: cbuf_index 0 and cbuf_offset = stage_base + 0x10 * slot, with stage_base
//         0x110 (VS), 0x210 (TCS), 0x310 (TES), 0x410 (GS), 0x510 (FS), 0x310 (CS): the slot is the
//         nvnCommandBufferBindStorageBuffer one. In compute, cbuf_offset = 0x210 + 0x10 * i is
//         UBO i (NVN does not use hardware cbufs for compute UBOs) [V].
//       - Textures/images: the handle is at cbuf[cbuf_index][cbuf_offset]. cbuf 2 = handles from
//         nvnCommandBufferBindTexture: slot = (cbuf_offset - 0x20) / 8 [V]; images: slot = (cbuf_offset - 0x120) / 8 [V].
//         Any other cbuf_index is a "bindless" handle read from a game UBO on each draw.
//   * Push constants: if ShaderModule::uses_render_area, a vec4 (width, height, 0, 0) at
//     offset 0 (only when the game disables the viewport transform). Nothing else.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nvnvk::shader {

inline constexpr uint32_t kShaderIfaceVersion = 3;

/// Value of the control's "stage" field (+0x714). Verified in the RomFS: Vertex = 0,
/// Fragment = 1, Compute = 5. Geometry/TessControl/TessEval [S]: the game does not use them.
enum class Stage : uint8_t {
    Vertex = 0,
    Fragment = 1,
    Geometry = 2,
    TessControl = 3,
    TessEval = 4,
    Compute = 5,
};

/// What nvnProgramSetShaders receives in each NVNshaderData, already in host memory.
struct ShaderBinary {
    const void* control = nullptr; ///< NVNshaderData::control (game CPU pointer)
    size_t control_size = 0;       ///< 0 = read from the control itself (+0x6F0)
    const void* data = nullptr;    ///< bytes at NVNshaderData::data (GPU address → host)
    size_t data_size = 0;          ///< 0 = read from the control (+0x704)
};

/// NVN control metadata (untranslated). See docs/port/SHADERS.md §3.
struct ShaderHeader {
    Stage stage = Stage::Vertex;
    uint32_t code_offset = 0;     ///< 0x30 for graphics (NVN header), 0 for compute
    uint32_t code_size = 0;       ///< code bytes (includes the header and the SPH)
    uint32_t const_offset = 0;    ///< offset in data of bank c1 (compiler constants)
    uint32_t const_size = 0;
    uint32_t data_size = 0;
    uint32_t num_registers = 0;
    std::array<uint32_t, 3> workgroup_size{}; ///< compute only
    uint32_t shared_memory_size = 0;          ///< compute only
    uint32_t xfb_buffers = 0;                 ///< VS: declared transform feedback buffers
    uint64_t code_hash = 0;                   ///< hash of data[0, data_size)
};

enum class AttribType : uint8_t { Float, SInt, UInt, SScaled, UScaled, Disabled };
enum class CompareOp : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class Topology : uint8_t { Points, Lines, LinesAdjacency, Triangles, TrianglesAdjacency, Patches };
enum class TessDomain : uint8_t { Isolines, Triangles, Quads };
enum class TessSpacing : uint8_t { Equal, FractionalOdd, FractionalEven };

struct XfbVarying {
    uint8_t buffer = 0;
    uint8_t stream = 0;
    uint16_t stride = 0;
    uint16_t offset = 0;
    uint8_t components = 0; ///< 0 = attribute not captured
};

enum class TextureType : uint8_t {
    Color1D, ColorArray1D, Color2D, ColorArray2D, Color3D, ColorCube, ColorArrayCube, Buffer,
    Color2DRect,
};

/// Hint for a bound texture (the translator cannot see the real texture). The recompiler asks
/// for the type in three cases (textureSize, 2D sample that might be RECT, 1D fetch that might be a
/// buffer) and whether the format is integer (int/uint result; required for image atomics).
/// The handle is at cbuf[cbuf_index][cbuf_offset] (c2 + 0x20 + 8*slot for BindTexture).
/// No hint: Color2D and non-integer. The backend compares ShaderModule::texture_queries with what
/// is bound at draw time and, on mismatch, retranslates with the correct hint (different key).
struct TextureHint {
    uint32_t cbuf_index = 2;
    uint32_t cbuf_offset = 0;
    TextureType type = TextureType::Color2D;
    bool is_integer = false;
};

/// Question the recompiler asked about a texture and what it assumed.
struct TextureQuery {
    uint32_t cbuf_index = 0;
    uint32_t cbuf_offset = 0;
    bool asked_type = false;
    bool asked_integer = false;
    TextureType assumed_type = TextureType::Color2D;
    bool assumed_integer = false;
};

/// NVN state that changes the generated SPIR-V. Everything else (blend, depth/stencil,
/// rasterization, render target formats except integer ones) goes into the VkPipeline, not here.
struct TranslationState {
    // --- Vertex (or last geometry stage) ---
    /// Type of each vertex attribute (nvnVertexAttribStateSetFormat): Float for
    /// UNORM/SNORM/FLOAT, SInt/UInt for integer formats, *Scaled for *SCALED.
    std::array<AttribType, 32> attrib_types{};
    Topology topology = Topology::Triangles;
    float point_size = 1.0f;            ///< nvnCommandBufferSetPointSize (Points only)
    bool depth_minus_one_to_one = false; ///< NVN_DEPTH_MODE_NEAR_IS_MINUS_W (converts z)
    bool y_negate = false;               ///< lower-left window origin
    bool viewport_transform = true;      ///< false = the VS writes window coordinates
    /// Transform feedback active (between nvnCommandBufferBeginTransformFeedback and End). If `xfb`
    /// is empty, the layout is read from the shader control (ShaderHeader::xfb_buffers > 0).
    bool xfb_active = false;
    /// Explicit XFB layout, indexed by suyu attribute (32 = generic 0.x, 36 = generic
    /// 1.x...; 256 entries). Normally empty: the control provides it.
    std::vector<XfbVarying> xfb;
    // --- Tessellation ---
    TessDomain tess_domain = TessDomain::Triangles;
    TessSpacing tess_spacing = TessSpacing::Equal;
    bool tess_clockwise = false;
    // --- Fragment ---
    CompareOp alpha_test = CompareOp::Always; ///< nvnColorStateSetAlphaTest
    float alpha_ref = 0.0f;                   ///< nvnCommandBufferSetAlphaRef
    bool dual_source_blend = false;
    bool force_early_z = false;
    /// Output type of each render target (Float except integer formats).
    std::array<AttribType, 8> color_output_types{};
    // --- Between stages ---
    /// Outputs of the previous stage (that stage's ShaderModule::stores, 8 words).
    /// Empty = "all" (like suyu with no previous stage). Affects which inputs are read as
    /// default constants.
    std::vector<uint64_t> previous_stage_stores;
    // --- Textures ---
    std::vector<TextureHint> texture_hints;

    /// Stable hash of all fields (cache key).
    uint64_t Hash() const;
};

enum class DescriptorType : uint8_t {
    UniformBuffer,   ///< VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    StorageBuffer,   ///< VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
    TextureBuffer,   ///< VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
    ImageBuffer,     ///< VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
    SampledTexture,  ///< VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
    StorageImage,    ///< VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
};


struct Descriptor {
    DescriptorType type = DescriptorType::UniformBuffer;
    uint32_t binding = 0;  ///< binding in the SPIR-V as emitted (before relocation)
    uint32_t count = 1;    ///< > 1: array (dynamic index)
    uint32_t cbuf_index = 0;
    uint32_t cbuf_offset = 0;
    uint32_t size = 0;     ///< UBO: bytes used (64 KiB if the index is dynamic)
    bool is_written = false;
    bool is_read = false;
    // Textures / images
    TextureType texture_type = TextureType::Color2D;
    bool is_depth = false;
    bool is_multisample = false;
    bool is_integer = false;
    uint32_t image_format = 0; ///< Shader::ImageFormat (0 = typeless)
    bool has_secondary = false; ///< handle = (cbuf << shift) | (cbuf2 << shift2)
    uint32_t shift_left = 0;
    uint32_t secondary_cbuf_index = 0;
    uint32_t secondary_cbuf_offset = 0;
    uint32_t secondary_shift_left = 0;
    uint32_t size_shift = 0;    ///< arrays: log2 of the stride between handles
};

struct ShaderModule {
    Stage stage = Stage::Vertex;
    std::vector<uint32_t> spirv;
    std::vector<Descriptor> descriptors;
    /// Generic attributes (32 × 4 components, bit = attribute*4 + component).
    std::array<uint64_t, 2> input_mask{};
    std::array<uint64_t, 2> output_mask{};
    /// Full output mask (512 bits, suyu IR::Attribute). This is what to pass in
    /// TranslationState::previous_stage_stores when translating the next stage.
    std::array<uint64_t, 8> stores{};
    uint32_t color_outputs_mask = 0; ///< FS: render targets written
    bool writes_depth = false;
    bool writes_sample_mask = false;
    bool uses_render_area = false;   ///< push constant vec4 at offset 0
    bool uses_layer = false;
    bool uses_viewport_index = false;
    uint32_t clip_distances_mask = 0;
    uint32_t local_memory_size = 0;
    uint32_t shared_memory_size = 0;
    std::array<uint32_t, 3> workgroup_size{};
    uint64_t code_hash = 0;
    uint64_t state_hash = 0;
    std::string warnings; ///< recompiler warnings (non-fatal)
    /// Texture type/format queries made during translation (see TextureHint).
    std::vector<TextureQuery> texture_queries;
};

/// Vulkan device capabilities that change the SPIR-V (filled from
/// VkPhysicalDevice*Features/Properties). Defaults are those of an RTX 40xx.
struct HostCaps {
    uint32_t spirv_version = 0x00010600;
    // Float controls (VkPhysicalDeviceFloatControlsProperties). Values from an RTX 4070 SUPER with
    // driver 616.92: no flush-to-zero, denormals preserved only in fp16.
    bool float_controls = true;
    bool separate_denorm_behavior = true;   ///< denormBehaviorIndependence == ALL
    bool separate_rounding_mode = true;     ///< roundingModeIndependence == ALL
    bool denorm_preserve_fp16 = true;
    bool denorm_preserve_fp32 = false;
    bool denorm_flush_fp16 = false;
    bool denorm_flush_fp32 = false;
    bool signed_zero_inf_nan_preserve_fp16 = true;
    bool signed_zero_inf_nan_preserve_fp32 = true;
    bool signed_zero_inf_nan_preserve_fp64 = true;
    bool int8 = true, int16 = true, int64 = true, float16 = true, float64 = true;
    bool storage_8bit = true, storage_16bit = true;
    bool demote_to_helper = true;
    bool int64_atomics = true;
    bool subgroup_vote = true;
    bool viewport_index_layer_any_stage = true; ///< VK_EXT_shader_viewport_index_layer
    bool typeless_image_loads = true;            ///< shaderStorageImageReadWithoutFormat
    bool native_ndc = true;                      ///< VK_EXT_depth_clip_control
    bool scaled_attributes = false;              ///< native *SCALED formats
    bool geometry_streams = true;
    bool sampled_image_array_nonuniform = true;
    bool explicit_workgroup_layout = true;
    bool nv_viewport_array2 = false;
    bool nv_geometry_passthrough = false;
    bool transform_feedback = true;
    bool is_nvidia = true;
    uint32_t min_ssbo_alignment = 16;
    uint32_t max_user_clip_distances = 8;

    uint64_t Hash() const;
};

/// What the backend knows about the bound texture whose handle is at cbuf[cbuf_index][cbuf_offset]
/// in the current draw (c2: slot = (offset - 0x20) / 8; other cbuf: bindless handle read from the UBO).
/// Fills `out->type` (TextureTypeFromNvnTarget) and `out->is_integer` (UINT/SINT format) and
/// returns true; false = unknown (the translator's assumption is accepted).
using TextureResolver = std::function<bool(uint32_t cbuf_index, uint32_t cbuf_offset, TextureHint* out)>;

/// true if all of `m`'s assumptions (texture_queries) match what is bound.
bool TextureQueriesMatch(const ShaderModule& m, const TextureResolver& resolver);

/// Hints for the slots `m` queried, with the resolver's real values.
std::vector<TextureHint> HintsFromResolver(const ShaderModule& m, const TextureResolver& resolver);

/// Stable signature of a hint set (order-independent).
uint64_t TextureSignature(const std::vector<TextureHint>& hints);

/// NVN target (TextureBuilder +0x24 / TextureView +0x20: 0 1D, 1 2D, 2 3D, 3 1D array,
/// 4 2D array, 5 2D MS, 6 2D MS array, 7 rectangle, 8 cubemap, 9 cubemap array, 10 buffer).
TextureType TextureTypeFromNvnTarget(uint32_t nvn_target);

class ITranslator {
public:
    virtual ~ITranslator() = default;
    /// Reads the control. false if it is not a recognizable NVN shader.
    virtual bool Parse(const ShaderBinary& binary, ShaderHeader* out, std::string* error) = 0;
    /// Translates one stage to SPIR-V. Thread-safe (each call uses its own pools).
    virtual bool Translate(const ShaderBinary& binary, const TranslationState& state,
                           ShaderModule* out, std::string* error) = 0;
    /// Returns the shader variant matching the bound textures (at draw time or when
    /// binding textures): searches the already translated variants of (code, state without hints)
    /// for the first whose texture_queries match the resolver; if none, translates with the real
    /// hints and stores it (memory + disk). Ignores state.texture_hints. Thread-safe. The pointer is
    /// stable while the translator lives: usable as part of the VkPipeline key
    /// (ShaderModule::state_hash already distinguishes variants). nullptr = error.
    virtual std::shared_ptr<const ShaderModule> TranslateBound(const ShaderBinary& binary,
                                                               const TranslationState& state,
                                                               const TextureResolver& resolver,
                                                               std::string* error) {
        (void)binary, (void)state, (void)resolver;
        if (error) {
            *error = "TranslateBound not implemented";
        }
        return nullptr;
    }
};

/// Rewrites the SPIR-V DescriptorSet/Binding decorations: set = new_set and
/// binding = original_binding + binding_base. Returns how many decorations changed.
size_t RelocateDescriptors(std::vector<uint32_t>& spirv, uint32_t new_set, uint32_t binding_base);

} // namespace nvnvk::shader
