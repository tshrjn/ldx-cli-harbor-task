/* ------------------------------------------------------------------ */
/* Family D -- geometry, coordinate-remap engine                       */
/*                                                                     */
/* Every command in this family is a thin wrapper over one engine.     */
/* The engine walks the *destination* raster in scan order and, for    */
/* each destination pixel, asks remap_src() for the source pixel that  */
/* feeds it (a pull remap).  Pixels with no source are written from a  */
/* per-record fill value.  The same map is applied to every raster and */
/* every mask record of the document at once, so all records stay the  */
/* same size as the canvas, and the document width/height are updated  */
/* when the transform changes them.                                    */
/*                                                                     */
/* All arithmetic is integer.  px-crop is NOT part of this file; it    */
/* lives in ldx.c as cmd_px_crop.                                      */
/* ------------------------------------------------------------------ */

#define GEOM_FLIP_H 0
#define GEOM_FLIP_V 1
#define GEOM_ROT90 2
#define GEOM_ROT180 3
#define GEOM_ROT270 4
#define GEOM_TRANSPOSE 5
#define GEOM_SCALE 6
#define GEOM_OFFSET 7 /* px-pad and px-translate */
#define GEOM_TILE 8

typedef struct {
    int op;
    long a, b;       /* op-specific pair: SCALE num/den, OFFSET x/y, TILE cols/rows */
    int wrap;        /* OFFSET only: 1 = wrap around the canvas, 0 = fill */
    uint32_t sw, sh; /* source canvas */
    uint32_t dw, dh; /* destination canvas */
} Remap;

/*
 * Map a destination pixel to its source pixel.  Returns 1 and sets sx, sy
 * when the destination pixel is fed by a source pixel, 0 when it must be
 * filled instead.
 *
 * Orientation conventions (all "clockwise" as seen on screen, y down):
 *   rot90   dst(x,y) = src(y, sh-1-x)          dims (w,h) -> (h,w)
 *   rot180  dst(x,y) = src(sw-1-x, sh-1-y)     dims unchanged
 *   rot270  dst(x,y) = src(sw-1-y, x)          dims (w,h) -> (h,w)
 * rot270 is written as the algebraic composition rot90 o rot90 o rot90, so
 * it is bit-identical to running px-rot90 three times (E09).  Substituting
 * the rot90 rule into itself three times gives exactly src(sw-1-y, x).
 */
static int remap_src(const Remap *R, uint32_t x, uint32_t y, uint32_t *sx, uint32_t *sy) {
    long tx, ty;
    switch (R->op) {
    case GEOM_FLIP_H:
        *sx = R->sw - 1 - x;
        *sy = y;
        return 1;
    case GEOM_FLIP_V:
        *sx = x;
        *sy = R->sh - 1 - y;
        return 1;
    case GEOM_ROT90:
        *sx = y;
        *sy = R->sh - 1 - x;
        return 1;
    case GEOM_ROT180:
        *sx = R->sw - 1 - x;
        *sy = R->sh - 1 - y;
        return 1;
    case GEOM_ROT270:
        *sx = R->sw - 1 - y;
        *sy = x;
        return 1;
    case GEOM_TRANSPOSE:
        *sx = y;
        *sy = x;
        return 1;
    case GEOM_SCALE:
        /*
         * Integer nearest neighbour with pixel-centre sampling.  The centre
         * of destination pixel x sits at source coordinate
         *     (x + 1/2) * den / num
         * which, doubled and floored, is exactly
         *     (x * den + den / 2) / num                (a = num, b = den)
         * Both operands are non-negative, the division truncates, and the
         * result is clamped to the last source pixel so that a destination
         * size rounded up can never read past the canvas.
         */
        tx = ((long)x * R->b + R->b / 2) / R->a;
        ty = ((long)y * R->b + R->b / 2) / R->a;
        if (tx > (long)R->sw - 1) tx = (long)R->sw - 1;
        if (ty > (long)R->sh - 1) ty = (long)R->sh - 1;
        *sx = (uint32_t)tx;
        *sy = (uint32_t)ty;
        return 1;
    case GEOM_OFFSET:
        tx = (long)x + R->a;
        ty = (long)y + R->b;
        if (R->wrap) {
            tx %= (long)R->sw;
            ty %= (long)R->sh;
            if (tx < 0) tx += (long)R->sw;
            if (ty < 0) ty += (long)R->sh;
        } else if (tx < 0 || ty < 0 || tx >= (long)R->sw || ty >= (long)R->sh)
            return 0;
        *sx = (uint32_t)tx;
        *sy = (uint32_t)ty;
        return 1;
    case GEOM_TILE:
        *sx = x % R->sw;
        *sy = y % R->sh;
        return 1;
    }
    return 0;
}

/* dw/dh for the six orientation ops; the parametric ops set their own. */
static void remap_orient_dims(int op, uint32_t sw, uint32_t sh, uint32_t *dw, uint32_t *dh) {
    if (op == GEOM_ROT90 || op == GEOM_ROT270 || op == GEOM_TRANSPOSE) {
        *dw = sh;
        *dh = sw;
    } else {
        *dw = sw;
        *dh = sh;
    }
}

static void geom_check_dims(uint32_t w, uint32_t h) {
    if (w < 1 || h < 1) die(EXIT_USAGE, "E_BAD_ARGS", "resulting canvas would be empty");
    if (w > MAX_DIM || h > MAX_DIM)
        die(EXIT_USAGE, "E_BAD_ARGS", "resulting canvas exceeds the maximum dimension");
}

/*
 * Apply a remap to every pixel-bearing record of the document and adopt the
 * new canvas size.  rfill holds doc_channels(d) bytes used for destination
 * pixels with no source in raster records; mfill is the same for masks.
 * Group records carry no data and are left alone.
 */
static void remap_doc(Doc *d, const Remap *R, const uint8_t *rfill, uint8_t mfill) {
    uint16_t i;
    for (i = 0; i < d->nlayers; i++) {
        Layer *L = &d->layers[i];
        int ch = L->kind == KIND_RASTER ? doc_channels(d) : (L->kind == KIND_MASK ? 1 : 0);
        const uint8_t *fill = L->kind == KIND_RASTER ? rfill : &mfill;
        uint8_t *nd;
        uint32_t x, y, sx, sy;
        int k;
        if (!ch) continue;
        nd = (uint8_t *)xmalloc((size_t)R->dw * R->dh * (size_t)ch);
        for (y = 0; y < R->dh; y++) {
            for (x = 0; x < R->dw; x++) {
                uint8_t *dp = nd + ((size_t)y * R->dw + x) * (size_t)ch;
                if (remap_src(R, x, y, &sx, &sy)) {
                    const uint8_t *sp = L->data + ((size_t)sy * R->sw + sx) * (size_t)ch;
                    for (k = 0; k < ch; k++) dp[k] = sp[k];
                } else {
                    for (k = 0; k < ch; k++) dp[k] = fill[k];
                }
            }
        }
        layer_free(L);
        L->data = nd;
        L->data_len = R->dw * R->dh * (uint32_t)ch;
    }
    d->w = R->dw;
    d->h = R->dh;
}

/* Shared body of the six orientation commands: no flags, two positionals. */
static int geom_orient_cmd(int argc, char **argv, int op) {
    Args a;
    Doc *d;
    Remap R;
    uint8_t fill[4] = {0, 0, 0, 0};
    parse_args(argc, argv, 2, &a, NULL, 0);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    memset(&R, 0, sizeof R);
    R.op = op;
    R.sw = d->w;
    R.sh = d->h;
    remap_orient_dims(op, d->w, d->h, &R.dw, &R.dh);
    remap_doc(d, &R, fill, 0);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

static int cmd_px_flip_h(int argc, char **argv) { return geom_orient_cmd(argc, argv, GEOM_FLIP_H); }
static int cmd_px_flip_v(int argc, char **argv) { return geom_orient_cmd(argc, argv, GEOM_FLIP_V); }
static int cmd_px_rot90(int argc, char **argv) { return geom_orient_cmd(argc, argv, GEOM_ROT90); }
static int cmd_px_rot180(int argc, char **argv) { return geom_orient_cmd(argc, argv, GEOM_ROT180); }
static int cmd_px_rot270(int argc, char **argv) { return geom_orient_cmd(argc, argv, GEOM_ROT270); }
static int cmd_px_transpose(int argc, char **argv) {
    return geom_orient_cmd(argc, argv, GEOM_TRANSPOSE);
}

/*
 * px-scale --num --den: nearest neighbour by the rational factor num/den.
 *
 * Destination size = round_half_up(src * num / den) via div_round, never
 * below 1 and never above MAX_DIM.  Sampling is the pixel-centre rule
 * documented in remap_src(): source index = (dst * den + den / 2) / num,
 * clamped to the last source pixel.  No filtering, no fill: every
 * destination pixel has a source.
 */
static int cmd_px_scale(int argc, char **argv) {
    static const char *allowed[] = {"num", "den"};
    Args a;
    Doc *d;
    Remap R;
    long num, den, nw, nh;
    uint8_t fill[4] = {0, 0, 0, 0};
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    num = parse_int(arg_req(&a, "num"), "num", 1, MAX_DIM);
    den = parse_int(arg_req(&a, "den"), "den", 1, MAX_DIM);
    d = ldx_read(a.pos[0]);
    nw = (long)div_round((int64_t)d->w * num, den);
    nh = (long)div_round((int64_t)d->h * num, den);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    geom_check_dims((uint32_t)nw, (uint32_t)nh);
    memset(&R, 0, sizeof R);
    R.op = GEOM_SCALE;
    R.a = num;
    R.b = den;
    R.sw = d->w;
    R.sh = d->h;
    R.dw = (uint32_t)nw;
    R.dh = (uint32_t)nh;
    remap_doc(d, &R, fill, 0);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/*
 * px-pad --left --right --top --bottom --fill
 *
 * The pads describe a rectangle in source coordinates:
 *     x0 = -left, y0 = -top, x1 = w + right, y1 = h + bottom
 * so positive pads grow the canvas and negative pads eat into it.  Source
 * pixels inside the canvas are copied; destination pixels outside it take
 * --fill (raster) or 0 (mask), the transparent/zero default.
 *
 * E07: a *negative* pad that eats past the far edge is clamped to the canvas
 * for sampling, warns W_CROP_CLAMPED and still exits 0.  Growing the canvas is
 * the ordinary use and is silent.  A rectangle whose
 * intersection with the canvas is empty (or that is itself degenerate) is
 * E_BAD_RECT, exit 2.
 */
static int cmd_px_pad(int argc, char **argv) {
    static const char *allowed[] = {"left", "right", "top", "bottom", "fill"};
    Args a;
    Doc *d;
    Remap R;
    long left, right, top, bottom;
    long x0, y0, x1, y1, cx0, cy0, cx1, cy1;
    uint8_t fill[4] = {0, 0, 0, 0};
    parse_args(argc, argv, 2, &a, allowed, 5);
    positional(&a, 2);
    left = arg_int(&a, "left", 0, -MAX_DIM, MAX_DIM);
    right = arg_int(&a, "right", 0, -MAX_DIM, MAX_DIM);
    top = arg_int(&a, "top", 0, -MAX_DIM, MAX_DIM);
    bottom = arg_int(&a, "bottom", 0, -MAX_DIM, MAX_DIM);
    d = ldx_read(a.pos[0]);
    if (arg_get(&a, "fill")) parse_fill(arg_get(&a, "fill"), doc_channels(d), fill);
    x0 = -left;
    y0 = -top;
    x1 = (long)d->w + right;
    y1 = (long)d->h + bottom;
    if (x1 <= x0 || y1 <= y0) die(EXIT_USAGE, "E_BAD_RECT", "padded rectangle is empty");
    cx0 = x0 < 0 ? 0 : x0;
    cy0 = y0 < 0 ? 0 : y0;
    cx1 = x1 > (long)d->w ? (long)d->w : x1;
    cy1 = y1 > (long)d->h ? (long)d->h : y1;
    if (cx1 <= cx0 || cy1 <= cy0)
        die(EXIT_USAGE, "E_BAD_RECT", "padded rectangle does not intersect the canvas");
    /* E07 applies to *shrinking* only.  Growing the canvas is the ordinary use of px-pad and
       nothing is lost, so it is silent; a negative pad that eats past the far edge is the
       clamped case and warns, matching cmd_px_crop. */
    if (left < 0 || right < 0 || top < 0 || bottom < 0)
        if (x0 != cx0 || y0 != cy0 || x1 != cx1 || y1 != cy1)
            warn("W_CROP_CLAMPED", "padded rectangle extends beyond the canvas; sampling clamped to canvas");
    geom_check_dims((uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
    memset(&R, 0, sizeof R);
    R.op = GEOM_OFFSET;
    R.a = x0;
    R.b = y0;
    R.wrap = 0;
    R.sw = d->w;
    R.sh = d->h;
    R.dw = (uint32_t)(x1 - x0);
    R.dh = (uint32_t)(y1 - y0);
    remap_doc(d, &R, fill, 0);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/*
 * px-translate --dx --dy --wrap 0|1
 *
 * Shifts every record by (dx, dy) on an unchanged canvas: destination
 * (x, y) reads source (x - dx, y - dy).  With --wrap 1 the source index is
 * reduced modulo the canvas (the mathematical modulus, so negative shifts
 * wrap the same way positive ones do); with --wrap 0 (the default) pixels
 * shifted in from outside are transparent/zero in every channel.
 */
static int cmd_px_translate(int argc, char **argv) {
    static const char *allowed[] = {"dx", "dy", "wrap"};
    Args a;
    Doc *d;
    Remap R;
    long dx, dy;
    int wrap;
    uint8_t fill[4] = {0, 0, 0, 0};
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    dx = arg_int(&a, "dx", 0, -MAX_DIM, MAX_DIM);
    dy = arg_int(&a, "dy", 0, -MAX_DIM, MAX_DIM);
    wrap = (int)arg_int(&a, "wrap", 0, 0, 1);
    d = ldx_read(a.pos[0]);
    memset(&R, 0, sizeof R);
    R.op = GEOM_OFFSET;
    R.a = -dx;
    R.b = -dy;
    R.wrap = wrap;
    R.sw = d->w;
    R.sh = d->h;
    R.dw = d->w;
    R.dh = d->h;
    remap_doc(d, &R, fill, 0);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/*
 * px-tile --cols --rows: repeat the canvas cols x rows times.  Destination
 * (x, y) reads source (x mod w, y mod h); the new canvas is w*cols by
 * h*rows and must stay within MAX_DIM.
 */
static int cmd_px_tile(int argc, char **argv) {
    static const char *allowed[] = {"cols", "rows"};
    Args a;
    Doc *d;
    Remap R;
    long cols, rows, nw, nh;
    uint8_t fill[4] = {0, 0, 0, 0};
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    cols = arg_int(&a, "cols", 1, 1, MAX_DIM);
    rows = arg_int(&a, "rows", 1, 1, MAX_DIM);
    d = ldx_read(a.pos[0]);
    nw = (long)d->w * cols;
    nh = (long)d->h * rows;
    if (nw > MAX_DIM || nh > MAX_DIM)
        die(EXIT_USAGE, "E_BAD_ARGS", "resulting canvas exceeds the maximum dimension");
    geom_check_dims((uint32_t)nw, (uint32_t)nh);
    memset(&R, 0, sizeof R);
    R.op = GEOM_TILE;
    R.a = cols;
    R.b = rows;
    R.sw = d->w;
    R.sh = d->h;
    R.dw = (uint32_t)nw;
    R.dh = (uint32_t)nh;
    remap_doc(d, &R, fill, 0);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/*
 * px-transform --op flip_h|flip_v|rot90|rot180|rot270
 * Dispatcher over the orientation half of the engine; each name produces
 * output identical to the standalone command of the same name.
 */
static int cmd_px_transform(int argc, char **argv) {
    static const char *allowed[] = {"op"};
    Args a;
    Doc *d;
    Remap R;
    const char *op;
    int code;
    uint8_t fill[4] = {0, 0, 0, 0};
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    op = arg_req(&a, "op");
    if (strcmp(op, "flip_h") == 0) code = GEOM_FLIP_H;
    else if (strcmp(op, "flip_v") == 0) code = GEOM_FLIP_V;
    else if (strcmp(op, "rot90") == 0) code = GEOM_ROT90;
    else if (strcmp(op, "rot180") == 0) code = GEOM_ROT180;
    else if (strcmp(op, "rot270") == 0) code = GEOM_ROT270;
    else {
        die(EXIT_USAGE, "E_BAD_ARGS", "--op must be flip_h, flip_v, rot90, rot180 or rot270");
        return EXIT_USAGE;
    }
    d = ldx_read(a.pos[0]);
    memset(&R, 0, sizeof R);
    R.op = code;
    R.sw = d->w;
    R.sh = d->h;
    remap_orient_dims(code, d->w, d->h, &R.dw, &R.dh);
    remap_doc(d, &R, fill, 0);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

#define FAM_GEOM_COMMANDS \
    {"px-flip-h", cmd_px_flip_h, "geom"}, \
    {"px-flip-v", cmd_px_flip_v, "geom"}, \
    {"px-rot90", cmd_px_rot90, "geom"}, \
    {"px-rot180", cmd_px_rot180, "geom"}, \
    {"px-rot270", cmd_px_rot270, "geom"}, \
    {"px-transpose", cmd_px_transpose, "geom"}, \
    {"px-scale", cmd_px_scale, "geom"}, \
    {"px-pad", cmd_px_pad, "geom"}, \
    {"px-translate", cmd_px_translate, "geom"}, \
    {"px-tile", cmd_px_tile, "geom"}, \
    {"px-transform", cmd_px_transform, "geom"},
