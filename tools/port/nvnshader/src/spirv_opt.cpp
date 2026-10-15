// SPDX-License-Identifier: GPL-3.0-or-later
// Optional SPIR-V optimization with the SPIRV-Tools optimizer (Vulkan SDK C API).
// Preserves bindings and interface (inputs/outputs) so the descriptor layout and VS/FS
// matching are not broken.

#include "nvnshader/nvnshader.h"

#if NVNSHADER_WITH_SPIRV_OPT
#include <spirv-tools/libspirv.h>
#endif

namespace nvnshader {

bool SpirvOptimizerAvailable() {
    return NVNSHADER_WITH_SPIRV_OPT != 0;
}

bool OptimizeSpirv(std::vector<uint32_t>& spirv, OptLevel level, std::string* error) {
    if (level == OptLevel::None) {
        return true;
    }
#if NVNSHADER_WITH_SPIRV_OPT
    spv_optimizer_t* opt = spvOptimizerCreate(SPV_ENV_VULKAN_1_3);
    const char* flag = level == OptLevel::Size ? "-Os" : "-O";
    if (!spvOptimizerRegisterPassesFromFlagsWhilePreservingTheInterface(opt, &flag, 1)) {
        spvOptimizerDestroy(opt);
        if (error) {
            *error = "spirv-opt: could not register the pass";
        }
        return false;
    }
    spv_optimizer_options options = spvOptimizerOptionsCreate();
    spvOptimizerOptionsSetRunValidator(options, false); // the input SPIR-V is already valid
    spvOptimizerOptionsSetPreserveBindings(options, true);
    spv_validator_options vopts = spvValidatorOptionsCreate();
    spvValidatorOptionsSetScalarBlockLayout(vopts, true);
    spvOptimizerOptionsSetValidatorOptions(options, vopts);
    spv_binary out = nullptr;
    const spv_result_t r = spvOptimizerRun(opt, spirv.data(), spirv.size(), &out, options);
    if (r == SPV_SUCCESS && out) {
        spirv.assign(out->code, out->code + out->wordCount);
    } else if (error) {
        *error = "spirv-opt failed: " + std::to_string(static_cast<int>(r));
    }
    spvBinaryDestroy(out);
    spvValidatorOptionsDestroy(vopts);
    spvOptimizerOptionsDestroy(options);
    spvOptimizerDestroy(opt);
    return r == SPV_SUCCESS;
#else
    if (error) {
        *error = "nvnshader built without SPIRV-Tools (NVNSHADER_WITH_SPIRV_OPT=OFF)";
    }
    return false;
#endif
}

} // namespace nvnshader
