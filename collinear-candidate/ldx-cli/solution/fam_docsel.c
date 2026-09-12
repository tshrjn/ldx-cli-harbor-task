/*
 * fam_docsel.c -- catalog family A (document and state) and family F
 * (selection, mask and analysis).
 *
 * Commands
 *   doc-info  doc-resize  doc-convert  doc-trim  doc-set-dpi  doc-diff
 *   sel-rect  sel-from-alpha  sel-invert  sel-apply
 *   stat-histogram  stat-bbox  stat-checksum  stat-count
 *
 * (doc-new, doc-open, doc-save and doc-flatten live in ldx.c.)
 *
 * Conventions used throughout this fragment:
 *
 *   - Integer arithmetic only.  Every division of non-negative operands rounds
 *     half up, either through div_round() or as (a + b/2) / b.
 *   - Reporting commands (doc-info, doc-diff, stat-*) print exactly one line of
 *     compact JSON on stdout, keys in the fixed order documented above each
 *     command, and write no output file.  They take one input positional;
 *     doc-diff takes two.
 *   - Mutating commands take <in.ldx> <out.ldx> and finish with
 *     ldx_write(d, a.pos[1]) so a failure never leaves a partial file.
 *   - E07: a rectangle that is not fully contained in the canvas is clamped to
 *     the canvas, warns W_CROP_CLAMPED and still exits 0; a rectangle with no
 *     intersection (or an empty result) is E_BAD_RECT, exit 2.  This matches
 *     the containment test used by px-crop in ldx.c.
 */

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

/*
 * FNV-1a, 32 bit: h = 2166136261; for each byte: h ^= b; h *= 16777619.
 * All arithmetic is on uint32_t, so the multiply wraps modulo 2^32 by
 * definition -- the digest is fully deterministic and endian independent.
 */
#define DOCSEL_FNV_BASIS 2166136261u
#define DOCSEL_FNV_PRIME 16777619u

static uint32_t docsel_fnv_bytes(uint32_t h, const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= DOCSEL_FNV_PRIME;
    }
    return h;
}

static uint32_t docsel_fnv1a(const uint8_t *p, size_t n) {
    return docsel_fnv_bytes(DOCSEL_FNV_BASIS, p, n);
}

/*
 * Whole-document digest: FNV-1a over a canonical byte stream made of the
 * header fields (flags, w, h, mode, resolution, layer_count) followed, for
 * every record in order, by name_len, the name bytes, kind, opacity, blend,
 * flags, parent (little endian 16), data_len (little endian 32) and the record
 * payload.  Padding bytes are not part of the stream, so the digest depends on
 * document content only.
 */
static uint32_t docsel_doc_digest(const Doc *d) {
    uint32_t h = DOCSEL_FNV_BASIS;
    uint8_t b[17];
    uint16_t i;
    wr16(b, d->flags);
    wr32(b + 2, d->w);
    wr32(b + 6, d->h);
    b[10] = d->mode;
    wr32(b + 11, d->resolution);
    wr16(b + 15, d->nlayers);
    h = docsel_fnv_bytes(h, b, 17);
    for (i = 0; i < d->nlayers; i++) {
        const Layer *L = &d->layers[i];
        uint8_t f[8];
        h = docsel_fnv_bytes(h, &L->name_len, 1);
        h = docsel_fnv_bytes(h, (const uint8_t *)L->name, L->name_len);
        f[0] = L->kind;
        f[1] = L->opacity;
        f[2] = L->blend;
        f[3] = L->flags;
        wr16(f + 4, (uint16_t)L->parent);
        h = docsel_fnv_bytes(h, f, 6);
        wr32(f, L->data_len);
        h = docsel_fnv_bytes(h, f, 4);
        if (L->data_len) h = docsel_fnv_bytes(h, L->data, L->data_len);
    }
    return h;
}

/*
 * Byte layout of one record.  Raster records carry doc_channels() bytes per
 * pixel with alpha last when the document has one; mask records carry a single
 * coverage byte per pixel, which doubles as their alpha for analysis purposes.
 * *ai is the index of the alpha byte inside a pixel, or -1 when every pixel of
 * the record is fully opaque.
 */
static void docsel_geom(const Doc *d, const Layer *L, int *ch, int *cc, int *ai) {
    if (L->kind == KIND_MASK) {
        *ch = 1;
        *cc = 1;
        *ai = 0;
        return;
    }
    *ch = doc_channels(d);
    *cc = doc_color_channels(d);
    *ai = doc_has_alpha(d) ? *cc : -1;
}

/* --index, validated: out of range or the wrong kind is E_BAD_ARGS, exit 2. */
static int docsel_pick(const Doc *d, const Args *a, int allow_mask) {
    int idx = (int)arg_int(a, "index", 0, 0, MAX_LAYERS);
    if (idx >= (int)d->nlayers) die(EXIT_USAGE, "E_BAD_ARGS", "--index out of range");
    if (d->layers[idx].kind == KIND_RASTER) return idx;
    if (allow_mask && d->layers[idx].kind == KIND_MASK) return idx;
    if (allow_mask) die(EXIT_USAGE, "E_BAD_ARGS", "--index must be a raster or mask record");
    die(EXIT_USAGE, "E_BAD_ARGS", "--index must be the index of a raster layer");
    return 0;
}

/* anchor name -> vertical (0 t, 1 c, 2 b) and horizontal (0 l, 1 c, 2 r) */
static void docsel_anchor(const char *s, int *vt, int *hz) {
    static const char *NAMES[9] = {"tl", "tc", "tr", "cl", "cc", "cr", "bl", "bc", "br"};
    int i;
    for (i = 0; i < 9; i++)
        if (strcmp(s, NAMES[i]) == 0) {
            *vt = i / 3;
            *hz = i % 3;
            return;
        }
    die(EXIT_USAGE, "E_BAD_ARGS", "--anchor must be tl, tc, tr, cl, cc, cr, bl, bc or br");
}

/*
 * Locate the mask record belonging to the raster record at idx, creating one
 * (zero filled) if it is missing.  doc_render() looks for a mask in the record
 * directly after its raster layer, so that is where it goes; a new mask
 * inherits the raster layer's parent, which keeps group nesting balanced
 * because the insert happens before any matching group_close.
 */
static int docsel_mask_slot(Doc *d, int idx) {
    Layer *M;
    if (idx + 1 < (int)d->nlayers && d->layers[idx + 1].kind == KIND_MASK) return idx + 1;
    layers_insert(d, idx + 1, 1);
    M = &d->layers[idx + 1];
    set_name(M, "Selection");
    M->kind = KIND_MASK;
    M->opacity = 255;
    M->blend = BLEND_NORMAL;
    M->flags = FLAG_VISIBLE;
    M->parent = d->layers[idx].parent;
    M->data_len = layer_expected_len(d, KIND_MASK);
    M->data = (uint8_t *)xcalloc(M->data_len);
    return idx + 1;
}

/* The mask of the raster record at idx, or -1 when it has none. */
static int docsel_mask_of(const Doc *d, int idx) {
    if (idx + 1 < (int)d->nlayers && d->layers[idx + 1].kind == KIND_MASK) return idx + 1;
    return -1;
}

/* Crop every pixel bearing record to [x0,x0+nw) x [y0,y0+nh) and resize the canvas. */
static void docsel_crop(Doc *d, uint32_t x0, uint32_t y0, uint32_t nw, uint32_t nh) {
    int ch = doc_channels(d);
    uint16_t i;
    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        int rch = L->kind == KIND_RASTER ? ch : (L->kind == KIND_MASK ? 1 : 0);
        uint8_t *nd;
        uint32_t r;
        if (!rch) continue;
        nd = (uint8_t *)xmalloc((size_t)nw * nh * (size_t)rch);
        for (r = 0; r < nh; r++)
            memcpy(nd + (size_t)r * nw * (size_t)rch,
                   L->data + ((size_t)(y0 + r) * d->w + x0) * (size_t)rch,
                   (size_t)nw * (size_t)rch);
        layer_free(L);
        L->data = nd;
        L->data_len = nw * nh * (uint32_t)rch;
    }
    d->w = nw;
    d->h = nh;
}

/* ------------------------------------------------------------------ */
/* A4. doc-info                                                        */
/* ------------------------------------------------------------------ */

/*
 * doc-info [--fields document,counts,layers] <in.ldx>
 *
 * --fields selects which top level sections are printed (default: all three).
 * The sections always appear in the order document, counts, layers.
 *
 * Key order:
 *   {"document":{"width","height","mode","has_alpha","bit_depth","resolution",
 *                "layer_count"},
 *    "counts":{"raster","group_open","group_close","mask"},
 *    "layers":[{"index","name","kind","opacity","blend","visible","locked",
 *               "is_background","parent","data_len","checksum"}]}
 * "checksum" is FNV-1a-32 over that record's payload bytes, unsigned decimal.
 */
static int cmd_doc_info(int argc, char **argv) {
    static const char *allowed[] = {"fields"};
    Args a;
    Doc *d;
    const char *fields;
    int want_doc = 1, want_counts = 1, want_layers = 1, first = 1;
    uint32_t cnt[4];
    uint16_t i;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 1);
    fields = arg_get(&a, "fields");
    if (fields) {
        const char *p = fields;
        want_doc = want_counts = want_layers = 0;
        while (*p) {
            const char *q = p;
            size_t n;
            while (*q && *q != ',') q++;
            n = (size_t)(q - p);
            if (n == 8 && memcmp(p, "document", 8) == 0) want_doc = 1;
            else if (n == 6 && memcmp(p, "counts", 6) == 0) want_counts = 1;
            else if (n == 6 && memcmp(p, "layers", 6) == 0) want_layers = 1;
            else die(EXIT_USAGE, "E_BAD_ARGS", "--fields items must be document, counts or layers");
            p = *q ? q + 1 : q;
        }
        if (!want_doc && !want_counts && !want_layers)
            die(EXIT_USAGE, "E_BAD_ARGS", "--fields must name at least one section");
    }
    d = ldx_read(a.pos[0]);
    for (i = 0; i < 4; i++) cnt[i] = 0;
    for (i = 0; i < d->nlayers; i++) cnt[d->layers[i].kind]++;
    putchar('{');
    if (want_doc) {
        printf("\"document\":{\"width\":%u,\"height\":%u,\"mode\":\"%s\",\"has_alpha\":%s,"
               "\"bit_depth\":8,\"resolution\":%u,\"layer_count\":%u}",
               d->w, d->h, d->mode == 1 ? "rgb" : "gray", doc_has_alpha(d) ? "true" : "false",
               d->resolution, d->nlayers);
        first = 0;
    }
    if (want_counts) {
        if (!first) putchar(',');
        printf("\"counts\":{\"raster\":%u,\"group_open\":%u,\"group_close\":%u,\"mask\":%u}",
               cnt[KIND_RASTER], cnt[KIND_GROUP_OPEN], cnt[KIND_GROUP_CLOSE], cnt[KIND_MASK]);
        first = 0;
    }
    if (want_layers) {
        if (!first) putchar(',');
        fputs("\"layers\":[", stdout);
        for (i = 0; i < d->nlayers; i++) {
            const Layer *L = &d->layers[i];
            if (i) putchar(',');
            printf("{\"index\":%u,\"name\":\"", i);
            json_escape_out(stdout, L->name, L->name_len);
            printf("\",\"kind\":\"%s\",\"opacity\":%u,\"blend\":\"%s\",\"visible\":%s,"
                   "\"locked\":%s,\"is_background\":%s,\"parent\":%d,\"data_len\":%u,"
                   "\"checksum\":%u}",
                   KIND_NAMES[L->kind], L->opacity, BLEND_NAMES[L->blend],
                   (L->flags & FLAG_VISIBLE) ? "true" : "false",
                   (L->flags & FLAG_LOCKED) ? "true" : "false",
                   (L->flags & FLAG_BACKGROUND) ? "true" : "false", L->parent, L->data_len,
                   docsel_fnv1a(L->data, L->data_len));
        }
        putchar(']');
    }
    puts("}");
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* A6. doc-resize                                                      */
/* ------------------------------------------------------------------ */

/*
 * doc-resize --w W --h H [--anchor tl|tc|tr|cl|cc|cr|bl|bc|br] [--fill r,g,b,a]
 *            <in.ldx> <out.ldx>
 *
 * Resizes the canvas, not the image: existing pixels keep their size and are
 * placed in the new canvas according to the anchor.  Growing fills the new
 * area with --fill (raster records; default all zero, i.e. transparent black)
 * and with 0 for mask records.  Shrinking crops.
 *
 * Placement: with dw = W - w, the old origin lands at
 *     dx = 0 (left anchors), dw / 2 (centre anchors), dw (right anchors)
 * and likewise dy for top/centre/bottom.  The centre case divides by two with
 * C truncation towards zero, so a 10 -> 15 grow centres at dx = 2 and a
 * 10 -> 5 shrink starts at source x = 2.
 *
 * E07: the source window [-dx, -dx+W) x [-dy, -dy+H) is clamped to the canvas;
 * if it was not already contained in the canvas we warn W_CROP_CLAMPED and
 * still exit 0.  An empty result (--w 0 or --h 0, or an empty intersection) is
 * E_BAD_RECT, exit 2.
 */
static int cmd_doc_resize(int argc, char **argv) {
    static const char *allowed[] = {"w", "h", "anchor", "fill"};
    Args a;
    Doc *d;
    long nw, nh, dx, dy, sx0, sy0, sx1, sy1, cx0, cy0, cx1, cy1, r;
    int vt = 0, hz = 0, ch, k;
    uint8_t fill[4] = {0, 0, 0, 0};
    uint16_t i;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    nw = parse_int(arg_req(&a, "w"), "w", 0, MAX_DIM);
    nh = parse_int(arg_req(&a, "h"), "h", 0, MAX_DIM);
    docsel_anchor(arg_get(&a, "anchor") ? arg_get(&a, "anchor") : "tl", &vt, &hz);
    if (nw == 0 || nh == 0) die(EXIT_USAGE, "E_BAD_RECT", "resize would produce an empty canvas");
    d = ldx_read(a.pos[0]);
    ch = doc_channels(d);
    if (arg_get(&a, "fill")) parse_fill(arg_get(&a, "fill"), ch, fill);
    dx = hz == 0 ? 0 : hz == 1 ? (nw - (long)d->w) / 2 : nw - (long)d->w;
    dy = vt == 0 ? 0 : vt == 1 ? (nh - (long)d->h) / 2 : nh - (long)d->h;
    sx0 = -dx;
    sy0 = -dy;
    sx1 = sx0 + nw;
    sy1 = sy0 + nh;
    cx0 = sx0 < 0 ? 0 : sx0;
    cy0 = sy0 < 0 ? 0 : sy0;
    cx1 = sx1 > (long)d->w ? (long)d->w : sx1;
    cy1 = sy1 > (long)d->h ? (long)d->h : sy1;
    if (cx1 <= cx0 || cy1 <= cy0)
        die(EXIT_USAGE, "E_BAD_RECT", "resize rectangle does not intersect the canvas");
    if (sx0 != cx0 || sy0 != cy0 || sx1 != cx1 || sy1 != cy1) /* E07 */
        warn("W_CROP_CLAMPED", "resize rectangle extends beyond the canvas; clamped to canvas");
    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        int rch = L->kind == KIND_RASTER ? ch : (L->kind == KIND_MASK ? 1 : 0);
        uint8_t *nd;
        size_t npx = (size_t)nw * (size_t)nh, p;
        if (!rch) continue;
        nd = (uint8_t *)xmalloc(npx * (size_t)rch);
        if (L->kind == KIND_MASK) memset(nd, 0, npx);
        else
            for (p = 0; p < npx; p++)
                for (k = 0; k < rch; k++) nd[p * (size_t)rch + (size_t)k] = fill[k];
        for (r = cy0; r < cy1; r++)
            memcpy(nd + (((size_t)(r + dy) * (size_t)nw) + (size_t)(cx0 + dx)) * (size_t)rch,
                   L->data + ((size_t)r * d->w + (size_t)cx0) * (size_t)rch,
                   (size_t)(cx1 - cx0) * (size_t)rch);
        layer_free(L);
        L->data = nd;
        L->data_len = (uint32_t)(npx * (size_t)rch);
    }
    d->w = (uint32_t)nw;
    d->h = (uint32_t)nh;
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* A7. doc-convert                                                     */
/* ------------------------------------------------------------------ */

/*
 * doc-convert [--mode gray|rgb] [--alpha 0|1] <in.ldx> <out.ldx>
 *
 * Converts the channel layout of every raster record; mask records are one
 * byte per pixel in either mode and are left alone.  Omitting a flag keeps the
 * current setting, so `doc-convert in out` is a rewrite.
 *
 * rgb -> gray uses the Rec.601 luma weights scaled to 256:
 *     y = (77*r + 150*g + 29*b + 128) / 256      (77 + 150 + 29 = 256)
 * with the +128 making the division round half up.  gray -> rgb replicates the
 * gray value into all three channels.
 *
 * Dropping alpha simply removes the byte: the colour channels are NOT
 * premultiplied by it.  Adding alpha sets every pixel to 255 (opaque).
 * data_len is recomputed from layer_expected_len() for the new layout.
 */
static int cmd_doc_convert(int argc, char **argv) {
    static const char *allowed[] = {"mode", "alpha"};
    Args a;
    Doc *d;
    const char *ms;
    int nmode, nalpha, ocs, ncs, och, nch, oai, k;
    uint16_t i;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    nmode = d->mode;
    ms = arg_get(&a, "mode");
    if (ms) {
        if (strcmp(ms, "gray") == 0) nmode = 0;
        else if (strcmp(ms, "rgb") == 0) nmode = 1;
        else if (strcmp(ms, "indexed") == 0)
            die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "indexed mode not supported");
        else die(EXIT_USAGE, "E_BAD_ARGS", "--mode must be gray or rgb");
    }
    nalpha = (int)arg_int(&a, "alpha", doc_has_alpha(d), 0, 1);
    ocs = doc_color_channels(d);
    och = doc_channels(d);
    oai = doc_has_alpha(d) ? ocs : -1;
    ncs = nmode == 1 ? 3 : 1;
    nch = ncs + (nalpha ? 1 : 0);
    npx = (size_t)d->w * d->h;
    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        uint8_t *nd;
        if (L->kind != KIND_RASTER) continue;
        nd = (uint8_t *)xmalloc(npx * (size_t)nch);
        for (p = 0; p < npx; p++) {
            const uint8_t *s = L->data + p * (size_t)och;
            uint8_t *o = nd + p * (size_t)nch;
            if (ocs == ncs) {
                for (k = 0; k < ncs; k++) o[k] = s[k];
            } else if (ocs == 3) { /* rgb -> gray, integer luma, half up */
                o[0] = (uint8_t)((77 * (int)s[0] + 150 * (int)s[1] + 29 * (int)s[2] + 128) / 256);
            } else { /* gray -> rgb */
                for (k = 0; k < 3; k++) o[k] = s[0];
            }
            if (nalpha) o[ncs] = oai >= 0 ? s[oai] : 255;
        }
        layer_free(L);
        L->data = nd;
    }
    d->mode = (uint8_t)nmode;
    d->flags = (uint16_t)((d->flags & (uint16_t)~(uint16_t)1) | (nalpha ? 1 : 0));
    for (i = 0; i < d->nlayers; i++)
        d->layers[i].data_len = layer_expected_len(d, d->layers[i].kind);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* A8. doc-trim                                                        */
/* ------------------------------------------------------------------ */

/*
 * doc-trim <in.ldx> <out.ldx>
 *
 * Removes a fully transparent border from the canvas.  The border is measured
 * on the rendered document (doc_render), so a pixel is kept when the composite
 * of every visible layer has a non-zero alpha there; all records, masks
 * included, are then cropped to that box.  A document without an alpha channel
 * has nothing transparent to trim, so it is a no-op that still rewrites the
 * file.  A document that renders fully transparent would trim to nothing:
 * E_BAD_RECT, exit 2.
 */
static int cmd_doc_trim(int argc, char **argv) {
    Args a;
    Doc *d;
    uint8_t *canvas;
    int cc;
    uint32_t x, y, x0, y0, x1, y1;
    int any = 0;
    parse_args(argc, argv, 2, &a, NULL, 0);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    if (!doc_has_alpha(d)) {
        ldx_write(d, a.pos[1]);
        return EXIT_OK;
    }
    cc = doc_color_channels(d);
    canvas = doc_render(d);
    x0 = d->w;
    y0 = d->h;
    x1 = 0;
    y1 = 0;
    for (y = 0; y < d->h; y++)
        for (x = 0; x < d->w; x++)
            if (canvas[((size_t)y * d->w + x) * (size_t)(cc + 1) + (size_t)cc]) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x + 1 > x1) x1 = x + 1;
                if (y + 1 > y1) y1 = y + 1;
                any = 1;
            }
    free(canvas);
    if (!any) die(EXIT_USAGE, "E_BAD_RECT", "document is fully transparent; nothing would remain");
    if (x0 != 0 || y0 != 0 || x1 != d->w || y1 != d->h) docsel_crop(d, x0, y0, x1 - x0, y1 - y0);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* A9. doc-set-dpi                                                     */
/* ------------------------------------------------------------------ */

/*
 * doc-set-dpi --dpi N <in.ldx> <out.ldx>
 * The stored resolution is Q16.16, so the integer dpi is shifted left 16.
 */
static int cmd_doc_set_dpi(int argc, char **argv) {
    static const char *allowed[] = {"dpi"};
    Args a;
    Doc *d;
    long dpi;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    dpi = parse_int(arg_req(&a, "dpi"), "dpi", 1, 65535);
    d = ldx_read(a.pos[0]);
    d->resolution = (uint32_t)dpi << 16;
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* A10. doc-diff                                                       */
/* ------------------------------------------------------------------ */

/*
 * doc-diff <a.ldx> <b.ldx>
 *
 * Structural and pixel difference report on stdout, no output file.
 *
 * Key order:
 *   {"same",
 *    "a":{"width","height","mode","has_alpha","resolution","layer_count"},
 *    "b":{ ... same six ... },
 *    "structural":{"dimensions","mode","has_alpha","resolution","layer_count",
 *                  "records"},
 *    "pixels":{"comparable","channels","total","differing_pixels",
 *              "differing_samples","max_abs_diff","sum_abs_diff"}}
 *
 * "records" is true when the two documents hold the same number of records and
 * every record agrees on name, kind, opacity, blend, flags, parent, data_len
 * and payload bytes.  "same" is true when every structural field agrees.
 * Pixels are comparable only when width, height, mode and has_alpha agree;
 * both documents are then rendered (doc_render, colour channels plus alpha)
 * and compared sample by sample.  When they are not comparable the five
 * numeric pixel fields are 0.
 */
static void docsel_diff_side(const char *tag, const Doc *d) {
    printf("\"%s\":{\"width\":%u,\"height\":%u,\"mode\":\"%s\",\"has_alpha\":%s,"
           "\"resolution\":%u,\"layer_count\":%u}",
           tag, d->w, d->h, d->mode == 1 ? "rgb" : "gray",
           doc_has_alpha(d) ? "true" : "false", d->resolution, d->nlayers);
}

static int cmd_doc_diff(int argc, char **argv) {
    Args a;
    Doc *A, *B;
    int m_dim, m_mode, m_alpha, m_res, m_count, m_rec = 1, comparable;
    int ch = 0;
    unsigned long long total = 0, dpix = 0, dsamp = 0, sum = 0;
    int maxd = 0;
    uint16_t i;
    parse_args(argc, argv, 2, &a, NULL, 0);
    positional(&a, 2);
    A = ldx_read(a.pos[0]);
    B = ldx_read(a.pos[1]);
    m_dim = (A->w == B->w && A->h == B->h);
    m_mode = (A->mode == B->mode);
    m_alpha = (doc_has_alpha(A) == doc_has_alpha(B));
    m_res = (A->resolution == B->resolution);
    m_count = (A->nlayers == B->nlayers);
    if (!m_count) m_rec = 0;
    else
        for (i = 0; i < A->nlayers && m_rec; i++) {
            const Layer *P = &A->layers[i], *Q = &B->layers[i];
            if (P->name_len != Q->name_len || memcmp(P->name, Q->name, P->name_len) != 0 ||
                P->kind != Q->kind || P->opacity != Q->opacity || P->blend != Q->blend ||
                P->flags != Q->flags || P->parent != Q->parent || P->data_len != Q->data_len ||
                (P->data_len && memcmp(P->data, Q->data, P->data_len) != 0))
                m_rec = 0;
        }
    comparable = m_dim && m_mode && m_alpha;
    if (comparable) {
        uint8_t *ca = doc_render(A), *cb = doc_render(B);
        size_t npx = (size_t)A->w * A->h, p;
        int k;
        ch = doc_color_channels(A) + 1;
        total = (unsigned long long)npx;
        for (p = 0; p < npx; p++) {
            int hit = 0;
            for (k = 0; k < ch; k++) {
                int va = ca[p * (size_t)ch + (size_t)k], vb = cb[p * (size_t)ch + (size_t)k];
                int dv = va > vb ? va - vb : vb - va;
                if (dv) {
                    hit = 1;
                    dsamp++;
                    sum += (unsigned long long)dv;
                    if (dv > maxd) maxd = dv;
                }
            }
            if (hit) dpix++;
        }
        free(ca);
        free(cb);
    }
    printf("{\"same\":%s,",
           (m_dim && m_mode && m_alpha && m_res && m_count && m_rec) ? "true" : "false");
    docsel_diff_side("a", A);
    putchar(',');
    docsel_diff_side("b", B);
    printf(",\"structural\":{\"dimensions\":%s,\"mode\":%s,\"has_alpha\":%s,\"resolution\":%s,"
           "\"layer_count\":%s,\"records\":%s}",
           m_dim ? "true" : "false", m_mode ? "true" : "false", m_alpha ? "true" : "false",
           m_res ? "true" : "false", m_count ? "true" : "false", m_rec ? "true" : "false");
    printf(",\"pixels\":{\"comparable\":%s,\"channels\":%d,\"total\":%llu,"
           "\"differing_pixels\":%llu,\"differing_samples\":%llu,\"max_abs_diff\":%d,"
           "\"sum_abs_diff\":%llu}}\n",
           comparable ? "true" : "false", ch, total, dpix, dsamp, maxd, sum);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* F77. sel-rect                                                       */
/* ------------------------------------------------------------------ */

/*
 * sel-rect --x X --y Y --w W --h H [--index I] <in.ldx> <out.ldx>
 *
 * Writes a mask record (kind mask, one byte per pixel) directly after the
 * raster layer at --index, which is where doc_render() looks for it; an
 * existing mask on that layer is overwritten in place.  Selected pixels are
 * 255, everything else 0.
 *
 * E07: the rectangle is clamped to the canvas with a W_CROP_CLAMPED warning
 * and exit 0; no intersection at all is E_BAD_RECT, exit 2.
 */
static int cmd_sel_rect(int argc, char **argv) {
    static const char *allowed[] = {"x", "y", "w", "h", "index"};
    Args a;
    Doc *d;
    long x, y, w, h, x0, y0, x1, y1, r, c;
    int idx, m;
    uint8_t *md;
    parse_args(argc, argv, 2, &a, allowed, 5);
    positional(&a, 2);
    x = parse_int(arg_req(&a, "x"), "x", -MAX_DIM, MAX_DIM);
    y = parse_int(arg_req(&a, "y"), "y", -MAX_DIM, MAX_DIM);
    w = parse_int(arg_req(&a, "w"), "w", 1, MAX_DIM);
    h = parse_int(arg_req(&a, "h"), "h", 1, MAX_DIM);
    d = ldx_read(a.pos[0]);
    idx = docsel_pick(d, &a, 0);
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > (long)d->w ? (long)d->w : x + w;
    y1 = y + h > (long)d->h ? (long)d->h : y + h;
    if (x1 <= x0 || y1 <= y0)
        die(EXIT_USAGE, "E_BAD_RECT", "selection rectangle does not intersect the canvas");
    if (x0 != x || y0 != y || x1 != x + w || y1 != y + h) /* E07 */
        warn("W_CROP_CLAMPED", "selection rectangle clamped to canvas");
    m = docsel_mask_slot(d, idx);
    md = d->layers[m].data;
    memset(md, 0, (size_t)d->w * d->h);
    for (r = y0; r < y1; r++)
        for (c = x0; c < x1; c++) md[(size_t)r * d->w + (size_t)c] = 255;
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* F78. sel-from-alpha                                                 */
/* ------------------------------------------------------------------ */

/*
 * sel-from-alpha [--index I] <in.ldx> <out.ldx>
 * Copies the alpha channel of the raster layer at --index into its mask
 * record, creating the mask if it has none.  A document without alpha is
 * E_MODE_UNSUPPORTED, exit 3.
 */
static int cmd_sel_from_alpha(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    int idx, m, ch, cc;
    size_t npx, p;
    uint8_t *md;
    const uint8_t *sd;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = docsel_pick(d, &a, 0);
    if (!doc_has_alpha(d))
        die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "document has no alpha channel");
    ch = doc_channels(d);
    cc = doc_color_channels(d);
    npx = (size_t)d->w * d->h;
    sd = d->layers[idx].data;
    m = docsel_mask_slot(d, idx);
    md = d->layers[m].data;
    for (p = 0; p < npx; p++) md[p] = sd[p * (size_t)ch + (size_t)cc];
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* F79. sel-invert                                                     */
/* ------------------------------------------------------------------ */

/*
 * sel-invert [--index I] <in.ldx> <out.ldx>
 * Inverts a mask record: m -> 255 - m.  --index may name the mask record
 * itself or the raster layer it is attached to; a raster layer without a mask
 * is E_BAD_ARGS, exit 2.
 */
static int cmd_sel_invert(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    int idx, m;
    uint8_t *md;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = docsel_pick(d, &a, 1);
    if (d->layers[idx].kind == KIND_MASK) m = idx;
    else {
        m = docsel_mask_of(d, idx);
        if (m < 0) die(EXIT_USAGE, "E_BAD_ARGS", "layer has no mask record to invert");
    }
    md = d->layers[m].data;
    npx = (size_t)d->w * d->h;
    for (p = 0; p < npx; p++) md[p] = (uint8_t)(255 - md[p]);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* F80. sel-apply                                                      */
/* ------------------------------------------------------------------ */

/*
 * sel-apply --op clear|fill [--colour r,g,b,a] [--index I] <in.ldx> <out.ldx>
 *
 * Applies the mask of the raster layer at --index to its pixels.  The mask is
 * a coverage weight, so a partially selected pixel is mixed, per channel:
 *     out = ((255 - m) * old + m * target + 127) / 255      (round half up)
 * With m = 255 that is exactly the target, with m = 0 the pixel is untouched.
 * --op clear uses a target of 0 in every channel (transparent black, or black
 * in a document without alpha) and rejects --colour; --op fill requires
 * --colour with one component per channel.  The mask record itself is kept.
 */
static int cmd_sel_apply(int argc, char **argv) {
    static const char *allowed[] = {"index", "op", "colour"};
    Args a;
    Doc *d;
    const char *op;
    int idx, m, ch, k, do_fill;
    uint8_t target[4] = {0, 0, 0, 0};
    const uint8_t *md;
    uint8_t *pd;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    op = arg_req(&a, "op");
    if (strcmp(op, "clear") == 0) do_fill = 0;
    else if (strcmp(op, "fill") == 0) do_fill = 1;
    else {
        die(EXIT_USAGE, "E_BAD_ARGS", "--op must be clear or fill");
        return EXIT_USAGE;
    }
    if (!do_fill && arg_get(&a, "colour"))
        die(EXIT_USAGE, "E_BAD_ARGS", "--colour is only valid with --op fill");
    d = ldx_read(a.pos[0]);
    idx = docsel_pick(d, &a, 0);
    m = docsel_mask_of(d, idx);
    if (m < 0) die(EXIT_USAGE, "E_BAD_ARGS", "layer has no mask record to apply");
    ch = doc_channels(d);
    if (do_fill) parse_fill(arg_req(&a, "colour"), ch, target);
    npx = (size_t)d->w * d->h;
    md = d->layers[m].data;
    pd = d->layers[idx].data;
    for (p = 0; p < npx; p++) {
        int mv = md[p];
        if (!mv) continue;
        for (k = 0; k < ch; k++) {
            int old = pd[p * (size_t)ch + (size_t)k];
            pd[p * (size_t)ch + (size_t)k] =
                (uint8_t)(((255 - mv) * old + mv * (int)target[k] + 127) / 255);
        }
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* F81. stat-histogram                                                 */
/* ------------------------------------------------------------------ */

/*
 * stat-histogram [--index I] [--channel r|g|b|a|v] <in.ldx>
 *
 * --channel names one byte of the pixel: r, g, b on an rgb document, v (alias
 * gray) on a gray document or a mask record, a on any document with alpha.
 * Default: v for gray and for mask records, r for rgb.  Asking for r, g or b
 * on a gray document is E_MODE_UNSUPPORTED, exit 3 (E10); asking for a without
 * an alpha channel is likewise E_MODE_UNSUPPORTED.
 *
 * Key order:
 *   {"index","channel","total","min","max","sum","mean","bins":[256 counts]}
 * "mean" is sum/total rounded half up.
 */
static int cmd_stat_histogram(int argc, char **argv) {
    static const char *allowed[] = {"index", "channel"};
    Args a;
    Doc *d;
    const Layer *L;
    const char *cs;
    int idx, ch, cc, ai, off = 0, i, mn = 255, mx = 0;
    uint32_t bins[256];
    unsigned long long sum = 0, total;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 1);
    d = ldx_read(a.pos[0]);
    idx = docsel_pick(d, &a, 1);
    L = &d->layers[idx];
    docsel_geom(d, L, &ch, &cc, &ai);
    cs = arg_get(&a, "channel");
    if (!cs) cs = (L->kind == KIND_MASK || cc == 1) ? "v" : "r";
    if (strcmp(cs, "v") == 0 || strcmp(cs, "gray") == 0) {
        if (L->kind != KIND_MASK && cc != 1)
            die(EXIT_USAGE, "E_BAD_ARGS", "--channel v is only valid on a gray document or a mask");
        off = 0;
    } else if (strcmp(cs, "a") == 0) {
        if (ai < 0) die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "document has no alpha channel");
        off = ai;
    } else if (strcmp(cs, "r") == 0 || strcmp(cs, "g") == 0 || strcmp(cs, "b") == 0) {
        if (L->kind == KIND_MASK)
            die(EXIT_USAGE, "E_BAD_ARGS", "--channel must be v for a mask record");
        if (cc != 3) /* E10 */
            die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "r, g and b need a three channel document");
        off = cs[0] == 'r' ? 0 : cs[0] == 'g' ? 1 : 2;
    } else
        die(EXIT_USAGE, "E_BAD_ARGS", "--channel must be r, g, b, a or v");
    for (i = 0; i < 256; i++) bins[i] = 0;
    npx = (size_t)d->w * d->h;
    for (p = 0; p < npx; p++) {
        int v = L->data[p * (size_t)ch + (size_t)off];
        bins[v]++;
        sum += (unsigned long long)v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    total = (unsigned long long)npx;
    printf("{\"index\":%d,\"channel\":\"%s\",\"total\":%llu,\"min\":%d,\"max\":%d,\"sum\":%llu,"
           "\"mean\":%llu,\"bins\":[",
           idx, cs, total, mn, mx, sum, (unsigned long long)div_round((int64_t)sum, (int64_t)total));
    for (i = 0; i < 256; i++) {
        if (i) putchar(',');
        printf("%u", bins[i]);
    }
    puts("]}");
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* F82. stat-bbox                                                      */
/* ------------------------------------------------------------------ */

/*
 * stat-bbox [--index I] <in.ldx>
 *
 * Bounding box of the non-transparent pixels of one record: alpha != 0 for a
 * raster layer, coverage != 0 for a mask record.  A document without an alpha
 * channel has no transparent pixels, so the box is the whole canvas.
 *
 * Key order: {"index","width","height","empty","x","y","w","h"}
 * An empty box reports "empty":true with x, y, w and h all 0.
 */
static int cmd_stat_bbox(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    const Layer *L;
    int idx, ch, cc, ai, any = 0;
    uint32_t x, y, x0, y0, x1, y1;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 1);
    d = ldx_read(a.pos[0]);
    idx = docsel_pick(d, &a, 1);
    L = &d->layers[idx];
    docsel_geom(d, L, &ch, &cc, &ai);
    x0 = d->w;
    y0 = d->h;
    x1 = 0;
    y1 = 0;
    for (y = 0; y < d->h; y++)
        for (x = 0; x < d->w; x++) {
            int op = ai < 0 ? 1 : L->data[((size_t)y * d->w + x) * (size_t)ch + (size_t)ai] != 0;
            if (!op) continue;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x + 1 > x1) x1 = x + 1;
            if (y + 1 > y1) y1 = y + 1;
            any = 1;
        }
    if (!any) {
        x0 = y0 = x1 = y1 = 0;
    }
    printf("{\"index\":%d,\"width\":%u,\"height\":%u,\"empty\":%s,\"x\":%u,\"y\":%u,\"w\":%u,"
           "\"h\":%u}\n",
           idx, d->w, d->h, any ? "false" : "true", x0, y0, x1 - x0, y1 - y0);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* F83. stat-checksum                                                  */
/* ------------------------------------------------------------------ */

/*
 * stat-checksum [--index I] <in.ldx>
 *
 * Deterministic integer digest of one record's payload, FNV-1a 32 bit (basis
 * 2166136261, prime 16777619, wrapping modulo 2^32).  --index may name any
 * record, including group markers, whose payload is empty and whose digest is
 * therefore the bare basis.  "document" digests the whole document (see
 * docsel_doc_digest above) so two files can be compared with one number.
 *
 * Key order:
 *   {"index","kind","data_len","algorithm","checksum","checksum_hex",
 *    "document","document_hex"}
 * "checksum" and "document" are unsigned decimal; the *_hex fields are the
 * same values as eight lower-case hex digits.
 */
static int cmd_stat_checksum(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    const Layer *L;
    int idx;
    uint32_t c, dg;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 1);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    if (idx >= (int)d->nlayers) die(EXIT_USAGE, "E_BAD_ARGS", "--index out of range");
    L = &d->layers[idx];
    c = docsel_fnv1a(L->data, L->data_len);
    dg = docsel_doc_digest(d);
    printf("{\"index\":%d,\"kind\":\"%s\",\"data_len\":%u,\"algorithm\":\"fnv1a32\","
           "\"checksum\":%u,\"checksum_hex\":\"%08x\",\"document\":%u,\"document_hex\":\"%08x\"}\n",
           idx, KIND_NAMES[L->kind], L->data_len, c, c, dg, dg);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* F84. stat-count                                                     */
/* ------------------------------------------------------------------ */

/*
 * stat-count [--index I] <in.ldx>
 *
 * Distinct colour count of one record.  A colour is the tuple of colour
 * channels only (the gray value, or the rgb triple, or the coverage byte of a
 * mask); alpha is counted separately as the number of distinct alpha values.
 * Exact, not sampled: gray uses a 256 entry table and rgb a 2 MiB bitset over
 * the 2^24 possible triples.
 *
 * Key order: {"index","pixels","distinct_colors","has_alpha","distinct_alpha"}
 * "distinct_alpha" is 0 when the record has no alpha channel.
 */
static int cmd_stat_count(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    const Layer *L;
    int idx, ch, cc, ai, i;
    uint32_t ncol = 0, nalp = 0;
    uint8_t seen1[256], seena[256];
    uint8_t *seen3 = NULL;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 1);
    d = ldx_read(a.pos[0]);
    idx = docsel_pick(d, &a, 1);
    L = &d->layers[idx];
    docsel_geom(d, L, &ch, &cc, &ai);
    for (i = 0; i < 256; i++) {
        seen1[i] = 0;
        seena[i] = 0;
    }
    if (L->kind != KIND_MASK && cc == 3) seen3 = (uint8_t *)xcalloc((size_t)1 << 21);
    npx = (size_t)d->w * d->h;
    for (p = 0; p < npx; p++) {
        const uint8_t *s = L->data + p * (size_t)ch;
        if (seen3) {
            uint32_t key = ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | (uint32_t)s[2];
            if (!(seen3[key >> 3] & (1u << (key & 7)))) {
                seen3[key >> 3] |= (uint8_t)(1u << (key & 7));
                ncol++;
            }
        } else if (!seen1[s[0]]) {
            seen1[s[0]] = 1;
            ncol++;
        }
        if (ai >= 0 && !seena[s[ai]]) {
            seena[s[ai]] = 1;
            nalp++;
        }
    }
    free(seen3);
    printf("{\"index\":%d,\"pixels\":%llu,\"distinct_colors\":%u,\"has_alpha\":%s,"
           "\"distinct_alpha\":%u}\n",
           idx, (unsigned long long)npx, ncol, ai >= 0 ? "true" : "false", nalp);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

#define FAM_DOCSEL_COMMANDS \
    {"doc-info", cmd_doc_info, "doc"}, \
    {"doc-resize", cmd_doc_resize, "doc"}, \
    {"doc-convert", cmd_doc_convert, "doc"}, \
    {"doc-trim", cmd_doc_trim, "doc"}, \
    {"doc-set-dpi", cmd_doc_set_dpi, "doc"}, \
    {"doc-diff", cmd_doc_diff, "doc"}, \
    {"sel-rect", cmd_sel_rect, "select"}, \
    {"sel-from-alpha", cmd_sel_from_alpha, "select"}, \
    {"sel-invert", cmd_sel_invert, "select"}, \
    {"sel-apply", cmd_sel_apply, "select"}, \
    {"stat-histogram", cmd_stat_histogram, "select"}, \
    {"stat-bbox", cmd_stat_bbox, "select"}, \
    {"stat-checksum", cmd_stat_checksum, "select"}, \
    {"stat-count", cmd_stat_count, "select"},
