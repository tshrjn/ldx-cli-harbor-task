/*
 * refldx -- reference implementation of the LDX layered-document CLI.
 *
 * Original work for the collinear-candidate/ldx-cli task.  Integer-only,
 * single-threaded, no SIMD intrinsics, no external libraries.
 *
 * Build:  cc -O2 -std=c99 -static -s -o refldx ldx.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Exit codes and structured errors                                    */
/* ------------------------------------------------------------------ */

#define EXIT_OK 0
#define EXIT_USAGE 2       /* E_USAGE, E_BAD_ARGS, E_BAD_RECT             */
#define EXIT_UNSUPPORTED 3 /* E_MODE_UNSUPPORTED, E_UNSUPPORTED            */
#define EXIT_BAD_FILE 4    /* E_BAD_FILE (malformed LDX or PNM input)      */
#define EXIT_IO 5          /* E_IO (cannot open/read/write)                */

static void json_escape_out(FILE *f, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') fputs("\\\"", f);
        else if (c == '\\') fputs("\\\\", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
}

static void emit_json(FILE *f, const char *code, const char *msg) {
    fputs("{\"code\":\"", f);
    json_escape_out(f, code, strlen(code));
    fputs("\",\"message\":\"", f);
    json_escape_out(f, msg, strlen(msg));
    fputs("\"}\n", f);
    fflush(f);
}

static void die(int rc, const char *code, const char *msg) {
    emit_json(stderr, code, msg);
    exit(rc);
}

static void warn(const char *code, const char *msg) { emit_json(stderr, code, msg); }

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die(EXIT_IO, "E_IO", "out of memory");
    return p;
}

static void *xcalloc(size_t n) {
    void *p = calloc(n ? n : 1, 1);
    if (!p) die(EXIT_IO, "E_IO", "out of memory");
    return p;
}

/* ------------------------------------------------------------------ */
/* Document model                                                      */
/* ------------------------------------------------------------------ */

#define KIND_RASTER 0
#define KIND_GROUP_OPEN 1
#define KIND_GROUP_CLOSE 2
#define KIND_MASK 3

#define FLAG_VISIBLE 1
#define FLAG_LOCKED 2
#define FLAG_BACKGROUND 4

#define BLEND_NORMAL 0
#define BLEND_MULTIPLY 1
#define BLEND_SCREEN 2
#define BLEND_DARKEN 3
#define BLEND_LIGHTEN 4
#define BLEND_DIFFERENCE 5
#define BLEND_COUNT 6

static const char *BLEND_NAMES[BLEND_COUNT] = {"normal",  "multiply", "screen",
                                               "darken",  "lighten",  "difference"};
static const char *KIND_NAMES[4] = {"raster", "group_open", "group_close", "mask"};

#define MAX_DIM 16384
#define MAX_LAYERS 4096

typedef struct {
    uint8_t name_len;
    char name[256];
    uint8_t kind, opacity, blend, flags;
    int16_t parent;
    uint32_t data_len;      /* DECODED length: always w*h*channels for a raster */
    uint32_t stored_len;    /* length actually on disk after the codec */
    uint8_t codec;          /* CODEC_* -- lives in flags bits 4-7 on disk */
    uint8_t *data;          /* always the decoded payload */
} Layer;

typedef struct {
    uint16_t flags; /* bit0 has_alpha, bit1 indexed */
    uint32_t w, h;
    uint8_t mode; /* 0 gray, 1 rgb, 2 indexed */
    uint32_t resolution;
    uint16_t nlayers;
    Layer *layers; /* capacity MAX_LAYERS */
} Doc;

static int doc_has_alpha(const Doc *d) { return d->flags & 1; }
static int doc_color_channels(const Doc *d) { return d->mode == 1 ? 3 : 1; }
static int doc_channels(const Doc *d) { return doc_color_channels(d) + (doc_has_alpha(d) ? 1 : 0); }

static Doc *doc_alloc(void) {
    Doc *d = (Doc *)xcalloc(sizeof(Doc));
    d->layers = (Layer *)xcalloc(sizeof(Layer) * MAX_LAYERS);
    return d;
}

static uint32_t layer_expected_len(const Doc *d, uint8_t kind) {
    switch (kind) {
    case KIND_RASTER: return d->w * d->h * (uint32_t)doc_channels(d);
    case KIND_MASK: return d->w * d->h;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Byte helpers                                                        */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

#include "fam_codec.c"

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    size_t cap = 1 << 16, len = 0, n;
    uint8_t *buf;
    if (!f) die(EXIT_IO, "E_IO", "cannot open input file");
    buf = (uint8_t *)xmalloc(cap);
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            uint8_t *nb;
            cap *= 2;
            nb = (uint8_t *)xmalloc(cap);
            memcpy(nb, buf, len);
            free(buf);
            buf = nb;
        }
    }
    fclose(f);
    *out_len = len;
    return buf;
}

/* ------------------------------------------------------------------ */
/* LDX reader (strict) and writer                                      */
/* ------------------------------------------------------------------ */

static void bad_file(const char *msg) { die(EXIT_BAD_FILE, "E_BAD_FILE", msg); }

static Doc *ldx_read(const char *path) {
    size_t len, off = 0;
    uint8_t *b = read_file(path, &len);
    Doc *d = doc_alloc();
    uint16_t version, i;
    uint8_t depth;
    int j;

    if (len < 32) bad_file("file shorter than header");
    if (memcmp(b, "LDX1", 4) != 0) bad_file("bad magic");
    version = rd16(b + 4);
    if (version != 1) bad_file("unsupported version");
    d->flags = rd16(b + 6);
    if (d->flags & 0xfffc) bad_file("reserved header flag bits set");
    d->w = rd32(b + 8);
    d->h = rd32(b + 12);
    d->mode = b[16];
    depth = b[17];
    d->nlayers = rd16(b + 18);
    d->resolution = rd32(b + 20);
    for (j = 24; j < 32; j++)
        if (b[j] != 0) bad_file("header reserved bytes not zero");
    if (d->w == 0 || d->h == 0 || d->w > MAX_DIM || d->h > MAX_DIM) bad_file("bad dimensions");
    if (d->mode > 2) bad_file("bad color_mode");
    if (d->mode == 2 || (d->flags & 2)) die(EXIT_UNSUPPORTED, "E_UNSUPPORTED", "indexed mode not supported");
    if (depth != 8) bad_file("bad bit_depth");
    if (d->nlayers > MAX_LAYERS) bad_file("too many layers");
    off = 32;

    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        size_t need;
        int pad, k;
        if (off + 1 > len) bad_file("truncated layer record");
        L->name_len = b[off];
        need = 1 + L->name_len;
        pad = (int)((4 - (need % 4)) % 4);
        if (off + need + pad + 12 > len) bad_file("truncated layer record");
        memcpy(L->name, b + off + 1, L->name_len);
        L->name[L->name_len] = 0;
        for (k = 0; k < L->name_len; k++)
            if ((unsigned char)L->name[k] < 0x20 || (unsigned char)L->name[k] > 0x7e)
                bad_file("layer name contains non-printable byte");
        for (k = 0; k < pad; k++)
            if (b[off + need + k] != 0) bad_file("name padding not zero");
        off += need + pad;
        L->kind = b[off];
        L->opacity = b[off + 1];
        L->blend = b[off + 2];
        L->flags = (uint8_t)(b[off + 3] & 0x0f);
        L->codec = (uint8_t)((b[off + 3] >> 4) & 0x0f);
        L->parent = (int16_t)rd16(b + off + 4);
        if (rd16(b + off + 6) != 0) bad_file("record reserved bytes not zero");
        L->stored_len = rd32(b + off + 8);
        off += 12;
        if (L->kind > 3) bad_file("bad layer kind");
        if (L->blend >= BLEND_COUNT) bad_file("bad blend mode");
        if (L->flags & 0xf8) bad_file("reserved layer flag bits set");
        if (L->codec >= CODEC_COUNT) bad_file("unknown record codec");
        if (L->parent < -1 || L->parent >= (int)d->nlayers) bad_file("bad parent_idx");
        L->data_len = layer_expected_len(d, L->kind);
        if (L->codec == CODEC_NONE && L->stored_len != L->data_len)
            bad_file("data_len does not match kind and canvas");
        if (off + L->stored_len > len) bad_file("truncated layer data");
        L->data = (uint8_t *)xmalloc(L->data_len ? L->data_len : 1);
        if (!codec_decode(L->codec, b + off, L->stored_len, L->data, L->data_len,
                          codec_stride(d, L->kind), codec_bpp(d, L->kind)))
            bad_file("record payload does not decode");
        off += L->stored_len;
        pad = (int)((4 - (off % 4)) % 4);
        if (off + pad > len) bad_file("truncated data padding");
        for (k = 0; k < pad; k++)
            if (b[off + k] != 0) bad_file("data padding not zero");
        off += pad;
    }
    if (off != len) bad_file("trailing bytes after last record");

    /* structural sanity: group nesting must be balanced */
    {
        int depth_ = 0;
        for (i = 0; i < d->nlayers; i++) {
            if (d->layers[i].kind == KIND_GROUP_OPEN) depth_++;
            else if (d->layers[i].kind == KIND_GROUP_CLOSE) {
                depth_--;
                if (depth_ < 0) bad_file("unbalanced group records");
            }
        }
        if (depth_ != 0) bad_file("unbalanced group records");
    }
    free(b);
    return d;
}

static void ldx_write(const Doc *d, const char *path) {
    FILE *f = fopen(path, "wb");
    uint8_t hdr[32];
    uint8_t zeros[4] = {0, 0, 0, 0};
    uint16_t i;
    if (!f) die(EXIT_IO, "E_IO", "cannot open output file");
    memset(hdr, 0, 32);
    memcpy(hdr, "LDX1", 4);
    wr16(hdr + 4, 1);
    wr16(hdr + 6, d->flags);
    wr32(hdr + 8, d->w);
    wr32(hdr + 12, d->h);
    hdr[16] = d->mode;
    hdr[17] = 8;
    wr16(hdr + 18, d->nlayers);
    wr32(hdr + 20, d->resolution);
    fwrite(hdr, 1, 32, f);
    for (i = 0; i < d->nlayers; i++) {
        const Layer *L = &d->layers[i];
        uint8_t fixed[12];
        size_t need = 1 + L->name_len;
        int pad = (int)((4 - (need % 4)) % 4);
        fputc(L->name_len, f);
        fwrite(L->name, 1, L->name_len, f);
        fwrite(zeros, 1, pad, f);
        fixed[0] = L->kind;
        fixed[1] = L->opacity;
        fixed[2] = L->blend;
        fixed[3] = (uint8_t)((L->flags & 0x0f) | ((L->codec & 0x0f) << 4));
        wr16(fixed + 4, (uint16_t)L->parent);
        wr16(fixed + 6, 0);
        {
            size_t enc_len = 0;
            uint8_t *enc = codec_encode(L->codec, L->data, L->data_len,
                                        codec_stride(d, L->kind), codec_bpp(d, L->kind), &enc_len);
            wr32(fixed + 8, (uint32_t)enc_len);
            fwrite(fixed, 1, 12, f);
            if (enc_len) fwrite(enc, 1, enc_len, f);
            pad = (int)((4 - (enc_len % 4)) % 4);
            fwrite(zeros, 1, pad, f);
            free(enc);
        }
    }
    if (fclose(f) != 0) die(EXIT_IO, "E_IO", "cannot write output file");
}

/* ------------------------------------------------------------------ */
/* JSON dump (doc-open)                                                */
/* ------------------------------------------------------------------ */

static void doc_print_json(const Doc *d, int dump) {
    uint16_t i;
    printf("{\"document\":{\"width\":%u,\"height\":%u,\"mode\":\"%s\",\"has_alpha\":%s,"
           "\"bit_depth\":8,\"resolution\":%u,\"layer_count\":%u},\"layers\":[",
           d->w, d->h, d->mode == 1 ? "rgb" : "gray", doc_has_alpha(d) ? "true" : "false",
           d->resolution, d->nlayers);
    for (i = 0; i < d->nlayers; i++) {
        const Layer *L = &d->layers[i];
        if (i) putchar(',');
        printf("{\"index\":%u,\"name\":\"", i);
        json_escape_out(stdout, L->name, L->name_len);
        printf("\",\"kind\":\"%s\",\"opacity\":%u,\"blend\":\"%s\",\"visible\":%s,\"locked\":%s,"
               "\"is_background\":%s,\"parent\":%d,\"data_len\":%u",
               KIND_NAMES[L->kind], L->opacity, BLEND_NAMES[L->blend],
               (L->flags & FLAG_VISIBLE) ? "true" : "false", (L->flags & FLAG_LOCKED) ? "true" : "false",
               (L->flags & FLAG_BACKGROUND) ? "true" : "false", L->parent, L->data_len);
        if (dump) {
            uint32_t k;
            static const char hex[] = "0123456789abcdef";
            fputs(",\"data\":\"", stdout);
            for (k = 0; k < L->data_len; k++) {
                putchar(hex[L->data[k] >> 4]);
                putchar(hex[L->data[k] & 15]);
            }
            putchar('"');
        }
        putchar('}');
    }
    puts("]}");
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *keys[32];
    const char *vals[32];
    int n;
    const char *pos[8];
    int npos;
} Args;

/* Flags that are switches rather than key/value pairs. */
static const char *BOOL_FLAGS[] = {"dump", "all", "strict"};
static int is_bool_flag(const char *k) {
    size_t i;
    for (i = 0; i < sizeof BOOL_FLAGS / sizeof BOOL_FLAGS[0]; i++)
        if (strcmp(k, BOOL_FLAGS[i]) == 0) return 1;
    return 0;
}

static void parse_args(int argc, char **argv, int start, Args *a, const char **allowed, int nallowed) {
    int i;
    memset(a, 0, sizeof(*a));
    for (i = start; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            const char *key = argv[i] + 2;
            int j, ok = 0;
            for (j = 0; j < nallowed; j++)
                if (strcmp(key, allowed[j]) == 0) ok = 1;
            if (!ok) {
                char msg[300];
                snprintf(msg, sizeof msg, "unknown flag --%s", key);
                die(EXIT_USAGE, "E_USAGE", msg);
            }
            for (j = 0; j < a->n; j++)
                if (strcmp(a->keys[j], key) == 0) die(EXIT_USAGE, "E_USAGE", "flag given twice");
            if (a->n >= 32) die(EXIT_USAGE, "E_USAGE", "too many flags");
            a->keys[a->n] = key;
            if (is_bool_flag(key)) { /* boolean flag, takes no value */
                a->vals[a->n] = "1";
            } else {
                if (i + 1 >= argc) die(EXIT_USAGE, "E_USAGE", "flag requires a value");
                a->vals[a->n] = argv[i + 1];
                i++;
            }
            a->n++;
        } else {
            if (a->npos >= 8) die(EXIT_USAGE, "E_USAGE", "too many positional arguments");
            a->pos[a->npos++] = argv[i];
        }
    }
}

static const char *arg_get(const Args *a, const char *key) {
    int i;
    for (i = 0; i < a->n; i++)
        if (strcmp(a->keys[i], key) == 0) return a->vals[i];
    return NULL;
}

static const char *arg_req(const Args *a, const char *key) {
    const char *v = arg_get(a, key);
    if (!v) {
        char msg[300];
        snprintf(msg, sizeof msg, "missing required flag --%s", key);
        die(EXIT_USAGE, "E_USAGE", msg);
    }
    return v;
}

static long parse_int(const char *s, const char *what, long lo, long hi) {
    char *end;
    long v;
    if (!s || !*s) die(EXIT_USAGE, "E_BAD_ARGS", "empty integer");
    v = strtol(s, &end, 10);
    if (*end != 0) {
        char msg[300];
        snprintf(msg, sizeof msg, "--%s must be an integer", what);
        die(EXIT_USAGE, "E_BAD_ARGS", msg);
    }
    if (v < lo || v > hi) {
        char msg[300];
        snprintf(msg, sizeof msg, "--%s out of range (%ld..%ld)", what, lo, hi);
        die(EXIT_USAGE, "E_BAD_ARGS", msg);
    }
    return v;
}

static long arg_int(const Args *a, const char *key, long dflt, long lo, long hi) {
    const char *v = arg_get(a, key);
    if (!v) return dflt;
    return parse_int(v, key, lo, hi);
}

static void positional(const Args *a, int n) {
    if (a->npos != n) {
        char msg[128];
        snprintf(msg, sizeof msg, "expected %d positional argument(s), got %d", n, a->npos);
        die(EXIT_USAGE, "E_USAGE", msg);
    }
}

static int parse_blend(const char *s) {
    int i;
    for (i = 0; i < BLEND_COUNT; i++)
        if (strcmp(s, BLEND_NAMES[i]) == 0) return i;
    die(EXIT_USAGE, "E_BAD_ARGS", "unknown blend mode");
    return 0;
}

/* parse "a,b,c,d" into up to 4 bytes, requires exactly n values */
static void parse_fill(const char *s, int n, uint8_t *out) {
    int i;
    const char *p = s;
    for (i = 0; i < n; i++) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 255) die(EXIT_USAGE, "E_BAD_ARGS", "--fill components must be 0..255");
        out[i] = (uint8_t)v;
        p = end;
        if (i < n - 1) {
            if (*p != ',') die(EXIT_USAGE, "E_BAD_ARGS", "--fill needs one component per channel");
            p++;
        }
    }
    if (*p != 0) die(EXIT_USAGE, "E_BAD_ARGS", "--fill needs one component per channel");
}

static void set_name(Layer *L, const char *name) {
    size_t n = strlen(name), i;
    if (n < 1 || n > 255) die(EXIT_USAGE, "E_BAD_ARGS", "--name must be 1..255 bytes");
    for (i = 0; i < n; i++)
        if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7e)
            die(EXIT_USAGE, "E_BAD_ARGS", "--name must be printable ASCII");
    L->name_len = (uint8_t)n;
    memcpy(L->name, name, n);
    L->name[n] = 0;
}

/* ------------------------------------------------------------------ */
/* PNM (P5 / P6) reader                                                */
/* ------------------------------------------------------------------ */

static size_t pnm_skip(const uint8_t *b, size_t len, size_t off) {
    for (;;) {
        while (off < len && (b[off] == ' ' || b[off] == '\n' || b[off] == '\r' || b[off] == '\t')) off++;
        if (off < len && b[off] == '#') {
            while (off < len && b[off] != '\n') off++;
        } else
            return off;
    }
}

static size_t pnm_num(const uint8_t *b, size_t len, size_t off, long *out) {
    long v = 0;
    int any = 0;
    off = pnm_skip(b, len, off);
    while (off < len && b[off] >= '0' && b[off] <= '9') {
        v = v * 10 + (b[off] - '0');
        if (v > 1000000) bad_file("PNM number too large");
        off++;
        any = 1;
    }
    if (!any) bad_file("malformed PNM header");
    *out = v;
    return off;
}

/* returns pixel bytes (w*h*ch), sets *ch to 1 (P5) or 3 (P6) */
static uint8_t *pnm_read(const char *path, uint32_t want_w, uint32_t want_h, int *ch) {
    size_t len, off;
    uint8_t *b = read_file(path, &len), *px;
    long w, h, maxv;
    size_t need;
    if (len < 2 || b[0] != 'P' || (b[1] != '5' && b[1] != '6')) bad_file("PNM must be binary P5 or P6");
    *ch = b[1] == '6' ? 3 : 1;
    off = pnm_num(b, len, 2, &w);
    off = pnm_num(b, len, off, &h);
    off = pnm_num(b, len, off, &maxv);
    if (maxv != 255) bad_file("PNM maxval must be 255");
    if (off >= len) bad_file("truncated PNM");
    off++; /* single whitespace after maxval */
    if ((uint32_t)w != want_w || (uint32_t)h != want_h)
        die(EXIT_USAGE, "E_BAD_ARGS", "PNM dimensions must match the canvas");
    need = (size_t)w * (size_t)h * (size_t)(*ch);
    if (off + need > len) bad_file("truncated PNM data");
    px = (uint8_t *)xmalloc(need);
    memcpy(px, b + off, need);
    free(b);
    return px;
}

/* ------------------------------------------------------------------ */
/* Compositing                                                         */
/* ------------------------------------------------------------------ */

static int blend_channel(int mode, int cb, int cs) {
    switch (mode) {
    case BLEND_NORMAL: return cs;
    case BLEND_MULTIPLY: return (cb * cs + 127) / 255;
    case BLEND_SCREEN: return cb + cs - (cb * cs + 127) / 255;
    case BLEND_DARKEN: return cb < cs ? cb : cs;
    case BLEND_LIGHTEN: return cb > cs ? cb : cs;
    case BLEND_DIFFERENCE: return cb > cs ? cb - cs : cs - cb;
    }
    return cs;
}

/* round-half-up division for non-negative operands */
static int64_t div_round(int64_t num, int64_t den) { return (2 * num + den) / (2 * den); }

/*
 * Composite one source pixel over the destination pixel, in place.
 * cc = colour channels, dst/src have cc colour bytes followed by alpha.
 * eff = effective layer opacity 0..255 (already compounded through groups).
 * mask = per-pixel mask value 0..255 (255 when no mask).
 */
static void composite_pixel(uint8_t *dst, const uint8_t *src, int cc, int eff, int mask, int mode) {
    int as = src[cc];
    int ad = dst[cc];
    int ae, oa, k;
    /* E06: mask first, then layer opacity */
    ae = (as * mask + 127) / 255;
    ae = (ae * eff + 127) / 255;
    if (ae == 0) return; /* E05: destination preserved exactly */
    oa = ae + ad - (ae * ad + 127) / 255;
    for (k = 0; k < cc; k++) {
        int cs = src[k], cb = dst[k];
        int b = ((255 - ad) * cs + ad * blend_channel(mode, cb, cs) + 127) / 255;
        int64_t num = (int64_t)b * ae * 255 + (int64_t)cb * ad * (255 - ae);
        int64_t den = (int64_t)oa * 255;
        dst[k] = (uint8_t)div_round(num, den); /* E01: round half up */
    }
    dst[cc] = (uint8_t)oa;
}

/* Render the whole document into a fresh canvas (cc+1 channels, alpha always present internally). */
static uint8_t *doc_render(const Doc *d) {
    int cc = doc_color_channels(d);
    int ch = doc_channels(d);
    int has_a = doc_has_alpha(d);
    size_t npx = (size_t)d->w * d->h, p;
    uint8_t *canvas = (uint8_t *)xcalloc(npx * (cc + 1));
    /* group stack: effective opacity and visibility */
    int st_eff[MAX_LAYERS], st_vis[MAX_LAYERS], sp = 0;
    uint16_t i;
    st_eff[0] = 255;
    st_vis[0] = 1;
    for (i = 0; i < d->nlayers; i++) {
        const Layer *L = &d->layers[i];
        int vis = st_vis[sp] && (L->flags & FLAG_VISIBLE);
        int eff = (st_eff[sp] * L->opacity + 127) / 255; /* E11 */
        if (L->kind == KIND_GROUP_OPEN) {
            sp++;
            st_eff[sp] = eff;
            st_vis[sp] = vis;
            continue;
        }
        if (L->kind == KIND_GROUP_CLOSE) {
            if (sp > 0) sp--;
            continue;
        }
        if (L->kind != KIND_RASTER || !vis) continue;
        {
            const uint8_t *mask = NULL;
            uint8_t src[4];
            if (i + 1 < d->nlayers && d->layers[i + 1].kind == KIND_MASK) mask = d->layers[i + 1].data;
            for (p = 0; p < npx; p++) {
                const uint8_t *sp_ = L->data + p * ch;
                int k;
                for (k = 0; k < cc; k++) src[k] = sp_[k];
                src[cc] = has_a ? sp_[cc] : 255;
                composite_pixel(canvas + p * (cc + 1), src, cc, eff, mask ? mask[p] : 255, L->blend);
            }
        }
    }
    return canvas;
}

/* ------------------------------------------------------------------ */
/* Layer list manipulation                                             */
/* ------------------------------------------------------------------ */

static void layer_free(Layer *L) {
    free(L->data);
    L->data = NULL;
}

/* insert n blank records at position pos, shifting parent indices */
static void layers_insert(Doc *d, int pos, int n) {
    int i;
    if (d->nlayers + n > MAX_LAYERS) die(EXIT_USAGE, "E_BAD_ARGS", "too many layers");
    for (i = 0; i < d->nlayers; i++)
        if (d->layers[i].parent >= pos) d->layers[i].parent = (int16_t)(d->layers[i].parent + n);
    memmove(&d->layers[pos + n], &d->layers[pos], sizeof(Layer) * (d->nlayers - pos));
    memset(&d->layers[pos], 0, sizeof(Layer) * n);
    d->nlayers = (uint16_t)(d->nlayers + n);
}

/* index of the group_close matching group_open at idx */
static int group_close_of(const Doc *d, int idx) {
    int depth = 0, i;
    for (i = idx; i < d->nlayers; i++) {
        if (d->layers[i].kind == KIND_GROUP_OPEN) depth++;
        else if (d->layers[i].kind == KIND_GROUP_CLOSE) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int cmd_doc_new(int argc, char **argv) {
    static const char *allowed[] = {"w", "h", "mode", "dpi", "fill", "alpha"};
    Args a;
    Doc *d = doc_alloc();
    Layer *L;
    const char *mode;
    uint8_t fill[4] = {255, 255, 255, 255};
    int ch, k;
    size_t p, npx;
    parse_args(argc, argv, 2, &a, allowed, 6);
    positional(&a, 1);
    d->w = (uint32_t)parse_int(arg_req(&a, "w"), "w", 1, MAX_DIM);
    d->h = (uint32_t)parse_int(arg_req(&a, "h"), "h", 1, MAX_DIM);
    mode = arg_req(&a, "mode");
    if (strcmp(mode, "gray") == 0) d->mode = 0;
    else if (strcmp(mode, "rgb") == 0) d->mode = 1;
    else if (strcmp(mode, "indexed") == 0) die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "indexed mode not supported");
    else die(EXIT_USAGE, "E_BAD_ARGS", "--mode must be gray or rgb");
    d->flags = arg_int(&a, "alpha", 1, 0, 1) ? 1 : 0;
    d->resolution = (uint32_t)parse_int(arg_get(&a, "dpi") ? arg_get(&a, "dpi") : "72", "dpi", 1, 65535) << 16;
    ch = doc_channels(d);
    if (arg_get(&a, "fill")) parse_fill(arg_get(&a, "fill"), ch, fill);
    d->nlayers = 1;
    L = &d->layers[0];
    set_name(L, "Background");
    L->kind = KIND_RASTER;
    L->opacity = 255;
    L->blend = BLEND_NORMAL;
    L->flags = FLAG_VISIBLE | FLAG_BACKGROUND;
    L->parent = -1;
    L->data_len = layer_expected_len(d, KIND_RASTER);
    L->data = (uint8_t *)xmalloc(L->data_len);
    npx = (size_t)d->w * d->h;
    for (p = 0; p < npx; p++)
        for (k = 0; k < ch; k++) L->data[p * ch + k] = fill[k];
    ldx_write(d, a.pos[0]);
    return EXIT_OK;
}

static int cmd_doc_open(int argc, char **argv) {
    static const char *allowed[] = {"dump"};
    Args a;
    Doc *d;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 1);
    d = ldx_read(a.pos[0]);
    doc_print_json(d, (int)arg_int(&a, "dump", 0, 0, 1));
    return EXIT_OK;
}

static int cmd_doc_save(int argc, char **argv) {
    static const char *allowed[] = {"compress"};
    Args a;
    Doc *d;
    const char *comp;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    /* Without --compress every record keeps the codec it arrived with, so a plain
     * doc-save of a well-formed file is still byte-identical to its input. */
    comp = arg_get(&a, "compress");
    if (comp) codec_apply(d, codec_from_name(comp), -1);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

static int cmd_layer_add(int argc, char **argv) {
    static const char *allowed[] = {"name", "kind", "parent", "opacity", "blend", "visible",
                                    "locked", "fill", "from", "alpha-from"};
    Args a;
    Doc *d;
    const char *kind, *name;
    int parent, pos, is_group = 0, opacity, blend, visible, locked;
    Layer *L;
    parse_args(argc, argv, 2, &a, allowed, 10);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    name = arg_req(&a, "name");
    kind = arg_get(&a, "kind") ? arg_get(&a, "kind") : "raster";
    if (strcmp(kind, "raster") == 0) is_group = 0;
    else if (strcmp(kind, "group") == 0) is_group = 1;
    else if (strcmp(kind, "mask") == 0) die(EXIT_UNSUPPORTED, "E_UNSUPPORTED", "use layer-mask-apply to add masks");
    else die(EXIT_USAGE, "E_BAD_ARGS", "--kind must be raster or group");
    parent = (int)arg_int(&a, "parent", -1, -1, MAX_LAYERS);
    opacity = (int)arg_int(&a, "opacity", 255, 0, 255);
    blend = arg_get(&a, "blend") ? parse_blend(arg_get(&a, "blend")) : BLEND_NORMAL;
    visible = (int)arg_int(&a, "visible", 1, 0, 1);
    locked = (int)arg_int(&a, "locked", 0, 0, 1);
    if (parent >= 0) {
        if (parent >= d->nlayers || d->layers[parent].kind != KIND_GROUP_OPEN)
            die(EXIT_USAGE, "E_BAD_ARGS", "--parent must be the index of a group_open record");
        pos = group_close_of(d, parent);
    } else
        pos = d->nlayers;

    layers_insert(d, pos, is_group ? 2 : 1);
    L = &d->layers[pos];
    set_name(L, name);
    L->kind = is_group ? KIND_GROUP_OPEN : KIND_RASTER;
    L->opacity = (uint8_t)opacity;
    L->blend = (uint8_t)blend;
    L->flags = (uint8_t)((visible ? FLAG_VISIBLE : 0) | (locked ? FLAG_LOCKED : 0));
    L->parent = (int16_t)parent;
    if (is_group) {
        Layer *C = &d->layers[pos + 1];
        C->name_len = 0;
        C->name[0] = 0;
        C->kind = KIND_GROUP_CLOSE;
        C->opacity = 255;
        C->blend = BLEND_NORMAL;
        C->flags = 0;
        C->parent = (int16_t)pos;
        C->data_len = 0;
        if (arg_get(&a, "fill") || arg_get(&a, "from") || arg_get(&a, "alpha-from"))
            die(EXIT_USAGE, "E_BAD_ARGS", "groups carry no pixel data");
    } else {
        int ch = doc_channels(d), cc = doc_color_channels(d), has_a = doc_has_alpha(d), k;
        size_t npx = (size_t)d->w * d->h, p;
        uint8_t fill[4] = {0, 0, 0, 0};
        L->data_len = layer_expected_len(d, KIND_RASTER);
        L->data = (uint8_t *)xcalloc(L->data_len);
        if (arg_get(&a, "fill") && arg_get(&a, "from"))
            die(EXIT_USAGE, "E_BAD_ARGS", "--fill and --from are mutually exclusive");
        if (arg_get(&a, "fill")) {
            parse_fill(arg_get(&a, "fill"), ch, fill);
            for (p = 0; p < npx; p++)
                for (k = 0; k < ch; k++) L->data[p * ch + k] = fill[k];
        } else if (arg_get(&a, "from")) {
            int pch;
            uint8_t *px = pnm_read(arg_get(&a, "from"), d->w, d->h, &pch);
            if (pch != cc) die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "PNM channel count does not match document mode");
            for (p = 0; p < npx; p++) {
                for (k = 0; k < cc; k++) L->data[p * ch + k] = px[p * cc + k];
                if (has_a) L->data[p * ch + cc] = 255;
            }
            free(px);
        }
        if (arg_get(&a, "alpha-from")) {
            int pch;
            uint8_t *px;
            if (!has_a) die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "document has no alpha channel");
            px = pnm_read(arg_get(&a, "alpha-from"), d->w, d->h, &pch);
            if (pch != 1) die(EXIT_USAGE, "E_BAD_ARGS", "--alpha-from must be a P5 file");
            for (p = 0; p < npx; p++) L->data[p * ch + cc] = px[p];
            free(px);
        }
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

static int cmd_doc_flatten(int argc, char **argv) {
    Args a;
    Doc *d, *o;
    uint8_t *canvas;
    Layer *L;
    int cc, ch, has_a, k;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, NULL, 0);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    canvas = doc_render(d);
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    has_a = doc_has_alpha(d);
    npx = (size_t)d->w * d->h;
    o = doc_alloc();
    o->flags = d->flags;
    o->w = d->w;
    o->h = d->h;
    o->mode = d->mode;
    o->resolution = d->resolution;
    o->nlayers = 1;
    L = &o->layers[0];
    set_name(L, "Background");
    L->kind = KIND_RASTER;
    L->opacity = 255;
    L->blend = BLEND_NORMAL;
    L->flags = FLAG_VISIBLE | FLAG_BACKGROUND;
    L->parent = -1;
    L->data_len = layer_expected_len(o, KIND_RASTER);
    L->data = (uint8_t *)xmalloc(L->data_len);
    for (p = 0; p < npx; p++) {
        for (k = 0; k < cc; k++) L->data[p * ch + k] = canvas[p * (cc + 1) + k];
        if (has_a) L->data[p * ch + cc] = canvas[p * (cc + 1) + cc];
    }
    ldx_write(o, a.pos[1]);
    return EXIT_OK;
}

static int cmd_px_crop(int argc, char **argv) {
    static const char *allowed[] = {"x", "y", "w", "h"};
    Args a;
    Doc *d;
    long x, y, w, h, x0, y0, x1, y1;
    uint32_t nw, nh;
    uint16_t i;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    x = parse_int(arg_req(&a, "x"), "x", -MAX_DIM, MAX_DIM);
    y = parse_int(arg_req(&a, "y"), "y", -MAX_DIM, MAX_DIM);
    w = parse_int(arg_req(&a, "w"), "w", 1, MAX_DIM);
    h = parse_int(arg_req(&a, "h"), "h", 1, MAX_DIM);
    d = ldx_read(a.pos[0]);
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > (long)d->w ? (long)d->w : x + w;
    y1 = y + h > (long)d->h ? (long)d->h : y + h;
    if (x1 <= x0 || y1 <= y0) die(EXIT_USAGE, "E_BAD_RECT", "crop rectangle does not intersect the canvas");
    if (x0 != x || y0 != y || x1 != x + w || y1 != y + h) warn("W_CROP_CLAMPED", "crop rectangle clamped to canvas"); /* E07 */
    nw = (uint32_t)(x1 - x0);
    nh = (uint32_t)(y1 - y0);
    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        int ch = L->kind == KIND_RASTER ? doc_channels(d) : (L->kind == KIND_MASK ? 1 : 0);
        uint8_t *nd;
        uint32_t r;
        if (!ch) continue;
        nd = (uint8_t *)xmalloc((size_t)nw * nh * ch);
        for (r = 0; r < nh; r++)
            memcpy(nd + (size_t)r * nw * ch, L->data + ((size_t)(y0 + r) * d->w + x0) * ch, (size_t)nw * ch);
        layer_free(L);
        L->data = nd;
        L->data_len = nw * nh * (uint32_t)ch;
    }
    d->w = nw;
    d->h = nh;
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

static int cmd_px_channel(int argc, char **argv) {
    static const char *allowed[] = {"op", "arg", "index"};
    Args a;
    Doc *d;
    const char *op;
    int idx, cc, ch, has_a, k;
    long arg;
    Layer *L;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    op = arg_req(&a, "op");
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    if (idx >= d->nlayers || d->layers[idx].kind != KIND_RASTER)
        die(EXIT_USAGE, "E_BAD_ARGS", "--index must be the index of a raster layer");
    L = &d->layers[idx];
    cc = doc_color_channels(d);
    ch = doc_channels(d);
    has_a = doc_has_alpha(d);
    npx = (size_t)d->w * d->h;
    if (strcmp(op, "invert") == 0) {
        for (p = 0; p < npx; p++)
            for (k = 0; k < cc; k++) L->data[p * ch + k] = (uint8_t)(255 - L->data[p * ch + k]);
    } else if (strcmp(op, "threshold") == 0) {
        arg = parse_int(arg_req(&a, "arg"), "arg", 0, 255);
        for (p = 0; p < npx; p++)
            for (k = 0; k < cc; k++) L->data[p * ch + k] = L->data[p * ch + k] >= arg ? 255 : 0;
    } else if (strcmp(op, "offset") == 0) {
        arg = parse_int(arg_req(&a, "arg"), "arg", -255, 255);
        for (p = 0; p < npx; p++)
            for (k = 0; k < cc; k++) {
                long v = (long)L->data[p * ch + k] + arg; /* E03: clamp, never wrap */
                L->data[p * ch + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
    } else if (strcmp(op, "swap") == 0) {
        if (d->mode != 1) die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "swap requires an rgb document"); /* E10 */
        for (p = 0; p < npx; p++) {
            uint8_t t = L->data[p * ch];
            L->data[p * ch] = L->data[p * ch + 2];
            L->data[p * ch + 2] = t;
        }
    } else if (strcmp(op, "extract_alpha") == 0) {
        if (!has_a) die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "document has no alpha channel");
        for (p = 0; p < npx; p++) {
            uint8_t av = L->data[p * ch + cc];
            for (k = 0; k < cc; k++) L->data[p * ch + k] = av;
            L->data[p * ch + cc] = 255;
        }
    } else
        die(EXIT_USAGE, "E_BAD_ARGS", "--op must be invert, threshold, offset, swap or extract_alpha");
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int (*fn)(int argc, char **argv);
    const char *family;
} Command;

/* Command families.  Each fragment defines only `static int cmd_*` functions and uses the
   helpers above; they are included into this single translation unit so the helpers stay
   internal and the binary keeps no exported symbols. */
#include "fam_layer.c"
#include "fam_tonal.c"
#include "fam_geom.c"
#include "fam_filter.c"
#include "fam_docsel.c"
#include "fam_gen.c"
#include "fam_genscore.c"
#include "fam_codec_cmds.c"
#include "fam_help.c"

static const Command COMMANDS[] = {
    {"doc-new", cmd_doc_new, "doc"},
    {"doc-open", cmd_doc_open, "doc"},
    {"doc-save", cmd_doc_save, "doc"},
    {"doc-flatten", cmd_doc_flatten, "doc"},
    {"layer-add", cmd_layer_add, "layer"},
    {"px-crop", cmd_px_crop, "geom"},
    {"px-channel", cmd_px_channel, "tonal"},
    FAM_LAYER_COMMANDS
    FAM_TONAL_COMMANDS
    FAM_GEOM_COMMANDS
    FAM_FILTER_COMMANDS
    FAM_DOCSEL_COMMANDS
    FAM_GEN_COMMANDS
    FAM_GENSCORE_COMMANDS
    FAM_CODEC_COMMANDS
};

static void usage(void) {
    size_t i;
    fputs("usage: ldx <command> [flags] <args>\ncommands:\n", stderr);
    for (i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++)
        fprintf(stderr, "  %s\n", COMMANDS[i].name);
}

int main(int argc, char **argv) {
    size_t i;
    if (argc < 2) {
        usage();
        die(EXIT_USAGE, "E_USAGE", "missing command");
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        if (argc >= 3 && help_command(argv[2]) == EXIT_OK) return EXIT_OK;
        usage();
        return EXIT_OK;
    }
    for (i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++)
        if (strcmp(argv[1], COMMANDS[i].name) == 0) return COMMANDS[i].fn(argc, argv);
    usage();
    die(EXIT_USAGE, "E_USAGE", "unknown command");
    return EXIT_USAGE;
}
