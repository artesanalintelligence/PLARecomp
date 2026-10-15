// nnshim: access to guest state (AAPCS64 registers and virtual memory).
//
// The "nn shim" implements nn:: functions at the CALL level: it takes the arguments in
// X0-X7 / V0-V7 and leaves the result in X0/W0, just like the SDK function it
// replaces. This interface separates that ABI from the concrete runtime:
//   - v1 inside suyu: PlaGuest (abi_common.hpp) over the mod system's pla_api
//     (recomp/MODDING.md); guest memory is copied with read_mem/write_mem.
//   - standalone executable (v0/v1 without suyu): another implementation over the
//     AOT runtime's GuestContext, with guest memory mapped in the host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nnshim {

class Guest {
public:
    virtual ~Guest() = default;
    virtual uint64_t x(unsigned n) = 0;                 // X0-X30
    virtual void set_x(unsigned n, uint64_t v) = 0;
    virtual size_t read(uint64_t va, void* dst, size_t n) = 0;        // bytes copied
    virtual size_t write(uint64_t va, const void* src, size_t n) = 0;

    uint32_t w(unsigned n) { return static_cast<uint32_t>(x(n)); }
    void ret(uint64_t v) { set_x(0, v); }               // return value (X0/W0)

    template <class T> bool load(uint64_t va, T& out) { return read(va, &out, sizeof(T)) == sizeof(T); }
    template <class T> bool store(uint64_t va, const T& v) { return write(va, &v, sizeof(T)) == sizeof(T); }

    // Guest C string (nn::fs paths: at most 0x301 bytes including the terminator).
    bool cstr(uint64_t va, std::string& out, size_t max = 0x301) {
        out.clear();
        char buf[64];
        while (out.size() < max) {
            size_t got = read(va + out.size(), buf, sizeof buf);
            if (got == 0) return false;
            for (size_t i = 0; i < got; ++i) {
                if (buf[i] == '\0') return true;
                out.push_back(buf[i]);
            }
        }
        return false;
    }
};

} // namespace nnshim
