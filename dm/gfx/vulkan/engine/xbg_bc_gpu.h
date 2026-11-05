// [nonadreno] Host-side planning of the chunked texture upload and the GPU BC decode (issue #6). No Vulkan calls: the engine
// (xbg_game.cpp BcUpload, bcgpu) records the work, engine/tests/bc_gpu_decode_test.cpp runs this planning together with the
// GPU decoder compiled for the host (xbg_bc_gpu.glsl through tests/glsl_shim.h) and compares with the CPU decoder.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>
#include <vector>

// A band of one subresource: slices z0..z0+nz, block rows by0..by0+nby (whole rows of blocks; for plain formats a block is a
// pixel). out_block: bytes one block takes in the staging (decoded: 16 pixels * bpp; passthrough: block bytes).
struct XbgBand { uint32_t z0, nz, by0, nby; };
static inline void xbg_upload_bands(uint32_t bx, uint32_t by, uint32_t d, size_t out_block, size_t cap, std::vector<XbgBand> &out) {
    out.clear();
    if(!bx || !by || !d || !out_block)return;
    const size_t row = size_t(bx) * out_block, slice = row * by;
    if(slice <= cap){
        const uint32_t k = uint32_t(std::min<size_t>(d, std::max<size_t>(1, cap / slice)));
        for(uint32_t z = 0; z < d; z += k)out.push_back({z, std::min(k, d - z), 0, by});
        return;
    }
    const uint32_t r = uint32_t(std::min<size_t>(by, std::max<size_t>(1, cap / row)));
    for(uint32_t z = 0; z < d; z++)for(uint32_t y = 0; y < by; y += r)out.push_back({z, 1, y, std::min(r, by - y)});
}

// The buffer-to-image copy of a band whose staging rows are bx blocks wide (padded) and nby block rows high, into a W x H
// subresource with bw x bh blocks (plain formats 1 x 1; native or decoded BC 4 x 4: decoded rows are padded to whole blocks).
struct XbgCopy { size_t offset; uint32_t row_length, image_height; int32_t x, y, z; uint32_t w, h, d; };
static inline XbgCopy xbg_band_copy(size_t offset, const XbgBand &b, uint32_t bx, uint32_t W, uint32_t H, uint32_t bw, uint32_t bh) {
    return {offset, bx * bw, b.nby * bh, 0, int32_t(b.by0 * bh), int32_t(b.z0), W, std::min(b.nby * bh, H - b.by0 * bh), b.nz};
}

// One GPU decode batch: the input buffer is 8 words per region followed by the raw blocks; the output is decoded pixels, each
// region at a 16-byte aligned offset, rows padded to whole blocks (bufferRowLength = blocks_x * 4, bufferImageHeight =
// blocks_y * 4). Region words: first block, blocks_x, blocks_x * blocks_y, blocks_y, input word of its first block, output
// word of its first pixel, output words per pixel row (blocks_x * bpp), block count. Matches xbg_bc_decode.comp.
struct XbgBcGpuBatch {
    std::vector<uint32_t> table; std::vector<uint8_t> raw; size_t out = 0; uint32_t blocks = 0;
    static constexpr uint32_t region_words = 8;
    void clear(){table.clear();raw.clear();out=0;blocks=0;}
    size_t regions() const {return table.size() / region_words;}
    static size_t align16(size_t n){return (n + 15) & ~size_t(15);}
    // Whether a region of raw_bytes / out_bytes still fits the scratch buffers (raw_cap counts the region table too).
    bool fits(size_t raw_bytes, size_t out_bytes, size_t raw_cap, size_t out_cap, size_t max_regions, size_t max_blocks, uint32_t nblocks) const {
        if(!regions())return true;
        return regions() < max_regions && (regions() + 1) * region_words * 4 + align16(raw.size()) + raw_bytes <= raw_cap &&
            align16(out) + out_bytes <= out_cap && size_t(blocks) + nblocks <= max_blocks;
    }
    // Appends bx * by * nz blocks (tight rows) and returns the byte offset of its decoded pixels in the output.
    size_t add(const void *blocks_data, uint32_t bx, uint32_t by, uint32_t nz, uint32_t block_bytes, uint32_t bpp) {
        const size_t raw_at = align16(raw.size()), n = size_t(bx) * by * nz, out_at = align16(out);
        raw.resize(raw_at + n * block_bytes);if(n)memcpy(raw.data() + raw_at, blocks_data, n * block_bytes);
        const uint32_t r[region_words] = {blocks, bx, bx * by, by, uint32_t(raw_at / 4), uint32_t(out_at / 4), bx * bpp, uint32_t(n)};
        table.insert(table.end(), r, r + region_words);
        out = out_at + size_t(bx) * 4 * by * 4 * nz * bpp; blocks += uint32_t(n);
        return out_at;
    }
    size_t input_bytes() const {return table.size() * 4 + raw.size();}
    // The input words: the table (input offsets moved past it), then the raw blocks.
    void write_input(void *dst) const {
        uint32_t *w = static_cast<uint32_t*>(dst);
        memcpy(w, table.data(), table.size() * 4);
        for(size_t i = 0; i < regions(); i++)w[i * region_words + 4] += uint32_t(table.size());
        if(!raw.empty())memcpy(reinterpret_cast<uint8_t*>(w + table.size()), raw.data(), raw.size());
    }
};
