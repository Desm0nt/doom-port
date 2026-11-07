// ASTC HDR overlay scan (docs/BUGS_AND_FIXES.md 2.37): reads the stream of tools/astc_hdr_selftest_gen.py stream on stdin and
// decodes every BC6H -> ASTC 4x4 HDR block again with the independent decoder dm/gfx/vulkan/engine/xbg_astc_ref_decode.h.
// Reports: error blocks, Inf / NaN / negative (sign bit) results, texels whose LNS -> FP16 conversion hits the spec's clamp at
// 0x7BFF (a decoder without the clamp returns Inf / NaN there), the largest values, endpoint modes / partitions / planes /
// void extents, bit differences against astcenc's decode, the error against the source BC6H, and the engine's error-block
// predicate (xbg_astc_hdr.h) against the decoder. --picks FILE writes the real blocks for the self-test vectors.
// Build (Windows LLVM or any C++17 compiler; the NDK sysroot gives vulkan/vulkan.h):
//   clang++ -O2 -std=c++17 -I<ndk>/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include -o astc_hdr_scan tools/astc_hdr_scan.cpp
//   python tools/astc_hdr_selftest_gen.py stream work/romfs work/astc_overlay_out --ldr | astc_hdr_scan --picks picks.bin
#include "../dm/gfx/vulkan/engine/xbg_astc_ref_decode.h"
#define XBG_ASTC_HDR_NO_DATA
#include "../dm/gfx/vulkan/engine/xbg_astc_hdr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static bool rd(void *p, size_t n) { return fread(p, 1, n, stdin) == n; }
static float h2f(uint16_t h) {
    const int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf(float(m | 1024), e - 25);
    return s ? -v : v;
}

struct Pick { std::string label; uint8_t blk[16]; };

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    const char *picks_path = nullptr;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--picks")) picks_path = argv[i + 1];
    char magic[4];
    if (!rd(magic, 4) || memcmp(magic, "AHS1", 4)) { fprintf(stderr, "bad stream\n"); return 2; }
    uint64_t textures = 0, subs = 0, blocks = 0, texels = 0, ldr_blocks = 0, ldr_err = 0, err = 0, nan_ref = 0, inf_ref = 0, neg_ref = 0;
    uint64_t clamped_texels = 0, clamped_blocks = 0, astcenc_diff_texels = 0, astcenc_diff_blocks = 0, voids = 0, void_hdr = 0, dual = 0;
    uint64_t pred_mismatch = 0, astcenc_nan = 0, src_nonfinite = 0, src_neg = 0;
    uint64_t parts[5] = {}, cem_hist[16] = {}, wq_hist[12] = {}, cq_hist[21] = {};
    int lns_max = -1; float vmax = 0, src_max = 0; double worst_ratio = 0; uint64_t big_ratio = 0;
    float vmin_pos = INFINITY;
    uint64_t over4 = 0, over4_bright = 0, under4 = 0; float worst_over = 0, worst_over_src = 0; uint64_t worst_over_tex = 0;
    std::map<std::string, Pick> picks;   // category -> first block
    std::map<std::string, std::pair<float, Pick>> extreme;
    std::vector<uint8_t> blk; std::vector<uint16_t> got, src;
    auto pick = [&](const std::string &k, const uint8_t *b) { if (!picks.count(k)) { Pick p; p.label = k; memcpy(p.blk, b, 16); picks[k] = p; } };
    auto pick_ext = [&](const std::string &k, float score, const uint8_t *b) {
        auto it = extreme.find(k);
        if (it == extreme.end() || score > it->second.first) { Pick p; p.label = k; memcpy(p.blk, b, 16); extreme[k] = {score, p}; } };
    for (;;) {
        uint32_t h[3];
        if (!rd(h, 12)) { fprintf(stderr, "truncated stream\n"); return 2; }
        if (h[0] == 0) break;
        const bool hdr = h[0] == 1;
        ++textures;
        for (uint32_t s = 0; s < h[2]; s++) {
            uint32_t g[3]; if (!rd(g, 12)) { fprintf(stderr, "truncated\n"); return 2; }
            const uint32_t W = g[0], H = g[1], nb = g[2], bw = W / 4;
            blk.resize(size_t(nb) * 16); if (!rd(blk.data(), blk.size())) return 2;
            if (!hdr) {
                for (uint32_t i = 0; i < nb; i++) {
                    uint16_t out[64]; AstcRefInfo info;
                    astcref::decode_block(&blk[i * 16], 4, 4, 0, out, &info);
                    ++ldr_blocks; if (info.error) ++ldr_err;
                    if ((xbg_astc_block_error(&blk[i * 16], 0) != 0) != (info.error != 0)) ++pred_mismatch;
                }
                continue;
            }
            ++subs;
            got.resize(size_t(W) * H * 4); src.resize(size_t(W) * H * 4);
            if (!rd(got.data(), got.size() * 2) || !rd(src.data(), src.size() * 2)) return 2;
            for (uint32_t i = 0; i < nb; i++) {
                const uint8_t *b = &blk[i * 16];
                uint16_t out[64]; AstcRefInfo info;
                astcref::decode_block(b, 4, 4, 1, out, &info);
                ++blocks;
                if ((xbg_astc_block_error(b, 1) != 0) != (info.error != 0)) { ++pred_mismatch; if (pred_mismatch <= 4) fprintf(stderr, "predicate mismatch (ref error=%d %s)\n", info.error, info.why ? info.why : ""); }
                if (info.error) { ++err; pick("error block", b); continue; }
                if (info.void_extent) { ++voids; void_hdr += info.void_hdr; pick(info.void_hdr ? "void extent hdr" : "void extent ldr", b); }
                else {
                    ++parts[info.partitions]; if (info.dual) { ++dual; pick("dual plane ccs " + std::to_string(info.ccs), b); }
                    for (int p = 0; p < info.partitions; p++) { ++cem_hist[info.cem[p]]; pick("cem " + std::to_string(info.cem[p]), b); }
                    ++wq_hist[info.weight_quant]; ++cq_hist[info.color_quant];
                    pick("partitions " + std::to_string(info.partitions), b);
                    pick("weight range index " + std::to_string(info.weight_quant), b);
                    pick("weight grid " + std::to_string(info.wx) + "x" + std::to_string(info.wy), b);
                }
                if (info.lns_clamped) { ++clamped_blocks; clamped_texels += info.lns_clamped; pick("lns clamped", b); }
                if (info.lns_max > lns_max) lns_max = info.lns_max;
                if (info.lns_max >= 0) pick_ext("largest lns", float(info.lns_max), b);
                const uint32_t bx = i % bw, by = i / bw;
                bool diff = false; float bmax = 0, bmin = INFINITY;
                for (int t = 0; t < 16; t++) {
                    const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                    const size_t at = (size_t(y) * W + x) * 4;
                    ++texels;
                    for (int c = 0; c < 4; c++) {
                        const uint16_t r = out[t * 4 + c];
                        const float f = h2f(r);
                        if (isnan(f)) ++nan_ref; else if (isinf(f)) ++inf_ref;
                        if (r & 0x8000) ++neg_ref;
                        if (r != got[at + c]) { ++astcenc_diff_texels; diff = true; if (astcenc_diff_texels <= 8) { fprintf(stderr, "astcenc differs: block %u texel %d ch %d ref %04x astcenc %04x cem %d/%d/%d parts %d dual %d void %d cq %d wq %d grid %dx%d block ", i, t, c, r, got[at + c], info.cem[0], info.cem[1], info.cem[2], info.partitions, info.dual, info.void_extent, info.color_quant, info.weight_quant, info.wx, info.wy); for (int q = 0; q < 16; q++) fprintf(stderr, "%02x", b[q]); fprintf(stderr, "\n"); } }
                        if (isnan(h2f(got[at + c])) || isinf(h2f(got[at + c]))) ++astcenc_nan;
                        if (c < 3) {
                            if (f > vmax) vmax = f;
                            if (f > 0 && f < vmin_pos) vmin_pos = f;
                            bmax = fmaxf(bmax, f); bmin = fminf(bmin, f);
                            const float sv = h2f(src[at + c]);
                            if (!isfinite(sv)) ++src_nonfinite;
                            if (src[at + c] & 0x8000) ++src_neg;
                            if (sv > src_max) src_max = sv;
                            const double ratio = fabs(log2((double(f) + 1e-3) / (double(sv) + 1e-3)));
                            if (ratio > worst_ratio) worst_ratio = ratio;
                            if (ratio > 1.0) ++big_ratio;
                            // brightening outliers: the substitute much brighter than the source in an already visible texel
                            if (f > 4.0f * sv + 0.05f) { ++over4; if (f - sv > worst_over) { worst_over = f - sv; worst_over_src = sv; worst_over_tex = textures - 1; } if (f > 1.0f) ++over4_bright; }
                            if (sv > 0.05f && f < 0.25f * sv) ++under4;
                        }
                    }
                }
                if (diff) ++astcenc_diff_blocks;
                pick_ext("brightest texel", bmax, b);
                if (bmax > 0) pick_ext("darkest non-zero block", -bmax, b);
                pick_ext("widest range in a block", bmax / fmaxf(bmin, 1e-4f), b);
            }
        }
    }
    printf("textures %llu (bc6h subresources %llu), ASTC HDR blocks %llu, texels %llu\n", (unsigned long long)textures, (unsigned long long)subs, (unsigned long long)blocks, (unsigned long long)texels);
    printf("error blocks %llu; reference decode: NaN %llu, Inf %llu, sign bit set %llu (channel values)\n", (unsigned long long)err, (unsigned long long)nan_ref, (unsigned long long)inf_ref, (unsigned long long)neg_ref);
    printf("LNS clamp to 0x7BFF: %llu channel values in %llu blocks; largest interpolated 16-bit LNS %#x (0xF800+, exponent 31, would be Inf/NaN without the clamp)\n",
           (unsigned long long)clamped_texels, (unsigned long long)clamped_blocks, lns_max);
    printf("largest decoded value %g (source BC6H %g), smallest positive %g\n", vmax, src_max, vmin_pos);
    printf("astcenc vs reference: %llu channel values differ in %llu blocks; astcenc Inf/NaN %llu\n", (unsigned long long)astcenc_diff_texels, (unsigned long long)astcenc_diff_blocks, (unsigned long long)astcenc_nan);
    printf("against the source BC6H: worst |log2| %.3f stops, values off by more than 1 stop %llu; source non-finite %llu, source sign bit %llu\n", worst_ratio, (unsigned long long)big_ratio, (unsigned long long)src_nonfinite, (unsigned long long)src_neg);
    printf("brighter than 4x the source (+0.05): %llu channel values (%llu above 1.0), worst +%g over a source of %g (texture %llu); darker than 1/4 of a source > 0.05: %llu\n", (unsigned long long)over4, (unsigned long long)over4_bright, worst_over, worst_over_src, (unsigned long long)worst_over_tex, (unsigned long long)under4);
    printf("void extents %llu (HDR %llu), dual plane %llu, partitions 1/2/3/4: %llu %llu %llu %llu\n", (unsigned long long)voids, (unsigned long long)void_hdr, (unsigned long long)dual,
           (unsigned long long)parts[1], (unsigned long long)parts[2], (unsigned long long)parts[3], (unsigned long long)parts[4]);
    printf("endpoint modes:"); for (int i = 0; i < 16; i++) if (cem_hist[i]) printf(" %d:%llu", i, (unsigned long long)cem_hist[i]); printf("\n");
    printf("weight range index:"); for (int i = 0; i < 12; i++) if (wq_hist[i]) printf(" %d:%llu", i, (unsigned long long)wq_hist[i]); printf("\n");
    printf("colour range index:"); for (int i = 0; i < 21; i++) if (cq_hist[i]) printf(" %d:%llu", i, (unsigned long long)cq_hist[i]); printf("\n");
    printf("ASTC LDR blocks (bc7 / bc3 substitutes) %llu, error blocks %llu\n", (unsigned long long)ldr_blocks, (unsigned long long)ldr_err);
    printf("engine error-block predicate disagrees with the reference decoder: %llu blocks\n", (unsigned long long)pred_mismatch);
    if (picks_path) {
        std::vector<Pick> all;
        for (auto &p : picks) if (p.first != "error block") all.push_back(p.second);
        for (auto &p : extreme) all.push_back(p.second.second);
        FILE *f = fopen(picks_path, "wb");
        if (!f) { perror(picks_path); return 2; }
        const uint32_t n = uint32_t(all.size());
        fwrite("AHP1", 1, 4, f); fwrite(&n, 4, 1, f);
        for (auto &p : all) { const uint8_t l = uint8_t(p.label.size()); fwrite(&l, 1, 1, f); fwrite(p.label.data(), 1, l, f); fwrite(p.blk, 1, 16, f); }
        fclose(f);
        printf("picks: %u blocks -> %s\n", n, picks_path);
    }
    return 0;
}
