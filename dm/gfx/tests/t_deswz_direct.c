// [r5] DM_GFX_DESWZ_DIRECT: nvn_deswizzle_gob must equal nvn_deswizzle byte for byte, and nvn_deswizzle_gob_block must equal
// partial_upload's former per-16-byte nvn_bl_offset walk of one block. Random storage, many formats (1-16 bytes per block,
// BC, ASTC), targets (1D, 1D array, 2D, 2D array, cube, cube array, 3D), sizes, mip levels, block heights (automatic and
// packaged layouts 0-5) and pitches; destinations pre-filled so a byte written by one and not the other shows up.
// Build and run (Linux / WSL):   make t_deswz_direct && ./t_deswz_direct
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../tex/nvn_deswizzle.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

static const int formats[] = {
    0x01, 0x05, 0x0A, 0x16, 0x19, 0x1D, 0x22, 0x25, 0x29, 0x2E,   // 1, 2, 4, 8, 3, 6, 12, 4, 8, 16 bytes per texel
    0x31, 0x32, 0x36, 0x37,                                       // 1, 2, 8, 4
    0x41, 0x43, 0x45, 0x47, 0x4B, 0x4F,                           // DXT / RGTC / BPTC (8 or 16 bytes per 4x4 block)
    0x79, 0x7D, 0x80, 0x86,                                       // ASTC 4x4, 6x6, 8x8, 12x12
};

static uint64_t checked_levels = 0, checked_bytes = 0, checked_blocks = 0, fails = 0, seen_th[8], seen_td[8];

static void block_check(const nvn_texdesc *d, unsigned level, unsigned layer, const uint8_t *mem, uint64_t size) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || !slices || !bpb || 64 % bpb) return;
    const uint32_t brows = 8u << th, bdepth = 1u << td, gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + brows - 1) / brows, blocks_z = (slices + bdepth - 1) / bdepth;
    const uint64_t block = 512ull << (th + td);
    static uint8_t a[64 * 256 * 32], b[64 * 256 * 32];
    for (uint32_t bz = 0; bz < blocks_z; bz++) for (uint32_t by = 0; by < blocks_y; by++) for (uint32_t bx = 0; bx < gobs_x; bx++) {
        if (rnd() % 4) continue;   // a sample of the blocks
        const uint32_t x0 = bx * 64, y0 = by * brows, z0 = bz * bdepth;
        const uint8_t *bl = mem + base + (nvn_bl_offset(x0, y0, z0, row_bytes, rows, th, td) & ~(block - 1));
        if (bl + block > mem + size) continue;
        const uint32_t xb = row_bytes - x0 < 64 ? row_bytes - x0 : 64, ny = rows - y0 < brows ? rows - y0 : brows, nz = slices - z0 < bdepth ? slices - z0 : bdepth;
        const size_t n = (size_t)xb * ny * nz; if (n > sizeof a) { fprintf(stderr, "block too big\n"); exit(2); }
        memset(a, 0x5A, n + 16); memset(b, 0x5A, n + 16);
        for (uint32_t z = 0; z < nz; z++) for (uint32_t y = 0; y < ny; y++) for (uint32_t x = 0; x < xb; x += 16) {
            uint32_t k = xb - x < 16 ? xb - x : 16;
            memcpy(a + ((size_t)z * ny + y) * xb + x, mem + base + nvn_bl_offset(x0 + x, y0 + y, z0 + z, row_bytes, rows, th, td), k);
        }
        nvn_deswizzle_gob_block(bl, th, xb, ny, nz, b);
        ++checked_blocks;
        if (memcmp(a, b, n + 16)) { if (fails++ < 10) fprintf(stderr, "BLOCK MISMATCH fmt=%#x target=%d %ux%ux%u level=%u layer=%u block %u,%u,%u th=%u td=%u\n",
                                                          d->format, d->target, d->width, d->height, d->depth, level, layer, bx, by, bz, th, td); }
    }
}

static void one(const nvn_texdesc *d) {
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (!L.size || L.size > (256u << 20)) return;
    const uint64_t slack = 1 << 20;
    uint8_t *mem = malloc(L.size + slack);
    for (uint64_t i = 0; i < L.size + slack; i += 4) { uint32_t v = rnd(); memcpy(mem + i, &v, 4); }
    for (unsigned layer = 0; layer < L.layers; layer++)
        for (unsigned level = 0; level < d->levels; level++) {
            uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
            if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) { fprintf(stderr, "geom failed\n"); exit(2); }
            const size_t pad = (rnd() & 1) ? 0 : (rnd() % 3) * 16 + (rnd() % 16), pitch = row_bytes + pad, n = pitch * rows * slices;
            uint8_t *a = malloc(n + 64), *b = malloc(n + 64);
            memset(a, 0xA5, n + 64); memset(b, 0xA5, n + 64);
            int ra = nvn_deswizzle(d, level, layer, mem, a, pitch), rb = nvn_deswizzle_gob(d, level, layer, mem, b, pitch);
            ++checked_levels; checked_bytes += n; ++seen_th[th & 7]; ++seen_td[td & 7];
            if (ra != rb || memcmp(a, b, n + 64)) {
                if (fails++ < 10) fprintf(stderr, "MISMATCH fmt=%#x target=%d %ux%ux%u levels=%u pkg=%d level=%u layer=%u row_bytes=%u rows=%u slices=%u th=%u td=%u pitch=%zu rc=%d/%d\n",
                                          d->format, d->target, d->width, d->height, d->depth, d->levels, d->pkg_block_h, level, layer, row_bytes, rows, slices, th, td, pitch, ra, rb);
            }
            free(a); free(b);
            block_check(d, level, layer, mem, L.size);
        }
    // the error paths agree too (pitch below the row, level / layer out of range)
    { uint8_t t[64]; if (nvn_deswizzle(d, d->levels, 0, mem, t, 64) != nvn_deswizzle_gob(d, d->levels, 0, mem, t, 64)) { ++fails; fprintf(stderr, "rc mismatch (level)\n"); }
      uint32_t rb, r, s, bpb; uint64_t base; unsigned th, td;
      if (!nvn_deswizzle_geom(d, 0, 0, &rb, &r, &s, &base, &th, &td, &bpb) && rb > 1 && nvn_deswizzle_gob(d, 0, 0, mem, t, rb - 1) != -1) { ++fails; fprintf(stderr, "short pitch accepted\n"); } }
    free(mem);
}

static unsigned max_levels(uint32_t w, uint32_t h, uint32_t z) { unsigned n = 1; uint32_t m = w > h ? w : h; if (z > m) m = z; while (m > 1) { m >>= 1; n++; } return n; }

int main(void) {
    static const int targets[] = {0, 1, 2, 3, 4, 8, 9};
    static const uint32_t sizes[] = {1, 2, 3, 4, 5, 7, 8, 13, 16, 17, 31, 32, 33, 63, 64, 65, 100, 127, 128, 129, 200, 255, 256, 257, 300, 511, 512, 513, 640, 720, 1000, 1024, 1280};
    const unsigned ns = sizeof sizes / sizeof *sizes;
    unsigned cases = 0;
    for (unsigned fi = 0; fi < sizeof formats / sizeof *formats; fi++)
        for (unsigned ti = 0; ti < sizeof targets / sizeof *targets; ti++)
            for (unsigned k = 0; k < 24; k++) {
                nvn_texdesc d; memset(&d, 0, sizeof d);
                d.target = targets[ti]; d.format = formats[fi]; d.samples = 1;
                int bw, bh; nvn_fmt_bpb(d.format, &bw, &bh);
                if ((bw > 1 || bh > 1) && (d.target == 0 || d.target == 3 || d.target == 2)) continue;   // block formats: 2D-like targets only
                d.width = sizes[rnd() % ns]; d.height = sizes[rnd() % ns]; d.depth = 1;
                if (d.target == 0) d.height = 1;
                if (d.target == 3) d.height = 1 + rnd() % 6;                   // 1D array: layers in height
                if (d.target == 4) d.depth = 1 + rnd() % 5;                    // 2D array
                if (d.target == 8) { d.height = d.width; d.depth = 6; }        // cube
                if (d.target == 9) { d.height = d.width; d.depth = 6 * (1 + rnd() % 2); }
                if (d.target == 2) { d.width = sizes[rnd() % 20]; d.height = sizes[rnd() % 20]; d.depth = sizes[rnd() % 16]; }
                const unsigned ml = max_levels(d.width, d.target == 0 || d.target == 3 ? 1 : d.height, d.target == 2 ? d.depth : 1);
                d.levels = (rnd() & 1) ? ml : 1 + rnd() % ml;
                d.pkg_block_h = (d.target == 2 || (rnd() % 3)) ? 0 : 1 + (int)(rnd() % 6);   // packaged layouts with block height 2^0..2^5
                if (d.target == 0 || d.target == 3) d.pkg_block_h = 0;
                one(&d); ++cases;
            }
    // speed (x86 here; the phone is arm64): a 2048x2048 RGBA8 and a 4096x4096 BC7 level 0, old vs GOB-wise
    { static const int bf[2] = {0x25, 0x4F}; static const uint32_t bs[2] = {2048, 4096};
      for (int i = 0; i < 2; i++) { nvn_texdesc d; memset(&d, 0, sizeof d); d.target = 1; d.format = bf[i]; d.samples = 1; d.width = d.height = bs[i]; d.depth = 1; d.levels = 1;
        nvn_texlayout L; nvn_tex_layout(&d, &L); uint8_t *mem = malloc(L.size), *o = malloc(L.size + 4096); memset(mem, 7, L.size);
        uint32_t rb, r, s, bpb; uint64_t base; unsigned th, td; nvn_deswizzle_geom(&d, 0, 0, &rb, &r, &s, &base, &th, &td, &bpb);
        struct timespec t0, t1, t2; clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int k = 0; k < 10; k++) nvn_deswizzle(&d, 0, 0, mem, o, rb);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        for (int k = 0; k < 10; k++) nvn_deswizzle_gob(&d, 0, 0, mem, o, rb);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        double a = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, b = (t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6;
        printf("bench fmt=%#x %ux%u %.1f MB: nvn_deswizzle %.2f ms, gob %.2f ms (x%.2f)\n", d.format, d.width, d.height, L.size / 1048576.0, a / 10, b / 10, a / b);
        free(mem); free(o); } }
    printf("t_deswz_direct: %u textures, %llu levels/layers (%.1f MB), %llu partial blocks, %llu mismatches\n", cases,
           (unsigned long long)checked_levels, checked_bytes / 1048576.0, (unsigned long long)checked_blocks, (unsigned long long)fails);
    printf("block heights (log2 GOBs) 0-5: %llu %llu %llu %llu %llu %llu; block depths 0-3: %llu %llu %llu %llu\n", (unsigned long long)seen_th[0], (unsigned long long)seen_th[1],
           (unsigned long long)seen_th[2], (unsigned long long)seen_th[3], (unsigned long long)seen_th[4], (unsigned long long)seen_th[5],
           (unsigned long long)seen_td[0], (unsigned long long)seen_td[1], (unsigned long long)seen_td[2], (unsigned long long)seen_td[3]);
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails ? 1 : 0;
}
