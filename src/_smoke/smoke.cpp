// Toolchain smoke test: trivial functions, not game code.
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace smoke {

struct Vec3 {
    float x, y, z;
};

int add(int a, int b) { return a + b; }

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

std::uint32_t hashFnv1a(const char* s) {
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0, n = std::strlen(s); i < n; ++i)
        h = (h ^ static_cast<std::uint8_t>(s[i])) * 16777619u;
    return h;
}

static_assert(std::is_trivially_copyable<Vec3>::value, "");

}  // namespace smoke
