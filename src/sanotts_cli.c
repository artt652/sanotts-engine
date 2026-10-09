/* sanotts_cli -- native text -> WAV for sanoTTS browser-format voices.
 *
 * Same C code the browser demo runs (snt_front_f32.c + snt_piperlite.c +
 * snt_g2p_wasm.c) with espeak-ng built in, instead of Emscripten.
 * No Python, no numpy, no system libespeak-ng.
 *
 *   sanotts_cli -d DATA -v VOICE_DIR -o out.wav [-s 1.0] [-p 0.15] [-l 1.0] [-t "текст"]
 *   sanotts_cli -d DATA --compile-dict DSOURCE_DIR [ru]  (build DATA/ru_dict from ru_rules/ru_list/ru_listx)
 *
 * espeak-ng data: -d, else $SANOTTS_ESPEAK_DATA, else <exe dir>/../share/espeak-ng-data
 * (exe dir from argv[0] or /proc/self/exe), else the system default.
 *     -v  voice dir: meta.json + front_{f16,f32}.bin + dec_{f16,f32}.bin (web/voices/<key>)
 *     -o  output WAV (16-bit mono);  "-" writes to stdout
 *     -O  PREFIX: streaming -- one WAV per input line (PREFIX000.wav, ...), each
 *         written as soon as it is ready and announced as "N PATH" on stdout
 *         ("N -" for a line with nothing to say); voice and espeak load once
 *     -s  tempo multiplier over the voice's own length_scale (>1 slower)
 *     -p  pause between sentences, seconds
 *     -l  leading silence, seconds
 *     -L  target loudness, LUFS (default -16; "off" = only peak normalisation)
 *     -d  espeak-ng data directory
 *     -t  text; read from stdin when omitted
 *     -P  pitch shift, semitones (-12..12); tempo stays: the model speaks
 *         slower by 2^(P/12) and the result is resampled back, as on the
 *         sanoTTS Russian web page
 *     -B  bass, dB (low shelf 250 Hz);  -T  treble, dB (high shelf 3500 Hz)
 *     -I  input lines are phoneme ids ("1 0 17 0 ... 2"), one utterance per
 *         line, as built by an external text frontend (see --g2p)
 *     -j  threads for the decoder (default: CPUs this process may use, at
 *         most 4; also $SANOTTS_THREADS); -j 1 = single-threaded. The result
 *         is the same bit for bit whatever the number of threads.
 *   sanotts_cli --g2p [espeak_voice slot]  (stdin line N -> "=N id id ..." on stdout)
 *   sanotts_cli --ids TEXT [espeak_voice slot]  (debug: ids of one text)
 * Exit code 0 on success; errors go to stderr.
 */
#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snt_par.h"
#include <espeak-ng/speak_lib.h>
#include <espeak-ng/espeak_ng.h>
#include <limits.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define PATH_SEP_CHARS "/\\"
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
static int abs_path(const char *in, char *out, size_t n) { return _fullpath(out, in, n) != NULL; }
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
/* main() gets arguments in the ANSI code page, which mangles UTF-8 text.
 * Text arguments (-t, --ids) are re-read from the UTF-16 command line and
 * converted to UTF-8; paths stay ANSI -- that is what fopen() expects. */
static void utf8_text_args(int argc, char **argv) {
    int wn = 0;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &wn);
    if (!w) return;
    if (wn == argc) {
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i - 1], "-t") && !(i == 2 && !strcmp(argv[1], "--ids"))) continue;
            int n = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
            char *s = n > 0 ? malloc((size_t)n) : NULL;
            if (s && WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s, n, NULL, NULL) > 0) argv[i] = s;
        }
    }
    LocalFree(w);
}
#else
#include <unistd.h>
#define PATH_SEP_CHARS "/"
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
static int abs_path(const char *in, char *out, size_t n) { (void)n; return realpath(in, out) != NULL; }
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Last path separator ('/' or, on Windows, '\\'). */
static char *last_sep(char *p) {
    char *r = NULL;
    for (; *p; p++) if (strchr(PATH_SEP_CHARS, *p)) r = p;
    return r;
}

int snt_g2p_init(void);
int snt_g2p_set_voice(const char *espeak_voice, int voice_slot);
int snt_g2p_text_to_ids(const char *text, int32_t *out_ids, int max_ids);
int snt_voice_synthesize(const uint8_t *front_blob, const uint8_t *dec_blob,
                         const int32_t *ids, int n_ids, float length_scale,
                         float *out, int out_cap);

#define MAX_IDS 4096
#define HOP 256

static void die(const char *msg, const char *arg) {
    fprintf(stderr, "sanotts_cli: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = 0;
    fclose(f);
    if (len) *len = (size_t)n;
    return buf;
}

static char *read_stdin(void) {
    size_t cap = 4096, n = 0;
    char *buf = (char *)malloc(cap);
    int c;
    while (buf && (c = getchar()) != EOF) {
        if (n + 1 >= cap) buf = (char *)realloc(buf, cap *= 2);
        if (buf) buf[n++] = (char)c;
    }
    if (buf) buf[n] = 0;
    return buf;
}

/* Tiny extractors for the flat keys we need from meta.json. */
static int json_str(const char *js, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return 0;
}

static double json_num(const char *js, const char *key, double dflt) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return dflt;
    p = strchr(p + strlen(pat), ':');
    return p ? atof(p + 1) : dflt;
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff, bits;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else { /* subnormal */
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) { man <<= 1; exp--; }
            man &= 0x3ff;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) bits = sign | 0x7f800000u | (man << 13);
    else bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* Blob = meta header (int32s) + weights. f16 bundles keep the header as-is
 * and store weights as halves; widen them into the f32 layout the runtime
 * expects. meta_off/ntens_off are the header's n_tensors position. */
static uint8_t *load_blob(const char *dir, const char *base, int ntens_off, int table_off) {
    char path[4096];
    size_t len;
    snprintf(path, sizeof path, "%s/%s_f32.bin", dir, base);
    char *raw = read_file(path, &len);
    if (raw) return (uint8_t *)raw;
    snprintf(path, sizeof path, "%s/%s_f16.bin", dir, base);
    raw = read_file(path, &len);
    if (!raw) die("cannot read voice blob", path);
    int32_t n_tensors;
    memcpy(&n_tensors, raw + ntens_off, 4);
    if (n_tensors <= 0 || n_tensors > 4096) die("bad blob header", path);
    size_t meta = (size_t)table_off + 8u * (size_t)n_tensors;
    size_t halves = (len - meta) / 2;
    uint8_t *out = (uint8_t *)malloc(meta + halves * 4);
    if (!out) die("out of memory", NULL);
    memcpy(out, raw, meta);
    const uint8_t *src = (const uint8_t *)raw + meta;
    float *dst = (float *)(void *)(out + meta);
    for (size_t i = 0; i < halves; i++) dst[i] = half_to_float((uint16_t)(src[2 * i] | (src[2 * i + 1] << 8)));
    free(raw);
    return out;
}

static void put_le(FILE *f, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) fputc((int)((v >> (8 * i)) & 0xff), f);
}

/* ---- Loudness (ITU-R BS.1770 / EBU R128) and a look-ahead limiter ---------
 *
 * Why here and not ffmpeg loudnorm: loudnorm's single pass needs ~3 s of audio
 * and is bound by its true-peak ceiling, so a short phrase ("Да.") came out
 * ~5 LU quieter than a long one. Here loudness is measured over speech only
 * (gated 400 ms blocks, so pauses and the lead-in do not count), the gain
 * brings it to the target, and a limiter -- not a static gain cut -- keeps
 * peaks at -1 dBFS, so short and long phrases end up equally loud. */

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } biquad;

static double bq_run(biquad *f, double x) {           /* transposed direct form II */
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* K-weighting for any sample rate (same design pyloudnorm uses). */
static void k_filters(int rate, biquad *shelf, biquad *hp) {
    const double PI = 3.14159265358979323846;
    double G = 3.99984385397, Q = 0.7071752369554193, fc = 1681.974450955533;
    double A = pow(10.0, G / 40.0), w = 2 * PI * fc / rate, c = cos(w), al = sin(w) / (2 * Q), sa = sqrt(A);
    double a0 = (A + 1) - (A - 1) * c + 2 * sa * al;
    shelf->b0 = A * ((A + 1) + (A - 1) * c + 2 * sa * al) / a0;
    shelf->b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
    shelf->b2 = A * ((A + 1) + (A - 1) * c - 2 * sa * al) / a0;
    shelf->a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
    shelf->a2 = ((A + 1) - (A - 1) * c - 2 * sa * al) / a0;
    Q = 0.5003270373238773; fc = 38.13547087602444;
    w = 2 * PI * fc / rate; c = cos(w); al = sin(w) / (2 * Q);
    a0 = 1 + al;
    hp->b0 = (1 + c) / 2 / a0; hp->b1 = -(1 + c) / a0; hp->b2 = (1 + c) / 2 / a0;
    hp->a1 = -2 * c / a0; hp->a2 = (1 - al) / a0;
    shelf->z1 = shelf->z2 = hp->z1 = hp->z2 = 0;
}

/* Integrated loudness in LUFS; -INFINITY for silence. */
static double loudness(const float *x, size_t n, int rate) {
    if (n == 0) return -INFINITY;
    biquad sh, hp;
    k_filters(rate, &sh, &hp);
    double *sq = (double *)malloc(n * sizeof(double));
    if (!sq) return -INFINITY;
    for (size_t i = 0; i < n; i++) { double y = bq_run(&hp, bq_run(&sh, x[i])); sq[i] = y * y; }
    size_t blk = (size_t)(0.4 * rate), step = (size_t)(0.1 * rate);
    if (n < blk) blk = n;                               /* very short phrase: one block */
    size_t nb = (n - blk) / step + 1;
    double *z = (double *)malloc(nb * sizeof(double));
    if (!z) { free(sq); return -INFINITY; }
    for (size_t b = 0; b < nb; b++) {
        double acc = 0;
        for (size_t i = b * step; i < b * step + blk; i++) acc += sq[i];
        z[b] = acc / blk;
    }
    free(sq);
    double sum = 0; size_t cnt = 0;                     /* absolute gate -70 LUFS */
    for (size_t b = 0; b < nb; b++) if (-0.691 + 10 * log10(z[b] + 1e-20) > -70) { sum += z[b]; cnt++; }
    if (!cnt) { free(z); return -INFINITY; }
    double rel = -0.691 + 10 * log10(sum / cnt) - 10;   /* relative gate -10 LU */
    sum = 0; cnt = 0;
    for (size_t b = 0; b < nb; b++) {
        double l = -0.691 + 10 * log10(z[b] + 1e-20);
        if (l > -70 && l > rel) { sum += z[b]; cnt++; }
    }
    free(z);
    return cnt ? -0.691 + 10 * log10(sum / cnt) : -INFINITY;
}

/* Look-ahead limiter: gain never exceeds what the upcoming 5 ms allow, is
 * ramped down over those 5 ms (no click) and recovers over ~80 ms.
 * Returns the deepest gain reduction in dB (<= 0). */
static double limit(float *x, size_t n, int rate, float ceiling) {
    size_t L = (size_t)(0.005 * rate); if (L < 1) L = 1;
    float *need = (float *)malloc(n * sizeof(float)), *m = (float *)malloc(n * sizeof(float));
    if (!need || !m) { free(need); free(m); return 0; }
    for (size_t i = 0; i < n; i++) { float a = fabsf(x[i]); need[i] = a > ceiling ? ceiling / a : 1.0f; }
    for (size_t i = 0; i < n; i++) {                    /* min over [i, i+L] */
        float g = 1.0f;
        for (size_t j = i; j <= i + L && j < n; j++) if (need[j] < g) g = need[j];
        m[i] = g;
    }
    /* Average of m over the last L samples: at a peak p every term is
     * <= need[p], so the smoothed gain still satisfies it -- but it moves
     * as a ramp, not a step. */
    double acc = 0, cur = 1.0, worst = 1.0, rel = 1.0 - exp(-1.0 / (0.08 * rate));
    for (size_t i = 0; i < n; i++) {
        acc += m[i];
        if (i >= L) acc -= m[i - L];
        double s = acc / (double)(i + 1 < L ? i + 1 : L);
        if (i + 1 < L) s = s < m[i] ? s : m[i];
        cur = s < cur ? s : cur + (s - cur) * rel;
        if (cur < worst) worst = cur;
        x[i] = (float)(x[i] * cur);
    }
    free(need); free(m);
    return 20 * log10(worst);
}

/* Split into sentences at . ! ? … followed by whitespace; long sentences are
 * cut at a comma/space so a chunk stays well under the model's token limit. */
/* Sentence enders. Byte length of the ender at p, 0 if none; *cjk is set for
 * full-width marks, which are not followed by a space in running text. */
static int ender_at(const char *p, int *cjk) {
    static const struct { const char *s; int cjk; } E[] = {
        {".", 0}, {"!", 0}, {"?", 0},
        {"\xE2\x80\xA6", 0},                            /* … */
        {"\xE0\xA5\xA4", 0}, {"\xE0\xA5\xA5", 0},        /* । ॥ (Devanagari) */
        {"\xD8\x9F", 0},                                 /* ؟ (Arabic) */
        {"\xE3\x80\x82", 1}, {"\xEF\xBC\x81", 1}, {"\xEF\xBC\x9F", 1}, /* 。！？ */
    };
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++) {
        size_t l = strlen(E[i].s);
        if (!strncmp(p, E[i].s, l)) { *cjk = E[i].cjk; return (int)l; }
    }
    return 0;
}

/* Split into sentences at the enders above (followed by whitespace, except
 * full-width CJK marks); long sentences are cut at a comma/space so a chunk
 * stays well under the model's token limit. */
static int next_chunk(const char **pp, char *out, size_t cap) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (!*p) { *pp = p; return 0; }
    const char *start = p, *end = NULL, *soft = NULL;
    while (*p) {
        int cjk = 0, w = ender_at(p, &cjk);
        if (w && (cjk || p[w] == ' ' || p[w] == '\n' || p[w] == '\r' || p[w] == 0)) { end = p + w; break; }
        if ((size_t)(p - start) > 300 && (*p == ',' || *p == ';')) { end = p + 1; break; }
        if ((size_t)(p - start) > 400 && *p == ' ') soft = p;
        if (soft) { end = soft; break; }
        p++;
    }
    if (!end) end = start + strlen(start);
    size_t n = (size_t)(end - start);
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = 0;
    *pp = end;
    return 1;
}

/* Bundled espeak-ng data next to the binary: <exe dir>/../share/espeak-ng-data.
 * The exe location comes from argv[0] when it is a path, else /proc/self/exe --
 * but not when that is the Android system linker: Termux on Android 10+ runs
 * programs as "/system/bin/linker64 /path/to/prog", and then /proc/self/exe
 * names the linker, not us. */
static int try_data_dir(const char *exe_path) {
    char buf[PATH_MAX], cand[PATH_MAX + 64], phontab[PATH_MAX + 80];
    if (!exe_path || !abs_path(exe_path, buf, sizeof buf)) return 0;
    char *slash = last_sep(buf);
    if (!slash) return 0;
    if (!strncmp(slash + 1, "linker", 6)) return 0;
    *slash = 0;
    snprintf(cand, sizeof cand, "%s/../share/espeak-ng-data", buf);
    snprintf(phontab, sizeof phontab, "%s/phontab", cand);
    struct stat st;
    if (stat(phontab, &st) != 0) return 0;
    set_env("SANOTTS_ESPEAK_DATA", cand);
    return 1;
}

static void use_bundled_espeak_data(const char *argv0) {
    if (getenv("SANOTTS_ESPEAK_DATA")) return;
    if (argv0 && strpbrk(argv0, PATH_SEP_CHARS) && try_data_dir(argv0)) return;
#ifdef _WIN32
    char exe[PATH_MAX];
    DWORD wn = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (wn > 0 && wn < sizeof exe) try_data_dir(exe);
#else
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    try_data_dir(exe);
#endif
}

static int compile_dict(const char *dsource, const char *name) {
    char src[PATH_MAX];
    size_t L = strlen(dsource);
    snprintf(src, sizeof src, "%s%s", dsource, (L && strchr(PATH_SEP_CHARS, dsource[L - 1])) ? "" : "/");
    espeak_ng_InitializePath(getenv("SANOTTS_ESPEAK_DATA"));
    espeak_ng_ERROR_CONTEXT ctx = NULL;
    if (espeak_ng_Initialize(&ctx) != ENS_OK) die("espeak-ng init failed", NULL);
    /* The compiler works against the current translator (phoneme table,
     * language options). With no voice loaded it dereferences NULL, so load
     * the target voice first; failing to open the not-yet-built <name>_dict
     * is expected here and harmless. */
    espeak_ng_SetVoiceByName(name);
    espeak_ng_STATUS st = espeak_ng_CompileDictionary(src, name, stderr, 0, &ctx);
    if (st != ENS_OK) {
        char msg[512];
        espeak_ng_GetStatusCodeMessage(st, msg, sizeof msg);
        fprintf(stderr, "sanotts_cli: compile %s failed: %s\n", name, msg);
        return 3;
    }
    fprintf(stderr, "sanotts_cli: compiled %s_dict into %s\n", name,
            getenv("SANOTTS_ESPEAK_DATA") ? getenv("SANOTTS_ESPEAK_DATA") : "(system espeak-ng data)");
    return 0;
}

typedef struct {
    const uint8_t *front, *dec;
    int32_t *ids;
    char *chunk;
    int ids_input;                                      /* -I: lines are ids, not text */
    float scale;
    int rate;
    size_t gap_n;
    double target;                                     /* LUFS; NAN = peak only */
    double pitch;                                      /* 2^(semitones/12); 1 = off */
    double bass, treble;                               /* shelf gains, dB */
    float *tmp;                                        /* scratch for resampling */
} synth_ctx;

/* RBJ shelf biquad (Audio EQ Cookbook, S = 1) -- the same filter as the Web
 * Audio BiquadFilterNode "lowshelf"/"highshelf" the web page uses. */
static void shelf(float *x, size_t n, int rate, double f0, double gain_db, int high) {
    if (fabs(gain_db) < 1e-3 || n == 0) return;
    double A = pow(10.0, gain_db / 40.0), w0 = 2.0 * M_PI * f0 / rate;
    double cs = cos(w0), sn = sin(w0), alpha = sn / 2.0 * sqrt(2.0), sa = 2.0 * sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * cs + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sa);
        a0 = (A + 1) + (A - 1) * cs + sa;       a1 = -2 * ((A - 1) + (A + 1) * cs);     a2 = (A + 1) + (A - 1) * cs - sa;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cs + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sa);
        a0 = (A + 1) - (A - 1) * cs + sa;       a1 = 2 * ((A - 1) - (A + 1) * cs);      a2 = (A + 1) - (A - 1) * cs - sa;
    }
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < n; i++) {
        double in = x[i], y = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in; y2 = y1; y1 = y;
        x[i] = (float)y;
    }
}

/* One synthesized chunk at x[0..n): pitch (resample by cx->pitch, linear
 * interpolation like a Web Audio playbackRate), then bass/treble. Each chunk
 * starts with fresh filters, as the web page processes sentence by sentence.
 * Returns the new length (at most cap). */
static size_t post_chunk(synth_ctx *cx, float *x, size_t n, size_t cap) {
    if (fabs(cx->pitch - 1.0) > 1e-6 && n > 1) {
        memcpy(cx->tmp, x, n * sizeof(float));
        size_t m = (size_t)ceil((double)n / cx->pitch);
        if (m > cap) m = cap;
        for (size_t i = 0; i < m; i++) {
            double pos = i * cx->pitch;
            size_t k = (size_t)pos;
            double fr = pos - (double)k;
            float a = k < n ? cx->tmp[k] : 0.0f, b = k + 1 < n ? cx->tmp[k + 1] : 0.0f;
            x[i] = (float)(a + (b - a) * fr);
        }
        n = m;
    }
    shelf(x, n, cx->rate, 250.0, cx->bass, 0);
    shelf(x, n, cx->rate, 3500.0, cx->treble, 1);
    return n;
}

/* Text -> PCM after lead_n samples of silence; sentences joined with gap_n.
 * Returns the number of chunks spoken (0 = nothing speakable), -1 on error. */
static int synth_text(synth_ctx *cx, const char *text, size_t lead_n, float *pcm, size_t cap, size_t *out_n) {
    size_t n = lead_n < cap ? lead_n : cap;
    memset(pcm, 0, n * sizeof(float));
    const char *cursor = text;
    int chunks = 0;
    if (cx->ids_input) {                               /* one line = one ready utterance */
        int n_ids = 0;
        char *end;
        const char *p = text;
        for (;;) {
            long v = strtol(p, &end, 10);
            if (end == p) break;
            if (n_ids < MAX_IDS) cx->ids[n_ids++] = (int32_t)v;
            p = end;
        }
        if (n_ids <= 3) { *out_n = n; return 0; }
        int got = snt_voice_synthesize(cx->front, cx->dec, cx->ids, n_ids, cx->scale, pcm + n, (int)(cap - n));
        if (got < 0) { fprintf(stderr, "sanotts_cli: synthesis error %d on ids\n", got); return -1; }
        *out_n = n + post_chunk(cx, pcm + n, (size_t)got, cap - n);
        return 1;
    }
    while (next_chunk(&cursor, cx->chunk, strlen(text) + 1)) {
        int n_ids = snt_g2p_text_to_ids(cx->chunk, cx->ids, MAX_IDS);
        if (n_ids <= 3) continue;                        /* nothing speakable */
        if (chunks && n + cx->gap_n < cap) { memset(pcm + n, 0, cx->gap_n * sizeof(float)); n += cx->gap_n; }
        int got = snt_voice_synthesize(cx->front, cx->dec, cx->ids, n_ids, cx->scale, pcm + n, (int)(cap - n));
        if (got < 0) { fprintf(stderr, "sanotts_cli: synthesis error %d on: %s\n", got, cx->chunk); return -1; }
        n += post_chunk(cx, pcm + n, (size_t)got, cap - n);
        chunks++;
    }
    *out_n = n;
    return chunks;
}

/* Loudness (or peak) normalisation of the speech after the lead-in, then WAV. */
static void finish_wav(synth_ctx *cx, float *pcm, size_t n, size_t lead_n, int chunks, const char *out_path) {
    int rate = cx->rate;
    double target = cx->target;
    /* Sample-peak ceiling -2 dBFS: the limiter sees samples, and inter-sample
     * peaks of this material run up to ~1 dB higher -- this keeps the true
     * peak (what a DAC and ffmpeg ebur128 see) at about -1 dBTP. */
    const float ceiling = 0.794f;
    float *speech = pcm + lead_n;                       /* the lead-in is silence, keep it out */
    size_t sn = n > lead_n ? n - lead_n : 0;
    float peak = 0.0f;
    for (size_t i = 0; i < sn; i++) if (fabsf(speech[i]) > peak) peak = fabsf(speech[i]);
    double before = loudness(speech, sn, rate), gain_db = 0, lim_db = 0;
    if (!isnan(target) && isfinite(before)) {
        /* Gain, limit, re-measure: the limiter takes some loudness back,
         * so a couple of rounds land within ~0.5 LU of the target. Total
         * gain is capped (do not blow up near-silence) and limiting depth
         * too (beyond ~10 dB it starts to pump audibly). */
        double now = before;
        for (int round = 0; round < 4; round++) {
            double step = target - now;
            if (fabs(step) < 0.3) break;
            if (gain_db + step > 30) step = 30 - gain_db;
            if (gain_db + step < -30) step = -30 - gain_db;
            if (step <= 0.05 && step >= -0.05) break;
            float g = (float)pow(10.0, step / 20.0);
            for (size_t i = 0; i < sn; i++) speech[i] *= g;
            gain_db += step;
            double d = limit(speech, sn, rate, ceiling);
            if (d < lim_db) lim_db = d;
            now = loudness(speech, sn, rate);
            if (lim_db < -10 && now < target) break;
        }
    } else if (peak > 1e-4f) {                          /* -L off: peak to -1 dBFS */
        float g = ceiling / peak;
        for (size_t i = 0; i < sn; i++) speech[i] *= g;
    }
    float gain = 1.0f;

    FILE *f = strcmp(out_path, "-") ? fopen(out_path, "wb") : stdout;
    if (!f) die("cannot write", out_path);
    uint32_t data = (uint32_t)(n * 2);
    fwrite("RIFF", 1, 4, f); put_le(f, 36 + data, 4); fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4); put_le(f, 1, 2); put_le(f, 1, 2); put_le(f, (uint32_t)rate, 4);
    put_le(f, (uint32_t)rate * 2, 4); put_le(f, 2, 2); put_le(f, 16, 2);
    fwrite("data", 1, 4, f); put_le(f, data, 4);
    for (size_t i = 0; i < n; i++) {
        float s = pcm[i] * gain;
        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
        put_le(f, (uint32_t)(uint16_t)(int16_t)lrintf(s * 32767.0f), 2);
    }
    if (f != stdout) fclose(f);
    if (!isnan(target) && isfinite(before))
        fprintf(stderr, "sanotts_cli: %.2fs at %d Hz, %d chunk(s), %d thread(s), loudness %.1f -> %.1f LUFS (gain %+.1f dB, limiter %.1f dB)\n",
                (double)n / rate, rate, chunks, snt_par_threads(), before, loudness(speech, sn, rate), gain_db, lim_db);
    else
        fprintf(stderr, "sanotts_cli: %.2fs at %d Hz, %d chunk(s), %d thread(s)\n", (double)n / rate, rate, chunks, snt_par_threads());
}

int main(int argc, char **argv) {
    const char *voice_dir = NULL, *out_path = NULL, *text = NULL, *stream_prefix = NULL;
#ifdef _WIN32
    utf8_text_args(argc, argv);
#endif
    /* -d DATADIR: espeak-ng data, anywhere on the command line and for every
     * mode. Preferred over the environment -- setting a variable for one
     * command differs between sh and cmd.exe, an argument does not. */
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "-d")) {
            set_env("SANOTTS_ESPEAK_DATA", argv[i + 1]);
            for (int j = i; j + 2 <= argc; j++) argv[j] = argv[j + 2];
            argc -= 2;
            break;
        }
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);             /* UTF-8 bytes as-is */
    _setmode(_fileno(stdout), _O_BINARY);            /* "-o -" writes WAV */
#endif
    use_bundled_espeak_data(argc > 0 ? argv[0] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "--compile-dict"))
        return compile_dict(argv[2], argc >= 4 ? argv[3] : "ru");
    if (argc >= 3 && !strcmp(argv[1], "--ids")) {        /* debug: --ids TEXT [espeak_voice slot] */
        int32_t dbg[MAX_IDS];
        const char *ev = argc >= 4 ? argv[3] : "ru";
        int sl = argc >= 5 ? atoi(argv[4]) : 10;
        if (snt_g2p_init() != 0 || snt_g2p_set_voice(ev, sl) != 0) die("espeak-ng init failed", NULL);
        int n = snt_g2p_text_to_ids(argv[2], dbg, MAX_IDS);
        for (int i = 0; i < n; i++) printf("%d ", dbg[i]);
        printf("\n");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--g2p")) {        /* batch: every stdin line -> its ids */
        const char *ev = argc >= 3 ? argv[2] : "ru";
        int sl = argc >= 4 ? atoi(argv[3]) : 10;
        if (snt_g2p_init() != 0 || snt_g2p_set_voice(ev, sl) != 0) die("espeak-ng init failed", NULL);
        char *in = read_stdin();
        if (!in) return 0;
        int32_t *buf = (int32_t *)malloc(MAX_IDS * sizeof(int32_t));
        char *line = in;
        int idx = 0;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            size_t len = strlen(line);
            if (len && line[len - 1] == '\r') line[len - 1] = 0;
            int n = *line ? snt_g2p_text_to_ids(line, buf, MAX_IDS) : 0;
            printf("=%d", idx++);                     /* "=N ids..." -- robust to stderr mixed in */
            for (int i = 0; i < n; i++) printf(" %d", buf[i]);
            printf("\n");
            line = nl ? nl + 1 : NULL;
        }
        fflush(stdout);
        return 0;
    }
    float tempo = 1.0f, pause = 0.15f, lead = 0.0f;
    int ids_input = 0, threads = 0;
    double semis = 0.0, bass = 0.0, treble = 0.0;
    double target = -16.0;                             /* LUFS; NAN = peak only */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "-v") && v) { voice_dir = v; i++; }
        else if (!strcmp(a, "-o") && v) { out_path = v; i++; }
        else if (!strcmp(a, "-O") && v) { stream_prefix = v; i++; }
        else if (!strcmp(a, "-t") && v) { text = v; i++; }
        else if (!strcmp(a, "-s") && v) { tempo = (float)atof(v); i++; }
        else if (!strcmp(a, "-p") && v) { pause = (float)atof(v); i++; }
        else if (!strcmp(a, "-l") && v) { lead = (float)atof(v); i++; }
        else if (!strcmp(a, "-I")) ids_input = 1;
        else if (!strcmp(a, "-P") && v) { semis = atof(v); i++; }
        else if (!strcmp(a, "-B") && v) { bass = atof(v); i++; }
        else if (!strcmp(a, "-T") && v) { treble = atof(v); i++; }
        else if (!strcmp(a, "-L") && v) { target = strcmp(v, "off") ? atof(v) : NAN; i++; }
        else if (!strcmp(a, "-j") && v) { threads = atoi(v); i++; }
        else die("usage: sanotts_cli -v VOICE_DIR (-o OUT.wav | -O PREFIX) [-s tempo] [-p pause] [-l lead] [-L lufs|off] [-P semitones] [-B db] [-T db] [-j threads] [-I] [-t text]", NULL);
    }
    if (!voice_dir || (!out_path && !stream_prefix)) die("-v and -o (or -O) are required", NULL);
    if (threads <= 0 && getenv("SANOTTS_THREADS")) threads = atoi(getenv("SANOTTS_THREADS"));
    snt_par_init(threads);
    if (tempo <= 0.0f) tempo = 1.0f;

    char meta_path[4096];
    snprintf(meta_path, sizeof meta_path, "%s/meta.json", voice_dir);
    char *meta = read_file(meta_path, NULL);
    if (!meta) die("cannot read", meta_path);
    char espeak_voice[64] = "ru";
    json_str(meta, "espeak_voice", espeak_voice, sizeof espeak_voice);
    int slot = (int)json_num(meta, "g2p_voice_slot", 10);
    if (semis < -12) semis = -12; else if (semis > 12) semis = 12;
    double pitch = pow(2.0, semis / 12.0);
    /* Pitch without a tempo change: the model speaks slower by the pitch
     * factor and post_chunk() plays it back faster by the same factor. */
    float scale = (float)(json_num(meta, "length_scale", 1.0) * tempo * pitch);
    int rate = (int)json_num(meta, "sample_rate", 22050);

    uint8_t *front = load_blob(voice_dir, "front", 68, 72);
    uint8_t *dec = load_blob(voice_dir, "dec", 44, 48);

    if (snt_g2p_init() != 0) die("espeak-ng init failed", NULL);
    if (snt_g2p_set_voice(espeak_voice, slot) != 0) die("espeak-ng voice not available", espeak_voice);

    char *input = text ? strdup(text) : read_stdin();
    if (!input) die("no text", NULL);

    size_t cap = (size_t)rate * 600;                    /* up to 10 min of audio */
    float *pcm = (float *)calloc(cap, sizeof(float));
    int32_t *ids = (int32_t *)malloc(MAX_IDS * sizeof(int32_t));
    char *chunk = (char *)malloc(strlen(input) + 1);
    if (!pcm || !ids || !chunk) die("out of memory", NULL);
    size_t lead_n = (size_t)(lead > 0 ? lead * rate : 0), gap_n = (size_t)(pause > 0 ? pause * rate : 0);
    float *tmp = (float *)malloc(cap * sizeof(float));
    if (!tmp) die("out of memory", NULL);
    synth_ctx cx = { front, dec, ids, chunk, ids_input, scale, rate, gap_n, target, pitch, bass, treble, tmp };

    if (stream_prefix) {
        /* -O PREFIX: one WAV per input line, written as soon as it is ready,
         * "N PATH" (or "N -" if the line has nothing speakable) on stdout.
         * The voice and espeak load once -- no per-sentence start-up. Line 0
         * gets the -l lead-in, the others -p (the pause between sentences). */
        char *line = input;
        int idx = 0, any = 0;
        while (line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            size_t n = 0;
            int chunks = synth_text(&cx, line, idx == 0 ? lead_n : gap_n, pcm, cap, &n);
            if (chunks < 0) return 2;
            if (chunks == 0) {
                printf("%d -\n", idx);
            } else {
                char path[4096], part[4200];
                snprintf(path, sizeof path, "%s%03d.wav", stream_prefix, idx);
                snprintf(part, sizeof part, "%s.part", path);
                finish_wav(&cx, pcm, n, idx == 0 ? lead_n : gap_n, chunks, part);
                remove(path);
                if (rename(part, path) != 0) die("cannot write", path);
                printf("%d %s\n", idx, path);
                any = 1;
            }
            fflush(stdout);
            idx++;
            line = nl ? nl + 1 : NULL;
        }
        if (!any) die("nothing to say", NULL);
        return 0;
    }

    size_t n = 0;
    int chunks = 0;
    if (ids_input) {                                   /* -I -o: the lines one after another */
        char *line = input;
        n = lead_n < cap ? lead_n : cap;
        memset(pcm, 0, n * sizeof(float));
        while (line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            size_t add = 0, start = n + (chunks && n + gap_n < cap ? gap_n : 0);
            int c = synth_text(&cx, line, 0, pcm + start, cap - start, &add);
            if (c < 0) return 2;
            if (c > 0) { memset(pcm + n, 0, (start - n) * sizeof(float)); n = start + add; chunks += c; }
            line = nl ? nl + 1 : NULL;
        }
    } else {
        chunks = synth_text(&cx, input, lead_n, pcm, cap, &n);
    }
    if (chunks < 0) return 2;
    if (!chunks) die("nothing to say", NULL);
    finish_wav(&cx, pcm, n, lead_n, chunks, out_path);
    return 0;
}
