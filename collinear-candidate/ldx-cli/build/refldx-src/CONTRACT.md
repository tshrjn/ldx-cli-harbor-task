# Family fragment contract — read before writing any `fam_*.c`

You are extending `refldx`, the reference implementation of the LDX layered-image CLI, from 7
commands to ~84. The catalog is in `../CATALOG.md`. The core lives in `ldx.c`; each family is
a fragment `#include`d into that single translation unit, so every `static` helper below is
already in scope. **Do not** add `#include` lines, global mutable state, or a `main`.

## What your file must contain

1. One `static int cmd_<name>(int argc, char **argv)` per command in your family. Return
   `EXIT_OK` on success; use `die(...)` for errors (it exits).
2. At the end, a single macro listing your dispatch entries, with a trailing comma on each:

```c
#define FAM_TONAL_COMMANDS \
    {"px-invert", cmd_px_invert, "tonal"}, \
    {"px-threshold", cmd_px_threshold, "tonal"},
```

The macro name is `FAM_LAYER_COMMANDS`, `FAM_TONAL_COMMANDS`, `FAM_GEOM_COMMANDS`,
`FAM_FILTER_COMMANDS` or `FAM_DOCSEL_COMMANDS` — exactly one, matching your filename.

## Hard rules

- **Integer arithmetic only.** No floating point anywhere, not even in a constant. Fixed point
  where a ratio is needed, with the rounding schedule written in a comment.
- Deterministic and single-threaded. Any randomness (e.g. `px-noise`) uses an explicit
  `--seed` and a documented integer LCG, so output is reproducible.
- C99, compiles clean under `-Wall -Wextra`. Declare variables at the top of a block (the
  existing file style). No VLAs.
- Never write a partial output file on error: validate arguments, then `ldx_read`, then work,
  then `ldx_write` last.
- Every command that writes a document ends with `ldx_write(d, a.pos[1])` and takes exactly
  two positionals unless the catalog says otherwise.
- Reserved bytes and record padding are zero-filled by `ldx_write`; never hand-roll a writer.
- Layer indices are validated: out of range or wrong kind is `E_BAD_ARGS`, exit 2.

## Available core API (all already in scope)

```c
/* exit codes */            EXIT_OK EXIT_USAGE EXIT_UNSUPPORTED EXIT_BAD_FILE EXIT_IO
/* diagnostics */           void die(int rc, const char *code, const char *msg);   /* exits */
                            void warn(const char *code, const char *msg);          /* stderr, continues */
/* memory */                void *xmalloc(size_t n); void *xcalloc(size_t n);

/* record kinds */          KIND_RASTER KIND_GROUP_OPEN KIND_GROUP_CLOSE KIND_MASK
/* layer flag bits */       FLAG_VISIBLE FLAG_LOCKED FLAG_BACKGROUND
/* blend modes */           BLEND_NORMAL MULTIPLY SCREEN DARKEN LIGHTEN DIFFERENCE, BLEND_COUNT
/* limits */                MAX_DIM (16384) MAX_LAYERS (4096)

typedef struct {            /* one record */
    uint8_t name_len; char name[256];
    uint8_t kind, opacity, blend, flags;
    int16_t parent; uint32_t data_len; uint8_t *data;
} Layer;

typedef struct {            /* a document */
    uint16_t flags;         /* bit0 has_alpha, bit1 indexed */
    uint32_t w, h; uint8_t mode;   /* 0 gray, 1 rgb */
    uint32_t resolution;    /* Q16.16 dpi */
    uint16_t nlayers; Layer *layers;
} Doc;

int  doc_has_alpha(const Doc *);        /* 0/1 */
int  doc_color_channels(const Doc *);   /* 1 gray, 3 rgb */
int  doc_channels(const Doc *);         /* colour + alpha */
Doc *doc_alloc(void);
uint32_t layer_expected_len(const Doc *, uint8_t kind);
Doc *ldx_read(const char *path);        /* strict; dies on malformed input */
void ldx_write(const Doc *, const char *path);
void doc_print_json(const Doc *, int dump);

/* argument parsing */
typedef struct { const char *keys[32], *vals[32]; int n; const char *pos[8]; int npos; } Args;
void parse_args(int argc, char **argv, int start, Args *a, const char **allowed, int nallowed);
const char *arg_get(const Args *, const char *key);      /* NULL if absent */
const char *arg_req(const Args *, const char *key);      /* dies if absent */
long parse_int(const char *s, const char *what, long lo, long hi);
long arg_int(const Args *, const char *key, long dflt, long lo, long hi);
void positional(const Args *, int n);                    /* dies unless exactly n */
int  parse_blend(const char *);                          /* name -> BLEND_* */
void parse_fill(const char *s, int n, uint8_t *out);     /* "r,g,b,a" -> n bytes */
void set_name(Layer *, const char *name);                /* validates 1..255 printable ASCII */
uint8_t *pnm_read(const char *path, uint32_t w, uint32_t h, int *ch);   /* P5/P6 */

/* pixels */
int  blend_channel(int mode, int cb, int cs);
int64_t div_round(int64_t num, int64_t den);             /* round half up, non-negative */
void composite_pixel(uint8_t *dst, const uint8_t *src, int cc, int eff, int mask, int mode);
uint8_t *doc_render(const Doc *);                        /* full render, cc+1 channels */
void layer_free(Layer *);
void layers_insert(Doc *, int pos, int n);               /* inserts blanks, fixes parents */
int  group_close_of(const Doc *, int idx);               /* matching group_close, or -1 */
```

Flags are `--name value` pairs. The only switches that take no value are `--dump` and
`--all`; if your family needs another, say so in your summary rather than inventing one.

`parse_args(argc, argv, 2, &a, allowed, n)` where `allowed` is a `static const char *[]` of
the flag names your command accepts — anything else is `E_USAGE`.

## Planted edge behaviours you must implement exactly

These are deliberately absent from the public `SPEC.md`; the binary is the definition.

- **Clamping (E03).** Every tonal result clamps to 0..255. Never wrap, never truncate to
  a narrower range.
- **Rounding.** Any division of non-negative operands rounds **half up**: use `div_round`, or
  `(a + b/2) / b`. In the fixed-point kernel engine (E04) rounding is applied **per stage**,
  half away from zero, so a negative accumulator rounds away from zero too.
- **Geometry (E09).** `px-rot270` must be bit-identical to applying `px-rot90` three times.
- **Out-of-canvas rectangles (E07).** Clamp to the canvas, emit
  `warn("W_CROP_CLAMPED", ...)`, and exit 0. A rectangle with no intersection is
  `E_BAD_RECT`, exit 2.
- **Mode errors (E10).** An operation that needs three colour channels, applied to a gray
  document, is `die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", ...)`.
- **Alpha.** Tonal operations leave the alpha channel untouched unless the command is
  explicitly about alpha.

## Deliverable

Write only your `fam_*.c` file. Then report: the command list, any flag you had to add, and
anything in the catalog you could not implement and why. Verify it compiles by running

```
cd <this directory> && cc -O2 -std=c99 -Wall -Wextra -c -o /dev/null -x c ldx.c
```

after your file is in place (the placeholder fragments for other families already exist, so a
clean compile means your fragment integrates).
