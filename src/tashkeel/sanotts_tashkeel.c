/*
 * sanotts_tashkeel — расстановка огласовок (ташкиль) в арабском тексте перед
 * синтезом. Отдельная утилита: движок sanotts_cli она не трогает.
 *
 *   sanotts_tashkeel -m tashkeel-q16.bin  < текст  > текст с огласовками
 *   sanotts_tashkeel -m tashkeel-q16.bin -t "مرحبا بكم"
 *   sanotts_tashkeel -m tashkeel-q16.bin -o OUT.txt  < текст      (результат — в файл)
 *
 * Модель — libtashkeel (mush42, MIT), та же, что Piper запускает перед espeak
 * для арабских голосов; на её выходе учился голос sanoTTS "arabic". Это
 * построчный перенос на C браузерной реализации sanoTTS
 * (web/tashkeel/tashkeel.mjs, без ONNX): тот же порядок вычислений в double,
 * та же обработка текста (piper/tashkeel/__init__.py). Веса —
 * web/tashkeel/tashkeel-q16.bin (2,4 МБ), раскладка — tashkeel_model.h.
 *
 * Уже расставленные огласовки модель получает как подсказки и сохраняет их
 * смысл; неарабские символы проходят как есть.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#include "tashkeel_model.h"

#define D 56
#define N_HEADS 8
#define HEAD_DIM 7
#define N_LAYERS 3
#define EPS_LN 1e-5
#define EPS_BN 9.999999747378752e-6
#define CHAR_EMB_SCALE 16.0
#define DIAC_EMB_SCALE 16.0
#define CHAR_LIMIT 12000

static void die(const char *m, const char *a) {
    fprintf(stderr, "sanotts_tashkeel: %s%s%s\n", m, a ? ": " : "", a ? a : "");
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = calloc(n ? n : 1, 1);
    if (!p) die("out of memory", NULL);
    return p;
}

/* ------------------------------------------------------------------ веса */

static float *W[TK_N_TENSORS];

static int rd_i16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static float rd_f32(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void load_weights(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open model", path);
    uint8_t *buf = xmalloc(TK_BLOB_BYTES + 1);
    size_t n = fread(buf, 1, TK_BLOB_BYTES + 1, f);
    fclose(f);
    if (n != TK_BLOB_BYTES) die("unexpected model size (need tashkeel-q16.bin from sanoTTS)", path);
    for (int t = 0; t < TK_N_TENSORS; t++) {
        const tk_tensor *e = &TK_TENSORS[t];
        long cnt = (long)e->shape[0] * e->shape[1] * e->shape[2];
        float *o = xmalloc(sizeof(float) * cnt);
        if (!e->q16) {
            for (long i = 0; i < cnt; i++) o[i] = rd_f32(buf + e->off + 4 * i);
        } else {
            /* масштаб — форма веса со свёрнутыми (=1) осями квантования */
            int nd = e->nd, idx[3] = {0, 0, 0};
            long strides[3], acc = 1;
            for (int d = nd - 1; d >= 0; d--) {
                strides[d] = e->sshape[d] == 1 ? 0 : acc;
                acc *= e->sshape[d];
            }
            for (long i = 0; i < cnt; i++) {
                long s = 0;
                for (int d = 0; d < nd; d++) s += idx[d] * strides[d];
                o[i] = (float)((double)rd_i16(buf + e->off + 2 * i) * (double)rd_f32(buf + e->soff + 4 * s));
                for (int d = nd - 1; d >= 0; d--) {
                    if (++idx[d] < e->shape[d]) break;
                    idx[d] = 0;
                }
            }
        }
        W[t] = o;
    }
    free(buf);
}

static const float *get(const char *name) {
    for (int t = 0; t < TK_N_TENSORS; t++)
        if (!strcmp(TK_TENSORS[t].name, name)) return W[t];
    die("missing tensor", name);
    return NULL;
}

static const float *getL(int L, const char *suffix) {
    char nm[48];
    snprintf(nm, sizeof nm, "L%d_%s", L, suffix);
    return get(nm);
}

/* ------------------------------------------------------------ вычисления */

static void layernorm(const double *x, int t, int d, const float *w, const float *b, double *out) {
    for (int i = 0; i < t; i++) {
        int o = i * d;
        double mean = 0;
        for (int j = 0; j < d; j++) mean += x[o + j];
        mean /= d;
        double v = 0;
        for (int j = 0; j < d; j++) {
            double e = x[o + j] - mean;
            v += e * e;
        }
        double inv = 1 / sqrt(v / d + EPS_LN);
        for (int j = 0; j < d; j++) out[o + j] = (x[o + j] - mean) * inv * w[j] + b[j];
    }
}

/* out(t,n) = x(t,m) @ W(m,n) + bias(n) */
static void matmul(const double *x, int t, int m, const float *Wm, int n, const float *bias, double *out) {
    for (int i = 0; i < t; i++) {
        int xo = i * m, oo = i * n;
        if (bias) for (int j = 0; j < n; j++) out[oo + j] = bias[j];
        else for (int j = 0; j < n; j++) out[oo + j] = 0;
        for (int k = 0; k < m; k++) {
            double xv = x[xo + k];
            if (xv == 0) continue;
            int wo = k * n;
            for (int j = 0; j < n; j++) out[oo + j] += xv * Wm[wo + j];
        }
    }
}

static double sigm(double v) { return 1 / (1 + exp(-v)); }

static void silu(double *a, long n) {
    for (long i = 0; i < n; i++) a[i] *= sigm(a[i]);
}

/* свёртка с 'same'-дополнением; x (t, cin), W (cout, cin, k) */
static void conv1d(const double *x, int t, int cin, const float *Wc, int cout, int k, const float *bias, double *out) {
    int pad = (k - 1) >> 1;
    for (int i = 0; i < t; i++) {
        int oo = i * cout;
        for (int o = 0; o < cout; o++) {
            double s = bias[o];
            int wo = o * cin * k;
            for (int j = 0; j < k; j++) {
                int p = i + j - pad;
                if (p < 0 || p >= t) continue;
                int xo = p * cin;
                for (int c = 0; c < cin; c++) s += x[xo + c] * Wc[wo + c * k + j];
            }
            out[oo + o] = s;
        }
    }
}

/* двунаправленный GRU ONNX (linear_before_reset=1); out (t, 2h) = [вперёд | назад] */
static void bigru(const double *x, int t, int cin, const float *Wg, const float *R, const float *B, int h, double *out) {
    int h3 = 3 * h;
    double *gates = xmalloc(sizeof(double) * (size_t)t * h3);
    double *state = xmalloc(sizeof(double) * h);
    for (int d = 0; d < 2; d++) {
        int Wo = d * h3 * cin, Ro = d * h3 * h, Bo = d * 6 * h;
        for (int i = 0; i < t; i++) {
            int xo = i * cin, go = i * h3;
            for (int r = 0; r < h3; r++) {
                double s = (double)B[Bo + r] + (r < 2 * h ? (double)B[Bo + 3 * h + r] : 0);
                int wr = Wo + r * cin;
                for (int c = 0; c < cin; c++) s += x[xo + c] * Wg[wr + c];
                gates[go + r] = s;
            }
        }
        for (int j = 0; j < h; j++) state[j] = 0;
        for (int n = 0; n < t; n++) {
            int i = d == 0 ? n : t - 1 - n;
            int go = i * h3;
            for (int j = 0; j < h; j++) {
                double z = gates[go + j], r = gates[go + h + j];
                int rz = Ro + j * h, rr = Ro + (h + j) * h;
                for (int c = 0; c < h; c++) {
                    z += state[c] * R[rz + c];
                    r += state[c] * R[rr + c];
                }
                out[i * 2 * h + d * h + j] = sigm(z);
                gates[go + h + j] = sigm(r);
            }
            for (int j = 0; j < h; j++) {
                double hh = 0;
                int rh = Ro + (2 * h + j) * h;
                for (int c = 0; c < h; c++) hh += state[c] * R[rh + c];
                hh = (hh + B[Bo + 5 * h + j]) * gates[go + h + j] + gates[go + 2 * h + j];
                double z = out[i * 2 * h + d * h + j];
                out[i * 2 * h + d * h + j] = (1 - z) * tanh(hh) + z * state[j];
            }
            for (int j = 0; j < h; j++) state[j] = out[i * 2 * h + d * h + j];
        }
    }
    free(gates);
    free(state);
}

static void predict(const int *charIds, const int *diacIds, int t, unsigned char *preds) {
    const float *charEmb = get("char_emb"), *diacEmb = get("diac_emb");
    double hintScale = (double)get("hint_scale")[0] * DIAC_EMB_SCALE;
    size_t T = (size_t)t;
    double *x = xmalloc(sizeof(double) * T * D);
    for (int i = 0; i < t; i++) {
        int co = charIds[i] * D, dof = diacIds[i] * D;
        for (int j = 0; j < D; j++) x[i * D + j] = charEmb[co + j] * CHAR_EMB_SCALE + diacEmb[dof + j] * hintScale;
    }
    double *buf = xmalloc(sizeof(double) * T * 256), *buf2 = xmalloc(sizeof(double) * T * 128);
    matmul(x, t, D, get("dense0_w"), 256, get("dense0_b"), buf);
    matmul(buf, t, 256, get("dense1_w"), 128, get("dense1_b"), buf2);
    matmul(buf2, t, 128, get("dense2_w"), D, get("dense2_b"), x);
    free(buf);
    free(buf2);

    double *y = xmalloc(sizeof(double) * T * D);
    for (int c = 0; c < 4; c++) {
        char nw[16], nb[16];
        snprintf(nw, sizeof nw, "conv%d_w", c);
        snprintf(nb, sizeof nb, "conv%d_b", c);
        conv1d(x, t, D, get(nw), D, 2 * c + 1, get(nb), y);
        double *tmp = x; x = y; y = tmp;
    }
    free(y);

    double *residual = xmalloc(sizeof(double) * T * D);
    memcpy(residual, x, sizeof(double) * T * D);
    double posScale = get("pos_scale")[0];
    double *pos = xmalloc(sizeof(double) * T * D);
    for (int i = 0; i < t; i++)
        for (int j = 0; j < 28; j++) {
            double ang = i * pow(10000, -j / 28.0);
            pos[i * D + j] = sin(ang) * posScale;
            pos[i * D + 28 + j] = cos(ang) * posScale;
        }

    double *h = x;
    double *tmpD = xmalloc(sizeof(double) * T * D), *tmp224 = xmalloc(sizeof(double) * T * 224);
    double *g1 = xmalloc(sizeof(double) * T * 224), *g2 = xmalloc(sizeof(double) * T * 224);
    double *gs = xmalloc(sizeof(double) * T * 112);
    double *q = xmalloc(sizeof(double) * T * D), *kk = xmalloc(sizeof(double) * T * D), *vv = xmalloc(sizeof(double) * T * D);
    double *att = xmalloc(sizeof(double) * T), *ao = xmalloc(sizeof(double) * T * D);
    double s = sqrt(1 / sqrt((double)HEAD_DIM));
    long tD = (long)t * D;

    for (int L = 0; L < N_LAYERS; L++) {
        layernorm(h, t, D, getL(L, "ffm1_ln_w"), getL(L, "ffm1_ln_b"), tmpD);
        matmul(tmpD, t, D, getL(L, "ffm1_w1"), 224, getL(L, "ffm1_b1"), tmp224);
        silu(tmp224, (long)t * 224);
        matmul(tmp224, t, 224, getL(L, "ffm1_w2"), D, getL(L, "ffm1_b2"), tmpD);
        for (long i = 0; i < tD; i++) h[i] += 0.5 * tmpD[i];

        layernorm(h, t, D, getL(L, "attn_ln_w"), getL(L, "attn_ln_b"), tmpD);
        for (long i = 0; i < tD; i++) tmpD[i] += pos[i];
        matmul(tmpD, t, D, getL(L, "attn_q"), D, NULL, q);
        matmul(tmpD, t, D, getL(L, "attn_k"), D, NULL, kk);
        matmul(tmpD, t, D, getL(L, "attn_v"), D, NULL, vv);
        for (long i = 0; i < tD; i++) ao[i] = 0;
        for (int hd = 0; hd < N_HEADS; hd++) {
            int off = hd * HEAD_DIM;
            for (int i = 0; i < t; i++) {
                double mx = -INFINITY;
                for (int j = 0; j < t; j++) {
                    double dot = 0;
                    for (int c = 0; c < HEAD_DIM; c++) dot += q[i * D + off + c] * s * (kk[j * D + off + c] * s);
                    att[j] = dot;
                    if (dot > mx) mx = dot;
                }
                double sum = 0;
                for (int j = 0; j < t; j++) {
                    att[j] = exp(att[j] - mx);
                    sum += att[j];
                }
                for (int j = 0; j < t; j++) {
                    double a = att[j] / sum;
                    for (int c = 0; c < HEAD_DIM; c++) ao[i * D + off + c] += a * vv[j * D + off + c];
                }
            }
        }
        matmul(ao, t, D, getL(L, "attn_o"), D, NULL, tmpD);
        for (long i = 0; i < tD; i++) h[i] += tmpD[i];

        layernorm(h, t, D, getL(L, "ccm_ln_w"), getL(L, "ccm_ln_b"), tmpD);
        bigru(tmpD, t, D, getL(L, "gru1_W"), getL(L, "gru1_R"), getL(L, "gru1_B"), 112, g1);
        for (int i = 0; i < t; i++)
            for (int j = 0; j < 112; j++) gs[i * 112 + j] = tanh(g1[i * 224 + j] + g1[i * 224 + 112 + j]);
        for (int i = 0; i < t; i++)
            for (int j = 0; j < D; j++) tmpD[i * D + j] = gs[i * 112 + j] * sigm(gs[i * 112 + D + j]);
        bigru(tmpD, t, D, getL(L, "gru2_W"), getL(L, "gru2_R"), getL(L, "gru2_B"), 112, g2);
        const float *bw = getL(L, "bn_w"), *bb = getL(L, "bn_b"), *bm = getL(L, "bn_m"), *bv = getL(L, "bn_v");
        for (int i = 0; i < t; i++)
            for (int j = 0; j < 112; j++) {
                double v = tanh(g2[i * 224 + j] + g2[i * 224 + 112 + j]);
                v = (v - bm[j]) / sqrt(bv[j] + EPS_BN) * bw[j] + bb[j];
                gs[i * 112 + j] = v * sigm(v);
            }
        bigru(gs, t, 112, getL(L, "gru3_W"), getL(L, "gru3_R"), getL(L, "gru3_B"), D, g1);
        for (int i = 0; i < t; i++)
            for (int j = 0; j < D; j++) h[i * D + j] += tanh(g1[i * 2 * D + j] + g1[i * 2 * D + D + j]);

        layernorm(h, t, D, getL(L, "ffm2_ln_w"), getL(L, "ffm2_ln_b"), tmpD);
        matmul(tmpD, t, D, getL(L, "ffm2_w1"), 224, getL(L, "ffm2_b1"), tmp224);
        silu(tmp224, (long)t * 224);
        matmul(tmp224, t, 224, getL(L, "ffm2_w2"), D, getL(L, "ffm2_b2"), tmpD);
        for (long i = 0; i < tD; i++) h[i] += 0.5 * tmpD[i];

        layernorm(h, t, D, getL(L, "post_w"), getL(L, "post_b"), tmpD);
        for (long i = 0; i < tD; i++) h[i] = tanh(tmpD[i]);
    }

    for (long i = 0; i < tD; i++) h[i] += residual[i];
    layernorm(h, t, D, get("res_ln_w"), get("res_ln_b"), tmpD);
    double *logits = xmalloc(sizeof(double) * T * 15);
    matmul(tmpD, t, D, get("fc_w"), 15, get("fc_b"), logits);
    for (int i = 0; i < t; i++) {
        int best = 0;
        double bvv = -INFINITY;
        for (int j = 0; j < 15; j++)
            if (logits[i * 15 + j] > bvv) { bvv = logits[i * 15 + j]; best = j; }
        preds[i] = (unsigned char)best;
    }
    free(logits); free(residual); free(pos); free(tmpD); free(tmp224); free(g1); free(g2);
    free(gs); free(q); free(kk); free(vv); free(att); free(ao); free(h);
}

/* --------------------------------------------------------------- текст */

typedef struct { unsigned *c; int n, cap; } ustr;

static void us_push(ustr *s, unsigned c) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->c = realloc(s->c, sizeof(unsigned) * s->cap);
        if (!s->c) die("out of memory", NULL);
    }
    s->c[s->n++] = c;
}

static ustr utf8_decode(const char *p, size_t len) {
    ustr s = {0};
    const unsigned char *u = (const unsigned char *)p, *e = u + len;
    while (u < e) {
        unsigned c = *u++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3F >> more;
        while (more-- && u < e && (*u & 0xC0) == 0x80) c = (c << 6) | (*u++ & 0x3F);
        us_push(&s, c);
    }
    return s;
}

static void utf8_put(unsigned c, FILE *o) {
    if (c < 0x80) fputc(c, o);
    else if (c < 0x800) { fputc(0xC0 | (c >> 6), o); fputc(0x80 | (c & 0x3F), o); }
    else if (c < 0x10000) { fputc(0xE0 | (c >> 12), o); fputc(0x80 | ((c >> 6) & 0x3F), o); fputc(0x80 | (c & 0x3F), o); }
    else { fputc(0xF0 | (c >> 18), o); fputc(0x80 | ((c >> 12) & 0x3F), o); fputc(0x80 | ((c >> 6) & 0x3F), o); fputc(0x80 | (c & 0x3F), o); }
}

static int is_diac(unsigned c) { return c >= 0x64B && c <= 0x652; }
static int is_numeral(unsigned c) { return (c >= '0' && c <= '9') || (c >= 0x660 && c <= 0x669); }
static int is_pyspace(unsigned c) {
    static const unsigned sp[] = {0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x85, 0xa0, 0x1680,
        0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200a, 0x2028, 0x2029,
        0x202f, 0x205f, 0x3000};
    for (size_t i = 0; i < sizeof sp / sizeof *sp; i++) if (sp[i] == c) return 1;
    return 0;
}
static int input_id(unsigned c) {
    for (size_t i = 0; i < sizeof TK_INPUT / sizeof *TK_INPUT; i++) if (TK_INPUT[i].cp == c) return TK_INPUT[i].id;
    return -1;
}
/* id подсказки по строке огласовок (n знаков), -1 — нет в словаре */
static int hint_id(const unsigned *d, int n) {
    for (size_t i = 0; i < sizeof TK_HINT / sizeof *TK_HINT; i++) {
        if (TK_HINT[i].n != n) continue;
        int ok = 1;
        for (int k = 0; k < n; k++) if (TK_HINT[i].cp[k] != d[k]) ok = 0;
        if (ok) return TK_HINT[i].id;
    }
    return -1;
}

/* Огласовки для одного куска текста (s[a..b)); результат — в o. */
static void diacritize(const unsigned *txt, int len, FILE *o) {
    int a = 0, b = len;
    while (a < b && is_pyspace(txt[a])) { utf8_put(txt[a], o); a++; }
    int tail = b;
    while (b > a && is_pyspace(txt[b - 1])) b--;
    const unsigned *s = txt + a;
    int n = b - a;

    /* toValidChars: known chars and diacritics stay, digits become '#', the rest is "removed" */
    ustr valid = {0};
    unsigned char *removed = xmalloc(n + 1);    /* removed[i] — s[i] выброшен */
    for (int i = 0; i < n; i++) {
        if (input_id(s[i]) >= 0 || is_diac(s[i])) us_push(&valid, s[i]);
        else if (is_numeral(s[i])) us_push(&valid, '#');
        else removed[i] = 1;
    }
    /* множество выброшенных символов: annotate проверяет символ, а не позицию */
    ustr rset = {0};
    for (int i = 0; i < n; i++) if (removed[i]) {
        int seen = 0;
        for (int k = 0; k < rset.n; k++) if (rset.c[k] == s[i]) seen = 1;
        if (!seen) us_push(&rset, s[i]);
    }

    /* extractCharsAndDiacritics */
    int st = 0;
    while (st < valid.n && is_diac(valid.c[st])) st++;
    int nb = valid.n - st;
    int *chars = xmalloc(sizeof(int) * (nb + 2));
    unsigned (*dia)[4] = xmalloc(sizeof(*dia) * (nb + 2));
    int *dn = xmalloc(sizeof(int) * (nb + 2));
    int nc = 0, ndia = 0;
    unsigned pend[4];
    int np = 0;
    for (int i = st; i <= valid.n; i++) {
        unsigned c = i < valid.n ? valid.c[i] : ' ';
        if (is_diac(c)) { if (np < 4) pend[np++] = c; }
        else {
            chars[nc++] = (int)c;
            memcpy(dia[ndia], pend, sizeof pend);
            dn[ndia++] = np;
            np = 0;
        }
    }
    if (nc) nc--;                        /* убрать добавленный пробел */
    /* diacritics.shift(): огласовки i-го символа — те, что идут после него */
    int *hid = xmalloc(sizeof(int) * (nc + 1));
    for (int i = 0; i < nc; i++) {
        unsigned *d = dia[i + 1];
        int k = dn[i + 1];
        int id = hint_id(d, k);
        if (id < 0) {
            id = -2;
            if (k == 2)
                for (size_t m = 0; m < sizeof TK_NORM / sizeof *TK_NORM; m++)
                    if (TK_NORM[m].from[0] == d[0] && TK_NORM[m].from[1] == d[1]) id = hint_id(TK_NORM[m].to, 2);
            if (id == -2) id = hint_id(NULL, 0);     /* иначе — «без огласовки» */
            if (id < 0) id = 0;
        }
        hid[i] = id;
    }

    if (nc == 0) {
        for (int i = 0; i < n; i++) utf8_put(s[i], o);
    } else {
        int *cid = xmalloc(sizeof(int) * nc);
        for (int i = 0; i < nc; i++) {
            int id = input_id((unsigned)chars[i]);
            cid[i] = id < 0 ? 0 : id;
        }
        unsigned char *preds = xmalloc(nc);
        predict(cid, hid, nc, preds);
        /* класс-заполнитель не выравнивается по позиции, а выкидывается из потока */
        int *outc = xmalloc(sizeof(int) * nc), no = 0;
        for (int i = 0; i < nc; i++) if (preds[i] != TK_META_TARGET) outc[no++] = preds[i];
        int k = 0;
        for (int i = 0; i < n; i++) {
            unsigned c = s[i];
            if (is_diac(c)) continue;
            int rem = 0;
            for (int r = 0; r < rset.n; r++) if (rset.c[r] == c) rem = 1;
            utf8_put(c, o);
            if (rem) continue;
            if (k < no) for (int m = 0; m < TK_TARGET[outc[k]].n; m++) utf8_put(TK_TARGET[outc[k]].cp[m], o);
            k++;
        }
        free(cid); free(preds); free(outc);
    }
    for (int i = b; i < tail; i++) utf8_put(txt[i], o);
    free(valid.c); free(rset.c); free(removed); free(chars); free(dia); free(dn); free(hid);
}

/* Текст длиннее предела модели режем по пробелам на куски до CHAR_LIMIT. */
static void process(const ustr *u, FILE *o) {
    int i = 0;
    while (i < u->n) {
        int end = u->n;
        if (end - i > CHAR_LIMIT) {
            end = i + CHAR_LIMIT;
            int sp = end;
            while (sp > i + CHAR_LIMIT / 2 && !is_pyspace(u->c[sp - 1])) sp--;
            if (sp > i + CHAR_LIMIT / 2) end = sp;
        }
        diacritize(u->c + i, end - i, o);
        i = end;
    }
}

static char *read_all(FILE *f, size_t *len) {
    size_t cap = 4096, n = 0;
    char *b = xmalloc(cap);
    size_t r;
    while ((r = fread(b + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) { cap *= 2; b = realloc(b, cap); if (!b) die("out of memory", NULL); }
    }
    *len = n;
    return b;
}

int main(int argc, char **argv) {
    const char *model = NULL, *text = NULL, *outp = NULL;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    /* текст из -t — из UTF-16 командной строки, как в sanotts_cli */
    int wn = 0;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &wn);
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            text = argv[++i];
#ifdef _WIN32
            if (w && wn == argc) {
                int n = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
                char *s = n > 0 ? malloc((size_t)n) : NULL;
                if (s && WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s, n, NULL, NULL) > 0) text = s;
            }
#endif
        } else {
            fprintf(stderr, "usage: sanotts_tashkeel -m tashkeel-q16.bin [-t text] [-o out.txt]  (text on stdin otherwise)\n");
            return 2;
        }
    }
    if (!model) die("-m MODEL is required", NULL);
    load_weights(model);
    size_t len;
    char *in = text ? (char *)text : read_all(stdin, &len);
    if (text) len = strlen(text);
    ustr u = utf8_decode(in, len);
    FILE *o = stdout;
    if (outp && strcmp(outp, "-") && !(o = fopen(outp, "wb"))) die("cannot write", outp);
    process(&u, o);
    if (text) fputc('\n', o);
    if (fflush(o) != 0 || (o != stdout && fclose(o) != 0)) die("write failed", outp);
    return 0;
}
