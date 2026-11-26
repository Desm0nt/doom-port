// [nonadreno] bc-astc-gpu (port-common/modules/bc-astc-gpu) on the host: the ASTC 4x4 encoder xbg_bc_astc.glsl compiled as C++ (glsl_shim.h
// + the float types below) after the GPU BC decoder xbg_bc_gpu.glsl (the bytes the device decodes, bc_gpu_decode_test), every block it
// writes decoded with the reference decoder xbg_astc_ref_decode.h:
//   golden     GameNative's self-test block -> their golden block (fidelity of the port)
//   kat        the engine's on-device self-test set (xbg_bc_astc.h); --emit-kat <file> writes xbg_bc_astc_kat.h, otherwise the table in
//              that header must equal what this build computes (a stale table would fail on the device)
//   random     random BC blocks of every family (BC7 per mode, BC1 three / four colour, BC2 / BC3 with one alpha value): PSNR of the
//              ASTC block against the decoded BC block, no error blocks, no all-zero blocks
//   pipeline   xbg_bc_decode.comp main() + xbg_bc_astc_encode.comp main() over XbgBcGpuBatch regions (levels, layers, slices, odd sizes,
//              partial GOB-like rectangles), the ASTC blocks copied into ASTC subresources as vkCmdCopyBufferToImage places them
//              (bufferOffset = first block * 16, bufferRowLength = blocks_x * 4) == the same blocks encoded one by one
//   content    synthetic "real-like" images (gradients, photo-like, hard edges, normal map, alpha cut-out) BC-encoded by small encoders
//              in this file (BC1 range fit, BC3, BC7 mode 6), then decoded -> ASTC -> decoded: PSNR against the BC decode and the source
// Build and run (WSL, from this directory):
//   gcc -O2 -fsigned-char -c -I.. ../xbg_texture.c ../../../tex/nvn_deswizzle.c ../../../nvn/nvn_tex.c &&
//   g++ -std=c++20 -O2 -fsigned-char -fsanitize=shift,bounds -fno-sanitize-recover=all -Wall -Wno-unused-function -I.. -I<vulkan headers>
//       -o bc_astc_encode_test bc_astc_encode_test.cpp xbg_texture.o nvn_deswizzle.o nvn_tex.o -lpthread && ./bc_astc_encode_test [--big]
// (the Vulkan headers only for the VkFormat enum, e.g. the NDK's sysroot/usr/include).
#include "glsl_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
extern "C" {
#include "../xbg_texture.h"
}
#include <vulkan/vulkan.h>
#include "../xbg_bc_gpu.h"

#define XBG_HOST 1
static uint XBG_FMT = 0x41u, XBG_OPT = 0u;
#include "../xbg_bc_gpu.glsl"

// ---- the float part of GLSL the encoder uses ----
typedef gvec<float, 2> vec2; typedef gvec<float, 3> vec3; typedef gvec<float, 4> vec4;
static inline float xbg_abs_f(float x) {return x < 0 ? -x : x;}
static inline float min(float a, float b) {return a < b ? a : b;}
static inline float clamp(float x, double lo, double hi) {return x < float(lo) ? float(lo) : x > float(hi) ? float(hi) : x;}
static inline float mix(float a, float b, float t) {return a * (1.0f - t) + b * t;}
template<int N> gvec<float, N> min(const gvec<float, N> &a, const gvec<float, N> &b) {gvec<float, N> r; for(int i = 0; i < N; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; return r;}
template<int N> gvec<float, N> max(const gvec<float, N> &a, const gvec<float, N> &b) {gvec<float, N> r; for(int i = 0; i < N; i++) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return r;}
template<int N> float dot(const gvec<float, N> &a, const gvec<float, N> &b) {float s = 0; for(int i = 0; i < N; i++) s += a.v[i] * b.v[i]; return s;}
struct mat4 { vec4 c[4]; explicit mat4(double s) {for(int j = 0; j < 4; j++) {c[j] = vec4(0.0f); c[j].v[j] = float(s);}} };   // GLSL mat4(s): s on the diagonal
static inline mat4 outerProduct(const vec4 &col, const vec4 &row) {mat4 m(0.0); for(int j = 0; j < 4; j++) m.c[j] = col * row.v[j]; return m;}
static inline mat4 &operator+=(mat4 &a, const mat4 &b) {for(int j = 0; j < 4; j++) a.c[j] += b.c[j]; return a;}
static inline vec4 operator*(const mat4 &m, const vec4 &v) {vec4 r(0.0f); for(int j = 0; j < 4; j++) r += m.c[j] * v.v[j]; return r;}

#include "../xbg_bc_astc_tables.h"
static uint XBG_ASTC_CANDS = 3u;   // the specialization constant
static const uint8_t *g_texels;   // 16 RGBA8 texels of the block being encoded, row-major
static vec4 xbg_astc_texel(uint i) {assert(i < 16); const uint8_t *p = g_texels + i * 4; return vec4(float(p[0]), float(p[1]), float(p[2]), float(p[3]));}
static uint xbg_astc_table(uint i) {assert(i < 2569); return xbg_bc_astc_tables[i];}
#define abs xbg_abs_f
#include "../xbg_bc_astc.glsl"
#undef abs
#include "../xbg_bc_astc.h"

static uint64_t rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd() {rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return uint32_t(rng >> 11);}
static int failures = 0;
#define CHECK(c, ...) do { if(!(c)) { if(failures++ < 40) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while(0)

static void encode(const uint8_t texels[64], uint32_t direct, const uint32_t bc[4], uint8_t out[16]) {
    g_texels = texels; uvec4 o = xbg_astc_encode(direct, uvec4(bc[0], bc[1], bc[2], bc[3]));
    const uint32_t w[4] = {o.x, o.y, o.z, o.w}; memcpy(out, w, 16);
}
// GLSL decoder (xbg_bc_gpu.glsl) of one BC block into 16 RGBA8 texels
static void bc_decode(uint32_t fmt, const uint8_t *blk, uint32_t bb, uint8_t out[64]) {
    XBG_FMT = fmt; XBG_OPT = 0; uint32_t w[4] = {0, 0, 0, 0}; memcpy(w, blk, bb); uvec4 p(w[0], w[1], w[2], w[3]);
    for(int r = 0; r < 4; r++) {XbgRow q = xbg_bc_row(p, r, 4); const uint32_t v[4] = {q.lo.x, q.lo.y, q.lo.z, q.lo.w}; memcpy(out + r * 16, v, 16);}
}

// ---- golden ----
static void test_golden() {
    uint8_t t[64]; for(int i = 0; i < 16; i++) {t[i * 4] = uint8_t(i * 17); t[i * 4 + 1] = uint8_t(255 - i * 15); t[i * 4 + 2] = uint8_t((i & 3) * 80); t[i * 4 + 3] = uint8_t(40 + i * 13);}
    static const uint32_t golden[4] = {0xffc10253, 0xb57f7460, 0xd9d04b38, 0x01256a2b};
    const uint32_t z[4] = {0, 0, 0, 0}; uint8_t o[16]; encode(t, 0, z, o); uint32_t w[4]; memcpy(w, o, 16);
    uint8_t d[64], g[64]; const bool ok = xbg_bc_astc_decode(o, d), gok = xbg_bc_astc_decode(reinterpret_cast<const uint8_t*>(golden), g);
    const double m = xbg_bc_astc_mse(d, t), gm = xbg_bc_astc_mse(g, t);
    printf("golden: %08x %08x %08x %08x (GameNative golden %08x %08x %08x %08x) %s; mse %.2f (golden %.2f)\n", w[0], w[1], w[2], w[3],
           golden[0], golden[1], golden[2], golden[3], memcmp(w, golden, 16) ? "DIFFERENT" : "identical", m, gm);
    CHECK(ok && gok, "golden blocks decode");
    CHECK(m <= gm * 1.25 + 10, "golden tolerance: mse %.2f vs golden %.2f", m, gm);
}

// ---- kat (the device self-test set) ----
static void kat_encode(const XbgAstcKatSet &s, std::vector<uint8_t> &enc) {
    enc.assign(size_t(s.blocks()) * 16, 0);
    for(const auto &c : s.cats) for(uint32_t i = c.first; i < c.first + c.count; i++) encode(s.rgba.data() + size_t(i) * 64, xbg_bc_astc_direct(c.nvn), s.bc.data() + size_t(i) * 4, enc.data() + size_t(i) * 16);
}
static void test_kat(const char *emit) {
    XbgAstcKatSet s; xbg_bc_astc_kat(s);
    std::vector<uint8_t> enc[2]; std::vector<float> mse[2];
    for(int k = 0; k < 2; k++) {
        XBG_ASTC_CANDS = k ? 1u : 3u; kat_encode(s, enc[k]); mse[k].resize(s.blocks());
        for(uint32_t i = 0; i < s.blocks(); i++) {uint8_t d[64]; xbg_bc_astc_decode(enc[k].data() + size_t(i) * 16, d); mse[k][i] = float(xbg_bc_astc_mse(d, s.rgba.data() + size_t(i) * 64));}
    }
    XBG_ASTC_CANDS = 3u;
    if(emit) {
        FILE *f = fopen(emit, "wb"); if(!f) {CHECK(0, "open %s", emit); return;}
        fprintf(f, "// [nonadreno] bc-astc-gpu: expected error (MSE over the 64 RGBA8 channels) of every block of the on-device self-test set\n"
                   "// (xbg_bc_astc.h xbg_bc_astc_kat), as the host build of the encoder writes them, with 3 and with 1 candidates per class\n"
                   "// (DM_GFX_BC_ASTC_CANDS). Generated by engine/tests/bc_astc_encode_test --emit-kat ../xbg_bc_astc_kat.h after any change of\n"
                   "// xbg_bc_astc.glsl, xbg_bc_astc.h or xbg_bc_gpu.h xbg_bc_kat_blocks (the test fails while this table is stale). Do not edit.\n#pragma once\n");
        fprintf(f, "#define XBG_BC_ASTC_KAT_N %uu\n", s.blocks());
        for(int k = 0; k < 2; k++) {
            fprintf(f, "static const float xbg_bc_astc_kat_mse%d[%u] = {", k ? 1 : 3, s.blocks());
            for(uint32_t i = 0; i < s.blocks(); i++) fprintf(f, "%s%#.7gf", i % 8 ? ", " : (i ? ",\n    " : "\n    "), double(mse[k][i]));
            fprintf(f, "\n};\n");
        }
        fclose(f); printf("kat: wrote %s (%u blocks)\n", emit, s.blocks());
    } else {
        for(int k = 0; k < 2; k++) {const float *e = xbg_bc_astc_kat_expected(k ? 1 : 3); unsigned stale = 0;
            for(uint32_t i = 0; i < s.blocks(); i++) if(!e || i >= XBG_BC_ASTC_KAT_N || fabs(double(mse[k][i]) - double(e[i])) > 1e-3 * (1 + mse[k][i])) ++stale;
            CHECK(!stale && XBG_BC_ASTC_KAT_N == s.blocks(), "xbg_bc_astc_kat.h is stale (cands %d: %u of %u blocks differ, table has %u): run --emit-kat", k ? 1 : 3, stale, s.blocks(), unsigned(XBG_BC_ASTC_KAT_N));}
    }
    for(int k = 0; k < 2; k++) {
        XbgAstcKatResult r = xbg_bc_astc_check(s, enc[k].data(), emit ? nullptr : xbg_bc_astc_kat_expected(k ? 1 : 3));
        printf("kat (%d candidates): %u blocks, bad=%u error=%u zero=%u worse=%u, mean mse %.2f, min psnr %.1f dB\n", k ? 1 : 3, r.blocks, r.bad, r.error, r.zero, r.worse, r.mse_sum / r.blocks, r.min_psnr);
        for(size_t c = 0; c < s.cats.size(); c++) printf("  kat %-5s %3u blocks: avg psnr %.2f dB min %.2f dB\n", s.cats[c].name, r.cat[c].n, r.cat[c].psnr_sum / r.cat[c].n, r.cat[c].min_psnr);
        CHECK(r.error == 0 && r.zero == 0 && r.bad == 0, "kat verdict (%d candidates)", k ? 1 : 3);
    }
    if(emit) return;
    // A broken encoder must fail the self-test: endpoint bits flipped, all-zero output, every block one place later, the other candidate set.
    const float *e3 = xbg_bc_astc_kat_expected(3);
    std::vector<uint8_t> bad = enc[0]; for(size_t i = 0; i < bad.size(); i += 16) bad[i + 3] ^= 0xff;
    XbgAstcKatResult rb = xbg_bc_astc_check(s, bad.data(), e3); CHECK(rb.bad > s.blocks() / 2, "mutation (endpoint bits) caught: %u bad", rb.bad);
    std::vector<uint8_t> zero(enc[0].size(), 0); XbgAstcKatResult rz = xbg_bc_astc_check(s, zero.data(), e3); CHECK(rz.bad == s.blocks(), "all-zero output caught");
    std::vector<uint8_t> shift(enc[0].size()); memcpy(shift.data() + 16, enc[0].data(), shift.size() - 16); memcpy(shift.data(), enc[0].data(), 16);
    XbgAstcKatResult rs = xbg_bc_astc_check(s, shift.data(), e3); CHECK(rs.bad > s.blocks() / 2, "mutation (blocks shifted) caught: %u bad", rs.bad);
    XbgAstcKatResult r1 = xbg_bc_astc_check(s, enc[1].data(), e3);
    printf("kat: mutations caught: endpoint bits %u / %u bad, zero output %u, blocks shifted by one %u; 1-candidate output against the 3-candidate table: %u bad\n",
           rb.bad, s.blocks(), rz.bad, rs.bad, r1.bad);
}

// ---- random blocks ----
struct Stat { double sum = 0, min = 99, mse = 0; unsigned n = 0, err = 0, zero = 0, direct = 0, below30 = 0; };
static void add(Stat &st, const uint8_t *ref, const uint8_t *enc) {
    static const uint8_t z[16] = {}; uint8_t d[64]; const bool ok = xbg_bc_astc_decode(enc, d); const double m = xbg_bc_astc_mse(d, ref), p = xbg_bc_astc_psnr(m);
    st.sum += p; st.mse += m; if(p < st.min) st.min = p; ++st.n; st.err += !ok; st.zero += !memcmp(enc, z, 16); st.below30 += p < 30;
    uint32_t w0; memcpy(&w0, enc, 4); st.direct += (w0 & 0x7ff) == 0x42 && ((w0 >> 13) & 15) == 12;
}
static void print(const char *name, const Stat &st) {
    printf("  %-34s %6u blocks: psnr avg %6.2f dB, of mean mse %6.2f dB, min %6.2f dB, <30 dB %5.1f%%, direct %5.1f%%, error %u, zero %u\n", name, st.n, st.sum / st.n,
           xbg_bc_astc_psnr(st.mse / st.n), st.min, 100.0 * st.below30 / st.n, 100.0 * st.direct / st.n, st.err, st.zero);
    CHECK(st.err == 0 && st.zero == 0, "%s: error / zero blocks", name);
}
static void random_block(uint32_t fmt, uint8_t *b, int variant) {
    const uint32_t bb = fmt <= 0x42 || (fmt >= 0x45 && fmt <= 0x46) ? 8 : 16;
    for(uint32_t k = 0; k < bb; k++) b[k] = uint8_t(rnd());
    if(fmt == 0x4d || fmt == 0x4e) {if(variant >= 0) b[0] = uint8_t((uint32_t(b[0]) << (variant + 1)) | (1u << variant));}
    if((fmt == 0x41 || fmt == 0x42 || fmt == 0x45) && variant >= 0) {uint16_t c0, c1; memcpy(&c0, b, 2); memcpy(&c1, b + 2, 2); if((c0 > c1) != (variant == 1)) {memcpy(b, &c1, 2); memcpy(b + 2, &c0, 2);}}
    if((fmt == 0x44 || fmt == 0x48) && variant == 1) {b[1] = b[0]; memset(b + 2, 0, 6);}
    if((fmt == 0x43 || fmt == 0x47) && variant == 1) {const uint8_t a = uint8_t(b[0] & 15); for(int k = 0; k < 8; k++) b[k] = uint8_t(a | a << 4);}
}
static void test_random(unsigned n) {
    struct Case { const char *name; uint32_t fmt; int variant; } cases[] = {
        {"bc1 four-colour (direct)", 0x41, 1}, {"bc1 three-colour", 0x41, 0}, {"bc1a three-colour (transparent)", 0x42, 0}, {"bc1 sRGB four-colour", 0x45, 1},
        {"bc2 random alpha", 0x43, 0}, {"bc2 one alpha (direct)", 0x43, 1}, {"bc3 random alpha", 0x44, 0}, {"bc3 one alpha (direct)", 0x44, 1}, {"bc3 sRGB one alpha", 0x48, 1},
        {"bc7 mode 0", 0x4d, 0}, {"bc7 mode 1", 0x4d, 1}, {"bc7 mode 2", 0x4d, 2}, {"bc7 mode 3", 0x4d, 3}, {"bc7 mode 4", 0x4d, 4}, {"bc7 mode 5", 0x4d, 5},
        {"bc7 mode 6", 0x4d, 6}, {"bc7 mode 7", 0x4d, 7}, {"bc7 sRGB mode 6", 0x4e, 6}};
    printf("random BC blocks -> GPU decoder -> encoder -> reference decoder (PSNR against the decoded BC block):\n");
    for(const Case &c : cases) {
        Stat st; const uint32_t bb = (c.fmt >= 0x41 && c.fmt <= 0x42) || c.fmt == 0x45 || c.fmt == 0x46 ? 8 : 16;
        for(unsigned i = 0; i < n; i++) {
            uint8_t b[16]; random_block(c.fmt, b, c.variant); uint8_t t[64]; bc_decode(c.fmt, b, bb, t);
            uint8_t cpu[128]; xbg_decode_block(c.fmt, b, bb, cpu, sizeof cpu); CHECK(!memcmp(cpu, t, 64), "GLSL decoder == CPU decoder (%s)", c.name);
            uint32_t w[4] = {0, 0, 0, 0}; memcpy(w, b, bb); uint8_t e[16]; encode(t, xbg_bc_astc_direct(c.fmt), w, e); add(st, t, e);
        }
        print(c.name, st);
    }
}

// ---- pipeline: decoder main() + encoder main() over batches, copies into ASTC subresources ----
static void run_decode(const XbgBcGpuBatch &batch, uint32_t block_words, std::vector<uint32_t> &src, std::vector<uint32_t> &out) {
    src.assign(batch.input_bytes() / 4 + 4, 0); batch.write_input(src.data()); out.assign(batch.out / 4 + 8, 0xdeadbeefu);
    const uint32_t regions = uint32_t(batch.regions()), bpp = 4;
    for(uint32_t g = 0; g < batch.blocks; g++) {
        uint32_t lo = 0, hi = regions - 1;
        while(lo < hi) {uint32_t mid = (lo + hi + 1) >> 1; if(src[mid * 8] <= g) lo = mid; else hi = mid - 1;}
        const uint32_t R = lo * 8, local = g - src[R], bx_n = src[R + 1], per_slice = src[R + 2], by_n = src[R + 3];
        const uint32_t z = local / per_slice, rem = local - z * per_slice, by = rem / bx_n, bx = rem - by * bx_n, s = src[R + 4] + local * block_words;
        uvec4 p(src[s], src[s + 1], block_words == 4 ? src[s + 2] : 0u, block_words == 4 ? src[s + 3] : 0u);
        const uint32_t row_words = src[R + 6], base = src[R + 5] + (z * by_n + by) * 4 * row_words + bx * bpp;
        const Bc6Block b6 = xbg_block(p);
        for(int r = 0; r < 4; r++) {XbgRow q = xbg_bc_row_b(b6, p, r, bpp); const uint32_t w[4] = {q.lo.x, q.lo.y, q.lo.z, q.lo.w}; for(uint32_t k = 0; k < 4; k++) out[base + r * row_words + k] = w[k];}
    }
}
// xbg_bc_astc_encode.comp main(): dispatch over [first, end)
static void run_encode(const std::vector<uint32_t> &src, const std::vector<uint32_t> &rgba, uint32_t regions, uint32_t first, uint32_t end, uint32_t flags, std::vector<uint32_t> &dst) {
    for(uint32_t g = first; g < end; g++) {
        uint32_t lo = 0, hi = regions - 1;
        while(lo < hi) {uint32_t mid = (lo + hi + 1) >> 1; if(src[mid * 8] <= g) lo = mid; else hi = mid - 1;}
        const uint32_t R = lo * 8, local = g - src[R], bx_n = src[R + 1], per_slice = src[R + 2], by_n = src[R + 3];
        const uint32_t z = local / per_slice, rem = local - z * per_slice, by = rem / bx_n, bx = rem - by * bx_n;
        const uint32_t row = src[R + 6], t0 = src[R + 5] + (z * by_n + by) * 4 * row + bx * 4;
        uint8_t tex[64]; for(uint32_t i = 0; i < 16; i++) {const uint32_t v = rgba[t0 + (i >> 2) * row + (i & 3)]; memcpy(tex + i * 4, &v, 4);}
        const uint32_t direct = flags & 3, bw = (flags >> 8) & 255; uint32_t bc[4] = {0, 0, 0, 0};
        if(direct) {const uint32_t s = src[R + 4] + local * bw; bc[0] = src[s]; bc[1] = src[s + 1]; if(bw == 4) {bc[2] = src[s + 2]; bc[3] = src[s + 3];}}
        uint8_t o[16]; encode(tex, direct, bc, o); memcpy(&dst[size_t(g) * 4], o, 16);
    }
}
static void test_pipeline(uint32_t fmt) {
    const uint32_t bb = fmt <= 0x42 || fmt == 0x45 || fmt == 0x46 ? 8 : 16;
    struct Tex {uint32_t w, h, d, levels, layers;} texs[] = {{37, 21, 1, 4, 2}, {256, 64, 1, 3, 1}, {13, 9, 5, 2, 1}, {130, 6, 1, 1, 3}, {64, 64, 1, 7, 6}};
    unsigned batches = 0, regions_total = 0, blocks_total = 0, mism = 0;
    for(int big = 0; big < 2; big++) for(const Tex &T : texs) {
        struct Sub {uint32_t W, H, D, bx, by; std::vector<uint8_t> raw, ref, img;};   // ref / img: ASTC blocks, tight rows of the subresource
        std::vector<Sub> subs;
        for(uint32_t layer = 0; layer < T.layers; layer++) for(uint32_t level = 0; level < T.levels; level++) {
            Sub s; s.W = std::max(1u, T.w >> level); s.H = std::max(1u, T.h >> level); s.D = std::max(1u, T.d >> level); s.bx = (s.W + 3) / 4; s.by = (s.H + 3) / 4;
            s.raw.resize(size_t(s.bx) * s.by * s.D * bb); for(size_t k = 0; k < s.raw.size(); k += bb) random_block(fmt, s.raw.data() + k, int(rnd() % 2));
            s.ref.resize(size_t(s.bx) * s.by * s.D * 16); s.img.assign(s.ref.size(), 0xcd);
            for(size_t k = 0; k < size_t(s.bx) * s.by * s.D; k++) {uint8_t t[64]; bc_decode(fmt, s.raw.data() + k * bb, bb, t); uint32_t w[4] = {0, 0, 0, 0}; memcpy(w, s.raw.data() + k * bb, bb);
                encode(t, xbg_bc_astc_direct(fmt), w, s.ref.data() + k * 16);}
            subs.push_back(std::move(s));
        }
        // the engine's planning: whole levels / row bands (upload_banded) and GOB-like rectangles (partial_upload: 16 x 8 blocks)
        const size_t out_cap = big ? (1u << 20) : 64 * size_t(subs[0].bx) * 3 + 64, raw_cap = big ? (1u << 20) : 4096;
        XbgBcGpuBatch batch; struct Copy {size_t sub; size_t offset; uint32_t row_length, image_height; int32_t x, y, z; uint32_t w, h, d;}; std::vector<Copy> copies;
        std::vector<std::vector<uint8_t>> rect_raw;
        auto flush = [&] {
            if(!batch.regions()) return;
            std::vector<uint32_t> src, rgba; run_decode(batch, bb / 4, src, rgba);
            std::vector<uint32_t> dst(size_t(batch.blocks) * 4, 0xabababab);
            // two dispatches with a split point, as a self-test runs ranges: [0, k) and [k, blocks)
            const uint32_t k = batch.blocks / 3; run_encode(src, rgba, uint32_t(batch.regions()), 0, k, xbg_bc_astc_flags(fmt), dst); run_encode(src, rgba, uint32_t(batch.regions()), k, batch.blocks, xbg_bc_astc_flags(fmt), dst);
            ++batches; regions_total += unsigned(batch.regions()); blocks_total += batch.blocks;
            const uint8_t *o = reinterpret_cast<const uint8_t*>(dst.data());
            for(const Copy &c : copies) {Sub &s = subs[c.sub]; CHECK(c.offset % 16 == 0 && c.row_length % 4 == 0 && c.image_height % 4 == 0, "copy alignment");
                const uint32_t nbx = (c.w + 3) / 4, nby = (c.h + 3) / 4, rb = c.row_length / 4, ib = c.image_height / 4;   // blocks: the copy writes whole blocks
                for(uint32_t z = 0; z < c.d; z++) for(uint32_t y = 0; y < nby; y++)
                    memcpy(s.img.data() + ((size_t(c.z + z) * s.by + c.y / 4 + y) * s.bx + c.x / 4) * 16, o + c.offset + ((size_t(z) * ib + y) * rb) * 16, size_t(nbx) * 16);}
            batch.clear(); copies.clear(); rect_raw.clear();
        };
        auto add_rect = [&](size_t k, const uint8_t *raw, uint32_t nbx, uint32_t nby, uint32_t nz, uint32_t bx0, uint32_t by0, uint32_t z0) {
            Sub &s = subs[k]; const size_t n = size_t(nbx) * nby * nz;
            if(!batch.fits(n * bb, n * 64, raw_cap, out_cap, big ? 4096 : 3, 1u << 22, uint32_t(n))) flush();
            const uint32_t first = batch.blocks; batch.add(raw, nbx, nby, nz, bb, 4);
            // BcUpload::blocks for an ASTC image: the copy reads the encoder's output at first block * 16, rows of nbx * 4 texels
            copies.push_back({k, size_t(first) * 16, nbx * 4, nby * 4, int32_t(bx0 * 4), int32_t(by0 * 4), int32_t(z0), std::min(nbx * 4, s.W - bx0 * 4), std::min(nby * 4, s.H - by0 * 4), nz});
        };
        for(size_t k = 0; k < subs.size(); k++) {
            Sub &s = subs[k];
            if(k % 2 == 0) {std::vector<XbgBand> bands; xbg_upload_bands(s.bx, s.by, s.D, 64, out_cap, bands);
                for(const XbgBand &b : bands) add_rect(k, s.raw.data() + (size_t(b.z0) * s.by + b.by0) * s.bx * bb, s.bx, b.nby, b.nz, 0, b.by0, b.z0);}
            else for(uint32_t z = 0; z < s.D; z++) for(uint32_t by0 = 0; by0 < s.by; by0 += 8) for(uint32_t bx0 = 0; bx0 < s.bx; bx0 += 16) {   // GOB blocks of a partial upload
                const uint32_t nbx = std::min(16u, s.bx - bx0), nby = std::min(8u, s.by - by0); std::vector<uint8_t> r(size_t(nbx) * nby * bb);
                for(uint32_t y = 0; y < nby; y++) memcpy(r.data() + size_t(y) * nbx * bb, s.raw.data() + ((size_t(z) * s.by + by0 + y) * s.bx + bx0) * bb, size_t(nbx) * bb);
                add_rect(k, r.data(), nbx, nby, 1, bx0, by0, z);}
        }
        flush();
        for(const Sub &s : subs) if(memcmp(s.img.data(), s.ref.data(), s.ref.size())) {++mism; CHECK(0, "pipeline fmt %#x %ux%ux%u (caps %s): ASTC subresource differs", fmt, s.W, s.H, s.D, big ? "large" : "small");}
    }
    printf("pipeline fmt %#x: decode + encode main() over %u batches, %u regions, %u blocks: subresources == block-by-block encode%s\n", fmt, batches, regions_total, blocks_total, mism ? " (MISMATCH)" : "");
}

// ---- content: synthetic images, small BC encoders ----
static void img_make(int kind, uint32_t W, uint32_t H, std::vector<uint8_t> &px) {
    px.resize(size_t(W) * H * 4);
    for(uint32_t y = 0; y < H; y++) for(uint32_t x = 0; x < W; x++) {
        uint8_t *p = px.data() + (size_t(y) * W + x) * 4; double fx = x / double(W), fy = y / double(H); double r, g, b, a = 255;
        switch(kind) {
        case 0: r = 255 * fx; g = 255 * fy; b = 128 + 127 * sin(6.28 * (fx + fy)); break;                                            // gradients
        case 1: {double n = (rnd() % 1000) / 1000.0 - 0.5; r = 120 + 60 * sin(23 * fx) * cos(17 * fy) + 30 * sin(71 * fx + 3) + 18 * n;   // photo-like
                 g = 100 + 50 * sin(19 * fy + 1) + 25 * cos(53 * fx * fy) + 18 * n; b = 80 + 40 * cos(29 * fx - 13 * fy) + 18 * n; break;}
        case 2: {const bool in = (int(x / 8) + int(y / 11)) % 3 == 0 || ((x * 7 + y * 3) % 37) < 4; r = in ? 240 : 25; g = in ? 230 : 30; b = in ? 210 : 60; break;}   // hard edges
        case 3: {double nx = 0.4 * sin(12 * fx) * cos(9 * fy), ny = 0.4 * cos(11 * fx + 2 * fy), nz = sqrt(std::max(0.0, 1 - nx * nx - ny * ny));               // normal map
                 r = 127.5 + 127.5 * nx; g = 127.5 + 127.5 * ny; b = 127.5 + 127.5 * nz; break;}
        default: {double d = sin(17 * fx) * cos(13 * fy) + 0.3 * sin(61 * fx * fy); r = 60 + 90 * fx; g = 120 + 80 * fy; b = 40;                              // alpha cut-out
                 a = d > 0.2 ? 255 : d > 0.1 ? 255 * (d - 0.1) / 0.1 : 0; break;}
        }
        auto c = [](double v) {return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);}; p[0] = c(r); p[1] = c(g); p[2] = c(b); p[3] = c(a);
    }
}
static void axis_fit(const uint8_t *t, int ch, float lo[4], float hi[4]) {   // endpoints along the principal axis of ch channels
    float mean[4] = {0, 0, 0, 0}; for(int i = 0; i < 16; i++) for(int c = 0; c < ch; c++) mean[c] += t[i * 4 + c] / 16.0f;
    float cov[4][4] = {}; for(int i = 0; i < 16; i++) for(int a = 0; a < ch; a++) for(int b = 0; b < ch; b++) cov[a][b] += (t[i * 4 + a] - mean[a]) * (t[i * 4 + b] - mean[b]);
    float v[4] = {1, 1, 1, 1}; for(int it = 0; it < 12; it++) {float n[4] = {0, 0, 0, 0}, m = 0; for(int a = 0; a < ch; a++) {for(int b = 0; b < ch; b++) n[a] += cov[a][b] * v[b]; m = std::max(m, fabsf(n[a]));} if(m <= 0) break; for(int a = 0; a < ch; a++) v[a] = n[a] / m;}
    float l = 0; for(int a = 0; a < ch; a++) l += v[a] * v[a]; float tmin = 0, tmax = 0;
    if(l > 0) {tmin = 1e9f; tmax = -1e9f; for(int i = 0; i < 16; i++) {float s = 0; for(int a = 0; a < ch; a++) s += (t[i * 4 + a] - mean[a]) * v[a]; s /= l; tmin = std::min(tmin, s); tmax = std::max(tmax, s);}}
    for(int a = 0; a < ch; a++) {lo[a] = std::clamp(mean[a] + v[a] * tmin, 0.0f, 255.0f); hi[a] = std::clamp(mean[a] + v[a] * tmax, 0.0f, 255.0f);}
}
static void bc1_color(const uint8_t *t, uint8_t out[8]) {   // four-colour BC1 block, range fit + nearest of the 4 palette colours
    float lo[4], hi[4]; axis_fit(t, 3, lo, hi);
    auto q = [](const float *c) {return uint16_t((int(c[0] * 31 / 255 + 0.5f) << 11) | (int(c[1] * 63 / 255 + 0.5f) << 5) | int(c[2] * 31 / 255 + 0.5f));};
    uint16_t c0 = q(hi), c1 = q(lo); if(c0 < c1) std::swap(c0, c1); if(c0 == c1) {if(c1) --c1; else ++c0;}
    auto ex = [](uint16_t c, int k) {int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31; return k == 0 ? (r << 3 | r >> 2) : k == 1 ? (g << 2 | g >> 4) : (b << 3 | b >> 2);};
    int pal[4][3]; for(int k = 0; k < 3; k++) {pal[0][k] = ex(c0, k); pal[1][k] = ex(c1, k); pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3; pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;}
    uint32_t idx = 0; for(int i = 0; i < 16; i++) {int best = 0, bd = 1 << 30; for(int p = 0; p < 4; p++) {int d = 0; for(int k = 0; k < 3; k++) {int e = t[i * 4 + k] - pal[p][k]; d += e * e;} if(d < bd) {bd = d; best = p;}} idx |= uint32_t(best) << (2 * i);}
    memcpy(out, &c0, 2); memcpy(out + 2, &c1, 2); memcpy(out + 4, &idx, 4);
}
static void bc3_alpha(const uint8_t *t, uint8_t out[8]) {   // eight-value alpha block, min / max
    int a0 = 0, a1 = 255; for(int i = 0; i < 16; i++) {a0 = std::max(a0, int(t[i * 4 + 3])); a1 = std::min(a1, int(t[i * 4 + 3]));}
    if(a0 == a1) {out[0] = out[1] = uint8_t(a0); memset(out + 2, 0, 6); return;}
    int pal[8] = {a0, a1}; for(int k = 1; k < 7; k++) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    uint64_t bits = 0; for(int i = 0; i < 16; i++) {int best = 0, bd = 1 << 30; for(int p = 0; p < 8; p++) {int d = abs(t[i * 4 + 3] - pal[p]); if(d < bd) {bd = d; best = p;}} bits |= uint64_t(best) << (3 * i);}
    out[0] = uint8_t(a0); out[1] = uint8_t(a1); for(int k = 0; k < 6; k++) out[2 + k] = uint8_t(bits >> (8 * k));
}
static void bc7_mode6(const uint8_t *t, uint8_t out[16]) {   // mode 6: RGBA 7 bits + p-bit, 4-bit indices, one subset
    float lo[4], hi[4]; axis_fit(t, 4, lo, hi);
    int e[2][4], pb[2]; const float *src[2] = {lo, hi};
    for(int k = 0; k < 2; k++) {int best = 0; double bd = 1e30; for(int p = 0; p < 2; p++) {double d = 0; for(int c = 0; c < 4; c++) {int v = std::clamp(int((src[k][c] - p) / 2 + 0.5f), 0, 127); double r = v * 2 + p - src[k][c]; d += r * r;} if(d < bd) {bd = d; best = p;}}
        pb[k] = best; for(int c = 0; c < 4; c++) e[k][c] = std::clamp(int((src[k][c] - best) / 2 + 0.5f), 0, 127);}
    int full[2][4]; for(int k = 0; k < 2; k++) for(int c = 0; c < 4; c++) full[k][c] = e[k][c] * 2 + pb[k];
    static const int w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    int idx[16]; for(int i = 0; i < 16; i++) {int best = 0; double bd = 1e30; for(int q = 0; q < 16; q++) {double d = 0; for(int c = 0; c < 4; c++) {int v = (full[0][c] * (64 - w4[q]) + full[1][c] * w4[q] + 32) >> 6; double r = v - t[i * 4 + c]; d += r * r;} if(d < bd) {bd = d; best = q;}} idx[i] = best;}
    if(idx[0] & 8) {for(int c = 0; c < 4; c++) std::swap(e[0][c], e[1][c]); std::swap(pb[0], pb[1]); for(int &v : idx) v = 15 - v;}   // anchor index MSB 0
    uint8_t b[16] = {}; int pos = 0; auto put = [&](uint32_t v, int n) {for(int k = 0; k < n; k++, pos++) if(v >> k & 1) b[pos >> 3] |= uint8_t(1u << (pos & 7));};
    put(1u << 6, 7); for(int c = 0; c < 4; c++) {put(uint32_t(e[0][c]), 7); put(uint32_t(e[1][c]), 7);} put(uint32_t(pb[0]), 1); put(uint32_t(pb[1]), 1);
    for(int i = 0; i < 16; i++) put(uint32_t(idx[i]), i ? 4 : 3);
    memcpy(out, b, 16);
}
static void test_content(uint32_t W, uint32_t H) {
    static const char *kinds[5] = {"gradients", "photo-like", "hard edges", "normal map", "alpha cut-out"};
    printf("content %ux%u: source -> BC (small encoders here) -> GPU decoder -> ASTC encoder -> reference decoder; PSNR of the image:\n", W, H);
    for(uint32_t fmt : {0x41u, 0x44u, 0x4du}) for(int kind = 0; kind < 5; kind++) {
        if(fmt == 0x41 && kind == 4) continue;
        std::vector<uint8_t> px; img_make(kind, W, H, px); double e_bc = 0, e_astc = 0, e_src = 0; Stat st; const uint32_t bb = fmt == 0x41 ? 8 : 16;
        for(uint32_t by = 0; by < H / 4; by++) for(uint32_t bx = 0; bx < W / 4; bx++) {
            uint8_t t[64]; for(int y = 0; y < 4; y++) memcpy(t + y * 16, px.data() + ((size_t(by) * 4 + y) * W + bx * 4) * 4, 16);
            if(fmt == 0x41) for(int i = 0; i < 16; i++) t[i * 4 + 3] = 255;
            uint8_t b[16]; if(fmt == 0x41) bc1_color(t, b); else if(fmt == 0x44) {bc3_alpha(t, b); bc1_color(t, b + 8);} else bc7_mode6(t, b);
            uint8_t d[64]; bc_decode(fmt, b, bb, d); uint32_t w[4] = {0, 0, 0, 0}; memcpy(w, b, bb); uint8_t e[16]; encode(d, xbg_bc_astc_direct(fmt), w, e); add(st, d, e);
            uint8_t a[64]; xbg_bc_astc_decode(e, a);
            const int ch = fmt == 0x41 ? 3 : 4;
            for(int i = 0; i < 16; i++) for(int c = 0; c < ch; c++) {double x = double(d[i * 4 + c]) - t[i * 4 + c], y2 = double(a[i * 4 + c]) - d[i * 4 + c], z = double(a[i * 4 + c]) - t[i * 4 + c]; e_bc += x * x; e_astc += y2 * y2; e_src += z * z;}
        }
        const double n = double(W) * H * (fmt == 0x41 ? 3 : 4);
        char name[64]; snprintf(name, sizeof name, "%s %s", fmt == 0x41 ? "bc1" : fmt == 0x44 ? "bc3" : "bc7m6", kinds[kind]);
        printf("  %-24s BC vs source %6.2f dB | ASTC vs BC %6.2f dB | ASTC vs source %6.2f dB | blocks: avg %6.2f min %6.2f dB, direct %5.1f%%\n", name,
               xbg_bc_astc_psnr(e_bc / n), xbg_bc_astc_psnr(e_astc / n), xbg_bc_astc_psnr(e_src / n), st.sum / st.n, st.min, 100.0 * st.direct / st.n);
        CHECK(st.err == 0 && st.zero == 0, "content %s: error / zero blocks", name);
    }
}

int main(int argc, char **argv) {
    const char *emit = nullptr; bool big = false;
    for(int i = 1; i < argc; i++) {if(!strcmp(argv[i], "--emit-kat") && i + 1 < argc) emit = argv[++i]; else if(!strcmp(argv[i], "--big")) big = true;}
    test_golden();
    test_kat(emit);
    if(emit) {printf("%s\n", failures ? "FAILURES" : "kat table written"); return failures ? 1 : 0;}
    test_random(big ? 100000 : 8000);
    for(uint32_t f : {0x41u, 0x42u, 0x43u, 0x44u, 0x45u, 0x48u, 0x4du, 0x4eu}) test_pipeline(f);
    test_content(big ? 1024 : 256, big ? 1024 : 256);
    printf("%s (%d failures)\n", failures ? "FAILED" : "all bc_astc_encode checks passed", failures);
    return failures ? 1 : 0;
}
