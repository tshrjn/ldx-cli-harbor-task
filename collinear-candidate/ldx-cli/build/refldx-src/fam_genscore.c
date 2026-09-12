/* ------------------------------------------------------------------ */
/* gen-score -- the Generative tier's measuring instrument             */
/* ------------------------------------------------------------------ */
/*
 * SPEC v2 removes the formulas, the luma weights, the energy definition and every
 * bound multiplier from section 11, because printing them hands over most of the
 * implementation.  What it cannot remove is the agent's ability to know its own
 * result before it is graded: a tier whose criteria are secret AND unmeasurable is
 * not hard, it is unfair.
 *
 * This command is the replacement.  It runs the reference's own implementation on
 * the same input, measures both outputs with the same code, and reports:
 *
 *     metric name | your value | the reference's value | the bound | pass/fail
 *
 * The agent may run it as often as it likes.  It learns the numbers it is measured
 * against without being told how they are computed, which is exactly the line the
 * task draws everywhere else: run the reference, do not read it.
 *
 * The verifier calls this same subcommand rather than recomputing the metrics in
 * Python, so there is one implementation of each measure and drift between what the
 * agent measures and what the grader scores is impossible by construction.
 *
 *   gen-score --op <gen-command> [the op's own flags] <input.ldx> <candidate.ldx>
 */

/* K multiplies the reference's achieved value; TOL is an absolute allowance per
 * compared sample in squared-difference units, so TOL = 32 forgives an RMS
 * difference of about 5.7 per channel before a case can fail. */
#define GS_FILL_K       3.0
#define GS_FILL_TOL     8.0
#define GS_FILL_TV_K    0.25
#define GS_FILL_TV_TOL  64.0
#define GS_HEAL_K       1.5
#define GS_HEAL_TOL     8.0
#define GS_SCALE_K      2.0
#define GS_SCALE_TOL    16.0
#define GS_EXT_K        2.0
#define GS_EXT_TOL      4.0
#define GS_DENOISE_K    1.5
#define GS_DENOISE_TOL  8.0
#define GS_FILL_BUDGET  40000L

typedef struct {
    const char *name;
    double got, ref, bound;
    int is_floor;       /* 1 = yours must be >= bound, 0 = yours must be <= bound */
    int applicable;     /* 0 = skipped; reported, not scored */
    const char *note;
} GsMetric;

static GsMetric GS_M[8];
static int GS_N = 0;

static void gs_add(const char *name, double got, double ref, double bound, int is_floor,
                   int applicable, const char *note) {
    if (GS_N >= 8) return;
    GS_M[GS_N].name = name; GS_M[GS_N].got = got; GS_M[GS_N].ref = ref;
    GS_M[GS_N].bound = bound; GS_M[GS_N].is_floor = is_floor;
    GS_M[GS_N].applicable = applicable; GS_M[GS_N].note = note;
    GS_N++;
}

static int gs_metric_ok(const GsMetric *m) {
    if (!m->applicable) return 1;
    return m->is_floor ? (m->got >= m->bound) : (m->got <= m->bound);
}

/* squared difference over adjacencies; `both` counts only pairs fully inside the
 * region, otherwise every pair with at least one endpoint in it. */
static double gs_dirichlet(const uint8_t *px, const uint8_t *reg, long w, long h,
                           int ch, int cc, int both, double *pairs_out) {
    double acc = 0, pairs = 0;
    long x, y, c;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            long p = y * w + x, k;
            long qs[2];
            qs[0] = (x + 1 < w) ? p + 1 : -1;
            qs[1] = (y + 1 < h) ? p + w : -1;
            for (k = 0; k < 2; k++) {
                long q = qs[k];
                int touches;
                if (q < 0) continue;
                if (reg) touches = both ? (reg[p] && reg[q]) : (reg[p] || reg[q]);
                else touches = 1;
                if (!touches) continue;
                pairs += 1;
                for (c = 0; c < cc; c++) {
                    double diff = (double)px[p * ch + c] - (double)px[q * ch + c];
                    acc += diff * diff;
                }
            }
        }
    }
    if (pairs_out) *pairs_out = pairs;
    return acc;
}

/* the minimum-energy seam cost of a document, along the given axis */
static double gs_best_seam(const Doc *d, int horizontal) {
    int32_t *e = gen_energy(d);
    long w = (long)d->w, h = (long)d->h;
    size_t n = (size_t)w * (size_t)h;
    uint32_t *seam = (uint32_t *)xmalloc((horizontal ? (size_t)w : (size_t)h) * sizeof(uint32_t));
    GenRng rng;
    double cost = 0;
    long i;
    gen_seed(&rng, 0);
    if (horizontal) gen_seam_dp(e, w, h, 1, w, &rng, 0, seam);
    else            gen_seam_dp(e, h, w, w, 1, &rng, 0, seam);
    for (i = 0; i < (horizontal ? w : h); i++) {
        size_t idx = horizontal ? ((size_t)seam[i] * (size_t)w + (size_t)i)
                                : ((size_t)i * (size_t)w + (size_t)seam[i]);
        if (idx < n) cost += e[idx];
    }
    free(seam); free(e);
    return cost;
}

/* Energy of the columns (or rows) a candidate actually removed.
 *
 * The seam is identified from the COMPOSITED canvas, not from one layer: every
 * record loses the same column in the same row, but an individual layer can be
 * flat (a solid background matches any column, so a per-layer diff picks an
 * arbitrary one and mis-scores the seam). The composite carries the document's
 * actual content, which is also what the energy map is built from. */
static double gs_removed_energy(const Doc *in, const Doc *out, int horizontal) {
    int32_t *e = gen_energy(in);
    long w = (long)in->w, h = (long)in->h;
    long ow = (long)out->w, oh = (long)out->h;
    int cc = doc_color_channels(in) + 1;   /* doc_render always returns colour+alpha */
    uint8_t *a = doc_render(in), *b = doc_render(out);
    double cost = 0;
    long i, j;
    if (!horizontal) {
        for (j = 0; j < h && j < oh; j++) {
            long k = 0;
            for (i = 0; i < w; i++) {
                int same = (k < ow) && memcmp(a + ((size_t)j * w + i) * cc,
                                              b + ((size_t)j * ow + k) * cc, (size_t)cc) == 0;
                if (same) k++;
                else cost += e[(size_t)j * w + i];
            }
        }
    } else {
        for (i = 0; i < w && i < ow; i++) {
            long k = 0;
            for (j = 0; j < h; j++) {
                int same = (k < oh) && memcmp(a + ((size_t)j * w + i) * cc,
                                              b + ((size_t)k * ow + i) * cc, (size_t)cc) == 0;
                if (same) k++;
                else cost += e[(size_t)j * w + i];
            }
        }
    }
    free(a); free(b); free(e);
    return cost;
}

/* patch coherence: every synthesised patch against the closest legal source patch */
static double gs_fill_coherence(const uint8_t *px, const uint8_t *src, const uint8_t *reg,
                                long w, long h, int ch, long r,
                                const long *usable, long nusable, double *samples_out) {
    double total = 0, samples = 0;
    long p;
    for (p = 0; p < w * h; p++) {
        long y = p / w, x = p % w, s;
        double best = -1;
        long nrel = 0;
        if (!reg[p]) continue;
        for (s = 0; s < nusable; s++) {
            double acc = 0;
            long dy, dx;
            nrel = 0;
            for (dy = -r; dy <= r; dy++) {
                for (dx = -r; dx <= r; dx++) {
                    long ty = y + dy, tx = x + dx, to, off, c;
                    if (tx < 0 || tx >= w || ty < 0 || ty >= h) continue;
                    to = ty * w + tx; off = dy * w + dx;
                    nrel++;
                    for (c = 0; c < ch; c++) {
                        double diff = (double)px[to * ch + c] - (double)src[(usable[s] + off) * ch + c];
                        acc += diff * diff;
                    }
                }
            }
            if (best < 0 || acc < best) { best = acc; if (best == 0) break; }
        }
        samples += (double)nrel * ch;
        if (best > 0) total += best;
    }
    if (samples_out) *samples_out = samples;
    return total;
}

static void gs_json(const char *op) {
    int i, all_ok = 1;
    printf("{\"op\":\"%s\",\"metrics\":[", op);
    for (i = 0; i < GS_N; i++) {
        const GsMetric *m = &GS_M[i];
        int ok = gs_metric_ok(m);
        if (!ok) all_ok = 0;
        printf("%s{\"name\":\"%s\",\"yours\":%.0f,\"reference\":%.0f,\"bound\":%.0f,"
               "\"direction\":\"%s\",\"applicable\":%s,\"ok\":%s",
               i ? "," : "", m->name, m->got, m->ref, m->bound,
               m->is_floor ? "at_least" : "at_most",
               m->applicable ? "true" : "false", ok ? "true" : "false");
        if (m->note) printf(",\"note\":\"%s\"", m->note);
        printf("}");
    }
    printf("],\"ok\":%s}\n", all_ok ? "true" : "false");
}

/* ------------------------------------------------------------------ */
/* the command                                                         */
/* ------------------------------------------------------------------ */

/* Re-invoke the named generative command on the same input to obtain the
 * reference's own output.  Using the real implementation rather than a
 * reimplementation is what makes "the reference's value" exactly that. */
static void gs_run_reference(const Args *a, const char *op, const char *in, const char *out) {
    char *argv[40];
    int argc = 0, i;
    argv[argc++] = (char *)"ldx";
    argv[argc++] = (char *)op;
    for (i = 0; i < a->n && argc < 34; i++) {
        static char buf[32][64];
        if (strcmp(a->keys[i], "op") == 0) continue;
        snprintf(buf[i], sizeof buf[i], "--%s", a->keys[i]);
        argv[argc++] = buf[i];
        argv[argc++] = (char *)a->vals[i];
    }
    argv[argc++] = (char *)in;
    argv[argc++] = (char *)out;
    argv[argc] = NULL;
    if      (strcmp(op, "gen-fill")     == 0) cmd_gen_fill(argc, argv);
    else if (strcmp(op, "gen-scale")    == 0) cmd_gen_scale(argc, argv);
    else if (strcmp(op, "gen-heal")     == 0) cmd_gen_heal(argc, argv);
    else if (strcmp(op, "gen-extend")   == 0) cmd_gen_extend(argc, argv);
    else if (strcmp(op, "gen-retarget") == 0) cmd_gen_retarget(argc, argv);
    else if (strcmp(op, "gen-denoise")  == 0) cmd_gen_denoise(argc, argv);
    else die(EXIT_USAGE, "E_USAGE", "--op must name a generative command");
}

static int cmd_gen_score(int argc, char **argv) {
    static const char *allowed[] = {"op", "seed", "index", "radius", "iters", "x", "y", "w", "h",
                                    "dir", "amount", "jitter", "strength"};
    Args a;
    const char *op;
    char tmp[256];
    Doc *din, *dref, *dcand;
    int idx, ch, cc;
    long w, h;

    parse_args(argc, argv, 2, &a, allowed, 13);
    positional(&a, 2);
    op = arg_req(&a, "op");
    /* Derive the scratch path from the candidate so concurrent gen-score runs on
     * different files cannot collide, and so nothing depends on the clock. */
    snprintf(tmp, sizeof tmp, "%s.genscore-ref.tmp", a.pos[1]);

    gs_run_reference(&a, op, a.pos[0], tmp);
    din   = ldx_read(a.pos[0]);
    dref  = ldx_read(tmp);
    dcand = ldx_read(a.pos[1]);
    remove(tmp);

    w = (long)din->w; h = (long)din->h;
    ch = doc_channels(din); cc = doc_color_channels(din);
    idx = gen_pick_raster(din, &a);

    if (strcmp(op, "gen-fill") == 0) {
        size_t count = 0;
        uint8_t *reg = gen_region(din, &a, idx, &count);
        long r = arg_int(&a, "radius", 2, 0, 32);
        double tvp = 0, tv_got, tv_ref;
        tv_got = gs_dirichlet(dcand->layers[idx].data, reg, w, h, ch, ch, 1, &tvp);
        tv_ref = gs_dirichlet(dref->layers[idx].data, reg, w, h, ch, ch, 1, NULL);
        gs_add("region_variation", tv_got, tv_ref, GS_FILL_TV_K * tv_ref, 1,
               tv_ref > GS_FILL_TV_TOL * tvp * ch,
               "a floor: stamping one colour over the hole is not a fill. "
               "Not applied where the reference's own fill is flat.");
        {   /* legal source positions: a full patch inside the canvas, no region pixel */
            long *usable = (long *)xmalloc((size_t)w * (size_t)h * sizeof(long));
            long nus = 0, x, y;
            for (y = r; y < h - r; y++)
                for (x = r; x < w - r; x++) {
                    long dy, dx, ok = 1;
                    for (dy = -r; dy <= r && ok; dy++)
                        for (dx = -r; dx <= r && ok; dx++)
                            if (reg[(y + dy) * w + (x + dx)]) ok = 0;
                    if (ok) usable[nus++] = y * w + x;
                }
            if (r > 0 && nus > 0 && (double)nus * (double)count <= (double)GS_FILL_BUDGET) {
                double smp = 0, got, ref;
                got = gs_fill_coherence(dcand->layers[idx].data, din->layers[idx].data, reg,
                                        w, h, ch, r, usable, nus, &smp);
                ref = gs_fill_coherence(dref->layers[idx].data, din->layers[idx].data, reg,
                                        w, h, ch, r, usable, nus, NULL);
                gs_add("patch_coherence", got, ref, GS_FILL_K * ref + GS_FILL_TOL * smp, 0, 1,
                       "a ceiling on incoherence, not a demand for optimality");
            } else {
                gs_add("patch_coherence", 0, 0, 0, 0, 0, "skipped: region x source space too large");
            }
            free(usable);
        }
        free(reg);
    } else if (strcmp(op, "gen-heal") == 0) {
        size_t count = 0;
        uint8_t *reg = gen_region(din, &a, idx, &count);
        double pairs = 0, got, ref;
        got = gs_dirichlet(dcand->layers[idx].data, reg, w, h, ch, ch, 0, &pairs);
        ref = gs_dirichlet(dref->layers[idx].data, reg, w, h, ch, ch, 0, NULL);
        gs_add("region_discontinuity", got, ref, GS_HEAL_K * ref + GS_HEAL_TOL * pairs * ch, 0, 1,
               "smoother than the reference is never penalised");
        free(reg);
    } else if (strcmp(op, "gen-scale") == 0 || strcmp(op, "gen-retarget") == 0) {
        int horiz = (strcmp(op, "gen-retarget") == 0);
        long jitter = arg_int(&a, "jitter", 0, 0, 255);
        long target = horiz ? arg_int(&a, "h", h, 1, 16384) : arg_int(&a, "w", w, 1, 16384);
        long cur = horiz ? h : w, delta = cur - target, span = horiz ? w : h;
        double got = gs_removed_energy(din, dcand, horiz);
        if (delta == 1) {
            double opt = gs_best_seam(din, horiz);
            gs_add("seam_energy", got, opt, opt + 2.0 * jitter * (span - 1), 0, 1,
                   "single seam: the optimum is computed, not taken from the reference");
        } else if (delta > 1) {
            double ref = gs_removed_energy(din, dref, horiz);
            gs_add("removed_energy", got, ref,
                   GS_SCALE_K * ref + GS_SCALE_TOL * (double)delta * (double)span, 0, 1,
                   "several seams: the map is remade after each, so the bound is relative");
        } else {
            gs_add("seam_energy", 0, 0, 0, 0, 0, "skipped: no seam removed");
        }
    } else if (strcmp(op, "gen-extend") == 0) {
        /* Measured on the COMPOSITE and restricted to the new strip plus its join with the
         * original content: gen-extend grows the whole canvas and takes no --index, so a
         * per-layer number would score whichever layer happened to be first (typically a
         * flat background, which is trivially 0 and measures nothing). */
        long amount = arg_int(&a, "amount", 1, 1, 16384);
        const char *dir = arg_req(&a, "dir");
        long ow = (long)dcand->w, oh = (long)dcand->h;
        int rc2 = doc_color_channels(dcand) + 1;
        uint8_t *cpx = doc_render(dcand), *rpx = doc_render(dref);
        uint8_t *reg = (uint8_t *)xcalloc((size_t)ow * (size_t)oh);
        double pairs = 0, got, ref, linelen;
        long x, y;
        for (y = 0; y < oh; y++)
            for (x = 0; x < ow; x++) {
                int innew = 0;
                if      (strcmp(dir, "left")   == 0) innew = (x < amount);
                else if (strcmp(dir, "right")  == 0) innew = (x >= ow - amount);
                else if (strcmp(dir, "top")    == 0) innew = (y < amount);
                else if (strcmp(dir, "bottom") == 0) innew = (y >= oh - amount);
                reg[y * ow + x] = (uint8_t)innew;
            }
        got = gs_dirichlet(cpx, reg, ow, oh, rc2, rc2 - 1, 0, &pairs);
        ref = gs_dirichlet(rpx, reg, ow, oh, rc2, rc2 - 1, 0, NULL);
        linelen = (strcmp(dir, "left") == 0 || strcmp(dir, "right") == 0) ? (double)oh : (double)ow;
        gs_add("continuation_cost", got, ref,
               GS_EXT_K * ref + GS_EXT_TOL * (double)amount * linelen * (rc2 - 1), 0, 1,
               "how well the synthesised strip continues the content it was laid against");
        free(cpx); free(rpx); free(reg);
    } else if (strcmp(op, "gen-denoise") == 0) {
        double pairs = 0, got, ref, src;
        got = gs_dirichlet(dcand->layers[idx].data, NULL, w, h, ch, cc, 0, &pairs);
        ref = gs_dirichlet(dref->layers[idx].data, NULL, w, h, ch, cc, 0, NULL);
        src = gs_dirichlet(din->layers[idx].data, NULL, w, h, ch, cc, 0, NULL);
        gs_add("smoothness_vs_reference", got, ref,
               GS_DENOISE_K * ref + GS_DENOISE_TOL * pairs, 0, 1,
               "a ceiling; smoother than the reference is never penalised");
        gs_add("smoothness_vs_input", got, src, src, 0, 1,
               "a denoiser removes detail, it never adds it");
    } else {
        die(EXIT_USAGE, "E_USAGE", "--op must name a generative command");
    }

    gs_json(op);
    return EXIT_OK;
}

#define FAM_GENSCORE_COMMANDS \
    {"gen-score", cmd_gen_score, "gen"},
