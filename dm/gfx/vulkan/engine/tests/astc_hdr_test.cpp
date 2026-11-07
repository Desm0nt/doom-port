// [nonadreno] ASTC HDR safety of the BC6H -> ASTC overlay (docs/BUGS_AND_FIXES.md 2.37): the production code of xbg_astc_hdr.h
// against the independent reference decoder xbg_astc_ref_decode.h (written from the Khronos spec; tools/astc_hdr_scan.cpp shows it
// bit-exact with astcenc on every block of the real overlay):
//   - every embedded self-test vector (xbg_astc_hdr_data.h) decodes to its expected FP16 bits, finite, without an error block
//   - xbg_astc_block_error == the reference decoder's error verdict (random and mutated blocks, both profiles)
//   - xbg_astc_sanitize: only error blocks change; the replacement decodes to black (HDR alpha 1); an all-zero block (memory
//     never written) is an error block whose HDR decode is NaN, which is the hazard
//   - the self-test plan and verdict: a GPU simulated with the reference decoder passes (fetch, cube faces through the shader's
//     direction formula and the Vulkan face selection, linear corners); NaN / Inf / wrong values / a wrong face fail it
//   - DM_GFX_ASTC_HDR_SELFTEST parsing
// Build and run (WSL or any C++17 compiler, from this directory):
//   g++ -std=c++17 -O2 -Wall -Wno-unused-function -o astc_hdr_test astc_hdr_test.cpp && ./astc_hdr_test
#include "../xbg_astc_hdr.h"
#include "../xbg_astc_ref_decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <random>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

// Vulkan cube map face selection (spec "Cube Map Face Selection"): direction -> face, s, t in [0, 1]
static void cube_select(float rx, float ry, float rz, int &face, float &s, float &t) {
    const float ax = fabsf(rx), ay = fabsf(ry), az = fabsf(rz);
    float sc, tc, ma;
    if (ax >= ay && ax >= az) { ma = ax; if (rx >= 0) { face = 0; sc = -rz; tc = -ry; } else { face = 1; sc = rz; tc = -ry; } }
    else if (ay >= az) { ma = ay; if (ry >= 0) { face = 2; sc = rx; tc = rz; } else { face = 3; sc = rx; tc = -rz; } }
    else { ma = az; if (rz >= 0) { face = 4; sc = rx; tc = -ry; } else { face = 5; sc = -rx; tc = -ry; } }
    s = 0.5f * (sc / ma + 1); t = 0.5f * (tc / ma + 1);
}
// xbg_astc_hdr_selftest.comp kind 1, ported line for line
static void shader_direction(uint32_t x, uint32_t y, uint32_t face, float dim, float d[3]) {
    const float u = 2.0f * (float(x) + 0.5f) / dim - 1.0f, w = 2.0f * (float(y) + 0.5f) / dim - 1.0f;
    if (face == 0) { d[0] = 1; d[1] = -w; d[2] = -u; }
    else if (face == 1) { d[0] = -1; d[1] = -w; d[2] = u; }
    else if (face == 2) { d[0] = u; d[1] = 1; d[2] = w; }
    else if (face == 3) { d[0] = u; d[1] = -1; d[2] = -w; }
    else if (face == 4) { d[0] = u; d[1] = -w; d[2] = 1; }
    else { d[0] = -u; d[1] = -w; d[2] = -1; }
}

// A simulated device: the image as the plan uploads it, decoded with the reference decoder, sampled like the shader.
struct SimGpu {
    const XbgAhsPlan &p;
    int swap_faces = 0;                  // fault injection: faces 0 and 1 swapped in the cube view
    explicit SimGpu(const XbgAhsPlan &plan) : p(plan) {}
    void texel(uint32_t layer, uint32_t level, uint32_t x, uint32_t y, float out[4]) const {
        for (const XbgAhsRegion &r : p.regions) if (r.layer == layer && r.level == level) {
            const uint32_t nb = (r.width + 3) / 4, bx = x / 4, by = y / 4;
            const uint8_t *blk = &p.blocks[r.offset + (size_t(by) * nb + bx) * 16];
            uint16_t px[64]; astcref::decode_block(blk, 4, 4, 1, px);
            for (int c = 0; c < 4; c++) out[c] = xbg_ahs_half(px[((y & 3) * 4 + (x & 3)) * 4 + c]);
            return;
        }
        out[0] = out[1] = out[2] = out[3] = -1;
    }
    std::vector<float> run() const {
        std::vector<float> got(p.queries.size() * 4);
        for (size_t i = 0; i < p.queries.size(); i++) {
            const XbgAhsQuery &q = p.queries[i];
            const uint32_t level = q.level_kind & 0xff, kind = q.level_kind >> 8, dim = XBG_AHS_SIZE >> level;
            float *g = &got[4 * i];
            if (kind == XBG_AHS_FETCH) texel(q.layer, level, q.x, q.y, g);
            else if (kind == XBG_AHS_CUBE) {
                float d[3]; shader_direction(q.x, q.y, q.layer % 6, float(dim), d);
                int face; float s, t; cube_select(d[0], d[1], d[2], face, s, t);
                if (swap_faces && face < 2) face ^= 1;
                const uint32_t x = uint32_t(floorf(s * dim)), y = uint32_t(floorf(t * dim));
                texel((q.layer / 6) * 6 + uint32_t(face), level, x, y, g);
            } else {
                float a[4][4];
                for (int k = 0; k < 4; k++) texel(q.layer, level, q.x - 1 + (k & 1), q.y - 1 + (k >> 1), a[k]);
                for (int c = 0; c < 4; c++) g[c] = 0.25f * (a[0][c] + a[1][c] + a[2][c] + a[3][c]);
            }
        }
        return got;
    }
};

int main() {
    // ---- the embedded vectors: the reference decoder agrees bit for bit with the expected values (from astcenc)
    CHECK(XBG_ASTC_HDR_TEST_BLOCKS >= 16);
    int clamp_vectors = 0, void_vectors = 0, cems = 0;
    for (int i = 0; i < XBG_ASTC_HDR_TEST_BLOCKS; i++) {
        uint16_t px[64]; AstcRefInfo info;
        astcref::decode_block(xbg_astc_hdr_tests[i].block, 4, 4, 1, px, &info);
        CHECK(!info.error);
        CHECK(!xbg_astc_block_error(xbg_astc_hdr_tests[i].block, 1));
        bool same = true, finite = true;
        for (int t = 0; t < 16; t++) for (int c = 0; c < 4; c++) {
            if (px[t * 4 + c] != xbg_astc_hdr_tests[i].expected[t][c]) same = false;
            if (!isfinite(xbg_ahs_half(xbg_astc_hdr_tests[i].expected[t][c]))) finite = false;
        }
        if (!same) printf("vector %d (%s): reference decode differs from the expected values\n", i, xbg_astc_hdr_tests[i].label);
        CHECK(same); CHECK(finite);
        clamp_vectors += info.lns_clamped > 0; void_vectors += info.void_extent; cems |= info.void_extent ? 0 : 1 << info.cem[0];
    }
    printf("vectors: %d (LNS clamp exercised by %d, void extents %d, endpoint modes mask %#x)\n", XBG_ASTC_HDR_TEST_BLOCKS, clamp_vectors, void_vectors, cems);
    CHECK(clamp_vectors >= 1);   // the clamp to 0x7BFF is tested on the GPU
    CHECK((cems & (1 << 7)) && (cems & (1 << 11)));

    // ---- the error-block predicate == the reference decoder (random blocks, and valid blocks with single bits flipped)
    std::mt19937_64 rng(2037);
    unsigned mismatches[2] = {0, 0}, errors[2] = {0, 0}, tried = 0;
    auto check_block = [&](const uint8_t *b) {
        for (int hdr = 0; hdr < 2; hdr++) {
            uint16_t px[64]; AstcRefInfo info; astcref::decode_block(b, 4, 4, hdr, px, &info);
            const int pred = xbg_astc_block_error(b, hdr);
            if (pred != info.error) { if (mismatches[hdr]++ < 4) { printf("predicate %d, reference %d (%s), hdr %d: ", pred, info.error, info.why ? info.why : "-", hdr); for (int k = 0; k < 16; k++) printf("%02x", b[k]); printf("\n"); } }
            errors[hdr] += info.error;
        }
        ++tried;
    };
    for (int n = 0; n < 400000; n++) { uint8_t b[16]; for (int k = 0; k < 16; k++) b[k] = uint8_t(rng()); check_block(b); }
    for (int n = 0; n < 200000; n++) {   // random block mode / partition / CEM fields over random payload: more valid blocks
        uint8_t b[16]; for (int k = 0; k < 16; k++) b[k] = uint8_t(rng());
        if (n & 1) { b[0] = uint8_t(rng() & 0xFF); b[1] = uint8_t((b[1] & 0xF8) | (rng() & 7)); }
        check_block(b);
    }
    for (int i = 0; i < XBG_ASTC_HDR_TEST_BLOCKS; i++) for (int bit = 0; bit < 128; bit++) {
        uint8_t b[16]; memcpy(b, xbg_astc_hdr_tests[i].block, 16); b[bit >> 3] ^= uint8_t(1 << (bit & 7)); check_block(b);
    }
    printf("predicate: %u blocks, reference errors LDR %u HDR %u, mismatches LDR %u HDR %u\n", tried, errors[0], errors[1], mismatches[0], mismatches[1]);
    CHECK(!mismatches[0] && !mismatches[1]);
    CHECK(errors[1] > 1000 && errors[1] < tried);

    // ---- all-zero memory: an error block; HDR decodes to NaN, LDR to the error colour
    {
        const uint8_t z[16] = {0};
        uint16_t px[64]; AstcRefInfo info; astcref::decode_block(z, 4, 4, 1, px, &info);
        CHECK(info.error && isnan(xbg_ahs_half(px[0])));
        CHECK(xbg_astc_block_error(z, 1) && xbg_astc_block_error(z, 0));
    }
    // ---- the replacement blocks: valid, black (HDR alpha 1.0, LDR alpha 0)
    {
        uint8_t b[16]; uint16_t px[64]; AstcRefInfo info;
        xbg_astc_black_block(b, 1); astcref::decode_block(b, 4, 4, 1, px, &info);
        CHECK(!info.error && info.void_extent && info.void_hdr && px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0x3C00 && px[63] == 0x3C00);
        CHECK(!xbg_astc_block_error(b, 1) && !xbg_astc_hdr_void_nonfinite(b));
        xbg_astc_black_block(b, 0); astcref::decode_block(b, 4, 4, 0, px, &info);
        CHECK(!info.error && info.void_extent && !info.void_hdr && px[0] == 0 && px[3] == 0);
        // a HDR void extent with an Inf / NaN / negative constant is replaced by the sanitizer (BC6H never makes one)
        xbg_astc_black_block(b, 1); b[8] = 0x00; b[9] = 0x7C; CHECK(xbg_astc_hdr_void_nonfinite(b));
        xbg_astc_black_block(b, 1); b[9] = 0x80; CHECK(xbg_astc_hdr_void_nonfinite(b));
    }
    // ---- sanitize: valid blocks untouched, error blocks replaced, afterwards every block decodes finite
    {
        std::vector<uint8_t> buf;
        for (int i = 0; i < XBG_ASTC_HDR_TEST_BLOCKS; i++) buf.insert(buf.end(), xbg_astc_hdr_tests[i].block, xbg_astc_hdr_tests[i].block + 16);
        const std::vector<uint8_t> valid = buf;
        size_t zero = 0;
        CHECK(xbg_astc_sanitize(buf.data(), buf.size(), 1, &zero) == 0 && zero == 0 && buf == valid);
        buf.insert(buf.end(), 16, 0);                       // never-written memory
        for (int n = 0; n < 2000; n++) for (int k = 0; k < 16; k++) buf.push_back(uint8_t(rng()));
        unsigned errs = 0;
        for (size_t at = 0; at < buf.size(); at += 16) { uint16_t px[64]; AstcRefInfo info; astcref::decode_block(&buf[at], 4, 4, 1, px, &info); errs += info.error; }
        const size_t n = xbg_astc_sanitize(buf.data(), buf.size(), 1, &zero);
        CHECK(n >= errs && zero == 1);
        CHECK(!memcmp(buf.data(), valid.data(), valid.size()));
        unsigned nonfinite = 0, still = 0;
        for (size_t at = 0; at < buf.size(); at += 16) {
            uint16_t px[64]; AstcRefInfo info; astcref::decode_block(&buf[at], 4, 4, 1, px, &info); still += info.error;
            for (int k = 0; k < 64; k++) nonfinite += !isfinite(xbg_ahs_half(px[k]));
        }
        printf("sanitize: %zu of %zu random blocks replaced (reference errors %u); after it: errors %u, non-finite texels %u\n", n, buf.size() / 16 - valid.size() / 16, errs, still, nonfinite);
        CHECK(still == 0 && nonfinite == 0);
    }
    // ---- the self-test plan and verdict with a simulated GPU
    {
        XbgAhsPlan plan; xbg_astc_hdr_plan(plan, xbg_astc_hdr_tests, XBG_ASTC_HDR_TEST_BLOCKS, true);
        size_t want_blocks = 0; for (int l = 0; l < XBG_AHS_LEVELS; l++) { const int nb = ((XBG_AHS_SIZE >> l) + 3) / 4; want_blocks += size_t(nb) * nb; }
        CHECK(plan.blocks.size() == want_blocks * XBG_AHS_LAYERS * 16 && plan.regions.size() == size_t(XBG_AHS_LEVELS) * XBG_AHS_LAYERS);
        CHECK(plan.expected.size() == plan.queries.size() * 4 && plan.vector_of.size() == plan.queries.size() && plan.edge.size() == plan.queries.size());
        bool every_vector = true;   // each vector is placed at least once
        for (int i = 0; i < XBG_ASTC_HDR_TEST_BLOCKS; i++) { bool f = false; for (int v : plan.vector_of) f |= v == i; every_vector &= f; }
        CHECK(every_vector);
        SimGpu gpu(plan);
        std::vector<float> got = gpu.run();
        XbgAhsVerdict v = xbg_astc_hdr_judge(plan, got.data());
        CHECK(v.edge_bad == 0 && v.edge_nonfinite == 0);
        printf("plan: %zu blocks, %zu queries; simulated GPU: pass=%d checked=%u max_rel=%g zero_block_nan=%d\n", plan.blocks.size() / 16, plan.queries.size(), v.pass, v.checked, v.max_rel, v.zero_block_nan);
        CHECK(v.pass && v.zero_block_nan && v.max_rel < 1e-3f);
        // a decoder that rounds differently by one FP16 ulp still passes
        std::vector<float> g2;
        g2 = got; for (size_t i = 0; i < g2.size(); i++) if (isfinite(g2[i])) g2[i] *= 1.0f + 1.0f / 1024;
        CHECK(xbg_astc_hdr_judge(plan, g2.data()).pass);
        // NaN, Inf (no clamp), a wrong value, a lost write, a wrong cube face: all fail
        size_t fetch0 = 0; while (plan.vector_of[fetch0] < 0 || plan.edge[fetch0] || (plan.queries[fetch0].level_kind >> 8) != XBG_AHS_FETCH) fetch0++;
        // an edge vector (65504 / LNS exponent 31) returning Inf, as a decoder without the spec's clamp would: reported, not judged
        size_t edge0 = 0; while (edge0 < plan.edge.size() && !plan.edge[edge0]) edge0++;
        CHECK(edge0 < plan.edge.size());
        if (edge0 < plan.edge.size()) { g2 = got; g2[4 * edge0] = INFINITY; v = xbg_astc_hdr_judge(plan, g2.data()); CHECK(v.pass && v.edge_nonfinite == 1 && v.edge_bad == 1); }
        g2 = got; g2[4 * fetch0] = NAN; v = xbg_astc_hdr_judge(plan, g2.data()); CHECK(!v.pass && v.nonfinite == 1 && v.first_bad == plan.vector_of[fetch0]);
        g2 = got; g2[4 * fetch0 + 1] = INFINITY; v = xbg_astc_hdr_judge(plan, g2.data()); CHECK(!v.pass && v.nonfinite == 1);
        g2 = got; g2[4 * fetch0 + 2] = got[4 * fetch0 + 2] * 1.5f + 0.01f; v = xbg_astc_hdr_judge(plan, g2.data()); CHECK(!v.pass && v.fetch_bad == 1);
        g2 = got; g2[4 * fetch0] = -12345.0f; CHECK(!xbg_astc_hdr_judge(plan, g2.data()).pass);
        SimGpu bad(plan); bad.swap_faces = 1; g2 = bad.run(); v = xbg_astc_hdr_judge(plan, g2.data());
        CHECK(!v.pass && v.cube_bad > 0 && v.fetch_bad == 0);
        // the zero block's NaN is reported, never judged
        g2 = got; for (size_t i = 0; i < plan.queries.size(); i++) if (plan.vector_of[i] < 0) for (int c = 0; c < 4; c++) g2[4 * i + c] = 0;
        v = xbg_astc_hdr_judge(plan, g2.data()); CHECK(v.pass && !v.zero_block_nan);
    }
    // ---- the cube direction formula of the shader lands on every texel of every face
    for (uint32_t face = 0; face < 6; face++) for (uint32_t dim = 2; dim <= 128; dim *= 2) for (uint32_t y = 0; y < dim; y++) for (uint32_t x = 0; x < dim; x++) {
        float d[3]; shader_direction(x, y, face, float(dim), d); int f; float s, t; cube_select(d[0], d[1], d[2], f, s, t);
        if (uint32_t(f) != face || uint32_t(floorf(s * dim)) != x || uint32_t(floorf(t * dim)) != y) { CHECK(!"cube direction"); face = 6; break; }
    }
    // ---- EAC R11 (DM_GFX_ASTC_CPU): base * 8 + 4 + modifier (multiplier 0 -> x1, else x multiplier x 8), indices column-major
    {
        // base 128, multiplier 0, table 13 {-1,-2,-3,-10,0,1,2,9}; every index 4 (modifier 0) except pixel x=1 y=0 (column-major 4): 7 (+9)
        uint64_t v = (uint64_t(128) << 56) | (uint64_t(0) << 52) | (uint64_t(13) << 48);
        for (int i = 0; i < 16; i++) v |= uint64_t(i == 4 ? 7 : 4) << (45 - 3 * i);
        uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = uint8_t(v >> (56 - 8 * i));
        uint16_t out[16]; xbg_eac_r11_block(b, out);
        CHECK(out[0] == 1028 && out[1] == 1037 && out[4] == 1028 && out[15] == 1028);
        // multiplier 2, table 0 index 3 (-15): 1028 - 15 * 2 * 8 = 788; clamping at both ends
        v = (uint64_t(128) << 56) | (uint64_t(2) << 52) | (uint64_t(0) << 48); for (int i = 0; i < 16; i++) v |= uint64_t(3) << (45 - 3 * i);
        for (int i = 0; i < 8; i++) b[i] = uint8_t(v >> (56 - 8 * i));
        xbg_eac_r11_block(b, out); CHECK(out[0] == 788 && out[9] == 788);
        v = (uint64_t(255) << 56) | (uint64_t(15) << 52) | (uint64_t(0) << 48); for (int i = 0; i < 16; i++) v |= uint64_t(7) << (45 - 3 * i);
        for (int i = 0; i < 8; i++) b[i] = uint8_t(v >> (56 - 8 * i));
        xbg_eac_r11_block(b, out); CHECK(out[0] == 2047);
        v = 0; for (int i = 0; i < 16; i++) v |= uint64_t(3) << (45 - 3 * i); v |= uint64_t(15) << 52;
        for (int i = 0; i < 8; i++) b[i] = uint8_t(v >> (56 - 8 * i));
        xbg_eac_r11_block(b, out); CHECK(out[0] == 0);
    }
    // ---- the EAC census discriminator: a smooth BC4 block reads as BC4, a smooth EAC block as EAC, a flat one is undecided
    {
        uint8_t b[8] = {255, 0};                       // BC4 8-value palette 255, 0, 219, 182, 146, 109, 73, 36: columns 0 2 3 4
        uint64_t idx = 0; const int col[4] = {0, 2, 3, 4};
        for (int i = 0; i < 16; i++) idx |= uint64_t(col[i & 3]) << (3 * i);
        for (int i = 0; i < 6; i++) b[2 + i] = uint8_t(idx >> (8 * i));
        uint16_t c[16]; xbg_bc4_block_11(b, c); CHECK(c[0] == 2047 && c[3] == 4 * 255 * 2047 / (7 * 255));
        CHECK(xbg_eac_vs_bc4(b) == -1);
        uint64_t v = (uint64_t(128) << 56) | (uint64_t(2) << 52) | (uint64_t(15) << 48);   // EAC: modifiers -9 -7 -5 -3 (x16) along x
        const int ex[4] = {3, 2, 1, 0};
        for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) v |= uint64_t(ex[x]) << (45 - 3 * (x * 4 + y));
        for (int i = 0; i < 8; i++) b[i] = uint8_t(v >> (56 - 8 * i));
        CHECK(xbg_eac_vs_bc4(b) == 1);
        const uint8_t flat[8] = {100, 100, 0, 0, 0, 0, 0, 0};
        CHECK(xbg_eac_vs_bc4(flat) == 0);
    }
    // ---- DM_GFX_ASTC_HDR_SELFTEST
    CHECK(xbg_astc_hdr_selftest_mode(nullptr) == XBG_AHS_AUTO && xbg_astc_hdr_selftest_mode("") == XBG_AHS_AUTO && xbg_astc_hdr_selftest_mode("auto") == XBG_AHS_AUTO && xbg_astc_hdr_selftest_mode("1") == XBG_AHS_AUTO);
    CHECK(xbg_astc_hdr_selftest_mode("0") == XBG_AHS_OFF && xbg_astc_hdr_selftest_mode("off") == XBG_AHS_OFF);
    CHECK(xbg_astc_hdr_selftest_mode("force") == XBG_AHS_FORCE && xbg_astc_hdr_selftest_mode("2") == XBG_AHS_FORCE && xbg_astc_hdr_selftest_mode("fail") == XBG_AHS_FAIL);
    CHECK(xbg_astc_hdr_selftest_mode("bogus") == XBG_AHS_AUTO);

    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("astc_hdr_test: all passed\n");
    return 0;
}
