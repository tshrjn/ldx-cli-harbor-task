/*
 * fam_layer.c -- family B: layer management (catalog rows 12..32, 21 commands).
 *
 * Everything here is record-list surgery on Doc.layers plus a little pixel work.
 * Two structural invariants are maintained by every command:
 *
 *   1. Group nesting stays balanced.  A group_open is never moved, copied or
 *      removed without its matching group_close and the whole body between them,
 *      so the running depth of the record list is never disturbed.
 *   2. A mask record belongs to the raster record immediately below it (the
 *      convention doc_render uses: the mask of layer i lives at i+1).  A raster
 *      therefore always travels together with its mask; nothing is ever inserted
 *      into the gap between them.
 *
 * parent_idx is repaired after every move/removal.  Records whose parent merely
 * shifted are renumbered arithmetically; records whose parent disappeared are
 * given the structurally correct parent (the innermost enclosing group_open, or
 * -1), matching the convention cmd_layer_add uses -- and a group_close keeps
 * pointing at its own group_open.
 *
 * Integer arithmetic only.  Compositing is delegated to composite_pixel() so
 * layer-merge-down, layer-merge-visible and doc-flatten agree bit for bit
 * (E01 round half up, E02 un-premultiply after compositing, E05 blend bypass,
 * E06 mask before opacity, E11 group opacity compounding).
 */

/* ------------------------------------------------------------------ */
/* Structural helpers                                                  */
/* ------------------------------------------------------------------ */

static void lyr_bad(const char *msg) { die(EXIT_USAGE, "E_BAD_ARGS", msg); }

/* innermost enclosing group_open of record idx, or -1 (positional nesting) */
static int lyr_enclosing_group(const Doc *d, int idx) {
    int stack[MAX_LAYERS], sp = 0, i;
    for (i = 0; i < idx && i < (int)d->nlayers; i++) {
        if (d->layers[i].kind == KIND_GROUP_OPEN) {
            if (sp < MAX_LAYERS) stack[sp++] = i;
        } else if (d->layers[i].kind == KIND_GROUP_CLOSE) {
            if (sp > 0) sp--;
        }
    }
    return sp > 0 ? stack[sp - 1] : -1;
}

/* the parent_idx a record should carry given the current nesting */
static int16_t lyr_default_parent(const Doc *d, int idx) {
    if (d->layers[idx].kind == KIND_GROUP_CLOSE) {
        int depth = 0, j;
        for (j = idx; j >= 0; j--) {
            if (d->layers[j].kind == KIND_GROUP_CLOSE) depth++;
            else if (d->layers[j].kind == KIND_GROUP_OPEN) {
                depth--;
                if (depth == 0) return (int16_t)j;
            }
        }
        return -1;
    }
    return (int16_t)lyr_enclosing_group(d, idx);
}

/* index of the mask attached to raster idx, or -1 */
static int lyr_mask_of(const Doc *d, int idx) {
    if (idx + 1 < (int)d->nlayers && d->layers[idx + 1].kind == KIND_MASK) return idx + 1;
    return -1;
}

/* Validate that idx names a movable/removable record and return its whole span
   [*lo,*hi] inclusive: a raster plus its mask, or a group_open .. group_close.
   `flag` only shapes the diagnostic. */
static void lyr_span(const Doc *d, int idx, const char *flag, int *lo, int *hi) {
    char msg[128];
    *lo = idx;
    *hi = idx;
    if (idx < 0 || idx >= (int)d->nlayers) {
        snprintf(msg, sizeof msg, "--%s out of range", flag);
        lyr_bad(msg);
        return; /* unreachable: lyr_bad exits */
    }
    if (d->layers[idx].kind == KIND_RASTER) {
        int m = lyr_mask_of(d, idx);
        *hi = m >= 0 ? m : idx; /* a raster always travels with its mask */
    } else if (d->layers[idx].kind == KIND_GROUP_OPEN) {
        int c = group_close_of(d, idx); /* a group takes its body and close */
        if (c < 0) die(EXIT_BAD_FILE, "E_BAD_FILE", "group_open without a matching group_close");
        *hi = c;
    } else {
        snprintf(msg, sizeof msg, "--%s must name a raster or group_open record", flag);
        lyr_bad(msg);
        *hi = idx;
    }
}

/* raster-only index check */
static Layer *lyr_raster(Doc *d, int idx) {
    if (idx < 0 || idx >= (int)d->nlayers || d->layers[idx].kind != KIND_RASTER)
        lyr_bad("--index must be the index of a raster layer");
    return &d->layers[idx];
}

/* raster or group_open (the two kinds that carry meaningful opacity/blend) */
static Layer *lyr_composable(Doc *d, int idx) {
    if (idx < 0 || idx >= (int)d->nlayers ||
        (d->layers[idx].kind != KIND_RASTER && d->layers[idx].kind != KIND_GROUP_OPEN))
        lyr_bad("--index must name a raster or group_open record");
    return &d->layers[idx];
}

/* Paranoia check before writing: nesting balanced and every parent in range.
   Should never fire; it exists so a bug can never produce a file that the
   strict reader would reject. */
static void lyr_check(const Doc *d) {
    int depth = 0, i;
    for (i = 0; i < (int)d->nlayers; i++) {
        if (d->layers[i].kind == KIND_GROUP_OPEN) depth++;
        else if (d->layers[i].kind == KIND_GROUP_CLOSE) {
            depth--;
            if (depth < 0) die(EXIT_BAD_FILE, "E_BAD_FILE", "unbalanced group records");
        }
        if (d->layers[i].parent < -1 || d->layers[i].parent >= (int)d->nlayers)
            die(EXIT_BAD_FILE, "E_BAD_FILE", "bad parent_idx");
    }
    if (depth != 0) die(EXIT_BAD_FILE, "E_BAD_FILE", "unbalanced group records");
}

/* Remove every record marked in rm[], repairing parent_idx.  This is the single
   removal primitive: whole spans are removed by marking the span. */
static void lyr_remove_marked(Doc *d, const unsigned char *rm) {
    int map[MAX_LAYERS];
    int i, n = 0, old = (int)d->nlayers;
    for (i = 0; i < old; i++) {
        if (rm[i]) {
            layer_free(&d->layers[i]);
            map[i] = -1;
        } else {
            map[i] = n;
            if (n != i) d->layers[n] = d->layers[i];
            n++;
        }
    }
    if (n < old) memset(&d->layers[n], 0, sizeof(Layer) * (size_t)(old - n));
    d->nlayers = (uint16_t)n;
    for (i = 0; i < n; i++) {
        int p = d->layers[i].parent;
        if (p < 0) {
            d->layers[i].parent = -1;
        } else if (p < old && map[p] >= 0) {
            d->layers[i].parent = (int16_t)map[p];
        } else {
            d->layers[i].parent = lyr_default_parent(d, i); /* parent went away */
        }
    }
}

static void lyr_remove_range(Doc *d, int lo, int hi) {
    unsigned char rm[MAX_LAYERS];
    int i;
    memset(rm, 0, sizeof rm);
    for (i = lo; i <= hi; i++) rm[i] = 1;
    lyr_remove_marked(d, rm);
}

/* Move the whole record range [lo,hi] so that it sits at gap `dest`, where dest
   is expressed in the current index space (0..nlayers) and must not fall
   strictly inside the range.  parent_idx is remapped for every record. */
static void lyr_move_range(Doc *d, int lo, int hi, int dest) {
    int n = hi - lo + 1, nl = (int)d->nlayers, i, newpos;
    Layer *tmp;
    int *map;
    if (dest > lo && dest <= hi) lyr_bad("cannot move a record range inside itself");
    if (dest == lo) return; /* no-op */
    tmp = (Layer *)xmalloc(sizeof(Layer) * (size_t)n);
    map = (int *)xmalloc(sizeof(int) * (size_t)(nl > 0 ? nl : 1));
    memcpy(tmp, &d->layers[lo], sizeof(Layer) * (size_t)n);
    memmove(&d->layers[lo], &d->layers[hi + 1], sizeof(Layer) * (size_t)(nl - hi - 1));
    newpos = dest > hi ? dest - n : dest;
    memmove(&d->layers[newpos + n], &d->layers[newpos], sizeof(Layer) * (size_t)(nl - n - newpos));
    memcpy(&d->layers[newpos], tmp, sizeof(Layer) * (size_t)n);
    for (i = 0; i < nl; i++) {
        if (i >= lo && i <= hi) {
            map[i] = newpos + (i - lo);
        } else {
            int j = i < lo ? i : i - n;
            map[i] = j < newpos ? j : j + n;
        }
    }
    for (i = 0; i < nl; i++) {
        int p = d->layers[i].parent;
        d->layers[i].parent = (int16_t)(p < 0 || p >= nl ? -1 : map[p]);
    }
    free(tmp);
    free(map);
}

/* Effective visibility of every record: own FLAG_VISIBLE and every enclosing
   group visible, exactly as doc_render decides it. */
static void lyr_vis_map(const Doc *d, unsigned char *vis) {
    int st[MAX_LAYERS], sp = 0, i;
    st[0] = 1;
    for (i = 0; i < (int)d->nlayers; i++) {
        const Layer *L = &d->layers[i];
        int v = st[sp] && (L->flags & FLAG_VISIBLE) ? 1 : 0;
        vis[i] = (unsigned char)v;
        if (L->kind == KIND_GROUP_OPEN) {
            if (sp + 1 < MAX_LAYERS) sp++;
            st[sp] = v;
        } else if (L->kind == KIND_GROUP_CLOSE) {
            if (sp > 0) sp--;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                       */
/* ------------------------------------------------------------------ */

/*
 * Composite raster record idx onto `canvas` (cc+1 channels, straight alpha),
 * with its own opacity, blend and mask, exactly the way doc_render composites a
 * top-level layer.  Invisible records contribute nothing.  Compositing (and so
 * the un-premultiplication, E02) happens inside composite_pixel, which also
 * carries E01 (round half up), E05 (blend bypassed over zero alpha) and E06
 * (mask applied before layer opacity).
 */
static void lyr_composite_record(const Doc *d, int idx, uint8_t *canvas) {
    int cc = doc_color_channels(d), ch = doc_channels(d), has_a = doc_has_alpha(d), k, m;
    const Layer *L = &d->layers[idx];
    const uint8_t *mask = NULL;
    size_t npx = (size_t)d->w * d->h, p;
    uint8_t src[4];
    if (!(L->flags & FLAG_VISIBLE)) return;
    m = lyr_mask_of(d, idx);
    if (m >= 0) mask = d->layers[m].data;
    for (p = 0; p < npx; p++) {
        const uint8_t *s = L->data + p * (size_t)ch;
        for (k = 0; k < cc; k++) src[k] = s[k];
        src[cc] = has_a ? s[cc] : 255;
        composite_pixel(canvas + p * (size_t)(cc + 1), src, cc, L->opacity,
                        mask ? mask[p] : 255, L->blend);
    }
}

/* Copy a rendered cc+1 canvas into a fresh raster payload for this document. */
static uint8_t *lyr_canvas_to_raster(const Doc *d, const uint8_t *canvas) {
    int cc = doc_color_channels(d), ch = doc_channels(d), has_a = doc_has_alpha(d), k;
    size_t npx = (size_t)d->w * d->h, p;
    uint8_t *out = (uint8_t *)xmalloc(npx * (size_t)ch);
    for (p = 0; p < npx; p++) {
        for (k = 0; k < cc; k++) out[p * (size_t)ch + k] = canvas[p * (size_t)(cc + 1) + k];
        if (has_a) out[p * (size_t)ch + cc] = canvas[p * (size_t)(cc + 1) + cc];
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* 12. layer-delete                                                    */
/* ------------------------------------------------------------------ */

static int cmd_layer_delete(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    int idx, lo, hi;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    lyr_span(d, idx, "index", &lo, &hi);
    lyr_remove_range(d, lo, hi);
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 13. layer-duplicate                                                 */
/* ------------------------------------------------------------------ */

static int cmd_layer_duplicate(int argc, char **argv) {
    static const char *allowed[] = {"index", "name"};
    Args a;
    Doc *d;
    int idx, lo, hi, n, i;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    lyr_span(d, idx, "index", &lo, &hi);
    n = hi - lo + 1;
    layers_insert(d, hi + 1, n); /* shifts every parent >= hi+1 by n */
    for (i = 0; i < n; i++) {
        const Layer *S = &d->layers[lo + i];
        Layer *T = &d->layers[hi + 1 + i];
        int p = S->parent;
        memcpy(T->name, S->name, sizeof T->name);
        T->name_len = S->name_len;
        T->kind = S->kind;
        T->opacity = S->opacity;
        T->blend = S->blend;
        T->flags = S->flags;
        /* a parent inside the copied block points at the copy (index + n) */
        T->parent = (int16_t)(p >= lo && p <= hi ? p + n : p);
        T->data_len = S->data_len;
        if (S->data_len) {
            T->data = (uint8_t *)xmalloc(S->data_len);
            memcpy(T->data, S->data, S->data_len);
        } else {
            T->data = NULL;
        }
    }
    if (arg_get(&a, "name")) set_name(&d->layers[hi + 1], arg_get(&a, "name"));
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 14. layer-rename                                                    */
/* ------------------------------------------------------------------ */

static int cmd_layer_rename(int argc, char **argv) {
    static const char *allowed[] = {"index", "name"};
    Args a;
    Doc *d;
    int idx;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    if (idx >= (int)d->nlayers || d->layers[idx].kind == KIND_GROUP_CLOSE)
        lyr_bad("--index must name a raster, group_open or mask record");
    set_name(&d->layers[idx], arg_req(&a, "name"));
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 15. layer-set                                                       */
/* ------------------------------------------------------------------ */

static int cmd_layer_set(int argc, char **argv) {
    static const char *allowed[] = {"index", "opacity", "blend", "visible", "locked"};
    Args a;
    Doc *d;
    Layer *L;
    int idx;
    parse_args(argc, argv, 2, &a, allowed, 5);
    positional(&a, 2);
    if (!arg_get(&a, "opacity") && !arg_get(&a, "blend") && !arg_get(&a, "visible") &&
        !arg_get(&a, "locked"))
        die(EXIT_USAGE, "E_USAGE", "layer-set needs at least one of --opacity --blend --visible --locked");
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    L = lyr_composable(d, idx);
    if (arg_get(&a, "opacity")) L->opacity = (uint8_t)arg_int(&a, "opacity", 255, 0, 255);
    if (arg_get(&a, "blend")) L->blend = (uint8_t)parse_blend(arg_get(&a, "blend"));
    if (arg_get(&a, "visible")) {
        if (arg_int(&a, "visible", 1, 0, 1)) L->flags |= FLAG_VISIBLE;
        else L->flags = (uint8_t)(L->flags & ~FLAG_VISIBLE);
    }
    if (arg_get(&a, "locked")) {
        if (arg_int(&a, "locked", 0, 0, 1)) L->flags |= FLAG_LOCKED;
        else L->flags = (uint8_t)(L->flags & ~FLAG_LOCKED);
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 16. layer-reorder                                                   */
/* ------------------------------------------------------------------ */

static int cmd_layer_reorder(int argc, char **argv) {
    static const char *allowed[] = {"from", "to"};
    Args a;
    Doc *d;
    int from, to, lo, hi, n, dest;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    from = (int)parse_int(arg_req(&a, "from"), "from", 0, MAX_LAYERS);
    to = (int)parse_int(arg_req(&a, "to"), "to", 0, MAX_LAYERS);
    lyr_span(d, from, "from", &lo, &hi);
    n = hi - lo + 1;
    /* --to is the index the moved record ends up at in the new list */
    if (to > (int)d->nlayers - n) lyr_bad("--to out of range for this record and its children");
    dest = to < lo ? to : to + n; /* back into the current index space */
    if (dest < (int)d->nlayers && d->layers[dest].kind == KIND_MASK)
        lyr_bad("--to would separate a raster layer from its mask");
    lyr_move_range(d, lo, hi, dest);
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 17. layer-group                                                     */
/* ------------------------------------------------------------------ */

static int cmd_layer_group(int argc, char **argv) {
    static const char *allowed[] = {"from", "to", "name"};
    Args a;
    Doc *d;
    Layer *O, *C;
    int from, to, i, depth = 0;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    from = (int)parse_int(arg_req(&a, "from"), "from", 0, MAX_LAYERS);
    to = (int)parse_int(arg_req(&a, "to"), "to", 0, MAX_LAYERS);
    if (from >= (int)d->nlayers || to >= (int)d->nlayers) lyr_bad("--from/--to out of range");
    if (to < from) lyr_bad("--to must not be smaller than --from");
    /* the run must be self-contained: balanced, and no mask cut off from its raster */
    for (i = from; i <= to; i++) {
        if (d->layers[i].kind == KIND_GROUP_OPEN) depth++;
        else if (d->layers[i].kind == KIND_GROUP_CLOSE) {
            depth--;
            if (depth < 0) lyr_bad("--from/--to must not split a group");
        }
    }
    if (depth != 0) lyr_bad("--from/--to must not split a group");
    /* never cut a raster away from its mask: widen the run to whole pairs */
    if (d->layers[from].kind == KIND_MASK) {
        if (from == 0 || d->layers[from - 1].kind != KIND_RASTER)
            lyr_bad("--from names a mask record that is not attached to a raster layer");
        from--;
    }
    if (d->layers[to].kind == KIND_RASTER && to + 1 < (int)d->nlayers &&
        d->layers[to + 1].kind == KIND_MASK)
        to++;

    layers_insert(d, from, 1);      /* group_open at from; run is now from+1..to+1 */
    layers_insert(d, to + 2, 1);    /* group_close after the run */
    O = &d->layers[from];
    set_name(O, arg_req(&a, "name"));
    O->kind = KIND_GROUP_OPEN;
    O->opacity = 255;
    O->blend = BLEND_NORMAL;
    O->flags = FLAG_VISIBLE;
    O->parent = lyr_default_parent(d, from);
    O->data_len = 0;
    O->data = NULL;
    C = &d->layers[to + 2];
    C->name_len = 0;
    C->name[0] = 0;
    C->kind = KIND_GROUP_CLOSE;
    C->opacity = 255;
    C->blend = BLEND_NORMAL;
    C->flags = 0;
    C->parent = (int16_t)from;
    C->data_len = 0;
    C->data = NULL;
    /* only the run's direct children change parent; nested records keep theirs */
    for (i = from + 1; i <= to + 1; i++)
        if (lyr_enclosing_group(d, i) == from) d->layers[i].parent = (int16_t)from;
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 18. layer-ungroup                                                   */
/* ------------------------------------------------------------------ */

static int cmd_layer_ungroup(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    unsigned char rm[MAX_LAYERS];
    int idx, close;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    if (idx >= (int)d->nlayers || d->layers[idx].kind != KIND_GROUP_OPEN)
        lyr_bad("--index must be the index of a group_open record");
    close = group_close_of(d, idx);
    if (close < 0) die(EXIT_BAD_FILE, "E_BAD_FILE", "group_open without a matching group_close");
    /* E08: an empty group simply disappears and layer_count drops by two.  The
       children (if any) are reparented to the group's own parent by
       lyr_remove_marked, which recomputes the parent of every record that
       pointed at the removed group_open. */
    memset(rm, 0, sizeof rm);
    rm[idx] = 1;
    rm[close] = 1;
    lyr_remove_marked(d, rm);
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 19. layer-merge-down                                                */
/* ------------------------------------------------------------------ */

static int cmd_layer_merge_down(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    Layer *B;
    unsigned char rm[MAX_LAYERS];
    uint8_t *canvas, *merged;
    int idx, below, cc, mtop, mbot;
    size_t npx;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    lyr_raster(d, idx);
    /* the record below is the previous one, or the one before it when that is
       this layer's neighbour's mask.  Anything else (a group boundary, or
       nothing at all) is E_BAD_ARGS -- see E08. */
    below = idx - 1;
    if (below >= 0 && d->layers[below].kind == KIND_MASK) below = idx - 2;
    if (below < 0 || d->layers[below].kind != KIND_RASTER)
        lyr_bad("there is no raster layer directly below --index");

    cc = doc_color_channels(d);
    npx = (size_t)d->w * d->h;
    canvas = (uint8_t *)xcalloc(npx * (size_t)(cc + 1));
    lyr_composite_record(d, below, canvas); /* backdrop first ... */
    lyr_composite_record(d, idx, canvas);   /* ... then the layer being merged  */
    merged = lyr_canvas_to_raster(d, canvas);
    free(canvas);

    mbot = lyr_mask_of(d, below);
    mtop = lyr_mask_of(d, idx);
    B = &d->layers[below];
    layer_free(B);
    B->data = merged;
    B->data_len = layer_expected_len(d, KIND_RASTER);
    /* opacity, blend and mask are baked in, so the merged record is a plain,
       fully opaque, normal-blend raster */
    B->opacity = 255;
    B->blend = BLEND_NORMAL;
    B->flags |= FLAG_VISIBLE;
    memset(rm, 0, sizeof rm);
    rm[idx] = 1;
    if (mtop >= 0) rm[mtop] = 1;
    if (mbot >= 0) rm[mbot] = 1;
    lyr_remove_marked(d, rm);
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 20. layer-merge-visible                                             */
/* ------------------------------------------------------------------ */

static int cmd_layer_merge_visible(int argc, char **argv) {
    Args a;
    Doc *d;
    Layer *L;
    unsigned char rm[MAX_LAYERS], vis[MAX_LAYERS];
    uint8_t *canvas, *merged;
    int i, changed;
    parse_args(argc, argv, 2, &a, NULL, 0);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    canvas = doc_render(d); /* honours group nesting, E11 opacity compounding */
    merged = lyr_canvas_to_raster(d, canvas);
    free(canvas);

    /* every record that contributed to the render goes away: visible rasters,
       their masks, and any group left holding nothing (E08) */
    lyr_vis_map(d, vis);
    memset(rm, 0, sizeof rm);
    for (i = 0; i < (int)d->nlayers; i++) {
        if (d->layers[i].kind == KIND_RASTER && vis[i]) {
            int m = lyr_mask_of(d, i);
            rm[i] = 1;
            if (m >= 0) rm[m] = 1;
        }
    }
    do {
        changed = 0;
        for (i = 0; i < (int)d->nlayers; i++) {
            if (d->layers[i].kind == KIND_GROUP_OPEN && !rm[i]) {
                int c = group_close_of(d, i), j, empty = 1;
                if (c < 0) break;
                for (j = i + 1; j < c; j++)
                    if (!rm[j]) empty = 0;
                if (empty) {
                    rm[i] = 1;
                    rm[c] = 1;
                    changed = 1;
                }
            }
        }
    } while (changed);
    lyr_remove_marked(d, rm);

    /* the merged raster goes to the bottom of the stack at the top level, so
       that re-rendering it reproduces the pixels exactly */
    layers_insert(d, 0, 1);
    L = &d->layers[0];
    set_name(L, "Merged");
    L->kind = KIND_RASTER;
    L->opacity = 255;
    L->blend = BLEND_NORMAL;
    L->flags = FLAG_VISIBLE;
    L->parent = -1;
    L->data_len = layer_expected_len(d, KIND_RASTER);
    L->data = merged;
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 21. layer-mask-add                                                  */
/* ------------------------------------------------------------------ */

static int cmd_layer_mask_add(int argc, char **argv) {
    static const char *allowed[] = {"index", "from", "fill"};
    Args a;
    Doc *d;
    Layer *M;
    int idx;
    size_t npx;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    lyr_raster(d, idx);
    if (lyr_mask_of(d, idx) >= 0) lyr_bad("layer already has a mask");
    if (arg_get(&a, "fill") && arg_get(&a, "from"))
        lyr_bad("--fill and --from are mutually exclusive");
    npx = (size_t)d->w * d->h;
    layers_insert(d, idx + 1, 1);
    M = &d->layers[idx + 1];
    set_name(M, "Mask");
    M->kind = KIND_MASK;
    M->opacity = 255;
    M->blend = BLEND_NORMAL;
    M->flags = FLAG_VISIBLE;
    /* the mask sits in the same group as the layer it belongs to */
    M->parent = (int16_t)lyr_enclosing_group(d, idx + 1);
    M->data_len = layer_expected_len(d, KIND_MASK);
    M->data = (uint8_t *)xmalloc(M->data_len);
    if (arg_get(&a, "from")) {
        int pch;
        uint8_t *px = pnm_read(arg_get(&a, "from"), d->w, d->h, &pch);
        if (pch != 1) lyr_bad("--from must be a P5 (gray) PNM file");
        memcpy(M->data, px, npx);
        free(px);
    } else {
        uint8_t v = 255; /* an opaque mask changes nothing */
        if (arg_get(&a, "fill")) parse_fill(arg_get(&a, "fill"), 1, &v);
        memset(M->data, v, npx);
    }
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 22. layer-mask-apply                                                */
/* ------------------------------------------------------------------ */

static int cmd_layer_mask_apply(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    Layer *L;
    const Layer *M;
    unsigned char rm[MAX_LAYERS];
    int idx, m, ch, cc;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    L = lyr_raster(d, idx);
    m = lyr_mask_of(d, idx);
    if (m < 0) lyr_bad("layer has no mask");
    if (!doc_has_alpha(d))
        die(EXIT_UNSUPPORTED, "E_MODE_UNSUPPORTED", "document has no alpha channel to bake the mask into");
    M = &d->layers[m];
    ch = doc_channels(d);
    cc = doc_color_channels(d);
    npx = (size_t)d->w * d->h;
    /* E06: the mask multiplies the *source alpha*, before the layer opacity --
       the same order composite_pixel uses, so the rendered result is unchanged.
       Rounding is half up: (a*mask + 127) / 255. */
    for (p = 0; p < npx; p++) {
        int av = L->data[p * (size_t)ch + cc];
        L->data[p * (size_t)ch + cc] = (uint8_t)((av * (int)M->data[p] + 127) / 255);
    }
    memset(rm, 0, sizeof rm);
    rm[m] = 1;
    lyr_remove_marked(d, rm);
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 23. layer-mask-remove                                               */
/* ------------------------------------------------------------------ */

static int cmd_layer_mask_remove(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    unsigned char rm[MAX_LAYERS];
    int idx, m;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    lyr_raster(d, idx);
    m = lyr_mask_of(d, idx);
    if (m < 0) lyr_bad("layer has no mask");
    memset(rm, 0, sizeof rm);
    rm[m] = 1;
    lyr_remove_marked(d, rm);
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 24. layer-mask-invert                                               */
/* ------------------------------------------------------------------ */

static int cmd_layer_mask_invert(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    Layer *M;
    int idx, m;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    lyr_raster(d, idx);
    m = lyr_mask_of(d, idx);
    if (m < 0) lyr_bad("layer has no mask");
    M = &d->layers[m];
    npx = (size_t)d->w * d->h;
    for (p = 0; p < npx; p++) M->data[p] = (uint8_t)(255 - M->data[p]);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 25. layer-fill                                                      */
/* ------------------------------------------------------------------ */

static int cmd_layer_fill(int argc, char **argv) {
    static const char *allowed[] = {"index", "colour"};
    Args a;
    Doc *d;
    Layer *L;
    uint8_t fill[4] = {0, 0, 0, 0};
    int idx, ch, k;
    size_t npx, p;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    L = lyr_raster(d, idx);
    ch = doc_channels(d);
    parse_fill(arg_req(&a, "colour"), ch, fill);
    npx = (size_t)d->w * d->h;
    for (p = 0; p < npx; p++)
        for (k = 0; k < ch; k++) L->data[p * (size_t)ch + k] = fill[k];
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 26. layer-clear                                                     */
/* ------------------------------------------------------------------ */

static int cmd_layer_clear(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    Layer *L;
    int idx;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    L = lyr_raster(d, idx);
    /* every channel to zero: transparent black where there is alpha, black otherwise */
    memset(L->data, 0, L->data_len);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 27. layer-offset                                                    */
/* ------------------------------------------------------------------ */

static int cmd_layer_offset(int argc, char **argv) {
    static const char *allowed[] = {"index", "dx", "dy", "wrap"};
    Args a;
    Doc *d;
    Layer *L;
    uint8_t *nd;
    long dx, dy, x, y, w, h;
    int idx, ch, wrap;
    parse_args(argc, argv, 2, &a, allowed, 4);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    L = lyr_raster(d, idx);
    dx = arg_int(&a, "dx", 0, -MAX_DIM, MAX_DIM);
    dy = arg_int(&a, "dy", 0, -MAX_DIM, MAX_DIM);
    wrap = (int)arg_int(&a, "wrap", 0, 0, 1);
    ch = doc_channels(d);
    w = (long)d->w;
    h = (long)d->h;
    /* vacated pixels are zero-filled; the layer's mask is canvas-space and is
       deliberately left alone */
    nd = (uint8_t *)xcalloc((size_t)w * (size_t)h * (size_t)ch);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            long sx = x - dx, sy = y - dy;
            if (wrap) {
                sx = ((sx % w) + w) % w;
                sy = ((sy % h) + h) % h;
            } else if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
                continue;
            }
            memcpy(nd + ((size_t)(y * w + x)) * (size_t)ch,
                   L->data + ((size_t)(sy * w + sx)) * (size_t)ch, (size_t)ch);
        }
    }
    layer_free(L);
    L->data = nd;
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 28. layer-copy                                                      */
/* ------------------------------------------------------------------ */

static int cmd_layer_copy(int argc, char **argv) {
    static const char *allowed[] = {"from", "to"};
    Args a;
    Doc *d;
    Layer *S, *T;
    int from, to;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    from = (int)parse_int(arg_req(&a, "from"), "from", 0, MAX_LAYERS);
    to = (int)parse_int(arg_req(&a, "to"), "to", 0, MAX_LAYERS);
    if (from >= (int)d->nlayers || to >= (int)d->nlayers) lyr_bad("--from/--to out of range");
    S = &d->layers[from];
    T = &d->layers[to];
    if (S->kind != T->kind || (S->kind != KIND_RASTER && S->kind != KIND_MASK))
        lyr_bad("--from and --to must both be raster records or both be mask records");
    if (from != to) memcpy(T->data, S->data, T->data_len);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 29. layer-swap                                                      */
/* ------------------------------------------------------------------ */

static int cmd_layer_swap(int argc, char **argv) {
    static const char *allowed[] = {"a", "b"};
    Args a;
    Doc *d;
    Layer *tmp;
    int *map;
    int ia, ib, alo, ahi, blo, bhi, nl, i, n = 0;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    ia = (int)parse_int(arg_req(&a, "a"), "a", 0, MAX_LAYERS);
    ib = (int)parse_int(arg_req(&a, "b"), "b", 0, MAX_LAYERS);
    lyr_span(d, ia, "a", &alo, &ahi);
    lyr_span(d, ib, "b", &blo, &bhi);
    if (alo == blo) {
        ldx_write(d, a.pos[1]); /* swapping a record with itself */
        return EXIT_OK;
    }
    if (blo < alo) { /* order them so a comes first */
        int t;
        t = alo; alo = blo; blo = t;
        t = ahi; ahi = bhi; bhi = t;
    }
    if (blo <= ahi) lyr_bad("--a and --b overlap; a group cannot be swapped with its own child");
    /* new order: [..alo) [b] (ahi..blo) [a] (bhi..]  -- both spans are whole and
       balanced, so nesting stays balanced and each mask stays with its raster */
    nl = (int)d->nlayers;
    tmp = (Layer *)xmalloc(sizeof(Layer) * (size_t)nl);
    map = (int *)xmalloc(sizeof(int) * (size_t)nl);
    for (i = 0; i < alo; i++) { tmp[n] = d->layers[i]; map[i] = n++; }
    for (i = blo; i <= bhi; i++) { tmp[n] = d->layers[i]; map[i] = n++; }
    for (i = ahi + 1; i < blo; i++) { tmp[n] = d->layers[i]; map[i] = n++; }
    for (i = alo; i <= ahi; i++) { tmp[n] = d->layers[i]; map[i] = n++; }
    for (i = bhi + 1; i < nl; i++) { tmp[n] = d->layers[i]; map[i] = n++; }
    memcpy(d->layers, tmp, sizeof(Layer) * (size_t)nl);
    for (i = 0; i < nl; i++) {
        int p = d->layers[i].parent;
        d->layers[i].parent = (int16_t)(p < 0 || p >= nl ? -1 : map[p]);
    }
    free(tmp);
    free(map);
    lyr_check(d);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 30. layer-opacity-scale                                             */
/* ------------------------------------------------------------------ */

static int cmd_layer_opacity_scale(int argc, char **argv) {
    static const char *allowed[] = {"index", "num", "den"};
    Args a;
    Doc *d;
    Layer *L;
    long num, den, v;
    int idx;
    parse_args(argc, argv, 2, &a, allowed, 3);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    L = lyr_composable(d, idx);
    num = parse_int(arg_req(&a, "num"), "num", 0, 65535);
    den = parse_int(arg_req(&a, "den"), "den", 1, 65535);
    /* rational scale, rounded half up, then clamped to 0..255 (E03 style) */
    v = (long)div_round((int64_t)L->opacity * num, den);
    L->opacity = (uint8_t)(v > 255 ? 255 : v);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 31. layer-blend-set                                                 */
/* ------------------------------------------------------------------ */

static int cmd_layer_blend_set(int argc, char **argv) {
    static const char *allowed[] = {"index", "blend"};
    Args a;
    Doc *d;
    Layer *L;
    int idx;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", 0, 0, MAX_LAYERS);
    L = lyr_composable(d, idx);
    L->blend = (uint8_t)parse_blend(arg_req(&a, "blend"));
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* 32. layer-lock-all                                                  */
/* ------------------------------------------------------------------ */

static int cmd_layer_lock_all(int argc, char **argv) {
    static const char *allowed[] = {"locked"};
    Args a;
    Doc *d;
    int locked, i;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    locked = (int)parse_int(arg_req(&a, "locked"), "locked", 0, 1);
    /* group_close records carry no attributes of their own and are left at flags 0 */
    for (i = 0; i < (int)d->nlayers; i++) {
        if (d->layers[i].kind == KIND_GROUP_CLOSE) continue;
        if (locked) d->layers[i].flags |= FLAG_LOCKED;
        else d->layers[i].flags = (uint8_t)(d->layers[i].flags & ~FLAG_LOCKED);
    }
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

#define FAM_LAYER_COMMANDS \
    {"layer-delete", cmd_layer_delete, "layer"}, \
    {"layer-duplicate", cmd_layer_duplicate, "layer"}, \
    {"layer-rename", cmd_layer_rename, "layer"}, \
    {"layer-set", cmd_layer_set, "layer"}, \
    {"layer-reorder", cmd_layer_reorder, "layer"}, \
    {"layer-group", cmd_layer_group, "layer"}, \
    {"layer-ungroup", cmd_layer_ungroup, "layer"}, \
    {"layer-merge-down", cmd_layer_merge_down, "layer"}, \
    {"layer-merge-visible", cmd_layer_merge_visible, "layer"}, \
    {"layer-mask-add", cmd_layer_mask_add, "layer"}, \
    {"layer-mask-apply", cmd_layer_mask_apply, "layer"}, \
    {"layer-mask-remove", cmd_layer_mask_remove, "layer"}, \
    {"layer-mask-invert", cmd_layer_mask_invert, "layer"}, \
    {"layer-fill", cmd_layer_fill, "layer"}, \
    {"layer-clear", cmd_layer_clear, "layer"}, \
    {"layer-offset", cmd_layer_offset, "layer"}, \
    {"layer-copy", cmd_layer_copy, "layer"}, \
    {"layer-swap", cmd_layer_swap, "layer"}, \
    {"layer-opacity-scale", cmd_layer_opacity_scale, "layer"}, \
    {"layer-blend-set", cmd_layer_blend_set, "layer"}, \
    {"layer-lock-all", cmd_layer_lock_all, "layer"},
