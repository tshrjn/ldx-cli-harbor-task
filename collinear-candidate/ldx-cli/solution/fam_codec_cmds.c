/* X1 container codecs -- the commands.  The engine is in fam_codec.c, which is
 * included before ldx_read/ldx_write because those call into it. */

/* ------------------------------------------------------------------ */
/* the five commands                                                   */
/* ------------------------------------------------------------------ */

static int cmd_doc_recompress(int argc, char **argv) {
    static const char *allowed[] = {"compress", "index"};
    Args a;
    Doc *d;
    int want, idx;
    parse_args(argc, argv, 2, &a, allowed, 2);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    want = codec_from_name(arg_req(&a, "compress"));
    idx = (int)arg_int(&a, "index", -1, -1, 65535);
    if (idx >= (int)d->nlayers) die(EXIT_USAGE, "E_BAD_ARGS", "--index out of range");
    codec_apply(d, want, idx);
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

static int cmd_doc_repack(int argc, char **argv) {
    static const char *allowed[] = {"compress"};
    Args a;
    Doc *d;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 2);
    d = ldx_read(a.pos[0]);
    /* re-lay the records without changing any codec: a pure rewrite */
    ldx_write(d, a.pos[1]);
    return EXIT_OK;
}

static int cmd_doc_stat_size(int argc, char **argv) {
    static const char *allowed[] = {"index"};
    Args a;
    Doc *d;
    uint16_t i;
    int idx, first = 1;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 1);
    d = ldx_read(a.pos[0]);
    idx = (int)arg_int(&a, "index", -1, -1, 65535);
    if (idx >= (int)d->nlayers) die(EXIT_USAGE, "E_BAD_ARGS", "--index out of range");
    printf("{\"records\":[");
    for (i = 0; i < d->nlayers; i++) {
        const Layer *L = &d->layers[i];
        uint32_t raw = layer_expected_len(d, L->kind);
        if (idx >= 0 && i != (uint16_t)idx) continue;
        printf("%s{\"index\":%u,\"codec\":\"%s\",\"raw\":%u,\"stored\":%u}",
               first ? "" : ",", (unsigned)i, codec_name(L->codec), raw, L->stored_len);
        first = 0;
    }
    printf("]}\n");
    return EXIT_OK;
}

static int cmd_doc_verify(int argc, char **argv) {
    static const char *allowed[] = {"strict"};
    Args a;
    Doc *d;
    uint16_t i;
    int faults = 0;
    parse_args(argc, argv, 2, &a, allowed, 1);
    positional(&a, 1);
    d = ldx_read(a.pos[0]);   /* any structural fault already exits with E_BAD_FILE */
    for (i = 0; i < d->nlayers; i++) {
        const Layer *L = &d->layers[i];
        size_t n = 0;
        uint8_t *re;
        if (layer_expected_len(d, L->kind) == 0) continue;
        /* the stored form must be exactly what this encoder would produce */
        re = codec_encode(L->codec, L->data, L->data_len, codec_stride(d, L->kind),
                          codec_bpp(d, L->kind), &n);
        if (n != L->stored_len) faults++;
        free(re);
    }
    printf("{\"records\":%u,\"codec_faults\":%d,\"ok\":%s}\n",
           (unsigned)d->nlayers, faults, faults ? "false" : "true");
    if (faults && arg_get(&a, "strict")) die(EXIT_BAD_FILE, "E_BAD_FILE", "record not canonically encoded");
    return EXIT_OK;
}

#define FAM_CODEC_COMMANDS \
    {"doc-recompress", cmd_doc_recompress, "codec"}, \
    {"doc-repack", cmd_doc_repack, "codec"}, \
    {"doc-stat-size", cmd_doc_stat_size, "codec"}, \
    {"doc-verify", cmd_doc_verify, "codec"},
