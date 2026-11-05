// [nonadreno] GPU BC decode (issue #6) on the host: xbg_bc_gpu.glsl compiled as C++ (glsl_shim.h) must produce the CPU decoder's
// bytes (xbg_decode_block / xbg_decode_bc_linear) for every NVN BC format and option, block by block (random blocks: every BC6H
// and BC7 mode) and end to end through the upload planning of xbg_bc_gpu.h (bands, batches, region table, padded rows, copies),
// with the shader's main() reproduced below. E5B9G9R9 packing: GLSL == C == the extension's float formula.
// This checks the decoder logic, not a GPU compiler: run DM_GFX_BC_DECODE=gpu against =cpu on a device for that.
// Build and run (WSL, from this directory):
//   gcc -O2 -c -I.. ../xbg_texture.c ../../../tex/nvn_deswizzle.c ../../../nvn/nvn_tex.c &&
//   g++ -std=c++20 -O2 -Wall -Wno-unused-function -I.. -o bc_gpu_decode_test bc_gpu_decode_test.cpp xbg_texture.o nvn_deswizzle.o nvn_tex.o -lpthread && ./bc_gpu_decode_test
#include "glsl_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
extern "C" {
#include "../xbg_texture.h"
}
#include "../xbg_bc_gpu.h"

#define XBG_HOST 1
static uint XBG_FMT = 0x41u, XBG_OPT = 0u;
#include "../xbg_bc_gpu.glsl"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd() {rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return uint32_t(rng >> 11);}
static int failures = 0;
#define CHECK(c, ...) do { if(!(c)) { if(failures++ < 30) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while(0)

static void set_flags(uint32_t fmt, uint32_t opt) {
    XBG_FMT = fmt; XBG_OPT = opt; xbg_bc_8bit_channels = (opt & 1) ? 1 : 0; xbg_bc6_e5 = (opt & 2) ? 1 : 0;
}
static uvec4 payload_of(const uint8_t *b, uint32_t block_bytes) {
    uint32_t w[4] = {0, 0, 0, 0}; memcpy(w, b, block_bytes); return uvec4(w[0], w[1], w[2], w[3]);
}
// Signed BC4/BC5: the CPU rounds a float palette; allow 1 in the 16-bit value (a GPU's division need not be correctly rounded).
static bool snorm_close(const uint8_t *a, const uint8_t *b, size_t n) {
    for(size_t i = 0; i + 1 < n; i += 2) {int16_t x, y; memcpy(&x, a + i, 2); memcpy(&y, b + i, 2); if(abs(int(x) - int(y)) > 1) return false;}
    return true;
}

// Per block: GLSL pixels and rows vs xbg_decode_block.
static void test_blocks(uint32_t fmt, uint32_t opt, unsigned count) {
    set_flags(fmt, opt);
    xbg_bc_format info; if(xbg_bc_format_info(fmt, &info)) {CHECK(0, "format info %#x", fmt); return;}
    const uint32_t bpp = info.bytes_per_pixel, bb = info.bytes_per_block;
    const bool snorm = fmt == 0x4a || fmt == 0x4c;
    unsigned mism = 0, exact = 0;
    for(unsigned n = 0; n < count; n++) {
        uint8_t blk[16]; for(auto &b : blk) b = uint8_t(rnd());
        if(n < 64 && (fmt == 0x4d || fmt == 0x4e)) blk[0] = uint8_t(1u << (n % 9));     // every BC7 mode, mode 8 (zero byte) included
        if(n < 64 && (fmt == 0x4f || fmt == 0x50)) blk[0] = uint8_t((blk[0] & ~31u) | (n & 31u));   // every BC6H mode field
        uint8_t cpu[128]; if(xbg_decode_block(fmt, blk, bb, cpu, sizeof(cpu))) {CHECK(0, "cpu decode %#x", fmt); return;}
        uint8_t gpu[128]; uvec4 p = payload_of(blk, bb);
        for(int r = 0; r < 4; r++) {XbgRow q = xbg_bc_row(p, r, bpp); uint32_t words[8] = {q.lo.x, q.lo.y, q.lo.z, q.lo.w, q.hi.x, q.hi.y, q.hi.z, q.hi.w}; memcpy(gpu + r * 4 * bpp, words, 4 * bpp);}
        if(memcmp(cpu, gpu, 16 * bpp)) {
            if(snorm && snorm_close(cpu, gpu, 16 * bpp)) {++mism; continue;}
            CHECK(0, "fmt %#x opt %u block %u differs (first bytes cpu %02x%02x%02x%02x gpu %02x%02x%02x%02x, block %02x%02x%02x%02x...)", fmt, opt, n,
                cpu[0], cpu[1], cpu[2], cpu[3], gpu[0], gpu[1], gpu[2], gpu[3], blk[0], blk[1], blk[2], blk[3]);
            return;
        }
        ++exact;
    }
    printf("fmt %#x opt %u bpp %u: %u blocks byte-identical%s", fmt, opt, bpp, exact, snorm ? "" : "\n");
    if(snorm) printf(", %u within 1/32767 (float palette)\n", mism);
}

// E5B9G9R9: GLSL arithmetic == C, and both == GL_EXT_texture_shared_exponent's formula in doubles.
static double half_value(uint32_t h) {uint32_t e = (h >> 10) & 31u, m = h & 1023u; return e ? ldexp(1024.0 + m, int(e) - 25) : ldexp(double(m), -24);}
static uint32_t e5_reference(uint32_t r, uint32_t g, uint32_t b) {
    const double mx = 511.0 / 512.0 * 65536.0; double c[3] = {half_value(r), half_value(g), half_value(b)};
    for(double &v : c) v = std::min(v, mx);
    double m = std::max(c[0], std::max(c[1], c[2]));
    int e = std::max(-16, m > 0 ? int(floor(log2(m))) : -16) + 16;
    if(floor(m / ldexp(1.0, e - 24) + 0.5) == 512.0) e++;
    uint32_t s[3]; for(int i = 0; i < 3; i++) s[i] = uint32_t(floor(c[i] / ldexp(1.0, e - 24) + 0.5));
    return s[0] | s[1] << 9 | s[2] << 18 | uint32_t(e) << 27;
}
static void test_e5() {
    unsigned n = 0;
    for(uint32_t i = 0; i < 0x7c00u; i++) {
        const uint32_t t[4][3] = {{i, 0, 0}, {0, i, 0}, {i, i, i}, {i, i >> 1, rnd() % 0x7c00u}};
        for(auto &v : t) {
            const uint32_t a = xbg_e5b9g9r9(v[0], v[1], v[2]), c = xbg_e5b9g9r9_from_half(v[0], v[1], v[2]), ref = e5_reference(v[0], v[1], v[2]);
            CHECK(a == c && c == ref, "e5 %#x %#x %#x: glsl %#x c %#x ref %#x", v[0], v[1], v[2], a, c, ref); ++n;
        }
    }
    for(unsigned k = 0; k < 2000000; k++) {
        const uint32_t r = rnd() % 0x7c00u, g = rnd() % 0x7c00u, b = rnd() % 0x7c00u;
        const uint32_t a = xbg_e5b9g9r9(r, g, b), c = xbg_e5b9g9r9_from_half(r, g, b), ref = e5_reference(r, g, b);
        CHECK(a == c && c == ref, "e5 %#x %#x %#x: glsl %#x c %#x ref %#x", r, g, b, a, c, ref); ++n;
    }
    printf("e5b9g9r9: %u triples identical (GLSL, C, float reference)\n", n);
}

// xbg_bc_decode.comp main() for every block of a batch.
static void run_batch(const XbgBcGpuBatch &batch, uint32_t bpp, uint32_t block_words, std::vector<uint32_t> &out) {
    std::vector<uint32_t> src(batch.input_bytes() / 4 + 4); batch.write_input(src.data());
    out.assign(batch.out / 4 + 8, 0xdeadbeefu);
    const uint32_t regions = uint32_t(batch.regions());
    for(uint32_t g = 0; g < batch.blocks; g++) {
        uint32_t lo = 0, hi = regions - 1;
        while(lo < hi) {uint32_t mid = (lo + hi + 1) >> 1; if(src[mid * 8] <= g) lo = mid; else hi = mid - 1;}
        const uint32_t R = lo * 8, local = g - src[R], bx_n = src[R + 1], per_slice = src[R + 2], by_n = src[R + 3];
        const uint32_t z = local / per_slice, rem = local - z * per_slice, by = rem / bx_n, bx = rem - by * bx_n, s = src[R + 4] + local * block_words;
        uvec4 p(src[s], src[s + 1], block_words == 4 ? src[s + 2] : 0u, block_words == 4 ? src[s + 3] : 0u);
        const uint32_t row_words = src[R + 6], base = src[R + 5] + (z * by_n + by) * 4 * row_words + bx * bpp;
        for(int r = 0; r < 4; r++) {
            XbgRow q = xbg_bc_row(p, r, bpp); const uint32_t w[8] = {q.lo.x, q.lo.y, q.lo.z, q.lo.w, q.hi.x, q.hi.y, q.hi.z, q.hi.w};
            for(uint32_t k = 0; k < bpp; k++) out[base + r * row_words + k] = w[k];
        }
    }
}

// End to end: random textures (levels, layers, 3D slices, odd sizes) through bands and batches, once with small scratch caps
// (levels split into bands, one band a batch) and once with large ones (many subresources share a batch).
static void test_planned(uint32_t fmt, uint32_t opt) {
    set_flags(fmt, opt);
    xbg_bc_format info; xbg_bc_format_info(fmt, &info);
    const uint32_t bpp = info.bytes_per_pixel, bb = info.bytes_per_block;
    struct Tex {uint32_t w, h, d, levels, layers;} texs[] = {{37, 21, 1, 4, 2}, {256, 64, 1, 3, 1}, {13, 9, 5, 2, 1}, {130, 6, 1, 1, 3}};
    struct Sub {uint32_t W, H, D, bx, by; std::vector<uint8_t> raw, ref, img;};
    unsigned regions_total = 0, batches = 0, mism = 0;
    for(int big = 0; big < 2; big++) for(const Tex &T : texs) {
        std::vector<Sub> subs;
        for(uint32_t layer = 0; layer < T.layers; layer++) for(uint32_t level = 0; level < T.levels; level++) {
            Sub s; s.W = std::max(1u, T.w >> level); s.H = std::max(1u, T.h >> level); s.D = std::max(1u, T.d >> level); s.bx = (s.W + 3) / 4; s.by = (s.H + 3) / 4;
            s.raw.resize(size_t(s.bx) * s.by * s.D * bb); for(auto &b : s.raw) b = uint8_t(rnd());
            s.ref.resize(size_t(s.W) * s.H * s.D * bpp); s.img.assign(s.ref.size(), 0xcd);
            if(xbg_decode_bc_linear(fmt, s.W, s.H, s.D, s.raw.data(), s.raw.size(), 0, 0, s.ref.data(), s.ref.size())) {CHECK(0, "cpu linear decode"); return;}
            subs.push_back(std::move(s));
        }
        const size_t out_cap = big ? (1u << 20) : 16 * bpp * size_t(subs[0].bx) * 3 + 64, raw_cap = big ? (1u << 20) : 4096;
        XbgBcGpuBatch batch; std::vector<std::pair<size_t, XbgCopy>> copies;
        auto flush = [&] {
            if(!batch.regions()) return;
            std::vector<uint32_t> out; run_batch(batch, bpp, bb / 4, out); ++batches; regions_total += unsigned(batch.regions());
            const uint8_t *o = reinterpret_cast<const uint8_t*>(out.data());
            for(const auto &sc : copies) {const XbgCopy &c = sc.second; Sub &s = subs[sc.first];
                for(uint32_t z = 0; z < c.d; z++) for(uint32_t y = 0; y < c.h; y++)
                    memcpy(s.img.data() + ((size_t(c.z + z) * s.H + c.y + y) * s.W + c.x) * bpp, o + c.offset + ((size_t(z) * c.image_height + y) * c.row_length) * bpp, size_t(c.w) * bpp);}
            batch.clear(); copies.clear();
        };
        for(size_t k = 0; k < subs.size(); k++) {
            Sub &s = subs[k]; std::vector<XbgBand> bands; xbg_upload_bands(s.bx, s.by, s.D, 16 * bpp, out_cap, bands);
            for(const XbgBand &b : bands) {
                const size_t n = size_t(s.bx) * b.nby * b.nz;
                if(!batch.fits(n * bb, n * 16 * bpp, raw_cap, out_cap, big ? 4096 : 3, 1u << 22, uint32_t(n))) flush();
                const size_t at = batch.add(s.raw.data() + (size_t(b.z0) * s.by + b.by0) * s.bx * bb, s.bx, b.nby, b.nz, bb, bpp);
                copies.push_back({k, xbg_band_copy(at, b, s.bx, s.W, s.H, 4, 4)});
            }
        }
        flush();
        for(const Sub &s : subs) if(memcmp(s.img.data(), s.ref.data(), s.ref.size())) {
            if((fmt == 0x4a || fmt == 0x4c) && snorm_close(s.img.data(), s.ref.data(), s.ref.size())) {++mism; continue;}
            CHECK(0, "planned decode fmt %#x opt %u %ux%ux%u (caps %s) differs", fmt, opt, s.W, s.H, s.D, big ? "large" : "small");
        }
    }
    printf("fmt %#x opt %u: planned upload == CPU decode (%u batches, %u regions%s)\n", fmt, opt, batches, regions_total, mism ? ", SNORM within 1" : "");
}

static void test_bands() {
    std::vector<XbgBand> b;
    xbg_upload_bands(2048, 1024, 1, 16 * 8, 32u << 20, b);   // 8192x4096 BC6H RGBA16F level 0 = 256 MB: 8+ bands of whole block rows
    CHECK(b.size() == 8 && b[0].nby == 128 && b.back().by0 + b.back().nby == 1024, "bands of 8192x4096 RGBA16F: %zu", b.size());
    xbg_upload_bands(32, 32, 5760, 16 * 4, 32u << 20, b);    // 128x128 cube array layer-like slices: many slices per band
    CHECK(b.size() == 12 && b[0].nz == 512, "slice bands: %zu nz %u", b.size(), b.empty() ? 0 : b[0].nz);
    xbg_upload_bands(4096, 1, 1, 16 * 8, 1024, b);           // a single block row larger than the cap: one row per band
    CHECK(b.size() == 1 && b[0].nby == 1, "row larger than cap");
    printf("bands: ok\n");
}

int main() {
    test_bands();
    const uint32_t fmts[] = {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50};
    for(uint32_t f : fmts) for(uint32_t opt = 0; opt < 4; opt++) {
        if(opt && !((opt == 1 && (f == 0x49 || f == 0x4b)) || (opt == 2 && f == 0x50))) continue;
        test_blocks(f, opt, 300000);
        test_planned(f, opt);
    }
    test_e5();
    set_flags(0x41, 0);
    printf(failures ? "bc_gpu_decode_test: %d FAILURES\n" : "bc_gpu_decode_test: all passed\n", failures);
    return failures != 0;
}
