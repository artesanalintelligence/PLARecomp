// nnshim: SDK startup services kept out of HLE IPC (v1.5 stage 5, family "os").
//   csrng  os::GenerateRandomBytes, crypto::GenerateCryptographicallyRandomBytes -> host BCryptGenRandom;
//          crypto::InitializeCsrng -> no-op (it opened the csrng session).
//   pctl   pctl::detail::ipc::InitializeIpcSessionIfNeeded (called by nninitInitializeSdkModule at startup):
//          no-op. The game imports no nn::pctl function; if some SDK code later needed the service,
//          the A/B run would show it (config "os.pctl": false hands it back to the HLE).
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

#include "abi_common.hpp"

using namespace nnshim;

namespace {

bool g_pctl = true;

void InitOs(const cfg::Value& c) { g_pctl = c.GetBool("os.pctl", true); }
ModuleHooks g_os_module{"os", &InitOs, nullptr};

bool HostRandom(void* buf, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    return false;
#endif
}

pla_action FillRandom(PlaGuest& g) {
    const uint64_t va = g.x(0), n = g.x(1);
    if (!va || n > (16u << 20)) return PLA_CONTINUE;
    std::vector<uint8_t> tmp(n);
    if (n && !HostRandom(tmp.data(), n)) return PLA_CONTINUE;
    if (n && g.write(va, tmp.data(), n) != n) return PLA_CONTINUE;
    CounterAdd("os.random.bytes", n);
    return PLA_SKIP;
}

} // namespace

// void os::GenerateRandomBytes(void* buf, size_t size)
NNSHIM_ADAPTER(Os, nnshim_os_GenerateRandomBytes) {
    PlaGuest g(ctx);
    return FillRandom(g);
}

// void crypto::GenerateCryptographicallyRandomBytes(void* buf, size_t size)
NNSHIM_ADAPTER(Os, nnshim_crypto_GenerateCryptographicallyRandomBytes) {
    PlaGuest g(ctx);
    return FillRandom(g);
}

// void crypto::InitializeCsrng()
NNSHIM_ADAPTER(Os, nnshim_crypto_InitializeCsrng) { return PLA_SKIP; }

// void pctl::detail::ipc::InitializeIpcSessionIfNeeded()
NNSHIM_ADAPTER(Os, nnshim_pctl_InitializeIpcSessionIfNeeded) { return g_pctl ? PLA_SKIP : PLA_CONTINUE; }
