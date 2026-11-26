// [turnip-ubwc-hint] Host test of vk_ubwc_hint.h (port-common/modules/turnip-ubwc-hint, PNH-B-11): the 740 model match, the system
// driver's version scan (marker inside a chunk, across a chunk border, absent, short file), and the FD_DEV_FEATURES composition
// (empty, appended with ':', a value that names the feature is kept, a too small buffer).
// Build (WSL or any host): gcc -std=c11 -Wall -Wextra -fsanitize=address,undefined -o ubwc_hint_test ubwc_hint_test.c && ./ubwc_hint_test <scratch dir>
#include "../vk_ubwc_hint.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

static int scan_with(const char *dir, const char *name, size_t pad, const char *marker) {   // pad bytes of 'x', then marker, then junk
    char path[512]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    for (size_t i = 0; i < pad; i++) fputc(i % 7 ? 'x' : 'V', f);   // stray 'V' without '@' must not match
    if (marker) fputs(marker, f);
    fputs("trailing", f); fclose(f);
    const int v = dm_ubwc_system_version(path); remove(path); return v;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    CHECK(dm_ubwc_model_740("Adreno740v2")); CHECK(dm_ubwc_model_740("Adreno740"));
    CHECK(!dm_ubwc_model_740("Adreno750v2")); CHECK(!dm_ubwc_model_740("Adreno7400")); CHECK(!dm_ubwc_model_740("Adreno74"));
    CHECK(!dm_ubwc_model_740("(unreadable)")); CHECK(!dm_ubwc_model_740(NULL)); CHECK(!dm_ubwc_model_740(""));
    { char m[32] = "junk"; dm_ubwc_model(m, sizeof m); CHECK(strlen(m) < sizeof m); dm_ubwc_model(NULL, 4); dm_ubwc_model(m, 0); }   // a host has no kgsl node: ""

    CHECK(scan_with(dir, "u1.bin", 100, "OpenGL ES V@0863.0 (GIT@abc)") == 863);
    CHECK(scan_with(dir, "u2.bin", 100, "V@0615.65") == 615);
    CHECK(scan_with(dir, "u3.bin", (1 << 16) - 3, "V@0762.10") == 762);   // the marker crosses the first chunk's end
    CHECK(scan_with(dir, "u4.bin", (1 << 16) * 2 + 5, "V@0744.0") == 744);
    CHECK(scan_with(dir, "u5.bin", 1000, "V@12") == 0);                     // too few digits
    CHECK(scan_with(dir, "u6.bin", 1000, NULL) == 0);
    CHECK(scan_with(dir, "u7.bin", 0, "V@0700") == 700);
    CHECK(dm_ubwc_system_version("/nonexistent/libGLESv2_adreno.so") == 0); CHECK(dm_ubwc_system_version(NULL) == 0);

    char out[128];
    CHECK(dm_ubwc_compose(out, sizeof out, NULL, 1) == 1 && !strcmp(out, "enable_tp_ubwc_flag_hint=1"));
    CHECK(dm_ubwc_compose(out, sizeof out, "", 0) == 1 && !strcmp(out, "enable_tp_ubwc_flag_hint=0"));
    CHECK(dm_ubwc_compose(out, sizeof out, "has_early_preamble=0", 1) == 1 && !strcmp(out, "has_early_preamble=0:enable_tp_ubwc_flag_hint=1"));
    CHECK(dm_ubwc_compose(out, sizeof out, "a=1:", 1) == 1 && !strcmp(out, "a=1:enable_tp_ubwc_flag_hint=1"));
    CHECK(dm_ubwc_compose(out, sizeof out, "enable_tp_ubwc_flag_hint=0", 1) == 0);           // the player's own value wins
    CHECK(dm_ubwc_compose(out, sizeof out, "a=1:enable_tp_ubwc_flag_hint=1:b=2", 0) == 0);
    CHECK(dm_ubwc_compose(out, sizeof out, "enable_tp_ubwc_flag_hint_x=1", 1) == 1);          // another name with the same prefix
    CHECK(dm_ubwc_compose(out, 10, "a=1", 1) == -1);
    CHECK(dm_ubwc_named("x=1:enable_tp_ubwc_flag_hint")); CHECK(!dm_ubwc_named(NULL)); CHECK(!dm_ubwc_named("xenable_tp_ubwc_flag_hint=1"));

    if (failures) { printf("ubwc_hint_test: %d failures\n", failures); return 1; }
    printf("ubwc_hint_test: all checks passed\n"); return 0;
}
