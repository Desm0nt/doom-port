// DOOM (Switch, nnSdk 6.4.0): [fallback, -DDM_DOOM_HOST_MIXER=1; default is hle_audren.c] nn::audio AudioRenderer as a host software mixer, AudioIn without a microphone,
// OpenDefaultAudioOut forwarded to the AudioOut implementation in hle_audio.c.
//
// Who uses what (main.raw, update v262144): the game's own "MainAudioOut" sink (functions around main 0x200ac6c..0x200b9a8)
// opens one renderer (48 kHz, sampleCount = rate / 200 = 240, mixBufferCount 6, voiceCount 32), one final mix with 6 buffers,
// one device sink with inputs {0,1,2,3,4,5}, and per stream one memory pool + one voice (PcmInt16, mono or stereo, no ADPCM:
// the only AcquireVoiceSlot call site 0x200b5b4 passes w4 = 2, x6 = x7 = 0) fed by a ring of up to 4 WaveBuffers.
// The SDK side of the renderer (config work buffer, update packets, audren service) is replaced entirely; every renderer
// entry point main imports is taken over here, so the SDK config structures are never built. Opaque handles get one host
// pointer in their first 8 bytes, which is exactly what the SDK itself writes there (see the layout notes below).
//
// Left running natively (pure SDK code, no service): InitializeAudioRendererParameter (only GetCurrentRevision),
// GetAudioRendererConfigWorkBufferSize, GetSampleByteSize (table), InitializeAudioInParameter, SetAudioInBufferInfo,
// GetAudioInBufferDataPointer/DataSize, and the PolyphaseResampler helpers (only libm sqrt/sin + an internal fn pointer).
//
// ---- Layouts (derived from SDK 6.4.0 sdk.raw; offsets are module offsets) ----
// AudioRendererParameter (0x3c bytes; InitializeAudioRendererParameter 0x7a7c0 writes 0x00..0x3b):
//   +0x00 int sampleRate (48000)          +0x04 int sampleCount (240)        +0x08 int mixBufferCount (1)
//   +0x0c int subMixCount (0)             +0x10 int voiceCount (1)           +0x14 int sinkCount (1)
//   +0x18 int effectCount (0)             +0x1c int performanceFrameCount    +0x20 bool isVoiceDropEnabled
//   +0x24 int splitterCount               +0x28 int splitterSendChannelCount +0x2c int renderingDevice
//   +0x30 int executionMode               +0x34 int (pool/bitmap size)       +0x38 uint32 revision (GetCurrentRevision)
//   evidence: GetAudioRendererConfigWorkBufferSize 0x7a840 passes +0x10 to VoiceInfoManager::GetWorkBufferSize, +0x24/+0x28 to
//   SplitterInfoManager sizes; InitializeAudioRendererConfig 0x7aa20 (+0x18 * 0xe0 EffectInfo), 0x7aa90 (+0xc * 0x930 MixInfo + 1
//   final mix), 0x7aae8 (+0x14 * 0x140 SinkInfo); OpenAudioRenderer 0x8ad60 tests +0x30 (manual) / +0x2c.
//   main writes +0x00 = 48000, +0x04 = 240, +0x08 = 6, +0x10 = 32 (0x200ad00..0x200ad1c).
// AudioRendererHandle (16 bytes, passed by value in x0:x1): +0 service object, +8 SystemEvent* (OpenAudioRenderer 0x8adcc
//   stores 0 there, 0x8adf8 the event in manual mode); Start/Stop/Close (0x8ae80 / 0x8aea0 / 0x8ae20) only use +0.
// AudioRendererConfig: InitializeAudioRendererConfig writes +0x00..+0x7f (0x7aea0..0x7aee4: manager pointers, update buffers);
//   we store a host pointer at +0 only. main keeps it in a 0xb0-byte slot (0x7387cc8 + 0x98).
// VoiceType / FinalMixType / MemoryPoolType / DeviceSinkType: the SDK writes only +0 (8 bytes):
//   detail::AcquireVoiceSlot 0x84aac `str x27,[x26]`, ReleaseVoiceSlot 0x84fac; AcquireFinalMix 0x80d20 (MixManager::Acquire
//   fills +0, 0x80d40.. dereference it); AcquireMemoryPool 0x80f10, ReleaseMemoryPool 0x80f78; AddDeviceSink 0x82bd0.
// WaveBuffer (0x30 bytes; AppendWaveBuffer 0x853a4..0x853e8 copies it field by field):
//   +0x00 void* buffer  +0x08 size_t size  +0x10 int startSampleOffset  +0x14 int endSampleOffset  +0x18 bool loop
//   +0x19 bool isEndOfStream  +0x20 void* pContext  +0x28 size_t contextSize. Offsets are in sample frames (main 0x200b694:
//   end = size >> (channels == 2 ? 2 : 1), start 0), data interleaved. At most 4 queued per voice (0x85374 `cmp w9,#4` -> false);
//   GetReleasedWaveBuffer 0x85420 returns the guest WaveBuffer pointer that was appended (0x853ec keeps it).
// SampleFormat: GetSampleByteSize 0x82af0 table 0x98a5a0 = {1,2,3,4,4} for 1..5 = PcmInt8, PcmInt16, PcmInt24, PcmInt32, PcmFloat.
// PlayState: AcquireVoiceSlot sets 1 (0x84b08 strb w26=1 at +0x4a); main: 0 = start (0x200b1ec), 2 = pause (0x200b290),
//   1 = stop (0x200b834) => Started 0, Stopped 1, Paused 2.
// SetVoiceMixVolume(voice, finalMix, volume, srcChannel, dstMixBuffer) 0x85194: voiceinfo[+0x1d0 + src*8] -> float[dst] (0x60
//   bytes = 24 destination buffers per source channel, cleared at acquire 0x84bb8); voice volume default 1.0 (0x84af8).
// AddDeviceSink returns Result 0 / 0xa99 (0x82be4); input count stored raw, at most 6 copied (0x82b80).
// AcquireAudioDeviceNotificationForInput 0x92290 returns Result and attaches the event with clear mode 1 (auto clear);
//   OpenAudioRenderer attaches the renderer event with clear mode 1 as well (0x8ade0 `mov w3,#1`).
#include "hle.h"
#ifdef DM_GAME_DOOM
#include <math.h>
#include <pthread.h>
// The SDK's own renderer (hle_audren.c, ported from nxnative) is the default; this host mixer is the fallback, built only with
// -DDM_DOOM_HOST_MIXER=1 (the two take over the same renderer entry points)
#ifdef DM_DOOM_HOST_MIXER

enum { AR_RATE = 48000, AR_MAXBUF = 24, AR_MAXCH = 6, AR_WBMAX = 4, AR_MAXFRAME = 4096 };
enum { PS_STARTED = 0, PS_STOPPED = 1, PS_PAUSED = 2 };
enum { SF_PCM8 = 1, SF_PCM16 = 2, SF_PCM24 = 3, SF_PCM32 = 4, SF_FLOAT = 5, SF_ADPCM = 6 };
#define AR_SINK_FAIL ((Result)0xa99u)   // what AddDeviceSink returns when no sink slot is free (0x82be4)
#define AR_MAGIC_CFG 0x41524346u
#define AR_MAGIC_VOICE 0x41525643u
#define AR_MAGIC_REN 0x4152524eu

typedef struct {   // guest WaveBuffer as seen at AppendWaveBuffer time
    const void *guest; const uint8_t *buf; int64_t start, end; int loop;
} hwbuf;

typedef struct hpool { struct hpool *next; void *addr; uint64_t size; int req, attached; } hpool;

typedef struct {
    uint32_t magic; int used, ch, rate, fmt, bps;
    // game side (written by the Set* calls, sent to the mixer at RequestUpdateAudioRenderer)
    int p_state, p_dest; float p_vol, p_mix[AR_MAXCH][AR_MAXBUF];
    hwbuf slot[AR_WBMAX]; uint32_t appended, returned, released;
    // mixer side (under g_lock)
    int state, dest; float vol, mix[AR_MAXCH][AR_MAXBUF], gprev[AR_MAXCH][AR_MAXBUF];
    uint32_t committed, consumed; int64_t pos; double frac; float prev[AR_MAXCH], cur[AR_MAXCH];
} hvoice;

typedef struct {
    uint32_t magic; int nvoice, max_mixbuf, max_sink;
    hvoice *voices;
    int fm_bufs;                       // final mix buffer count, 0 = none acquired
    int nsink, sink_n; int8_t sink_in[AR_MAXCH];
    hpool *pools;
} hcfg;

typedef struct {
    uint32_t magic; int n, mixbufs, och, started;
    void *event, *out;                 // guest SystemEvent; hle_audio source handle
    hcfg *cfg;                         // config of the latest RequestUpdate (read by the mixer under g_lock)
    float *mix, *vs; int16_t *obuf; unsigned opos;
    uint64_t frames;
    pthread_t nth; pthread_mutex_t nm; pthread_cond_t nc; int npend, nrun;
} hren;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   // voices / pools / config binding shared with the mixer

static hcfg *cfg_of(const void *config) {
    hcfg *c = *(hcfg *const *)config;
    if (!c || c->magic != AR_MAGIC_CFG) die("AudioRendererConfig %p 不是宿主建的 | not initialized by the host", config);
    return c;
}
static hvoice *voice_of(const void *voice) {
    hvoice *v = *(hvoice *const *)voice;
    if (!v || v->magic != AR_MAGIC_VOICE || !v->used) die("VoiceType %p 没有占用的声部 | holds no acquired voice", voice);
    return v;
}
static hren *ren_of(void *impl) {
    hren *r = impl;
    if (!r || r->magic != AR_MAGIC_REN) die("AudioRendererHandle %p 不是宿主建的 | not opened by the host", impl);
    return r;
}

// ---------- mixer ----------
// Next source frame of a voice into out[ch]; consumes finished wave buffers. 0 = queue empty (starved).
static int voice_fetch(hvoice *v, float *out) {
    while (v->consumed != v->committed) {
        hwbuf *b = &v->slot[v->consumed & (AR_WBMAX - 1)];
        int64_t len = b->end - b->start;
        if (v->pos >= len) { v->consumed++; v->pos = 0; continue; }   // empty / invalid buffer: released untouched
        const uint8_t *p = b->buf + (size_t)(b->start + v->pos) * (size_t)v->ch * (size_t)v->bps;
        for (int c = 0; c < v->ch; c++, p += v->bps)
            switch (v->fmt) {
            case SF_PCM8: out[c] = (float)(int8_t)p[0] * 256.0f; break;
            case SF_PCM16: { int16_t s; memcpy(&s, p, 2); out[c] = (float)s; break; }
            case SF_PCM24: out[c] = (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 256.0f; break;
            case SF_PCM32: { int32_t s; memcpy(&s, p, 4); out[c] = (float)s / 65536.0f; break; }
            default: { float s; memcpy(&s, p, 4); out[c] = s * 32768.0f; break; }
            }
        if (++v->pos >= len) { if (b->loop) v->pos = 0; else { v->consumed++; v->pos = 0; } }
        return 1;
    }
    return 0;
}

// Resample one voice (linear interpolation, source rate -> 48 kHz) into r->vs and add it to the mix buffers with the voice
// volume x mix volume ramped linearly across the frame (as the DSP does, no clicks on volume changes).
static void voice_render(hren *r, hvoice *v) {
    int n = r->n, nb = r->mixbufs;
    if (v->state != PS_STARTED || !v->dest) { memset(v->gprev, 0, sizeof v->gprev); return; }
    double step = (double)v->rate / AR_RATE;
    for (int i = 0; i < n; i++) {
        v->frac += step;
        while (v->frac >= 1.0) {
            v->frac -= 1.0;
            memcpy(v->prev, v->cur, sizeof v->prev);
            if (!voice_fetch(v, v->cur)) memset(v->cur, 0, sizeof v->cur);
        }
        float t = (float)v->frac;
        for (int c = 0; c < v->ch; c++) r->vs[c * n + i] = v->prev[c] + (v->cur[c] - v->prev[c]) * t;
    }
    for (int c = 0; c < v->ch; c++)
        for (int d = 0; d < nb; d++) {
            float g0 = v->gprev[c][d], g1 = v->vol * v->mix[c][d];
            v->gprev[c][d] = g1;
            if (g0 == 0.0f && g1 == 0.0f) continue;
            float dg = (g1 - g0) / (float)n, g = g0, *m = r->mix + d * n; const float *s = r->vs + c * n;
            for (int i = 0; i < n; i++) { g += dg; m[i] += s[i] * g; }
        }
}

static inline int16_t sat16(float x) { return x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (int16_t)lrintf(x); }

// One audio frame (sampleCount samples) of the renderer into r->obuf.
static void ren_frame(hren *r) {
    int n = r->n;
    memset(r->mix, 0, sizeof(float) * (size_t)AR_MAXBUF * (size_t)n);
    pthread_mutex_lock(&g_lock);
    hcfg *c = r->cfg;
    int nb = c && c->fm_bufs ? c->fm_bufs : 0;
    r->mixbufs = nb;
    if (nb) for (int i = 0; i < c->nvoice; i++) if (c->voices[i].used) voice_render(r, &c->voices[i]);
    int sn = c ? c->sink_n : 0; int8_t in[AR_MAXCH]; if (c) memcpy(in, c->sink_in, sizeof in);
    pthread_mutex_unlock(&g_lock);
    for (int j = 0; j < r->och; j++) {
        int b = j < sn ? in[j] : -1;
        const float *m = b >= 0 && b < nb ? r->mix + b * n : NULL;
        for (int i = 0; i < n; i++) r->obuf[i * r->och + j] = m ? sat16(m[i]) : 0;   // final mix volume is 1.0 (AcquireFinalMix 0x80d64); no setter imported
    }
    r->frames++;
}

static void ren_notify_kick(hren *r) {
    pthread_mutex_lock(&r->nm); r->npend = 1; pthread_cond_signal(&r->nc); pthread_mutex_unlock(&r->nm);
}
// Signals the renderer SystemEvent once per rendered frame, off the audio callback thread (hle_signal_system_event takes the
// kernel lock and wakes guest waiters; same split as hle_audio.c's notify thread).
static void *ren_notify(void *p) {
    hren *r = p;
    pthread_mutex_lock(&r->nm);
    for (;;) {
        while (!r->npend && r->nrun) pthread_cond_wait(&r->nc, &r->nm);
        if (!r->nrun) break;
        r->npend = 0;
        pthread_mutex_unlock(&r->nm);
        if (r->event) hle_signal_system_event(r->event);
        pthread_mutex_lock(&r->nm);
    }
    pthread_mutex_unlock(&r->nm);
    return NULL;
}

// hle_audio.c host-source callback: audio callback (or silent sink) thread.
static void ren_fill(void *ctx, int16_t *dst, unsigned frames) {
    hren *r = ctx;
    while (frames) {
        if (r->opos >= (unsigned)r->n) { ren_frame(r); r->opos = 0; ren_notify_kick(r); }
        unsigned k = (unsigned)r->n - r->opos; if (k > frames) k = frames;
        memcpy(dst, r->obuf + (size_t)r->opos * (size_t)r->och, (size_t)k * (size_t)r->och * 2);
        dst += (size_t)k * (size_t)r->och; frames -= k; r->opos += k;
    }
}

// ---------- renderer ----------
typedef struct { void *impl; void *event; } ar_handle;   // AudioRendererHandle, 16 bytes by value (x0:x1)

HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "audren 服务查询（IPC）；宿主混音不用工作缓冲 | audren service query (IPC); the host mixer needs no work buffer",
          uint64_t, h_ar_WorkSize, const uint8_t *param) { (void)param; return 0x1000; }

HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "连 audren 服务；宿主软件混音器 | audren service; host software mixer",
          Result, h_ar_Open, ar_handle *h, void *ev, const uint8_t *param, void *work, uint64_t work_size) {
    (void)work; (void)work_size;
    int rate = *(const int32_t *)(param + 0x00), n = *(const int32_t *)(param + 0x04), mixbufs = *(const int32_t *)(param + 0x08);
    int voices = *(const int32_t *)(param + 0x10), sinks = *(const int32_t *)(param + 0x14), dev = *(const int32_t *)(param + 0x2c), mode = *(const int32_t *)(param + 0x30);
    LOG("audio::OpenAudioRenderer rate %d, %d samples/frame, %d mix buffers, %d voices, %d sinks, device %d, mode %d", rate, n, mixbufs, voices, sinks, dev, mode);
    if (rate != AR_RATE || n < 1 || n > AR_MAXFRAME || mixbufs < 1 || mixbufs > AR_MAXBUF || voices < 0) {
        LOG("【红】AudioRenderer 参数不支持（只做 48 kHz、每帧 1..%d 样本、1..%d 混音缓冲）| unsupported renderer parameters", AR_MAXFRAME, AR_MAXBUF);
        return RES_FAIL;
    }
    if (mode == 1) LOG("【红】AudioRenderer 手动执行模式：宿主照样按声卡节奏自动渲染 | manual execution mode: the host still renders automatically");
    hren *r = calloc(1, sizeof *r);
    if (!r) die("AudioRenderer alloc");
    r->magic = AR_MAGIC_REN; r->n = n; r->och = 2; r->opos = (unsigned)n; r->event = ev;
    r->mix = calloc((size_t)AR_MAXBUF * (size_t)n, sizeof(float));
    r->vs = calloc((size_t)AR_MAXCH * (size_t)n, sizeof(float));
    r->obuf = calloc((size_t)AR_MAXCH * (size_t)n, sizeof(int16_t));
    if (!r->mix || !r->vs || !r->obuf) die("AudioRenderer buffers");
    if (ev) hle_bind_system_event(ev, 1);   // SDK attaches it with clear mode 1 (auto clear), 0x8ade0
    pthread_mutex_init(&r->nm, NULL); pthread_cond_init(&r->nc, NULL); r->nrun = 1;
    if (pthread_create(&r->nth, NULL, ren_notify, r)) die("AudioRenderer notify thread");
    h->impl = r; h->event = NULL;            // SDK: +0 service object, +8 = 0 (0x8adcc)
    return RES_OK;
}

HLE_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "开始渲染：开宿主输出 | start rendering: open the host output",
          Result, h_ar_Start, ar_handle h) {
    hren *r = ren_of(h.impl);
    if (r->started) return RES_OK;
    if (!r->out) {
        pthread_mutex_lock(&g_lock);
        int sn = r->cfg ? r->cfg->sink_n : 0;
        pthread_mutex_unlock(&g_lock);
        r->och = sn >= 1 && sn <= AR_MAXCH ? sn : 2;   // device sink input count = output channels (DOOM: 6 -> hle_audio downmixes 5.1)
        r->out = dm_audio_source_open(r->och, ren_fill, r, "AudioRenderer");
    }
    r->started = 1;
    dm_audio_source_start(r->out);
    LOG("audio::StartAudioRenderer → 宿主输出 %d 声道 | host output %d channels", r->och, r->och);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", "停止渲染 | stop rendering", void, h_ar_Stop, ar_handle h) {
    hren *r = ren_of(h.impl);
    if (!r->started) return;
    r->started = 0;
    dm_audio_source_stop(r->out);
}
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "关闭渲染器 | close the renderer", void, h_ar_Close, ar_handle h) {
    hren *r = ren_of(h.impl);
    h_ar_Stop(h);
    if (r->out) dm_audio_source_close(r->out);   // the mixer callback is gone after this
    pthread_mutex_lock(&r->nm); r->nrun = 0; pthread_cond_signal(&r->nc); pthread_mutex_unlock(&r->nm);
    pthread_join(r->nth, NULL);
    pthread_mutex_destroy(&r->nm); pthread_cond_destroy(&r->nc);
    LOG("audio::CloseAudioRenderer（渲染 %llu 帧 | %llu frames rendered）", (unsigned long long)r->frames, (unsigned long long)r->frames);
    r->magic = 0; free(r->mix); free(r->vs); free(r->obuf); free(r);
}

// Commit: what the real update packet carries. Voice parameters / play state / appended wave buffers / pool attach requests
// become visible to the mixer; the played wave buffer count comes back (GetReleasedWaveBuffer reads it).
HLE_FORCE("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE", "提交配置（audren IPC）| commit the config (audren IPC)",
          Result, h_ar_Update, ar_handle h, const void *config) {
    hren *r = ren_of(h.impl); hcfg *c = cfg_of(config);
    pthread_mutex_lock(&g_lock);
    r->cfg = c;
    for (int i = 0; i < c->nvoice; i++) {
        hvoice *v = &c->voices[i];
        if (!v->used) continue;
        v->committed = v->appended;
        if (v->p_state == PS_STOPPED && v->state != PS_STOPPED) {   // stop flushes every queued buffer and resets the decoder
            v->consumed = v->committed; v->pos = 0; v->frac = 0;
            memset(v->prev, 0, sizeof v->prev); memset(v->cur, 0, sizeof v->cur);
        }
        v->state = v->p_state; v->dest = v->p_dest; v->vol = v->p_vol;
        memcpy(v->mix, v->p_mix, sizeof v->mix);
        v->released = v->consumed;
    }
    for (hpool *p = c->pools; p; p = p->next) p->attached = p->req;
    pthread_mutex_unlock(&g_lock);
    return RES_OK;
}

// ---------- config ----------
HLE_FORCE("_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm", "渲染配置：宿主对象（只写前 8 字节）| renderer config: host object (first 8 bytes only)",
          void, h_ar_CfgInit, void *config, const uint8_t *param, void *buf, uint64_t size) {
    (void)buf; (void)size;
    hcfg *c = calloc(1, sizeof *c);
    if (!c) die("AudioRendererConfig alloc");
    c->magic = AR_MAGIC_CFG;
    c->nvoice = *(const int32_t *)(param + 0x10); if (c->nvoice < 0) c->nvoice = 0;
    c->max_mixbuf = *(const int32_t *)(param + 0x08); if (c->max_mixbuf > AR_MAXBUF) c->max_mixbuf = AR_MAXBUF;
    c->max_sink = *(const int32_t *)(param + 0x14);
    c->voices = calloc(c->nvoice ? (size_t)c->nvoice : 1, sizeof *c->voices);
    if (!c->voices) die("AudioRendererConfig voices");
    *(hcfg **)config = c;
}

HLE_FORCE("_ZN2nn5audio15AcquireFinalMixEPNS0_19AudioRendererConfigEPNS0_12FinalMixTypeEi", "最终混音 | final mix", int, h_ar_FinalMix, void *config, void *mix, int count) {
    hcfg *c = cfg_of(config);
    if (c->fm_bufs || count < 1 || count > c->max_mixbuf) { LOG("【红】AcquireFinalMix %d 失败 | failed", count); return 0; }
    pthread_mutex_lock(&g_lock); c->fm_bufs = count; pthread_mutex_unlock(&g_lock);
    *(hcfg **)mix = c;
    return 1;
}

HLE_FORCE("_ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc", "设备输出端：混音缓冲 → 声道 | device sink: mix buffers -> channels",
          Result, h_ar_Sink, void *config, void *sink, void *mix, const int8_t *input, int count, const char *name) {
    (void)mix;
    hcfg *c = cfg_of(config);
    if (c->nsink >= c->max_sink || c->nsink >= 1) return AR_SINK_FAIL;
    int n = count < AR_MAXCH ? count : AR_MAXCH; if (n < 0) n = 0;
    pthread_mutex_lock(&g_lock);
    memset(c->sink_in, -1, sizeof c->sink_in); memcpy(c->sink_in, input, (size_t)n); c->sink_n = n; c->nsink++;
    pthread_mutex_unlock(&g_lock);
    *(hcfg **)sink = c;
    LOG("audio::AddDeviceSink %s：%d 路输入 | %d inputs", name ? name : "?", count, count);
    return RES_OK;
}

// Memory pools are plain guest memory (shared address space): attach / detach only change state at the next update.
HLE_FORCE("_ZN2nn5audio17AcquireMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeEPvm", "内存池：就是客体内存 | memory pool: plain guest memory",
          int, h_ar_PoolAcq, void *config, void *pool, void *addr, uint64_t size) {
    hcfg *c = cfg_of(config);
    hpool *p = calloc(1, sizeof *p);
    if (!p) die("MemoryPool alloc");
    p->addr = addr; p->size = size;
    pthread_mutex_lock(&g_lock); p->next = c->pools; c->pools = p; pthread_mutex_unlock(&g_lock);
    *(hpool **)pool = p;
    return 1;
}
HLE_FORCE("_ZN2nn5audio17ReleaseMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeE", "释放内存池 | release memory pool", void, h_ar_PoolRel, void *config, void *pool) {
    hcfg *c = cfg_of(config); hpool *p = *(hpool **)pool;
    pthread_mutex_lock(&g_lock);
    for (hpool **pp = &c->pools; *pp; pp = &(*pp)->next) if (*pp == p) { *pp = p->next; free(p); break; }
    pthread_mutex_unlock(&g_lock);
    *(hpool **)pool = NULL;
}
HLE_FORCE("_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE", "挂接请求，下次提交生效 | attach request, effective at the next update",
          int, h_ar_PoolAtt, void *pool) { hpool *p = *(hpool **)pool; if (!p) return 0; p->req = 1; return 1; }
HLE_FORCE("_ZN2nn5audio23RequestDetachMemoryPoolEPNS0_14MemoryPoolTypeE", "摘除请求，下次提交生效 | detach request, effective at the next update",
          int, h_ar_PoolDet, void *pool) { hpool *p = *(hpool **)pool; if (!p) return 0; p->req = 0; return 1; }
HLE_FORCE("_ZN2nn5audio20IsMemoryPoolAttachedEPKNS0_14MemoryPoolTypeE", "挂接状态（游戏摘除后轮询到 false）| attach state (the game polls it until false after detach)",
          int, h_ar_PoolIs, const void *pool) { const hpool *p = *(hpool *const *)pool; return p && p->attached; }

// ---------- voices ----------
HLE_FORCE("_ZN2nn5audio16AcquireVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEiiNS0_12SampleFormatEiPKvm", "占用声部 | acquire a voice",
          int, h_ar_VoiceAcq, void *config, void *voice, int rate, int ch, int fmt, int prio, const void *param, uint64_t param_size) {
    (void)prio; (void)param; (void)param_size;
    hcfg *c = cfg_of(config);
    *(hvoice **)voice = NULL;
    static const int bps[] = {0, 1, 2, 3, 4, 4};
    if (fmt < SF_PCM8 || fmt > SF_FLOAT || ch < 1 || ch > AR_MAXCH || rate <= 0) {
        LOG("【红】AcquireVoiceSlot 不支持：格式 %d（%s）声道 %d 采样率 %d | unsupported", fmt, fmt == SF_ADPCM ? "ADPCM" : "?", ch, rate);
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    hvoice *v = NULL;
    for (int i = 0; i < c->nvoice; i++) if (!c->voices[i].used) { v = &c->voices[i]; break; }
    if (v) {
        memset(v, 0, sizeof *v);
        v->magic = AR_MAGIC_VOICE; v->ch = ch; v->rate = rate; v->fmt = fmt; v->bps = bps[fmt];
        v->p_state = v->state = PS_STOPPED; v->p_vol = v->vol = 1.0f;   // SDK defaults (0x84af8, 0x84b08)
        v->used = 1;
    }
    pthread_mutex_unlock(&g_lock);
    if (!v) { LOG("【红】AcquireVoiceSlot：%d 个声部已用完 | all voices in use", c->nvoice); return 0; }
    *(hvoice **)voice = v;
    return 1;
}
HLE_FORCE("_ZN2nn5audio16ReleaseVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeE", "释放声部 | release a voice", void, h_ar_VoiceRel, void *config, void *voice) {
    (void)cfg_of(config);
    hvoice *v = *(hvoice **)voice;
    if (!v) return;
    pthread_mutex_lock(&g_lock); v->used = 0; pthread_mutex_unlock(&g_lock);
    *(hvoice **)voice = NULL;
}
HLE_FORCE("_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_12FinalMixTypeE", "声部 → 最终混音 | voice -> final mix",
          void, h_ar_VoiceDest, void *config, void *voice, void *mix) { (void)config; (void)mix; voice_of(voice)->p_dest = 1; }
HLE_FORCE("_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE", "播放状态（下次提交生效）| play state (effective at the next update)",
          void, h_ar_VoiceState, void *voice, int state) {
    hvoice *v = voice_of(voice);
    if (state < PS_STARTED || state > PS_PAUSED) { LOG("【红】SetVoicePlayState %d 无效 | invalid", state); return; }
    v->p_state = state;
}
HLE_FORCE("_ZN2nn5audio14SetVoiceVolumeEPNS0_9VoiceTypeEf", "声部音量 | voice volume", void, h_ar_VoiceVol, void *voice, float vol) { voice_of(voice)->p_vol = vol; }
HLE_FORCE("_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_12FinalMixTypeEfii", "声部声道 → 混音缓冲的音量 | voice channel -> mix buffer volume",
          void, h_ar_VoiceMix, void *voice, void *mix, float vol, int src, int dst) {
    (void)mix;
    hvoice *v = voice_of(voice);
    if (src < 0 || src >= v->ch || dst < 0 || dst >= AR_MAXBUF) return;
    v->p_mix[src][dst] = vol;
}
HLE_FORCE("_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE", "追加波形缓冲（每声部最多 4 个）| append a wave buffer (at most 4 per voice)",
          int, h_ar_WaveAppend, void *voice, const uint8_t *wb) {
    hvoice *v = voice_of(voice);
    if (v->appended - v->returned >= AR_WBMAX) return 0;
    // The slot written here is never one the mixer reads (it plays [consumed, committed) and consumed >= returned).
    hwbuf *b = &v->slot[v->appended & (AR_WBMAX - 1)];
    const uint8_t *buf = *(const uint8_t *const *)(wb + 0x00); uint64_t size = *(const uint64_t *)(wb + 0x08);
    int64_t start = *(const int32_t *)(wb + 0x10), end = *(const int32_t *)(wb + 0x14), cap = (int64_t)(size / ((uint64_t)v->ch * (uint64_t)v->bps));
    if (start < 0) start = 0;
    if (end > cap) end = cap;
    if (!buf || end < start) end = start;   // nothing playable: released at the next mixed frame
    b->guest = wb; b->buf = buf; b->start = start; b->end = end; b->loop = wb[0x18] != 0;   // +0x19 isEndOfStream / +0x20 context: ADPCM only
    v->appended++;
    return 1;
}
HLE_FORCE("_ZN2nn5audio21GetReleasedWaveBufferEPNS0_9VoiceTypeE", "取已播完的波形缓冲（上次提交时的计数）| get a played wave buffer (count as of the last update)",
          const void *, h_ar_WaveReleased, void *voice) {
    hvoice *v = voice_of(voice);
    if (v->returned == v->released) return NULL;
    return v->slot[v->returned++ & (AR_WBMAX - 1)].guest;
}

#endif   // DM_DOOM_HOST_MIXER

// ---------- AudioOut ----------
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", "默认音频输出 → hle_audio.c 的 OpenAudioOut | default audio out -> hle_audio.c OpenAudioOut",
          Result, h_au_OpenDefault, void *out, void *ev, const void *param) { return dm_audio_open_out(out, ev, NULL, param); }

// ---------- AudioIn: no microphone ----------
HLE_FORCE("_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", "录音设备：没有 | audio inputs: none", int, h_ai_List, void *info, int n) { (void)info; (void)n; return 0; }
HLE_FORCE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPNS_2os11SystemEventEPKcRKNS0_16AudioInParameterE", "没有麦克风：打开失败 | no microphone: open fails",
          Result, h_ai_Open, void *in, void *ev, const char *name, const void *param) {
    (void)in; (void)ev; (void)param; LOG("audio::OpenAudioIn %s：没有麦克风 | no microphone", name ? name : "(default)"); return RES_FAIL;
}
HLE_FORCE("_ZN2nn5audio12StartAudioInEPNS0_7AudioInE", "录音：没有 | audio in: none", Result, h_ai_Start, void *in) { (void)in; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio11StopAudioInEPNS0_7AudioInE", "录音：没有 | audio in: none", void, h_ai_Stop, void *in) { (void)in; }
HLE_FORCE("_ZN2nn5audio12CloseAudioInEPNS0_7AudioInE", "录音：没有 | audio in: none", void, h_ai_Close, void *in) { (void)in; }
HLE_FORCE("_ZN2nn5audio19AppendAudioInBufferEPNS0_7AudioInEPNS0_13AudioInBufferE", "录音：没有 | audio in: none", int, h_ai_Append, void *in, void *b) { (void)in; (void)b; return 0; }
HLE_FORCE("_ZN2nn5audio24GetReleasedAudioInBufferEPNS0_7AudioInE", "录音：没有 | audio in: none", void *, h_ai_Released, void *in) { (void)in; return NULL; }
HLE_FORCE("_ZN2nn5audio38AcquireAudioDeviceNotificationForInputEPNS_2os11SystemEventE", "录音设备变化通知：绑定、永不触发 | input device notification: bound, never signaled",
          Result, h_ai_Notify, void *ev) { hle_bind_system_event(ev, 1); return RES_OK; }

#endif
