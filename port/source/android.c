// libandroid (AAssetManager) and libaaudio for FMOD.
//
// Assets are read straight out of the user's APK.
//
// AAudio: SDL2's Switch audio driver is a process-wide singleton (every
// device calls audrenInitialize, every close calls audrenExit), so one SDL
// device is opened once at startup and AAudio streams are mixed into it.
// FMOD opens and closes several streams while it initialises; those never
// touch the device.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"

// ------------------------------------------------------------------ AAsset

typedef struct AAsset {
    const ZipEntry *entry;
    uint64_t data_offset; // stored entries stream from the APK
    uint8_t *data;        // deflated entries are inflated in memory
    int64_t length, pos;
} AAsset;

static int g_asset_manager;

static void *AAssetManager_fromJava(JNIEnv *env, jobject am) { (void)env; (void)am; return &g_asset_manager; }

static AAsset *AAssetManager_open(void *mgr, const char *name, int mode)
{
    (void)mgr; (void)mode;
    char path[512];
    if (!strncmp(name, "file:///android_asset/", 22)) name += 22;
    snprintf(path, sizeof(path), "assets/%s", name);
    const ZipEntry *e = zip_find(g_apk, path);
    if (!e) {
        LOG("AAssetManager_open(%s): not found\n", name);
        return NULL;
    }
    AAsset *a = calloc(1, sizeof(*a));
    a->entry = e;
    a->length = e->size;
    if (e->method == 0) {
        a->data_offset = zip_data_offset(g_apk, e);
    } else {
        size_t n;
        a->data = zip_read(g_apk, e, &n);
        if (!a->data) { free(a); return NULL; }
    }
    return a;
}

static int AAsset_read(AAsset *a, void *buf, size_t count)
{
    if (a->pos >= a->length) return 0;
    if ((int64_t)count > a->length - a->pos) count = (size_t)(a->length - a->pos);
    size_t got;
    if (a->data) { memcpy(buf, a->data + a->pos, count); got = count; }
    else got = zip_pread(g_apk, buf, count, a->data_offset + a->pos);
    a->pos += got;
    return (int)got;
}

static int64_t AAsset_seek64(AAsset *a, int64_t off, int whence)
{
    int64_t np = whence == 0 ? off : whence == 1 ? a->pos + off : a->length + off;
    if (np < 0 || np > a->length) return -1;
    a->pos = np;
    return np;
}
static long AAsset_seek(AAsset *a, long off, int whence) { return (long)AAsset_seek64(a, off, whence); }
static long AAsset_getLength(AAsset *a) { return (long)a->length; }
static int64_t AAsset_getLength64(AAsset *a) { return a->length; }
static long AAsset_getRemainingLength(AAsset *a) { return (long)(a->length - a->pos); }
static int64_t AAsset_getRemainingLength64(AAsset *a) { return a->length - a->pos; }
static void AAsset_close(AAsset *a) { free(a->data); free(a); }
static const void *AAsset_getBuffer(AAsset *a)
{
    if (!a->data) {
        size_t n;
        a->data = zip_read(g_apk, a->entry, &n);
    }
    return a->data;
}
static int AAsset_isAllocated(AAsset *a) { return a->data != NULL; }

// ------------------------------------------------------------------ AAudio

typedef int32_t (*aaudio_data_cb)(void *stream, void *user, void *data, int32_t frames);
typedef void (*aaudio_error_cb)(void *stream, void *user, int32_t error);

typedef struct {
    int32_t direction, sample_rate, channels, format, perf_mode, frames_per_cb, buffer_capacity;
    aaudio_data_cb data_cb; void *data_user;
    aaudio_error_cb error_cb; void *error_user;
} AAudioBuilder;

typedef struct AAudioStreamImpl {
    AAudioBuilder cfg;
    volatile int32_t state;
    int32_t frame_bytes;
    int32_t buffer_size;
    int id;
    // ring buffer for AAudioStream_write
    uint8_t *ring;
    size_t ring_size, ring_r, ring_w;
    Mutex ring_lock;
    CondVar ring_cond;
    struct AAudioStreamImpl *next;
} AAudioStreamImpl;

enum { ST_OPEN = 2, ST_STARTING, ST_STARTED, ST_PAUSING, ST_PAUSED, ST_FLUSHING, ST_FLUSHED,
       ST_STOPPING, ST_STOPPED, ST_CLOSING, ST_CLOSED };
#define FMT_I16 1
#define FMT_FLOAT 2
#define AAUDIO_ERROR_UNAVAILABLE -889
#define OUT_RATE 48000
#define OUT_SAMPLES 512

static bool g_audio_ok;
static RMutex g_streams_lock;       // recursive: FMOD's callback may call AAudioStream_* back
static AAudioStreamImpl *g_streams;
static uint8_t *g_mix_tmp;
static int g_stream_ids;

static void mix_into(float *out, int frames, const AAudioStreamImpl *s, const void *src)
{
    int ch = s->cfg.channels;
    if (s->cfg.format == FMT_I16) {
        const int16_t *p = src;
        for (int i = 0; i < frames; i++) {
            float l = p[i * ch] / 32768.0f;
            float r = ch > 1 ? p[i * ch + 1] / 32768.0f : l;
            out[i * 2] += l;
            out[i * 2 + 1] += r;
        }
    } else {
        const float *p = src;
        for (int i = 0; i < frames; i++) {
            float l = p[i * ch];
            float r = ch > 1 ? p[i * ch + 1] : l;
            out[i * 2] += l;
            out[i * 2 + 1] += r;
        }
    }
}

static void mix_frames(float *out, int frames)
{
    memset(out, 0, (size_t)frames * 2 * sizeof(float));

    rmutexLock(&g_streams_lock);
    for (AAudioStreamImpl *s = g_streams; s; s = s->next) {
        if (s->state != ST_STARTED) continue;
        size_t need = (size_t)frames * s->frame_bytes;
        if (s->cfg.data_cb) {
            memset(g_mix_tmp, 0, need);
            int32_t r = s->cfg.data_cb(s, s->cfg.data_user, g_mix_tmp, frames);
            if (r != 0) { // AAUDIO_CALLBACK_RESULT_STOP
                LOG("AAudio stream %d: callback asked to stop\n", s->id);
                s->state = ST_STOPPED;
            }
            mix_into(out, frames, s, g_mix_tmp);
        } else {
            mutexLock(&s->ring_lock);
            size_t avail = s->ring_w - s->ring_r;
            size_t n = need < avail ? need : avail;
            n -= n % s->frame_bytes;
            for (size_t i = 0; i < n; i++) g_mix_tmp[i] = s->ring[(s->ring_r + i) % s->ring_size];
            s->ring_r += n;
            condvarWakeAll(&s->ring_cond);
            mutexUnlock(&s->ring_lock);
            if (n < need) memset(g_mix_tmp + n, 0, need - n);
            mix_into(out, frames, s, g_mix_tmp);
        }
    }
    rmutexUnlock(&g_streams_lock);
}

// ---- output: libnx audio renderer on a dedicated high-priority thread

#define OUT_BUFFERS 4

static AudioDriver g_drv;
static void *g_pool;
static AudioDriverWaveBuf g_wavebuf[OUT_BUFFERS];
static Thread g_audio_thread;
static volatile bool g_audio_run;
static float *g_mix_out;

static void audio_thread_main(void *arg)
{
    (void)arg;
    bionic_tls_setup_thread(); // this thread runs FMOD's mixer callback
    int16_t *pcm = g_pool;
    uint64_t underruns = 0;
    while (g_audio_run) {
        bool queued = false;
        int playing = 0;
        for (int i = 0; i < OUT_BUFFERS; i++) {
            AudioDriverWaveBuf *wb = &g_wavebuf[i];
            if (wb->state != AudioDriverWaveBufState_Free && wb->state != AudioDriverWaveBufState_Done) {
                playing++;
                continue;
            }
            mix_frames(g_mix_out, OUT_SAMPLES);
            int16_t *dst = pcm + (size_t)i * OUT_SAMPLES * 2;
            for (int k = 0; k < OUT_SAMPLES * 2; k++) {
                float v = g_mix_out[k];
                if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                dst[k] = (int16_t)(v * 32767.0f);
            }
            armDCacheFlush(dst, OUT_SAMPLES * 2 * sizeof(int16_t));
            audrvVoiceAddWaveBuf(&g_drv, 0, wb);
            queued = true;
        }
        if (queued && playing == 0 && ++underruns % 50 == 1)
            LOG("audio: underrun (%lu)\n", (unsigned long)underruns);
        if (!audrvVoiceIsPlaying(&g_drv, 0)) audrvVoiceStart(&g_drv, 0);
        audrvUpdate(&g_drv);
        audrenWaitFrame();
    }
}

void audio_init(void)
{
    static const AudioRendererConfig cfg = {
        .output_rate = AudioRendererOutputRate_48kHz,
        .num_voices = 4, .num_effects = 0, .num_sinks = 1,
        .num_mix_objs = 1, .num_mix_buffers = 2,
    };
    Result rc = audrenInitialize(&cfg);
    if (R_FAILED(rc)) { LOG("audrenInitialize failed: 0x%x\n", rc); return; }
    rc = audrvCreate(&g_drv, &cfg, 2);
    if (R_FAILED(rc)) { LOG("audrvCreate failed: 0x%x\n", rc); audrenExit(); return; }

    size_t buf_bytes = (size_t)OUT_SAMPLES * 2 * sizeof(int16_t);
    size_t pool_size = (buf_bytes * OUT_BUFFERS + AUDREN_MEMPOOL_ALIGNMENT - 1) & ~(size_t)(AUDREN_MEMPOOL_ALIGNMENT - 1);
    g_pool = aligned_alloc(AUDREN_MEMPOOL_ALIGNMENT, pool_size);
    memset(g_pool, 0, pool_size);
    armDCacheFlush(g_pool, pool_size);
    for (int i = 0; i < OUT_BUFFERS; i++) {
        g_wavebuf[i].data_raw = g_pool;
        g_wavebuf[i].size = pool_size;
        g_wavebuf[i].start_sample_offset = i * OUT_SAMPLES;
        g_wavebuf[i].end_sample_offset = (i + 1) * OUT_SAMPLES;
    }
    int mp = audrvMemPoolAdd(&g_drv, g_pool, pool_size);
    audrvMemPoolAttach(&g_drv, mp);
    static const u8 sink_channels[] = { 0, 1 };
    audrvDeviceSinkAdd(&g_drv, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels);
    audrvUpdate(&g_drv);
    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc)) { LOG("audrenStartAudioRenderer failed: 0x%x\n", rc); return; }
    audrvVoiceInit(&g_drv, 0, 2, PcmFormat_Int16, OUT_RATE);
    audrvVoiceSetDestinationMix(&g_drv, 0, AUDREN_FINAL_MIX_ID);
    audrvVoiceSetMixFactor(&g_drv, 0, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&g_drv, 0, 0.0f, 0, 1);
    audrvVoiceSetMixFactor(&g_drv, 0, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&g_drv, 0, 1.0f, 1, 1);
    audrvVoiceStart(&g_drv, 0);

    g_mix_tmp = malloc((size_t)OUT_SAMPLES * 2 * sizeof(float));
    g_mix_out = malloc((size_t)OUT_SAMPLES * 2 * sizeof(float));
    g_audio_ok = true;
    g_audio_run = true;

    // the last core the process owns, above the game's priority (0x2c)
    u64 mask = 0;
    svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    int core = -2;
    for (int c = 63; c > 0; c--) if ((mask >> c) & 1) { core = c; break; }
    rc = threadCreate(&g_audio_thread, audio_thread_main, NULL, NULL, 0x10000, 0x21, core);
    if (R_SUCCEEDED(rc)) threadStart(&g_audio_thread);
    LOG("audio: audren %d Hz, %d x %d frames, thread on core %d (0x%x)\n", OUT_RATE, OUT_BUFFERS,
        OUT_SAMPLES, core, rc);
}

void audio_shutdown(void)
{
    if (!g_audio_ok) return;
    g_audio_run = false;
    threadWaitForExit(&g_audio_thread);
    threadClose(&g_audio_thread);
    audrvVoiceStop(&g_drv, 0);
    audrvClose(&g_drv);
    audrenExit();
    g_audio_ok = false;
    LOG("audio: stopped\n");
}

static int32_t AAudio_createStreamBuilder(AAudioBuilder **out)
{
    AAudioBuilder *b = calloc(1, sizeof(*b));
    *out = b;
    return 0;
}

#define SETTER(name, field) static int32_t AAudioStreamBuilder_##name(AAudioBuilder *b, int32_t v) { b->field = v; return 0; }
SETTER(setDirection, direction)
SETTER(setSampleRate, sample_rate)
SETTER(setChannelCount, channels)
SETTER(setSamplesPerFrame, channels)
SETTER(setFormat, format)
SETTER(setPerformanceMode, perf_mode)
SETTER(setFramesPerDataCallback, frames_per_cb)
SETTER(setBufferCapacityInFrames, buffer_capacity)
static void AAudioStreamBuilder_ignore(AAudioBuilder *b, int32_t v) { (void)b; (void)v; }
static void AAudioStreamBuilder_setDataCallback(AAudioBuilder *b, aaudio_data_cb cb, void *u) { b->data_cb = cb; b->data_user = u; }
static void AAudioStreamBuilder_setErrorCallback(AAudioBuilder *b, aaudio_error_cb cb, void *u) { b->error_cb = cb; b->error_user = u; }
static int32_t AAudioStreamBuilder_delete(AAudioBuilder *b) { free(b); return 0; }

static int32_t AAudioStreamBuilder_openStream(AAudioBuilder *b, AAudioStreamImpl **out)
{
    if (b->direction == 1 || !g_audio_ok) // input (recording) is not supported
        return AAUDIO_ERROR_UNAVAILABLE;
    AAudioStreamImpl *s = calloc(1, sizeof(*s));
    s->cfg = *b;
    s->cfg.sample_rate = OUT_RATE; // the device's rate; FMOD reads it back
    if (s->cfg.channels != 1) s->cfg.channels = 2;
    if (s->cfg.format != FMT_I16) s->cfg.format = FMT_FLOAT;
    s->frame_bytes = (s->cfg.format == FMT_I16 ? 2 : 4) * s->cfg.channels;
    s->buffer_size = OUT_SAMPLES * 2;
    s->ring_size = (size_t)s->frame_bytes * 4096;
    s->ring = calloc(1, s->ring_size);
    mutexInit(&s->ring_lock);
    condvarInit(&s->ring_cond);
    s->state = ST_OPEN;
    rmutexLock(&g_streams_lock);
    s->id = ++g_stream_ids;
    s->next = g_streams;
    g_streams = s;
    rmutexUnlock(&g_streams_lock);
    LOG("AAudio stream %d opened: %d Hz, %d ch, format %d, callback %s\n", s->id,
        s->cfg.sample_rate, s->cfg.channels, s->cfg.format, s->cfg.data_cb ? "yes" : "no");
    *out = s;
    return 0;
}

static int32_t AAudioStream_requestStart(AAudioStreamImpl *s) { LOG("AAudio stream %d start\n", s->id); s->state = ST_STARTED; return 0; }
static int32_t AAudioStream_requestStop(AAudioStreamImpl *s)
{
    LOG("AAudio stream %d stop\n", s->id);
    rmutexLock(&g_streams_lock); // waits for a callback in progress
    s->state = ST_STOPPED;
    rmutexUnlock(&g_streams_lock);
    mutexLock(&s->ring_lock);
    condvarWakeAll(&s->ring_cond);
    mutexUnlock(&s->ring_lock);
    return 0;
}
static int32_t AAudioStream_requestPause(AAudioStreamImpl *s)
{
    rmutexLock(&g_streams_lock);
    s->state = ST_PAUSED;
    rmutexUnlock(&g_streams_lock);
    return 0;
}
static int32_t AAudioStream_requestFlush(AAudioStreamImpl *s)
{
    mutexLock(&s->ring_lock);
    s->ring_r = s->ring_w;
    condvarWakeAll(&s->ring_cond);
    mutexUnlock(&s->ring_lock);
    s->state = ST_FLUSHED;
    return 0;
}
static int32_t AAudioStream_close(AAudioStreamImpl *s)
{
    LOG("AAudio stream %d close\n", s->id);
    rmutexLock(&g_streams_lock);
    for (AAudioStreamImpl **pp = &g_streams; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    rmutexUnlock(&g_streams_lock);
    free(s->ring);
    free(s);
    return 0;
}
static int32_t AAudioStream_getState(AAudioStreamImpl *s) { return s->state; }
static int32_t AAudioStream_waitForStateChange(AAudioStreamImpl *s, int32_t in, int32_t *next, int64_t t)
{
    // States change synchronously here, so the transition has already happened.
    (void)in; (void)t;
    if (next) *next = s->state;
    return 0;
}
static int32_t AAudioStream_getSampleRate(AAudioStreamImpl *s) { return s->cfg.sample_rate; }
static int32_t AAudioStream_getChannelCount(AAudioStreamImpl *s) { return s->cfg.channels; }
static int32_t AAudioStream_getFormat(AAudioStreamImpl *s) { return s->cfg.format; }
static int32_t AAudioStream_getFramesPerBurst(AAudioStreamImpl *s) { (void)s; return OUT_SAMPLES; }
static int32_t AAudioStream_getFramesPerDataCallback(AAudioStreamImpl *s) { (void)s; return OUT_SAMPLES; }
static int32_t AAudioStream_getBufferSizeInFrames(AAudioStreamImpl *s) { return s->buffer_size; }
static int32_t AAudioStream_setBufferSizeInFrames(AAudioStreamImpl *s, int32_t n) { s->buffer_size = n; return n; }
static int32_t AAudioStream_getBufferCapacityInFrames(AAudioStreamImpl *s) { (void)s; return 4096; }
static int32_t AAudioStream_getDeviceId(AAudioStreamImpl *s) { (void)s; return 1; }
static int32_t AAudioStream_getXRunCount(AAudioStreamImpl *s) { (void)s; return 0; }
static int32_t AAudioStream_getPerformanceMode(AAudioStreamImpl *s) { return s->cfg.perf_mode; }
static int32_t AAudioStream_read(AAudioStreamImpl *s, void *b, int32_t n, int64_t t) { (void)s; (void)b; (void)n; (void)t; return 0; }

static int32_t AAudioStream_write(AAudioStreamImpl *s, const void *buf, int32_t frames, int64_t timeout_ns)
{
    size_t len = (size_t)frames * s->frame_bytes;
    const uint8_t *p = buf;
    uint64_t deadline = time_ns() + (timeout_ns > 0 ? (uint64_t)timeout_ns : 0);
    size_t done = 0;
    mutexLock(&s->ring_lock);
    while (done < len && s->state != ST_STOPPED) {
        size_t space = s->ring_size - (s->ring_w - s->ring_r);
        if (!space) {
            uint64_t now = time_ns();
            if (timeout_ns <= 0 || now >= deadline) break;
            uint64_t wait = deadline - now;
            if (wait > 20000000ULL) wait = 20000000ULL;
            condvarWaitTimeout(&s->ring_cond, &s->ring_lock, wait);
            continue;
        }
        size_t n = len - done < space ? len - done : space;
        for (size_t i = 0; i < n; i++) s->ring[(s->ring_w + i) % s->ring_size] = p[done + i];
        s->ring_w += n;
        done += n;
    }
    mutexUnlock(&s->ring_lock);
    return (int32_t)(done / s->frame_bytes);
}

static int64_t AAudioStream_getFramesWritten(AAudioStreamImpl *s) { return (int64_t)(s->ring_w / s->frame_bytes); }
static int64_t AAudioStream_getFramesRead(AAudioStreamImpl *s) { return (int64_t)(s->ring_r / s->frame_bytes); }
static const char *AAudio_convertResultToText(int32_t r) { (void)r; return "AAudio result"; }

// ------------------------------------------------------------------ lookup

typedef struct { const char *name; void *fn; } ASym;
#define E(n) { #n, (void *)n }
static const ASym g_android_syms[] = {
    E(AAssetManager_fromJava), E(AAssetManager_open), E(AAsset_read), E(AAsset_seek),
    E(AAsset_seek64), E(AAsset_getLength), E(AAsset_getLength64), E(AAsset_getRemainingLength),
    E(AAsset_getRemainingLength64), E(AAsset_close), E(AAsset_getBuffer), E(AAsset_isAllocated),
};
static const ASym g_aaudio_syms[] = {
    E(AAudio_createStreamBuilder), E(AAudioStreamBuilder_setDirection),
    E(AAudioStreamBuilder_setSampleRate), E(AAudioStreamBuilder_setChannelCount),
    E(AAudioStreamBuilder_setSamplesPerFrame), E(AAudioStreamBuilder_setFormat),
    E(AAudioStreamBuilder_setPerformanceMode), E(AAudioStreamBuilder_setFramesPerDataCallback),
    E(AAudioStreamBuilder_setBufferCapacityInFrames), E(AAudioStreamBuilder_setDataCallback),
    E(AAudioStreamBuilder_setErrorCallback), E(AAudioStreamBuilder_delete),
    E(AAudioStreamBuilder_openStream),
    { "AAudioStreamBuilder_setDeviceId", (void *)AAudioStreamBuilder_ignore },
    { "AAudioStreamBuilder_setUsage", (void *)AAudioStreamBuilder_ignore },
    { "AAudioStreamBuilder_setContentType", (void *)AAudioStreamBuilder_ignore },
    { "AAudioStreamBuilder_setInputPreset", (void *)AAudioStreamBuilder_ignore },
    { "AAudioStreamBuilder_setSharingMode", (void *)AAudioStreamBuilder_ignore },
    { "AAudioStreamBuilder_setAllowedCapturePolicy", (void *)AAudioStreamBuilder_ignore },
    { "AAudioStreamBuilder_setSessionId", (void *)AAudioStreamBuilder_ignore },
    E(AAudioStream_requestStart), E(AAudioStream_requestStop), E(AAudioStream_requestPause),
    E(AAudioStream_requestFlush), E(AAudioStream_close), E(AAudioStream_getState),
    E(AAudioStream_waitForStateChange), E(AAudioStream_getSampleRate),
    E(AAudioStream_getChannelCount), { "AAudioStream_getSamplesPerFrame", (void *)AAudioStream_getChannelCount },
    E(AAudioStream_getFormat), E(AAudioStream_getFramesPerBurst),
    E(AAudioStream_getFramesPerDataCallback), E(AAudioStream_getBufferSizeInFrames),
    E(AAudioStream_setBufferSizeInFrames), E(AAudioStream_getBufferCapacityInFrames),
    E(AAudioStream_getDeviceId), E(AAudioStream_getXRunCount), E(AAudioStream_getPerformanceMode),
    E(AAudioStream_read), E(AAudioStream_write), E(AAudioStream_getFramesWritten),
    E(AAudioStream_getFramesRead), E(AAudio_convertResultToText),
};

uintptr_t android_lookup(const char *lib, const char *name)
{
    if (!lib || !strcmp(lib, "libandroid.so"))
        for (size_t i = 0; i < sizeof(g_android_syms) / sizeof(g_android_syms[0]); i++)
            if (!strcmp(g_android_syms[i].name, name)) return (uintptr_t)g_android_syms[i].fn;
    if (!lib || !strcmp(lib, "libaaudio.so"))
        for (size_t i = 0; i < sizeof(g_aaudio_syms) / sizeof(g_aaudio_syms[0]); i++)
            if (!strcmp(g_aaudio_syms[i].name, name)) return (uintptr_t)g_aaudio_syms[i].fn;
    return 0;
}
