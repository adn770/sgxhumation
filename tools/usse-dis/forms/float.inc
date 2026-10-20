/* forms/float.inc — the float family of USSE forms, included twice by usse.c:
 * with USSE_PART_DEFS for this family's operand kinds and field macros (name
 * them with a family prefix), and with USSE_PART_FORMS for its table entries.
 * Clean-room like the rest of the table: layouts come from probing the
 * refdisasm545 binary and are held to 0 wrong by random_oracle_check.py. */
#ifdef USSE_PART_DEFS
#include <string.h>

/* ==== float operands =======================================================
 * A float source is a register operand plus .neg/.abs and, with the
 * instruction's f16 bit (enc[54] in the multiply-add group) set, a half-float
 * reading of the register number: numbers 64-127 then name a half of a
 * 32-bit register, (n - 64) >> 1, printed with a component suffix chosen by
 * n & 1 (".flt16.0"/".flt16.2", or ".flt16"/".flt16.2" for fsubflr). The
 * temporaries reach r0-r29 and then i0-i1. Indexed operands read the f16 bit
 * too: the bank is n[5:4], il/ih n[3] and the offset n[2:0], with the
 * first-component suffix when n[6] is set. Constants and immediates are not
 * affected. Swept bank by bank and number by number through refdisasm545. */
typedef struct {
    const char *imm;          /* format of the immediate bank */
    int f16;                  /* the half-float reading */
    const char *comp[2];      /* its component suffixes */
} flt_srcarg;

static size_t flt_put(char *out, size_t n, size_t at, const char *s) {
    int k = snprintf(out + (at < n ? at : n), at < n ? n - at : 0, "%s", s);
    return at + (size_t)k;
}

/* bank: the source-1/2 bank code (0 r, 1 o, 2 pa, 3 sa, 4 indexed, 5 c/g,
 * 6 immediate, 7 i). Return 1 when the text fits. */
static int flt_src(const flt_srcarg *a, unsigned bank, unsigned r, int neg, int abs,
                   char *out, size_t n) {
    int f16 = a->f16;
    static const char *const NAME[8] = { "r", "o", "pa", "sa", NULL, NULL, NULL, "i" };
    static const char *const IDX[4] = { "r", "o", "pa", "sa" };
    const char *suffix = "";
    int at;
    if (f16 && bank == 0 && r >= 60 && r < 64) {
        at = snprintf(out, n, "i%u", r - 60);
    } else if (!f16 || bank == 5 || bank == 6 || (bank != 4 && r < 64)) {
        if (bank == 6) at = snprintf(out, n, a->imm, r);
        else           { k_src_print(&K_SRCD, r | bank << 7, out, n); at = (int)strlen(out); }
    } else if (bank == 4) {
        at = snprintf(out, n, "%s[%s + #%u]", IDX[r >> 4 & 3], (r & 8) ? "ih" : "il", r & 7);
        if (r >= 64) suffix = a->comp[0];
    } else {
        unsigned m = (r - 64) >> 1;
        if (bank == 0 && m >= 30) at = snprintf(out, n, "i%u", m - 30);
        else                      at = snprintf(out, n, "%s%u", NAME[bank], m);
        suffix = a->comp[r & 1];
    }
    if (at < 0) return 0;
    size_t p = (size_t)at;
    if (neg) p = flt_put(out, n, p, ".neg");
    if (abs) p = flt_put(out, n, p, ".abs");
    p = flt_put(out, n, p, suffix);
    return p < n;
}

/* The f16 bit is fixed by each form (the encoder cannot share a bit between
 * fields), so a kind is either the plain or the half-float reading. */

/* Source 1 or 2: v = number[6:0] | bank[9:7] | neg[10] | abs[11]. */
static int k_flt_src_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    return flt_src(k->arg, v >> 7 & 7, v & 0x7f, v >> 10 & 1, v >> 11 & 1, out, n);
}

/* Source 0: v = number[6:0] | pa[7] | abs[8]. */
static int k_flt_src0_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    return flt_src(k->arg, (v >> 7 & 1) ? 2 : 0, v & 0x7f, 0, v >> 8 & 1, out, n);
}

/* .incsXYZ: v = off[0] (enc[53], set: nothing printed) | enc[40] << 1 |
 * enc[46] << 2 | enc[47] << 3, printed as enc[47] enc[46] enc[40]. */
static int k_flt_incs_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    if (v & 1) out[0] = '\0';
    else       snprintf(out, n, ".incs%u%u%u", v >> 3 & 1, v >> 2 & 1, v >> 1 & 1);
    return 1;
}

static const flt_srcarg FLT_ARG_32  = { "#%u", 0, { "", "" } };
static const flt_srcarg FLT_ARG_MAD = { "#%u", 1, { ".flt16.0", ".flt16.2" } };
static const flt_srcarg FLT_ARG_FLR = { "#%u", 1, { ".flt16", ".flt16.2" } };
static const usse_kind K_FLT_SRC     = { k_flt_src_print, NULL, &FLT_ARG_32 };
static const usse_kind K_FLT_SRC0    = { k_flt_src0_print, NULL, &FLT_ARG_32 };
static const usse_kind K_FLT_SRC_MAD16  = { k_flt_src_print, NULL, &FLT_ARG_MAD };
static const usse_kind K_FLT_SRC0_MAD16 = { k_flt_src0_print, NULL, &FLT_ARG_MAD };
static const usse_kind K_FLT_SRC_FLR16  = { k_flt_src_print, NULL, &FLT_ARG_FLR };
static const usse_kind K_FLT_SRC0_FLR16 = { k_flt_src0_print, NULL, &FLT_ARG_FLR };
static const usse_kind K_FLT_INCS = { k_flt_incs_print, NULL, NULL };
static const char *const FLT_FLAGS_T[] = { ".skipinv", ".syncs", ".nosched", ".end" };
/* The flags of every float form. The SGX540 reads these classes differently
 * (a write mask, source-0 negation, no .incs), so the float forms are the
 * SGX545's only: on the 540 the flags refuse to print and the word stays
 * undecoded. */
static int k_flt_flags_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    return core != 540 && k_flags_print(k, v, out, n);
}
static const usse_kind K_FLT_FLAGS = { k_flt_flags_print, NULL, FLT_FLAGS_T };

ENUM(FLT_OP_MAD, 4, "fmad", "fadm", "fmsa", NULL);          /* enc[42:41] */
ENUM(FLT_OP_MINMAX, 4, "fmaxmin", "fminmax", NULL, NULL);
ENUM(FLT_OP_DS, 4, "fdsx", "fdsy", NULL, NULL);
ENUM(FLT_REPEAT4, 4, "", ".repeat2", ".repeat3", ".repeat4");

#define F_FLT_FLAGS  FLD(K_FLT_FLAGS, C(55, 1), C(52, 1), C(43, 1), C(50, 1))
#define F_FLT_REP2   FLD(FLT_REPEAT4, C(44, 2))
#define F_FLT_INCS   FLD(K_FLT_INCS, C(53, 1), C(40, 1), C(46, 2))
#define F_FLT_SRC0(k) FLD(k, C(14, 7), C(34, 1), C(39, 1))
#define F_FLT_SRC1(k) FLD(k, C(7, 7), C(30, 2), C(49, 1), C(37, 1), C(38, 1))
#define F_FLT_SRC2(k) FLD(k, C(0, 7), C(28, 2), C(48, 1), C(35, 1), C(36, 1))
/* ... the same operands without the modifier bits (which the kinds read as 0) */
#define F_FLT_SRC0P(k) FLD(k, C(14, 7), C(34, 1))
#define F_FLT_SRC1P(k) FLD(k, C(7, 7), C(30, 2), C(49, 1))
#define F_FLT_SRC2P(k) FLD(k, C(0, 7), C(28, 2), C(48, 1))
/* a destination without the bank extension bit (enc[51] used otherwise) */
#define F_FLT_DST9   FLD(K_DST, C(21, 7), C(32, 2))

/* ==== class 1: single-source transcendentals ================================
 * The source modifiers: v = neg[0] | abs[1] | data type[3:2] | component[5:4]
 * (enc[37], enc[38], enc[40:39], enc[36:35]). Type 1 is a half float, its
 * component enc[35] printed as 0 or 2; type 2 a c10 value with the 2-bit
 * component; type 3 is invalid. */
static int k_flt_mods1_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    unsigned t = v >> 2 & 3, c = v >> 4 & 3;
    (void)k;
    if (t == 3) return 0;
    size_t p = flt_put(out, n, 0, (v & 1) ? ".neg" : "");
    if (v & 2) p = flt_put(out, n, p, ".abs");
    if (t == 1) p += (size_t)snprintf(out + p, p < n ? n - p : 0, ".flt16.%u", (c & 1) * 2);
    if (t == 2) p += (size_t)snprintf(out + p, p < n ? n - p : 0, ".c10.%u", c);
    return p < n;
}
static const usse_kind K_FLT_MODS1 = { k_flt_mods1_print, NULL, NULL };
static const char *const FLT_FLAGS1_T[] = { ".skipinv", ".syncs", ".nosched", ".end", ".tpres" };
static const usse_kind K_FLT_FLAGS1 = { k_flt_flags_print, NULL, FLT_FLAGS1_T };
ENUM(FLT_OP_SC, 8, "frcp", "frsq", "flog", "fexp", "fsqrt", "fsin", "fcos", NULL);
#define F_FLT_FLAGS1 FLD(K_FLT_FLAGS1, C(55, 1), C(52, 1), C(43, 1), C(50, 1), C(34, 1))
#define F_FLT_MODS1  FLD(K_FLT_MODS1, C(37, 2), C(39, 2), C(35, 2))

#endif
#ifdef USSE_PART_FORMS
    /* ---- class 0: the float multiply-add group ---------------------------- *
     * fmad/fadm/fmsa (enc[42:41] 0-2) read three sources, fsubflr (3) two.
     * enc[54] reads the sources as half floats; enc[53] clear adds .incs. */
#define FLT_MAD(f16, s0, s, sf) \
    { "{0}{1}{2}{3}{4} {5}, {6}, {7}, {8}", CLSM | B(54), CLS(0) | (f16), \
      { FLD(PRED_ALL, C(56, 3)), FLD(FLT_OP_MAD, C(41, 2)), F_FLT_FLAGS, F_FLT_REP2, \
        F_FLT_INCS, F_DST, F_FLT_SRC0(s0), F_FLT_SRC1(s), F_FLT_SRC2(s) }, B(53) }, \
    { "{0}fsubflr{1}{2}{3} {4}, {5}, {6}", CLSM | B(54) | M(42, 41), CLS(0) | (f16) | M(42, 41), \
      { FLD(PRED_ALL, C(56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_DST, \
        F_FLT_SRC1(sf), F_FLT_SRC2(sf) }, B(53) }
    FLT_MAD(0, K_FLT_SRC0, K_FLT_SRC, K_FLT_SRC),
    FLT_MAD(B(54), K_FLT_SRC0_MAD16, K_FLT_SRC_MAD16, K_FLT_SRC_FLR16),
#undef FLT_MAD
    /* ---- class 1: frcp frsq flog fexp fsqrt fsin fcos --------------------- *
     * The operation is enc[42:41] | enc[48] << 2; source 1 is the operand.
     * enc[53] clear: a destination write mask in enc[47:44]; set: a repeat
     * count there. */
    { "{0}{1}{2} {3}{4}, {5}{6}", CLSM | B(53), CLS(1),
      { FLD(PRED_ALL, C(56, 3)), FLD(FLT_OP_SC, C(41, 2), C(48, 1)), F_FLT_FLAGS1, F_DST,
        FLD(WMASK, C(44, 4)), F_SRC1D, F_FLT_MODS1 } },
    { "{0}{1}{2}{3} {4}, {5}{6}", CLSM | B(53), CLS(1) | B(53),
      { FLD(PRED_ALL, C(56, 3)), FLD(FLT_OP_SC, C(41, 2), C(48, 1)), F_FLT_FLAGS1,
        FLD(REPEAT, C(44, 4)), F_DST, F_SRC1D, F_FLT_MODS1 } },
    /* ---- class 2: the dot products ----------------------------------------- *
     * enc[42:41]: fdp (0; enc[14] set makes it fdpc, with a cp operand in
     * enc[17:15]), fddp (1), fddpc (2); 3 is invalid. fddp/fddpc name an
     * internal register i<enc[51]> and so lose the destination's bank
     * extension; fddpc's two cp operands take the source modifier bits and
     * enc[40], and it has no .incs. enc[54] reads the sources as half floats:
     * fdp prints the first component as .flt16.0, the others as .flt16. */
#define FLT_DP(f16, s0f, sm, sf) \
    { "{0}fdp{1}{2}{3} {4}, {5}, {6}", CLSM | M(42, 41) | B(54) | B(14), CLS(2) | (f16), \
      { FLD(PRED_ALL, C(56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_DST, F_FLT_SRC1(sm), \
        F_FLT_SRC2(sm) }, B(53) }, \
    { "{0}fdpc{1}{2}{3} {4}, cp{5}, {6}, {7}", CLSM | M(42, 41) | B(54) | B(14), \
      CLS(2) | (f16) | B(14), \
      { FLD(PRED_ALL, C(56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_DST, \
        FLD(K_DEC, C(15, 3)), F_FLT_SRC1(sf), F_FLT_SRC2(sf) }, B(53) }, \
    { "{0}fddp{1}{2}{3} {4}, i{5}, {6}, {7}, {8}", CLSM | M(42, 41) | B(54), \
      CLS(2) | (f16) | B(41), \
      { FLD(PRED_ALL, C(56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST9, \
        FLD(K_DEC, C(51, 1)), F_FLT_SRC0(s0f), F_FLT_SRC1(sf), F_FLT_SRC2(sf) }, B(53) }, \
    { "{0}fddpc{1}{2} {3}, i{4}, cp{5}, cp{6}, {7}, {8}, {9}", CLSM | M(42, 41) | B(54), \
      CLS(2) | (f16) | B(42), \
      { FLD(PRED_ALL, C(56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_DST9, FLD(K_DEC, C(51, 1)), \
        FLD(K_DEC, C(35, 3)), FLD(K_DEC, C(38, 3)), F_FLT_SRC0P(s0f), F_FLT_SRC1P(sf), \
        F_FLT_SRC2P(sf) } }
    FLT_DP(0, K_FLT_SRC0, K_FLT_SRC, K_FLT_SRC),
    FLT_DP(B(54), K_FLT_SRC0_FLR16, K_FLT_SRC_MAD16, K_FLT_SRC_FLR16),
#undef FLT_DP
    /* ---- class 3: fmaxmin / fminmax ---------------------------------------- *
     * The class-0 layout, with the operation in enc[41] (enc[42] invalid). */
    { "{0}{1}{2}{3}{4} {5}, {6}, {7}, {8}", CLSM | B(54), CLS(3),
      { FLD(PRED_ALL, C(56, 3)), FLD(FLT_OP_MINMAX, C(41, 2)), F_FLT_FLAGS, F_FLT_REP2,
        F_FLT_INCS, F_DST, F_FLT_SRC0(K_FLT_SRC0), F_FLT_SRC1(K_FLT_SRC),
        F_FLT_SRC2(K_FLT_SRC) }, B(53) },
    { "{0}{1}{2}{3}{4} {5}, {6}, {7}, {8}", CLSM | B(54), CLS(3) | B(54),
      { FLD(PRED_ALL, C(56, 3)), FLD(FLT_OP_MINMAX, C(41, 2)), F_FLT_FLAGS, F_FLT_REP2,
        F_FLT_INCS, F_DST, F_FLT_SRC0(K_FLT_SRC0_MAD16), F_FLT_SRC1(K_FLT_SRC_MAD16),
        F_FLT_SRC2(K_FLT_SRC_MAD16) }, B(53) },
    /* ---- class 4: fdsx / fdsy ---------------------------------------------- *
     * Two sources (1 and 2) in the class-0 layout, the operation in enc[41]. */
#define FLT_DS(f16, s) \
    { "{0}{1}{2}{3}{4} {5}, {6}, {7}", CLSM | B(54), CLS(4) | (f16), \
      { FLD(PRED_ALL, C(56, 3)), FLD(FLT_OP_DS, C(41, 2)), F_FLT_FLAGS, F_FLT_REP2, \
        F_FLT_INCS, F_DST, F_FLT_SRC1(s), F_FLT_SRC2(s) }, B(53) }
    FLT_DS(0, K_FLT_SRC),
    FLT_DS(B(54), K_FLT_SRC_MAD16),
#undef FLT_DS
#endif
