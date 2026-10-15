// SPDX-License-Identifier: GPL-3.0-or-later
//
// nvnshader: NVN (Maxwell) to SPIR-V shader translator for the PLA v2 port.
// Implements nvnvk::shader::ITranslator (tools/port/nvnvk/include/nvnvk/shader_iface.h) on top of
// suyu's shader_recompiler, isolated from video_core/core.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nvnshader/log.h"
#include "nvnvk/shader_iface.h"

namespace nvnshader {

using nvnvk::shader::AttribType;
using nvnvk::shader::CompareOp;
using nvnvk::shader::Descriptor;
using nvnvk::shader::DescriptorType;
using nvnvk::shader::HostCaps;
using nvnvk::shader::ITranslator;
using nvnvk::shader::ShaderBinary;
using nvnvk::shader::ShaderHeader;
using nvnvk::shader::ShaderModule;
using nvnvk::shader::Stage;
using nvnvk::shader::TextureType;
using nvnvk::shader::Topology;
using nvnvk::shader::TranslationState;

/// Output format version (part of the cache key: bump it if the SPIR-V changes).
inline constexpr uint32_t kTranslatorVersion = 5;  // 5: dynamic cbuf reads clamped before reading (V2-CORE)

using nvnvk::shader::TextureHint;
using nvnvk::shader::TextureQuery;
using nvnvk::shader::TextureQueriesMatch;
using nvnvk::shader::TextureResolver;
using nvnvk::shader::HintsFromResolver;
using nvnvk::shader::TextureSignature;
using nvnvk::shader::TextureTypeFromNvnTarget;

/// SPIR-V optimization with SPIRV-Tools (if nvnshader was built with NVNSHADER_WITH_SPIRV_OPT).
enum class OptLevel : uint8_t { None, Size, Performance };

struct TranslateOptions {
    /// Runs VerificationPass on the IR (slow).
    bool verify_ir = false;
    /// Runs the SPIR-V through spirv-opt (-Os / -O), preserving bindings and interface.
    OptLevel optimize = OptLevel::None;
};

/// true if nvnshader was built with the SPIRV-Tools optimizer.
bool SpirvOptimizerAvailable();
/// Optimizes in place. false on failure (the SPIR-V is left unchanged).
bool OptimizeSpirv(std::vector<uint32_t>& spirv, OptLevel level, std::string* error);

struct TranslateResult {
    ShaderModule module;
};

/// Parses the NVN control section (no translation).
bool ParseControl(const ShaderBinary& binary, ShaderHeader* out, std::string* error);

/// Direct translation (no cache). Thread-safe.
bool Translate(const ShaderBinary& binary, const TranslationState& state, const HostCaps& caps,
               const TranslateOptions& options, TranslateResult* out, std::string* error);

/// Cache key: hash of code + state (including texture hints) + host profile
/// + translator version.
uint64_t CacheKey(const ShaderHeader& header, const TranslationState& state,
                  const TranslateOptions& options, const HostCaps& caps);

/// On-disk cache of translated modules: one file per key in `dir` (<key>.nvsh).
class DiskCache {
public:
    explicit DiskCache(std::string dir);
    bool Load(uint64_t key, TranslateResult* out) const;
    bool Store(uint64_t key, const TranslateResult& result) const;
    const std::string& Dir() const {
        return dir_;
    }

private:
    std::string dir_;
};

/// Result serialization (on-disk cache format).
std::vector<uint8_t> Serialize(const TranslateResult& result);
bool Deserialize(const uint8_t* data, size_t size, TranslateResult* out);

/// Creates an ITranslator with an optional cache (empty cache_dir = no disk cache).
std::unique_ptr<ITranslator> CreateTranslator(const HostCaps& caps, std::string cache_dir = {},
                                              TranslateOptions options = {});

/// 64-bit FNV-1a hash (stable across platforms).
uint64_t Hash64(const void* data, size_t size, uint64_t seed = 0xcbf29ce484222325ULL);

} // namespace nvnshader
