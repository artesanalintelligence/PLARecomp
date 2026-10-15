// Maxwell block-linear deswizzle (no Vulkan dependencies; also used by the test).
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace nvnvk {

// ---------------------------------------------------------------- block-linear (Maxwell)
// GOB = 64 bytes x 8 rows (512 bytes). Block = 1 GOB wide x 2^bh GOBs tall x 2^bd deep.
static inline uint32_t GobSwizzle(uint32_t x, uint32_t y) {
    return ((x & 32) << 3) | ((y & 6) << 5) | ((x & 16) << 1) | ((y & 1) << 4) | (x & 15);
}

uint32_t AdjustBlockLog2(uint32_t log2, uint32_t extent, uint32_t gob_extent) {
    while (log2 > 0 && extent <= (1u << (log2 - 1)) * gob_extent) --log2;
    return log2;
}

uint64_t BlockLinearLevelSize(uint32_t w_bytes, uint32_t rows, uint32_t depth, uint32_t bh, uint32_t bd) {
    const uint64_t gobs_x = (w_bytes + 63) / 64;
    const uint64_t rows_per_block = 8ull << bh;
    const uint64_t blocks_y = (rows + rows_per_block - 1) / rows_per_block;
    const uint64_t blocks_z = (depth + (1ull << bd) - 1) >> bd;
    return gobs_x * blocks_y * blocks_z * (512ull << (bh + bd));
}

void UnswizzleLevel(const uint8_t* src, size_t src_size, uint8_t* dst, uint32_t w_bytes, uint32_t rows,
                    uint32_t depth, uint32_t bh, uint32_t bd) {
    const uint32_t gobs_x = (w_bytes + 63) / 64;
    const uint32_t rows_per_block = 8u << bh;
    const uint32_t blocks_y = (rows + rows_per_block - 1) / rows_per_block;
    const uint64_t block_bytes = 512ull << (bh + bd);
    const uint32_t dmask = (1u << bd) - 1;
    for (uint32_t z = 0; z < depth; ++z) {
        for (uint32_t y = 0; y < rows; ++y) {
            const uint64_t row_base = ((uint64_t)(z >> bd) * blocks_y + y / rows_per_block) * gobs_x * block_bytes +
                                      ((uint64_t)(((z & dmask) << bh) + ((y % rows_per_block) >> 3)) * 512);
            uint8_t* out = dst + ((uint64_t)z * rows + y) * w_bytes;
            for (uint32_t x = 0; x < w_bytes; x += 16) {
                const uint64_t off = row_base + (uint64_t)(x >> 6) * block_bytes + GobSwizzle(x & 63, y & 7);
                const uint32_t n = std::min(16u, w_bytes - x);
                if (off + n <= src_size) std::memcpy(out + x, src + off, n);
            }
        }
    }
}

}  // namespace nvnvk
