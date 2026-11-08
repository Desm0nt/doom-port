# Host unit test of dm/host/hle_doom_settings.c (BUGS_AND_FIXES 2.40, issue #31): the gear <-> game controller settings bridge.
# The module is compiled for the host with a stub hle.h, a fake "main" image (GOT slots -> fake idCVar objects with the update's
# layout, the setters' instruction words at their addresses so fns_ok passes) and the game's setters replaced by C stand-ins that
# behave like SetBool / SetFloat (value string + int + float). Runs two "launches" against one mirror file:
#   A: fresh profile -> save gate forced open, menu change -> mirror dirty + one debounced save request, gear changes, a profile save
#      cleans the mirror, a last gear change with no profile save after it stays dirty;
#   B: next launch, the profile still has the old Aim Assist -> restored from the dirty mirror; clean values are not touched.
# Also: slider <-> joy_sixAxisGlobalScale mapping for 0..100, the truth read from the value string (a touch-look gate on the int field
# does not show). Usage: python tools/test_doom_settings.py   (needs gcc, or WSL's gcc on Windows)
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src', 'dm', 'host', 'hle_doom_settings.c')

STUB = r'''
#define DM_GAME_DOOM 1
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef struct { char name[16]; uint8_t *base; size_t size; } module_t;
module_t g_mod[4]; int g_nmod;
#define LOG(...) (printf("  log: " __VA_ARGS__), printf("\n"))
static void fake_set_aim(void *self, int v);
static void fake_set_gs(void *self, float v);
static void fake_set_bool(void *cv, int v, int force);
'''

TEST = r'''
#include <assert.h>
#include <sys/mman.h>
typedef struct { uint8_t b[0x60]; char str[32]; char name[48]; } fake_cvar;
static fake_cvar cv_aim, cv_gs, cv_req;
static uint8_t profile[0x4800];
static int save_requests;
uint8_t *doom_profile(void) { return profile; }
void doom_profile_save_request(void) { save_requests++; }
static void cv_init(fake_cvar *c, uint32_t got, const char *name, const char *val) {
    snprintf(c->name, sizeof c->name, "%s", name); snprintf(c->str, sizeof c->str, "%s", val);
    *(char **)(c->b + 0x50) = c->name; *(char **)(c->b + 0x10) = c->str;
    *(int32_t *)(c->b + 0x30) = atoi(val); *(float *)(c->b + 0x34) = (float)atof(val);
    *(uint64_t *)(g_mod[0].base + got) = (uint64_t)(uintptr_t)c->b;
}
static void cv_set(fake_cvar *c, const char *s) { snprintf(c->str, sizeof c->str, "%s", s); *(int32_t *)(c->b + 0x30) = atoi(s); *(float *)(c->b + 0x34) = (float)atof(s); }
static fake_cvar *of(void *b) { return b == cv_aim.b ? &cv_aim : b == cv_gs.b ? &cv_gs : &cv_req; }
static void fake_set_aim(void *self, int v) { (void)self; cv_set(&cv_aim, v & 1 ? "1" : "0"); }
static void fake_set_gs(void *self, float v) { (void)self; char s[32]; snprintf(s, sizeof s, "%f", v); cv_set(&cv_gs, s); }
static void fake_set_bool(void *cv, int v, int force) { (void)force; cv_set(of(cv), v & 1 ? "1" : "0"); }
static void words(uint32_t off, const uint32_t *w, int n) { memcpy(g_mod[0].base + off, w, 4 * n); }
static void frames(int n, int player) { for (int i = 0; i < n; i++) doom_settings_frame(player, 0); }

int main(int argc, char **argv) {
    int phase = argc > 1 ? argv[1][0] : 'A';
    size_t sz = 0x48da000;
    g_mod[0].base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); g_mod[0].size = sz; strcpy(g_mod[0].name, "main"); g_nmod = 1;
    static const uint32_t aim[5] = {0xb001bd20u, 0xf945bc00u, 0x12000021u, 0x320003e2u, 0x17c0e244u};
    static const uint32_t gs[4] = {0xb001bd40u, 0xf9441400u, 0x320003e1u, 0x17c0ce95u};
    static const uint32_t setb[5] = {0xd10143ffu, 0xa9034ff4u, 0xa9047bfdu, 0x910103fdu, 0xaa0003f4u};
    words(FN_SET_AIM_TOGGLE, aim, 5); words(FN_SET_GLOBAL_SCALE, gs, 4); words(FN_CVAR_SET_BOOL, setb, 5);
    if (phase == 'A') {
        // mapping: the game's slider <-> GlobalScale
        for (int n = 0; n <= 100; n++) assert(slider_of(scale_of(n)) == n);
        assert(slider_of(25.799999f) == 43 && slider_of(30.0f) == 50 && slider_of(60.0f) == 100 && slider_of(0.0f) == 0);
        cv_init(&cv_aim, GOT_AIM_TOGGLE, "joy_sixAxisAimToggle", "0"); cv_init(&cv_gs, GOT_GLOBAL_SCALE, "joy_sixAxisGlobalScale", "30");
        cv_init(&cv_req, GOT_REQ_EXPLAIN, "profile_requireSaveExplanation", "1");
        profile[PROF_AIM_ASSIST] = 1; profile[PROF_SEEN_EXPLAIN] = 0;
        assert(dm_doom_game_setting_get(0) == -1 && dm_doom_game_setting_set(0, 1) == 0);   // before the profile load: not known
        frames(3, 0);
        doom_settings_frame(1, 1);                                                        // profile loaded
        assert(*(int32_t *)(cv_req.b + 0x30) == 0);                                       // save gate opened
        assert(dm_doom_game_setting_get(0) == 0 && dm_doom_game_setting_get(1) == 50 && dm_doom_game_setting_get(2) == 1);
        // touch look opens the AimToggle gate on the int field only: the truth stays the string
        *(int32_t *)(cv_aim.b + 0x30) = 1; frames(1, 1); assert(dm_doom_game_setting_get(0) == 0); *(int32_t *)(cv_aim.b + 0x30) = 0;
        // the player turns Motion Aiming on in the game menu: mirror dirty, one save request ~90 frames later
        fake_set_aim(NULL, 1); int r0 = save_requests; frames(1, 1);
        assert(dm_doom_game_setting_get(0) == 1 && save_requests == r0);
        frames(95, 1); assert(save_requests == r0 + 1);
        // gear: sensitivity 43 -> GlobalScale 25.8 through the setter, a save request at once
        assert(dm_doom_game_setting_set(1, 43) == 1); assert(dm_doom_game_setting_get(1) == 43);
        frames(1, 1); assert(slider_of(cvar_value(cv_gs.b, 0)) == 43 && save_requests == r0 + 2);
        assert(fabsf(*(float *)(cv_gs.b + 0x34) - 25.8f) < 1e-4f);
        // a profile save completes: mirror clean
        doom_settings_profile_state(0, 1); frames(1, 1); doom_settings_profile_state(1, 0); frames(1, 1);
        assert(!g_m[GS_AIM].dirty && !g_m[GS_SENS].dirty);
        // gear: Aim Assist off, no profile save after it (the game is killed): stays dirty
        assert(dm_doom_game_setting_set(2, 0) == 1); frames(1, 1);
        assert(profile[PROF_AIM_ASSIST] == 0 && g_m[GS_AA].dirty && g_m[GS_AA].val == 0);
        printf("phase A ok\n");
    } else {
        // next launch: the profile has what was saved (aim 1, sens 43) and the OLD Aim Assist 1 (its change was not saved)
        cv_init(&cv_aim, GOT_AIM_TOGGLE, "joy_sixAxisAimToggle", "1"); cv_init(&cv_gs, GOT_GLOBAL_SCALE, "joy_sixAxisGlobalScale", "25.799999");
        cv_init(&cv_req, GOT_REQ_EXPLAIN, "profile_requireSaveExplanation", "0");
        profile[PROF_AIM_ASSIST] = 1; profile[PROF_SEEN_EXPLAIN] = 0;
        int r0 = save_requests;
        doom_settings_frame(1, 1);
        assert(profile[PROF_AIM_ASSIST] == 0 && save_requests == r0 + 1);                 // the unsaved choice is restored
        assert(dm_doom_game_setting_get(0) == 1 && dm_doom_game_setting_get(1) == 43 && dm_doom_game_setting_get(2) == 0);
        // an imported profile with other values for the clean keys keeps them
        cv_set(&cv_aim, "0");   // (a load would set it; simulate a second load)
        doom_settings_frame(0, 1); assert(dm_doom_game_setting_get(0) == 0 && !strcmp(cv_aim.str, "0"));
        printf("phase B ok\n");
    }
    return 0;
}
'''

def main():
    src = open(SRC, encoding='utf8').read()
    src = src.replace('#include "hle.h"', STUB)
    src = src.replace('(g_base + FN_SET_AIM_TOGGLE))', '(fake_set_aim))').replace('(g_base + FN_SET_GLOBAL_SCALE))', '(fake_set_gs))')
    src = src.replace('(g_base + FN_CVAR_SET_BOOL))', '(fake_set_bool))')
    assert src.count('fake_set_aim)') >= 1 and src.count('fake_set_gs)') >= 1 and src.count('fake_set_bool)') >= 1
    tmp = tempfile.mkdtemp(prefix='t_settings_')
    try:
        c = os.path.join(tmp, 't_settings.c')
        open(c, 'w', encoding='utf8', newline='\n').write(src + TEST)
        exe = os.path.join(tmp, 't_settings')
        def wsl(p): d, rest = p[0].lower(), p[2:].replace('\\', '/'); return f'/mnt/{d}{rest}'
        use_wsl = shutil.which('gcc') is None and os.name == 'nt'
        cc = (['wsl.exe', '-e', 'gcc'] if use_wsl else ['gcc'])
        cv = (lambda p: wsl(p)) if use_wsl else (lambda p: p)
        subprocess.run(cc + ['-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function', '-fsanitize=address,undefined', '-o', cv(exe), cv(c), '-lm'], check=True)
        env = dict(os.environ)
        save = os.path.join(tmp, 'save')
        for ph in 'AB':
            if use_wsl:
                r = subprocess.run(['wsl.exe', '-e', 'env', 'DM_SAVE=' + cv(save), cv(exe), ph], capture_output=True, text=True)
            else:
                env['DM_SAVE'] = save; r = subprocess.run([exe, ph], capture_output=True, text=True, env=env)
            sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)
            if r.returncode: print(f'FAIL in phase {ph}'); return 1
        print('mirror file:'); print(open(save + '.ctl').read())
        print('test_doom_settings: OK')
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

if __name__ == '__main__':
    sys.exit(main())
