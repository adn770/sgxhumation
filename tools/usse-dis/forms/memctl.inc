/* forms/memctl.inc — the memctl family of USSE forms, included twice by usse.c:
 * with USSE_PART_DEFS for this family's operand kinds and field macros (name
 * them with a family prefix), and with USSE_PART_FORMS for its table entries.
 * Clean-room like the rest of the table: layouts come from probing the
 * refdisasm545 binary and are held to 0 wrong by random_oracle_check.py. */
#ifdef USSE_PART_DEFS

/* A sequence of small enumerations in one field, printed one after another:
 * the field's chunks, LSB first, are the subfields in print order, so a run of
 * flags and modes costs a single field. A NULL name marks an invalid value. */
typedef struct { uint8_t w; const char *const *t; } mem_sub;
typedef struct { const mem_sub *s; unsigned n; } mem_seq;

static int mem_k_seq_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    const mem_seq *q = k->arg;
    size_t at = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < q->n; i++) {
        const char *s = q->s[i].t[v & ((1u << q->s[i].w) - 1)];
        if (!s) return 0;
        at += (size_t)snprintf(out + at, at < n ? n - at : 0, "%s", s);
        v >>= q->s[i].w;
    }
    return at < n;
}

#define MEM_SEQ(name, ...) \
    static const mem_sub name##_S[] = { __VA_ARGS__ }; \
    static const mem_seq name##_Q = { name##_S, sizeof name##_S / sizeof name##_S[0] }; \
    static const usse_kind name = { mem_k_seq_print, NULL, &name##_Q }
#define MEM_SUB(w, ...) { w, (const char *const[1u << (w)]){ __VA_ARGS__ } }

/* ---- class 28: texture sampling ------------------------------------------
 * Probed by single-bit flips and field sweeps of random class-28 words: the
 * dimension enc[43:42] (1d, 2d, 3d) and the LOD mode enc[41:40] (none, bias,
 * replace, grad; dimension 3 prints invalid_smp_lod_mode); the flags skipinv
 * enc[55], syncs enc[52], nosched enc[54], PCF enc[38:37] and SRD enc[47:46];
 * the coordinate format enc[36:35]. The value 3 of PCF, SRD and the format is
 * invalid (invalid_smp_pcf_mode, _srd_mode, _data_type). */
MEM_SEQ(MEM_K_SMP0, MEM_SUB(2, "1d", "2d", "3d", NULL), MEM_SUB(2, "", NULL, NULL, NULL));
MEM_SEQ(MEM_K_SMPL, MEM_SUB(2, "1d", "2d", "3d", NULL),
        MEM_SUB(2, NULL, "bias", "replace", "grad"));
MEM_SEQ(MEM_K_SMPFLAGS, MEM_SUB(1, "", ".skipinv"), MEM_SUB(1, "", ".syncs"),
        MEM_SUB(1, "", ".nosched"), MEM_SUB(2, "", ".pcf", ".pcff16", NULL),
        MEM_SUB(2, "", ".sinf", ".rsd", NULL));
MEM_SEQ(MEM_K_SMPFMT, MEM_SUB(2, "", ".flt16", ".c10", NULL));

/* ---- class 29 with enc[54] set: eldd / eldq --------------------------------
 * Probed like the samplers: every bit of the word is read. The flags are
 * skipinv enc[55], syncs enc[52], the cache mode enc[33] | enc[53] << 1 and
 * the fetch count enc[47:44] (count + 1, nothing for 0). */
MEM_SEQ(MEM_K_ELDFLAGS, MEM_SUB(1, "", ".skipinv"), MEM_SUB(1, "", ".syncs"),
        MEM_SUB(2, "", ".bpcache", ".fcfill", ".bypassl1"),
        MEM_SUB(4, "", ".fetch2", ".fetch3", ".fetch4", ".fetch5", ".fetch6", ".fetch7",
                ".fetch8", ".fetch9", ".fetch10", ".fetch11", ".fetch12", ".fetch13",
                ".fetch14", ".fetch15", ".fetch16"));
ENUM(MEM_ELDTYPE, 2, "d", "q");

#endif
#ifdef USSE_PART_FORMS

    /* ---- class 28: smp1d/smp2d/smp3d[bias|replace|grad] -------------------- *
     * The destination is r or pa (enc[39]) with a 2-bit write mask enc[45:44],
     * or with enc[53] set a repeat count in the same bits. The coordinates are
     * a source-0 operand (bank enc[34] | enc[50] << 1) with their format, the
     * state a source-1 operand, and a LOD mode other than none adds its value
     * as a source-2 operand (whose bits are otherwise ignored). */
#define MEM_SMP(pat, v, op, cnt, ...) \
    { pat, CLSM | B(53), CLS(28) | (v), \
      { FLD(PRED_ALL, C(56, 3)), FLD(op, C(42, 2), C(40, 2)), \
        FLD(MEM_K_SMPFLAGS, C(55, 1), C(52, 1), C(54, 1), C(37, 2), C(46, 2)), cnt, \
        F_LSDST, FLD(K_SRC0, C(14, 7), C(34, 1), C(50, 1)), FLD(MEM_K_SMPFMT, C(35, 2)), \
        F_SRC1D, __VA_ARGS__ } }
    MEM_SMP("{0}smp{1}{2} {4}{3}, {5}{6}, {7}, drc{8}", 0, MEM_K_SMP0,
            FLD(WMASK, C(44, 2)), FLD(K_DEC, C(32, 2))),
    MEM_SMP("{0}smp{1}{2}{3} {4}, {5}{6}, {7}, drc{8}", B(53), MEM_K_SMP0,
            FLD(REPEAT, C(44, 2)), FLD(K_DEC, C(32, 2))),
    MEM_SMP("{0}smp{1}{2} {4}{3}, {5}{6}, {7}, {8}, drc{9}", 0, MEM_K_SMPL,
            FLD(WMASK, C(44, 2)), F_SRC2D, FLD(K_DEC, C(32, 2))),
    MEM_SMP("{0}smp{1}{2}{3} {4}, {5}{6}, {7}, {8}, drc{9}", B(53), MEM_K_SMPL,
            FLD(REPEAT, C(44, 2)), F_SRC2D, FLD(K_DEC, C(32, 2))),
#undef MEM_SMP

    /* ---- class 29 with enc[54] set: eldd / eldq ---------------------------- *
     * eld + d/q (enc[35]): an r/pa destination (enc[36]), then the base, a
     * source-1-like operand of the r, o, pa or sa bank (enc[31:30]), an r/pa
     * index (enc[34]) and an 11-bit immediate offset enc[43:37] | enc[51:48] << 7,
     * then a source-2-like operand of the same four banks (enc[29:28]) and a
     * 1-bit DRC. The SGX540 oracle reads these words as ld; the SGX540 corpus
     * has none. */
    { "{0}eld{1}{2} {3}, [{4}, {5}, #{6}], {7}, drc{8}", CLSM | B(54), CLS(29) | B(54),
      { FLD(PRED_ALL, C(56, 3)), FLD(MEM_ELDTYPE, C(35, 1)),
        FLD(MEM_K_ELDFLAGS, C(55, 1), C(52, 1), C(33, 1), C(53, 1), C(44, 4)),
        FLD(K_LSDST, C(21, 7), C(36, 1)), FLD(K_SRCD, C(7, 7), C(30, 2)),
        FLD(K_SRC0, C(14, 7), C(34, 1)), FLD(K_DEC, C(37, 7), C(48, 4)),
        FLD(K_SRCD, C(0, 7), C(28, 2)), FLD(K_DEC, C(32, 1)) } },

#endif
