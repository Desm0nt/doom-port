// [nonadreno] ASTC / ETC2 / EAC texture overlay ("variant A", tools/astc_overlay_doom.py; docs/BUGS_AND_FIXES.md 2.26).
// The PC setup re-encodes the BC blocks of the game's file textures into formats of the same block size (BC3 / BC7 -> ASTC 4x4,
// BC6H -> ASTC 4x4 HDR, BC1 -> ETC2 RGB8, BC4 -> EAC R11, BC5 -> EAC RG11) and writes them as an overlay of the RomFS
// (Download/DOOM Mobile/astc/{data,nohdr}/base/gameresources_003.{patch,pindex} + manifest.json). The game loads, swizzles and
// copies those blocks into its atlases and the light-probe array exactly as before; the engine only has to create the images
// in the substitute VkFormat and upload the blocks as they are. Pure functions (no device calls) so that
// engine/tests/astc_overlay_test.cpp runs the production code; the boot choice is in android/vulkan_present.cpp, the per-image
// decision in xbg_game.cpp bc_store().
//
// Which textures are substituted: every texture of a substituted NVN BC format, except the shapes xbg_astc_keep_bc names. Those
// are the virtual-texture physical caches, which the game's own transcoder fills with real BC blocks at run time (BC3 sRGB
// 4096x4096x3 array, BC6H 8192x4096, both single-level), and anything shaped like them; they stay on the GPU BC decoder.
// tools/astc_overlay_doom.py keep_bc() mirrors the rule, so their file data (none in this game) is left as BC too. A wrong guess
// can only give a wrong-looking texture: the image is created and uploaded either way, both paths take any bytes.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

enum { XBG_AO_BC1 = 1u << 0, XBG_AO_BC3 = 1u << 1, XBG_AO_BC4 = 1u << 2, XBG_AO_BC5 = 1u << 3, XBG_AO_BC6H = 1u << 4, XBG_AO_BC7 = 1u << 5 };
static const char *const xbg_ao_names[6] = {"bc1", "bc3", "bc4", "bc5", "bc6h", "bc7"};

// "bc1 bc3 bc4 ..." (space or comma separated) -> mask; unknown words are ignored.
static inline uint32_t xbg_astc_formats_mask(const char *list) {
    uint32_t m = 0;
    for (const char *p = list ? list : ""; *p;) {
        while (*p == ' ' || *p == ',') p++;
        size_t n = strcspn(p, " ,");
        for (unsigned i = 0; i < 6; i++) if (n == strlen(xbg_ao_names[i]) && !strncmp(p, xbg_ao_names[i], n)) m |= 1u << i;
        p += n;
    }
    return m;
}
static inline void xbg_astc_formats_str(uint32_t mask, char *out, size_t n) {
    size_t at = 0; if (n) out[0] = 0;
    for (unsigned i = 0; i < 6; i++) if (mask & (1u << i)) {
        int w = snprintf(out + at, at < n ? n - at : 0, "%s%s", at ? " " : "", xbg_ao_names[i]);
        if (w > 0) at += size_t(w);
    }
    if (!at && n) snprintf(out, n, "none");
}

// The overlay format bit of an NVN BC format, 0 for formats the overlay never holds (BC2; signed BC4 / BC5 / BC6H: the file
// textures are unsigned, a signed texture is not one of them).
static inline uint32_t xbg_astc_format_bit(uint32_t nvn) {
    switch (nvn) {
    case 0x41: case 0x42: case 0x45: case 0x46: return XBG_AO_BC1;   // DXT1 RGB / RGBA, unorm / sRGB (the game uses 0x42)
    case 0x44: case 0x48: return XBG_AO_BC3;                         // DXT5 unorm / sRGB
    case 0x49: return XBG_AO_BC4;                                    // RGTC1 unsigned
    case 0x4b: return XBG_AO_BC5;                                    // RGTC2 unsigned
    case 0x50: return XBG_AO_BC6H;                                   // BPTC unsigned float
    case 0x4d: case 0x4e: return XBG_AO_BC7;                         // BPTC unorm / sRGB
    default: return 0;
    }
}
// The substitute VkFormat of an NVN BC format (same block size, same sRGB-ness). BC1 RGBA maps to ETC2 RGB8: the game's BC1
// data has no punch-through alpha (no 3-colour block uses index 3: tools/astc_overlay_doom.py counts them), so alpha is 1 either way.
static inline VkFormat xbg_astc_substitute_format(uint32_t nvn) {
    switch (nvn) {
    case 0x41: case 0x42: return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    case 0x45: case 0x46: return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
    case 0x44: case 0x4d: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case 0x48: case 0x4e: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_EAC_R11_UNORM_BLOCK;
    case 0x4b: return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    case 0x50: return VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}
// Shapes that stay real BC even when their format is substituted (NULL: substitute). NVN targets: 0 1D, 1 2D, 2 3D, 3 1D array,
// 4 2D array, 8 cube, 9 cube array. Mirror: tools/astc_overlay_doom.py keep_bc (file textures are 2D or cube, never single-level
// with both sides >= 4096, every BC6H file texture is a cube map).
static inline const char *xbg_astc_keep_bc(uint32_t nvn, int target, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers) {
    const uint32_t bit = xbg_astc_format_bit(nvn);
    if (target == 0 || target == 2 || target == 3) return "not-2d";
    if (levels == 1 && w >= 4096 && h >= 4096) return "vt-cache-shape";       // 103 (BC3 4096x4096x3), 1880 (BC6H 8192x4096)
    if (bit == XBG_AO_BC6H && target != 8 && target != 9) return "bc6h-not-cube";
    if (bit == XBG_AO_BC3 && (target == 4 || layers > 1)) return "bc3-array";
    return NULL;
}
// The VkFormat to create an NVN texture in when the overlay substitutes `mask`: the substitute, or VK_FORMAT_UNDEFINED (*why says
// why not: "format" = not substituted, else xbg_astc_keep_bc's reason).
static inline VkFormat xbg_astc_texture_format(uint32_t mask, uint32_t nvn, int target, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers,
                                               const char **why) {
    const uint32_t bit = xbg_astc_format_bit(nvn);
    if (why) *why = "format";
    if (!bit || !(mask & bit)) return VK_FORMAT_UNDEFINED;
    const char *keep = xbg_astc_keep_bc(nvn, target, w, h, levels, layers);
    if (keep) { if (why) *why = keep; return VK_FORMAT_UNDEFINED; }
    if (why) *why = NULL;
    return xbg_astc_substitute_format(nvn);
}

// ---------------------------------------------------------------- manifest.json (written by the tool, read at boot)
// Only the flat engine_* keys and format / version are read: plain strings without escapes.
enum { XBG_AO_MAX_FILES = 8 };
struct XbgAstcManifest {
    int version;
    uint32_t data_mask, nohdr_mask;   // what the variants data/ (with BC6H) and nohdr/ substitute
    int nfiles, nsources;
    char file[XBG_AO_MAX_FILES][128]; uint64_t file_size[XBG_AO_MAX_FILES];       // overlay files relative to the overlay folder
    char source[XBG_AO_MAX_FILES][128]; uint64_t source_size[XBG_AO_MAX_FILES];   // RomFS files the overlay was made from
};
// The string value of "key" (first occurrence) into out; 0 when absent.
static inline int xbg_astc_json_string(const char *text, const char *key, char *out, size_t n) {
    char pat[96]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p || !n) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '"') {
        p++; size_t k = 0;
        while (*p && *p != '"' && k + 1 < n) out[k++] = *p++;
        out[k] = 0; return *p == '"';
    }
    size_t k = 0;                                   // a bare number
    while (*p && (*p == '-' || (*p >= '0' && *p <= '9')) && k + 1 < n) out[k++] = *p++;
    out[k] = 0; return k > 0;
}
// "path=size path=size ..." -> arrays; the number of pairs, -1 on a malformed list.
static inline int xbg_astc_path_sizes(const char *list, char (*path)[128], uint64_t *size, int max) {
    int n = 0;
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        size_t len = strcspn(p, " ");
        const char *eq = (const char *)memchr(p, '=', len);
        if (!eq || eq == p || n >= max || size_t(eq - p) >= 128) return -1;
        memcpy(path[n], p, size_t(eq - p)); path[n][eq - p] = 0;
        char *end = NULL; unsigned long long v = strtoull(eq + 1, &end, 10);
        if (end != p + len || end == eq + 1) return -1;
        if (strstr(path[n], "..") || path[n][0] == '/') return -1;   // stays inside the overlay / RomFS folder
        size[n++] = v; p += len;
    }
    return n;
}
// 0 = usable, else -1 with *why.
static inline int xbg_astc_manifest_parse(const char *text, XbgAstcManifest *m, char *why, size_t why_n) {
    memset(m, 0, sizeof *m);
    char v[1024];
    if (!xbg_astc_json_string(text, "format", v, sizeof v) || strcmp(v, "doom-astc-overlay")) { snprintf(why, why_n, "format is not doom-astc-overlay (%s)", v); return -1; }
    if (!xbg_astc_json_string(text, "version", v, sizeof v) || (m->version = atoi(v)) != 1) { snprintf(why, why_n, "unsupported version %s", v); return -1; }
    if (!xbg_astc_json_string(text, "engine_formats_data", v, sizeof v)) { snprintf(why, why_n, "no engine_formats_data"); return -1; }
    m->data_mask = xbg_astc_formats_mask(v);
    if (!xbg_astc_json_string(text, "engine_formats_nohdr", v, sizeof v)) { snprintf(why, why_n, "no engine_formats_nohdr"); return -1; }
    m->nohdr_mask = xbg_astc_formats_mask(v);
    if ((m->nohdr_mask & XBG_AO_BC6H) || (m->nohdr_mask & ~m->data_mask)) { snprintf(why, why_n, "inconsistent variants"); return -1; }
    if (!xbg_astc_json_string(text, "engine_files", v, sizeof v) || (m->nfiles = xbg_astc_path_sizes(v, m->file, m->file_size, XBG_AO_MAX_FILES)) <= 0) {
        snprintf(why, why_n, "bad engine_files"); return -1; }
    if (!xbg_astc_json_string(text, "engine_source", v, sizeof v) || (m->nsources = xbg_astc_path_sizes(v, m->source, m->source_size, XBG_AO_MAX_FILES)) <= 0) {
        snprintf(why, why_n, "bad engine_source"); return -1; }
    return 0;
}

// ---------------------------------------------------------------- the boot choice
// mode: DM_GFX_ASTC_OVERLAY, -1 auto (unset / "auto"), 0 off, 1 on (also where the device samples BC, e.g. Adreno, for tests).
static inline int xbg_astc_mode(const char *e) {
    if (!e || !*e || !strcmp(e, "auto")) return -1;
    return !strcmp(e, "0") ? 0 : !strcmp(e, "1") ? 1 : -1;
}
struct XbgAstcChoice { int use, hdr; uint32_t mask; const char *variant; const char *why; };
// bc / astc_ldr / etc2: device features; astc_hdr: VK_EXT_texture_compression_astc_hdr's feature enabled on the device.
// The overlay is all or nothing for ASTC LDR and ETC2 (every variant needs both: Android Vulkan devices report both);
// without ASTC HDR the nohdr variant keeps BC6H as BC (GPU decode).
static inline XbgAstcChoice xbg_astc_choose(int mode, int bc, int astc_ldr, int etc2, int astc_hdr, const XbgAstcManifest *m) {
    XbgAstcChoice c = {0, 0, 0, "", ""};
    if (mode == 0) { c.why = "DM_GFX_ASTC_OVERLAY=0"; return c; }
    if (!m) { c.why = "no usable overlay"; return c; }
    if (mode < 0 && bc) { c.why = "the device samples BC (auto)"; return c; }
    if (!astc_ldr || !etc2) { c.why = !astc_ldr ? "no textureCompressionASTC_LDR" : "no textureCompressionETC2"; return c; }
    c.use = 1; c.hdr = astc_hdr && (m->data_mask & XBG_AO_BC6H);
    c.mask = c.hdr ? m->data_mask : m->nohdr_mask; c.variant = c.hdr ? "data" : "nohdr";
    c.why = mode > 0 ? (bc ? "forced (DM_GFX_ASTC_OVERLAY=1) on a device with BC" : "forced (DM_GFX_ASTC_OVERLAY=1)") : "no BC on this device (auto)";
    return c;
}
