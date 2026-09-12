/*
 * fam_filter.c -- family E, "filters, kernel engine" (12 commands).
 *
 *   px-convolve  px-blur-box  px-blur-gauss  px-sharpen  px-unsharp  px-edge
 *   px-emboss    px-median    px-erode       px-dilate   px-noise    px-dither
 *
 * Everything here is built on two engines:
 *
 *   1. the fixed-point kernel engine (krn_stage) used by every convolution
 *      style command, and
 *   2. a small rank-filter engine (rank_stage) used by median / erode / dilate.
 *
 * ------------------------------------------------------------------------
 * EDGE SAMPLING
 * ------------------------------------------------------------------------
 * Both engines sample with CLAMP-TO-EDGE: a tap that falls outside the canvas
 * reads the nearest in-canvas pixel (the coordinate is clamped independently
 * on each axis, so corners repeat the corner pixel).  No wrap, no zero pad,
 * no window shrinking; every output pixel sees a full-size window.
 *
 * ------------------------------------------------------------------------
 * ROUNDING SCHEDULE (planted edge E04)
 * ------------------------------------------------------------------------
 * A "stage" is one pass of the kernel engine (one convolution, or one axis of
 * a separable blur), or one explicit combine step.  Within a stage the taps
 * are multiplied and accumulated into a SIGNED 64-BIT accumulator with no
 * intermediate rounding at all.  The accumulator is rounded exactly ONCE, at
 * the end of the stage, HALF AWAY FROM ZERO:
 *
 *     acc >= 0 :   (  acc  + (1 << (shift-1))) >> shift
 *     acc <  0 :  -(((-acc) + (1 << (shift-1))) >> shift)
 *
 * The negative branch is the whole point: a negative accumulator is NEVER fed
 * to an arithmetic right shift.  For acc = -384, shift = 8 the rule above
 * gives -2, whereas the naive (acc + 128) >> 8 gives -1 because >> rounds
 * toward negative infinity.  For a divisor that is not a power of two (the box
 * blur divides by 2r+1) the identical rule is written as a division:
 *
 *     acc >= 0 :   (  acc  + den/2) / den
 *     acc <  0 :  -(((-acc) + den/2) / den)
 *
 * The two forms agree exactly when den == 1 << shift.
 *
 * Only after that single rounding is --bias added, and only then is the value
 * clamped to 0..255 (E03: clamp, never wrap, never truncate to a narrower
 * range).  Per-command schedules:
 *
 *   px-convolve    1 stage,  shift = --shift (default 8, i.e. Q8.8 taps)
 *   px-blur-box    2 stages, horizontal then vertical, den = 2r+1 each
 *   px-blur-gauss  2 stages, horizontal then vertical, shift = 2r each
 *   px-sharpen     1 stage,  shift = 8
 *   px-unsharp     3 stages, gauss H (2r), gauss V (2r), combine (8)
 *   px-edge        2 stages, Sobel gx and gy, shift = 0 each, then |gx|+|gy|
 *   px-emboss      1 stage,  shift = 0, bias = 128
 *   px-median      rank filter, no division
 *   px-erode       rank filter, no division
 *   px-dilate      rank filter, no division
 *   px-noise       additive LCG draw, no division
 *   px-dither      quantise, then one round-half-up reconstruction
 *
 * ------------------------------------------------------------------------
 * ALPHA
 * ------------------------------------------------------------------------
 * None of these twelve commands is an alpha operation, so ALPHA IS LEFT
 * UNTOUCHED by all of them: the engines read and write only the colour
 * channels (0..cc-1) and copy the alpha byte through byte-for-byte.  Mask
 * records are not filtered either -- only KIND_RASTER records are targets.
 *
 * ------------------------------------------------------------------------
 * TARGET SELECTION
 * ------------------------------------------------------------------------
 * Every command takes the family conventions --index I (default 0) and the
 * --all switch; they are mutually exclusive.  --all visits every raster
 * record in ascending index order.
 */

#define KRN_MAX_SIDE 65 /* widest separable kernel: box radius 32 -> 65 taps */

/* ------------------------------------------------------------------ */
/* Rounding primitives (E04)                                           */
/* ------------------------------------------------------------------ */

/* Round acc / 2^shift, half away from zero.  Written in the shift form the
   engine actually uses; the negative branch shifts the magnitude, never the
   negative value itself. */
static int64_t krn_shift_round(int64_t acc, int shift) {
    int64_t half;
    if (shift <= 0) return acc;
    half = (int64_t)1 << (shift - 1);
    if (acc >= 0) return (acc + half) >> shift;
    return -(((-acc) + half) >> shift);
}

/* Round acc / den (den > 0), half away from zero.  Identical to
   krn_shift_round when den is a power of two. */
static int64_t krn_div_round_signed(int64_t acc, int64_t den) {
    int64_t half = den / 2;
    if (acc >= 0) return (acc + half) / den;
    return -(((-acc) + half) / den);
}

/* exact log2 of a positive power of two, else -1 */
static int krn_log2_exact(int64_t den) {
    int s = 0;
    if (den <= 0) return -1;
    while ((den & 1) == 0) { den >>= 1; s++; }
    return den == 1 ? s : -1;
}

static uint8_t krn_clamp255(int64_t v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* ------------------------------------------------------------------ */
/* Sampling and the kernel engine                                      */
/* ------------------------------------------------------------------ */

/* Clamp-to-edge sample of colour channel c at (x,y). */
static int krn_sample(const uint8_t *src, uint32_t w, uint32_t h, int ch, long x, long y, int c) {
    if (x < 0) x = 0;
    else if (x >= (long)w) x = (long)w - 1;
    if (y < 0) y = 0;
    else if (y >= (long)h) y = (long)h - 1;
    return src[((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c];
}

/*
 * One kernel stage.  src and dst are distinct w*h*ch interleaved buffers.
 * kw and kh are the odd kernel side lengths (kh == 1 or kw == 1 for the
 * separable passes).  den is the stage divisor.  One rounding, half away
 * from zero, then + bias, then clamp 0..255.  Alpha copied through.
 */
static void krn_stage(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h, int ch, int cc,
                      const int32_t *k, int kw, int kh, int64_t den, int bias) {
    int shift = krn_log2_exact(den);
    int ax = kw / 2, ay = kh / 2;
    uint32_t x, y;
    int c, ki, kj;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t o = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch;
            for (c = 0; c < cc; c++) {
                int64_t acc = 0, v;
                for (kj = 0; kj < kh; kj++)
                    for (ki = 0; ki < kw; ki++) {
                        int32_t kv = k[kj * kw + ki];
                        if (kv == 0) continue;
                        acc += (int64_t)kv * krn_sample(src, w, h, ch, (long)x + ki - ax,
                                                        (long)y + kj - ay, c);
                    }
                v = (shift >= 0 ? krn_shift_round(acc, shift) : krn_div_round_signed(acc, den));
                dst[o + (size_t)c] = krn_clamp255(v + bias);
            }
            for (c = cc; c < ch; c++) dst[o + (size_t)c] = src[o + (size_t)c];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Rank-filter engine (median / erode / dilate)                        */
/* ------------------------------------------------------------------ */

#define RANK_MEDIAN 0
#define RANK_MIN 1 /* erode */
#define RANK_MAX 2 /* dilate */

/*
 * Square window of side 2r+1, clamp-to-edge, each colour channel ranked
 * independently.  No division, so no rounding is involved.  Median uses a
 * 256-bin counting sort and takes element n/2 of the sorted window (n is
 * always odd, so the middle is exact).
 */
static void rank_stage(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h, int ch, int cc,
                       int r, int mode) {
    int n = (2 * r + 1) * (2 * r + 1);
    int half = n / 2;
    uint32_t x, y;
    int c, i, j;
    int hist[256];
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t o = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch;
            for (c = 0; c < cc; c++) {
                int out = 0;
                if (mode == RANK_MEDIAN) {
                    int seen = 0;
                    memset(hist, 0, sizeof hist);
                    for (j = -r; j <= r; j++)
                        for (i = -r; i <= r; i++)
                            hist[krn_sample(src, w, h, ch, (long)x + i, (long)y + j, c)]++;
                    for (i = 0; i < 256; i++) {
                        seen += hist[i];
                        if (seen > half) { out = i; break; }
                    }
                } else {
                    out = mode == RANK_MIN ? 255 : 0;
                    for (j = -r; j <= r; j++)
                        for (i = -r; i <= r; i++) {
                            int v = krn_sample(src, w, h, ch, (long)x + i, (long)y + j, c);
                            if (mode == RANK_MIN) { if (v < out) out = v; }
                            else if (v > out) out = v;
                        }
                }
                dst[o + (size_t)c] = (uint8_t)out;
            }
            for (c = cc; c < ch; c++) dst[o + (size_t)c] = src[o + (size_t)c];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Target selection: --index / --all                                   */
/* ------------------------------------------------------------------ */

typedef void (*FltFn)(const Doc *d, Layer *L, void *p);

static void flt_run(Doc *d, const Args *a, FltFn fn, void *p) {
    int nl = (int)d->nlayers, i, hit = 0;
    if (arg_get(a, "all") && arg_get(a, "index"))
        die(EXIT_USAGE, "E_BAD_ARGS", "--index and --all are mutually exclusive");
    if (arg_int(a, "all", 0, 0, 1)) {
        for (i = 0; i < nl; i++)
            if (d->layers[i].kind == KIND_RASTER) {
                fn(d, &d->layers[i], p);
                hit++;
            }
        if (!hit) die(EXIT_USAGE, "E_BAD_ARGS", "--all: document contains no raster layer");
    } else {
        i = (int)arg_int(a, "index", 0, 0, MAX_LAYERS);
        if (i >= nl || d->layers[i].kind != KIND_RASTER)
            die(EXIT_USAGE, "E_BAD_ARGS", "--index must be the index of a raster layer");
        fn(d, &d->layers[i], p);
    }
}

/* fresh scratch buffer the size of a layer payload */
static uint8_t *flt_scratch(const Layer *L) { return (uint8_t *)xmalloc(L->data_len ? L->data_len : 1); }

static void flt_adopt(Layer *L, uint8_t *nd) {
    layer_free(L);
    L->data = nd;
}

/* ------------------------------------------------------------------ */
/* Shared parameter blocks and callbacks                               */
/* ------------------------------------------------------------------ */

typedef struct { /* one non-separable convolution stage */
    const int32_t *k;
    int kw, kh;
    int64_t den;
    int bias;
} KrnP;

static void cb_conv(const Doc *d, Layer *L, void *vp) {
    KrnP *p = (KrnP *)vp;
    uint8_t *nd = flt_scratch(L);
    krn_stage(L->data, nd, d->w, d->h, doc_channels(d), doc_color_channels(d), p->k, p->kw, p->kh,
              p->den, p->bias);
    flt_adopt(L, nd);
}

/* Fill k[0..2r] with the binomial row C(2r, j); the row sums to 2^(2r). */
static void krn_binomial(int32_t *k, int r) {
    int n = 2 * r, i, j;
    for (i = 0; i <= n; i++) k[i] = 0;
    k[0] = 1;
    for (i = 1; i <= n; i++)
        for (j = i; j >= 1; j--) k[j] += k[j - 1];
}

/* Two-pass separable blur: horizontal stage then vertical stage, each rounded
   once by the E04 rule.  Returns with the result installed on the layer. */
static void flt_separable(const Doc *d, Layer *L, const int32_t *k, int side, int64_t den) {
    int ch = doc_channels(d), cc = doc_color_channels(d);
    uint8_t *t1 = flt_scratch(L), *t2 = flt_scratch(L);
    krn_stage(L->data, t1, d->w, d->h, ch, cc, k, side, 1, den, 0); /* stage 1: horizontal */
    krn_stage(t1, t2, d->w, d->h, ch, cc, k, 1, side, den, 0);      /* stage 2: vertical   */
    free(t1);
    flt_adopt(L, t2);
}

/* ------------------------------------------------------------------ */
/* 65. px-convolve                                                     */
/* ------------------------------------------------------------------ */

/* parse "a,b,c,..." into out[]; returns the count */
static int krn_parse_list(const char *s, int32_t *out, int maxn) {
    const char *p = s;
    int n = 0;
    for (;;) {
        char *end;
        long v;
        if (n >= maxn) die(EXIT_USAGE, "E_BAD_ARGS", "--kernel has too many values");
        v = strtol(p, &end, 10);
        if (end == p) die(EXIT_USAGE, "E_BAD_ARGS", "--kernel must be comma-separated integers");
        if (v < -32768 || v > 32767)
            die(EXIT_USAGE, "E_BAD_ARGS", "--kernel values must be -32768..32767");
        out[n++] = (int32_t)v;
        p = end;
        if (*p == 0) break;
        if (*p != ',') die(EXIT_USAGE, "E_BAD_ARGS", "--kernel must be comma-separated integers");
        p++;
    }
    return n;
}

static int cmd_px_convolve(int argc, char **argv) {
    static const char *allowed[] = {"kernel", "shift", "bias", "index", "all"};
    Args a;
    Doc *d;
    KrnP p;
    int32_t k[25];
    int n, side, shift, bias;
    parse_args(argc, argv, 2, &a, allowed, 5);
    positional(&a, 2);
    n = krn_parse_list(arg_req(&a, "kernel"), k, 25);
    if (n != 9 && n != 25) die(EXIT_USAGE, "E_BAD_ARGS", "--kernel must have 9 or 25 values");
    side = n == 9 ? 3 : 5;
    /* Q8.8 taps by default: shift 8 means the coefficients are 256ths. */
    shift = (int)arg_int(&a, "shift", 8, 0, 30);
    bias = (int)arg_int(&a, "bias", 0, -255, 255);
    d = ldx_read(a.pos[0]);
    p.k = k;
    p.kw = side;
    p.kh = side;
    p.den = (int64_t)1 << shift;
    p.bias = bias;
    flt_run(d, &a, cb_conv, &p);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 66. px-blur-box                                                     */
/* ------------------------------------------------------------------ */

/* Radius plus one command-specific integer: unsharp strength (Q8.8), rank mode
   (RANK_*), or dither level count. */
typedef struct {
    int r;
    int amount;
} RadP;

static void cb_box(const Doc *d, Layer *L, void *vp) {
    RadP *p = (RadP *)vp;
    int32_t k[KRN_MAX_SIDE];
    int side = 2 * p->r + 1, i;
    for (i = 0; i < side; i++) k[i] = 1;
    /* divisor 2r+1 per axis; not a power of two, so the division form of the
       E04 rule is used.  Two stages, one rounding each. */
    flt_separable(d, L, k, side, (int64_t)side);
}

static int cmd_px_blur_box(int argc, char **argv) {
    static const char *allowed[] = {"radius", "index", "all"};
    Args a;
    Doc *d;
    RadP p;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    p.r = (int)arg_int(&a, "radius", 1, 0, 32); /* radius 0 is the identity kernel {1}/1 */
    p.amount = 0;
    d = ldx_read(a.pos[0]);
    flt_run(d, &a, cb_box, &p);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 67. px-blur-gauss                                                   */
/* ------------------------------------------------------------------ */

static void cb_gauss(const Doc *d, Layer *L, void *vp) {
    RadP *p = (RadP *)vp;
    int32_t k[KRN_MAX_SIDE];
    int side = 2 * p->r + 1;
    krn_binomial(k, p->r);
    /* binomial row C(2r,j) sums to exactly 2^(2r): a power-of-two divisor, so
       the shift form of the E04 rule applies.  Two stages, one rounding each. */
    flt_separable(d, L, k, side, (int64_t)1 << (2 * p->r));
}

static int cmd_px_blur_gauss(int argc, char **argv) {
    static const char *allowed[] = {"radius", "index", "all"};
    Args a;
    Doc *d;
    RadP p;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    p.r = (int)arg_int(&a, "radius", 1, 0, 8); /* r <= 8: widest binomial row is C(16,j) */
    p.amount = 0;
    d = ldx_read(a.pos[0]);
    flt_run(d, &a, cb_gauss, &p);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 68. px-sharpen                                                      */
/* ------------------------------------------------------------------ */

static int cmd_px_sharpen(int argc, char **argv) {
    static const char *allowed[] = {"amount", "index", "all"};
    Args a;
    Doc *d;
    KrnP p;
    int32_t k[9];
    int amount;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    /* --amount is Q8.8: 256 == 1.0, the classic unit-strength sharpen. */
    amount = (int)arg_int(&a, "amount", 256, 0, 1024);
    /*      0        -amount       0
     *   -amount  256+4*amount  -amount     / 256   (taps sum to 256: DC preserved)
     *      0        -amount       0
     */
    k[0] = 0;       k[1] = -amount;         k[2] = 0;
    k[3] = -amount; k[4] = 256 + 4 * amount; k[5] = -amount;
    k[6] = 0;       k[7] = -amount;         k[8] = 0;
    d = ldx_read(a.pos[0]);
    p.k = k;
    p.kw = 3;
    p.kh = 3;
    p.den = 256; /* shift 8 */
    p.bias = 0;
    flt_run(d, &a, cb_conv, &p);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 69. px-unsharp                                                      */
/* ------------------------------------------------------------------ */

static void cb_unsharp(const Doc *d, Layer *L, void *vp) {
    RadP *p = (RadP *)vp;
    int ch = doc_channels(d), cc = doc_color_channels(d);
    int32_t k[KRN_MAX_SIDE];
    int side = 2 * p->r + 1;
    uint8_t *orig = flt_scratch(L), *t1 = flt_scratch(L), *t2 = flt_scratch(L);
    size_t npx = (size_t)d->w * (size_t)d->h, q;
    int c;
    memcpy(orig, L->data, L->data_len);
    krn_binomial(k, p->r);
    /* stages 1 and 2: separable binomial blur, shift 2r, one rounding each */
    krn_stage(orig, t1, d->w, d->h, ch, cc, k, side, 1, (int64_t)1 << (2 * p->r), 0);
    krn_stage(t1, t2, d->w, d->h, ch, cc, k, 1, side, (int64_t)1 << (2 * p->r), 0);
    /* stage 3: out = orig + round((orig - blur) * amount / 256), half away from
       zero -- the difference is routinely negative, which is exactly where the
       E04 rule differs from an arithmetic shift.  Then clamp 0..255. */
    for (q = 0; q < npx; q++) {
        for (c = 0; c < cc; c++) {
            size_t o = q * (size_t)ch + (size_t)c;
            int64_t diff = (int64_t)orig[o] - (int64_t)t2[o];
            int64_t delta = krn_shift_round(diff * (int64_t)p->amount, 8);
            t2[o] = krn_clamp255((int64_t)orig[o] + delta);
        }
        for (c = cc; c < ch; c++) t2[q * (size_t)ch + (size_t)c] = orig[q * (size_t)ch + (size_t)c];
    }
    free(orig);
    free(t1);
    flt_adopt(L, t2);
}

static int cmd_px_unsharp(int argc, char **argv) {
    static const char *allowed[] = {"radius", "amount", "index", "all"};
    Args a;
    Doc *d;
    RadP p;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    p.r = (int)arg_int(&a, "radius", 1, 0, 8);
    p.amount = (int)arg_int(&a, "amount", 256, 0, 1024); /* Q8.8, 256 == 1.0 */
    d = ldx_read(a.pos[0]);
    flt_run(d, &a, cb_unsharp, &p);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 70. px-edge                                                         */
/* ------------------------------------------------------------------ */

static const int32_t SOBEL_GX[9] = {-1, 0, 1, -2, 0, 2, -1, 0, 1};
static const int32_t SOBEL_GY[9] = {-1, -2, -1, 0, 0, 0, 1, 2, 1};

static void cb_edge(const Doc *d, Layer *L, void *vp) {
    int ch = doc_channels(d), cc = doc_color_channels(d);
    uint8_t *nd = flt_scratch(L);
    uint32_t x, y;
    int c, ki, kj;
    (void)vp;
    for (y = 0; y < d->h; y++) {
        for (x = 0; x < d->w; x++) {
            size_t o = ((size_t)y * (size_t)d->w + (size_t)x) * (size_t)ch;
            for (c = 0; c < cc; c++) {
                int64_t gx = 0, gy = 0;
                for (kj = 0; kj < 3; kj++)
                    for (ki = 0; ki < 3; ki++) {
                        int v = krn_sample(L->data, d->w, d->h, ch, (long)x + ki - 1,
                                           (long)y + kj - 1, c);
                        gx += (int64_t)SOBEL_GX[kj * 3 + ki] * v;
                        gy += (int64_t)SOBEL_GY[kj * 3 + ki] * v;
                    }
                /* two stages, shift 0 (the Sobel taps carry no scale), so each
                   rounding is the identity; then |gx| + |gy| clamped 0..255. */
                gx = krn_shift_round(gx, 0);
                gy = krn_shift_round(gy, 0);
                if (gx < 0) gx = -gx;
                if (gy < 0) gy = -gy;
                nd[o + (size_t)c] = krn_clamp255(gx + gy);
            }
            for (c = cc; c < ch; c++) nd[o + (size_t)c] = L->data[o + (size_t)c];
        }
    }
    flt_adopt(L, nd);
}

static int cmd_px_edge(int argc, char **argv) {
    static const char *allowed[] = {"index", "all"};
    Args a;
    Doc *d;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    flt_run(d, &a, cb_edge, NULL);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 71. px-emboss                                                       */
/* ------------------------------------------------------------------ */

static const char *EMBOSS_DIRS[8] = {"n", "ne", "e", "se", "s", "sw", "w", "nw"};
static const int EMBOSS_DX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int EMBOSS_DY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

static int cmd_px_emboss(int argc, char **argv) {
    static const char *allowed[] = {"dir", "index", "all"};
    Args a;
    Doc *d;
    KrnP p;
    int32_t k[9];
    const char *dir;
    int di = -1, i, u, v;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    dir = arg_get(&a, "dir") ? arg_get(&a, "dir") : "nw";
    for (i = 0; i < 8; i++)
        if (strcmp(dir, EMBOSS_DIRS[i]) == 0) di = i;
    if (di < 0) die(EXIT_USAGE, "E_BAD_ARGS", "--dir must be n, ne, e, se, s, sw, w or nw");
    /* tap at offset (u,v) is u*dx + v*dy; the taps sum to 0, so flat areas land
       on the bias, 128. */
    for (v = -1; v <= 1; v++)
        for (u = -1; u <= 1; u++) k[(v + 1) * 3 + (u + 1)] = (int32_t)(u * EMBOSS_DX[di] + v * EMBOSS_DY[di]);
    d = ldx_read(a.pos[0]);
    p.k = k;
    p.kw = 3;
    p.kh = 3;
    p.den = 1; /* shift 0 */
    p.bias = 128;
    flt_run(d, &a, cb_conv, &p);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 72-74. px-median, px-erode, px-dilate                               */
/* ------------------------------------------------------------------ */

static void cb_rank(const Doc *d, Layer *L, void *vp) {
    RadP *p = (RadP *)vp;
    uint8_t *nd = flt_scratch(L);
    rank_stage(L->data, nd, d->w, d->h, doc_channels(d), doc_color_channels(d), p->r, p->amount);
    flt_adopt(L, nd);
}

static int flt_rank_cmd(int argc, char **argv, int mode) {
    static const char *allowed[] = {"radius", "index", "all"};
    Args a;
    Doc *d;
    RadP p;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    p.r = (int)arg_int(&a, "radius", 1, 0, 16);
    p.amount = mode;
    d = ldx_read(a.pos[0]);
    flt_run(d, &a, cb_rank, &p);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

static int cmd_px_median(int argc, char **argv) { return flt_rank_cmd(argc, argv, RANK_MEDIAN); }
static int cmd_px_erode(int argc, char **argv) { return flt_rank_cmd(argc, argv, RANK_MIN); }
static int cmd_px_dilate(int argc, char **argv) { return flt_rank_cmd(argc, argv, RANK_MAX); }

/* ------------------------------------------------------------------ */
/* 75. px-noise                                                        */
/* ------------------------------------------------------------------ */

/*
 * Explicit integer LCG -- no library rand(), no floating point, so the output
 * is reproducible byte-for-byte for a given --seed.
 *
 *   state_0    = seed * 1664525 + 1013904223            (mod 2^32)
 *   state_n+1  = state_n * 1664525 + 1013904223         (mod 2^32)
 *   draw       = (state_n+1 >> 16) & 0x7fff             (bits 16..30)
 *   delta      = (draw % (2*amount + 1)) - amount       (in -amount..+amount)
 *
 * The multiplier 1664525 and increment 1013904223 are the Numerical Recipes
 * ranqd1 constants (a full-period LCG modulo 2^32).  Only the high bits are
 * used because the low bits of a power-of-two-modulus LCG have short periods.
 *
 * Draw order is fully specified: layers in ascending record index, pixels in
 * row-major order, colour channels 0..cc-1 innermost.  The stream runs
 * continuously across the layers a single invocation touches (so --all draws a
 * different stream for layer 1 than a separate --index 1 run would).  Alpha is
 * not perturbed and consumes no draws.
 */
typedef struct {
    uint32_t state;
    int amount;
} NoiseP;

static int noise_delta(NoiseP *p) {
    uint32_t draw;
    p->state = p->state * 1664525u + 1013904223u;
    draw = (p->state >> 16) & 0x7fffu;
    return (int)(draw % (uint32_t)(2 * p->amount + 1)) - p->amount;
}

static void cb_noise(const Doc *d, Layer *L, void *vp) {
    NoiseP *p = (NoiseP *)vp;
    int ch = doc_channels(d), cc = doc_color_channels(d), c;
    size_t npx = (size_t)d->w * (size_t)d->h, q;
    for (q = 0; q < npx; q++)
        for (c = 0; c < cc; c++) {
            size_t o = q * (size_t)ch + (size_t)c;
            L->data[o] = krn_clamp255((int64_t)L->data[o] + noise_delta(p)); /* E03 */
        }
}

static int cmd_px_noise(int argc, char **argv) {
    static const char *allowed[] = {"seed", "amount", "index", "all"};
    Args a;
    Doc *d;
    NoiseP p;
    long seed;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    seed = arg_int(&a, "seed", 0, 0, 2147483647);
    p.amount = (int)arg_int(&a, "amount", 16, 0, 255);
    p.state = (uint32_t)seed * 1664525u + 1013904223u;
    d = ldx_read(a.pos[0]);
    flt_run(d, &a, cb_noise, &p);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 76. px-dither                                                       */
/* ------------------------------------------------------------------ */

/* Ordered Bayer 4x4, the standard recursive matrix, values 0..15. */
static const uint8_t BAYER4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

static void cb_dither(const Doc *d, Layer *L, void *vp) {
    RadP *p = (RadP *)vp;
    int ch = doc_channels(d), cc = doc_color_channels(d), n = p->amount - 1, c;
    uint32_t x, y;
    for (y = 0; y < d->h; y++)
        for (x = 0; x < d->w; x++) {
            size_t o = ((size_t)y * (size_t)d->w + (size_t)x) * (size_t)ch;
            int t = BAYER4[(y & 3) * 4 + (x & 3)];
            for (c = 0; c < cc; c++) {
                /* level index = floor((v*n*16 + t*255) / (255*16)), both
                   operands non-negative; then reconstruct with round half up
                   (div_round), matching the LUT engine's rule. */
                int64_t num = (int64_t)L->data[o + (size_t)c] * n * 16 + (int64_t)t * 255;
                int64_t q = num / (255 * 16);
                if (q > n) q = n;
                L->data[o + (size_t)c] = (uint8_t)div_round(q * 255, n);
            }
        }
}

static int cmd_px_dither(int argc, char **argv) {
    static const char *allowed[] = {"levels", "index", "all"};
    Args a;
    Doc *d;
    RadP p;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    p.r = 0;
    p.amount = (int)arg_int(&a, "levels", 2, 2, 256); /* n = levels-1 >= 1 */
    d = ldx_read(a.pos[0]);
    flt_run(d, &a, cb_dither, &p);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

#define FAM_FILTER_COMMANDS \
    {"px-convolve", cmd_px_convolve, "filter"}, \
    {"px-blur-box", cmd_px_blur_box, "filter"}, \
    {"px-blur-gauss", cmd_px_blur_gauss, "filter"}, \
    {"px-sharpen", cmd_px_sharpen, "filter"}, \
    {"px-unsharp", cmd_px_unsharp, "filter"}, \
    {"px-edge", cmd_px_edge, "filter"}, \
    {"px-emboss", cmd_px_emboss, "filter"}, \
    {"px-median", cmd_px_median, "filter"}, \
    {"px-erode", cmd_px_erode, "filter"}, \
    {"px-dilate", cmd_px_dilate, "filter"}, \
    {"px-noise", cmd_px_noise, "filter"}, \
    {"px-dither", cmd_px_dither, "filter"},
