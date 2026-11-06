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

// [nonadreno] Known-answer blocks of the GPU decode self-test (06.11; bcgpu::selftest in xbg_game.cpp runs them through each new
// decode pipeline on the device and compares with the CPU decoder, engine/tests/bc_gpu_decode_test.cpp through the host build). First
// the 20 BC6H mode-1 blocks the Adreno 750 decoded with zero pixel rows 1-3 (Mi 14, DM_GFX_SIM_GPU=mali, BUGS_AND_FIXES §2.24), then
// deterministic pseudo-random blocks whose mode field cycles through every BC7 mode (8 = reserved) and every BC6H mode code (valid and
// reserved); other formats random.
static const uint8_t xbg_bc_kat_bc6h_mode1[20][16] = {
    {0xd9,0x23,0x0c,0x32,0x27,0xea,0x78,0x85,0x8c,0x1f,0x8c,0x9f,0xaa,0xcb,0xf1,0x86}, {0xa5,0x71,0x45,0x17,0x7f,0x8e,0x31,0xa8,0xe6,0x04,0x8c,0xa0,0x12,0x4c,0xe2,0x3d},
    {0x9d,0xc3,0x4c,0x21,0x26,0xe1,0x34,0xf8,0x63,0x52,0x87,0x1f,0xfb,0xf8,0x8f,0xff}, {0xc9,0xe0,0xc0,0x0d,0x79,0x0a,0x1a,0x47,0xad,0x09,0x50,0x60,0x36,0xf8,0x63,0x3e},
    {0x7d,0xc3,0x4b,0x24,0x87,0x3b,0xbf,0x19,0xdc,0x5a,0x70,0x41,0x94,0x3a,0xe5,0x53}, {0x6d,0xc1,0xc3,0x09,0x79,0x92,0x89,0x86,0x27,0x89,0xb0,0x1f,0x33,0xa0,0x03,0x3a},
    {0xc9,0x25,0x96,0x56,0x54,0x74,0xb1,0x45,0x13,0x4a,0x03,0x2f,0xf5,0xac,0xed,0x7b}, {0x5d,0xd2,0xc6,0x27,0x6b,0xd6,0x19,0x44,0x77,0x98,0x0a,0x4e,0x21,0x96,0x72,0x6e},
    {0xa1,0x60,0x40,0x0a,0x78,0x00,0x82,0x67,0x77,0xe1,0x02,0x80,0x03,0x5c,0xe3,0x2f}, {0xe1,0xd4,0x91,0x43,0x08,0x64,0x18,0x62,0x1e,0x01,0xb0,0x7f,0xb4,0x4d,0xf7,0x1c},
    {0xc5,0xd4,0xd1,0x4d,0xe8,0x3b,0x47,0x1b,0xfb,0xa3,0xb9,0x1e,0xa2,0xab,0xe0,0xca}, {0x01,0x30,0x00,0x00,0x29,0x00,0xb0,0x02,0x80,0xc5,0x00,0x00,0x00,0x0c,0xe0,0x00},
    {0xc9,0x60,0x40,0x0c,0x97,0x98,0x2a,0x47,0x95,0x2a,0xd0,0x1f,0xb4,0xa1,0x0e,0x18}, {0x61,0x43,0xcb,0x32,0xb8,0x03,0x2f,0xdb,0x03,0x02,0x18,0x79,0x09,0x4d,0xec,0xc7},
    {0x25,0xf4,0xce,0x39,0xb0,0xd3,0x36,0x5e,0x6b,0xc2,0x31,0xbf,0x0b,0xdc,0xe1,0xa6}, {0xd1,0xe2,0x48,0x2c,0x5b,0xe9,0xfd,0xb5,0x7b,0x55,0x82,0x3c,0x89,0xf5,0x8c,0xff},
    {0x25,0xb3,0x4a,0x2a,0x0b,0x36,0x00,0x61,0xcf,0x53,0x78,0x02,0x27,0x74,0xa2,0x26}, {0x01,0x40,0x04,0x01,0x50,0x02,0x29,0x21,0xb5,0x08,0x70,0x02,0x7c,0xc0,0x0f,0xfb},
    {0x41,0x53,0x8d,0x35,0x69,0x16,0xe1,0x25,0x39,0x08,0xf8,0x43,0x7f,0x72,0x0b,0xb6}, {0xb5,0xb2,0x48,0x2a,0xcb,0x1f,0x7f,0x7c,0xd7,0x58,0x04,0x60,0x05,0x0a,0xe0,0x00},
};
// count blocks of block_bytes for NVN format fmt (0x41..0x50) into out.
static inline void xbg_bc_kat_blocks(uint32_t fmt, uint32_t block_bytes, uint32_t count, std::vector<uint8_t> &out) {
    out.assign(size_t(count) * block_bytes, 0);
    uint64_t x = 0x9E3779B97F4A7C15ull ^ fmt;
    static const uint8_t bc6_modes[] = {0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15, 19, 23, 27, 31};
    uint32_t fixed = 0;
    if(fmt == 0x4f || fmt == 0x50) {fixed = count < 20 ? count : 20; memcpy(out.data(), xbg_bc_kat_bc6h_mode1, size_t(fixed) * 16);}
    for(uint32_t i = fixed; i < count; i++) {
        uint8_t *b = out.data() + size_t(i) * block_bytes;
        for(uint32_t k = 0; k < block_bytes; k++) {x ^= x << 13; x ^= x >> 7; x ^= x << 17; b[k] = uint8_t(x >> 24);}
        if(fmt == 0x4d || fmt == 0x4e) {const uint32_t m = i % 9; b[0] = m < 8 ? uint8_t((uint32_t(b[0]) << (m + 1)) | (1u << m)) : 0;}
        if(fmt == 0x4f || fmt == 0x50) {const uint8_t m = bc6_modes[i % sizeof bc6_modes]; b[0] = uint8_t(m < 2 ? (b[0] & ~3u) | m : (b[0] & ~31u) | m);}
    }
}
