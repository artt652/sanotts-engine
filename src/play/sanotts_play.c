/*
 * sanotts_play — проигрыватель WAV для Android без Java и без пакетов:
 * AAudio (Android 8.1+), при неудаче — OpenSL ES (Android 2.3+). Обе
 * библиотеки системные (/system/lib*), подключаются через dlopen, поэтому
 * программа запускается и там, где одной из них нет.
 *
 *   sanotts_play [--api auto|aaudio|opensl] [--usage assistant|media|notification|alarm] [-q] FILE.wav
 *     --usage: канал громкости (AAudio, Android 9+); по умолчанию assistant, при отказе — media
 *   sanotts_play --info FILE.wav     — только формат и длительность
 *
 * Возвращает управление, когда звук доиграл. Коды выхода: 0 — сыграно,
 * 1 — аргументы, 2 — файл не читается / не PCM WAV, 3 — нет ни AAudio, ни
 * OpenSL ES, 4 — воспроизведение не удалось.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "SLES/OpenSLES.h"

static int quiet = 0;
#define LOG(...) do { if (!quiet) fprintf(stderr, "sanotts_play: " __VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- WAV */

typedef struct {
    int rate, channels;
    int16_t *pcm;        /* чередующиеся отсчёты */
    int64_t frames;
} wav_t;

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav_load(const char *path, wav_t *w)
{
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > 512L * 1024 * 1024) { fclose(f); LOG("bad file size\n"); return 0; }
    unsigned char *b = malloc((size_t)size);
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(b); LOG("read error\n"); return 0; }
    fclose(f);
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); LOG("not a WAV file\n"); return 0; }
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    const unsigned char *data = NULL;
    uint32_t dlen = 0;
    long o = 12;
    while (o + 8 <= size) {
        uint32_t len = rd32(b + o + 4);
        const unsigned char *c = b + o + 8;
        if (!memcmp(b + o, "fmt ", 4) && len >= 16 && o + 8 + 16 <= size) {
            fmt = rd16(c); ch = rd16(c + 2); rate = (int)rd32(c + 4); bits = rd16(c + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16(c + 24);   /* WAVE_FORMAT_EXTENSIBLE */
        } else if (!memcmp(b + o, "data", 4)) {
            data = c;
            dlen = (uint32_t)((long)len > size - (o + 8) ? size - (o + 8) : (long)len);
            break;
        }
        o += 8 + (long)len + (len & 1);
    }
    if (!data || ch < 1 || ch > 2 || rate < 4000 || rate > 192000) { free(b); LOG("unsupported WAV (need mono/stereo PCM)\n"); return 0; }
    int bps;
    if (fmt == 1 && bits == 16) bps = 2;
    else if (fmt == 1 && bits == 8) bps = 1;
    else if (fmt == 1 && bits == 24) bps = 3;
    else if (fmt == 1 && bits == 32) bps = 4;
    else if (fmt == 3 && bits == 32) bps = 4;
    else { free(b); LOG("unsupported sample format %d/%d bit\n", fmt, bits); return 0; }
    int64_t n = dlen / bps;                 /* отсчётов всего */
    n -= n % ch;
    w->pcm = malloc((size_t)(n ? n : 1) * sizeof(int16_t));
    if (!w->pcm) { free(b); return 0; }
    for (int64_t i = 0; i < n; i++) {
        const unsigned char *p = data + i * bps;
        int32_t v;
        if (bps == 2) v = (int16_t)rd16(p);
        else if (bps == 1) v = ((int)p[0] - 128) << 8;
        else if (bps == 3) v = (int16_t)(p[1] | p[2] << 8);
        else if (fmt == 3) {
            float fl; memcpy(&fl, p, 4);
            if (fl > 1.0f) fl = 1.0f; else if (fl < -1.0f) fl = -1.0f;
            v = (int32_t)(fl * 32767.0f);
        } else v = (int16_t)rd16(p + 2);
        w->pcm[i] = (int16_t)v;
    }
    free(b);
    w->rate = rate; w->channels = ch; w->frames = n / ch;
    return 1;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Системная библиотека: по имени, затем по полному пути; причины отказа — в журнал. */
static void *open_lib(const char *tag, const char *name)
{
#if defined(__LP64__)
    static const char *dirs[] = { "", "/system/lib64/", "/apex/com.android.media/lib64/", "/apex/com.android.runtime/lib64/" };
#else
    static const char *dirs[] = { "", "/system/lib/", "/apex/com.android.media/lib/", "/apex/com.android.runtime/lib/" };
#endif
    char path[256];
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s%s", dirs[i], name);
        void *h = dlopen(path, RTLD_NOW);
        if (h) {
            if (i) LOG("%s: loaded %s\n", tag, path);
            return h;
        }
        const char *e = dlerror();
        LOG("%s: dlopen %s: %s\n", tag, path, e ? e : "failed");
    }
    return NULL;
}

/* ---------------------------------------------------------------- AAudio */

typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef struct AAudioStreamStruct AAudioStream;
#define AAUDIO_DIRECTION_OUTPUT 0
#define AAUDIO_FORMAT_PCM_I16 1
#define AAUDIO_SHARING_MODE_SHARED 1
#define AAUDIO_PERFORMANCE_MODE_NONE 10
#define AAUDIO_CONTENT_TYPE_SPEECH 1

static int play_aaudio(const wav_t *w, int usage)
{
    void *h = open_lib("aaudio", "libaaudio.so");
    if (!h) { LOG("aaudio: libaaudio.so not available (Android 8+)\n"); return 3; }
#define SYM(R, n, A) R (*n) A = (R (*) A)dlsym(h, #n)
    SYM(int32_t, AAudio_createStreamBuilder, (AAudioStreamBuilder **));
    SYM(const char *, AAudio_convertResultToText, (int32_t));
    SYM(void, AAudioStreamBuilder_setDirection, (AAudioStreamBuilder *, int32_t));
    SYM(void, AAudioStreamBuilder_setSampleRate, (AAudioStreamBuilder *, int32_t));
    SYM(void, AAudioStreamBuilder_setChannelCount, (AAudioStreamBuilder *, int32_t));
    SYM(void, AAudioStreamBuilder_setFormat, (AAudioStreamBuilder *, int32_t));
    SYM(void, AAudioStreamBuilder_setSharingMode, (AAudioStreamBuilder *, int32_t));
    SYM(void, AAudioStreamBuilder_setPerformanceMode, (AAudioStreamBuilder *, int32_t));
    SYM(void, AAudioStreamBuilder_setUsage, (AAudioStreamBuilder *, int32_t));        /* API 28 */
    SYM(void, AAudioStreamBuilder_setContentType, (AAudioStreamBuilder *, int32_t));  /* API 28 */
    SYM(int32_t, AAudioStreamBuilder_openStream, (AAudioStreamBuilder *, AAudioStream **));
    SYM(int32_t, AAudioStreamBuilder_delete, (AAudioStreamBuilder *));
    SYM(int32_t, AAudioStream_requestStart, (AAudioStream *));
    SYM(int32_t, AAudioStream_requestStop, (AAudioStream *));
    SYM(int32_t, AAudioStream_close, (AAudioStream *));
    SYM(int32_t, AAudioStream_write, (AAudioStream *, const void *, int32_t, int64_t));
    SYM(int64_t, AAudioStream_getFramesRead, (AAudioStream *));
    SYM(int32_t, AAudioStream_getSampleRate, (AAudioStream *));
    SYM(int32_t, AAudioStream_getChannelCount, (AAudioStream *));
#undef SYM
    if (!AAudio_createStreamBuilder || !AAudioStreamBuilder_openStream || !AAudioStream_write ||
        !AAudioStream_requestStart || !AAudioStream_getFramesRead || !AAudioStream_close) {
        LOG("aaudio: incomplete library\n");
        return 3;
    }
#define ERR(r) (AAudio_convertResultToText ? AAudio_convertResultToText(r) : "error")
    AAudioStreamBuilder *bld = NULL;
    AAudioStream *s = NULL;
    int32_t r;
reopen:
    r = AAudio_createStreamBuilder(&bld);
    if (r != 0) { LOG("aaudio: createStreamBuilder: %s\n", ERR(r)); return 4; }
    AAudioStreamBuilder_setDirection(bld, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(bld, w->rate);
    AAudioStreamBuilder_setChannelCount(bld, w->channels);
    AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_NONE);
    if (AAudioStreamBuilder_setUsage) AAudioStreamBuilder_setUsage(bld, usage);
    if (AAudioStreamBuilder_setContentType) AAudioStreamBuilder_setContentType(bld, AAUDIO_CONTENT_TYPE_SPEECH);
    r = AAudioStreamBuilder_openStream(bld, &s);
    AAudioStreamBuilder_delete(bld);
    bld = NULL;
    if (r != 0 || !s) {
        LOG("aaudio: openStream (usage %d): %s\n", usage, ERR(r));
        /* Прошивка не приняла канал (например, «Ассистент») — ещё раз с «Медиа». */
        if (usage != 1 && AAudioStreamBuilder_setUsage) { usage = 1; s = NULL; goto reopen; }
        return 4;
    }
    if ((AAudioStream_getSampleRate && AAudioStream_getSampleRate(s) != w->rate) ||
        (AAudioStream_getChannelCount && AAudioStream_getChannelCount(s) != w->channels)) {
        LOG("aaudio: stream opened with another format\n");
        AAudioStream_close(s);
        return 4;
    }
    r = AAudioStream_requestStart(s);
    if (r != 0) { LOG("aaudio: requestStart: %s\n", ERR(r)); AAudioStream_close(s); return 4; }
    int64_t done = 0;
    const int32_t chunk = w->rate / 10;
    while (done < w->frames) {
        int32_t n = (int32_t)(w->frames - done < chunk ? w->frames - done : chunk);
        int32_t k = AAudioStream_write(s, w->pcm + done * w->channels, n, 2000000000LL);
        if (k < 0) { LOG("aaudio: write: %s\n", ERR(k)); AAudioStream_requestStop(s); AAudioStream_close(s); return 4; }
        if (k == 0) { LOG("aaudio: write timeout\n"); AAudioStream_requestStop(s); AAudioStream_close(s); return 4; }
        done += k;
    }
    /* Ждём, пока всё записанное прозвучит. */
    int64_t deadline = now_ms() + 3000;
    while (AAudioStream_getFramesRead(s) < w->frames && now_ms() < deadline) sleep_ms(20);
    sleep_ms(60);
    if (AAudioStream_requestStop) AAudioStream_requestStop(s);
    AAudioStream_close(s);
    LOG("aaudio: played %.2f s (usage %d)\n", (double)w->frames / w->rate, AAudioStreamBuilder_setUsage ? usage : 1);
    return 0;
#undef ERR
}

/* ---------------------------------------------------------------- OpenSL ES */

#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE ((SLuint32)0x800007BD)

static int play_opensl(const wav_t *w)
{
    void *h = open_lib("opensl", "libOpenSLES.so");
    if (!h) { LOG("opensl: libOpenSLES.so not available\n"); return 3; }
    SLresult (*create)(SLObjectItf *, SLuint32, const SLEngineOption *, SLuint32, const SLInterfaceID *, const SLboolean *) =
        (SLresult (*)(SLObjectItf *, SLuint32, const SLEngineOption *, SLuint32, const SLInterfaceID *, const SLboolean *))dlsym(h, "slCreateEngine");
    const SLInterfaceID *iidEngine = (const SLInterfaceID *)dlsym(h, "SL_IID_ENGINE");
    const SLInterfaceID *iidPlay = (const SLInterfaceID *)dlsym(h, "SL_IID_PLAY");
    const SLInterfaceID *iidBq = (const SLInterfaceID *)dlsym(h, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
    if (!create || !iidEngine || !iidPlay || !iidBq) { LOG("opensl: incomplete library\n"); return 3; }

    SLObjectItf eng = NULL, mix = NULL, pl = NULL;
    SLEngineItf e;
    SLPlayItf play;
    SLBufferQueueItf bq;   /* раскладка совпадает с SLAndroidSimpleBufferQueueItf */
    int rc = 4;
    SLresult r;
#define CK(x, what) do { r = (x); if (r != SL_RESULT_SUCCESS) { LOG("opensl: %s failed (%u)\n", what, (unsigned)r); goto out; } } while (0)
    CK(create(&eng, 0, NULL, 0, NULL, NULL), "slCreateEngine");
    CK((*eng)->Realize(eng, SL_BOOLEAN_FALSE), "engine Realize");
    CK((*eng)->GetInterface(eng, *iidEngine, &e), "engine GetInterface");
    CK((*e)->CreateOutputMix(e, &mix, 0, NULL, NULL), "CreateOutputMix");
    CK((*mix)->Realize(mix, SL_BOOLEAN_FALSE), "mix Realize");
    {
        SLDataLocator_BufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2 };
        SLDataFormat_PCM fmt = { SL_DATAFORMAT_PCM, (SLuint32)w->channels, (SLuint32)w->rate * 1000,
            SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
            w->channels == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT) : SL_SPEAKER_FRONT_CENTER,
            SL_BYTEORDER_LITTLEENDIAN };
        SLDataSource src = { &loc, &fmt };
        SLDataLocator_OutputMix out = { SL_DATALOCATOR_OUTPUTMIX, mix };
        SLDataSink snk = { &out, NULL };
        SLInterfaceID ids[1] = { *iidBq };
        SLboolean req[1] = { SL_BOOLEAN_TRUE };
        CK((*e)->CreateAudioPlayer(e, &pl, &src, &snk, 1, ids, req), "CreateAudioPlayer");
    }
    CK((*pl)->Realize(pl, SL_BOOLEAN_FALSE), "player Realize");
    CK((*pl)->GetInterface(pl, *iidPlay, &play), "GetInterface PLAY");
    CK((*pl)->GetInterface(pl, *iidBq, &bq), "GetInterface BUFFERQUEUE");
    CK((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING), "SetPlayState");
    {
        const int64_t chunk = w->rate / 4;          /* 250 мс на буфер */
        int64_t pos = 0;
        int64_t deadline = now_ms() + (int64_t)(w->frames * 1000 / w->rate) + 5000;
        for (;;) {
            SLBufferQueueState st;
            CK((*bq)->GetState(bq, &st), "GetState");
            if (pos < w->frames && st.count < 2) {
                int64_t n = w->frames - pos < chunk ? w->frames - pos : chunk;
                CK((*bq)->Enqueue(bq, w->pcm + pos * w->channels, (SLuint32)(n * w->channels * 2)), "Enqueue");
                pos += n;
                continue;
            }
            if (pos >= w->frames && st.count == 0) break;
            if (now_ms() > deadline) { LOG("opensl: playback timeout\n"); goto out; }
            sleep_ms(10);
        }
    }
    sleep_ms(150);   /* хвост в микшере */
    LOG("opensl: played %.2f s\n", (double)w->frames / w->rate);
    rc = 0;
out:
    if (pl) (*pl)->Destroy(pl);
    if (mix) (*mix)->Destroy(mix);
    if (eng) (*eng)->Destroy(eng);
    return rc;
#undef CK
}

/* ---------------------------------------------------------------- main */

static void usage_text(void)
{
    fprintf(stderr,
        "usage: sanotts_play [--api auto|aaudio|opensl] [--usage media|assistant|notification|alarm] [-q] FILE.wav\n"
        "       sanotts_play --info FILE.wav\n");
}

int main(int argc, char **argv)
{
    const char *api = "auto", *file = NULL;
    int usage = 16, info = 0;   /* AAUDIO_USAGE_ASSISTANT; где его нет — «Медиа» */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-q")) quiet = 1;
        else if (!strcmp(a, "--info")) info = 1;
        else if (!strcmp(a, "--api") && i + 1 < argc) api = argv[++i];
        else if (!strcmp(a, "--usage") && i + 1 < argc) {
            const char *u = argv[++i];
            if (!strcmp(u, "media")) usage = 1;
            else if (!strcmp(u, "alarm")) usage = 4;
            else if (!strcmp(u, "notification")) usage = 5;
            else if (!strcmp(u, "assistant")) usage = 16;
            else { usage_text(); return 1; }
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage_text(); return 0; }
        else if (a[0] == '-' && a[1]) { usage_text(); return 1; }
        else file = a;
    }
    if (!file || (strcmp(api, "auto") && strcmp(api, "aaudio") && strcmp(api, "opensl"))) { usage_text(); return 1; }
    wav_t w = { 0 };
    if (!wav_load(file, &w)) return 2;
    if (info) {
        printf("%d Hz, %d ch, %.2f s\n", w.rate, w.channels, (double)w.frames / w.rate);
        return 0;
    }
    if (w.frames == 0) return 0;
    int rc = 3;
    if (strcmp(api, "opensl")) {
        rc = play_aaudio(&w, usage);
        if (rc == 0 || !strcmp(api, "aaudio")) return rc;
    }
    int rc2 = play_opensl(&w);
    return rc2 == 3 && rc != 3 ? rc : rc2;
}
