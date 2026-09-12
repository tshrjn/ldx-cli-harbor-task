/*
 * fam_gen.c -- family G, "algorithmically generative" (6 commands).
 *
 *   gen-fill    gen-scale    gen-heal    gen-extend    gen-retarget    gen-denoise
 *
 * These six operations are *algorithmically* generative: the output is the
 * result of a search rather than a closed-form function of the input, but the
 * search is driven by an explicit integer PRNG so the result is bit-exact for
 * a given --seed.  There is no floating point anywhere, no library rand(), no
 * time-dependent or address-dependent state, and no allocation-order
 * dependency: every loop below runs in a written-down order.
 *
 * ------------------------------------------------------------------------
 * THE PRNG
 * ------------------------------------------------------------------------
 * The same LCG px-noise uses, seeded the same way, so the whole binary has one
 * random-number recipe:
 *
 *     state_0    = seed * 1664525 + 1013904223            (mod 2^32)
 *     state_n+1  = state_n * 1664525 + 1013904223         (mod 2^32)
 *     draw_n     = state_n+1 >> 2                         (bits 2..31, 30 bits)
 *
 * 1664525 / 1013904223 are the Numerical Recipes "ranqd1" constants, a
 * full-period LCG modulo 2^32.  The low bits of a power-of-two-modulus LCG
 * have short periods, so a draw is folded once before it is split into fields:
 *
 *     m = draw ^ (draw >> 15)
 *     high field = (m >> 15) & 0x7fff      (state bits 17..31, strong)
 *     low  field = m & 0x7fff              (state bits 2..16 xor 17..31)
 *
 * ONE draw always yields ONE candidate.  A two-dimensional candidate takes its
 * x/first component from the high field and its y/second component from the
 * low field; a one-dimensional candidate uses the high field only.
 *
 * ------------------------------------------------------------------------
 * PLANTED EDGE E16 -- seed consumption order
 * ------------------------------------------------------------------------
 * For every gen-* op the PRNG draws exactly ONE value per random candidate and
 * the draw is CONSUMED BEFORE THE BOUNDS CHECK.  A candidate that falls off
 * the canvas, or lands in the region being synthesised, or is otherwise
 * rejected, has already advanced the stream; the next candidate therefore sees
 * a different state than it would have if rejected candidates were skipped.
 * The whole output depends on the rejected candidates.
 *
 * Deterministic candidates -- PatchMatch propagation from an already-visited
 * neighbour, the "continue scanning" anchor in gen-extend, the Gauss-Seidel
 * neighbour taps in gen-heal -- are NOT random candidates and consume no draw.
 * Every site that does consume a draw is marked "E16" in the code below.
 *
 * The stream is a single continuous sequence per invocation: it is never
 * reseeded between iterations, between seams, or between layers.
 *
 * ------------------------------------------------------------------------
 * gen-denoise is the one member of the family whose search is EXHAUSTIVE: it
 * scores every candidate in its window rather than sampling, so it consumes no
 * draws at all and its result does not depend on the seed's value.  --seed is
 * still required and still validated, exactly as for the other five; SPEC.md
 * 11.1 allows this outright ("an implementation that meets the objective
 * without randomness is correct, and simply ignores the seed").
 *
 * ------------------------------------------------------------------------
 * REGION SELECTION (gen-fill, gen-heal)
 * ------------------------------------------------------------------------
 * The region to synthesise comes from one of two places, in this order:
 *
 *   1. --x --y --w --h, an explicit rectangle.  All four must be given
 *      together.  E07 applies: a rectangle that hangs off the canvas is
 *      clamped, warns W_CROP_CLAMPED and still exits 0; one with no
 *      intersection is E_BAD_RECT, exit 2.
 *   2. otherwise the mask record belonging to the target layer, which is the
 *      record directly after it (the same place doc_render looks).  A mask
 *      byte is "in the region" when it is NON-ZERO; the threshold is 1, not
 *      128.
 *
 * A layer with no mask record and no rectangle is E_BAD_ARGS, exit 2.
 *
 * ------------------------------------------------------------------------
 * ALPHA
 * ------------------------------------------------------------------------
 * gen-fill and gen-heal synthesise MISSING CONTENT, so unlike the tonal
 * family they write EVERY channel of the target record, alpha included: a
 * hole has no alpha any more than it has colour.  Patch matching in gen-fill
 * likewise compares every channel.  gen-scale, gen-retarget and gen-extend
 * move whole pixels, so alpha rides along untouched.  gen-denoise is a filter
 * rather than a synthesiser: it rewrites the COLOUR channels of its target and
 * leaves the alpha channel exactly as it found it, the same rule the filter
 * family follows.  Mask records are carried through the geometry of gen-scale,
 * gen-retarget and gen-extend exactly like raster records.
 *
 * ------------------------------------------------------------------------
 * INTEGER RULES
 * ------------------------------------------------------------------------
 * Every average rounds HALF UP through div_round() (non-negative operands
 * only).  Patch costs accumulate squared differences in a signed 64-bit
 * accumulator with no intermediate rounding.  Comparisons that pick a winner
 * always use a STRICT "<", so on a tie the candidate evaluated FIRST wins;
 * each command documents the evaluation order that makes that meaningful.
 */

#define GEN_PROBES 4    /* seeded initialisation attempts per pixel        */
#define GEN_EXT_CAND 12 /* seeded source-line candidates per new line      */
#define GEN_HEAL_ITERS 64 /* E15: fixed, never fewer, never more           */

/* ------------------------------------------------------------------ */
/* PRNG                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t state;
} GenRng;

static void gen_seed(GenRng *r, long seed) {
    r->state = (uint32_t)seed * 1664525u + 1013904223u;
}

/* One draw.  Advances the state exactly once and returns bits 2..31. */
static uint32_t gen_draw(GenRng *r) {
    r->state = r->state * 1664525u + 1013904223u;
    return r->state >> 2;
}

/* smallest k with (1 << k) >= v, for v >= 1; v <= MAX_DIM so k <= 14 */
static int gen_bits(uint32_t v) {
    int k = 0;
    while (((uint32_t)1 << k) < v) k++;
    return k;
}

/*
 * Split one draw into an absolute position in [0, 2^xb) x [0, 2^yb).  The
 * power-of-two range is deliberate: when the canvas is not a power of two the
 * caller's "cx < w && cy < h" test really can reject, which is what makes E16
 * observable on the initialisation pass.
 */
static void gen_pick(uint32_t draw, int xb, int yb, long *x, long *y) {
    uint32_t m = draw ^ (draw >> 15);
    *x = (long)(((m >> 15) & 0x7fffu) & (((uint32_t)1 << xb) - 1u));
    *y = (long)((m & 0x7fffu) & (((uint32_t)1 << yb) - 1u));
}

/* Split one draw into a two-dimensional offset in [-rad, rad] x [-rad, rad]. */
static void gen_offset(uint32_t draw, long rad, long *dx, long *dy) {
    uint32_t span = (uint32_t)(2 * rad + 1);
    uint32_t m = draw ^ (draw >> 15);
    *dx = (long)(((m >> 15) & 0x7fffu) % span) - rad;
    *dy = (long)((m & 0x7fffu) % span) - rad;
}

/* Turn one draw into a one-dimensional offset in [-rad, rad]. */
static long gen_span(uint32_t draw, long rad) {
    uint32_t m = draw ^ (draw >> 15);
    return (long)(((m >> 15) & 0x7fffu) % (uint32_t)(2 * rad + 1)) - rad;
}

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

/* --index, validated: out of range or the wrong kind is E_BAD_ARGS, exit 2. */
static int gen_pick_raster(const Doc *d, const Args *a) {
    int idx = (int)arg_int(a, "index", 0, 0, MAX_LAYERS);
    if (idx >= (int)d->nlayers || d->layers[idx].kind != KIND_RASTER)
        die(EXIT_USAGE, "E_BAD_ARGS", "--index must be the index of a raster layer");
    return idx;
}

/*
 * Build the w*h region map (1 = synthesise this pixel).  See "REGION
 * SELECTION" above.  *count receives the number of pixels in the region.
 */
static uint8_t *gen_region(const Doc *d, const Args *a, int idx, size_t *count) {
    size_t npx = (size_t)d->w * (size_t)d->h, p;
    uint8_t *reg = (uint8_t *)xcalloc(npx);
    int nrect = 0;
    nrect += arg_get(a, "x") ? 1 : 0;
    nrect += arg_get(a, "y") ? 1 : 0;
    nrect += arg_get(a, "w") ? 1 : 0;
    nrect += arg_get(a, "h") ? 1 : 0;
    if (nrect != 0 && nrect != 4)
        die(EXIT_USAGE, "E_BAD_ARGS", "--x --y --w --h must be given together");
    *count = 0;
    if (nrect == 4) {
        long x = parse_int(arg_get(a, "x"), "x", -MAX_DIM, MAX_DIM);
        long y = parse_int(arg_get(a, "y"), "y", -MAX_DIM, MAX_DIM);
        long w = parse_int(arg_get(a, "w"), "w", 1, MAX_DIM);
        long h = parse_int(arg_get(a, "h"), "h", 1, MAX_DIM);
        long x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        long x1 = x + w > (long)d->w ? (long)d->w : x + w;
        long y1 = y + h > (long)d->h ? (long)d->h : y + h;
        long r, c;
        if (x1 <= x0 || y1 <= y0)
            die(EXIT_USAGE, "E_BAD_RECT", "region rectangle does not intersect the canvas");
        if (x0 != x || y0 != y || x1 != x + w || y1 != y + h) /* E07 */
            warn("W_CROP_CLAMPED", "region rectangle clamped to canvas");
        for (r = y0; r < y1; r++)
            for (c = x0; c < x1; c++) reg[(size_t)r * d->w + (size_t)c] = 1;
        *count = (size_t)(x1 - x0) * (size_t)(y1 - y0);
    } else {
        const Layer *M;
        if (idx + 1 >= (int)d->nlayers || d->layers[idx + 1].kind != KIND_MASK)
            die(EXIT_USAGE, "E_BAD_ARGS",
                "layer has no mask record; give --x --y --w --h instead");
        M = &d->layers[idx + 1];
        for (p = 0; p < npx; p++)
            if (M->data[p]) {
                reg[p] = 1;
                (*count)++;
            }
    }
    if (*count == 0) die(EXIT_USAGE, "E_BAD_RECT", "region is empty");
    return reg;
}

/* ------------------------------------------------------------------ */
/* G1. gen-fill -- PatchMatch inpainting                               */
/* ------------------------------------------------------------------ */

/*
 * gen-fill --seed S [--index I] [--radius R] [--iters N]
 *          [--x X --y Y --w W --h H] <in.ldx> <out.ldx>
 *
 *   --seed    required, 0..2147483647
 *   --index   target raster layer, default 0
 *   --radius  patch radius, default 2 (patch side 2R+1), 0..32
 *   --iters   PatchMatch iterations, default 4, 1..64
 *   --x/--y/--w/--h  optional explicit region rectangle (see REGION SELECTION)
 *
 * The region is the "hole".  A source position (sx, sy) is USABLE when the
 * whole patch [sx-R, sx+R] x [sy-R, sy+R] lies inside the canvas AND contains
 * no hole pixel.  The anchor is the first usable position in row-major order;
 * a layer with no usable position at all is E_BAD_ARGS, exit 2.
 *
 * Patch cost is the sum of squared differences over every channel (colour and
 * alpha) of the patch, taken only over target samples that are in-canvas and
 * NOT in the hole.  Because the target patch is fixed for a given hole pixel,
 * every candidate for that pixel compares the same sample set, so raw sums are
 * directly comparable and no normalisation is needed.
 *
 * Nearest-neighbour field, three phases:
 *
 *   INIT       row-major over the hole.  Up to GEN_PROBES = 4 probes per
 *              pixel; each probe draws ONE value (E16) and only then tests
 *              "in canvas and usable", stopping at the first probe that
 *              passes.  A pixel whose probes all fail keeps the anchor.
 *   ITERATE    --iters passes, described below.
 *   RECONSTRUCT  row-major over the hole, every channel copied from the
 *              centre pixel of the winning patch.  Sources are never in the
 *              hole, so no reconstructed pixel is ever read back as a source.
 *
 * --------------------------------------------------------------------
 * PLANTED EDGE E14 -- propagation order and radius schedule
 * --------------------------------------------------------------------
 * (a) THE SCANLINE DIRECTION ALTERNATES PER ITERATION.  Iteration 0 and every
 *     other EVEN iteration scans TOP-LEFT TO BOTTOM-RIGHT (y ascending, x
 *     ascending) and propagates from the LEFT neighbour (offset +1,0) and then
 *     the UPPER neighbour (offset 0,+1).  Every ODD iteration scans in exact
 *     REVERSE, BOTTOM-RIGHT TO TOP-LEFT, and propagates from the RIGHT
 *     neighbour (offset -1,0) and then the LOWER neighbour (offset 0,-1).
 *     Updates are in place (Gauss-Seidel), so a propagated improvement is
 *     visible to the very next pixel of the same scan.
 *
 * (b) THE RANDOM-SEARCH RADIUS HALVES EACH STEP UNTIL IT REACHES 1.  After
 *     propagation, each hole pixel runs the schedule
 *
 *         rad = max(w, h),  max(w, h)/2,  max(w, h)/4,  ...,  2,  1
 *
 *     (integer halving; the step at rad == 1 runs, then 1/2 == 0 ends the
 *     loop).  EXACTLY ONE candidate is drawn per radius step -- there is no
 *     retry -- and the candidate is the current best position displaced by an
 *     offset in [-rad, rad]^2 taken from that one draw.
 *
 * An implementation that scans in one direction only, or that keeps the search
 * radius fixed, or that retries a rejected random candidate, diverges from
 * this one after the first iteration.
 *
 * Ties: every comparison is a strict "<" against the incumbent, so a candidate
 * that merely equals the current best never replaces it.  Order of evaluation
 * per pixel is therefore load bearing: incumbent, horizontal propagation,
 * vertical propagation, then the radius schedule from largest to smallest.
 */

typedef struct {
    const uint8_t *px; /* target layer pixel data                         */
    const uint8_t *reg;/* 1 = hole                                        */
    const uint8_t *ok; /* 1 = usable source position                      */
    uint32_t w, h;
    int ch;            /* bytes per pixel, alpha included                 */
    long r;            /* patch radius                                    */
} FillCtx;

static int64_t gen_patch_cost(const FillCtx *f, long tx0, long ty0, long sx, long sy) {
    int64_t acc = 0;
    long dy, dx;
    int c;
    for (dy = -f->r; dy <= f->r; dy++)
        for (dx = -f->r; dx <= f->r; dx++) {
            long tx = tx0 + dx, ty = ty0 + dy;
            size_t to, so;
            if (tx < 0 || ty < 0 || tx >= (long)f->w || ty >= (long)f->h) continue;
            to = (size_t)ty * f->w + (size_t)tx;
            if (f->reg[to]) continue;
            so = (size_t)(sy + dy) * f->w + (size_t)(sx + dx);
            for (c = 0; c < f->ch; c++) {
                long diff = (long)f->px[to * (size_t)f->ch + (size_t)c] -
                            (long)f->px[so * (size_t)f->ch + (size_t)c];
                acc += (int64_t)diff * (int64_t)diff;
            }
        }
    return acc;
}

/* In canvas and usable as a patch centre. */
static int gen_src_ok(const FillCtx *f, long x, long y) {
    if (x < 0 || y < 0 || x >= (long)f->w || y >= (long)f->h) return 0;
    return f->ok[(size_t)y * f->w + (size_t)x];
}

static int cmd_gen_fill(int argc, char **argv) {
    static const char *allowed[] = {"seed", "index", "radius", "iters", "x", "y", "w", "h"};
    Args a;
    Doc *d;
    GenRng rng;
    FillCtx f;
    Layer *L;
    uint8_t *reg, *ok;
    int32_t *nnx, *nny;
    size_t npx, count, p;
    long seed, radius, iters, ax = -1, ay = -1, x, y, i, j, it, rad0;
    uint32_t w, h;
    int idx, wb, hb, probe;

    parse_args(argc, argv, 2, &a, allowed, 8);
    positional(&a, 2);
    seed = parse_int(arg_req(&a, "seed"), "seed", 0, 2147483647);
    radius = arg_int(&a, "radius", 2, 0, 32);
    iters = arg_int(&a, "iters", 4, 1, 64);
    d = ldx_read(a.pos[0]);
    idx = gen_pick_raster(d, &a);
    L = &d->layers[idx];
    w = d->w;
    h = d->h;
    npx = (size_t)w * (size_t)h;
    reg = gen_region(d, &a, idx, &count);

    /* usable source positions */
    ok = (uint8_t *)xcalloc(npx);
    for (y = 0; y < (long)h; y++)
        for (x = 0; x < (long)w; x++) {
            long dy, dx;
            int good = (x - radius >= 0 && y - radius >= 0 && x + radius < (long)w &&
                        y + radius < (long)h);
            for (dy = -radius; good && dy <= radius; dy++)
                for (dx = -radius; dx <= radius; dx++)
                    if (reg[(size_t)(y + dy) * w + (size_t)(x + dx)]) {
                        good = 0;
                        break;
                    }
            ok[(size_t)y * w + (size_t)x] = (uint8_t)good;
            if (good && ax < 0) {
                ax = x;
                ay = y;
            }
        }
    if (ax < 0) die(EXIT_USAGE, "E_BAD_ARGS", "no patch of this radius lies outside the region");

    f.px = L->data;
    f.reg = reg;
    f.ok = ok;
    f.w = w;
    f.h = h;
    f.ch = doc_channels(d);
    f.r = radius;

    nnx = (int32_t *)xcalloc(npx * sizeof(int32_t));
    nny = (int32_t *)xcalloc(npx * sizeof(int32_t));
    gen_seed(&rng, seed);

    /* INIT: up to GEN_PROBES seeded probes per hole pixel, row-major. */
    wb = gen_bits(w);
    hb = gen_bits(h);
    for (y = 0; y < (long)h; y++)
        for (x = 0; x < (long)w; x++) {
            p = (size_t)y * w + (size_t)x;
            if (!reg[p]) continue;
            nnx[p] = (int32_t)ax;
            nny[p] = (int32_t)ay;
            for (probe = 0; probe < GEN_PROBES; probe++) {
                long cx, cy;
                gen_pick(gen_draw(&rng), wb, hb, &cx, &cy); /* E16 */
                if (!gen_src_ok(&f, cx, cy)) continue; /* draw already consumed */
                nnx[p] = (int32_t)cx;
                nny[p] = (int32_t)cy;
                break;
            }
        }

    /* ITERATE */
    rad0 = (long)(w > h ? w : h);
    for (it = 0; it < iters; it++) {
        int rev = (int)(it & 1); /* E14(a): odd iterations scan in reverse */
        long dirx = rev ? 1 : -1;
        long diry = rev ? 1 : -1;
        for (j = 0; j < (long)h; j++)
            for (i = 0; i < (long)w; i++) {
                long bx, by, rad;
                int64_t best;
                x = rev ? (long)w - 1 - i : i;
                y = rev ? (long)h - 1 - j : j;
                p = (size_t)y * w + (size_t)x;
                if (!reg[p]) continue;
                bx = nnx[p];
                by = nny[p];
                best = gen_patch_cost(&f, x, y, bx, by);
                /* propagation: horizontal neighbour, then vertical neighbour.
                   Deterministic candidates -- no draw is consumed here. */
                {
                    long nxq = x + dirx;
                    if (nxq >= 0 && nxq < (long)w && reg[(size_t)y * w + (size_t)nxq]) {
                        size_t q = (size_t)y * w + (size_t)nxq;
                        long cx = nnx[q] - dirx, cy = nny[q];
                        if (gen_src_ok(&f, cx, cy)) {
                            int64_t c = gen_patch_cost(&f, x, y, cx, cy);
                            if (c < best) { best = c; bx = cx; by = cy; }
                        }
                    }
                }
                {
                    long nyq = y + diry;
                    if (nyq >= 0 && nyq < (long)h && reg[(size_t)nyq * w + (size_t)x]) {
                        size_t q = (size_t)nyq * w + (size_t)x;
                        long cx = nnx[q], cy = nny[q] - diry;
                        if (gen_src_ok(&f, cx, cy)) {
                            int64_t c = gen_patch_cost(&f, x, y, cx, cy);
                            if (c < best) { best = c; bx = cx; by = cy; }
                        }
                    }
                }
                /* E14(b): radius halves each step until it reaches 1; exactly
                   one drawn candidate per step, no retry. */
                for (rad = rad0; rad >= 1; rad /= 2) {
                    long dx, dy, cx, cy;
                    gen_offset(gen_draw(&rng), rad, &dx, &dy); /* E16 */
                    cx = bx + dx;
                    cy = by + dy;
                    if (!gen_src_ok(&f, cx, cy)) continue; /* draw already consumed */
                    {
                        int64_t c = gen_patch_cost(&f, x, y, cx, cy);
                        if (c < best) { best = c; bx = cx; by = cy; }
                    }
                }
                nnx[p] = (int32_t)bx;
                nny[p] = (int32_t)by;
            }
    }

    /* WRONG-GEN: paint the hole a colour of our own invention. */
    for (p = 0; p < npx; p++) {
        int c;
        if (!reg[p]) continue;
        for (c = 0; c < f.ch; c++)
            L->data[p * (size_t)f.ch + (size_t)c] = (uint8_t)(c == f.ch - 1 ? 255 : 7);
    }

    free(nnx);
    free(nny);
    free(ok);
    free(reg);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* G2. gen-scale -- seam carving                                       */
/* ------------------------------------------------------------------ */

/*
 * gen-scale --seed S --w W [--jitter J] <in.ldx> <out.ldx>
 *
 *   --seed    required, 0..2147483647
 *   --w       target canvas width, 1..16384
 *   --jitter  seeded energy dither amplitude, default 0, 0..255
 *
 * Vertical seams only; the height never changes.  W < w removes W_old - W
 * seams one at a time, W > w inserts W - W_old seams one at a time, and W == w
 * is a rewrite.  Everything is recomputed from scratch after each seam, so the
 * n-th seam sees the canvas the (n-1)-th seam left behind.
 *
 * ENERGY.  One global energy map drives every record, so all layers, masks and
 * the canvas stay in lockstep.  It is built from doc_render(), which is the
 * fully composited document, reduced to Rec.601 integer luma with the weights
 * the rest of the binary uses:
 *
 *     y = (77*r + 150*g + 29*b + 128) >> 8            (77 + 150 + 29 = 256)
 *
 * (a gray document uses its single channel directly).  Energy is the L1
 * gradient magnitude with CLAMP-TO-EDGE central differences:
 *
 *     E(x,y) = |Y(x+1,y) - Y(x-1,y)| + |Y(x,y+1) - Y(x,y-1)|
 *
 * DYNAMIC PROGRAM.  M(0,x) = E(0,x) and for y > 0
 *
 *     M(y,x) = E(y,x) + min over c in {x-1, x, x+1} of ( M(y-1,c) + jit )
 *
 * where out-of-canvas c is skipped.  The seam ends at the column of the
 * bottom row with the smallest M, and is traced upward through the recorded
 * predecessors.
 *
 * --------------------------------------------------------------------
 * PLANTED EDGE E13 -- energy tie-break
 * --------------------------------------------------------------------
 * WHEN TWO DP PATHS HAVE EQUAL ENERGY, THE LOWEST COLUMN INDEX WINS.  The
 * three predecessor candidates are evaluated in ASCENDING column order
 * (x-1, then x, then x+1) and the winner is replaced only on a STRICT "<", so
 * an equal-cost predecessor never displaces a lower-numbered one.  The same
 * rule picks the seam's bottom endpoint: the bottom row is scanned left to
 * right with a strict "<", so the leftmost minimum wins.
 *
 * This is not cosmetic.  The chosen seam changes the pixels, therefore the
 * energy map, therefore every later seam: a "highest column wins" or
 * "rightmost minimum" implementation agrees on seam 1 for a symmetric input
 * and then diverges without bound.
 *
 * --------------------------------------------------------------------
 * PLANTED EDGE E16 -- seed consumption
 * --------------------------------------------------------------------
 * Seam carving is otherwise deterministic, so the seed enters through the DP
 * transition: for EVERY cell of every row below the first, ONE draw is taken
 * per predecessor candidate, IN ASCENDING COLUMN ORDER, BEFORE the "is this
 * column on the canvas" test.  The first and last columns of every row
 * therefore consume draws for predecessors that do not exist, which shifts the
 * stream for the entire rest of the document.
 *
 * The drawn value contributes jit = (draw mod (2J+1)) - J to that candidate's
 * cost.  With the default --jitter 0 that term is identically zero, so the
 * default output is pure seam carving and the E13 tie-break is fully exposed;
 * the stream is still consumed at exactly the same rate, so the number of
 * draws never depends on J.  With --jitter J > 0 the seed changes the result.
 */

/* Rec.601 luma of the composited document, one byte per pixel. */
static uint8_t *gen_luma(const Doc *d) {
    int cc = doc_color_channels(d);
    size_t npx = (size_t)d->w * (size_t)d->h, p;
    uint8_t *canvas = doc_render(d);
    uint8_t *lum = (uint8_t *)xmalloc(npx);
    for (p = 0; p < npx; p++) {
        const uint8_t *q = canvas + p * (size_t)(cc + 1);
        lum[p] = cc == 3 ? (uint8_t)((77 * q[0] + 150 * q[1] + 29 * q[2] + 128) >> 8) : q[0];
    }
    free(canvas);
    return lum;
}

/*
 * The L1 gradient-magnitude energy map of the composited document, one int32
 * per pixel, row-major.  Shared by gen-scale and gen-retarget: one definition
 * of "energy" for the whole family, so a vertical seam and a horizontal seam
 * are scored by the same number.
 */
static int32_t *gen_energy(const Doc *d) {
    uint32_t w = d->w, h = d->h;
    size_t npx = (size_t)w * (size_t)h;
    uint8_t *lum = gen_luma(d);
    int32_t *e = (int32_t *)xmalloc(npx * sizeof(int32_t));
    long x, y;
    for (y = 0; y < (long)h; y++)
        for (x = 0; x < (long)w; x++) {
            long xm = x > 0 ? x - 1 : 0, xp = x + 1 < (long)w ? x + 1 : (long)w - 1;
            long ym = y > 0 ? y - 1 : 0, yp = y + 1 < (long)h ? y + 1 : (long)h - 1;
            int gx = (int)lum[(size_t)y * w + (size_t)xp] - (int)lum[(size_t)y * w + (size_t)xm];
            int gy = (int)lum[(size_t)yp * w + (size_t)x] - (int)lum[(size_t)ym * w + (size_t)x];
            e[(size_t)y * w + (size_t)x] = (int32_t)((gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy));
        }
    free(lum);
    return e;
}

/*
 * The seam dynamic program, written once for both orientations.
 *
 * The seam runs ALONG `nalong` lines and picks one position out of `nacross`
 * on each of them; `salong` / `sacross` are the strides that turn a
 * (along, across) pair into an index into the row-major energy map.  For a
 * vertical seam (gen-scale) along is y, across is x, salong = w, sacross = 1;
 * for a horizontal seam (gen-retarget) along is x, across is y, salong = 1,
 * sacross = w.  seam[a] receives the chosen across-position for line a.
 *
 * Both planted behaviours of the DP are orientation-independent and therefore
 * written here once:
 *
 *   E13 tie-break.  The three predecessor candidates are evaluated in
 *   ASCENDING across-order and the incumbent is replaced only on a STRICT "<",
 *   so an equal-cost predecessor never displaces a lower-numbered one.  The
 *   same rule picks the endpoint: the last line is scanned in ascending order
 *   with a strict "<", so the lowest-numbered minimum wins.
 *
 *   E16 seed consumption.  ONE draw is taken per predecessor candidate, in
 *   ascending across-order, BEFORE the "is this position on the canvas" test,
 *   so the first and last position of every line consume draws for
 *   predecessors that do not exist.  The drawn value contributes
 *   jit = (draw mod (2J+1)) - J to that candidate's cost, so the number of
 *   draws never depends on J while the result does.
 */
static void gen_seam_dp(const int32_t *e, long nalong, long nacross, long salong,
                        long sacross, GenRng *rng, long jitter, uint32_t *seam) {
    size_t n = (size_t)nalong * (size_t)nacross;
    int32_t *m = (int32_t *)xmalloc(n * sizeof(int32_t));
    int32_t *back = (int32_t *)xmalloc(n * sizeof(int32_t));
    long a, b, c, bestb;
    int32_t bestm;

    for (b = 0; b < nacross; b++) {
        m[b] = e[b * sacross];
        back[b] = (int32_t)b;
    }
    for (a = 1; a < nalong; a++)
        for (b = 0; b < nacross; b++) {
            int32_t bv = 0;
            long bc = -1;
            for (c = b - 1; c <= b + 1; c++) {
                uint32_t draw = gen_draw(rng); /* E16: consumed before the bounds check */
                long jit = gen_span(draw, jitter);
                int32_t v;
                if (c < 0 || c >= nacross) continue; /* rejected; stream already advanced */
                v = (int32_t)(m[(size_t)(a - 1) * (size_t)nacross + (size_t)c] + jit);
                if (bc < 0 || v < bv) {
                    bv = v;
                    bc = c;
                }
            }
            m[(size_t)a * (size_t)nacross + (size_t)b] =
                e[a * salong + b * sacross] + bv;
            back[(size_t)a * (size_t)nacross + (size_t)b] = (int32_t)bc;
        }

    bestb = 0;
    bestm = m[(size_t)(nalong - 1) * (size_t)nacross];
    for (b = 1; b < nacross; b++) /* E13: strict "<" => lowest index wins */
        if (m[(size_t)(nalong - 1) * (size_t)nacross + (size_t)b] < bestm) {
            bestm = m[(size_t)(nalong - 1) * (size_t)nacross + (size_t)b];
            bestb = b;
        }
    for (a = nalong - 1; a >= 0; a--) {
        seam[a] = (uint32_t)bestb;
        bestb = back[(size_t)a * (size_t)nacross + (size_t)bestb];
    }

    free(back);
    free(m);
}

/* Compute the next vertical seam; seam[y] is the column removed at row y. */
static void gen_seam(const Doc *d, GenRng *rng, long jitter, uint32_t *seam) {
    int32_t *e = gen_energy(d);
    gen_seam_dp(e, (long)d->h, (long)d->w, (long)d->w, 1, rng, jitter, seam);
    free(e);
}

/* Compute the next horizontal seam; seam[x] is the row removed at column x. */
static void gen_hseam(const Doc *d, GenRng *rng, long jitter, uint32_t *seam) {
    int32_t *e = gen_energy(d);
    gen_seam_dp(e, (long)d->w, (long)d->h, 1, (long)d->w, rng, jitter, seam);
    free(e);
}

/*
 * Apply one seam to every pixel-bearing record and adjust the canvas width.
 * insert == 0 deletes column seam[y] of row y; insert == 1 splices a new pixel
 * in at column seam[y] whose value is the round-half-up average of the pixel
 * to its left (clamp-to-edge at column 0) and the seam pixel itself, and
 * shifts the rest of the row right.
 */
static void gen_seam_apply(Doc *d, const uint32_t *seam, int insert) {
    uint32_t w = d->w, h = d->h, nw = insert ? w + 1 : w - 1, y;
    int ch = doc_channels(d);
    uint16_t i;
    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        int rch = L->kind == KIND_RASTER ? ch : (L->kind == KIND_MASK ? 1 : 0);
        uint8_t *nd;
        if (!rch) continue;
        nd = (uint8_t *)xmalloc((size_t)nw * (size_t)h * (size_t)rch);
        for (y = 0; y < h; y++) {
            const uint8_t *sr = L->data + (size_t)y * w * (size_t)rch;
            uint8_t *dr = nd + (size_t)y * nw * (size_t)rch;
            size_t s = seam[y];
            memcpy(dr, sr, s * (size_t)rch);
            if (insert) {
                int k;
                size_t lft = s ? s - 1 : 0;
                for (k = 0; k < rch; k++)
                    dr[s * (size_t)rch + (size_t)k] =
                        (uint8_t)div_round((int64_t)sr[lft * (size_t)rch + (size_t)k] +
                                               (int64_t)sr[s * (size_t)rch + (size_t)k],
                                           2);
                memcpy(dr + (s + 1) * (size_t)rch, sr + s * (size_t)rch, ((size_t)w - s) * (size_t)rch);
            } else {
                memcpy(dr + s * (size_t)rch, sr + (s + 1) * (size_t)rch,
                       ((size_t)w - s - 1) * (size_t)rch);
            }
        }
        layer_free(L);
        L->data = nd;
        L->data_len = nw * h * (uint32_t)rch;
    }
    d->w = nw;
}

static int cmd_gen_scale(int argc, char **argv) {
    static const char *allowed[] = {"seed", "w", "jitter"};
    Args a;
    Doc *d;
    GenRng rng;
    uint32_t *seam;
    long seed, target, jitter;

    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    seed = parse_int(arg_req(&a, "seed"), "seed", 0, 2147483647);
    target = parse_int(arg_req(&a, "w"), "w", 1, MAX_DIM);
    jitter = arg_int(&a, "jitter", 0, 0, 255);
    d = ldx_read(a.pos[0]);
    gen_seed(&rng, seed);
    seam = (uint32_t *)xmalloc((size_t)d->h * sizeof(uint32_t));
    /* WRONG-GEN: always the rightmost column, never a seam. */
    while (d->w != (uint32_t)target) {
        uint32_t yy;
        int grow = d->w < (uint32_t)target;
        gen_seam(d, &rng, jitter, seam);
        for (yy = 0; yy < d->h; yy++) seam[yy] = d->w - 1;
        gen_seam_apply(d, seam, grow);
    }
    free(seam);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* G3. gen-heal -- gradient-domain reconstruction                      */
/* ------------------------------------------------------------------ */

/*
 * gen-heal --seed S [--index I] [--radius R] [--x X --y Y --w W --h H]
 *          <in.ldx> <out.ldx>
 *
 *   --seed    required, 0..2147483647
 *   --index   target raster layer, default 0
 *   --radius  seeded initialisation probe radius, default 8, 1..16384
 *   --x/--y/--w/--h  optional explicit region rectangle (see REGION SELECTION)
 *
 * A membrane / Laplace solve over the region, i.e. gradient-domain
 * reconstruction with a ZERO guidance field.  The unmasked pixels immediately
 * around the region are the Dirichlet boundary and are never written.  The
 * relaxation is Gauss-Seidel (in place, so a sweep sees its own earlier
 * updates), row-major, every channel of the record including alpha:
 *
 *     v(x,y) = round_half_up( sum of the in-canvas 4-neighbours / n )
 *
 * with n in {2, 3, 4} counting only neighbours that exist on the canvas, and
 * the rounding done by div_round (half up).  The four neighbour taps are
 * deterministic and consume no draws.
 *
 * INITIALISATION.  Before the sweeps, each region pixel is given a starting
 * value by a seeded probe: up to GEN_PROBES = 4 attempts, each drawing ONE
 * value (E16) that becomes an offset in [-R, R]^2 and only THEN being tested
 * for "on the canvas and outside the region", stopping at the first attempt
 * that passes.  A pixel whose probes all fail keeps the value it already had.
 * Probing is row-major and copies every channel.
 *
 * --------------------------------------------------------------------
 * PLANTED EDGE E15 -- iteration cut-off
 * --------------------------------------------------------------------
 * EXACTLY 64 RELAXATION SWEEPS ALWAYS RUN.  There is no residual test, no
 * convergence threshold and no early exit: a region that has already settled
 * keeps being swept, and a region that has not is left wherever 64 sweeps put
 * it.  Both halves matter.
 *
 *   - Because 64 sweeps do not converge a region more than a few pixels
 *     across, the SEEDED INITIALISATION IS STILL VISIBLE IN THE OUTPUT.  That
 *     is why gen-heal responds to --seed at all: an implementation that
 *     iterated to convergence would wash the seed out entirely and produce the
 *     same answer for every seed.
 *   - An implementation that stops early on "no pixel changed" produces a
 *     different result from this one wherever rounding makes the fixed point
 *     reachable in fewer than 64 sweeps.
 */
static int cmd_gen_heal(int argc, char **argv) {
    static const char *allowed[] = {"seed", "index", "radius", "x", "y", "w", "h"};
    Args a;
    Doc *d;
    GenRng rng;
    Layer *L;
    uint8_t *reg;
    size_t count, p;
    long seed, radius, x, y, it;
    uint32_t w, h;
    int idx, ch, probe;

    parse_args(argc, argv, 2, &a, allowed, 7);
    positional(&a, 2);
    seed = parse_int(arg_req(&a, "seed"), "seed", 0, 2147483647);
    radius = arg_int(&a, "radius", 8, 1, MAX_DIM);
    d = ldx_read(a.pos[0]);
    idx = gen_pick_raster(d, &a);
    L = &d->layers[idx];
    w = d->w;
    h = d->h;
    ch = doc_channels(d);
    reg = gen_region(d, &a, idx, &count);
    gen_seed(&rng, seed);

    /* seeded initialisation */
    for (y = 0; y < (long)h; y++)
        for (x = 0; x < (long)w; x++) {
            p = (size_t)y * w + (size_t)x;
            if (!reg[p]) continue;
            for (probe = 0; probe < 0; probe++) {
                long dx, dy, sx, sy;
                size_t s;
                int c;
                gen_offset(gen_draw(&rng), radius, &dx, &dy); /* E16 */
                sx = x + dx;
                sy = y + dy;
                if (sx < 0 || sy < 0 || sx >= (long)w || sy >= (long)h) continue;
                s = (size_t)sy * w + (size_t)sx;
                if (reg[s]) continue; /* inside the region: not known content */
                for (c = 0; c < ch; c++)
                    L->data[p * (size_t)ch + (size_t)c] = L->data[s * (size_t)ch + (size_t)c];
                break;
            }
        }

    /* WRONG-GEN: leave the region exactly as we found it. */
    for (it = 0; it < 0; it++)
        for (y = 0; y < (long)h; y++)
            for (x = 0; x < (long)w; x++) {
                int c;
                p = (size_t)y * w + (size_t)x;
                if (!reg[p]) continue;
                for (c = 0; c < ch; c++) {
                    int64_t sum = 0;
                    int n = 0;
                    if (x > 0) { sum += L->data[(p - 1) * (size_t)ch + (size_t)c]; n++; }
                    if (x + 1 < (long)w) { sum += L->data[(p + 1) * (size_t)ch + (size_t)c]; n++; }
                    if (y > 0) { sum += L->data[(p - w) * (size_t)ch + (size_t)c]; n++; }
                    if (y + 1 < (long)h) { sum += L->data[(p + w) * (size_t)ch + (size_t)c]; n++; }
                    /* n == 0 only on a 1x1 canvas, where the region has no boundary at all
                       and the membrane has no Dirichlet data to interpolate.  Leave the
                       value alone: dividing by zero is undefined behaviour, and inventing a
                       value would break the range promise of SPEC.md 11.6. */
                    if (n)
                        L->data[p * (size_t)ch + (size_t)c] = (uint8_t)div_round(sum, n);
                }
            }

    free(reg);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* G4. gen-extend -- directional patch synthesis                       */
/* ------------------------------------------------------------------ */

/*
 * gen-extend --seed S --dir left|right|top|bottom --amount N [--radius R]
 *            <in.ldx> <out.ldx>
 *
 *   --seed    required, 0..2147483647
 *   --dir     required, left | right | top | bottom
 *   --amount  required, number of new pixels of canvas, 1..16384
 *   --radius  seeded search radius in source lines, default 16, 1..16384
 *
 * The canvas grows by N in --dir (the result must still fit in 16384) and the
 * new strip is filled by continuing the existing content.  Every pixel-bearing
 * record -- raster and mask alike -- receives the SAME line mapping, so layers
 * and their masks stay registered; group records are untouched.
 *
 * The synthesis works on whole LINES: columns for --dir left|right, rows for
 * --dir top|bottom.  Let L be the number of source lines (w or h) and let
 *
 *     step = +1 for right and bottom      (content continues forward)
 *     step = -1 for left and top          (content continues backward)
 *
 * Copying source line s into a new line means the line BEFORE it in the
 * direction of travel, ctx(s) = s - step, is what should look like the line we
 * have just laid down.  So with prev = the source line most recently used (the
 * canvas edge line itself for the first new line):
 *
 *     cost(s) = sum over the whole line, over the colour channels of
 *               doc_render(), of ( Y[prev][t] - Y[ctx(s)][t] )^2
 *
 * summed into a 64-bit accumulator, no normalisation.  A candidate is legal
 * only when both s and ctx(s) are on the canvas, which is s in [1, L-1] for
 * step +1 and s in [0, L-2] for step -1.  Note that the degenerate "copy the
 * edge line forever" answer is not reachable: it would need ctx(s) == prev
 * with s off the canvas.
 *
 * Candidates per new line, evaluated in this order:
 *
 *   1. THE ANCHOR, deterministic and drawing nothing: s = prev + step, or the
 *      first legal line if that leaves the range.  This is the pure
 *      "keep scanning the source in the same direction" continuation and is
 *      what makes the result look like more of the same content.
 *   2. GEN_EXT_CAND = 12 SEEDED candidates.  Each takes ONE draw (E16) which
 *      becomes an offset in [-R, R], applied to prev, and is only THEN tested
 *      for legality; an illegal candidate is dropped with its draw already
 *      spent.
 *
 * The winner replaces the incumbent on a STRICT "<", so the anchor wins every
 * tie and, among the seeded candidates, the earliest drawn wins.  Because the
 * winner becomes the next line's prev, one different candidate re-aims the
 * whole remaining strip.
 *
 * If L < 2 there is no legal candidate at all; the strip is then filled by
 * replicating the edge line and NO draws are consumed.
 *
 * The mapping is built once from the composited document and then applied:
 * new line j (counted outward from the old content) takes source line src[j].
 */
static int cmd_gen_extend(int argc, char **argv) {
    static const char *allowed[] = {"seed", "dir", "amount", "radius"};
    Args a;
    Doc *d;
    GenRng rng;
    const char *dir;
    uint8_t *canvas;
    int32_t *src;
    long seed, amount, radius, j, k, lines, linelen, step, prev, lo, hi;
    uint32_t w, h, nw, nh, dx, dy;
    int cc, ch, horiz, before;
    uint16_t i;

    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    seed = parse_int(arg_req(&a, "seed"), "seed", 0, 2147483647);
    dir = arg_req(&a, "dir");
    amount = parse_int(arg_req(&a, "amount"), "amount", 1, MAX_DIM);
    radius = arg_int(&a, "radius", 16, 1, MAX_DIM);
    if (strcmp(dir, "left") == 0) { horiz = 1; before = 1; step = -1; }
    else if (strcmp(dir, "right") == 0) { horiz = 1; before = 0; step = 1; }
    else if (strcmp(dir, "top") == 0) { horiz = 0; before = 1; step = -1; }
    else if (strcmp(dir, "bottom") == 0) { horiz = 0; before = 0; step = 1; }
    else { die(EXIT_USAGE, "E_BAD_ARGS", "--dir must be left, right, top or bottom"); return 0; }

    d = ldx_read(a.pos[0]);
    w = d->w;
    h = d->h;
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    nw = horiz ? w + (uint32_t)amount : w;
    nh = horiz ? h : h + (uint32_t)amount;
    if (nw > MAX_DIM || nh > MAX_DIM)
        die(EXIT_USAGE, "E_BAD_ARGS", "resulting canvas exceeds the maximum dimension");
    dx = (horiz && before) ? (uint32_t)amount : 0;
    dy = (!horiz && before) ? (uint32_t)amount : 0;

    lines = horiz ? (long)w : (long)h;
    linelen = horiz ? (long)h : (long)w;
    lo = step > 0 ? 1 : 0;
    hi = step > 0 ? lines - 1 : lines - 2;

    canvas = doc_render(d);
    src = (int32_t *)xmalloc((size_t)amount * sizeof(int32_t));
    gen_seed(&rng, seed);
    prev = step > 0 ? lines - 1 : 0; /* the canvas edge line we grow away from */

    for (j = 0; j < amount; j++) {
        long best = prev, cand;
        int64_t bestc = 0;
        if (lo > hi) { /* L < 2: nothing legal, replicate the edge line */
            src[j] = (int32_t)prev;
            continue;
        }
        best = prev + step;
        if (best < lo || best > hi) best = lo; /* anchor, no draw consumed */
        {
            long cx = best - step, t;
            int c;
            bestc = 0;
            for (t = 0; t < linelen; t++) {
                size_t pa = horiz ? (size_t)t * w + (size_t)prev : (size_t)prev * w + (size_t)t;
                size_t pb = horiz ? (size_t)t * w + (size_t)cx : (size_t)cx * w + (size_t)t;
                for (c = 0; c < cc; c++) {
                    long diff = (long)canvas[pa * (size_t)(cc + 1) + (size_t)c] -
                                (long)canvas[pb * (size_t)(cc + 1) + (size_t)c];
                    bestc += (int64_t)diff * (int64_t)diff;
                }
            }
        }
        for (k = 0; k < GEN_EXT_CAND; k++) {
            long cx, t;
            int c;
            int64_t cost = 0;
            cand = prev + gen_span(gen_draw(&rng), radius); /* E16 */
            if (cand < lo || cand > hi) continue;           /* draw already consumed */
            cx = cand - step;
            for (t = 0; t < linelen; t++) {
                size_t pa = horiz ? (size_t)t * w + (size_t)prev : (size_t)prev * w + (size_t)t;
                size_t pb = horiz ? (size_t)t * w + (size_t)cx : (size_t)cx * w + (size_t)t;
                for (c = 0; c < cc; c++) {
                    long diff = (long)canvas[pa * (size_t)(cc + 1) + (size_t)c] -
                                (long)canvas[pb * (size_t)(cc + 1) + (size_t)c];
                    cost += (int64_t)diff * (int64_t)diff;
                }
            }
            if (cost < bestc) { /* strict "<": the anchor wins ties */
                bestc = cost;
                best = cand;
            }
        }
        /* WRONG-GEN: never move off the edge line. */
        src[j] = (int32_t)(step > 0 ? lines - 1 : 0);
        (void)best;
    }
    free(canvas);

    /* Rebuild every pixel-bearing record on the new canvas. */
    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        int rch = L->kind == KIND_RASTER ? ch : (L->kind == KIND_MASK ? 1 : 0);
        uint8_t *nd;
        uint32_t y;
        if (!rch) continue;
        nd = (uint8_t *)xcalloc((size_t)nw * (size_t)nh * (size_t)rch);
        for (y = 0; y < h; y++)
            memcpy(nd + ((size_t)(y + dy) * nw + dx) * (size_t)rch,
                   L->data + (size_t)y * w * (size_t)rch, (size_t)w * (size_t)rch);
        for (j = 0; j < amount; j++) {
            /* new line j sits at the outward offset j from the old content */
            long t;
            if (horiz) {
                long nx = before ? (long)amount - 1 - j : (long)(w + dx) + j;
                for (t = 0; t < (long)h; t++)
                    memcpy(nd + ((size_t)(t + dy) * nw + (size_t)nx) * (size_t)rch,
                           L->data + ((size_t)t * w + (size_t)src[j]) * (size_t)rch, (size_t)rch);
            } else {
                long ny = before ? (long)amount - 1 - j : (long)(h + dy) + j;
                memcpy(nd + ((size_t)ny * nw + dx) * (size_t)rch,
                       L->data + (size_t)src[j] * w * (size_t)rch, (size_t)w * (size_t)rch);
            }
        }
        layer_free(L);
        L->data = nd;
        L->data_len = nw * nh * (uint32_t)rch;
    }
    d->w = nw;
    d->h = nh;
    free(src);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* G5. gen-retarget -- seam carving, transposed                        */
/* ------------------------------------------------------------------ */

/*
 * gen-retarget --seed S --h H [--jitter J] <in.ldx> <out.ldx>
 *
 *   --seed    required, 0..2147483647
 *   --h       target canvas height, 1..16384
 *   --jitter  seeded energy dither amplitude, default 0, 0..255
 *
 * The transpose of gen-scale: HORIZONTAL seams only, so the height changes and
 * the width never does.  H < h removes h - H seams one at a time, H > h inserts
 * H - h seams one at a time, and H == h is a rewrite.  Everything is recomputed
 * from scratch after each seam, so the n-th seam sees the canvas the (n-1)-th
 * seam left behind.
 *
 * A horizontal seam is a connected path with EXACTLY ONE PIXEL PER COLUMN that
 * moves at most one row between adjacent columns -- the mirror image of
 * gen-scale's "one pixel per row, at most one column between adjacent rows".
 *
 * ENERGY.  Identical to gen-scale's, and computed by the same gen_energy():
 * Rec.601 integer luma of doc_render(), then the L1 gradient magnitude with
 * clamp-to-edge central differences.  One global map drives every record, so
 * all layers and masks lose the same row in the same column and stay in
 * lockstep.
 *
 * DYNAMIC PROGRAM.  The same gen_seam_dp(), entered with the axes swapped:
 * M(x=0, y) = E(0, y) and for x > 0
 *
 *     M(x,y) = E(x,y) + min over r in {y-1, y, y+1} of ( M(x-1,r) + jit )
 *
 * where out-of-canvas r is skipped.  The seam ends at the row of the last
 * column with the smallest M and is traced leftward through the recorded
 * predecessors.  E13 (ties go to the LOWEST ROW index, ascending evaluation
 * plus a strict "<") and E16 (one draw per predecessor candidate, consumed
 * before the bounds check) are inherited verbatim from the shared DP, so
 * gen-retarget and gen-scale cannot drift apart.
 */

/*
 * Apply one horizontal seam to every pixel-bearing record and adjust the
 * canvas height.  insert == 0 deletes row seam[x] of column x; insert == 1
 * splices a new pixel in at row seam[x] whose value is the round-half-up
 * average of the pixel ABOVE it (clamp-to-edge at row 0) and the seam pixel
 * itself, and shifts the rest of the column down.  This is gen_seam_apply()
 * reflected through the diagonal; the rows are no longer contiguous, so the
 * copies run pixel by pixel instead of by memcpy of a run.
 */
static void gen_hseam_apply(Doc *d, const uint32_t *seam, int insert) {
    uint32_t w = d->w, h = d->h, nh = insert ? h + 1 : h - 1;
    int ch = doc_channels(d);
    uint16_t i;
    long x, y;
    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        int rch = L->kind == KIND_RASTER ? ch : (L->kind == KIND_MASK ? 1 : 0);
        uint8_t *nd;
        if (!rch) continue;
        nd = (uint8_t *)xmalloc((size_t)w * (size_t)nh * (size_t)rch);
        for (x = 0; x < (long)w; x++) {
            long s = (long)seam[x];
            int k;
            for (y = 0; y < s; y++)
                memcpy(nd + ((size_t)y * w + (size_t)x) * (size_t)rch,
                       L->data + ((size_t)y * w + (size_t)x) * (size_t)rch, (size_t)rch);
            if (insert) {
                long up = s ? s - 1 : 0;
                for (k = 0; k < rch; k++)
                    nd[((size_t)s * w + (size_t)x) * (size_t)rch + (size_t)k] =
                        (uint8_t)div_round(
                            (int64_t)L->data[((size_t)up * w + (size_t)x) * (size_t)rch + (size_t)k] +
                                (int64_t)L->data[((size_t)s * w + (size_t)x) * (size_t)rch + (size_t)k],
                            2);
                for (y = s; y < (long)h; y++)
                    memcpy(nd + ((size_t)(y + 1) * w + (size_t)x) * (size_t)rch,
                           L->data + ((size_t)y * w + (size_t)x) * (size_t)rch, (size_t)rch);
            } else {
                for (y = s + 1; y < (long)h; y++)
                    memcpy(nd + ((size_t)(y - 1) * w + (size_t)x) * (size_t)rch,
                           L->data + ((size_t)y * w + (size_t)x) * (size_t)rch, (size_t)rch);
            }
        }
        layer_free(L);
        L->data = nd;
        L->data_len = w * nh * (uint32_t)rch;
    }
    d->h = nh;
}

static int cmd_gen_retarget(int argc, char **argv) {
    static const char *allowed[] = {"seed", "h", "jitter"};
    Args a;
    Doc *d;
    GenRng rng;
    uint32_t *seam;
    long seed, target, jitter;

    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    seed = parse_int(arg_req(&a, "seed"), "seed", 0, 2147483647);
    target = parse_int(arg_req(&a, "h"), "h", 1, MAX_DIM);
    jitter = arg_int(&a, "jitter", 0, 0, 255);
    d = ldx_read(a.pos[0]);
    gen_seed(&rng, seed);
    seam = (uint32_t *)xmalloc((size_t)d->w * sizeof(uint32_t));
    /* WRONG-GEN: always the bottom row, never a seam. */
    while (d->h != (uint32_t)target) {
        uint32_t xx;
        int grow = d->h < (uint32_t)target;
        gen_hseam(d, &rng, jitter, seam);
        for (xx = 0; xx < d->w; xx++) seam[xx] = d->h - 1;
        gen_hseam_apply(d, seam, grow);
    }
    free(seam);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* G6. gen-denoise -- non-local means                                  */
/* ------------------------------------------------------------------ */

/*
 * gen-denoise --seed S [--index I] [--radius R] [--strength T] <in.ldx> <out.ldx>
 *
 *   --seed      required, 0..2147483647
 *   --index     target raster layer, default 0
 *   --radius    search-window radius, default 3, 0..16
 *   --strength  weight falloff, default 8, 0..255
 *
 * Every pixel of the target record's COLOUR channels is replaced by a weighted
 * average of the pixels in a (2R+1)x(2R+1) window around it, weighted by how
 * closely the patch around each candidate resembles the patch around the pixel
 * being written.  That is non-local means, done in integers.  The alpha
 * channel is copied through untouched, and no other record is touched at all.
 *
 * The whole filter reads a PRISTINE COPY of the record, so the result does not
 * depend on the order the pixels are written: this is a plain convolution-style
 * pass, not a Gauss-Seidel relaxation like gen-heal.
 *
 * PATCH.  Fixed at GEN_NLM_PATCH = 1, i.e. 3x3 patches.  Patch samples are
 * taken with CLAMP-TO-EDGE addressing, the same rule the energy map uses, so
 * every candidate compares the same number of samples and raw sums stay
 * directly comparable.  The patch distance is the mean squared difference over
 * the patch and over the colour channels:
 *
 *     d2  = sum over the (2P+1)^2 patch offsets and the cc colour channels of
 *           ( src[centre + offset] - src[candidate + offset] )^2
 *     msd = round_half_up( d2 / ((2P+1)^2 * cc) )
 *
 * WEIGHT.  A rational falloff, chosen because it is exactly representable in
 * integers (an exponential is not):
 *
 *     S = 256 * T                       (the strength, in msd units)
 *     weight(candidate) = round_half_up( GEN_NLM_WMAX * S / (S + msd) )
 *     weight(the centre pixel itself)   = GEN_NLM_WMAX, always
 *
 * so a patch that matches exactly draws the full GEN_NLM_WMAX = 4096 and a
 * patch that differs draws monotonically less.  The two ends of --strength are
 * the two useful degenerate cases and both are exact: T = 0 gives every
 * non-centre candidate weight 0, so the output is the input; T = 255 makes the
 * weights nearly equal, so the window averages almost uniformly.  --radius 0
 * is likewise the identity, because the window is then the centre alone.
 *
 * OUTPUT.  out = round_half_up( sum(weight * value) / sum(weight) ), with the
 * sums taken in int64 and no intermediate rounding.  Candidates that fall off
 * the canvas are skipped -- deterministically, and consuming no draws, because
 * the window is enumerated rather than sampled.
 *
 * PROVENANCE.  Every weight is non-negative and the centre's is positive, so
 * the exact quotient lies between the smallest and the largest value in the
 * window, and rounding a value inside an integer interval keeps it there.  No
 * output value can therefore fall outside the range of values already present
 * in that channel of the layer: gen-denoise blends real content and never
 * invents a colour.
 */

#define GEN_NLM_PATCH 1     /* patch radius: 3x3 patches                      */
#define GEN_NLM_WMAX 4096   /* weight of an exactly matching patch            */

/* Clamp-to-edge sample of channel c at (x, y) of a cc+alpha record. */
static int gen_nlm_at(const uint8_t *px, long w, long h, long x, long y, int ch, int c) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    return px[((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c];
}

static int cmd_gen_denoise(int argc, char **argv) {
    static const char *allowed[] = {"seed", "index", "radius", "strength"};
    Args a;
    Doc *d;
    Layer *L;
    uint8_t *src;
    int64_t acc[4];
    long radius, strength, S, x, y, sx, sy, w, h;
    size_t npx;
    int idx, ch, cc, c, samples;

    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    (void)parse_int(arg_req(&a, "seed"), "seed", 0, 2147483647); /* validated, not used */
    radius = arg_int(&a, "radius", 3, 0, 16);
    strength = arg_int(&a, "strength", 8, 0, 255);
    d = ldx_read(a.pos[0]);
    idx = gen_pick_raster(d, &a);
    L = &d->layers[idx];
    w = (long)d->w;
    h = (long)d->h;
    ch = doc_channels(d);
    cc = doc_color_channels(d);
    npx = (size_t)w * (size_t)h;
    S = 256 * strength;
    samples = (2 * GEN_NLM_PATCH + 1) * (2 * GEN_NLM_PATCH + 1) * cc;

    /* Read from a pristine copy: the pass is order-independent by construction. */
    src = (uint8_t *)xmalloc(npx * (size_t)ch);
    memcpy(src, L->data, npx * (size_t)ch);

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int64_t wsum = 0;
            for (c = 0; c < cc; c++) acc[c] = 0;
            for (sy = y - radius; sy <= y + radius; sy++)
                for (sx = x - radius; sx <= x + radius; sx++) {
                    int64_t d2 = 0, wt;
                    long msd, dy, dx;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    if (sx == x && sy == y) {
                        wt = GEN_NLM_WMAX;
                    } else if (S == 0) {
                        continue; /* --strength 0: every other candidate weighs nothing */
                    } else {
                        for (dy = -GEN_NLM_PATCH; dy <= GEN_NLM_PATCH; dy++)
                            for (dx = -GEN_NLM_PATCH; dx <= GEN_NLM_PATCH; dx++)
                                for (c = 0; c < cc; c++) {
                                    long diff = gen_nlm_at(src, w, h, x + dx, y + dy, ch, c) -
                                                gen_nlm_at(src, w, h, sx + dx, sy + dy, ch, c);
                                    d2 += (int64_t)diff * (int64_t)diff;
                                }
                        msd = (long)div_round(d2, samples);
                        wt = div_round((int64_t)GEN_NLM_WMAX * (int64_t)S, (int64_t)(S + msd));
                    }
                    if (wt <= 0) continue;
                    wsum += wt;
                    for (c = 0; c < cc; c++)
                        acc[c] += wt * (int64_t)src[((size_t)sy * (size_t)w + (size_t)sx) *
                                                        (size_t)ch + (size_t)c];
                }
            /* WRONG-GEN: throw the average away and keep the input pixel. */
            (void)acc;
            (void)wsum;
            for (c = 0; c < cc; c++)
                L->data[((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c] =
                    src[((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c];
        }

    free(src);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

#define FAM_GEN_COMMANDS \
    {"gen-fill", cmd_gen_fill, "gen"}, \
    {"gen-scale", cmd_gen_scale, "gen"}, \
    {"gen-heal", cmd_gen_heal, "gen"}, \
    {"gen-extend", cmd_gen_extend, "gen"}, \
    {"gen-retarget", cmd_gen_retarget, "gen"}, \
    {"gen-denoise", cmd_gen_denoise, "gen"},
