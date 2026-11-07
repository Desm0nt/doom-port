// LDR substitutes of the DOOM ASTC overlay against their sources (docs/BUGS_AND_FIXES.md 2.37): reads
// tools/astc_hdr_selftest_gen.py ldrstream on stdin and decodes every substitute block with independent spec decoders:
//   BC3 / BC7 -> ASTC 4x4 LDR   dm/gfx/vulkan/engine/xbg_astc_ref_decode.h (LDR profile), against the source decoded by
//                               texture2ddecoder (streamed)
//   BC4 -> EAC R11, BC5 -> EAC RG11   the engine's EAC decoder xbg_eac_r11_block (xbg_astc_hdr.h) against the BC4 decoder below
// Per format: PSNR, the largest error, per channel mean signed error (a constant offset or a lost / moved channel shows here), and
// the worst textures by name. Neither astcenc nor etcpak nor texture2ddecoder's ASTC / ETC decoders are used.
// Build: clang++ -O2 -std=c++17 -o astc_ldr_scan tools/astc_ldr_scan.cpp
//   python tools/astc_hdr_selftest_gen.py ldrstream work/romfs work/astc_overlay_out | astc_ldr_scan
#include "../dm/gfx/vulkan/engine/xbg_astc_ref_decode.h"
#define XBG_ASTC_HDR_NO_DATA
#include "../dm/gfx/vulkan/engine/xbg_astc_hdr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static bool rd(void *p, size_t n) { return fread(p, 1, n, stdin) == n; }

// BC4 unsigned: 16 values 0..255 (float: the 6- / 8-value palette is not rounded on hardware either)
static void bc4_decode(const uint8_t *b, float out[16]) {
    const float r0 = b[0], r1 = b[1];
    float pal[8];
    pal[0] = r0; pal[1] = r1;
    if (b[0] > b[1]) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * r0 + i * r1) / 7.0f;
    else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * r0 + i * r1) / 5.0f; pal[6] = 0; pal[7] = 255; }
    uint64_t idx = 0; for (int i = 0; i < 6; i++) idx |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = pal[(idx >> (3 * i)) & 7];
}
// EAC R11 unsigned: the engine's decoder (xbg_astc_hdr.h, used by DM_GFX_ASTC_CPU), 16 values scaled to 0..255, row-major
static void eac_r11_decode(const uint8_t *b, float out[16]) { uint16_t v[16]; xbg_eac_r11_block(b, v); for (int i = 0; i < 16; i++) out[i] = v[i] * 255.0f / 2047.0f; }

struct Stat {
    double se = 0, n = 0, mean[4] = {0, 0, 0, 0}, cnt[4] = {0, 0, 0, 0}; float maxerr = 0; uint64_t big = 0;
    void add(int ch, float src, float got) { const float e = got - src; se += double(e) * e; n += 1; mean[ch] += e; cnt[ch] += 1; if (fabsf(e) > maxerr) maxerr = fabsf(e); if (fabsf(e) > 64) ++big; }
    double psnr() const { return n && se ? 10 * log10(255.0 * 255.0 / (se / n)) : 99; }
};

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    int show = 12;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--show")) show = atoi(argv[i + 1]);
    char magic[4];
    if (!rd(magic, 4) || memcmp(magic, "AHL1", 4)) { fprintf(stderr, "bad stream\n"); return 2; }
    static const char *names[5] = {"?", "bc3->astc", "bc4->eac_r11", "bc5->eac_rg11", "bc7->astc"};
    Stat fmt[5]; uint64_t textures[5] = {}, errblocks[5] = {};
    uint64_t disc[2][3] = {};   // [substitute EAC / source BC4][bc4-like, undecided, eac-like]
    struct Tex { double psnr; float maxerr; int code; std::string name; double mean[4]; };
    std::vector<Tex> worst;
    std::vector<uint8_t> src, sub, rgba;
    for (;;) {
        uint32_t h[3]; if (!rd(h, 12)) { fprintf(stderr, "truncated\n"); return 2; }
        if (h[0] == 0) break;
        const int code = int(h[0]);
        uint8_t ln; rd(&ln, 1); std::string name(ln, ' '); rd(&name[0], ln);
        uint32_t shape[4]; rd(shape, 16);
        Stat t;
        for (uint32_t s = 0; s < h[2]; s++) {
            uint32_t g[4]; if (!rd(g, 16)) return 2;
            const uint32_t W = g[0], nb = g[2], sz = g[3], bw = W / 4, bpb = sz / nb;
            src.resize(sz); sub.resize(sz); rd(src.data(), sz); rd(sub.data(), sz);
            if (code == 1 || code == 4) { rgba.resize(size_t(W) * g[1] * 4); rd(rgba.data(), rgba.size()); }
            for (uint32_t i = 0; i < nb; i++) {
                const uint32_t bx = i % bw, by = i / bw;
                if (code == 1 || code == 4) {
                    uint16_t px[64]; AstcRefInfo info; astcref::decode_block(&sub[i * 16], 4, 4, 0, px, &info);
                    if (info.error) ++errblocks[code];
                    for (int tx = 0; tx < 16; tx++) {
                        const size_t at = ((size_t(by) * 4 + tx / 4) * W + bx * 4 + tx % 4) * 4;
                        for (int c = 0; c < 4; c++) { const float got = px[tx * 4 + c] * 255.0f / 65535.0f; t.add(c, rgba[at + c], got); fmt[code].add(c, rgba[at + c], got); }
                    }
                } else {
                    const int chans = code == 2 ? 1 : 2;
                    for (int c = 0; c < chans; c++) {   // the EAC census discriminator on both the substitute and its source
                        const int vs = xbg_eac_vs_bc4(&sub[i * bpb + 8 * c]), vb = xbg_eac_vs_bc4(&src[i * bpb + 8 * c]);
                        ++disc[0][vs + 1]; ++disc[1][vb + 1];
                    }
                    for (int c = 0; c < chans; c++) {
                        float a[16], b[16];
                        bc4_decode(&src[i * bpb + 8 * c], a); eac_r11_decode(&sub[i * bpb + 8 * c], b);
                        for (int k = 0; k < 16; k++) { t.add(c, a[k], b[k]); fmt[code].add(c, a[k], b[k]); }
                    }
                }
            }
        }
        ++textures[code];
        Tex x{t.psnr(), t.maxerr, code, name, {0, 0, 0, 0}};
        for (int c = 0; c < 4; c++) x.mean[c] = t.cnt[c] ? t.mean[c] / t.cnt[c] : 0;
        worst.push_back(x);
    }
    for (int c = 1; c < 5; c++) {
        if (!textures[c]) continue;
        const Stat &s = fmt[c];
        printf("%-14s textures %5llu  PSNR %6.2f dB  max error %5.1f  values off by >64: %llu  mean signed error R %+.3f G %+.3f B %+.3f A %+.3f  ASTC error blocks %llu\n",
               names[c], (unsigned long long)textures[c], s.psnr(), s.maxerr, (unsigned long long)s.big,
               s.cnt[0] ? s.mean[0] / s.cnt[0] : 0, s.cnt[1] ? s.mean[1] / s.cnt[1] : 0, s.cnt[2] ? s.mean[2] / s.cnt[2] : 0, s.cnt[3] ? s.mean[3] / s.cnt[3] : 0,
               (unsigned long long)errblocks[c]);
    }
    if (disc[0][0] + disc[0][1] + disc[0][2])
        printf("EAC census discriminator (xbg_eac_vs_bc4): EAC substitute halves: eac-like %llu, bc4-like %llu, undecided %llu; their BC4 source halves: eac-like %llu, bc4-like %llu, undecided %llu\n",
               (unsigned long long)disc[0][2], (unsigned long long)disc[0][0], (unsigned long long)disc[0][1], (unsigned long long)disc[1][2], (unsigned long long)disc[1][0], (unsigned long long)disc[1][1]);
    std::sort(worst.begin(), worst.end(), [](const Tex &a, const Tex &b) { return a.psnr < b.psnr; });
    printf("worst textures:\n");
    for (int i = 0; i < show && i < int(worst.size()); i++)
        printf("  %-14s %6.2f dB max %5.1f mean %+.2f %+.2f %+.2f %+.2f  %s\n", names[worst[i].code], worst[i].psnr, worst[i].maxerr,
               worst[i].mean[0], worst[i].mean[1], worst[i].mean[2], worst[i].mean[3], worst[i].name.c_str());
    return 0;
}
