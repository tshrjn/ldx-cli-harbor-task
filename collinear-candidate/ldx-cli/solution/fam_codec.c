/* ------------------------------------------------------------------ */
/* X1. Container codecs                                                */
/* ------------------------------------------------------------------ */
/*
 * Record payloads may be stored compressed.  The codec id lives in bits 4-7 of
 * the record's flags byte, which were previously reserved-and-zero; the record's
 * `reserved` u16 stays zero, so planted edge E12 is untouched.  `data_len` is the
 * STORED length; the decoded length is always layer_expected_len(), which is how
 * a reader knows when to stop.
 *
 * Writing a decoder is easy.  Reproducing the ENCODER byte for byte is the task,
 * and every decision below is invisible to a reader of the format and obvious in
 * a hexdump of the output:
 *
 *   E17  a run of exactly two bytes.  RLE emits it as a run; PACKBITS emits it as
 *        literals.  The two codecs disagree on purpose.
 *   E18  runs never cross a row boundary.  The encoder restarts at every row, so
 *        a uniform image does NOT collapse to one long run.
 *   E19  --compress auto picks the smallest result; ties go to the LOWER codec id.
 *   E20  the final literal run of a row is flushed at its exact remaining length.
 */

#define CODEC_NONE 0
#define CODEC_RLE 1
#define CODEC_PACKBITS 2
#define CODEC_DELTA 3
#define CODEC_COUNT 4

static int codec_from_name(const char *s) {
    if (strcmp(s, "none") == 0) return CODEC_NONE;
    if (strcmp(s, "rle") == 0) return CODEC_RLE;
    if (strcmp(s, "packbits") == 0) return CODEC_PACKBITS;
    if (strcmp(s, "delta") == 0) return CODEC_DELTA;
    if (strcmp(s, "auto") == 0) return -2;
    die(EXIT_USAGE, "E_BAD_ARGS", "--compress must be none, rle, packbits, delta or auto");
    return -1;
}

static const char *codec_name(int c) {
    switch (c) {
    case CODEC_RLE: return "rle";
    case CODEC_PACKBITS: return "packbits";
    case CODEC_DELTA: return "delta";
    default: return "none";
    }
}

/* row stride in bytes for the record's payload */
static size_t codec_stride(const Doc *d, uint8_t kind) {
    return (size_t)d->w * (size_t)(kind == KIND_MASK ? 1 : doc_channels(d));
}

/* bytes per pixel -- the delta filter subtracts the SAME CHANNEL of the previous
 * pixel, not the previous byte.  Subtracting the previous byte would difference R
 * against G and turn a flat image into noise, which is the classic mistake. */
static size_t codec_bpp(const Doc *d, uint8_t kind) {
    return (size_t)(kind == KIND_MASK ? 1 : doc_channels(d));
}

/* --- RLE: [count][byte], count 1..255, runs confined to one row (E18) ------ */
static uint8_t *codec_rle_enc(const uint8_t *src, size_t len, size_t stride, size_t *out) {
    uint8_t *dst = (uint8_t *)xmalloc(len * 2 + 2);
    size_t o = 0, p = 0;
    while (p < len) {
        size_t row_end = p - (stride ? p % stride : 0) + (stride ? stride : len);
        if (row_end > len) row_end = len;
        while (p < row_end) {
            size_t run = 1;
            while (p + run < row_end && src[p + run] == src[p] && run < 255) run++;
            dst[o++] = (uint8_t)run;          /* E17: a run of 2 IS a run here */
            dst[o++] = src[p];
            p += run;
        }
    }
    *out = o;
    return dst;
}

static int codec_rle_dec(const uint8_t *src, size_t len, uint8_t *dst, size_t want) {
    size_t i = 0, o = 0;
    while (i + 1 < len) {
        size_t n = src[i], k;
        if (n == 0) return 0;
        if (o + n > want) return 0;
        for (k = 0; k < n; k++) dst[o++] = src[i + 1];
        i += 2;
    }
    return i == len && o == want;
}

/* --- PackBits: n<=127 -> n+1 literals; n>=129 -> next byte 257-n times ----- */
static uint8_t *codec_pb_enc(const uint8_t *src, size_t len, size_t stride, size_t *out) {
    uint8_t *dst = (uint8_t *)xmalloc(len * 2 + 2);
    size_t o = 0, p = 0;
    while (p < len) {
        size_t row_end = p - (stride ? p % stride : 0) + (stride ? stride : len);
        if (row_end > len) row_end = len;
        while (p < row_end) {
            size_t run = 1, lit;
            while (p + run < row_end && src[p + run] == src[p] && run < 128) run++;
            if (run > 2) {                     /* E17: only 3+ is worth a run here */
                dst[o++] = (uint8_t)(257 - run);
                dst[o++] = src[p];
                p += run;
                continue;
            }
            /* gather literals up to the next run of 3, the row end, or 128 bytes */
            lit = 0;
            while (p + lit < row_end && lit < 128) {
                if (p + lit + 2 < row_end && src[p + lit] == src[p + lit + 1]
                    && src[p + lit] == src[p + lit + 2]) break;
                lit++;
            }
            dst[o++] = (uint8_t)(lit - 1);     /* E20: exact remaining count */
            memcpy(dst + o, src + p, lit);
            o += lit;
            p += lit;
        }
    }
    *out = o;
    return dst;
}

static int codec_pb_dec(const uint8_t *src, size_t len, uint8_t *dst, size_t want) {
    size_t i = 0, o = 0;
    while (i < len) {
        int n = src[i++];
        if (n == 128) return 0;                /* reserved, never emitted */
        if (n <= 127) {
            size_t k = (size_t)n + 1;
            if (i + k > len || o + k > want) return 0;
            memcpy(dst + o, src + i, k);
            o += k; i += k;
        } else {
            size_t k = (size_t)(257 - n), j;
            if (i >= len || o + k > want) return 0;
            for (j = 0; j < k; j++) dst[o++] = src[i];
            i++;
        }
    }
    return o == want;
}

/* --- Delta: per-row byte difference, then RLE ------------------------------ */
static uint8_t *codec_delta_enc(const uint8_t *src, size_t len, size_t stride,
                                size_t bpp, size_t *out) {
    uint8_t *tmp = (uint8_t *)xmalloc(len ? len : 1), *res;
    size_t p;
    for (p = 0; p < len; p++) {
        size_t col = stride ? p % stride : p;
        tmp[p] = (uint8_t)(col < bpp ? src[p] : (uint8_t)(src[p] - src[p - bpp]));
    }
    res = codec_rle_enc(tmp, len, stride, out);
    free(tmp);
    return res;
}

static int codec_delta_dec(const uint8_t *src, size_t len, uint8_t *dst, size_t want,
                           size_t stride, size_t bpp) {
    size_t p;
    if (!codec_rle_dec(src, len, dst, want)) return 0;
    for (p = 0; p < want; p++) {
        size_t col = stride ? p % stride : p;
        if (col >= bpp) dst[p] = (uint8_t)(dst[p] + dst[p - bpp]);
    }
    return 1;
}

/* --- one entry point each way --------------------------------------------- */
static uint8_t *codec_encode(int codec, const uint8_t *src, size_t len, size_t stride, size_t bpp, size_t *out) {
    switch (codec) {
    case CODEC_RLE: return codec_rle_enc(src, len, stride, out);
    case CODEC_PACKBITS: return codec_pb_enc(src, len, stride, out);
    case CODEC_DELTA: return codec_delta_enc(src, len, stride, bpp, out);
    default: {
        uint8_t *d = (uint8_t *)xmalloc(len ? len : 1);
        memcpy(d, src, len);
        *out = len;
        return d;
    }
    }
}

static int codec_decode(int codec, const uint8_t *src, size_t len, uint8_t *dst, size_t want, size_t stride, size_t bpp) {
    switch (codec) {
    case CODEC_RLE: return codec_rle_dec(src, len, dst, want);
    case CODEC_PACKBITS: return codec_pb_dec(src, len, dst, want);
    case CODEC_DELTA: return codec_delta_dec(src, len, dst, want, stride, bpp);
    default:
        if (len != want) return 0;
        memcpy(dst, src, want);
        return 1;
    }
}

/* E19: smallest wins; a tie goes to the lower codec id. */
static int codec_pick_auto(const uint8_t *src, size_t len, size_t stride, size_t bpp) {
    int best = CODEC_NONE, c;
    size_t bestlen = len;
    for (c = CODEC_RLE; c < CODEC_COUNT; c++) {
        size_t n = 0;
        uint8_t *e = codec_encode(c, src, len, stride, bpp, &n);
        free(e);
        if (n < bestlen) { bestlen = n; best = c; }
    }
    return best;
}

/* Apply a codec choice to the whole document (or one record) and write it out. */
static void codec_apply(Doc *d, int want, int only_idx) {
    uint16_t i;
    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        if (only_idx >= 0 && i != (uint16_t)only_idx) continue;
        if (layer_expected_len(d, L->kind) == 0) { L->codec = CODEC_NONE; continue; }
        L->codec = (want == -2) ? codec_pick_auto(L->data, L->data_len, codec_stride(d, L->kind),
                                                  codec_bpp(d, L->kind))
                                : want;
    }
}

