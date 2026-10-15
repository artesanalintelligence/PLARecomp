// nn::Result: (description << 9) | module. 0 = success. Public values (switchbrew).
#pragma once

#include <cstdint>

namespace nnshim {

using Result = uint32_t;

constexpr Result MakeResult(uint32_t module, uint32_t description) {
    return (description << 9) | module;
}

constexpr Result ResultSuccess = 0;

namespace fs_result {
constexpr uint32_t kModule = 2;
constexpr Result PathNotFound        = MakeResult(kModule, 1);     // 2002-0001
constexpr Result PathAlreadyExists   = MakeResult(kModule, 2);     // 2002-0002
constexpr Result TargetLocked        = MakeResult(kModule, 7);     // 2002-0007
constexpr Result DirectoryNotEmpty   = MakeResult(kModule, 8);     // 2002-0008
constexpr Result UsableSpaceNotEnough= MakeResult(kModule, 35);    // 2002-0035
constexpr Result TargetNotFound      = MakeResult(kModule, 1002);  // 2002-1002 (save does not exist)
constexpr Result OutOfRange          = MakeResult(kModule, 3005);  // 2002-3005
constexpr Result InvalidArgument     = MakeResult(kModule, 6001);  // 2002-6001
constexpr Result InvalidPath         = MakeResult(kModule, 6002);  // 2002-6002 (approx.)
constexpr Result InvalidOperationForOpenMode = MakeResult(kModule, 6200);
constexpr Result FileExtensionWithoutOpenModeAllowAppend = MakeResult(kModule, 6201);
constexpr Result MountNameAlreadyExists = MakeResult(kModule, 6061);
constexpr Result NotMounted          = MakeResult(kModule, 6062);  // 2002-6062 (approx.)
constexpr Result InvalidHandle       = MakeResult(kModule, 6063);  // approx.
} // namespace fs_result

} // namespace nnshim
