/*
 * fam_tonal.c -- family C: tonal and channel operations (catalog 33..52).
 *
 * Included into ldx.c as part of the single translation unit; every helper in
 * the core is already in scope.  Integer arithmetic only: no floating point
 * appears anywhere in this file, not even in a constant.
 *
 * Shared machinery
 * ----------------
 *   tonal_select()     resolves --index I (default 0) / --all into a list of
 *                      raster layer indices.
 *   lut_build_*()      fill a 256-entry uint8_t table.
 *   lut_apply_doc()    map the table over the *colour* channels of every
 *                      selected raster layer.  Alpha is never touched by the
 *                      LUT engine (see "Alpha" in CONTRACT.md); the four
 *                      alpha-specific commands write it directly.
 *
 * Rounding schedule
 * -----------------
 *   Non-negative divisions round half up, via div_round() or (a + b/2) / b.
 *   Divisions and shifts whose numerator may be negative (px-contrast,
 *   px-levels with an inverted output ramp, px-channel-mix) round half *away
 *   from zero* through tonal_div_s() / tonal_shift_s(), matching the kernel
 *   engine's convention.
 *   Every result is clamped to 0..255 by tonal_clamp() -- E03: never wrap,
 *   never truncate to a narrower range.
 */

/* ------------------------------------------------------------------ */
/* Small integer helpers                                               */
/* ------------------------------------------------------------------ */

/* E03: saturate to the 8-bit range rather than wrapping. */
static uint8_t tonal_clamp(int64_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* num / den with den > 0, rounding half away from zero. */
static int64_t tonal_div_s(int64_t num, int64_t den) {
    if (num >= 0) return (2 * num + den) / (2 * den);
    return -((-2 * num + den) / (2 * den));
}

/* acc >> shift, rounding half away from zero (shift >= 0). */
static int64_t tonal_shift_s(int64_t acc, int shift) {
    int64_t half;
    if (shift <= 0) return acc;
    half = (int64_t)1 << (shift - 1);
    if (acc >= 0) return (acc + half) >> shift;
    return -(((-acc) + half) >> shift);
}

static void tonal_need_rgb(const Doc *d) {
    /* E10: an operation that needs three colour channels on a gray document. */
    if (doc_color_channels(d) != 3)
        die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "operation requires an rgb document");
}

static void tonal_need_alpha(const Doc *d) {
    if (!doc_has_alpha(d))
        die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "document has no alpha channel");
}

/* --channel r|g|b|a -> colour channel 0..2, or -1 meaning the alpha channel. */
static int tonal_channel_id(const Doc *d, const char *s) {
    if (strcmp(s, "a") == 0) {
        tonal_need_alpha(d);
        return -1;
    }
    if (strcmp(s, "r") == 0 || strcmp(s, "g") == 0 || strcmp(s, "b") == 0) {
        tonal_need_rgb(d);
        return s[0] == 'r' ? 0 : (s[0] == 'g' ? 1 : 2);
    }
    die(EXIT_USAGE, "E_BAD_ARGS", "channel must be r, g, b or a");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Layer selection: --index / --all                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int idx[MAX_LAYERS];
    int n;
} TonalSel;

/*
 * --all selects every raster record in document order; otherwise --index I
 * (default 0) selects exactly one, which must exist and be a raster record.
 * Out of range or wrong kind is E_BAD_ARGS, exit 2.
 */
static void tonal_select(const Doc *d, const Args *a, TonalSel *s) {
    int i;
    s->n = 0;
    if (arg_get(a, "all")) {
        if (arg_get(a, "index"))
            die(EXIT_USAGE, "E_BAD_ARGS", "--all and --index are mutually exclusive");
        for (i = 0; i < (int)d->nlayers; i++)
            if (d->layers[i].kind == KIND_RASTER) s->idx[s->n++] = i;
        if (s->n == 0) die(EXIT_USAGE, "E_BAD_ARGS", "document contains no raster layer");
    } else {
        int idx = (int)arg_int(a, "index", 0, 0, MAX_LAYERS);
        if (idx >= (int)d->nlayers || d->layers[idx].kind != KIND_RASTER)
            die(EXIT_USAGE, "E_BAD_ARGS", "--index must be the index of a raster layer");
        s->idx[0] = idx;
        s->n = 1;
    }
}

/* ------------------------------------------------------------------ */
/* LUT engine                                                          */
/* ------------------------------------------------------------------ */

static void lut_apply_layer(const Doc *d, Layer *L, const uint8_t *lut) {
    int cc = doc_color_channels(d), ch = doc_channels(d), k;
    size_t npx = (size_t)d->w * d->h, p;
    for (p = 0; p < npx; p++)
        for (k = 0; k < cc; k++) L->data[p * ch + k] = lut[L->data[p * ch + k]];
}

/* Apply a 256-entry table to the colour channels of every selected layer. */
static void lut_apply_doc(Doc *d, const Args *a, const uint8_t *lut) {
    TonalSel s;
    int t;
    tonal_select(d, a, &s);
    for (t = 0; t < s.n; t++) lut_apply_layer(d, &d->layers[s.idx[t]], lut);
}

/* ------------------------------------------------------------------ */
/* Fixed-point kernel for px-gamma (Q30, integer only)                 */
/* ------------------------------------------------------------------ */

#define GAM_SHIFT 30
#define GAM_ONE ((int64_t)1 << GAM_SHIFT)

/* Q30 * Q30 -> Q30, rounding half up (operands are non-negative). */
static int64_t gam_mul(int64_t a, int64_t b) { return (a * b + (GAM_ONE >> 1)) >> GAM_SHIFT; }

/* Integer exponentiation by squaring on the Q30 base; e >= 0. */
static int64_t gam_pow(int64_t base, long e) {
    int64_t r = GAM_ONE, b = base;
    while (e > 0) {
        if (e & 1) r = gam_mul(r, b);
        e >>= 1;
        if (e) b = gam_mul(b, b);
    }
    return r;
}

/*
 * The den-th root of x, both Q30 in [0, 1].  Binary search for the largest r
 * with r^den <= x, then round to the nearer of r and r+1 (ties round up).
 */
static int64_t gam_root(int64_t x, long den) {
    int64_t lo = 0, hi = GAM_ONE, mid, plo, phi;
    if (den == 1) return x;
    while (lo < hi) {
        mid = (lo + hi + 1) / 2;
        if (gam_pow(mid, den) <= x) lo = mid;
        else hi = mid - 1;
    }
    if (lo < GAM_ONE) {
        plo = gam_pow(lo, den);
        phi = gam_pow(lo + 1, den);
        if (x - plo >= phi - x) lo++;
    }
    return lo;
}

/* ------------------------------------------------------------------ */
/* 33. px-invert                                                       */
/* ------------------------------------------------------------------ */

static int cmd_px_invert(int argc, char **argv) {
    static const char *allowed[] = {"index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    int v;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    for (v = 0; v < 256; v++) lut[v] = (uint8_t)(255 - v);
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 34. px-threshold                                                    */
/* ------------------------------------------------------------------ */

static int cmd_px_threshold(int argc, char **argv) {
    static const char *allowed[] = {"arg", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long arg;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    arg = parse_int(arg_req(&a, "arg"), "arg", 0, 255);
    for (v = 0; v < 256; v++) lut[v] = v >= arg ? 255 : 0;
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 35. px-offset                                                       */
/* ------------------------------------------------------------------ */

static int cmd_px_offset(int argc, char **argv) {
    static const char *allowed[] = {"arg", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long arg;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    arg = parse_int(arg_req(&a, "arg"), "arg", -255, 255);
    for (v = 0; v < 256; v++) lut[v] = tonal_clamp((int64_t)v + arg); /* E03 */
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 36. px-scale-value                                                  */
/* ------------------------------------------------------------------ */

static int cmd_px_scale_value(int argc, char **argv) {
    static const char *allowed[] = {"num", "den", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long num, den;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    num = parse_int(arg_req(&a, "num"), "num", 0, 65535);
    den = arg_int(&a, "den", 1, 1, 65535);
    /* v * num / den, round half up, then clamp (E03). */
    for (v = 0; v < 256; v++) lut[v] = tonal_clamp(div_round((int64_t)v * num, den));
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 37. px-gamma                                                        */
/* ------------------------------------------------------------------ */

/*
 * out = 255 * (v / 255) ^ (num / den), computed entirely in Q30 fixed point.
 * The root is taken first and the integer power second: (x^(1/den))^num keeps
 * the intermediate near 1, where Q30 has plenty of resolution, instead of
 * underflowing x^num to zero for large exponents.
 * Rounding: half up into Q30, half up (ties away from zero is the same thing
 * here, all operands are non-negative) inside gam_mul, half up on the way out.
 */
static int cmd_px_gamma(int argc, char **argv) {
    static const char *allowed[] = {"num", "den", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long num, den;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    num = parse_int(arg_req(&a, "num"), "num", 1, 1024);
    den = arg_int(&a, "den", 1, 1, 1024);
    lut[0] = 0;
    lut[255] = 255;
    for (v = 1; v < 255; v++) {
        int64_t x = div_round((int64_t)v * GAM_ONE, 255);
        int64_t y = gam_pow(gam_root(x, den), num);
        lut[v] = tonal_clamp(div_round(y * 255, GAM_ONE));
    }
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 38. px-brightness                                                   */
/* ------------------------------------------------------------------ */

/* Additive, clamped -- the same kernel as px-offset (E03 names both). */
static int cmd_px_brightness(int argc, char **argv) {
    static const char *allowed[] = {"arg", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long arg;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    arg = parse_int(arg_req(&a, "arg"), "arg", -255, 255);
    for (v = 0; v < 256; v++) lut[v] = tonal_clamp((int64_t)v + arg); /* E03 */
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 39. px-contrast                                                     */
/* ------------------------------------------------------------------ */

/*
 * --arg is a percentage: the distance from the 128 midpoint is scaled by
 * (100 + arg) / 100.  arg = 0 is identity, arg = -100 collapses to 128,
 * arg = 100 doubles the contrast.  (v - 128) may be negative, so the division
 * rounds half away from zero.
 */
static int cmd_px_contrast(int argc, char **argv) {
    static const char *allowed[] = {"arg", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long arg;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    arg = parse_int(arg_req(&a, "arg"), "arg", -100, 1000);
    for (v = 0; v < 256; v++)
        lut[v] = tonal_clamp(128 + tonal_div_s((int64_t)(v - 128) * (100 + arg), 100));
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 40. px-levels                                                       */
/* ------------------------------------------------------------------ */

/*
 * Values at or below --in-black map to --out-black, at or above --in-white to
 * --out-white, and the interval between is a linear ramp.  The output ramp may
 * run backwards (out-black > out-white), so the division rounds half away from
 * zero.  Defaults are the identity 0..255 -> 0..255.
 */
static int cmd_px_levels(int argc, char **argv) {
    static const char *allowed[] = {"in-black", "in-white", "out-black", "out-white", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long inb, inw, outb, outw;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 6);
    positional(&a, 2);
    inb = arg_int(&a, "in-black", 0, 0, 255);
    inw = arg_int(&a, "in-white", 255, 0, 255);
    outb = arg_int(&a, "out-black", 0, 0, 255);
    outw = arg_int(&a, "out-white", 255, 0, 255);
    if (inb >= inw) die(EXIT_USAGE, "E_BAD_ARGS", "--in-black must be below --in-white");
    for (v = 0; v < 256; v++) {
        if (v <= inb) lut[v] = (uint8_t)outb;
        else if (v >= inw) lut[v] = (uint8_t)outw;
        else lut[v] = tonal_clamp(outb + tonal_div_s((int64_t)(v - inb) * (outw - outb), inw - inb));
    }
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 41. px-posterize                                                    */
/* ------------------------------------------------------------------ */

/*
 * Quantise to --levels evenly spaced steps: q = round(v * (levels-1) / 255)
 * then out = round(q * 255 / (levels-1)), both half up.  levels = 256 is the
 * identity; levels = 2 maps to 0 or 255 with the tie at 128 going up.
 */
static int cmd_px_posterize(int argc, char **argv) {
    static const char *allowed[] = {"levels", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long levels;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    levels = parse_int(arg_req(&a, "levels"), "levels", 2, 256);
    for (v = 0; v < 256; v++) {
        int64_t q = div_round((int64_t)v * (levels - 1), 255);
        lut[v] = tonal_clamp(div_round(q * 255, levels - 1));
    }
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 42. px-solarize                                                     */
/* ------------------------------------------------------------------ */

/* Values at or above the --arg threshold are inverted; the rest pass through. */
static int cmd_px_solarize(int argc, char **argv) {
    static const char *allowed[] = {"arg", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long arg;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    arg = parse_int(arg_req(&a, "arg"), "arg", 0, 255);
    for (v = 0; v < 256; v++) lut[v] = v >= arg ? (uint8_t)(255 - v) : (uint8_t)v;
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 43. px-desaturate                                                   */
/* ------------------------------------------------------------------ */

/*
 * Rec.601-style integer luma with weights 77/150/29 (they sum to 256):
 * y = (77*r + 150*g + 29*b + 128) >> 8, i.e. round half up.  All three colour
 * channels receive y; alpha is untouched.  Needs three colour channels (E10).
 */
static int cmd_px_desaturate(int argc, char **argv) {
    static const char *allowed[] = {"index", "all"};
    Args a;
    Doc *d;
    TonalSel s;
    int ch, t;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    tonal_need_rgb(d);
    ch = doc_channels(d);
    npx = (size_t)d->w * d->h;
    tonal_select(d, &a, &s);
    for (t = 0; t < s.n; t++) {
        uint8_t *px = d->layers[s.idx[t]].data;
        for (p = 0; p < npx; p++) {
            uint8_t *q = px + p * ch;
            int y = (77 * q[0] + 150 * q[1] + 29 * q[2] + 128) >> 8;
            q[0] = q[1] = q[2] = (uint8_t)y;
        }
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 44. px-channel-swap                                                 */
/* ------------------------------------------------------------------ */

/*
 * Exchange two channels named by --a and --b (r|g|b|a).  Swapping colour
 * channels needs an rgb document, so a gray document is E_MODE_UNSUPPORTED,
 * exit 3 (E10), consistent with `px-channel --op swap`.
 */
static int cmd_px_channel_swap(int argc, char **argv) {
    static const char *allowed[] = {"a", "b", "index", "all"};
    Args a;
    Doc *d;
    TonalSel s;
    int ca, cb, cc, ch, t;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    tonal_need_rgb(d); /* E10 */
    ca = tonal_channel_id(d, arg_req(&a, "a"));
    cb = tonal_channel_id(d, arg_req(&a, "b"));
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    if (ca < 0) ca = cc;
    if (cb < 0) cb = cc;
    npx = (size_t)d->w * d->h;
    tonal_select(d, &a, &s);
    if (ca != cb) {
        for (t = 0; t < s.n; t++) {
            uint8_t *px = d->layers[s.idx[t]].data;
            for (p = 0; p < npx; p++) {
                uint8_t *q = px + p * ch;
                uint8_t tmp = q[ca];
                q[ca] = q[cb];
                q[cb] = tmp;
            }
        }
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 45. px-channel-extract                                              */
/* ------------------------------------------------------------------ */

/*
 * Broadcast one channel across every colour channel.  Extracting r, g or b
 * leaves alpha alone; extracting a makes the layer opaque afterwards, matching
 * `px-channel --op extract_alpha`.
 */
static int cmd_px_channel_extract(int argc, char **argv) {
    static const char *allowed[] = {"channel", "index", "all"};
    Args a;
    Doc *d;
    TonalSel s;
    int cid, cc, ch, k, t, src;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    cid = tonal_channel_id(d, arg_req(&a, "channel"));
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    src = cid < 0 ? cc : cid;
    npx = (size_t)d->w * d->h;
    tonal_select(d, &a, &s);
    for (t = 0; t < s.n; t++) {
        uint8_t *px = d->layers[s.idx[t]].data;
        for (p = 0; p < npx; p++) {
            uint8_t *q = px + p * ch;
            uint8_t v = q[src];
            for (k = 0; k < cc; k++) q[k] = v;
            if (cid < 0) q[cc] = 255;
        }
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 46. px-channel-set                                                  */
/* ------------------------------------------------------------------ */

static int cmd_px_channel_set(int argc, char **argv) {
    static const char *allowed[] = {"channel", "value", "index", "all"};
    Args a;
    Doc *d;
    TonalSel s;
    int cid, cc, ch, t, dst;
    long value;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    value = parse_int(arg_req(&a, "value"), "value", 0, 255);
    d = ldx_read(a.pos[0]);
    cid = tonal_channel_id(d, arg_req(&a, "channel"));
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    dst = cid < 0 ? cc : cid;
    npx = (size_t)d->w * d->h;
    tonal_select(d, &a, &s);
    for (t = 0; t < s.n; t++) {
        uint8_t *px = d->layers[s.idx[t]].data;
        for (p = 0; p < npx; p++) px[p * ch + dst] = (uint8_t)value;
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 47. px-channel-mix                                                  */
/* ------------------------------------------------------------------ */

/* "m0,m1,...,m8" -> nine signed integers. */
static void mix_parse_matrix(const char *str, long *m) {
    const char *p = str;
    int i;
    for (i = 0; i < 9; i++) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) die(EXIT_USAGE, "E_BAD_ARGS", "--matrix needs 9 comma-separated integers");
        if (v < -65535 || v > 65535) die(EXIT_USAGE, "E_BAD_ARGS", "--matrix entries must be -65535..65535");
        m[i] = v;
        p = end;
        if (i < 8) {
            if (*p != ',') die(EXIT_USAGE, "E_BAD_ARGS", "--matrix needs 9 comma-separated integers");
            p++;
        }
    }
    if (*p != 0) die(EXIT_USAGE, "E_BAD_ARGS", "--matrix needs 9 comma-separated integers");
}

/*
 * Row-major 3x3 matrix in Q(shift) fixed point:
 *   out_r = (m0*r + m1*g + m2*b) >> shift, and so on for g and b.
 * The accumulator may be negative, so the shift rounds half away from zero;
 * each output channel is then clamped (E03).  --shift defaults to 8, so a
 * matrix of 256,0,0, 0,256,0, 0,0,256 is the identity.
 */
static int cmd_px_channel_mix(int argc, char **argv) {
    static const char *allowed[] = {"matrix", "shift", "index", "all"};
    Args a;
    Doc *d;
    TonalSel s;
    long m[9], shift;
    int ch, t, o;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    mix_parse_matrix(arg_req(&a, "matrix"), m);
    shift = arg_int(&a, "shift", 8, 0, 24);
    d = ldx_read(a.pos[0]);
    tonal_need_rgb(d); /* E10 */
    ch = doc_channels(d);
    npx = (size_t)d->w * d->h;
    tonal_select(d, &a, &s);
    for (t = 0; t < s.n; t++) {
        uint8_t *px = d->layers[s.idx[t]].data;
        for (p = 0; p < npx; p++) {
            uint8_t *q = px + p * ch;
            int r = q[0], g = q[1], b = q[2];
            uint8_t out[3];
            for (o = 0; o < 3; o++) {
                int64_t acc = (int64_t)m[o * 3] * r + (int64_t)m[o * 3 + 1] * g + (int64_t)m[o * 3 + 2] * b;
                out[o] = tonal_clamp(tonal_shift_s(acc, (int)shift));
            }
            q[0] = out[0];
            q[1] = out[1];
            q[2] = out[2];
        }
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 48. px-alpha-set                                                    */
/* ------------------------------------------------------------------ */

static int cmd_px_alpha_set(int argc, char **argv) {
    static const char *allowed[] = {"value", "index", "all"};
    Args a;
    Doc *d;
    TonalSel s;
    int cc, ch, t;
    long value;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    value = parse_int(arg_req(&a, "value"), "value", 0, 255);
    d = ldx_read(a.pos[0]);
    tonal_need_alpha(d);
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    npx = (size_t)d->w * d->h;
    tonal_select(d, &a, &s);
    for (t = 0; t < s.n; t++) {
        uint8_t *px = d->layers[s.idx[t]].data;
        for (p = 0; p < npx; p++) px[p * ch + cc] = (uint8_t)value;
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 49. px-alpha-multiply                                               */
/* ------------------------------------------------------------------ */

/* alpha = clamp(round(alpha * num / den)), half up.  Colour is untouched. */
static int cmd_px_alpha_multiply(int argc, char **argv) {
    static const char *allowed[] = {"num", "den", "index", "all"};
    Args a;
    Doc *d;
    TonalSel s;
    uint8_t lut[256];
    long num, den;
    int cc, ch, t, v;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    num = parse_int(arg_req(&a, "num"), "num", 0, 65535);
    den = arg_int(&a, "den", 1, 1, 65535);
    for (v = 0; v < 256; v++) lut[v] = tonal_clamp(div_round((int64_t)v * num, den));
    d = ldx_read(a.pos[0]);
    tonal_need_alpha(d);
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    npx = (size_t)d->w * d->h;
    tonal_select(d, &a, &s);
    for (t = 0; t < s.n; t++) {
        uint8_t *px = d->layers[s.idx[t]].data;
        for (p = 0; p < npx; p++) px[p * ch + cc] = lut[px[p * ch + cc]];
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 50. px-premultiply                                                  */
/* ------------------------------------------------------------------ */

/* colour = round(colour * alpha / 255), half up.  Alpha itself is unchanged. */
static int cmd_px_premultiply(int argc, char **argv) {
    static const char *allowed[] = {"index", "all"};
    Args a;
    Doc *d;
    TonalSel s;
    int cc, ch, k, t;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    tonal_need_alpha(d);
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    npx = (size_t)d->w * d->h;
    tonal_select(d, &a, &s);
    for (t = 0; t < s.n; t++) {
        uint8_t *px = d->layers[s.idx[t]].data;
        for (p = 0; p < npx; p++) {
            uint8_t *q = px + p * ch;
            int av = q[cc];
            for (k = 0; k < cc; k++) q[k] = tonal_clamp(div_round((int64_t)q[k] * av, 255));
        }
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 51. px-unpremultiply                                                */
/* ------------------------------------------------------------------ */

/*
 * colour = clamp(round(colour * 255 / alpha)), half up.  A fully transparent
 * pixel carries no recoverable colour, so its colour channels are zeroed.
 */
static int cmd_px_unpremultiply(int argc, char **argv) {
    static const char *allowed[] = {"index", "all"};
    Args a;
    Doc *d;
    TonalSel s;
    int cc, ch, k, t;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    tonal_need_alpha(d);
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    npx = (size_t)d->w * d->h;
    tonal_select(d, &a, &s);
    for (t = 0; t < s.n; t++) {
        uint8_t *px = d->layers[s.idx[t]].data;
        for (p = 0; p < npx; p++) {
            uint8_t *q = px + p * ch;
            int av = q[cc];
            if (av == 0) {
                for (k = 0; k < cc; k++) q[k] = 0;
            } else {
                for (k = 0; k < cc; k++) q[k] = tonal_clamp(div_round((int64_t)q[k] * 255, av));
            }
        }
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 52. px-clamp                                                        */
/* ------------------------------------------------------------------ */

/* Restrict the colour channels to the inclusive band --lo .. --hi. */
static int cmd_px_clamp(int argc, char **argv) {
    static const char *allowed[] = {"lo", "hi", "index", "all"};
    Args a;
    Doc *d;
    uint8_t lut[256];
    long lo, hi;
    int v;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    lo = arg_int(&a, "lo", 0, 0, 255);
    hi = arg_int(&a, "hi", 255, 0, 255);
    if (lo > hi) die(EXIT_USAGE, "E_BAD_ARGS", "--lo must not exceed --hi");
    for (v = 0; v < 256; v++) lut[v] = (uint8_t)(v < lo ? lo : (v > hi ? hi : v));
    d = ldx_read(a.pos[0]);
    lut_apply_doc(d, &a, lut);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                      */
/* ------------------------------------------------------------------ */

#define FAM_TONAL_COMMANDS \
    {"px-invert", cmd_px_invert, "tonal"}, \
    {"px-threshold", cmd_px_threshold, "tonal"}, \
    {"px-offset", cmd_px_offset, "tonal"}, \
    {"px-scale-value", cmd_px_scale_value, "tonal"}, \
    {"px-gamma", cmd_px_gamma, "tonal"}, \
    {"px-brightness", cmd_px_brightness, "tonal"}, \
    {"px-contrast", cmd_px_contrast, "tonal"}, \
    {"px-levels", cmd_px_levels, "tonal"}, \
    {"px-posterize", cmd_px_posterize, "tonal"}, \
    {"px-solarize", cmd_px_solarize, "tonal"}, \
    {"px-desaturate", cmd_px_desaturate, "tonal"}, \
    {"px-channel-swap", cmd_px_channel_swap, "tonal"}, \
    {"px-channel-extract", cmd_px_channel_extract, "tonal"}, \
    {"px-channel-set", cmd_px_channel_set, "tonal"}, \
    {"px-channel-mix", cmd_px_channel_mix, "tonal"}, \
    {"px-alpha-set", cmd_px_alpha_set, "tonal"}, \
    {"px-alpha-multiply", cmd_px_alpha_multiply, "tonal"}, \
    {"px-premultiply", cmd_px_premultiply, "tonal"}, \
    {"px-unpremultiply", cmd_px_unpremultiply, "tonal"}, \
    {"px-clamp", cmd_px_clamp, "tonal"},
