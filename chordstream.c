#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#ifdef _MSC_VER
#include <wchar.h>
#endif

#ifdef _MSC_VER
static wchar_t *utf8_to_wide(const char *s) {
    size_t n = strlen(s);
    wchar_t *w = malloc((n + 1) * 2 * sizeof(wchar_t));
    size_t o = 0;
    if (w == NULL) return NULL;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        unsigned long cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else { cp = c & 0x07; extra = 3; }
        ++i;
        for (int k = 0; k < extra && i < n; ++k, ++i) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            w[o++] = (wchar_t)(0xD800 + (cp >> 10));
            w[o++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            w[o++] = (wchar_t)cp;
        }
    }
    w[o] = 0;
    return w;
}

static char *wide_to_utf8(const wchar_t *w) {
    size_t n = wcslen(w);
    char *s = malloc(n * 4 + 1);
    size_t o = 0;
    if (s == NULL) return NULL;
    for (size_t i = 0; i < n; ++i) {
        unsigned long cp = (unsigned long)w[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned long)w[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) s[o++] = (char)cp;
        else if (cp < 0x800) { s[o++] = (char)(0xC0 | (cp >> 6)); s[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            s[o++] = (char)(0xE0 | (cp >> 12)); s[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            s[o++] = (char)(0xF0 | (cp >> 18)); s[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            s[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); s[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    s[o] = 0;
    return s;
}
#endif

static FILE *cs_fopen(const char *path, const char *mode) {
#ifdef _MSC_VER
    wchar_t *wp = utf8_to_wide(path), *wm = utf8_to_wide(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

#define CHORDSTREAM_MAX_TEMPLATES   16
#define CHORDSTREAM_PITCH_CLASSES   12
#define CHORDSTREAM_EPSILON         1e-6
#define CHORDSTREAM_TIE_EPSILON     1e-9
#define CHORDSTREAM_KEY_TIE         0.03
#define CHORDSTREAM_WHOLE_KEY_BONUS 0.05
#define CHORDSTREAM_KEY_HYSTERESIS  4
#define CHORDSTREAM_PEDAL_MIN       32.0
#define CHORDSTREAM_MAX_BAR         65535
#define CHORDSTREAM_MAX_ONSET       63
#define CHORDSTREAM_MAX_DUR         255
#define CHORDSTREAM_MAX_BARLEN      64
#define CHORDSTREAM_DEGREE_SOLO     14
#define CHORDSTREAM_DEGREE_SILENCE  15

typedef uint64_t chordstream_token_t;

typedef struct {
    uint8_t  tonic;
    uint8_t  mode;
    uint8_t  degree;
    uint16_t mask;
    uint8_t  bass;
    uint8_t  barlen;
    uint16_t bar;
    uint8_t  onset;
    uint8_t  dur;
    uint8_t  cont;
} chordstream_unpacked_t;

typedef struct {
    double   onset;
    double   offset;
    uint8_t  pitch;
    uint16_t voice;
    uint32_t order;
} chordstream_note_t;

typedef struct {
    double start;
    int    num;
    int    den;
} chordstream_timesig_t;

typedef struct {
    bool fold_add9;
    bool absorb;

    double miss_penalty;
    double extra_penalty;
    double bass_bonus;
    double w_thresh;
    int    key_window;
    int    key_hold;
    bool   key_retroactive;
    bool   evidence_runs;
    double key_tie;
    double ev_ratio;
    double key_low_weight;
    double key_bass_weight;
    double whole_bonus;
    bool   power_fallback;
    bool   ref;
    double onset_weight;
    double phrase_weight;
    double six_penalty;
    bool   pop_profile;
} chordstream_options_t;

static chordstream_options_t g_opts;

typedef struct {
    double start16;
    double end16;
} chordstream_sidecar_t;

typedef struct {
    chordstream_token_t   *tokens;
    chordstream_sidecar_t *sidecar;
    int                    count;
} chordstream_stream_t;

typedef struct {
    const char *name;
    uint8_t     len;
    uint8_t     intervals[5];
    uint16_t    mask;
} chord_template_t;

typedef enum {
    REMI_TOKEN_BAR,
    REMI_TOKEN_POSITION,
    REMI_TOKEN_PITCH,
    REMI_TOKEN_VELOCITY,
    REMI_TOKEN_DURATION,
    REMI_TOKEN_REST,
    REMI_TOKEN_TIMESIG,
    REMI_TOKEN_TEMPO
} remi_token_type_t;

typedef struct {
    remi_token_type_t type;
    int               val1;
    int               val2;
} remi_event_t;

typedef struct {
    double start;
    double end;
    bool   end_is_closed;
} remi_window_span_t;

typedef enum { STEP_CHORD = 0, STEP_SOLO = 1, STEP_SILENCE = 2 } step_kind_t;

typedef struct {
    double start;
    double len;
    int    num;
    int    den;
    double step;
    int    first_step;
    int    n_steps;
} bar_t;

typedef struct {
    double      start;
    double      end;
    int         bar;
    double      hist[12];
    bool        has_bass;
    uint8_t     bass_pitch;
    bool        sounding;
    int         n_notes;
    int         n_pcs;
    bool        any_overlap;
    double      nonmelodic_sound;
    step_kind_t kind;
    int         root;
    int         tmpl;
    int         key;
} step_t;

typedef struct {
    double      start;
    double      end;
    int         key;
    step_kind_t kind;
    int         root;
    int         tmpl;
    bool        has_bass;
    uint8_t     bass_pitch;
} span_t;

static const double AARDEN_ESSEN_MAJOR[12] = {
    17.7661, 0.145624, 14.9265, 0.160186, 19.8049, 11.3587,
    0.291248, 22.062, 0.145624, 8.15494, 0.232998, 4.95122
};

static const double AARDEN_ESSEN_MINOR[12] = {
    18.2648, 0.737619, 14.0499, 16.8599, 0.702494, 14.4362,
    0.702494, 18.6161, 4.56621, 1.93186, 7.37619, 1.75623
};

static const chord_template_t TEMPLATES[CHORDSTREAM_MAX_TEMPLATES] = {
    {"maj",   3, {0, 4, 7, 0, 0},   (1 << 3) | (1 << 6)},
    {"m",     3, {0, 3, 7, 0, 0},   (1 << 2) | (1 << 6)},
    {"7",     4, {0, 4, 7, 10, 0},  (1 << 3) | (1 << 6) | (1 << 9)},
    {"m7",    4, {0, 3, 7, 10, 0},  (1 << 2) | (1 << 6) | (1 << 9)},
    {"maj7",  4, {0, 4, 7, 11, 0},  (1 << 3) | (1 << 6) | (1 << 10)},
    {"5",     2, {0, 7, 0, 0, 0},   (1 << 6)},
    {"sus4",  3, {0, 5, 7, 0, 0},   (1 << 4) | (1 << 6)},
    {"sus2",  3, {0, 2, 7, 0, 0},   (1 << 1) | (1 << 6)},
    {"dim",   3, {0, 3, 6, 0, 0},   (1 << 2) | (1 << 5)},
    {"aug",   3, {0, 4, 8, 0, 0},   (1 << 3) | (1 << 7)},
    {"m7b5",  4, {0, 3, 6, 10, 0},  (1 << 2) | (1 << 5) | (1 << 9)},
    {"dim7",  4, {0, 3, 6, 9, 0},   (1 << 2) | (1 << 5) | (1 << 8)},
    {"6",     4, {0, 4, 7, 9, 0},   (1 << 3) | (1 << 6) | (1 << 8)},
    {"m6",    4, {0, 3, 7, 9, 0},   (1 << 2) | (1 << 6) | (1 << 8)},
    {"add9",  4, {0, 2, 4, 7, 0},   (1 << 1) | (1 << 3) | (1 << 6)},
    {"madd9", 4, {0, 2, 3, 7, 0},   (1 << 1) | (1 << 2) | (1 << 6)}
};

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (q == NULL) {
        fprintf(stderr, "chordstream: out of memory\n");
        exit(EXIT_FAILURE);
    }
    return q;
}

static void *xcalloc(size_t n, size_t sz) {
    void *q = calloc(n ? n : 1, sz ? sz : 1);
    if (q == NULL) {
        fprintf(stderr, "chordstream: out of memory\n");
        exit(EXIT_FAILURE);
    }
    return q;
}

#define GROW(arr, n, cap) do {                                            \
        if ((n) >= (cap)) {                                               \
            (cap) = (cap) ? (cap) * 2 : 64;                               \
            (arr) = xrealloc((arr), sizeof(*(arr)) * (size_t)(cap));      \
        }                                                                 \
    } while (0)

static double overlap_len(double a0, double a1, double b0, double b1) {
    double lo = (a0 > b0) ? a0 : b0;
    double hi = (a1 < b1) ? a1 : b1;
    return hi - lo;
}

static bool divides_approx(double whole, double part) {
    double q = whole / part;
    double r = fabs(q - floor(q + 0.5));
    return r * part < CHORDSTREAM_EPSILON;
}

static int popcount12(unsigned v) {
    int c = 0;
    while (v) { c += (int)(v & 1u); v >>= 1; }
    return c;
}

int64_t round_half_even(double x) {
    double r = round(x);
    double diff = fabs(x - r);
    if (fabs(diff - 0.5) < 1e-9) {
        int64_t ir = (int64_t)r;
        if ((ir & 1) != 0) {
            if (x < r) {
                r -= 1.0;
            } else {
                r += 1.0;
            }
        }
    }
    return (int64_t)r;
}

chordstream_token_t chordstream_pack(const chordstream_unpacked_t *u) {
    uint32_t a = ((uint32_t)(u->tonic & 0x0F))
               | (((uint32_t)(u->mode & 0x01)) << 4)
               | (((uint32_t)(u->degree & 0x0F)) << 5)
               | (((uint32_t)(u->mask & 0x7FF)) << 9)
               | (((uint32_t)(u->bass & 0x0F)) << 20)
               | (((uint32_t)((u->barlen - 1) & 0x3F)) << 24);

    uint32_t b = ((uint32_t)(u->bar & 0xFFFF))
               | (((uint32_t)(u->onset & 0x3F)) << 16)
               | (((uint32_t)(u->dur & 0xFF)) << 22)
               | (((uint32_t)(u->cont & 0x01)) << 30);

    return ((chordstream_token_t)a) | (((chordstream_token_t)b) << 32);
}

void chordstream_unpack(chordstream_token_t tok, chordstream_unpacked_t *u) {
    uint32_t a = (uint32_t)(tok & 0xFFFFFFFFULL);
    uint32_t b = (uint32_t)((tok >> 32) & 0xFFFFFFFFULL);

    u->tonic   = (uint8_t)(a & 0x0F);
    u->mode    = (uint8_t)((a >> 4) & 0x01);
    u->degree  = (uint8_t)((a >> 5) & 0x0F);
    u->mask    = (uint16_t)((a >> 9) & 0x7FF);
    u->bass    = (uint8_t)((a >> 20) & 0x0F);
    u->barlen  = (uint8_t)(((a >> 24) & 0x3F) + 1);

    u->bar     = (uint16_t)(b & 0xFFFF);
    u->onset   = (uint8_t)((b >> 16) & 0x3F);
    u->dur     = (uint8_t)((b >> 22) & 0xFF);
    u->cont    = (uint8_t)((b >> 30) & 0x01);
}

int calculate_bar_length_sixteenths(int num, int den) {
    return num * 16 / den;
}

static double bar_length16(int num, int den) {
    return (double)num * 16.0 / (double)den;
}

static double chord_step_len(int num, int den, double barlen) {
    if (barlen < 4.0 - CHORDSTREAM_EPSILON) {
        return barlen;
    }
    double unit = 16.0 / (double)den;
    double beat = (num % 3 == 0 && num > 3 && den >= 8) ? 3.0 * unit : unit;

    for (int m = 1; ; ++m) {
        double candidate = (double)m * beat;
        if (candidate > barlen + CHORDSTREAM_EPSILON) {
            return barlen;
        }
        if (candidate >= 4.0 - CHORDSTREAM_EPSILON && divides_approx(barlen, candidate)) {
            return candidate;
        }
    }
}

int calculate_chord_step(int num, int den, int barlen) {
    return (int)floor(chord_step_len(num, den, (double)barlen) + 0.5);
}

static bool is_placeholder_signature(int num, int den, double segment_len) {
    if (num != 1 || den < 8) {
        return false;
    }
    return segment_len > 2.0 * bar_length16(num, den) + CHORDSTREAM_EPSILON;
}

static void split_long_bar_beats(int num, int den, int *count, int *out_sub_num) {
    if (bar_length16(num, den) <= (double)CHORDSTREAM_MAX_BARLEN + CHORDSTREAM_EPSILON) {
        out_sub_num[(*count)++] = num;
        return;
    }
    for (int d = num - 1; d >= 2; --d) {
        if (num % d == 0 && bar_length16(d, den) <= (double)CHORDSTREAM_MAX_BARLEN + CHORDSTREAM_EPSILON) {
            for (int i = 0; i < num / d; ++i) {
                out_sub_num[(*count)++] = d;
            }
            return;
        }
    }

    split_long_bar_beats((num + 1) / 2, den, count, out_sub_num);
    split_long_bar_beats(num / 2, den, count, out_sub_num);
}

void split_long_bar(int num, int den, int *out_count, int *out_sub_barlen) {
    int *beats = xcalloc((size_t)num + 2, sizeof(int));
    int count = 0;
    split_long_bar_beats(num, den, &count, beats);
    for (int i = 0; i < count; ++i) {
        out_sub_barlen[i] = (int)floor(bar_length16(beats[i], den) + 0.5);
    }
    *out_count = count;
    free(beats);
}

static int compare_timesig(const void *a, const void *b) {
    const chordstream_timesig_t *x = (const chordstream_timesig_t *)a;
    const chordstream_timesig_t *y = (const chordstream_timesig_t *)b;
    if (x->start < y->start) return -1;
    if (x->start > y->start) return 1;
    return 0;
}

static int split_bar_ref(int num, int den, int *out) {
    if (bar_length16(num, den) <= (double)CHORDSTREAM_MAX_BARLEN) { out[0] = num; return 1; }
    int k = ((int)(num * 16 / den) + CHORDSTREAM_MAX_BARLEN - 1) / CHORDSTREAM_MAX_BARLEN;
    for (int kk = k; kk <= num / 2; ++kk) {
        if (num % kk == 0 && bar_length16(num / kk, den) <= (double)CHORDSTREAM_MAX_BARLEN) {
            for (int i = 0; i < kk; ++i) out[i] = num / kk;
            return kk;
        }
    }
    int base = num / k, extra = num % k;
    while (bar_length16(base + (extra > 0), den) > (double)CHORDSTREAM_MAX_BARLEN) {
        ++k;
        base = num / k;
        extra = num % k;
    }
    for (int i = 0; i < k; ++i) out[i] = i < extra ? base + 1 : base;
    return k;
}

static int build_bars_ref(const chordstream_timesig_t *sigs, int m, double piece_end, bar_t **out_bars) {
    bar_t *bars = NULL;
    int n = 0, cap = 0, i = 0;
    double cur = 0.0;
    while (cur < piece_end) {
        while (i + 1 < m && sigs[i + 1].start <= cur + CHORDSTREAM_EPSILON) ++i;
        int num = sigs[i].num, den = sigs[i].den;
        double nxt = (i + 1 < m) ? sigs[i + 1].start : piece_end;
        if (num == 1 && den >= 8 && nxt - sigs[i].start > 2.0 * bar_length16(num, den)) { num = 4; den = 4; }
        int *subs = xcalloc((size_t)num + 2, sizeof(int));
        int n_sub = split_bar_ref(num, den, subs);
        for (int k = 0; k < n_sub; ++k) {
            double len = bar_length16(subs[k], den);
            GROW(bars, n, cap);
            bars[n].start = cur;
            bars[n].len = len;
            bars[n].num = subs[k];
            bars[n].den = den;
            bars[n].step = chord_step_len(subs[k], den, len);
            bars[n].first_step = 0;
            bars[n].n_steps = 0;
            ++n;
            cur += len;
        }
        free(subs);
    }
    *out_bars = bars;
    return n;
}

static int build_bars(const chordstream_timesig_t *sigs_in, int n_sigs, double piece_end, bar_t **out_bars) {
    bar_t *bars = NULL;
    int n = 0, cap = 0;

    chordstream_timesig_t *sigs = xcalloc((size_t)n_sigs + 1, sizeof(chordstream_timesig_t));
    int m = 0;
    for (int i = 0; i < n_sigs; ++i) {
        if (sigs_in[i].num > 0 && sigs_in[i].den > 0 && sigs_in[i].start > -CHORDSTREAM_EPSILON) {
            sigs[m] = sigs_in[i];
            if (sigs[m].start < 0.0) sigs[m].start = 0.0;
            ++m;
        }
    }
    for (int i = 1; i < m; ++i) {
        chordstream_timesig_t v = sigs[i];
        int j = i - 1;
        while (j >= 0 && compare_timesig(&sigs[j], &v) > 0) { sigs[j + 1] = sigs[j]; --j; }
        sigs[j + 1] = v;
    }
    if (m == 0 || sigs[0].start > CHORDSTREAM_EPSILON) {
        memmove(sigs + 1, sigs, sizeof(chordstream_timesig_t) * (size_t)m);
        sigs[0].start = 0.0;
        sigs[0].num = 4;
        sigs[0].den = 4;
        ++m;
    }
    if (g_opts.ref) {
        int nb = build_bars_ref(sigs, m, piece_end, out_bars);
        free(sigs);
        free(bars);
        return nb;
    }

    for (int k = 0; k < m; ++k) {
        double seg_start = sigs[k].start;
        double seg_end = (k + 1 < m) ? sigs[k + 1].start : piece_end;
        if (seg_start >= piece_end - CHORDSTREAM_EPSILON) break;
        if (seg_end <= seg_start + CHORDSTREAM_EPSILON) continue;

        int num = sigs[k].num, den = sigs[k].den;
        if (is_placeholder_signature(num, den, seg_end - seg_start)) {
            num = 4;
            den = 4;
        }

        int *subs = xcalloc((size_t)num + 2, sizeof(int));
        int n_sub = 0;
        split_long_bar_beats(num, den, &n_sub, subs);

        bool cut_by_next_signature = (k + 1 < m);
        double t = seg_start;
        while (t < seg_end - CHORDSTREAM_EPSILON) {
            for (int i = 0; i < n_sub && t < seg_end - CHORDSTREAM_EPSILON; ++i) {
                double len = bar_length16(subs[i], den);
                if (cut_by_next_signature && t + len > seg_end + CHORDSTREAM_EPSILON) {
                    len = seg_end - t;
                }
                GROW(bars, n, cap);
                bars[n].start = t;
                bars[n].len = len;
                bars[n].num = subs[i];
                bars[n].den = den;
                bars[n].step = chord_step_len(subs[i], den, len);
                bars[n].first_step = 0;
                bars[n].n_steps = 0;
                ++n;
                t += len;
            }
        }
        free(subs);
    }
    free(sigs);
    *out_bars = bars;
    return n;
}

static int find_bar(const bar_t *bars, int n_bars, double t) {
    for (int i = 0; i < n_bars; ++i) {
        if (t < bars[i].start + bars[i].len - CHORDSTREAM_EPSILON) {
            return i;
        }
    }
    return n_bars - 1;
}

static chordstream_options_t g_opts;

typedef struct {
    int    root;
    int    template_idx;
    double score;
    bool   is_silence;
} fit_result_t;

static double template_score(const double *w, int r, int t, bool has_bass, uint8_t bass_pitch) {
    const chord_template_t *tmpl = &TEMPLATES[t];
    double sum_w = 0.0;
    int count_below_threshold = 0;

    for (int k = 0; k < tmpl->len; ++k) {
        int pitch = (r + tmpl->intervals[k]) % 12;
        double val = w[pitch];
        sum_w += val;
        if (val < g_opts.w_thresh) {
            count_below_threshold++;
        }
    }

    int excess_tones = tmpl->len > 3 ? (tmpl->len - 3) : 0;
    double score = 2.0 * sum_w - 1.0
                 - g_opts.miss_penalty * count_below_threshold
                 - g_opts.extra_penalty * excess_tones;
    if (t == 12 || t == 13) score -= g_opts.six_penalty;

    if (has_bass && ((bass_pitch % 12) == r)) {
        score += g_opts.bass_bonus;
    }
    return score;
}

static bool normalise_hist(const double *hist12, double *w) {
    double sum_h = 0.0;
    for (int p = 0; p < 12; ++p) {
        sum_h += hist12[p];
    }
    if (sum_h < CHORDSTREAM_EPSILON) {
        return false;
    }
    for (int p = 0; p < 12; ++p) {
        w[p] = hist12[p] / sum_h;
    }
    return true;
}

fit_result_t fit_chord_step(const double *weighted_hist12, bool has_bass, uint8_t bass_pitch) {
    fit_result_t res;
    res.root = 0;
    res.template_idx = 0;
    res.score = -1e9;
    res.is_silence = false;

    double w[12];
    if (!normalise_hist(weighted_hist12, w)) {
        res.is_silence = true;
        res.score = 0.0;
        return res;
    }

    double scores[12 * CHORDSTREAM_MAX_TEMPLATES];
    double best = -1e9;
    for (int r = 0; r < 12; ++r) {
        for (int t = 0; t < CHORDSTREAM_MAX_TEMPLATES; ++t) {
            double score = template_score(w, r, t, has_bass, bass_pitch);
            scores[r * CHORDSTREAM_MAX_TEMPLATES + t] = score;
            if (score > best) best = score;
        }
    }

    for (int k = 0; k < 12 * CHORDSTREAM_MAX_TEMPLATES; ++k) {
        if (scores[k] >= best - CHORDSTREAM_TIE_EPSILON) {
            res.score = scores[k];
            res.root = k / CHORDSTREAM_MAX_TEMPLATES;
            res.template_idx = k % CHORDSTREAM_MAX_TEMPLATES;
            break;
        }
    }
    return res;
}

static int fit_template_fixed_root(const double *hist12, int root, int keep_tmpl, bool has_bass, uint8_t bass_pitch) {
    double w[12];
    if (!normalise_hist(hist12, w)) {
        return keep_tmpl;
    }
    double scores[CHORDSTREAM_MAX_TEMPLATES];
    double best = -1e9;
    for (int t = 0; t < CHORDSTREAM_MAX_TEMPLATES; ++t) {
        scores[t] = template_score(w, root, t, has_bass, bass_pitch);
        if (scores[t] > best) best = scores[t];
    }
    for (int t = 0; t < CHORDSTREAM_MAX_TEMPLATES; ++t) {
        if (scores[t] >= best - CHORDSTREAM_TIE_EPSILON) return t;
    }
    return keep_tmpl;
}

uint16_t get_template_mask(int template_idx, bool fold_add9) {
    if (fold_add9) {
        if (template_idx == 14) return TEMPLATES[0].mask;
        if (template_idx == 15) return TEMPLATES[1].mask;
    }
    return TEMPLATES[template_idx].mask;
}

static double note_weight(uint8_t pitch, bool melodic) {
    if (melodic) return 0.25;
    if (pitch < 60) return 1.5;
    if (pitch >= 72) return 0.6;
    return 1.0;
}

static const chordstream_note_t *g_sort_notes = NULL;

static int compare_voice_onset(const void *a, const void *b) {
    int i = *(const int *)a, j = *(const int *)b;
    const chordstream_note_t *x = &g_sort_notes[i], *y = &g_sort_notes[j];
    if (x->voice != y->voice) return (x->voice < y->voice) ? -1 : 1;
    if (x->onset < y->onset) return -1;
    if (x->onset > y->onset) return 1;
    return (i < j) ? -1 : (i > j);
}

static int compare_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x < y) ? -1 : (x > y);
}

bool chordstream_mark_melodic(const chordstream_note_t *notes, int n, bool *melodic) {
    for (int i = 0; i < n; ++i) melodic[i] = false;
    if (n == 0) return false;

    int *pitches = xcalloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; ++i) pitches[i] = notes[i].pitch;
    qsort(pitches, (size_t)n, sizeof(int), compare_int);
    double median = (n % 2 == 1) ? (double)pitches[n / 2]
                                 : 0.5 * ((double)pitches[n / 2 - 1] + (double)pitches[n / 2]);
    free(pitches);

    int *order = xcalloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; ++i) order[i] = i;
    g_sort_notes = notes;
    qsort(order, (size_t)n, sizeof(int), compare_voice_onset);

    int n_voices = 0;
    for (int i = 0; i < n; ++i) {
        if (i == 0 || notes[order[i]].voice != notes[order[i - 1]].voice) ++n_voices;
    }

    bool found = false;
    if (n_voices > 1) {
        int i = 0;
        while (i < n) {
            int j = i;
            while (j + 1 < n && notes[order[j + 1]].voice == notes[order[i]].voice) ++j;
            int cnt = j - i + 1;
            int overlapping = 0;
            double sum_pitch = 0.0;
            for (int k = i; k <= j; ++k) {
                sum_pitch += notes[order[k]].pitch;
                if (k > i && notes[order[k]].onset < notes[order[k - 1]].offset - 0.5) {
                    ++overlapping;
                }
            }
            double mono = (cnt > 1) ? 1.0 - (double)overlapping / (double)(cnt - 1) : 1.0;
            double mean = sum_pitch / (double)cnt;
            if (mono >= 0.85 && mean > median + 2.0 && mean >= 60.0) {
                found = true;
                for (int k = i; k <= j; ++k) melodic[order[k]] = true;
            }
            i = j + 1;
        }
    }
    free(order);
    if (found) return true;

    if (g_opts.ref) {

        int *ord = xcalloc((size_t)n, sizeof(int));
        double *key = xcalloc((size_t)n, sizeof(double));
        for (int i = 0; i < n; ++i) { ord[i] = i; char buf[64]; snprintf(buf, sizeof buf, "%.2f", notes[i].onset); key[i] = strtod(buf, NULL); }

        for (int i = 1; i < n; ++i) {
            int v = ord[i], j = i - 1;
            while (j >= 0 && key[ord[j]] > key[v]) { ord[j + 1] = ord[j]; --j; }
            ord[j + 1] = v;
        }
        int g = 0;
        while (g < n) {
            int h = g;
            while (h + 1 < n && key[ord[h + 1]] == key[ord[g]]) ++h;
            double t = key[ord[g]];
            int top = ord[g];
            for (int k = g + 1; k <= h; ++k) if (notes[ord[k]].pitch > notes[top].pitch) top = ord[k];
            int max_other = -1;
            for (int k = g; k <= h; ++k) if (ord[k] != top && notes[ord[k]].pitch > max_other) max_other = notes[ord[k]].pitch;
            for (int k = 0; k < g; ++k) if (notes[ord[k]].offset > t && notes[ord[k]].pitch > max_other) max_other = notes[ord[k]].pitch;
            if (notes[top].pitch >= 60 && (max_other < 0 || notes[top].pitch - max_other >= 5)) melodic[top] = true;
            g = h + 1;
        }
        free(ord);
        free(key);
        return false;
    }

    for (int i = 0; i < n; ++i) {
        double t = notes[i].onset;
        if (notes[i].pitch < 60) continue;
        bool ok = true;
        for (int j = 0; j < n && ok; ++j) {
            if (j == i) continue;
            if (fabs(notes[j].onset - t) <= CHORDSTREAM_EPSILON && notes[j].pitch > notes[i].pitch) {
                ok = false;
            } else if (notes[j].onset <= t + CHORDSTREAM_EPSILON && notes[j].offset > t + CHORDSTREAM_EPSILON
                       && (int)notes[i].pitch - (int)notes[j].pitch < 5) {
                ok = false;
            }
        }
        melodic[i] = ok;
    }
    return false;
}

static void step_histogram(const step_t *s, const chordstream_note_t *notes, int n, const bool *melodic,
                           int exclude_pitch, double *hist) {
    memset(hist, 0, sizeof(double) * 12);
    for (int i = 0; i < n; ++i) {
        if (exclude_pitch >= 0 && notes[i].pitch == exclude_pitch) continue;
        double ov = overlap_len(notes[i].onset, notes[i].offset, s->start, s->end);
        if (ov <= CHORDSTREAM_EPSILON) continue;
        hist[notes[i].pitch % 12] += ov * note_weight(notes[i].pitch, melodic[i]);
        if (g_opts.onset_weight > 0.0 && notes[i].onset >= s->start - CHORDSTREAM_EPSILON
            && notes[i].onset < s->end - CHORDSTREAM_EPSILON) {
            hist[notes[i].pitch % 12] += g_opts.onset_weight * ov * note_weight(notes[i].pitch, melodic[i]);
        }
    }
}

static void analyse_step(step_t *s, const chordstream_note_t *notes, int n, const bool *melodic, int *scratch) {
    double len = s->end - s->start;
    int m = 0;
    unsigned pcs = 0;

    memset(s->hist, 0, sizeof s->hist);
    s->has_bass = false;
    s->bass_pitch = 0;
    s->sounding = false;
    s->n_notes = 0;
    s->n_pcs = 0;
    s->any_overlap = false;
    s->nonmelodic_sound = 0.0;

    for (int i = 0; i < n; ++i) {
        double ov = overlap_len(notes[i].onset, notes[i].offset, s->start, s->end);
        if (ov <= CHORDSTREAM_EPSILON) continue;
        scratch[m++] = i;
        s->sounding = true;
        pcs |= 1u << (notes[i].pitch % 12);
        s->hist[notes[i].pitch % 12] += ov * note_weight(notes[i].pitch, melodic[i]);
        if (g_opts.onset_weight > 0.0 && notes[i].onset >= s->start - CHORDSTREAM_EPSILON
            && notes[i].onset < s->end - CHORDSTREAM_EPSILON) {

            s->hist[notes[i].pitch % 12] += g_opts.onset_weight * ov * note_weight(notes[i].pitch, melodic[i]);
        }
        if (ov >= 0.25 * len - CHORDSTREAM_EPSILON) {
            if (!s->has_bass || notes[i].pitch < s->bass_pitch) {
                s->has_bass = true;
                s->bass_pitch = notes[i].pitch;
            }
        }
        if (!melodic[i]) {
            s->nonmelodic_sound += ov;
        }
    }
    s->n_notes = m;
    s->n_pcs = popcount12(pcs);
    if (g_opts.ref) {

        for (int a = 1; a < m; ++a) {
            int v = scratch[a], b = a - 1;
            while (b >= 0 && (notes[scratch[b]].onset > notes[v].onset
                              || (notes[scratch[b]].onset == notes[v].onset && scratch[b] > v))) {
                scratch[b + 1] = scratch[b];
                --b;
            }
            scratch[b + 1] = v;
        }
        for (int a = 1; a < m; ++a) {
            if (notes[scratch[a]].onset < notes[scratch[a - 1]].offset - 0.5) { s->any_overlap = true; break; }
        }
        return;
    }
    for (int a = 0; a < m && !s->any_overlap; ++a) {
        for (int b = a + 1; b < m; ++b) {
            const chordstream_note_t *x = &notes[scratch[a]], *y = &notes[scratch[b]];
            if (overlap_len(x->onset, x->offset, y->onset, y->offset) > CHORDSTREAM_EPSILON) {
                s->any_overlap = true;
                break;
            }
        }
    }
}

static void fit_step(step_t *s) {
    fit_result_t r = fit_chord_step(s->hist, s->has_bass, s->bass_pitch);
    if (r.is_silence) {
        s->kind = STEP_SILENCE;
        s->root = 0;
        s->tmpl = 0;
    } else {
        s->kind = STEP_CHORD;
        s->root = r.root;
        s->tmpl = r.template_idx;
    }
}

static void resolve_pedal_points(step_t *steps, int n_steps, const bar_t *bars,
                                 const chordstream_note_t *notes, int n_notes, const bool *melodic) {
    int i = 0;
    while (i < n_steps) {
        if (!steps[i].has_bass) { ++i; continue; }
        int j = i;
        while (j + 1 < n_steps && steps[j + 1].has_bass && steps[j + 1].bass_pitch == steps[i].bass_pitch) ++j;

        if (steps[j].end - steps[i].start >= CHORDSTREAM_PEDAL_MIN - CHORDSTREAM_EPSILON) {
            int pedal = steps[i].bass_pitch;
            if (g_opts.ref) {

                for (int k = i; k <= j; ++k) step_histogram(&steps[k], notes, n_notes, melodic, pedal, steps[k].hist);
            }
            int g = i;
            while (g <= j) {

                const bar_t *b = &bars[steps[g].bar];
                bool first_half = (steps[g].start - b->start) < 0.5 * b->len - CHORDSTREAM_EPSILON;
                int h = g;
                while (h + 1 <= j && steps[h + 1].bar == steps[g].bar
                       && (((steps[h + 1].start - b->start) < 0.5 * b->len - CHORDSTREAM_EPSILON) == first_half)) ++h;

                double group_hist[12] = {0};
                double removed[12];
                double sum = 0.0;
                for (int k = g; k <= h; ++k) {
                    step_histogram(&steps[k], notes, n_notes, melodic, pedal, removed);
                    for (int p = 0; p < 12; ++p) group_hist[p] += removed[p];
                }
                for (int p = 0; p < 12; ++p) sum += group_hist[p];

                if (sum >= CHORDSTREAM_EPSILON) {
                    fit_result_t r = fit_chord_step(group_hist, false, 0);
                    for (int k = g; k <= h; ++k) {
                        step_histogram(&steps[k], notes, n_notes, melodic, pedal, steps[k].hist);
                        steps[k].kind = STEP_CHORD;
                        steps[k].root = r.root;
                        steps[k].tmpl = r.template_idx;

                    }
                }

                g = h + 1;
            }
        }
        i = j + 1;
    }
}

static void resolve_solo_lines(step_t *steps, int n_steps, bool melody_found, bool nonmelodic_at_least_10pct) {
    for (int i = 0; i < n_steps; ++i) {
        step_t *s = &steps[i];
        if (!s->sounding || s->kind == STEP_SILENCE) {
            s->kind = STEP_SILENCE;
            continue;
        }
        bool rule_a = melody_found && nonmelodic_at_least_10pct && s->nonmelodic_sound < 0.5;
        bool rule_b = !s->any_overlap && s->n_pcs < 3;
        if (rule_a || rule_b) {
            s->kind = STEP_SOLO;
        }
    }
}

double pearson_correlation(const double *x, const double *y, int n) {
    double sum_x = 0.0, sum_y = 0.0;
    for (int i = 0; i < n; ++i) {
        sum_x += x[i];
        sum_y += y[i];
    }
    double mean_x = sum_x / n;
    double mean_y = sum_y / n;

    double num = 0.0, den_x = 0.0, den_y = 0.0;
    for (int i = 0; i < n; ++i) {
        double dx = x[i] - mean_x;
        double dy = y[i] - mean_y;
        num += dx * dy;
        den_x += dx * dx;
        den_y += dy * dy;
    }
    if (den_x < CHORDSTREAM_EPSILON || den_y < CHORDSTREAM_EPSILON) {
        return 0.0;
    }
    return num / sqrt(den_x * den_y);
}

static int window_lo_n(int i, int n_bars, int window) {
    if (n_bars < window) return 0;
    int lo = i - window / 2;
    if (lo < 0) lo = 0;
    if (lo > n_bars - window) lo = n_bars - window;
    return lo;
}

int get_window_lo(int i, int n_bars) {
    return window_lo_n(i, n_bars, 8);
}

static const double POP_MAJOR[12] = {
    21.4055, 0.2025, 13.3891, 0.3057, 14.4522, 8.5471, 0.4273, 20.8253, 0.5971, 12.5270, 0.6847, 6.6365
};
static const double POP_MINOR[12] = {
    23.4970, 0.1901, 8.2014, 14.5657, 0.3527, 12.3330, 0.0875, 20.0467, 6.5986, 0.5512, 12.5857, 0.9906
};

static double g_prof_major[12], g_prof_minor[12];
static bool g_custom_profile = false;

bool chordstream_load_profiles(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *p = buf;
    for (int i = 0; i < 24; ++i) {
        char *end;
        double v = strtod(p, &end);
        if (end == p) return false;
        p = end;
        if (i < 12) g_prof_major[i] = v; else g_prof_minor[i - 12] = v;
    }
    g_custom_profile = true;
    return true;
}

static void key_scores(const double *hist12, int whole_piece_best_key, double bonus, double *scores) {
    for (int k = 0; k < 24; ++k) {
        double rotated_profile[12];
        int tonic = (k < 12) ? k : (k - 12);
        const double *base = g_custom_profile ? ((k < 12) ? g_prof_major : g_prof_minor)
                           : g_opts.pop_profile ? ((k < 12) ? POP_MAJOR : POP_MINOR)
                                                : ((k < 12) ? AARDEN_ESSEN_MAJOR : AARDEN_ESSEN_MINOR);
        for (int p = 0; p < 12; ++p) {
            rotated_profile[p] = base[(p - tonic + 12) % 12];
        }
        scores[k] = pearson_correlation(hist12, rotated_profile, 12);
        if (k == whole_piece_best_key) {
            scores[k] += bonus;
        }
    }
}

static int argmax_key(const double *scores) {
    double best_r = -2.0;
    int best_key = 0;
    for (int k = 0; k < 24; ++k) {

        if (g_opts.ref ? scores[k] - best_r > 1e-12 : scores[k] - best_r > CHORDSTREAM_TIE_EPSILON) {
            best_r = scores[k];
            best_key = k;
        }
    }
    return best_key;
}

int correlate_best_key(const double *hist12, int whole_piece_best_key, double bonus) {
    double scores[24];
    key_scores(hist12, whole_piece_best_key, bonus, scores);
    return argmax_key(scores);
}

static int relative_key(int k) {
    return (k < 12) ? 12 + (k + 9) % 12 : (k - 12 + 3) % 12;
}

static int g_n_steps;

static void tonic_evidence(const step_t *steps, int s_from, int s_to, double *ev) {
    for (int p = 0; p < 12; ++p) ev[p] = 0.0;
    if (g_opts.ref) {

        for (int i = s_from; i < s_to; ++i) {
            double len = steps[i].end - steps[i].start;
            if (steps[i].has_bass) ev[steps[i].bass_pitch % 12] += len;
            if (steps[i].kind == STEP_CHORD) ev[steps[i].root] += len;
        }
        int roots[3] = {-1, -1, -1};
        int first = -1;
        double len = 0.0;
        for (int i = 0; i <= g_n_steps; ++i) {
            bool end = (i == g_n_steps);
            if (!end && steps[i].kind != STEP_CHORD) continue;
            if (!end && steps[i].root == roots[2]) { len += steps[i].end - steps[i].start; continue; }
            if (roots[2] >= 0 && roots[1] >= 0 && first >= s_from && first < s_to) {
                int from = (roots[1] - roots[2] + 12) % 12;
                if (from == 5 || from == 7 || from == 10) {
                    ev[roots[2]] += len;
                    if (from == 10 && roots[0] >= 0 && (roots[0] - roots[2] + 12) % 12 == 8) ev[roots[2]] += len;
                }
            }
            if (end) break;
            roots[0] = roots[1];
            roots[1] = roots[2];
            roots[2] = steps[i].root;
            first = i;
            len = steps[i].end - steps[i].start;
        }
        return;
    }
    if (g_opts.evidence_runs) {

        int prev_root = -1, prev2_root = -1;
        int run_root = -1;
        double run_len = 0.0;
        bool run_in_window = false;
        for (int i = 0; i < s_to; ++i) {
            const step_t *s = &steps[i];
            double len = s->end - s->start;
            if (i >= s_from && s->has_bass) ev[s->bass_pitch % 12] += len;
            if (s->kind != STEP_CHORD) continue;
            if (i >= s_from) ev[s->root] += len;
            if (s->root == run_root) {
                run_len += len;
                continue;
            }

            if (run_root >= 0 && run_in_window && prev_root >= 0) {
                int from = (prev_root - run_root + 12) % 12;
                if (from == 5 || from == 7 || from == 10) {
                    ev[run_root] += run_len;
                    if (from == 10 && prev2_root >= 0 && (prev2_root - run_root + 12) % 12 == 8) ev[run_root] += run_len;
                }
            }
            prev2_root = prev_root;
            prev_root = run_root;
            run_root = s->root;
            run_len = len;
            run_in_window = (i >= s_from);
        }
        if (run_root >= 0 && run_in_window && prev_root >= 0) {
            int from = (prev_root - run_root + 12) % 12;
            if (from == 5 || from == 7 || from == 10) {
                ev[run_root] += run_len;
                if (from == 10 && prev2_root >= 0 && (prev2_root - run_root + 12) % 12 == 8) ev[run_root] += run_len;
            }
        }
        return;
    }
    for (int i = s_from; i < s_to; ++i) {
        const step_t *s = &steps[i];
        if (s->kind != STEP_CHORD) continue;
        double len = s->end - s->start;
        if (s->has_bass) ev[s->bass_pitch % 12] += len;
        ev[s->root] += len;
        if (g_opts.phrase_weight > 0.0 && s->bar % 4 == 0 && (i == 0 || steps[i - 1].bar != s->bar)) {

            ev[s->root] += g_opts.phrase_weight * len;
        }
        if (i > 0 && steps[i - 1].kind == STEP_CHORD) {

            int from = (steps[i - 1].root - s->root + 12) % 12;
            if (from == 5 || from == 7 || from == 10) {
                ev[s->root] += len;
                if (from == 10 && i > 1 && steps[i - 2].kind == STEP_CHORD
                    && (steps[i - 2].root - s->root + 12) % 12 == 8) {
                    ev[s->root] += len;
                }
            }
        }
    }
}

static int resolve_relative_tie(int best, const double *hist12, const step_t *steps, int s_from, int s_to) {
    int rel = relative_key(best);
    double ev[12];
    tonic_evidence(steps, s_from, s_to, ev);
    int minor_key = (best >= 12) ? best : rel;
    int major_key = (best >= 12) ? rel : best;
    double ev_best = ev[best % 12], ev_rel = ev[rel % 12];
    if (g_opts.ref) {
        int m_t = minor_key - 12;
        double ev_min = ev[m_t], ev_maj = ev[major_key % 12];
        if (fabs(ev_min - ev_maj) > 1e-6) return ev_min > ev_maj ? minor_key : major_key;
        return hist12[(m_t + 11) % 12] > hist12[(m_t + 10) % 12] ? minor_key : major_key;
    }
    if (ev_best > ev_rel * g_opts.ev_ratio + CHORDSTREAM_TIE_EPSILON) return best;
    if (ev_rel > ev_best * g_opts.ev_ratio + CHORDSTREAM_TIE_EPSILON) return rel;
    if (g_opts.ev_ratio != 1.0) return best;
    int m_tonic = minor_key - 12;
    bool minor = hist12[(m_tonic + 11) % 12] > hist12[(m_tonic + 10) % 12];
    return minor ? minor_key : major_key;
}

static int choose_key(const double *hist12, int whole_key, double bonus, const step_t *steps, int s_from, int s_to) {
    double scores[24];
    key_scores(hist12, whole_key, bonus, scores);
    int best = argmax_key(scores);
    int rel = relative_key(best);
    if (fabs(scores[best] - scores[rel]) <= g_opts.key_tie) {
        best = resolve_relative_tie(best, hist12, steps, s_from, s_to);
    }
    return best;
}

void apply_key_hysteresis(const int *winner, int n_bars, int *key) {
    if (n_bars == 0) return;
    int current = winner[0];
    int candidate = -1, run = 0;
    key[0] = current;
    for (int i = 1; i < n_bars; ++i) {
        int w = winner[i];
        if (w == current) {
            candidate = -1;
            run = 0;
        } else {
            if (w == candidate) ++run; else { candidate = w; run = 1; }
            if (run >= g_opts.key_hold) {
                current = w;
                if (g_opts.key_retroactive) {
                    for (int j = i - g_opts.key_hold + 1; j < i; ++j) key[j] = current;
                }
                candidate = -1;
                run = 0;
            }
        }
        key[i] = current;
    }
}

static void estimate_keys(const bar_t *bars, int n_bars, step_t *steps, int n_steps,
                          const chordstream_note_t *notes, int n_notes, int *key_out) {
    double *bar_hist = xcalloc((size_t)n_bars * 12, sizeof(double));
    double whole[12] = {0};

    for (int i = 0; i < n_notes; ++i) {
        for (int b = 0; b < n_bars; ++b) {
            double ov = overlap_len(notes[i].onset, notes[i].offset, bars[b].start, bars[b].start + bars[b].len);
            if (ov > CHORDSTREAM_EPSILON) {
                double w = (notes[i].pitch < 60) ? g_opts.key_low_weight : 1.0;
                bar_hist[b * 12 + notes[i].pitch % 12] += ov * w;
                whole[notes[i].pitch % 12] += ov * w;
            }
        }
    }
    if (g_opts.key_bass_weight != 0.0) {

        for (int i = 0; i < n_steps; ++i) {
            if (!steps[i].has_bass) continue;
            double d = (steps[i].end - steps[i].start) * g_opts.key_bass_weight;
            bar_hist[steps[i].bar * 12 + steps[i].bass_pitch % 12] += d;
            whole[steps[i].bass_pitch % 12] += d;
        }
    }

    g_n_steps = n_steps;
    int whole_key = choose_key(whole, -1, 0.0, steps, 0, n_steps);

    int *winner = xcalloc((size_t)n_bars, sizeof(int));
    for (int i = 0; i < n_bars; ++i) {
        int lo = window_lo_n(i, n_bars, g_opts.key_window);
        int hi = lo + g_opts.key_window;
        if (hi > n_bars) hi = n_bars;
        double hist[12] = {0};
        for (int b = lo; b < hi; ++b) {
            for (int p = 0; p < 12; ++p) hist[p] += bar_hist[b * 12 + p];
        }
        int s_from = bars[lo].first_step;
        int s_to = bars[hi - 1].first_step + bars[hi - 1].n_steps;
        if (g_opts.ref) {
            double tot = 0.0;
            for (int p = 0; p < 12; ++p) tot += hist[p];
            if (tot <= 0.0) { winner[i] = i > 0 ? winner[i - 1] : whole_key; continue; }
        }
        winner[i] = choose_key(hist, whole_key, g_opts.whole_bonus, steps, s_from, s_to);
    }
    apply_key_hysteresis(winner, n_bars, key_out);
    free(winner);
    free(bar_hist);
}

static int step_degree(const step_t *s) {
    return (s->root - (s->key % 12) + 12) % 12;
}

static bool same_chord(const step_t *a, const step_t *b, bool fold) {
    return a->root == b->root && get_template_mask(a->tmpl, fold) == get_template_mask(b->tmpl, fold);
}

static void absorb_single_steps(step_t *steps, int n_steps, bool fold) {
    for (int i = 1; i + 1 < n_steps; ++i) {
        step_t *a = &steps[i - 1], *b = &steps[i], *c = &steps[i + 1];
        if (a->kind != STEP_CHORD || b->kind != STEP_CHORD || c->kind != STEP_CHORD) continue;
        if (!same_chord(a, c, fold) || same_chord(a, b, fold)) continue;
        int da = step_degree(a), db = step_degree(b), dc = step_degree(c);
        if (db == 7 || db == 11 || da == 7 || da == 11 || dc == 7 || dc == 11) continue;
        b->root = a->root;
        b->tmpl = a->tmpl;
    }
}

static bool can_merge(const step_t *prev, const step_t *s) {
    if (prev->key != s->key || prev->kind != s->kind) return false;
    if (s->kind == STEP_CHORD) return prev->root == s->root;
    return true;
}

static int merge_steps(const step_t *steps, int n_steps, span_t **out_spans) {
    span_t *spans = NULL;
    int n = 0, cap = 0;
    int i = 0;
    while (i < n_steps) {
        int j = i;
        while (j + 1 < n_steps && can_merge(&steps[i], &steps[j + 1])) ++j;

        GROW(spans, n, cap);
        span_t *sp = &spans[n++];
        sp->start = steps[i].start;
        sp->end = steps[j].end;
        sp->key = steps[i].key;
        sp->kind = steps[i].kind;
        sp->root = steps[i].root;
        sp->tmpl = steps[i].tmpl;
        sp->has_bass = false;
        sp->bass_pitch = 0;

        if (sp->kind == STEP_CHORD) {
            double hist[12] = {0};
            for (int k = i; k <= j; ++k) {
                for (int p = 0; p < 12; ++p) hist[p] += steps[k].hist[p];
                if (steps[k].has_bass && (!sp->has_bass || steps[k].bass_pitch < sp->bass_pitch)) {
                    sp->has_bass = true;
                    sp->bass_pitch = steps[k].bass_pitch;
                }
            }
            if (g_opts.ref && i == j) {

            } else if (g_opts.ref) {
                double tot = 0.0;
                for (int p = 0; p < 12; ++p) tot += hist[p];
                if (tot < CHORDSTREAM_EPSILON) {
                    sp->has_bass = steps[i].has_bass;
                    sp->bass_pitch = steps[i].bass_pitch;
                } else {
                    sp->tmpl = fit_template_fixed_root(hist, sp->root, sp->tmpl, sp->has_bass, sp->bass_pitch);
                }
            } else {
                sp->tmpl = fit_template_fixed_root(hist, sp->root, sp->tmpl, sp->has_bass, sp->bass_pitch);
            }
            if (g_opts.power_fallback && sp->tmpl == 5) {

                double maj3 = hist[(sp->root + 4) % 12], min3 = hist[(sp->root + 3) % 12];
                if (maj3 > min3 + CHORDSTREAM_TIE_EPSILON) {
                    sp->tmpl = 0;
                } else if (min3 > maj3 + CHORDSTREAM_TIE_EPSILON) {
                    sp->tmpl = 1;
                } else {
                    int degree = (sp->root - sp->key % 12 + 12) % 12;
                    bool minor_key = sp->key >= 12;

                    bool minor_third = minor_key ? (degree == 0 || degree == 2 || degree == 5 || degree == 7)
                                                 : (degree == 2 || degree == 4 || degree == 9 || degree == 11);
                    sp->tmpl = minor_third ? 1 : 0;
                }
            }
        }
        i = j + 1;
    }
    *out_spans = spans;
    return n;
}

static void stream_push(chordstream_stream_t *st, int *cap, chordstream_token_t tok, double s16, double e16) {
    if (st->count >= *cap) {
        *cap = *cap ? *cap * 2 : 64;
        st->tokens = xrealloc(st->tokens, sizeof(chordstream_token_t) * (size_t)*cap);
        st->sidecar = xrealloc(st->sidecar, sizeof(chordstream_sidecar_t) * (size_t)*cap);
    }
    st->tokens[st->count] = tok;
    st->sidecar[st->count].start16 = s16;
    st->sidecar[st->count].end16 = e16;
    st->count++;
}

static void emit_spans(const span_t *spans, int n_spans, const bar_t *bars, int n_bars, bool fold,
                       chordstream_stream_t *out) {
    int cap = 0;
    out->tokens = NULL;
    out->sidecar = NULL;
    out->count = 0;

    while (n_spans > 0 && spans[n_spans - 1].kind == STEP_SILENCE) --n_spans;

    for (int i = 0; i < n_spans; ++i) {
        const span_t *sp = &spans[i];
        double seg_start = sp->start;
        while (seg_start < sp->end - CHORDSTREAM_EPSILON) {
            int b = find_bar(bars, n_bars, seg_start);
            double seg_end = sp->end;

            for (int k = b + 1; k < n_bars && bars[k].start < seg_end - CHORDSTREAM_EPSILON; ++k) {
                if (fabs(bars[k].len - bars[b].len) > CHORDSTREAM_EPSILON) {
                    seg_end = bars[k].start;
                    break;
                }
            }

            if (seg_end - seg_start > (double)CHORDSTREAM_MAX_DUR + CHORDSTREAM_EPSILON) {
                seg_end = seg_start + (double)CHORDSTREAM_MAX_DUR;
            }

            if (b <= CHORDSTREAM_MAX_BAR) {
                chordstream_unpacked_t u;
                int64_t onset = round_half_even(seg_start - bars[b].start);
                int64_t dur = round_half_even(seg_end - seg_start);
                int64_t barlen = round_half_even(bars[b].len);
                if (onset < 0) onset = 0;
                if (onset > CHORDSTREAM_MAX_ONSET) onset = CHORDSTREAM_MAX_ONSET;
                if (dur < 1) dur = 1;
                if (dur > CHORDSTREAM_MAX_DUR) dur = CHORDSTREAM_MAX_DUR;
                if (barlen < 1) barlen = 1;
                if (barlen > CHORDSTREAM_MAX_BARLEN) barlen = CHORDSTREAM_MAX_BARLEN;

                u.tonic = (uint8_t)(sp->key % 12);
                u.mode = (uint8_t)(sp->key / 12);
                if (sp->kind == STEP_CHORD) {
                    u.degree = (uint8_t)((sp->root - sp->key % 12 + 12) % 12);
                    u.mask = get_template_mask(sp->tmpl, fold);
                    u.bass = sp->has_bass ? (uint8_t)((sp->bass_pitch % 12 - sp->root + 12) % 12) : 0;
                } else {
                    u.degree = (sp->kind == STEP_SOLO) ? CHORDSTREAM_DEGREE_SOLO : CHORDSTREAM_DEGREE_SILENCE;
                    u.mask = 0;
                    u.bass = 0;
                }
                u.barlen = (uint8_t)barlen;
                u.bar = (uint16_t)b;
                u.onset = (uint8_t)onset;
                u.dur = (uint8_t)dur;
                u.cont = (seg_start > sp->start + CHORDSTREAM_EPSILON) ? 1 : 0;
                stream_push(out, &cap, chordstream_pack(&u), seg_start, seg_end);
            }
            seg_start = seg_end;
        }
    }
}

chordstream_options_t chordstream_default_options(void) {
    chordstream_options_t o;
    memset(&o, 0, sizeof o);
    o.fold_add9 = true;
    o.absorb = false;
    o.miss_penalty = 0.12;
    o.extra_penalty = 0.06;
    o.bass_bonus = 0.08;
    o.w_thresh = 0.03;
    o.key_window = 8;
    o.key_hold = CHORDSTREAM_KEY_HYSTERESIS;
    o.key_retroactive = false;
    o.evidence_runs = false;
    o.key_tie = CHORDSTREAM_KEY_TIE;
    o.ev_ratio = 1.0;
    o.key_low_weight = 1.0;
    o.key_bass_weight = 0.0;
    o.whole_bonus = CHORDSTREAM_WHOLE_KEY_BONUS;
    o.power_fallback = false;
    o.ref = false;
    o.onset_weight = 0.0;
    o.phrase_weight = 0.0;
    o.six_penalty = 0.0;
    o.pop_profile = false;
    return o;
}

chordstream_options_t chordstream_ref_options(void) {
    chordstream_options_t o = chordstream_default_options();
    o.key_retroactive = true;
    o.evidence_runs = true;
    o.ref = true;
    return o;
}

chordstream_options_t chordstream_tuned_options(void) {
    chordstream_options_t o = chordstream_default_options();
    o.extra_penalty = 0.25;
    o.key_window = 16;
    o.key_hold = 6;
    o.key_retroactive = true;
    o.power_fallback = true;
    o.pop_profile = true;
    return o;
}

static chordstream_options_t g_opts = {
    true, false, 0.12, 0.06, 0.08, 0.03, 8, CHORDSTREAM_KEY_HYSTERESIS, false, false,
    CHORDSTREAM_KEY_TIE, 1.0, 1.0, 0.0, CHORDSTREAM_WHOLE_KEY_BONUS, false, false
};

bool chordstream_set_option(chordstream_options_t *o, const char *kv) {
    const char *eq = strchr(kv, '=');
    if (eq == NULL) return false;
    char name[48];
    size_t n = (size_t)(eq - kv);
    if (n >= sizeof name) return false;
    memcpy(name, kv, n);
    name[n] = 0;
    double v = strtod(eq + 1, NULL);
    if (strcmp(name, "miss_penalty") == 0) o->miss_penalty = v;
    else if (strcmp(name, "extra_penalty") == 0) o->extra_penalty = v;
    else if (strcmp(name, "bass_bonus") == 0) o->bass_bonus = v;
    else if (strcmp(name, "w_thresh") == 0) o->w_thresh = v;
    else if (strcmp(name, "key_window") == 0) o->key_window = (int)v;
    else if (strcmp(name, "key_hold") == 0) o->key_hold = (int)v;
    else if (strcmp(name, "key_retroactive") == 0) o->key_retroactive = v != 0.0;
    else if (strcmp(name, "evidence_runs") == 0) o->evidence_runs = v != 0.0;
    else if (strcmp(name, "key_tie") == 0) o->key_tie = v;
    else if (strcmp(name, "ev_ratio") == 0) o->ev_ratio = v;
    else if (strcmp(name, "key_low_weight") == 0) o->key_low_weight = v;
    else if (strcmp(name, "key_bass_weight") == 0) o->key_bass_weight = v;
    else if (strcmp(name, "whole_bonus") == 0) o->whole_bonus = v;
    else if (strcmp(name, "fold_add9") == 0) o->fold_add9 = v != 0.0;
    else if (strcmp(name, "absorb") == 0) o->absorb = v != 0.0;
    else if (strcmp(name, "power_fallback") == 0) o->power_fallback = v != 0.0;
    else if (strcmp(name, "onset_weight") == 0) o->onset_weight = v;
    else if (strcmp(name, "phrase_weight") == 0) o->phrase_weight = v;
    else if (strcmp(name, "six_penalty") == 0) o->six_penalty = v;
    else if (strcmp(name, "pop_profile") == 0) o->pop_profile = v != 0.0;
    else if (strcmp(name, "ref") == 0) o->ref = v != 0.0;
    else return false;
    return true;
}

void chordstream_stream_free(chordstream_stream_t *s) {
    free(s->tokens);
    free(s->sidecar);
    s->tokens = NULL;
    s->sidecar = NULL;
    s->count = 0;
}

int chordstream_extract(const chordstream_note_t *notes_in, int n_notes_in,
                        const chordstream_timesig_t *sigs, int n_sigs,
                        const chordstream_options_t *opts_in,
                        chordstream_stream_t *out) {
    chordstream_options_t opts = opts_in ? *opts_in : chordstream_default_options();
    g_opts = opts;
    out->tokens = NULL;
    out->sidecar = NULL;
    out->count = 0;

    chordstream_note_t *notes = xcalloc((size_t)n_notes_in + 1, sizeof(chordstream_note_t));
    int n_notes = 0;
    double piece_end = 0.0;
    for (int i = 0; i < n_notes_in; ++i) {
        if (notes_in[i].offset - notes_in[i].onset <= CHORDSTREAM_EPSILON) continue;
        if (notes_in[i].pitch > 127 || notes_in[i].onset < -CHORDSTREAM_EPSILON) continue;
        notes[n_notes] = notes_in[i];
        if (notes[n_notes].onset < 0.0) notes[n_notes].onset = 0.0;
        if (notes[n_notes].offset > piece_end) piece_end = notes[n_notes].offset;
        ++n_notes;
    }
    if (n_notes == 0) {
        free(notes);
        return 0;
    }

    bar_t *bars = NULL;
    int n_bars = build_bars(sigs, n_sigs, piece_end, &bars);

    step_t *steps = NULL;
    int n_steps = 0, cap_steps = 0;
    for (int b = 0; b < n_bars; ++b) {
        int k = (int)floor(bars[b].len / bars[b].step + 0.5);
        if (k < 1) k = 1;
        bars[b].first_step = n_steps;
        bars[b].n_steps = k;
        for (int j = 0; j < k; ++j) {
            GROW(steps, n_steps, cap_steps);
            step_t *s = &steps[n_steps++];
            memset(s, 0, sizeof *s);
            s->start = bars[b].start + (double)j * bars[b].step;
            s->end = (j + 1 == k) ? bars[b].start + bars[b].len : bars[b].start + (double)(j + 1) * bars[b].step;
            s->bar = b;
        }
    }

    bool *melodic = xcalloc((size_t)n_notes, sizeof(bool));
    bool melody_found = chordstream_mark_melodic(notes, n_notes, melodic);
    int n_nonmelodic = 0;
    for (int i = 0; i < n_notes; ++i) if (!melodic[i]) ++n_nonmelodic;
    bool nonmelodic_10pct = (double)n_nonmelodic >= 0.10 * (double)n_notes;

    int *scratch = xcalloc((size_t)n_notes, sizeof(int));
    for (int i = 0; i < n_steps; ++i) {
        analyse_step(&steps[i], notes, n_notes, melodic, scratch);
        fit_step(&steps[i]);
    }
    resolve_pedal_points(steps, n_steps, bars, notes, n_notes, melodic);
    resolve_solo_lines(steps, n_steps, melody_found, nonmelodic_10pct);

    int *bar_key = xcalloc((size_t)n_bars, sizeof(int));
    estimate_keys(bars, n_bars, steps, n_steps, notes, n_notes, bar_key);
    for (int i = 0; i < n_steps; ++i) steps[i].key = bar_key[steps[i].bar];

    if (opts.absorb) {
        absorb_single_steps(steps, n_steps, opts.fold_add9);
    }
    span_t *spans = NULL;
    int n_spans = merge_steps(steps, n_steps, &spans);
    emit_spans(spans, n_spans, bars, n_bars, opts.fold_add9, out);

    free(spans);
    free(bar_key);
    free(scratch);
    free(melodic);
    free(steps);
    free(bars);
    free(notes);
    return out->count;
}

void remi_clock_replay(const remi_event_t *events, int n_events, double *out_clocks) {
    double current_time = 0.0;
    double latest_note_end = 0.0;
    double bar_start = 0.0;
    double current_barlen = 16.0;
    double ts_anchor = 0.0;
    bool seen_bar = false;

    for (int i = 0; i < n_events; ++i) {
        remi_event_t ev = events[i];
        switch (ev.type) {
            case REMI_TOKEN_BAR:

                if (seen_bar) {
                    bar_start += current_barlen;
                } else {
                    seen_bar = true;
                }
                current_time = bar_start;
                break;
            case REMI_TOKEN_POSITION:
                current_time = bar_start + (double)ev.val1;
                break;
            case REMI_TOKEN_PITCH:
                break;
            case REMI_TOKEN_VELOCITY:
                break;
            case REMI_TOKEN_DURATION: {
                double end = current_time + (double)ev.val1;
                if (end > latest_note_end) {
                    latest_note_end = end;
                }
                break;
            }
            case REMI_TOKEN_REST: {
                double adv_from = (current_time > latest_note_end) ? current_time : latest_note_end;
                current_time = adv_from + (double)ev.val1;

                double bars_since = floor((current_time - ts_anchor) / current_barlen + CHORDSTREAM_EPSILON);
                bar_start = ts_anchor + bars_since * current_barlen;
                seen_bar = true;
                break;
            }
            case REMI_TOKEN_TIMESIG:
                current_barlen = (double)calculate_bar_length_sixteenths(ev.val1, ev.val2);
                ts_anchor = bar_start;
                break;
            case REMI_TOKEN_TEMPO:
                if (current_time > latest_note_end) {
                    latest_note_end = current_time;
                }
                break;
        }
        out_clocks[i] = current_time;
    }
}

remi_window_span_t remi_get_window_span(const remi_event_t *events, const double *clocks, int s, int e) {
    remi_window_span_t span;
    span.start = 0.0;
    span.end = 0.0;
    span.end_is_closed = true;

    int first_pitch = -1;
    for (int i = s; i < e; ++i) {
        if (events[i].type == REMI_TOKEN_PITCH) {
            first_pitch = i;
            break;
        }
    }

    if (first_pitch != -1) {

        span.start = clocks[first_pitch];
        bool have_end = false;
        for (int i = s; i < e; ++i) {
            if (events[i].type != REMI_TOKEN_PITCH && events[i].type != REMI_TOKEN_REST) continue;
            if (!have_end || clocks[i] > span.end + CHORDSTREAM_EPSILON) {
                have_end = true;
                span.end = clocks[i];
                span.end_is_closed = (events[i].type == REMI_TOKEN_PITCH);
            } else if (fabs(clocks[i] - span.end) <= CHORDSTREAM_EPSILON && events[i].type == REMI_TOKEN_PITCH) {
                span.end_is_closed = true;
            }
        }
        return span;
    }

    int first_rest = -1;
    int last_rest = -1;
    for (int i = s; i < e; ++i) {
        if (events[i].type == REMI_TOKEN_REST) {
            if (first_rest == -1) first_rest = i;
            last_rest = i;
        }
    }

    if (first_rest != -1) {
        span.start = (first_rest > 0) ? clocks[first_rest - 1] : 0.0;
        span.end = clocks[last_rest];
        span.end_is_closed = false;
        return span;
    }

    double t_end = (e > 0) ? clocks[e - 1] : 0.0;
    span.start = t_end;
    span.end = t_end;
    span.end_is_closed = true;
    return span;
}

int remi_window_chords(const chordstream_stream_t *st, remi_window_span_t span, int *out_idx, bool *first_began_before) {
    int n = 0;
    *first_began_before = false;
    for (int i = 0; i < st->count; ++i) {
        double a = st->sidecar[i].start16, b = st->sidecar[i].end16;
        bool starts_in_time = span.end_is_closed ? (a <= span.end + CHORDSTREAM_EPSILON)
                                                 : (a < span.end - CHORDSTREAM_EPSILON);
        if (starts_in_time && b > span.start + CHORDSTREAM_EPSILON) {
            if (n == 0 && a < span.start - CHORDSTREAM_EPSILON) *first_began_before = true;
            out_idx[n++] = i;
        }
    }
    return n;
}

typedef struct {
    chordstream_note_t    *notes;
    int                    n_notes;
    chordstream_timesig_t *sigs;
    int                    n_sigs;
    int                    tpq;
    int                    n_tracks;
    int                    n_drum_notes;
} chordstream_midi_t;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static bool read_vlq(const uint8_t *d, size_t end, size_t *pos, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && *pos < end; ++i) {
        uint8_t b = d[(*pos)++];
        v = (v << 7) | (b & 0x7Fu);
        if (!(b & 0x80u)) {
            *out = v;
            return true;
        }
    }
    return false;
}

void chordstream_midi_free(chordstream_midi_t *m) {
    free(m->notes);
    free(m->sigs);
    memset(m, 0, sizeof *m);
}

static int compare_note_symusic(const void *a, const void *b) {
    const chordstream_note_t *x = (const chordstream_note_t *)a, *y = (const chordstream_note_t *)b;
    if (x->voice != y->voice) return x->voice < y->voice ? -1 : 1;
    if (x->onset != y->onset) return x->onset < y->onset ? -1 : 1;
    return (x->order > y->order) - (x->order < y->order);
}

int chordstream_read_midi(const char *path, chordstream_midi_t *out) {
    memset(out, 0, sizeof *out);
    FILE *f = cs_fopen(path, "rb");
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 14 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *d = xcalloc((size_t)sz, 1);
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); return -1; }
    fclose(f);

    size_t len = (size_t)sz;
    if (memcmp(d, "MThd", 4) != 0) { free(d); return -1; }
    uint32_t hlen = be32(d + 4);
    uint16_t ntracks = be16(d + 10);
    uint16_t division = be16(d + 12);
    if ((division & 0x8000u) || division == 0) { free(d); return -1; }
    out->tpq = division;
    double tick16 = 4.0 / (double)division;
    size_t pos = 8 + hlen;

    int cap_notes = 0, cap_sigs = 0;

    typedef struct { double onset; uint8_t ch; uint8_t pitch; uint32_t seq; } open_note_t;
    open_note_t *open_notes = NULL;
    uint32_t note_on_seq = 0;
    int n_open = 0, cap_open = 0;

    for (int trk = 0; trk < (int)ntracks && pos + 8 <= len; ++trk) {
        if (memcmp(d + pos, "MTrk", 4) != 0) break;
        uint32_t tlen = be32(d + pos + 4);
        size_t tpos = pos + 8, tend = tpos + tlen;
        if (tend > len) tend = len;
        pos = tend;
        out->n_tracks++;
        n_open = 0;

        uint64_t tick = 0;
        uint8_t status = 0;
        while (tpos < tend) {
            uint32_t delta;
            if (!read_vlq(d, tend, &tpos, &delta)) break;
            tick += delta;
            if (tpos >= tend) break;
            uint8_t b = d[tpos];
            if (b & 0x80u) { status = b; ++tpos; }

            if (status == 0xFF) {
                if (tpos >= tend) break;
                uint8_t type = d[tpos++];
                uint32_t mlen;
                if (!read_vlq(d, tend, &tpos, &mlen)) break;
                if (type == 0x58 && mlen >= 2 && tpos + 2 <= tend) {
                    GROW(out->sigs, out->n_sigs, cap_sigs);
                    out->sigs[out->n_sigs].start = (double)tick * tick16;
                    out->sigs[out->n_sigs].num = d[tpos];
                    out->sigs[out->n_sigs].den = 1 << d[tpos + 1];
                    out->n_sigs++;
                }
                tpos += mlen;
                if (type == 0x2F) break;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t slen;
                if (!read_vlq(d, tend, &tpos, &slen)) break;
                tpos += slen;
            } else if (status >= 0x80) {
                int hi = status & 0xF0, ch = status & 0x0F;
                int nbytes = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
                if (tpos + (size_t)nbytes > tend) break;
                uint8_t d1 = d[tpos] & 0x7Fu, d2 = (nbytes == 2) ? (d[tpos + 1] & 0x7Fu) : 0;
                tpos += (size_t)nbytes;
                bool is_on = (hi == 0x90 && d2 > 0);
                bool is_off = (hi == 0x80) || (hi == 0x90 && d2 == 0);
                if (is_off) {
                    for (int q = 0; q < n_open; ++q) {
                        if (open_notes[q].ch != ch || open_notes[q].pitch != d1) continue;
                        double onset = open_notes[q].onset, offset = (double)tick * tick16;
                        uint32_t seq = open_notes[q].seq;
                        memmove(open_notes + q, open_notes + q + 1, sizeof(open_note_t) * (size_t)(n_open - q - 1));
                        --n_open;
                        if (ch == 9) {
                            out->n_drum_notes++;
                        } else if (offset > onset) {
                            GROW(out->notes, out->n_notes, cap_notes);
                            out->notes[out->n_notes].onset = onset;
                            out->notes[out->n_notes].offset = offset;
                            out->notes[out->n_notes].pitch = d1;
                            out->notes[out->n_notes].voice = (uint16_t)(trk * 16 + ch);
                            out->notes[out->n_notes].order = seq;
                            out->n_notes++;
                        }
                        break;
                    }
                } else if (is_on) {
                    GROW(open_notes, n_open, cap_open);
                    open_notes[n_open].onset = (double)tick * tick16;
                    open_notes[n_open].ch = (uint8_t)ch;
                    open_notes[n_open].pitch = d1;
                    open_notes[n_open].seq = note_on_seq++;
                    n_open++;
                }
            } else {
                break;
            }
        }
    }
    free(open_notes);
    free(d);

    if (out->n_sigs > 1) {
        qsort(out->sigs, (size_t)out->n_sigs, sizeof(chordstream_timesig_t), compare_timesig);
        int w = 0;
        for (int i = 0; i < out->n_sigs; ++i) {
            if (w > 0 && fabs(out->sigs[w - 1].start - out->sigs[i].start) <= CHORDSTREAM_EPSILON) {
                out->sigs[w - 1] = out->sigs[i];
            } else {
                out->sigs[w++] = out->sigs[i];
            }
        }
        out->n_sigs = w;
    }
    return 0;
}

static const char *json_find_key_array(const char *s, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(s, pat);
    if (p == NULL) return NULL;
    p = strchr(p + strlen(pat), ':');
    if (p == NULL) return NULL;
    p = strchr(p, '[');
    return p;
}

static int json_read_rows(const char *p, int cols, double **rows) {
    int n = 0, cap = 0;
    *rows = NULL;
    if (p == NULL || *p != '[') return 0;
    ++p;
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
        if (*p != '[') break;
        ++p;
        if (n >= cap) {
            cap = cap ? cap * 2 : 256;
            *rows = xrealloc(*rows, sizeof(double) * (size_t)cols * (size_t)cap);
        }
        for (int c = 0; c < cols; ++c) {
            char *end;
            (*rows)[n * cols + c] = strtod(p, &end);
            if (end == p) { free(*rows); *rows = NULL; return 0; }
            p = end;
            while (*p == ' ' || *p == ',') ++p;
        }
        while (*p && *p != ']') ++p;
        if (*p == ']') ++p;
        ++n;
    }
    return n;
}

int chordstream_read_notes_json(const char *path, chordstream_midi_t *out) {
    memset(out, 0, sizeof *out);
    FILE *f = cs_fopen(path, "rb");
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 2 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *text = xcalloc((size_t)sz + 1, 1);
    if (fread(text, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(text); return -1; }
    fclose(f);

    double *rows = NULL;
    int n = json_read_rows(json_find_key_array(text, "notes"), 4, &rows);
    out->notes = xcalloc((size_t)n + 1, sizeof(chordstream_note_t));
    for (int i = 0; i < n; ++i) {
        double s = rows[i * 4], e = rows[i * 4 + 1];
        int pitch = (int)floor(rows[i * 4 + 2] + 0.5), voice = (int)floor(rows[i * 4 + 3] + 0.5);
        if (e <= s || pitch < 0 || pitch > 127) continue;
        out->notes[out->n_notes].onset = s;
        out->notes[out->n_notes].offset = e;
        out->notes[out->n_notes].pitch = (uint8_t)pitch;
        out->notes[out->n_notes].voice = (uint16_t)(voice < 0 ? 0 : voice);
        out->n_notes++;
    }
    free(rows);
    n = json_read_rows(json_find_key_array(text, "ts"), 3, &rows);
    out->sigs = xcalloc((size_t)n + 1, sizeof(chordstream_timesig_t));
    for (int i = 0; i < n; ++i) {
        out->sigs[out->n_sigs].start = rows[i * 3];
        out->sigs[out->n_sigs].num = (int)floor(rows[i * 3 + 1] + 0.5);
        out->sigs[out->n_sigs].den = (int)floor(rows[i * 3 + 2] + 0.5);
        out->n_sigs++;
    }
    free(rows);
    free(text);
    out->tpq = 4;
    out->n_tracks = 1;
    return 0;
}

void chordstream_print_tsv(const chordstream_stream_t *st, FILE *out) {
    fprintf(out, "tonic\tmode\tdegree\tmask\tbass\tbar\tonset\tdur\tbarlen\tcont\tstart16\tend16\ttoken\n");
    for (int i = 0; i < st->count; ++i) {
        chordstream_unpacked_t u;
        chordstream_unpack(st->tokens[i], &u);
        fprintf(out, "%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%.6f\t%.6f\t%llu\n",
                (unsigned)u.tonic, (unsigned)u.mode, (unsigned)u.degree, (unsigned)u.mask, (unsigned)u.bass,
                (unsigned)u.bar, (unsigned)u.onset, (unsigned)u.dur, (unsigned)u.barlen, (unsigned)u.cont,
                st->sidecar[i].start16, st->sidecar[i].end16, (unsigned long long)st->tokens[i]);
    }
}

static const char *PITCH_NAMES[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

static int template_from_mask(uint16_t mask) {
    for (int t = 0; t < CHORDSTREAM_MAX_TEMPLATES; ++t) {
        if (TEMPLATES[t].mask == mask) return t;
    }
    return -1;
}

void chordstream_print_stream(const chordstream_stream_t *st, FILE *out) {
    fprintf(out, "%5s %5s %5s %3s %6s %4s %6s %6s %-8s %-5s %10s %s\n",
            "idx", "bar", "onset", "dur", "barlen", "cont", "key", "degree", "chord", "bass", "start16", "token");
    for (int i = 0; i < st->count; ++i) {
        chordstream_unpacked_t u;
        chordstream_unpack(st->tokens[i], &u);
        char key[8], chord[16], degree[8], bass[8];
        snprintf(key, sizeof key, "%s%s", PITCH_NAMES[u.tonic], u.mode ? "m" : "");
        if (u.degree == CHORDSTREAM_DEGREE_SILENCE) {
            snprintf(chord, sizeof chord, "silence");
            snprintf(degree, sizeof degree, "-");
            snprintf(bass, sizeof bass, "-");
        } else if (u.degree == CHORDSTREAM_DEGREE_SOLO) {
            snprintf(chord, sizeof chord, "solo");
            snprintf(degree, sizeof degree, "-");
            snprintf(bass, sizeof bass, "-");
        } else {
            int root = (u.tonic + u.degree) % 12;
            int t = template_from_mask(u.mask);
            snprintf(chord, sizeof chord, "%s%s", PITCH_NAMES[root], t >= 0 ? TEMPLATES[t].name : "?");
            snprintf(degree, sizeof degree, "%d", u.degree);
            snprintf(bass, sizeof bass, "%s", PITCH_NAMES[(root + u.bass) % 12]);
        }
        fprintf(out, "%5d %5u %5u %3u %6u %4u %6s %6s %-8s %-5s %10.3f %016llx\n",
                i, (unsigned)u.bar, (unsigned)u.onset, (unsigned)u.dur, (unsigned)u.barlen, (unsigned)u.cont,
                key, degree, chord, bass, st->sidecar ? st->sidecar[i].start16 : 0.0,
                (unsigned long long)st->tokens[i]);
    }
}

static int run_cli(int argc, char **argv) {
    chordstream_options_t opts = chordstream_default_options();
    bool quiet = false, dump_notes = false, tsv = false;
    int files = 0;
    for (int a = 1; a < argc; ++a) {
        if (strcmp(argv[a], "--absorb") == 0) { opts.absorb = true; continue; }
        if (strcmp(argv[a], "--tuned") == 0) {
            bool ref = opts.ref;
            opts = chordstream_tuned_options();
            opts.ref = ref;
            continue;
        }
        if (strcmp(argv[a], "--ref") == 0) {
            opts.key_retroactive = true;
            opts.evidence_runs = true;
            opts.ref = true;
            continue;
        }
        if (strcmp(argv[a], "--keep-add9") == 0) { opts.fold_add9 = false; continue; }
        if (strcmp(argv[a], "--quiet") == 0) { quiet = true; continue; }
        if (strcmp(argv[a], "--tsv") == 0) { tsv = true; continue; }
        if (strcmp(argv[a], "--profiles") == 0 && a + 1 < argc) {
            if (!chordstream_load_profiles(argv[a + 1])) {
                fprintf(stderr, "cannot read key profiles from %s\n", argv[a + 1]);
                return 2;
            }
            ++a;
            continue;
        }
        if (strcmp(argv[a], "--opt") == 0 && a + 1 < argc) {
            if (!chordstream_set_option(&opts, argv[a + 1])) {
                fprintf(stderr, "unknown option %s\n", argv[a + 1]);
                return 2;
            }
            ++a;
            continue;
        }
        if (strcmp(argv[a], "--dump-notes") == 0) { dump_notes = true; continue; }
        if (strcmp(argv[a], "--help") == 0 || strcmp(argv[a], "-h") == 0) {
            printf("usage: chordstream                 run the specification test suite\n"
                   "       chordstream [options] FILE.mid|FILE.notes.json ...\n"
                   "  --tuned       the measured configuration (see chordstream_tuned_options)\n"
                   "  --ref         reproduce the Python reference (chordstream_tokens.py) token for token\n"
                   "  --absorb      enable single-step absorption (default off)\n"
                   "  --keep-add9   emit add9/madd9 instead of folding to triads\n"
                   "  --quiet       summary line per file only\n"
                   "  --tsv         machine-readable token table (no summary line)\n"
                   "  --dump-notes  print the decoded notes instead of extracting\n"
                   "  --opt k=v     experimental knob (miss_penalty, extra_penalty, bass_bonus, w_thresh,\n"
                   "                key_window, key_hold, key_retroactive, evidence_runs, key_tie, ev_ratio,\n"
                   "                key_low_weight, key_bass_weight, whole_bonus, power_fallback);\n"
                   "                defaults are the spec\n");
            return 0;
        }
        ++files;
        chordstream_midi_t midi;
        size_t alen = strlen(argv[a]);
        bool is_json = alen > 5 && strcmp(argv[a] + alen - 5, ".json") == 0;
        int rc = is_json ? chordstream_read_notes_json(argv[a], &midi) : chordstream_read_midi(argv[a], &midi);
        if (rc != 0) {
            fprintf(stderr, "%s: cannot read as a %s\n", argv[a], is_json ? "notes JSON file" : "Standard MIDI File");
            continue;
        }
        if (opts.ref && !is_json) {

            qsort(midi.notes, (size_t)midi.n_notes, sizeof(chordstream_note_t), compare_note_symusic);
        }
        if (dump_notes) {
            for (int i = 0; i < midi.n_notes; ++i) {
                printf("%.6f %.6f %u %u\n", midi.notes[i].onset, midi.notes[i].offset,
                       (unsigned)midi.notes[i].pitch, (unsigned)midi.notes[i].voice);
            }
            chordstream_midi_free(&midi);
            continue;
        }
        clock_t t0 = clock();
        chordstream_stream_t st;
        chordstream_extract(midi.notes, midi.n_notes, midi.sigs, midi.n_sigs, &opts, &st);
        double ms = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;

        int max_bar = -1;
        double key_dur[24] = {0};
        for (int i = 0; i < st.count; ++i) {
            chordstream_unpacked_t u;
            chordstream_unpack(st.tokens[i], &u);
            if ((int)u.bar > max_bar) max_bar = u.bar;
            key_dur[u.tonic + 12 * u.mode] += u.dur;
        }
        int main_key = 0;
        for (int k = 1; k < 24; ++k) if (key_dur[k] > key_dur[main_key]) main_key = k;
        if (tsv) {
            chordstream_print_tsv(&st, stdout);
            chordstream_stream_free(&st);
            chordstream_midi_free(&midi);
            continue;
        }
        printf("%s: notes=%d drums_skipped=%d sigs=%d tokens=%d bars=%d tokens/bar=%.2f main_key=%s%s time=%.1fms\n",
               argv[a], midi.n_notes, midi.n_drum_notes, midi.n_sigs, st.count, max_bar + 1,
               max_bar >= 0 ? (double)st.count / (double)(max_bar + 1) : 0.0,
               PITCH_NAMES[main_key % 12], main_key >= 12 ? "m" : "", ms);
        if (!quiet) chordstream_print_stream(&st, stdout);
        chordstream_stream_free(&st);
        chordstream_midi_free(&midi);
    }
    if (files == 0) {
        fprintf(stderr, "no input files\n");
        return 2;
    }
    return 0;
}

static int g_tests_run = 0;

#define PASS(msg) do { ++g_tests_run; printf("PASS: %s\n", msg); } while (0)

static void test_bit_packing_and_top_bit(void) {
    chordstream_unpacked_t in, out;
    in.tonic   = 11;
    in.mode    = 1;
    in.degree  = 11;
    in.mask    = 0x7FF;
    in.bass    = 11;
    in.barlen  = 64;
    in.bar     = 65535;
    in.onset   = 63;
    in.dur     = 255;
    in.cont    = 1;

    chordstream_token_t tok = chordstream_pack(&in);
    assert((tok & (1ULL << 63)) == 0);
    assert((tok & (3ULL << 30)) == 0);
    chordstream_unpack(tok, &out);

    assert(out.tonic == in.tonic);
    assert(out.mode == in.mode);
    assert(out.degree == in.degree);
    assert(out.mask == in.mask);
    assert(out.bass == in.bass);
    assert(out.barlen == in.barlen);
    assert(out.bar == in.bar);
    assert(out.onset == in.onset);
    assert(out.dur == in.dur);
    assert(out.cont == in.cont);

    memset(&in, 0, sizeof in);
    in.barlen = 1;
    in.dur = 1;
    in.tonic = 1;   assert(chordstream_pack(&in) == (1ULL | (1ULL << 54)));
    in.tonic = 0; in.mode = 1;   assert((chordstream_pack(&in) & 0xFFFFFFFFULL) == (1ULL << 4));
    in.mode = 0; in.degree = 1;  assert((chordstream_pack(&in) & 0xFFFFFFFFULL) == (1ULL << 5));
    in.degree = 0; in.mask = 1;  assert((chordstream_pack(&in) & 0xFFFFFFFFULL) == (1ULL << 9));
    in.mask = 0; in.bass = 1;    assert((chordstream_pack(&in) & 0xFFFFFFFFULL) == (1ULL << 20));
    in.bass = 0; in.barlen = 2;  assert((chordstream_pack(&in) & 0xFFFFFFFFULL) == (1ULL << 24));
    in.barlen = 1; in.bar = 1;   assert((chordstream_pack(&in) >> 32) == (1ULL | (1ULL << 22)));
    in.bar = 0; in.onset = 1;    assert((chordstream_pack(&in) >> 32) == ((1ULL << 16) | (1ULL << 22)));
    in.onset = 0; in.cont = 1;   assert((chordstream_pack(&in) >> 32) == ((1ULL << 30) | (1ULL << 22)));

    memset(&in, 0, sizeof in);
    in.barlen = 16; in.dur = 4;
    in.degree = 14; in.mask = 0; in.bass = 0;
    tok = chordstream_pack(&in);
    chordstream_unpack(tok, &out);
    assert(out.degree == 14 && out.mask == 0 && out.bass == 0);

    in.degree = 15;
    tok = chordstream_pack(&in);
    chordstream_unpack(tok, &out);
    assert(out.degree == 15 && out.mask == 0 && out.bass == 0);
    PASS("Bit packing, field positions, and 63-bit signed safety");
}

static void test_bankers_rounding(void) {
    assert(round_half_even(2.5) == 2);
    assert(round_half_even(3.5) == 4);
    assert(round_half_even(0.5) == 0);
    assert(round_half_even(1.5) == 2);
    assert(round_half_even(-2.5) == -2);
    assert(round_half_even(-3.5) == -4);
    assert(round_half_even(2.4999) == 2);
    assert(round_half_even(2.5001) == 3);
    assert(round_half_even(3.9667) == 4);
    PASS("Half-to-even rounding");
}

static void test_metre_and_chord_steps(void) {

    assert(calculate_chord_step(4, 4, 16) == 4);
    assert(calculate_chord_step(3, 4, 12) == 4);
    assert(calculate_chord_step(7, 4, 28) == 4);
    assert(calculate_chord_step(2, 2, 16) == 8);
    assert(calculate_chord_step(6, 8, 12) == 6);
    assert(calculate_chord_step(12, 8, 24) == 6);
    assert(calculate_chord_step(12, 16, 12) == 6);
    assert(calculate_chord_step(16, 16, 16) == 4);
    assert(calculate_chord_step(5, 8, 10) == 10);
    assert(calculate_chord_step(3, 8, 6) == 6);

    assert(calculate_chord_step(1, 16, 1) == 1);
    assert(calculate_chord_step(3, 16, 3) == 3);

    int count = 0;
    int sub_bars[256];

    split_long_bar(166, 4, &count, sub_bars);
    assert(count == 83);
    for (int i = 0; i < count; ++i) assert(sub_bars[i] == 8);

    split_long_bar(17, 4, &count, sub_bars);
    assert(count == 2);
    assert(sub_bars[0] == 36);
    assert(sub_bars[1] == 32);

    split_long_bar(9, 4, &count, sub_bars);  assert(count == 1 && sub_bars[0] == 36);
    split_long_bar(15, 4, &count, sub_bars); assert(count == 1 && sub_bars[0] == 60);
    split_long_bar(7, 2, &count, sub_bars);  assert(count == 1 && sub_bars[0] == 56);

    assert(is_placeholder_signature(1, 8, 100.0));
    assert(is_placeholder_signature(1, 16, 100.0));
    assert(!is_placeholder_signature(1, 8, 4.0));
    assert(!is_placeholder_signature(1, 4, 100.0));
    assert(!is_placeholder_signature(3, 8, 100.0));

    bar_t *bars = NULL;
    chordstream_timesig_t s1[1] = {{0.0, 1, 8}};
    int nb = build_bars(s1, 1, 64.0, &bars);
    assert(nb == 4 && fabs(bars[0].len - 16.0) < 1e-9 && fabs(bars[0].step - 4.0) < 1e-9);
    free(bars);

    chordstream_timesig_t s2[2] = {{0.0, 1, 4}, {4.0, 4, 4}};
    nb = build_bars(s2, 2, 36.0, &bars);
    assert(nb == 3);
    assert(fabs(bars[0].len - 4.0) < 1e-9 && fabs(bars[0].step - 4.0) < 1e-9);
    assert(fabs(bars[1].start - 4.0) < 1e-9 && fabs(bars[1].len - 16.0) < 1e-9);
    assert(fabs(bars[2].start - 20.0) < 1e-9);
    free(bars);

    chordstream_timesig_t s3[1] = {{0.0, 17, 4}};
    nb = build_bars(s3, 1, 136.0, &bars);
    assert(nb == 4);
    assert(fabs(bars[0].len - 36.0) < 1e-9 && fabs(bars[1].len - 32.0) < 1e-9);
    assert(fabs(bars[2].start - 68.0) < 1e-9 && fabs(bars[2].len - 36.0) < 1e-9);
    free(bars);
    PASS("Metre, chord steps, placeholder signatures, and long bar splits");
}

static void test_template_masks_and_folding(void) {

    assert(TEMPLATES[4].mask == 1096);
    assert(TEMPLATES[0].mask == 72);
    assert(TEMPLATES[1].mask == 68);
    for (int t = 0; t < CHORDSTREAM_MAX_TEMPLATES; ++t) {
        uint16_t m = 0;
        for (int k = 1; k < TEMPLATES[t].len; ++k) m |= (uint16_t)(1u << (TEMPLATES[t].intervals[k] - 1));
        assert(TEMPLATES[t].mask == m);
        assert(TEMPLATES[t].intervals[0] == 0);
    }

    assert(get_template_mask(14, true) == TEMPLATES[0].mask);
    assert(get_template_mask(15, true) == TEMPLATES[1].mask);
    assert(get_template_mask(14, false) == TEMPLATES[14].mask);
    assert(get_template_mask(15, false) == TEMPLATES[15].mask);
    PASS("Template intervals, 11-bit mask packing, and add9 folding");
}

static void test_chord_scoring_and_root_c_guard(void) {
    double hist[12] = {0.0};

    fit_result_t res_silence = fit_chord_step(hist, false, 0);
    assert(res_silence.is_silence);
    hist[0] = 5e-7;
    assert(fit_chord_step(hist, false, 0).is_silence);
    hist[0] = 0.0;

    hist[0] = 1.0; hist[4] = 1.0; hist[7] = 1.0;
    fit_result_t res_c = fit_chord_step(hist, true, 36);
    assert(!res_c.is_silence && res_c.root == 0 && res_c.template_idx == 0);
    assert(fabs(res_c.score - (2.0 * 1.0 - 1.0 + 0.08)) < 1e-9);

    double d_hist[12] = {0.0};
    d_hist[2] = 1.0; d_hist[6] = 1.0; d_hist[9] = 1.0;
    fit_result_t res_d = fit_chord_step(d_hist, false, 0);
    assert(res_d.root == 2 && res_d.template_idx == 0);

    double p_hist[12] = {0.0};
    p_hist[0] = 1.0; p_hist[7] = 1.0;
    fit_result_t res_p = fit_chord_step(p_hist, false, 0);
    assert(res_p.root == 0 && res_p.template_idx == 5);
    assert(fabs(res_p.score - 1.0) < 1e-9);

    double s_hist[12] = {0.0};
    s_hist[0] = 1.0; s_hist[4] = 1.0; s_hist[7] = 1.0; s_hist[10] = 1.0;
    fit_result_t res_7 = fit_chord_step(s_hist, false, 0);
    assert(res_7.root == 0 && res_7.template_idx == 2 && fabs(res_7.score - 0.94) < 1e-9);

    double t_hist[12] = {0.0};
    t_hist[0] = 1.0; t_hist[3] = 1.0; t_hist[6] = 1.0; t_hist[9] = 1.0;
    fit_result_t res_t = fit_chord_step(t_hist, false, 0);
    assert(res_t.root == 0 && res_t.template_idx == 11);
    PASS("Chord score normalisation, penalties, tie order, and staccato root-C guard");
}

static void test_key_estimation_window_and_hysteresis(void) {
    int n_bars = 10;
    assert(get_window_lo(0, n_bars) == 0);
    assert(get_window_lo(3, n_bars) == 0);
    assert(get_window_lo(4, n_bars) == 0);
    assert(get_window_lo(5, n_bars) == 1);
    assert(get_window_lo(6, n_bars) == 2);
    assert(get_window_lo(9, n_bars) == 2);
    assert(get_window_lo(2, 5) == 0);

    double c_maj_hist[12];
    for (int p = 0; p < 12; ++p) c_maj_hist[p] = AARDEN_ESSEN_MAJOR[p];
    assert(correlate_best_key(c_maj_hist, 0, 0.05) == 0);
    double g_maj_hist[12];
    for (int p = 0; p < 12; ++p) g_maj_hist[p] = AARDEN_ESSEN_MAJOR[(p - 7 + 12) % 12];
    assert(correlate_best_key(g_maj_hist, -1, 0.0) == 7);
    double a_min_hist[12];
    for (int p = 0; p < 12; ++p) a_min_hist[p] = AARDEN_ESSEN_MINOR[(p - 9 + 12) % 12];
    assert(correlate_best_key(a_min_hist, -1, 0.0) == 21);

    double scores[24];
    key_scores(c_maj_hist, 7, 10.0, scores);
    assert(argmax_key(scores) == 7);

    assert(relative_key(0) == 21 && relative_key(21) == 0);
    assert(relative_key(7) == 16 && relative_key(16) == 7);

    int winner[11] = {0, 0, 7, 7, 7, 0, 7, 7, 7, 7, 7};
    int key[11];
    apply_key_hysteresis(winner, 11, key);
    for (int i = 0; i < 9; ++i) assert(key[i] == 0);
    assert(key[9] == 7 && key[10] == 7);
    int w2[3] = {5, 0, 0};
    apply_key_hysteresis(w2, 3, key);
    assert(key[0] == 5 && key[1] == 5 && key[2] == 5);
    PASS("Key-finding window indices, profiles, whole-piece prior, and hysteresis");
}

static void test_tonic_evidence_and_relative_tie(void) {
    step_t steps[3];
    memset(steps, 0, sizeof steps);
    for (int i = 0; i < 3; ++i) {
        steps[i].kind = STEP_CHORD;
        steps[i].start = 4.0 * i;
        steps[i].end = 4.0 * (i + 1);
    }
    double ev[12];

    steps[0].root = 7; steps[1].root = 0; steps[2].root = 0;
    tonic_evidence(steps, 0, 2, ev);
    assert(fabs(ev[0] - 8.0) < 1e-9 && fabs(ev[7] - 4.0) < 1e-9);

    steps[0].root = 8; steps[1].root = 10; steps[2].root = 0;
    tonic_evidence(steps, 0, 3, ev);
    assert(fabs(ev[0] - 12.0) < 1e-9);
    assert(fabs(ev[10] - 8.0) < 1e-9);

    steps[0].root = 0; steps[1].root = 0;
    steps[2].has_bass = true; steps[2].bass_pitch = 48;
    tonic_evidence(steps, 2, 3, ev);
    assert(fabs(ev[0] - 8.0) < 1e-9);

    double hist[12] = {0};
    hist[8] = 2.0; hist[7] = 1.0;
    assert(resolve_relative_tie(0, hist, steps, 0, 0) == 21);
    hist[8] = 1.0; hist[7] = 2.0;
    assert(resolve_relative_tie(0, hist, steps, 0, 0) == 0);
    assert(resolve_relative_tie(21, hist, steps, 0, 0) == 0);

    hist[8] = 2.0; hist[7] = 1.0;
    steps[0].root = 0; steps[1].root = 0; steps[2].root = 0;
    assert(resolve_relative_tie(21, hist, steps, 0, 3) == 0);
    steps[0].root = 9; steps[1].root = 9; steps[2].root = 9;
    assert(resolve_relative_tie(0, hist, steps, 0, 3) == 21);
    PASS("Tonic evidence, closure, and relative major/minor tie resolution");
}

static void test_melody_and_weights(void) {
    assert(fabs(note_weight(59, false) - 1.5) < 1e-9);
    assert(fabs(note_weight(60, false) - 1.0) < 1e-9);
    assert(fabs(note_weight(71, false) - 1.0) < 1e-9);
    assert(fabs(note_weight(72, false) - 0.6) < 1e-9);
    assert(fabs(note_weight(40, true) - 0.25) < 1e-9);

    chordstream_note_t notes[16];
    int n = 0;
    for (int b = 0; b < 4; ++b) {
        notes[n].onset = 4.0 * b; notes[n].offset = 4.0 * b + 4.0; notes[n].pitch = 48; notes[n].voice = 0; ++n;
        notes[n].onset = 4.0 * b; notes[n].offset = 4.0 * b + 4.0; notes[n].pitch = 55; notes[n].voice = 0; ++n;
        notes[n].onset = 4.0 * b; notes[n].offset = 4.0 * b + 4.0; notes[n].pitch = 60; notes[n].voice = 0; ++n;
        notes[n].onset = 4.0 * b; notes[n].offset = 4.0 * b + 4.0; notes[n].pitch = (uint8_t)(76 + b); notes[n].voice = 1; ++n;
    }
    bool melodic[16];
    bool found = chordstream_mark_melodic(notes, n, melodic);
    assert(found);
    for (int i = 0; i < n; ++i) assert(melodic[i] == (notes[i].voice == 1));

    for (int i = 0; i < n; ++i) notes[i].voice = 0;
    found = chordstream_mark_melodic(notes, n, melodic);
    assert(!found);
    for (int i = 0; i < n; ++i) assert(melodic[i] == (notes[i].pitch >= 76));

    for (int i = 0; i < n; ++i) notes[i].voice = (notes[i].pitch >= 76) ? 1 : 0;
    for (int i = 0; i < n; ++i) if (notes[i].voice == 1) notes[i].pitch = 50;
    found = chordstream_mark_melodic(notes, n, melodic);
    assert(!found);
    PASS("Melody voice rule, top-note fallback, and register weights");
}

static int add_chord_bar(chordstream_note_t *notes, int n, int bar, const uint8_t *pitches, int count, uint8_t bass) {
    double s = 16.0 * bar, e = s + 16.0;
    for (int i = 0; i < count; ++i) {
        notes[n].onset = s; notes[n].offset = e; notes[n].pitch = pitches[i]; notes[n].voice = 0; ++n;
    }
    if (bass) {
        notes[n].onset = s; notes[n].offset = e; notes[n].pitch = bass; notes[n].voice = 1; ++n;
    }
    return n;
}

static void test_end_to_end_progression(void) {

    chordstream_note_t notes[64];
    int n = 0;
    const uint8_t C[3] = {60, 64, 67}, F[3] = {60, 65, 69}, G[3] = {59, 62, 67};
    for (int rep = 0; rep < 2; ++rep) {
        n = add_chord_bar(notes, n, rep * 4 + 0, C, 3, 36);
        n = add_chord_bar(notes, n, rep * 4 + 1, F, 3, 41);
        n = add_chord_bar(notes, n, rep * 4 + 2, G, 3, 43);
        n = add_chord_bar(notes, n, rep * 4 + 3, C, 3, 36);
    }
    chordstream_timesig_t sig = {0.0, 4, 4};
    chordstream_stream_t st;
    int count = chordstream_extract(notes, n, &sig, 1, NULL, &st);

    assert(count == 7);

    const int expect_degree[7] = {0, 5, 7, 0, 5, 7, 0};
    const int expect_bar[7]    = {0, 1, 2, 3, 5, 6, 7};
    const int expect_dur[7]    = {16, 16, 16, 32, 16, 16, 16};
    for (int i = 0; i < 7; ++i) {
        chordstream_unpacked_t u;
        chordstream_unpack(st.tokens[i], &u);
        assert((st.tokens[i] & (1ULL << 63)) == 0);
        assert(u.tonic == 0 && u.mode == 0);
        assert(u.degree == expect_degree[i]);
        assert(u.mask == TEMPLATES[0].mask);
        assert(u.bass == 0);
        assert(u.barlen == 16 && u.bar == expect_bar[i] && u.onset == 0);
        assert(u.dur == expect_dur[i] && u.cont == 0);
        assert(fabs(st.sidecar[i].start16 - 16.0 * expect_bar[i]) < 1e-9);
        assert(fabs(st.sidecar[i].end16 - (16.0 * expect_bar[i] + expect_dur[i])) < 1e-9);
    }

    int idx[8];
    bool before = false;
    remi_window_span_t w;
    w.start = 16.0; w.end = 40.0; w.end_is_closed = true;
    int m = remi_window_chords(&st, w, idx, &before);
    assert(m == 2 && idx[0] == 1 && idx[1] == 2 && !before);
    w.start = 20.0; w.end = 48.0; w.end_is_closed = false;
    m = remi_window_chords(&st, w, idx, &before);
    assert(m == 2 && idx[0] == 1 && idx[1] == 2 && before);
    w.start = 48.0; w.end = 48.0; w.end_is_closed = true;
    m = remi_window_chords(&st, w, idx, &before);
    assert(m == 1 && idx[0] == 3 && !before);
    w.start = 64.0; w.end = 72.0; w.end_is_closed = true;
    m = remi_window_chords(&st, w, idx, &before);
    assert(m == 1 && idx[0] == 3 && before);
    chordstream_stream_free(&st);

    n = 0;
    const uint8_t Cinv[2] = {67, 72};
    n = add_chord_bar(notes, n, 0, Cinv, 2, 52);
    n = add_chord_bar(notes, n, 1, G, 3, 43);
    chordstream_timesig_t sigs2[2] = {{0.0, 2, 4}, {8.0, 4, 4}};

    for (int i = 0; i < 3; ++i) notes[i].offset = 8.0;
    for (int i = 3; i < n; ++i) { notes[i].onset = 8.0; notes[i].offset = 24.0; }
    count = chordstream_extract(notes, n, sigs2, 2, NULL, &st);
    assert(count == 2);
    chordstream_unpacked_t u0, u1;
    chordstream_unpack(st.tokens[0], &u0);
    chordstream_unpack(st.tokens[1], &u1);
    assert((u0.tonic + u0.degree) % 12 == 0 && u0.bass == 4);
    assert(u0.barlen == 8 && u0.bar == 0 && u0.onset == 0 && u0.dur == 8 && u0.cont == 0);
    assert((u1.tonic + u1.degree) % 12 == 7 && u1.bass == 0);
    assert(u1.barlen == 16 && u1.bar == 1 && u1.onset == 0 && u1.dur == 16 && u1.cont == 0);
    chordstream_stream_free(&st);

    n = 0;
    n = add_chord_bar(notes, n, 0, Cinv, 2, 52);
    n = add_chord_bar(notes, n, 1, C, 3, 36);
    for (int i = 0; i < 3; ++i) notes[i].offset = 8.0;
    for (int i = 3; i < n; ++i) { notes[i].onset = 8.0; notes[i].offset = 24.0; }
    count = chordstream_extract(notes, n, sigs2, 2, NULL, &st);
    assert(count == 2);
    chordstream_unpack(st.tokens[0], &u0);
    chordstream_unpack(st.tokens[1], &u1);
    assert(u0.bass == 0 && u0.barlen == 8 && u0.dur == 8 && u0.cont == 0);
    assert(u1.bass == 0 && u1.barlen == 16 && u1.bar == 1 && u1.dur == 16 && u1.cont == 1);
    chordstream_stream_free(&st);
    PASS("End-to-end I-IV-V-I extraction, inversions, pickup bars, and window lookup");
}

static void test_silence_solo_and_trailing(void) {
    chordstream_note_t notes[32];
    int n = 0;
    const uint8_t C[3] = {60, 64, 67};
    chordstream_timesig_t sig = {0.0, 4, 4};
    chordstream_stream_t st;
    chordstream_unpacked_t u;

    n = add_chord_bar(notes, n, 0, C, 3, 36);
    n = add_chord_bar(notes, n, 2, C, 3, 36);
    for (int i = 4; i < n; ++i) notes[i].offset = 40.0;
    int count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 3);
    chordstream_unpack(st.tokens[1], &u);
    assert(u.degree == 15 && u.mask == 0 && u.bass == 0 && u.bar == 1 && u.dur == 16);
    chordstream_unpack(st.tokens[2], &u);
    assert(u.degree == 0 && u.bar == 2 && u.dur == 8);
    assert(fabs(st.sidecar[2].end16 - 40.0) < 1e-9);
    chordstream_stream_free(&st);

    n = 0;
    for (int k = 0; k < 8; ++k) {
        notes[n].onset = 2.0 * k; notes[n].offset = 2.0 * k + 2.0;
        notes[n].pitch = (k % 2 == 0) ? 72 : 74; notes[n].voice = 0; ++n;
    }
    count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 1);
    chordstream_unpack(st.tokens[0], &u);
    assert(u.degree == 14 && u.mask == 0 && u.bass == 0 && u.dur == 16);
    chordstream_stream_free(&st);

    n = 0;
    for (int k = 0; k < 12; ++k) {
        static const uint8_t arp[3] = {60, 64, 67};
        notes[n].onset = 4.0 * k / 3.0; notes[n].offset = 4.0 * (k + 1) / 3.0;
        notes[n].pitch = arp[k % 3]; notes[n].voice = 0; ++n;
    }
    count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 1);
    chordstream_unpack(st.tokens[0], &u);
    assert(u.degree != 14 && u.mask == TEMPLATES[0].mask);
    chordstream_stream_free(&st);

    n = 0;
    n = add_chord_bar(notes, n, 0, C, 3, 36);
    for (int k = 0; k < 8; ++k) {
        static const uint8_t mel[4] = {79, 81, 83, 84};
        notes[n].onset = 4.0 * k; notes[n].offset = 4.0 * k + 4.0;
        notes[n].pitch = mel[k % 4]; notes[n].voice = 2; ++n;
    }
    count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 2);
    chordstream_unpack(st.tokens[0], &u);
    assert(u.degree != 14);
    chordstream_unpack(st.tokens[1], &u);
    assert(u.degree == 14 && u.bar == 1);
    chordstream_stream_free(&st);
    PASS("Silence tokens, solo rules A and B, arpeggio guard, trailing silence dropped");
}

static void test_pedal_point(void) {

    chordstream_note_t notes[8];
    int n = 0;
    notes[n].onset = 0.0; notes[n].offset = 32.0; notes[n].pitch = 36; notes[n].voice = 1; ++n;
    const uint8_t dm[3] = {62, 65, 69};
    for (int i = 0; i < 3; ++i) {
        notes[n].onset = 0.0; notes[n].offset = 32.0; notes[n].pitch = dm[i]; notes[n].voice = 0; ++n;
    }
    chordstream_timesig_t sig = {0.0, 4, 4};
    chordstream_stream_t st;
    int count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 1);
    chordstream_unpacked_t u;
    chordstream_unpack(st.tokens[0], &u);
    int root = (u.tonic + u.degree) % 12;
    assert(root == 2);
    assert(u.mask == TEMPLATES[1].mask);
    assert(u.bass == 10);
    assert(u.dur == 32);
    chordstream_stream_free(&st);

    for (int i = 0; i < n; ++i) notes[i].offset = 28.0;
    count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 1);
    chordstream_unpack(st.tokens[0], &u);
    root = (u.tonic + u.degree) % 12;
    assert(root == 2 && u.mask == TEMPLATES[3].mask && u.bass == 10);
    assert(u.dur == 28 && u.barlen == 16 && u.cont == 0);
    chordstream_stream_free(&st);
    PASS("Pedal point removal, half-bar refit, and bass restoration");
}

static void test_merge_bass_rule(void) {

    chordstream_note_t notes[16];
    int n = 0;
    notes[n].onset = 0.0; notes[n].offset = 4.0; notes[n].pitch = 52; notes[n].voice = 0; ++n;
    notes[n].onset = 0.0; notes[n].offset = 4.0; notes[n].pitch = 67; notes[n].voice = 0; ++n;
    notes[n].onset = 0.0; notes[n].offset = 4.0; notes[n].pitch = 72; notes[n].voice = 0; ++n;
    for (int k = 0; k < 3; ++k) {
        static const uint8_t p[3] = {60, 64, 67};
        notes[n].onset = 4.0; notes[n].offset = 4.5; notes[n].pitch = p[k]; notes[n].voice = 0; ++n;
    }
    notes[n].onset = 8.0; notes[n].offset = 16.0; notes[n].pitch = 48; notes[n].voice = 0; ++n;
    notes[n].onset = 8.0; notes[n].offset = 16.0; notes[n].pitch = 64; notes[n].voice = 0; ++n;
    notes[n].onset = 8.0; notes[n].offset = 16.0; notes[n].pitch = 67; notes[n].voice = 0; ++n;

    chordstream_timesig_t sig = {0.0, 4, 4};
    chordstream_stream_t st;
    int count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 1);
    chordstream_unpacked_t u;
    chordstream_unpack(st.tokens[0], &u);
    assert((u.tonic + u.degree) % 12 == 0);
    assert(u.bass == 0);
    assert(u.dur == 16);
    chordstream_stream_free(&st);

    n = 0;
    notes[n].onset = 0.0; notes[n].offset = 16.0; notes[n].pitch = 52; notes[n].voice = 0; ++n;
    notes[n].onset = 0.0; notes[n].offset = 16.0; notes[n].pitch = 67; notes[n].voice = 0; ++n;
    notes[n].onset = 0.0; notes[n].offset = 16.0; notes[n].pitch = 72; notes[n].voice = 0; ++n;
    count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 1);
    chordstream_unpack(st.tokens[0], &u);
    assert(u.bass == 4);
    chordstream_stream_free(&st);
    PASS("Merged bass by absolute pitch, bassless steps excluded");
}

static void test_emission_splits(void) {
    chordstream_note_t notes[8];
    int n = 0;
    const uint8_t C[3] = {60, 64, 67};
    chordstream_stream_t st;
    chordstream_unpacked_t u;

    n = add_chord_bar(notes, n, 0, C, 3, 36);
    for (int i = 0; i < n; ++i) notes[i].offset = 28.0;
    chordstream_timesig_t sigs[2] = {{0.0, 4, 4}, {16.0, 3, 4}};
    int count = chordstream_extract(notes, n, sigs, 2, NULL, &st);
    assert(count == 2);
    chordstream_unpack(st.tokens[0], &u);
    assert(u.bar == 0 && u.onset == 0 && u.dur == 16 && u.barlen == 16 && u.cont == 0);
    chordstream_unpack(st.tokens[1], &u);
    assert(u.bar == 1 && u.onset == 0 && u.dur == 12 && u.barlen == 12 && u.cont == 1);
    assert(u.degree == 0 && u.mask == TEMPLATES[0].mask);
    chordstream_stream_free(&st);

    for (int i = 0; i < n; ++i) notes[i].offset = 300.0;
    chordstream_timesig_t sig = {0.0, 4, 4};
    count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 2);
    chordstream_unpack(st.tokens[0], &u);
    assert(u.dur == 255 && u.cont == 0 && u.bar == 0 && u.onset == 0);
    chordstream_unpack(st.tokens[1], &u);
    assert(u.dur == 45 && u.cont == 1 && u.bar == 15 && u.onset == 15);
    assert(fabs(st.sidecar[1].start16 - 255.0) < 1e-9 && fabs(st.sidecar[1].end16 - 300.0) < 1e-9);
    chordstream_stream_free(&st);

    n = 0;
    for (int i = 0; i < 3; ++i) {
        notes[n].onset = 119.0 * 4.0 / 120.0; notes[n].offset = 16.0; notes[n].pitch = C[i]; notes[n].voice = 0; ++n;
    }
    count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 1);
    chordstream_unpack(st.tokens[0], &u);
    assert(u.degree != 15 && u.onset == 0 && u.dur == 16);
    chordstream_stream_free(&st);

    for (int i = 0; i < n; ++i) notes[i].onset = 4.0 - 1e-7;
    count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 2);
    chordstream_unpack(st.tokens[0], &u);
    assert(u.degree == 15 && u.dur == 4);
    chordstream_unpack(st.tokens[1], &u);
    assert(u.degree != 15 && u.onset == 4 && u.dur == 12);
    chordstream_stream_free(&st);

    n = 0;
    for (int i = 0; i < 3; ++i) {
        notes[n].onset = 2.5; notes[n].offset = 4.0; notes[n].pitch = C[i]; notes[n].voice = 0; ++n;
    }
    chordstream_timesig_t sig_short = {0.0, 1, 4};
    count = chordstream_extract(notes, n, &sig_short, 1, NULL, &st);
    assert(count == 1);
    chordstream_unpack(st.tokens[0], &u);
    assert(u.onset == 0 && u.dur == 4 && u.barlen == 4);
    chordstream_stream_free(&st);
    PASS("Emission splits at bar-length change and 255, continuation flag, rounding");
}

static void test_absorption_option(void) {

    chordstream_note_t notes[32];
    int n = 0;
    const uint8_t C[3] = {60, 64, 67}, F[3] = {60, 65, 69}, G[3] = {59, 62, 67};

    for (int i = 0; i < 3; ++i) { notes[n].onset = 0.0;  notes[n].offset = 8.0;  notes[n].pitch = C[i]; notes[n].voice = 0; ++n; }
    for (int i = 0; i < 3; ++i) { notes[n].onset = 8.0;  notes[n].offset = 12.0; notes[n].pitch = F[i]; notes[n].voice = 0; ++n; }
    for (int i = 0; i < 3; ++i) { notes[n].onset = 12.0; notes[n].offset = 16.0; notes[n].pitch = C[i]; notes[n].voice = 0; ++n; }

    for (int i = 0; i < 3; ++i) { notes[n].onset = 16.0; notes[n].offset = 24.0; notes[n].pitch = C[i]; notes[n].voice = 0; ++n; }
    for (int i = 0; i < 3; ++i) { notes[n].onset = 24.0; notes[n].offset = 28.0; notes[n].pitch = G[i]; notes[n].voice = 0; ++n; }
    for (int i = 0; i < 3; ++i) { notes[n].onset = 28.0; notes[n].offset = 32.0; notes[n].pitch = C[i]; notes[n].voice = 0; ++n; }

    notes[n].onset = 0.0;  notes[n].offset = 8.0;  notes[n].pitch = 36; notes[n].voice = 1; ++n;
    notes[n].onset = 8.0;  notes[n].offset = 12.0; notes[n].pitch = 41; notes[n].voice = 1; ++n;
    notes[n].onset = 12.0; notes[n].offset = 24.0; notes[n].pitch = 36; notes[n].voice = 1; ++n;
    notes[n].onset = 24.0; notes[n].offset = 28.0; notes[n].pitch = 43; notes[n].voice = 1; ++n;
    notes[n].onset = 28.0; notes[n].offset = 32.0; notes[n].pitch = 36; notes[n].voice = 1; ++n;

    chordstream_timesig_t sig = {0.0, 4, 4};
    chordstream_stream_t st;
    chordstream_unpacked_t u;

    int count = chordstream_extract(notes, n, &sig, 1, NULL, &st);
    assert(count == 5);
    chordstream_unpack(st.tokens[1], &u); assert(u.tonic == 0 && u.degree == 5);
    chordstream_unpack(st.tokens[3], &u); assert(u.degree == 7);
    chordstream_stream_free(&st);

    chordstream_options_t o = chordstream_default_options();
    o.absorb = true;
    count = chordstream_extract(notes, n, &sig, 1, &o, &st);
    assert(count == 3);
    chordstream_unpack(st.tokens[0], &u); assert(u.degree == 0 && u.dur == 24);
    chordstream_unpack(st.tokens[1], &u); assert(u.degree == 7 && u.dur == 4);
    chordstream_unpack(st.tokens[2], &u); assert(u.degree == 0 && u.dur == 4);
    chordstream_stream_free(&st);
    PASS("Optional single-step absorption with dominant-function exceptions");
}

static void test_remi_clock_and_asymmetric_windows(void) {

    remi_event_t stream[7];
    memset(stream, 0, sizeof stream);
    stream[0].type = REMI_TOKEN_TIMESIG; stream[0].val1 = 4; stream[0].val2 = 4;
    stream[1].type = REMI_TOKEN_BAR;
    stream[2].type = REMI_TOKEN_POSITION; stream[2].val1 = 0;
    stream[3].type = REMI_TOKEN_PITCH;    stream[3].val1 = 60;
    stream[4].type = REMI_TOKEN_DURATION; stream[4].val1 = 8;
    stream[5].type = REMI_TOKEN_TEMPO;    stream[5].val1 = 120;
    stream[6].type = REMI_TOKEN_REST;     stream[6].val1 = 4;

    double clocks[7];
    remi_clock_replay(stream, 7, clocks);

    assert(fabs(clocks[1] - 0.0) < CHORDSTREAM_EPSILON);
    assert(fabs(clocks[3] - 0.0) < CHORDSTREAM_EPSILON);
    assert(fabs(clocks[5] - 0.0) < CHORDSTREAM_EPSILON);
    assert(fabs(clocks[6] - 12.0) < CHORDSTREAM_EPSILON);

    remi_window_span_t span_onset = remi_get_window_span(stream, clocks, 1, 5);
    assert(fabs(span_onset.start - 0.0) < CHORDSTREAM_EPSILON);
    assert(fabs(span_onset.end - 0.0) < CHORDSTREAM_EPSILON);
    assert(span_onset.end_is_closed == true);

    remi_window_span_t span_rest = remi_get_window_span(stream, clocks, 1, 7);
    assert(fabs(span_rest.start - 0.0) < CHORDSTREAM_EPSILON);
    assert(fabs(span_rest.end - 12.0) < CHORDSTREAM_EPSILON);
    assert(span_rest.end_is_closed == false);

    remi_event_t pitchless_stream[2];
    memset(pitchless_stream, 0, sizeof pitchless_stream);
    pitchless_stream[0].type = REMI_TOKEN_REST; pitchless_stream[0].val1 = 4;
    pitchless_stream[1].type = REMI_TOKEN_REST; pitchless_stream[1].val1 = 4;
    double p_clocks[2] = {4.0, 8.0};
    remi_window_span_t span_p_rest = remi_get_window_span(pitchless_stream, p_clocks, 0, 2);
    assert(fabs(span_p_rest.start - 0.0) < CHORDSTREAM_EPSILON);
    assert(fabs(span_p_rest.end - 8.0) < CHORDSTREAM_EPSILON);
    assert(span_p_rest.end_is_closed == false);

    remi_event_t bars_only[2];
    memset(bars_only, 0, sizeof bars_only);
    bars_only[0].type = REMI_TOKEN_BAR;
    bars_only[1].type = REMI_TOKEN_BAR;
    double b_clocks[2] = {16.0, 32.0};
    remi_window_span_t span_b = remi_get_window_span(bars_only, b_clocks, 0, 2);
    assert(fabs(span_b.start - 32.0) < CHORDSTREAM_EPSILON && fabs(span_b.end - 32.0) < CHORDSTREAM_EPSILON);
    assert(span_b.end_is_closed == true);

    remi_event_t s2[12];
    memset(s2, 0, sizeof s2);
    int k = 0;
    s2[k].type = REMI_TOKEN_TIMESIG;  s2[k].val1 = 4; s2[k].val2 = 4; ++k;
    s2[k].type = REMI_TOKEN_BAR;      ++k;
    s2[k].type = REMI_TOKEN_POSITION; s2[k].val1 = 0; ++k;
    s2[k].type = REMI_TOKEN_PITCH;    s2[k].val1 = 60; ++k;
    s2[k].type = REMI_TOKEN_DURATION; s2[k].val1 = 16; ++k;
    s2[k].type = REMI_TOKEN_BAR;      ++k;
    s2[k].type = REMI_TOKEN_TIMESIG;  s2[k].val1 = 3; s2[k].val2 = 4; ++k;
    s2[k].type = REMI_TOKEN_POSITION; s2[k].val1 = 0; ++k;
    s2[k].type = REMI_TOKEN_PITCH;    s2[k].val1 = 62; ++k;
    s2[k].type = REMI_TOKEN_DURATION; s2[k].val1 = 4; ++k;
    s2[k].type = REMI_TOKEN_REST;     s2[k].val1 = 20; ++k;
    s2[k].type = REMI_TOKEN_BAR;      ++k;
    double c2[12];
    remi_clock_replay(s2, 12, c2);
    assert(fabs(c2[1] - 0.0) < CHORDSTREAM_EPSILON);
    assert(fabs(c2[5] - 16.0) < CHORDSTREAM_EPSILON);
    assert(fabs(c2[8] - 16.0) < CHORDSTREAM_EPSILON);
    assert(fabs(c2[10] - 40.0) < CHORDSTREAM_EPSILON);
    assert(fabs(c2[11] - 52.0) < CHORDSTREAM_EPSILON);

    remi_window_span_t w2 = remi_get_window_span(s2, c2, 1, 12);
    assert(fabs(w2.start - 0.0) < CHORDSTREAM_EPSILON && fabs(w2.end - 40.0) < CHORDSTREAM_EPSILON);
    assert(w2.end_is_closed == false);

    remi_window_span_t w3 = remi_get_window_span(s2, c2, 1, 10);
    assert(fabs(w3.end - 16.0) < CHORDSTREAM_EPSILON && w3.end_is_closed == true);
    PASS("REMI clock replay, first-bar anchor, time-signature re-anchoring, window bounds");
}

#ifndef CHORDSTREAM_NO_MAIN
static int real_main(int argc, char **argv);

#ifdef _MSC_VER
int wmain(int argc, wchar_t **wargv) {
    char **argv = malloc(sizeof(char *) * (size_t)(argc + 1));
    if (argv == NULL) return 1;
    for (int i = 0; i < argc; ++i) argv[i] = wide_to_utf8(wargv[i]);
    argv[argc] = NULL;
    int rc = real_main(argc, argv);
    for (int i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
    return rc;
}
#else
int main(int argc, char **argv) {
    return real_main(argc, argv);
}
#endif

static int real_main(int argc, char **argv) {
    if (argc > 1) {
        return run_cli(argc, argv);
    }
    printf("Running ChordStream specification test suite...\n");

    test_bit_packing_and_top_bit();
    test_bankers_rounding();
    test_metre_and_chord_steps();
    test_template_masks_and_folding();
    test_chord_scoring_and_root_c_guard();
    test_key_estimation_window_and_hysteresis();
    test_tonic_evidence_and_relative_tie();
    test_melody_and_weights();
    test_end_to_end_progression();
    test_silence_solo_and_trailing();
    test_pedal_point();
    test_merge_bass_rule();
    test_emission_splits();
    test_absorption_option();
    test_remi_clock_and_asymmetric_windows();

    printf("\nAll ChordStream specification tests passed (%d/%d).\n", g_tests_run, g_tests_run);
    return 0;
}
#endif
