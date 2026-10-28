// 联网类（排除范围：联机/好友/商城/云存档）：一律接成「网络不可用」，游戏走离线分支。
// 剪口：nifm 报无网络；socket 只接 Initialize（返回成功，不建会话），其余 socket/ssl/friends 用到时再接成失败。
// Networking (out of scope: online play/friends/store/cloud saves): all hooked up as "network unavailable", the game takes the offline branch.
// Cuts: nifm reports no network; socket only hooks up Initialize (returns success, creates no session), the rest of socket/ssl/friends are hooked up as failures when they come into use.
#include "hle.h"
// Portions ported from nxnative (https://github.com/seedds/nxnative) dm/host/hle_net.c (nifm request hold, Poll / Select with nothing to wait on),
// Copyright (c) 2026 seedds, MIT License. DOOM-only (DM_GAME_DOOM); D2R keeps its answers.
#include <stdatomic.h>
#ifdef DM_GAME_DOOM
// [DOOM] The request is asynchronous on Horizon: SubmitNetworkRequest only hands it to the nifm service and IsNetworkRequestOnHold stays true until
// the service has evaluated it, so a query right after the submission finds it on hold, and the next one finds it settled (offline: rejected).
// Answering "settled" at once lets a game's network thread finish before the thread that started it has stored its own state (nxnative: Pikmin 4's
// boot hang); counting queries instead of host time keeps a query-bounded wait (Hades: cancel after 2,000 queries) from spinning.
static _Atomic int g_nifm_hold;
#endif

HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "连 bsd 套接字服务；离线，不建会话（剪）", Result, h_socket_Initialize, const void *cfg) { (void)cfg; return RES_OK; }
HLE_FORCE("_ZN2nn4nifm10InitializeEv", "连 nifm 网络管理服务；离线（剪）", Result, h_nifm_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "离线：网络请求不提交", void, h_nifm_Submit, void) {
#ifdef DM_GAME_DOOM
    atomic_store(&g_nifm_hold, 1);   // on hold for the first query, then settled as unavailable
#endif
}
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "离线：无请求可取消", void, h_nifm_Cancel, void) {
#ifdef DM_GAME_DOOM
    atomic_store(&g_nifm_hold, 0);
#endif
}
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "离线：请求不在等待中", int, h_nifm_OnHold, void) {
#ifdef DM_GAME_DOOM
    int h = atomic_load(&g_nifm_hold);
    while (h > 0 && !atomic_compare_exchange_weak(&g_nifm_hold, &h, h - 1)) {}
    return h > 0;
#else
    return 0;
#endif
}
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "离线：网络恒不可用", int, h_nifm_Available, void) { return 0; }
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "离线：报失败，不弹系统联网界面", Result, h_nifm_HandleErr, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "离线：无 IP", Result, h_nifm_GetIp, uint32_t *a) { if (a) *a = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "离线：无代理设置", Result, h_nifm_GetProxy, void *p) { (void)p; return RES_FAIL; }
HLE_FORCE("nnsslInitialize", "连 ssl 服务；网络库全局初始化依赖它成功，离线不建会话（剪）", Result, h_ssl_Initialize, void) { return RES_OK; }
HLE_FORCE("nnsslFinalize", "与 nnsslInitialize 配对（剪）", Result, h_ssl_Finalize, void) { return RES_OK; }

// ---------- socket：离线，所有连服务的调用按 BSD 习惯回 -1、错误码 ENETDOWN ----------
// 不剪：Inet* / nnsocketInet* 字节序与地址文本转换（纯计算）。
// ---------- socket: offline, all calls that contact the service return -1 in BSD convention, error code ENETDOWN ----------
// Not cut: Inet* / nnsocketInet* byte order and address text conversion (pure computation).
#define SK_ENETDOWN 50                  // Horizon 套接字用 FreeBSD 错误码 | Horizon sockets use FreeBSD error codes
static __thread int t_skerr = SK_ENETDOWN;
static int sk_fail(void) { t_skerr = SK_ENETDOWN; return -1; }
#define SKF(m, cname, ...) HLE_FORCE(m, "bsd 套接字服务（剪）：回 -1/ENETDOWN", int, cname, __VA_ARGS__)
#ifdef DM_GAME_DOOM
// [DOOM] Poll / Select with nothing to wait on: the host creates no socket, so every descriptor the game holds is -1, or it passes none. POSIX and
// Horizon's bsd service ignore negative descriptors and wait out the timeout, returning 0; failing at once makes a network thread that polls with a
// timeout spin a core (nxnative: DARK SOULS REMASTERED, 11,000 calls a second). An endless wait on nothing returns 0 after 100 ms here.
// dm_guest_sleep_ns (hle_thread.c, dm.h) is the guest-clock-aware sleep.
static int sk_idle(int64_t ms) { dm_guest_sleep_ns((ms < 0 ? 100 : ms) * 1000000); return 0; }
typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
static int sk_poll(void *fds, uint64_t n, int ms) {
    sk_pollfd *p = fds;
    for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0) return sk_fail();
    for (uint64_t i = 0; i < n; i++) p[i].revents = 0;
    return sk_idle(ms);
}
static int sk_select(int nfds, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *tv) {   // fd_set: 1024 bits; timeval: {i64 s, i64 us}
    const uint64_t *sets[3] = {r, w, e};
    for (int k = 0; k < 3; k++) for (int fd = 0; sets[k] && fd < nfds && fd < 1024; fd++) if (sets[k][fd >> 6] >> (fd & 63) & 1) return sk_fail();
    return sk_idle(tv ? tv[0] * 1000 + tv[1] / 1000 : -1);
}
#endif
SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { (void)a; (void)b; (void)c; return sk_fail(); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { (void)s; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
#ifdef DM_GAME_DOOM
HLE_FORCE("_ZN2nn6socket5CloseEi", "bsd close -> host close", int, h_sk_Close, int s) { int r = close(s); if (r < 0) t_skerr = errno; return r; }
#else
SKF("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { (void)s; return sk_fail(); }
#endif
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int s, int c) { (void)s; (void)c; return sk_fail(); }
SKF("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, unsigned long n, int t) { (void)f; (void)n; (void)t; return sk_fail(); }
SKF("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { (void)n; (void)r; (void)w; (void)e; (void)t; return sk_fail(); }
SKF("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", h_sk_EventFd, unsigned long v, int f) { (void)v; (void)f; return sk_fail(); }
SKF("_ZN2nn6socket11EventFdReadEiPm", h_sk_EventFdRead, int s, unsigned long *v) { (void)s; (void)v; return sk_fail(); }
SKF("_ZN2nn6socket12EventFdWriteEim", h_sk_EventFdWrite, int s, unsigned long v) { (void)s; (void)v; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "bsd 套接字服务（剪）", long, h_sk_Recv, int s, void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "bsd 套接字服务（剪）", long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "bsd 套接字服务（剪）", long, h_sk_Send, int s, const void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "域名解析（剪）：EAI_FAIL", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    (void)h; (void)s; (void)hint; if (res) *res = NULL; return 4;   // EAI_FAIL
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "与 GetAddrInfo 配对（剪）", void, h_sk_FreeAddrInfo, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "域名解析（剪）：NULL", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "套接字错误码由宿主记录", int, h_sk_GetLastError, void) { return t_skerr; }
// C 接口
// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { (void)a; (void)b; (void)c; return sk_fail(); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketClose", h_nsk_Close, int s) { (void)s; return sk_fail(); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("nnsocketFcntl", h_nsk_Fcntl, int s, int c) { (void)s; (void)c; return sk_fail(); }
SKF("nnsocketPoll", h_nsk_Poll, void *f, unsigned long n, int t) {
#ifdef DM_GAME_DOOM
    return sk_poll(f, n, t);
#else
    (void)f; (void)n; (void)t; return sk_fail();
#endif
}
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("nnsocketRecv", "bsd 套接字服务（剪）", long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("nnsocketSend", "bsd 套接字服务（剪）", long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("nnsocketGetHostByNameWithOptions", "域名解析（剪）：NULL", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("nnsocketGetLastErrno", "套接字错误码由宿主记录", int, h_nsk_GetErrno, void) { return t_skerr; }
HLE_FORCE("nnsocketSetLastErrno", "套接字错误码由宿主记录", void, h_nsk_SetErrno, int e) { t_skerr = e; }

// ---------- ssl：离线，Initialize 成功（网络库全局初始化依赖），其余连服务的一律失败 ----------
// 不剪：Context / Connection 构造析构、Context::GetContextId（只读写游戏对象）。
// ---------- ssl: offline, Initialize succeeds (the network library's global initialization depends on it), everything else that contacts the service fails ----------
// Not cut: Context / Connection construction and destruction, Context::GetContextId (only read and write game objects).
#define SSF(m, cname, ...) HLE_FORCE(m, "ssl 服务（剪）：失败", Result, cname, __VA_ARGS__)
HLE_FORCE("_ZN2nn3ssl10InitializeEv", "与 nnsslInitialize 一致：成功、不建会话（剪）", Result, h_ssl_InitCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl8FinalizeEv", "与 Initialize 配对（剪）", Result, h_ssl_FinCpp, void) { return RES_OK; }
SSF("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", h_ssl_CtxCreate, void *self, int v) { (void)self; (void)v; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context7DestroyEv", h_ssl_CtxDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context19RegisterInternalPkiEPmNS1_11InternalPkiE", h_ssl_CtxRegPki, void *self, uint64_t *id, int p) { (void)self; (void)id; (void)p; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context9RemovePkiEm", h_ssl_CtxRemovePki, void *self, uint64_t id) { (void)self; (void)id; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", h_ssl_BiBufSize, uint32_t *o, void *ids, unsigned n) { (void)ids; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj", h_ssl_BiCerts, void **o, uint8_t *b, unsigned bs, void *ids, unsigned n) { (void)o; (void)b; (void)bs; (void)ids; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", h_ssl_CnCreate, void *self, void *ctx) { (void)self; (void)ctx; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection7DestroyEv", h_ssl_CnDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11DoHandshakeEv", h_ssl_CnHandshake, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11SetHostNameEPKcj", h_ssl_CnHost, void *self, const char *h, unsigned n) { (void)self; (void)h; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection9SetIoModeENS1_6IoModeE", h_ssl_CnIoMode, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", h_ssl_CnCache, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", h_ssl_CnSock, void *self, int s) { (void)self; (void)s; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection12GetLastErrorEPNS_6ResultE", h_ssl_CnLastErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection18GetVerifyCertErrorEPNS_6ResultE", h_ssl_CnVerifyErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PeekEPcPij", h_ssl_CnPeek, void *self, char *b, int *o, unsigned n) { (void)self; (void)b; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PollEPNS1_9PollEventES3_j", h_ssl_CnPoll, void *self, void *o, void *i, unsigned t) { (void)self; (void)o; (void)i; (void)t; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl10Connection4ReadEPcj", "ssl 服务（剪）：读到 -1", int, h_ssl_CnRead, void *self, char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
HLE_FORCE("_ZN2nn3ssl10Connection5WriteEPKcj", "ssl 服务（剪）：写出 -1", int, h_ssl_CnWrite, void *self, const char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
// C 接口：全部返回 nnResult
// C interface: all return nnResult
#define SSC(m, cname) HLE_FORCE(m, "ssl 服务（剪）：失败", Result, cname, void) { return RES_FAIL; }
SSC("nnsslConnectionCreate", h_nssl_Create) SSC("nnsslConnectionDestroy", h_nssl_Destroy)
SSC("nnsslConnectionDoHandshakeWithCertBuffer", h_nssl_Handshake) SSC("nnsslConnectionGetConnectionId", h_nssl_GetId)
SSC("nnsslConnectionGetNextAlpnProto", h_nssl_GetAlpn) SSC("nnsslConnectionGetVerifyCertError", h_nssl_VerifyErr)
SSC("nnsslConnectionPeek", h_nssl_Peek) SSC("nnsslConnectionPending", h_nssl_Pending) SSC("nnsslConnectionRead", h_nssl_Read)
SSC("nnsslConnectionSetHostName", h_nssl_SetHost) SSC("nnsslConnectionSetIoMode", h_nssl_SetIo)
SSC("nnsslConnectionSetNextAlpnProto", h_nssl_SetAlpn) SSC("nnsslConnectionSetOption", h_nssl_SetOpt)
SSC("nnsslConnectionSetSessionCacheMode", h_nssl_SetCache) SSC("nnsslConnectionSetSocketDescriptor", h_nssl_SetSock)
SSC("nnsslConnectionSetVerifyOption", h_nssl_SetVerify) SSC("nnsslConnectionWrite", h_nssl_Write)
SSC("nnsslContextDestroy", h_nssl_CtxDestroy) SSC("nnsslContextGetContextId", h_nssl_CtxGetId)
#ifdef DM_GAME_DOOM
// nnSdk 6.4.0 sockets for DOOM: real host sockets. Creating / binding a local UDP socket needs no network on the console
// either, and DOOM's lobby layer retries InitPort every frame (holding the legal menu) while Socket() fails. nifm still
// reports no network, so nothing goes online. Horizon's socket ABI is FreeBSD's: sockaddr has sin_len, SOL_SOCKET / SO_*,
// O_NONBLOCK, MSG_* and errno values differ from Linux and are translated here.
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>
static int bsd_errno(int e) {
    switch (e) {
    case EAGAIN: return 35; case EINPROGRESS: return 36; case EALREADY: return 37; case ENOTSOCK: return 38; case EMSGSIZE: return 40;
    case EADDRINUSE: return 48; case EADDRNOTAVAIL: return 49; case ENETDOWN: return 50; case ENETUNREACH: return 51; case ECONNRESET: return 54;
    case EISCONN: return 56; case ENOTCONN: return 57; case ETIMEDOUT: return 60; case ECONNREFUSED: return 61; case EHOSTUNREACH: return 65;
    default: return e;   // EINTR / EBADF / EINVAL / EACCES / ENOMEM ... share values
    }
}
static int sk_ret(long r) { if (r < 0) t_skerr = bsd_errno(errno); return (int)r; }
// BSD sockaddr {u8 len, u8 family, ...} <-> Linux {u16 family, ...}
static socklen_t sa_in(const uint8_t *b, unsigned n, struct sockaddr_storage *o) {
    memset(o, 0, sizeof *o); if (!b || n < 2) return 0;
    if (n > sizeof *o) n = sizeof *o;
    memcpy(o, b, n); ((struct sockaddr *)o)->sa_family = b[1]; return (socklen_t)n;
}
static void sa_out(const struct sockaddr_storage *in, socklen_t len, uint8_t *b, unsigned *n) {
    if (!b || !n) return;
    unsigned k = len < *n ? len : *n; memcpy(b, in, k);
    if (k >= 2) { b[1] = (uint8_t)((const struct sockaddr *)in)->sa_family; b[0] = (uint8_t)len; }
    *n = len;
}
static int sol_level(int l) { return l == 0xffff ? SOL_SOCKET : l; }
static int so_opt(int l, int o) {
    if (l != 0xffff) return o;
    switch (o) {
    case 0x1: return SO_DEBUG; case 0x4: return SO_REUSEADDR; case 0x8: return SO_KEEPALIVE; case 0x10: return SO_DONTROUTE; case 0x20: return SO_BROADCAST;
    case 0x80: return SO_LINGER; case 0x100: return SO_OOBINLINE; case 0x200: return SO_REUSEPORT; case 0x1001: return SO_SNDBUF; case 0x1002: return SO_RCVBUF;
    case 0x1003: return SO_SNDLOWAT; case 0x1004: return SO_RCVLOWAT; case 0x1005: return SO_SNDTIMEO; case 0x1006: return SO_RCVTIMEO;
    case 0x1007: return SO_ERROR; case 0x1008: return SO_TYPE; default: return -1;
    }
}
static int msg_flags(int f) { int r = f & 3; if (f & 0x80) r |= MSG_DONTWAIT; if (f & 0x40) r |= MSG_WAITALL; return r; }
#define SKD(m, cname, ...) HLE_FORCE(m, "bsd socket -> host socket (FreeBSD ABI translated)", int, cname, __VA_ARGS__)
SKD("_ZN2nn6socket6SocketEiii", h_d_sk_Socket, int a, int b, int c) {
    int t = b & 0x0fffffff;                                   // FreeBSD type flags: SOCK_NONBLOCK 0x20000000, SOCK_CLOEXEC 0x10000000
    if (b & 0x20000000) t |= SOCK_NONBLOCK; if (b & 0x10000000) t |= SOCK_CLOEXEC;
    int fd = sk_ret(socket(a, t, c)); LOG("[net] socket(%d, %#x, %d) -> %d", a, b, c, fd); return fd;
}
SKD("_ZN2nn6socket4BindEiPK8sockaddrj", h_d_sk_Bind, int s, const uint8_t *a, unsigned l) { struct sockaddr_storage ss; socklen_t n = sa_in(a, l, &ss); return sk_ret(bind(s, (struct sockaddr *)&ss, n)); }
// Offline build: only loopback traffic may leave a socket (lobby / voice / HTTP all stay local, like D2R's offline port).
static int sa_offline(const struct sockaddr_storage *ss) {
    if (ss->ss_family == AF_INET) return (ntohl(((const struct sockaddr_in *)ss)->sin_addr.s_addr) >> 24) != 127;
    return ss->ss_family != AF_UNSPEC;   // IPv6 and anything else: not allowed
}
SKD("_ZN2nn6socket7ConnectEiPK8sockaddrj", h_d_sk_Connect, int s, const uint8_t *a, unsigned l) {
    struct sockaddr_storage ss; socklen_t n = sa_in(a, l, &ss);
    if (sa_offline(&ss)) { t_skerr = 51; return -1; }   // ENETUNREACH
    return sk_ret(connect(s, (struct sockaddr *)&ss, n));
}
SKD("_ZN2nn6socket6AcceptEiP8sockaddrPj", h_d_sk_Accept, int s, uint8_t *a, unsigned *l) {
    struct sockaddr_storage ss; socklen_t n = sizeof ss; int r = sk_ret(accept(s, (struct sockaddr *)&ss, &n)); if (r >= 0) sa_out(&ss, n, a, l); return r;
}
HLE_FORCE("_ZN2nn6socket4RecvEiPvmi", "bsd recv -> host", long, h_d_sk_Recv, int s, void *b, unsigned long n, int f) { long r = recv(s, b, n, msg_flags(f)); if (r < 0) { sk_ret(r); return -1; } return r; }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", "bsd recvfrom -> host", long, h_d_sk_RecvFrom, int s, void *b, unsigned long n, int f, uint8_t *a, unsigned *l) {
    struct sockaddr_storage ss; socklen_t sl = sizeof ss; long r = recvfrom(s, b, n, msg_flags(f), (struct sockaddr *)&ss, &sl);
    if (r < 0) { sk_ret(r); return -1; } sa_out(&ss, sl, a, l); return r;
}
HLE_FORCE("_ZN2nn6socket4SendEiPKvmi", "bsd send -> host", long, h_d_sk_Send, int s, const void *b, unsigned long n, int f) { long r = send(s, b, n, msg_flags(f) | MSG_NOSIGNAL); if (r < 0) { sk_ret(r); return -1; } return r; }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", "bsd sendto -> host", long, h_d_sk_SendTo, int s, const void *b, unsigned long n, int f, const uint8_t *a, unsigned l) {
    struct sockaddr_storage ss; socklen_t sl = sa_in(a, l, &ss);
    if (a && sa_offline(&ss)) { t_skerr = 51; return -1; }   // offline: ENETUNREACH
    long r = sendto(s, b, n, msg_flags(f) | MSG_NOSIGNAL, a ? (struct sockaddr *)&ss : NULL, sl);
    if (r < 0) { sk_ret(r); return -1; } return r;
}
SKD("_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval", h_d_sk_Select, int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t) { return sk_ret(select(n, r, w, e, t)); }
SKD("_ZN2nn6socket4PollEP6pollfdmi", h_d_sk_Poll, struct pollfd *f, unsigned long n, int t) { return sk_ret(poll(f, (nfds_t)n, t)); }
SKD("_ZN2nn6socket5FcntlEiiz", h_d_sk_Fcntl, int s, int c, long a) {
    if (c == F_GETFL) { int r = fcntl(s, F_GETFL); if (r < 0) return sk_ret(r); return (r & O_NONBLOCK) ? 4 : 0; }
    if (c == F_SETFL) { int cur = fcntl(s, F_GETFL); if (cur < 0) return sk_ret(cur); return sk_ret(fcntl(s, F_SETFL, (a & 4) ? (cur | O_NONBLOCK) : (cur & ~O_NONBLOCK))); }
    t_skerr = 22; return -1;
}
SKD("_ZN2nn6socket10SetSockOptEiiiPKvj", h_d_sk_SetOpt, int s, int l, int o, const void *v, unsigned n) {
    int lo = so_opt(l, o); if (lo < 0) { LOG("[net] setsockopt level %#x option %#x ignored", l, o); return 0; } return sk_ret(setsockopt(s, sol_level(l), lo, v, n));
}
SKD("_ZN2nn6socket10GetSockOptEiiiPvPj", h_d_sk_GetOpt, int s, int l, int o, void *v, unsigned *n) {
    int lo = so_opt(l, o); if (lo < 0) { t_skerr = 42; return -1; } socklen_t sl = n ? *n : 0; int r = sk_ret(getsockopt(s, sol_level(l), lo, v, &sl)); if (n) *n = sl; return r;
}
SKD("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", h_d_sk_GetName, int s, uint8_t *a, unsigned *l) {
    struct sockaddr_storage ss; socklen_t n = sizeof ss; int r = sk_ret(getsockname(s, (struct sockaddr *)&ss, &n)); if (r >= 0) sa_out(&ss, n, a, l); return r;
}
SKD("_ZN2nn6socket8ShutdownEii", h_d_sk_Shutdown, int s, int h) { return sk_ret(shutdown(s, h)); }
SKD("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", h_d_sk_GetAddrInfo, const char *n, const char *sv, const void *h, void **r) { (void)n; (void)sv; (void)h; if (r) *r = NULL; t_skerr = SK_ENETDOWN; return -1; }   // name resolution stays offline
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "nothing was allocated", void, h_d_sk_FreeAddrInfo, void *a) { (void)a; }
HLE_FORCE("_ZN2nn6socket13GetHostByNameEPKc", "name resolution offline: NULL", void *, h_d_sk_GetHostByName, const char *n) { (void)n; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "last socket error (thread-local)", int, h_d_sk_GetLastErrno, void) { return t_skerr; }
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", "offline: no IP configuration", Result, h_d_nifm_IpCfg, void *a, void *b, void *c, void *d, void *e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; return RES_FAIL;
}
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", "offline: no IP address", Result, h_d_nifm_Ip, void *a) { (void)a; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7ContextC1Ev", "ssl context object: no session (the SDK constructor stores 0 at +0, sdk+0x36a670)", void, h_d_ssl_CtxCtor, void *self) { *(uint64_t *)self = 0; }
HLE_FORCE("_ZN2nn3ssl7ContextD1Ev", "ssl context object: no session", void, h_d_ssl_CtxDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", "ssl offline: import fails", Result, h_d_ssl_ImportPki, void *self, uint64_t *id, const char *p, unsigned n, int fmt) {
    (void)self; (void)p; (void)n; (void)fmt; if (id) *id = 0; return RES_FAIL;
}
#endif
