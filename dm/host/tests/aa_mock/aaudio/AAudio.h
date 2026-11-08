// [DOOM] t_audio_aa.c 用的 AAudio 替身（只含 hle_audio.c 用到的部分）：每路流一条回调线程，低延迟 2 ms × 96 帧、NONE 20 ms × 960 帧。
// 故障注入（issues #12/#20 的 Samsung 行为）：mock_stall_ll_ms > 0 时低延迟流回调满这么久后卡住（不再回调），再过 1 s 报 AAUDIO_ERROR_TIMEOUT
// 并退出回调线程，和真机 processData() TIMEOUT 一样；mock_fail_start_ll > 0 时低延迟流的 requestStart 回 DISCONNECTED（「stolen」）这么多次。
// [DOOM] AAudio stand-in for t_audio_aa.c (only what hle_audio.c uses): one callback thread per stream, low latency 2 ms x 96 frames, NONE 20 ms x 960 frames.
// Fault injection (the Samsung behaviour of issues #12/#20): with mock_stall_ll_ms > 0 a low-latency stream stops calling back after that long, and 1 s later
// reports AAUDIO_ERROR_TIMEOUT and leaves its callback thread, like the device's processData() TIMEOUT; with mock_fail_start_ll > 0 a low-latency stream's
// requestStart returns DISCONNECTED ("stolen") that many times.
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
typedef int32_t aaudio_result_t;
typedef int32_t aaudio_data_callback_result_t;
enum { AAUDIO_OK = 0, AAUDIO_ERROR_DISCONNECTED = -899, AAUDIO_ERROR_INVALID_STATE = -895, AAUDIO_ERROR_TIMEOUT = -885 };
enum { AAUDIO_DIRECTION_OUTPUT = 0, AAUDIO_FORMAT_PCM_I16 = 1, AAUDIO_SHARING_MODE_SHARED = 1, AAUDIO_USAGE_GAME = 14, AAUDIO_CONTENT_TYPE_SONIFICATION = 4 };
enum { AAUDIO_PERFORMANCE_MODE_NONE = 10, AAUDIO_PERFORMANCE_MODE_POWER_SAVING = 11, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12 };
enum { AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_CALLBACK_RESULT_STOP = 1 };
typedef struct AAudioStream AAudioStream;
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream *, void *, void *, int32_t);
typedef void (*AAudioStream_errorCallback)(AAudioStream *, void *, aaudio_result_t);
typedef struct { int rate, ch, perf; AAudioStream_dataCallback cb; AAudioStream_errorCallback ecb; void *ctx, *ectx; } AAudioStreamBuilder;
struct AAudioStream { AAudioStreamBuilder p; pthread_t th; int live; _Atomic int run; int16_t buf[960 * 8]; int32_t bufsz; };

static _Atomic int mock_stall_ll_ms, mock_fail_start_ll, mock_opens_ll, mock_opens_none, mock_closes, mock_timeouts, mock_live;
static _Atomic uint64_t mock_cb_ll, mock_cb_none;
static uint64_t mock_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void *mock_loop(void *p) {
    AAudioStream *s = p; int ll = s->p.perf == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY, frames = ll ? 96 : 960;
    uint64_t period = ll ? 2000000ull : 20000000ull, t0 = mock_ns(), t = t0; int stall = ll ? atomic_load(&mock_stall_ll_ms) : 0;
    while (atomic_load(&s->run)) {
        uint64_t now = mock_ns();
        if (stall && now - t0 > (uint64_t)stall * 1000000ull) {   // 卡在 processData：1 s 后 TIMEOUT，回调线程退出 | stuck in processData: TIMEOUT after 1 s, the callback thread leaves
            uint64_t d = now + 1000000000ull; while (atomic_load(&s->run) && mock_ns() < d) usleep(1000);
            if (atomic_load(&s->run)) { atomic_fetch_add(&mock_timeouts, 1); if (s->p.ecb) s->p.ecb(s, s->p.ectx, AAUDIO_ERROR_TIMEOUT); }
            break;
        }
        t += period; if (t > now) usleep((useconds_t)((t - now) / 1000));
        atomic_fetch_add(ll ? &mock_cb_ll : &mock_cb_none, 1);
        if (s->p.cb(s, s->p.ctx, s->buf, frames) == AAUDIO_CALLBACK_RESULT_STOP) break;
    }
    return NULL;
}
static aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **b) { *b = calloc(1, sizeof **b); (*b)->perf = AAUDIO_PERFORMANCE_MODE_NONE; return AAUDIO_OK; }
static void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, int v) { (void)b; (void)v; }
static void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int v) { b->rate = v; }
static void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int v) { b->ch = v; }
static void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, int v) { (void)b; (void)v; }
static void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, int v) { b->perf = v; }
static void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, int v) { (void)b; (void)v; }
static void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, int v) { (void)b; (void)v; }
static void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, int v) { (void)b; (void)v; }
static void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *ctx) { b->cb = cb; b->ctx = ctx; }
static void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *ctx) { b->ecb = cb; b->ectx = ctx; }
static aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **out) {
    AAudioStream *s = calloc(1, sizeof *s); s->p = *b; *out = s;
    atomic_fetch_add(b->perf == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? &mock_opens_ll : &mock_opens_none, 1); atomic_fetch_add(&mock_live, 1);
    return AAUDIO_OK;
}
static aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { free(b); return AAUDIO_OK; }
static aaudio_result_t AAudioStream_requestStart(AAudioStream *s) {
    if (s->live) return AAUDIO_OK;
    if (s->p.perf == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY && atomic_load(&mock_fail_start_ll) > 0) { atomic_fetch_sub(&mock_fail_start_ll, 1); return AAUDIO_ERROR_DISCONNECTED; }
    atomic_store(&s->run, 1); s->live = !pthread_create(&s->th, NULL, mock_loop, s); return s->live ? AAUDIO_OK : AAUDIO_ERROR_INVALID_STATE;
}
static aaudio_result_t AAudioStream_requestStop(AAudioStream *s) { if (s->live) { atomic_store(&s->run, 0); pthread_join(s->th, NULL); s->live = 0; } return AAUDIO_OK; }
static aaudio_result_t AAudioStream_close(AAudioStream *s) { AAudioStream_requestStop(s); atomic_fetch_add(&mock_closes, 1); atomic_fetch_sub(&mock_live, 1); free(s); return AAUDIO_OK; }
static int32_t AAudioStream_getChannelCount(AAudioStream *s) { return s->p.ch; }
static int32_t AAudioStream_getSampleRate(AAudioStream *s) { return s->p.rate; }
static int32_t AAudioStream_getPerformanceMode(AAudioStream *s) { return s->p.perf; }
static int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { return s->p.perf == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? 96 : 960; }
static int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { return s->bufsz ? s->bufsz : 2 * AAudioStream_getFramesPerBurst(s); }
static int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream *s) { return 4 * AAudioStream_getFramesPerBurst(s); }
// [DOOM] issue #32: buffer floor (hle_audio.c aa_open) and the keeper's underrun count
static aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n) { int32_t c = AAudioStream_getBufferCapacityInFrames(s); s->bufsz = n > c ? c : n; return s->bufsz; }
static int32_t AAudioStream_getXRunCount(AAudioStream *s) { (void)s; return 0; }
static const char *AAudio_convertResultToText(aaudio_result_t r) { return r == AAUDIO_ERROR_TIMEOUT ? "AAUDIO_ERROR_TIMEOUT" : r == AAUDIO_ERROR_DISCONNECTED ? "AAUDIO_ERROR_DISCONNECTED" : "?"; }
