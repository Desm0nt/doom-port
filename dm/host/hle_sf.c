// nn::sf::hipc client entry points (DOOM, nnSdk 6.4.0): every SDK service client (the CmifProxy / Hipc2ProxyKind templates in sdk) reaches the
// kernel only through a handful of exported functions, and sdk calls them through its own PLT, so an HLE_FORCE on each name catches the SDK's
// internal calls too (checked against sdk.raw: all nine names below are exported and have PLT slots in the 6.4.0 build).
// On a Switch, InitializeHipcServiceResolution connects to sm, ConnectToHipcService asks sm for a named port, and SendSyncRequest is
// svcSendSyncRequest on the session. The host has no sm and no service processes, so any SDK path that reaches a service the host did not take
// over by name used to fail inside sm::Initialize (SVC 0x1F / 0x21 placeholders, then an anonymous nn::diag abort). Here:
//   - name resolution always succeeds (nothing to connect to);
//   - ConnectToHipcService hands out a host session for the services listed in DM_SF_SERVE (comma-separated, empty by default) and refuses every
//     other name with a red line naming the service, as sm refuses a service nobody registered (ResultNotRegistered 0xE15): the caller's own
//     error handling runs and the log says which service DOOM reached, instead of an unnamed abort in sm;
//   - every request on a host session succeeds with the output data zeroed and no objects or handles (DM_SF_TRACE=1 dumps each request).
// Session handles live outside the kernel handle table (hle_thread.c hands out 0x1000 + slot, MAX_H 4096), so svc CloseHandle on one is a no-op there.
// Message layout (Atmosphère / libnx): HIPC header {u16 type, counts of X / A / B / W descriptors; u32 data words, receive list, special header flag},
// the special header and descriptors, then the data words, whose CMIF part starts 16-byte aligned to the buffer: in a domain session the domain
// header {u8 type, u8 objects, u16 size, u32 object, u32, u32 token} first, then {"SFCI", version, command, token} and the arguments. The response
// is the same with {u32 objects, 3 x u32} and {"SFCO", version, result, token}.
//
// Portions ported from nxnative (https://github.com/seedds/nxnative) dm/host/hle_sf.c, Copyright (c) 2026 seedds, MIT License.
// Android / DOOM adaptations: the served list comes from DM_SF_SERVE instead of a fixed olsc:u entry (DOOM links no client of its own), and the
// whole file is DOOM-only so the D2R force table (which aborts on unreferenced entries) is unchanged.
#include "hle.h"
#ifdef DM_GAME_DOOM
#include <pthread.h>

#define SF_H0 0xd0000u   // session handles: outside the kernel handle table (0x1000 + slot, 4096 slots, hle_thread.c)
#define SF_MAX 64
#define SF_RES_NOT_REGISTERED 0xe15u   // sm ResultNotRegistered (module 21, description 7)
#define SF_RES_INVALID_HANDLE 0xe401u  // kernel ResultInvalidHandle (module 1, description 114)
static struct { char name[16]; int used, domain; unsigned seen; } g_sf[SF_MAX];
static pthread_mutex_t g_sf_mu = PTHREAD_MUTEX_INITIALIZER;

static int sf_slot(uint32_t h) {
    unsigned i = h - SF_H0;
    return i < SF_MAX && g_sf[i].used ? (int)i : -1;
}
static uint32_t sf_new(const char *name, int domain) {
    pthread_mutex_lock(&g_sf_mu);
    unsigned i = 0; while (i < SF_MAX && g_sf[i].used) i++;
    if (i == SF_MAX) die("sf: more than %d host service sessions", SF_MAX);
    strlcpy(g_sf[i].name, name, sizeof g_sf[i].name); g_sf[i].used = 1; g_sf[i].domain = domain; g_sf[i].seen = 0;
    pthread_mutex_unlock(&g_sf_mu);
    return SF_H0 + i;
}
// DM_SF_SERVE="svc:a,svc:b": the services the host answers with zeroed success (a bring-up aid; service names are at most 8 characters)
static int sf_served(const char *name) {
    const char *e = getenv("DM_SF_SERVE");
    for (const char *p = e ? e : ""; *p;) {
        size_t l = strcspn(p, ",");
        if (l && l <= 8 && !strncmp(p, name, l) && (l == 8 || !name[l])) return 1;
        p += l; if (*p == ',') p++;
    }
    return 0;
}

HLE_FORCE("_ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv", "service name resolution (connects to sm); the host resolves names itself", Result, h_sf_InitRes, void) { return RES_OK; }
HLE_FORCE("_ZN2nn2sf4hipc29FinalizeHipcServiceResolutionEv", "service name resolution (sm)", void, h_sf_FinRes, void) {}
HLE_FORCE("_ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc", "connect to a named service through sm -> a host session for DM_SF_SERVE names, otherwise refused (named red line)",
          Result, h_sf_Connect, uint32_t *out, const char *name) {
    char nm[9] = {0}; memcpy(nm, name, strnlen(name, 8));
    if (sf_served(nm)) { *out = sf_new(nm, 0); LOG("sf: %s -> host session %#x (every request succeeds with zeroed output)", nm, *out); return RES_OK; }
    LOG("【红】sf: the SDK connects to service %s, which the host does not serve (sm: not registered) -- hook the nn:: call that led here", nm);
    return SF_RES_NOT_REGISTERED;
}
HLE_FORCE("_ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE", "close a client session -> the host session", void, h_sf_Close, uint32_t h) {
    int i = sf_slot(h);
    if (i < 0) { LOG("【红】sf: CloseClientSessionHandle on %#x, not a host session", h); return; }
    pthread_mutex_lock(&g_sf_mu); g_sf[i].used = 0; pthread_mutex_unlock(&g_sf_mu);
}
// Control requests: the pointer buffer is the server's receive buffer for X descriptors (none: the host reads them in place), the domain conversion
// makes the session's object 1, a duplicate is a new session to the same service
HLE_FORCE("_ZN2nn2sf4hipc6client22QueryPointerBufferSizeEPtNS_3svc6HandleE", "control request: pointer buffer size -> 0 for a host session", Result, h_sf_PtrBuf, uint16_t *out, uint32_t h) {
    if (sf_slot(h) < 0) { LOG("【红】sf: QueryPointerBufferSize on %#x, not a host session", h); return SF_RES_INVALID_HANDLE; }
    *out = 0; return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client22ConvertSessionToDomainEPNS0_4cmif18CmifDomainObjectIdENS_3svc6HandleE", "control request: convert to a domain -> the host session's object 1",
          Result, h_sf_ToDomain, uint32_t *out, uint32_t h) {
    int i = sf_slot(h);
    if (i < 0) { LOG("【红】sf: ConvertSessionToDomain on %#x, not a host session", h); return SF_RES_INVALID_HANDLE; }
    g_sf[i].domain = 1; *out = 1; return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client16DuplicateSessionEPNS_3svc6HandleES4_", "control request: clone a session -> a new host session to the same service", Result, h_sf_Dup, uint32_t *out, uint32_t h) {
    int i = sf_slot(h);
    if (i < 0) { LOG("【红】sf: DuplicateSession on %#x, not a host session", h); return SF_RES_INVALID_HANDLE; }
    *out = sf_new(g_sf[i].name, g_sf[i].domain); return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client18DuplicateSessionExEPNS_3svc6HandleES4_j", "control request: clone a session (with a tag) -> a new host session", Result, h_sf_DupEx,
          uint32_t *out, uint32_t h, unsigned tag) { (void)tag; return h_sf_Dup(out, h); }

HLE_FORCE("_ZN2nn2sf4hipc15SendSyncRequestENS_3svc6HandleEPvm", "send a request on a session (svcSendSyncRequest) -> answered by the host for its sessions", Result, h_sf_Send,
          uint32_t h, uint32_t *m, uint64_t size) {
    int i = sf_slot(h);
    if (i < 0 || size < 0x40) { LOG("【红】sf: SendSyncRequest on %#x (%llu bytes), not a host session", h, (unsigned long long)size); return SF_RES_INVALID_HANDLE; }
    uint8_t *base = (uint8_t *)m;
    unsigned type = m[0] & 0xffff, nx = m[0] >> 16 & 15, nabw = (m[0] >> 20 & 15) + (m[0] >> 24 & 15) + (m[0] >> 28 & 15), ndw = m[1] & 0x3ff;
    uint32_t *p = m + 2;
    if (m[1] >> 31) { uint32_t sh = *p++; p += (sh & 1 ? 2 : 0) + (sh >> 1 & 15) + (sh >> 5 & 15); }   // special header: process ID, copy and move handles
    p += nx * 2 + nabw * 3;
    size_t data = (size_t)((uint8_t *)p - base), cm = (data + 15) & ~(size_t)15;
    int dom = g_sf[i].domain && (type == 4 || type == 6);
    unsigned dtype = dom ? base[cm] : 1, obj = dom ? *(uint32_t *)(base + cm + 4) : 0;
    size_t hin = cm + (dom ? 16 : 0);
    if (hin + 16 > size || data + ndw * 4 > size) { LOG("【红】sf: %s: request of %u data words does not fit %llu bytes", g_sf[i].name, ndw, (unsigned long long)size); return SF_RES_INVALID_HANDLE; }
    uint32_t magic = *(uint32_t *)(base + hin), ver = *(uint32_t *)(base + hin + 4), cmd = *(uint32_t *)(base + hin + 8);
    static int trace = -1; if (trace < 0) trace = getenv("DM_SF_TRACE") && atoi(getenv("DM_SF_TRACE"));
    if (trace) { char hx[3 * 0x80 + 1] = ""; for (unsigned k = 0; k < 0x80 && k < size; k++) snprintf(hx + 3 * k, 4, "%02x ", base[k]); LOG("sf: %s request: %s", g_sf[i].name, hx); }
    if (type != 4 && type != 6) { LOG("【红】sf: %s: HIPC message type %u not served", g_sf[i].name, type); return SF_RES_INVALID_HANDLE; }
    if (dtype == 2) { memset(base, 0, 16); return RES_OK; }   // a domain object closed: nothing to answer
    if (magic != 0x49434653) { LOG("【红】sf: %s: request without SFCI (%#x)", g_sf[i].name, magic); return SF_RES_INVALID_HANDLE; }
    unsigned key = (obj & 0xff) << 16 | (cmd & 0xffff);   // log each object / command once
    pthread_mutex_lock(&g_sf_mu); int first = g_sf[i].seen != key; g_sf[i].seen = key; pthread_mutex_unlock(&g_sf_mu);
    if (first || trace) LOG("sf: %s object %u command %u (%u data words in): success, zeroed output", g_sf[i].name, obj, cmd, ndw);
    // The response: no special header and no descriptors, so the data words start at +8 and the CMIF part at +0x10; everything after the headers up to the buffer's end reads as zero
    size_t end = size < 0x1000 ? size : 0x1000;
    memset(base, 0, end);
    m[1] = (uint32_t)((end - 8) / 4 < 0x3ff ? (end - 8) / 4 : 0x3ff);
    size_t out = 0x10 + (dom ? 16 : 0);
    uint32_t *o = (uint32_t *)(base + out); o[0] = 0x4f434653; o[1] = ver; o[2] = RES_OK; o[3] = 0;   // "SFCO"; a domain's out-object count before it stays 0
    return RES_OK;
}
#endif
