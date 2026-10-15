// Tests the block-linear deswizzle against a byte-by-byte reference implementation.
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

namespace nvnvk {
void UnswizzleLevel(const uint8_t* src, size_t src_size, uint8_t* dst, uint32_t w_bytes, uint32_t rows,
                    uint32_t depth, uint32_t bh_log2, uint32_t bd_log2);
uint64_t BlockLinearLevelSize(uint32_t w_bytes, uint32_t rows, uint32_t depth, uint32_t bh_log2, uint32_t bd_log2);
}

// Reference address (public Tegra formula: 64x8-byte GOBs, blocks of GOBs).
static uint64_t RefAddr(uint32_t x, uint32_t y, uint32_t z, uint32_t w_bytes, uint32_t rows, uint32_t bh, uint32_t bd) {
    const uint32_t gobs_x = (w_bytes + 63) / 64;
    const uint32_t bh_rows = 8u << bh, bdz = 1u << bd;
    const uint32_t blocks_y = (rows + bh_rows - 1) / bh_rows;
    const uint64_t block_bytes = 512ull << (bh + bd);
    const uint64_t block = ((uint64_t)(z / bdz) * blocks_y + y / bh_rows) * gobs_x + x / 64;
    const uint64_t gob = (uint64_t)(z % bdz) * (1u << bh) + (y % bh_rows) / 8;
    const uint32_t gx = x % 64, gy = y % 8;
    const uint32_t in_gob = (gx / 32) * 256 + (gy / 2) * 64 + ((gx % 32) / 16) * 32 + (gy % 2) * 16 + gx % 16;
    return block * block_bytes + gob * 512 + in_gob;
}

int main() {
    int fails = 0, cases = 0;
    const uint32_t ws[] = {4, 16, 60, 64, 100, 256, 4100};
    const uint32_t hs[] = {1, 7, 8, 33, 128, 300};
    const uint32_t ds[] = {1, 3};
    for (uint32_t w : ws) for (uint32_t h : hs) for (uint32_t d : ds) for (uint32_t bh = 0; bh <= 5; ++bh)
        for (uint32_t bd = 0; bd <= (d > 1 ? 2u : 0u); ++bd) {
            ++cases;
            const uint64_t size = nvnvk::BlockLinearLevelSize(w, h, d, bh, bd);
            std::vector<uint8_t> src(size);
            for (size_t i = 0; i < size; ++i) src[i] = (uint8_t)(i * 2654435761u >> 13);
            std::vector<uint8_t> dst((size_t)w * h * d);
            nvnvk::UnswizzleLevel(src.data(), src.size(), dst.data(), w, h, d, bh, bd);
            bool ok = true;
            for (uint32_t z = 0; z < d && ok; ++z) for (uint32_t y = 0; y < h && ok; ++y) for (uint32_t x = 0; x < w; ++x) {
                const uint64_t a = RefAddr(x, y, z, w, h, bh, bd);
                if (a >= size || dst[((size_t)z * h + y) * w + x] != src[a]) { ok = false; break; }
            }
            if (!ok) { ++fails; std::printf("FAIL w=%u h=%u d=%u bh=%u bd=%u\n", w, h, d, bh, bd); }
        }
    std::printf("blocklinear: %d/%d cases passed\n", cases - fails, cases);
    return fails ? 1 : 0;
}
