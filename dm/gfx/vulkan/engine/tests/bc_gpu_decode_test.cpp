// [nonadreno] GPU BC decode (issue #6) on the host: xbg_bc_gpu.glsl compiled as C++ (glsl_shim.h) must produce the CPU decoder's
// bytes (xbg_decode_block / xbg_decode_bc_linear) for every NVN BC format and option, block by block (random blocks: every BC6H
// and BC7 mode) and end to end through the upload planning of xbg_bc_gpu.h (bands, batches, region table, padded rows, copies),
// with the shader's main() reproduced below. E5B9G9R9 packing: GLSL == C == the extension's float formula.
// test_real_layouts (06.11): the shapes the game uploads (VT caches 8192x4096 / 8192x8192, the BC6H cube array, partial GOB blocks
// clipped at the edges, the 16384-region limit) with the engine's scratch caps; ./bc_gpu_decode_test --big runs them at full size.
// This checks the decoder logic, not a GPU compiler: run DM_GFX_BC_DECODE=gpu against =cpu on a device for that, or
// DM_GFX_BC_VERIFY=1 (xbg_game.cpp bcv), which compares the device's own decode with the CPU decoder batch by batch.
// Build and run (WSL, from this directory):
//   gcc -O2 -c -I.. ../xbg_texture.c ../../../tex/nvn_deswizzle.c ../../../nvn/nvn_tex.c &&
//   g++ -std=c++20 -O2 -fsanitize=shift,bounds -fno-sanitize-recover=all -Wall -Wno-unused-function -I.. -o bc_gpu_decode_test bc_gpu_decode_test.cpp xbg_texture.o nvn_deswizzle.o nvn_tex.o -lpthread && ./bc_gpu_decode_test
// (-fsanitize=shift,bounds: a shift by >= 32 / < 0 or an array index out of range aborts, as glsl_shim.h does for vector indices and
// bitfieldExtract ranges: what GLSL leaves undefined must not pass on the host, 06.11.)
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
        const Bc6Block b6 = xbg_block(p);   // once per block, as main() does
        for(int r = 0; r < 4; r++) {
            XbgRow q = xbg_bc_row_b(b6, p, r, bpp); const uint32_t w[8] = {q.lo.x, q.lo.y, q.lo.z, q.lo.w, q.hi.x, q.hi.y, q.hi.z, q.hi.w};
            for(uint32_t k = 0; k < bpp; k++) out[base + r * row_words + k] = w[k];
        }
    }
}

// Known-answer blocks (xbg_bc_gpu.h xbg_bc_kat_blocks, the engine's on-device self-test uses the same ones): the 20 BC6H mode-1 blocks
// that the Adreno 750 decoded with zero rows 1-3 (06.11) and every mode of BC6H / BC7, as one region through main() and per pixel.
static void test_kat(uint32_t fmt, uint32_t opt) {
    set_flags(fmt, opt); xbg_bc_format info; xbg_bc_format_info(fmt, &info); const uint32_t bpp = info.bytes_per_pixel, bb = info.bytes_per_block;
    const uint32_t bx = 64, by = 32, n = bx * by; std::vector<uint8_t> raw; xbg_bc_kat_blocks(fmt, bb, n, raw);
    XbgBcGpuBatch batch; batch.add(raw.data(), bx, by, 1, bb, bpp); std::vector<uint32_t> out; run_batch(batch, bpp, bb / 4, out);
    const uint8_t *o = reinterpret_cast<const uint8_t*>(out.data()); unsigned bad = 0, bad_pixel = 0;
    for(uint32_t i = 0; i < n; i++) {
        uint8_t cpu[128]; if(xbg_decode_block(fmt, raw.data() + size_t(i) * bb, bb, cpu, sizeof cpu)) {CHECK(0, "cpu decode"); return;}
        const uint32_t x = i % bx, y = i / bx; bool ok = true, ok_pixel = true;
        for(uint32_t r = 0; r < 4; r++) {
            const uint8_t *g = o + ((size_t(y) * 4 + r) * bx * 4 + size_t(x) * 4) * bpp, *c = cpu + r * 4 * bpp;
            if(memcmp(g, c, 4 * bpp) && !((fmt == 0x4a || fmt == 0x4c) && snorm_close(g, c, 4 * bpp))) ok = false;
            for(int k = 0; k < 4; k++) {uvec4 p = payload_of(raw.data() + size_t(i) * bb, bb); uvec2 v = xbg_bc_pixel(p, int(r * 4 + k)); uint32_t w[2] = {v.x, v.y};
                if(memcmp(w, c + k * bpp, bpp) && !((fmt == 0x4a || fmt == 0x4c) && snorm_close(reinterpret_cast<uint8_t*>(w), c + k * bpp, bpp))) ok_pixel = false;}
        }
        if(!ok && bad++ < 3) CHECK(0, "KAT fmt %#x opt %u block %u (%s) differs through main()", fmt, opt, i, i < 20 && fmt >= 0x4f ? "Adreno BC6H mode-1 vector" : "generated");
        if(!ok_pixel && bad_pixel++ < 3) CHECK(0, "KAT fmt %#x opt %u block %u differs per pixel", fmt, opt, i);
    }
    printf("KAT fmt %#x opt %u: %u blocks%s %s\n", fmt, opt, n, fmt >= 0x4f ? " (20 Adreno mode-1 vectors first)" : "", bad || bad_pixel ? "DIFFER" : "== CPU");
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

// ---- The layouts the game really uploads, with the engine's caps (regression for the black patches of §2.24, 06.11) ----
// Planner = BcUpload::blocks / flush in GPU mode (xbg_game.cpp) with bcgpu's scratch: out = DM_GFX_BC_BATCH_MB, in = out / 2 + the
// table of 16384 regions, 16384 regions, maxComputeWorkGroupCount[0] = 65535 groups of 64. Every batch goes through run_batch (the
// shader's main()) and its copies land in the decoded subresources exactly as vkCmdCopyBufferToImage would place them.
struct Planner {
    uint32_t fmt, bpp, bb; size_t out_cap, in_cap, max_regions, max_blocks;
    uint32_t levels, layers; std::vector<uint32_t> W, H, D; std::vector<std::vector<uint8_t>> img;   // per layer * levels + level
    struct Copy { uint32_t sub; size_t off; uint32_t row, height; int32_t x, y, z; uint32_t w, h, d; };
    XbgBcGpuBatch batch; std::vector<Copy> copies; unsigned batches = 0, regions = 0, max_batch_regions = 0; size_t max_in = 0;
    Planner(uint32_t f, uint32_t bpp_, uint32_t bb_, size_t out_mb, uint32_t lv, uint32_t ly, uint32_t w0, uint32_t h0, uint32_t d0, size_t regions_cap = 16384)
        : fmt(f), bpp(bpp_), bb(bb_), out_cap(out_mb << 20), in_cap((out_mb << 20) / 2 + 16384u * 32u), max_regions(regions_cap), max_blocks(size_t(65535) * 64), levels(lv), layers(ly) {
        for(uint32_t layer = 0; layer < ly; layer++) for(uint32_t level = 0; level < lv; level++) {
            W.push_back(std::max(1u, w0 >> level)); H.push_back(std::max(1u, h0 >> level)); D.push_back(std::max(1u, d0 >> level));
            img.emplace_back(size_t(W.back()) * H.back() * D.back() * bpp, uint8_t(0xcd));
        }
    }
    uint32_t sub(uint32_t level, uint32_t layer) const {return layer * levels + level;}
    void blocks(uint32_t level, uint32_t layer, const uint8_t *raw, uint32_t nbx, uint32_t nby, uint32_t nz, int32_t x, int32_t y, int32_t z, uint32_t w, uint32_t h, uint32_t d) {
        const size_t n = size_t(nbx) * nby * nz, raw_n = n * bb, out_n = n * 16 * bpp;
        if(!batch.fits(raw_n, out_n, in_cap, out_cap, max_regions, max_blocks, uint32_t(n))) flush();
        CHECK(raw_n + 16384 * 32 <= in_cap && out_n <= out_cap, "fmt %#x: a rectangle of %zu blocks exceeds the GPU scratch (the engine's need() would abort)", fmt, n);
        const size_t off = batch.add(raw, nbx, nby, nz, bb, bpp);
        copies.push_back({sub(level, layer), off, nbx * 4, nby * 4, x, y, z, w, h, d});
    }
    void flush() {
        if(copies.empty()) return;
        CHECK(batch.input_bytes() <= in_cap && batch.out <= out_cap && batch.regions() <= max_regions && batch.blocks <= max_blocks && batch.regions() == copies.size(),
            "fmt %#x batch %u outside the scratch: in %zu/%zu out %zu/%zu regions %zu blocks %u", fmt, batches, batch.input_bytes(), in_cap, batch.out, out_cap, batch.regions(), batch.blocks);
        std::vector<uint32_t> out; run_batch(batch, bpp, bb / 4, out);
        const uint8_t *o = reinterpret_cast<const uint8_t*>(out.data());
        for(const Copy &c : copies) {
            const uint32_t sw = W[c.sub], sh = H[c.sub], sd = D[c.sub];
            CHECK(c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x + c.w <= sw && c.y + c.h <= sh && c.z + c.d <= sd && c.w <= c.row && c.h <= c.height, "copy outside its subresource");
            CHECK(c.off % 16 == 0 && c.off + ((size_t(c.d - 1) * c.height + c.h - 1) * c.row + c.w) * bpp <= batch.out, "copy reads outside its region");
            for(uint32_t zz = 0; zz < c.d; zz++) for(uint32_t yy = 0; yy < c.h; yy++)
                memcpy(img[c.sub].data() + ((size_t(c.z + zz) * sh + c.y + yy) * sw + c.x) * bpp, o + c.off + ((size_t(zz) * c.height + yy) * c.row) * bpp, size_t(c.w) * bpp);
        }
        ++batches; regions += unsigned(batch.regions()); max_batch_regions = std::max(max_batch_regions, unsigned(batch.regions())); max_in = std::max(max_in, batch.input_bytes());
        batch.clear(); copies.clear();
    }
};
static bool same_image(uint32_t fmt, const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    if(a.size() != b.size()) return false;
    if(!memcmp(a.data(), b.data(), a.size())) return true;
    return (fmt == 0x4a || fmt == 0x4c) && snorm_close(a.data(), b.data(), a.size());
}
static void random_blocks(std::vector<uint8_t> &raw, uint32_t fmt, uint32_t bb) {
    for(size_t i = 0; i < raw.size(); i += 4) {uint32_t r = rnd(); memcpy(raw.data() + i, &r, std::min<size_t>(4, raw.size() - i));}
    if(fmt == 0x4d || fmt == 0x4e) for(size_t i = 0; i < raw.size(); i += bb) {const unsigned m = rnd() % 8; raw[i] = uint8_t((unsigned(raw[i]) << (m + 1)) | (1u << m));}   // real BC7 modes only
    if(fmt == 0x50 || fmt == 0x4f) for(size_t i = 0; i < raw.size(); i += bb) {static const uint8_t m[] = {0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15};
        raw[i] = uint8_t((raw[i] & ~31u) | m[rnd() % sizeof m]);}
}
// Whole-texture upload (upload_banded): every subresource in bands of block rows / slices, one Planner for the texture.
static void test_real_full(const char *what, uint32_t fmt, uint32_t opt, uint32_t w0, uint32_t h0, uint32_t d0, uint32_t levels, uint32_t layers, size_t out_mb, size_t regions_cap = 16384) {
    set_flags(fmt, opt); xbg_bc_format info; xbg_bc_format_info(fmt, &info); const uint32_t bpp = info.bytes_per_pixel, bb = info.bytes_per_block;
    Planner P(fmt, bpp, bb, out_mb, levels, layers, w0, h0, d0, regions_cap); unsigned bad = 0; size_t blocks_total = 0;
    std::vector<std::vector<uint8_t>> raws;
    for(uint32_t layer = 0; layer < layers; layer++) for(uint32_t level = 0; level < levels; level++) {
        const uint32_t s = P.sub(level, layer), w = P.W[s], h = P.H[s], d = P.D[s], bx = (w + 3) / 4, by = (h + 3) / 4;
        raws.emplace_back(size_t(bx) * by * d * bb); random_blocks(raws.back(), fmt, bb); blocks_total += size_t(bx) * by * d;
        std::vector<XbgBand> bands; xbg_upload_bands(bx, by, d, 16 * bpp, P.out_cap, bands);   // min(DM_GFX_BC_BATCH_MB, out_cap): equal
        for(const XbgBand &b : bands) {const XbgCopy c = xbg_band_copy(0, b, bx, w, h, 4, 4);
            P.blocks(level, layer, raws.back().data() + (size_t(b.z0) * by + b.by0) * bx * bb, bx, b.nby, b.nz, c.x, c.y, c.z, c.w, c.h, c.d);}
    }
    P.flush();
    for(uint32_t layer = 0, k = 0; layer < layers; layer++) for(uint32_t level = 0; level < levels; level++, k++) {
        const uint32_t s = P.sub(level, layer); std::vector<uint8_t> ref(P.img[s].size());
        if(xbg_decode_bc_linear(fmt, P.W[s], P.H[s], P.D[s], raws[k].data(), raws[k].size(), 0, 0, ref.data(), ref.size())) {CHECK(0, "cpu decode"); return;}
        if(!same_image(fmt, P.img[s], ref)) {if(bad++ < 3) CHECK(0, "%s: fmt %#x opt %u level %u layer %u (%ux%ux%u) differs from the CPU decode", what, fmt, opt, level, layer, P.W[s], P.H[s], P.D[s]);}
    }
    printf("real full %-34s fmt %#x opt %u: %zu blocks, %u batches, %u regions (max %u a batch, input max %.1f MB of %.1f) %s\n", what, fmt, opt, blocks_total, P.batches, P.regions,
        P.max_batch_regions, P.max_in / 1048576.0, P.in_cap / 1048576.0, bad ? "DIFFERS" : "== CPU");
}
// Partial upload (partial_upload): the dirty GOB blocks of a block-linear subresource (64 bytes x brows block rows), clipped at the
// right / bottom edges, gathered as tight rows and handed over one GOB block a region; the image starts as the decode of the old
// blocks and must end as the decode of the old blocks with every dirty GOB block taken from the new ones.
static void test_real_partial(const char *what, uint32_t fmt, uint32_t opt, uint32_t w0, uint32_t h0, uint32_t levels, uint32_t layers, unsigned th, unsigned dirty_pct, size_t out_mb, size_t regions_cap = 16384) {
    set_flags(fmt, opt); xbg_bc_format info; xbg_bc_format_info(fmt, &info); const uint32_t bpp = info.bytes_per_pixel, bb = info.bytes_per_block;
    Planner P(fmt, bpp, bb, out_mb, levels, layers, w0, h0, 1, regions_cap); unsigned bad = 0, gobs = 0, clipped = 0;
    std::vector<std::vector<uint8_t>> merged;
    for(uint32_t layer = 0; layer < layers; layer++) for(uint32_t level = 0; level < levels; level++) {
        const uint32_t s = P.sub(level, layer), w = P.W[s], h = P.H[s], bx = (w + 3) / 4, rows = (h + 3) / 4, row_bytes = bx * bb;
        std::vector<uint8_t> old_raw(size_t(row_bytes) * rows), new_raw(old_raw.size()); random_blocks(old_raw, fmt, bb); random_blocks(new_raw, fmt, bb);
        if(xbg_decode_bc_linear(fmt, w, h, 1, old_raw.data(), old_raw.size(), 0, 0, P.img[s].data(), P.img[s].size())) {CHECK(0, "cpu decode"); return;}   // the image before
        merged.push_back(old_raw);
        const uint32_t brows = 8u << std::min(th, 5u), gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + brows - 1) / brows;
        std::vector<uint8_t> linear;
        for(uint32_t gy = 0; gy < blocks_y; gy++) for(uint32_t gx = 0; gx < gobs_x; gx++) {
            if(rnd() % 100 >= dirty_pct) continue;
            const uint32_t x0 = gx * 64, y0 = gy * brows, xb = std::min<uint32_t>(64, row_bytes - x0), ny = std::min<uint32_t>(brows, rows - y0);
            linear.resize(size_t(xb) * ny);
            for(uint32_t y = 0; y < ny; y++) {memcpy(linear.data() + size_t(y) * xb, new_raw.data() + size_t(y0 + y) * row_bytes + x0, xb);
                memcpy(merged.back().data() + size_t(y0 + y) * row_bytes + x0, new_raw.data() + size_t(y0 + y) * row_bytes + x0, xb);}
            const int32_t ix = int32_t((x0 / bb) * 4), iy = int32_t(y0 * 4);
            const uint32_t ew = std::min<uint32_t>((xb / bb) * 4, w - uint32_t(ix)), eh = std::min<uint32_t>(ny * 4, h - uint32_t(iy));
            clipped += ew < (xb / bb) * 4 || eh < ny * 4;
            P.blocks(level, layer, linear.data(), xb / bb, ny, 1, ix, iy, 0, ew, eh, 1); ++gobs;
        }
    }
    P.flush();
    for(uint32_t layer = 0, k = 0; layer < layers; layer++) for(uint32_t level = 0; level < levels; level++, k++) {
        const uint32_t s = P.sub(level, layer); std::vector<uint8_t> ref(P.img[s].size());
        if(xbg_decode_bc_linear(fmt, P.W[s], P.H[s], 1, merged[k].data(), merged[k].size(), 0, 0, ref.data(), ref.size())) {CHECK(0, "cpu decode"); return;}
        if(!same_image(fmt, P.img[s], ref)) {if(bad++ < 3) CHECK(0, "%s: fmt %#x opt %u level %u layer %u (%ux%u) differs after the partial upload", what, fmt, opt, level, layer, P.W[s], P.H[s]);}
    }
    printf("real partial %-31s fmt %#x opt %u: %u GOB blocks (%u clipped), %u batches, %u regions (max %u a batch) %s\n", what, fmt, opt, gobs, clipped, P.batches, P.regions,
        P.max_batch_regions, bad ? "DIFFERS" : "== CPU");
}
static void test_real_layouts(bool big) {
    // VT caches: 1880 BC6H 8192x4096 (E5B9G9R9 on Mali: 256-row bands), 27 BC7 8192x8192 5 levels, 103 BC3 sRGB 4096^2 x3, BC5 / BC4
    // 8192x4096 decoded to 8-bit channels (512 / 1024-row bands: the raw blocks of a band fill the input scratch exactly).
    test_real_full("BC5 8192x4096 RG8 (in exactly full)", 0x4b, 1, 8192, 4096, 1, 1, 1, 32);
    test_real_full("BC4 8192x4096 R8 (in exactly full)", 0x49, 1, 8192, 4096, 1, 1, 1, 32);
    test_real_full("BC5 8192x4096 RG8, 128 MB batches", 0x4b, 1, 8192, 4096, 1, 1, 1, 128);
    test_real_full("BC6H E5 8192x1024|4096 (1880)", 0x50, 2, 8192, big ? 4096 : 1024, 1, 1, 1, 32);
    test_real_full("BC7 8192x512|8192 5 lv (27)", 0x4d, 0, 8192, big ? 8192 : 512, 1, 5, 1, 32);
    test_real_full("BC3 sRGB 1024|4096^2 x3 (103)", 0x48, 0, big ? 4096 : 1024, big ? 4096 : 1024, 1, 1, 3, 32);
    // 94: BC6H cube array 128x128, 7 levels (5760 layers in the game): many tiny regions a batch, down to 2x2 / 1x1 mips.
    test_real_full("BC6H E5 cube array 128^2 x96 7 lv", 0x50, 2, 128, 128, 1, 7, 96, 32);
    test_real_full("BC6H F16 cube array 128^2 x96 7 lv", 0x50, 0, 128, 128, 1, 7, 96, 32);
    test_real_full("BC6H E5 cube array, 4 MB batches", 0x50, 2, 128, 128, 1, 7, 96, 4);
    // The region limit, not the bytes, ends the batches: 20000 layers of an 8x8 BC1 texture with 2 levels (40000 regions).
    test_real_full("BC1 8x8 x20000 2 lv (region limit)", 0x41, 0, 8, 8, 1, 2, 20000, 32);
    test_real_full("BC7 8x8 x3000 4 lv, 64 regions cap", 0x4d, 0, 8, 8, 1, 4, 3000, 32, 64);
    // Partial uploads: GOB blocks of 4 (16-byte blocks) or 8 (8-byte) blocks x 8 << th block rows, sizes that clip the last ones.
    test_real_partial("BC3 sRGB 4096^2 all dirty th=4", 0x48, 0, 4096, 4096, 1, 1, 4, 100, 32);
    test_real_partial("BC7 1002x506 th=3 30%", 0x4d, 0, 1002, 506, 1, 1, 3, 30, 32);
    test_real_partial("BC1 1002x506 th=3 30%", 0x41, 0, 1002, 506, 1, 1, 3, 30, 32);
    test_real_partial("BC5 RG8 2042x1018 th=4 50%", 0x4b, 1, 2042, 1018, 1, 1, 4, 50, 32);
    test_real_partial("BC4 R8 2042x1018 th=5 50%", 0x49, 1, 2042, 1018, 1, 1, 5, 50, 32);
    test_real_partial("BC6H E5 8192x512|4096 th=4 3%", 0x50, 2, 8192, big ? 4096 : 512, 1, 1, 4, 3, 32);
    test_real_partial("BC6H E5 cube 128^2 x24 7 lv th=2", 0x50, 2, 128, 128, 7, 24, 2, 40, 32);
    test_real_partial("BC7 4096x1024 th=4 60%, 1 MB batches", 0x4d, 0, 4096, 1024, 1, 1, 4, 60, 1);
    test_real_partial("BC3 1024^2 th=4 all, 16 regions cap", 0x44, 0, 1024, 1024, 1, 1, 4, 100, 32, 16);
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

int main(int argc, char **argv) {
    const bool big = argc > 1 && !strcmp(argv[1], "--big");   // --big: the real layouts at full size (8192x8192 BC7, 8192x4096 BC6H; ~1 min)
    test_bands();
    test_real_layouts(big);
    const uint32_t fmts[] = {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50};
    for(uint32_t f : fmts) for(uint32_t opt = 0; opt < 4; opt++) {
        if(opt && !((opt == 1 && (f == 0x49 || f == 0x4b)) || (opt == 2 && f == 0x50))) continue;
        test_kat(f, opt);
        test_blocks(f, opt, 300000);
        test_planned(f, opt);
    }
    test_e5();
    set_flags(0x41, 0);
    printf(failures ? "bc_gpu_decode_test: %d FAILURES\n" : "bc_gpu_decode_test: all passed\n", failures);
    return failures != 0;
}
