/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Josep Torra */
/* forms/misc.inc — the code kinds of the misc family of USSE forms, included
 * by usse.c before the generated table; the family's data kinds and forms are
 * in isa/usse/misc.toml (isa/README.md). Clean-room like the rest of the table:
 * layouts come from probing the refdisasm545 binary and are held to 0 wrong by
 * random_oracle_check.py. */
#include <string.h>

/* ---- classes 9 and 15: the test group ---------------------------------- *
 * Class 9 tests the result into a predicate, class 15 into a mask (b, w or
 * d, enc[37:36]; enc[38] and the predicate number enc[35:34] are ignored).
 * usse.c decodes class 9 with enc[50] clear and the bitwise operations; the
 * forms here add enc[50] set for the others: .sat on the 8-bit integer
 * operations, nothing on the 16- and 32-bit ones, and a per-operand format on
 * the float operations: bit 6 of the operand number selects .flt16 (number
 * [5:1], component [0], printed .2 when set) or, for fpmul8/fpadd8/fpsub8,
 * .c10 (number [5:0]). Constants and immediates keep their 7-bit numbers. */

/* Class 15 test: v = mask type[1:0] (enc[37:36]) | rest[6:2] (enc[43:39]:
 * all-or-any, zero test, sign test, as in K_TEST). */
static int k_msc_testmsk_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    static const char *const SIGN[4] = { "t", "n", "p", NULL };
    static const char *const ZERO[4] = { "t", "z", "nz", NULL };
    static const char *const MSK[4] = { "bmsk", "wmsk", "dmsk", NULL };
    (void)k;
    if (!SIGN[v >> 5] || !ZERO[v >> 3 & 3] || !MSK[v & 3]) return 0;
    snprintf(out, n, "test%s%s%s.%s", SIGN[v >> 5], (v & 4) ? "a" : "o", ZERO[v >> 3 & 3],
             MSK[v & 3]);
    return 1;
}
static const usse_kind K_MSC_TESTMSK = { k_msc_testmsk_print, NULL, NULL };

/* A float operand with enc[50] set: v = number[6:0] | bank[8:7] | ext[9],
 * banks numbered as for K_DST (dst) or K_SRCD. Temporaries past r59 (r29
 * for .flt16) print as i0-i3; an indexed operand takes a 6-bit index
 * ([5:4] bank, [3] il/ih, [2:0] offset) and no component. */
typedef struct { uint8_t dst, mode; } msc_fop;
enum { MSC_FLT16, MSC_FLT16_0, MSC_C10 };   /* MSC_FLT16_0: a source prints .0 */
static int k_msc_fop_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    static const char *const SB[8] = { "r", "o", "pa", "sa", "[", "c", "#", "i" };
    static const char *const DB[8] = { "r", "o", "pa", "[", "sa", "c", "i.", "i" };
    static const char *const IB[4] = { "r", "o", "pa", "sa" };
    const msc_fop *a = k->arg;
    const char *b = (a->dst ? DB : SB)[v >> 7];
    unsigned r = v & 0x7f, fmt = r >> 6, n6 = r & 63, h = fmt && a->mode != MSC_C10;
    unsigned num = h ? n6 >> 1 : n6, comp = h && (n6 & 1) && b[0] != '[', lim = h ? 30 : 60;
    int at;
    if (b[0] == 'c') return print_const(out, n, r);
    if (b[0] == '#') { snprintf(out, n, "#%u", r); return 1; }
    if (b[1] == '.') return k_dst_print(&K_DST, 6u << 7 | r, out, n);
    if (b[0] == '[')
        at = snprintf(out, n, "%s[%s + #%u]", IB[n6 >> 4], (n6 & 8) ? "ih" : "il", n6 & 7);
    else if (b[0] == 'r' && num >= lim)
        at = snprintf(out, n, "i%u", num - lim);
    else
        at = snprintf(out, n, "%s%u", b, num);
    if (!fmt || at < 0 || (size_t)at >= n) return 1;
    if (a->mode == MSC_C10) snprintf(out + at, n - (size_t)at, ".c10");
    else snprintf(out + at, n - (size_t)at, ".flt16%s",
                  comp ? ".2" : (!a->dst && a->mode == MSC_FLT16_0) ? ".0" : "");
    return 1;
}
static const msc_fop MSC_FOP_A[] = { { 1, MSC_FLT16 }, { 0, MSC_FLT16 }, { 0, MSC_FLT16_0 },
                                     { 1, MSC_C10 }, { 0, MSC_C10 } };
static const usse_kind K_MSC_FDST = { k_msc_fop_print, NULL, &MSC_FOP_A[0] };
static const usse_kind K_MSC_FSRC = { k_msc_fop_print, NULL, &MSC_FOP_A[1] };
static const usse_kind K_MSC_FSRC0 = { k_msc_fop_print, NULL, &MSC_FOP_A[2] };
static const usse_kind K_MSC_CDST = { k_msc_fop_print, NULL, &MSC_FOP_A[3] };
static const usse_kind K_MSC_CSRC = { k_msc_fop_print, NULL, &MSC_FOP_A[4] };

/* firh: v = edge mode[1:0] | filter coefficient set[4:2] */
static int k_msc_firh_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    static const char *const EDGE[4] = { "rep", "msc", "mdc", "reserved" };
    (void)k;
    snprintf(out, n, "%s, fcs%u", EDGE[v & 3], v >> 2);
    return 1;
}
static const usse_kind K_MSC_FIRH = { k_msc_firh_print, NULL, NULL };

/* A 5-bit two's-complement number. */
static int k_msc_s5_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    snprintf(out, n, "%d", (int)v - (v & 16 ? 32 : 0));
    return 1;
}
static const usse_kind K_MSC_S5 = { k_msc_s5_print, NULL, NULL };

/* firv/firh operands 2 and 3: v = number[6:0] (enc[20:14]) | source-1
 * number[13:7] | the shared bank[16:14], printed as two K_SRCD operands.
 * One field, so that the assembler cannot pick two different banks; it
 * parses the pair by searching the bank and both numbers. */
static int k_msc_pair_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    uint32_t bank = v >> 14 << 7;
    size_t at;
    (void)k;
    k_src_print(&K_SRCD, bank | (v & 0x7f), out, n);
    at = strlen(out);
    if (at + 2 >= n) return 0;
    out[at++] = ',';
    out[at++] = ' ';
    return k_src_print(&K_SRCD, bank | (v >> 7 & 0x7f), out + at, n - at);
}

/* Match printed operand x at s, blanks in either side being free; the
 * operand must end at a comma or the end. Return the end of the match. */
static const char *msc_operand(const char *x, const char *s) {
    for (;;) {
        while (*x == ' ') x++;
        while (*s == ' ' || *s == '\t') s++;
        if (!*x) return (*s == ',' || !*s) ? s : NULL;
        if (*s++ != *x++) return NULL;
    }
}

static int k_msc_pair_parse(const usse_kind *k, const char *s, const char **end, uint32_t *v) {
    char buf[32];
    (void)k;
    for (uint32_t bank = 0; bank < 8; bank++)
        for (uint32_t a = 0; a < 128; a++) {
            const char *q;
            k_src_print(&K_SRCD, bank << 7 | a, buf, sizeof buf);
            if (!(q = msc_operand(buf, s)) || *q != ',') continue;
            for (uint32_t b = 0; b < 128; b++) {
                const char *r;
                k_src_print(&K_SRCD, bank << 7 | b, buf, sizeof buf);
                if ((r = msc_operand(buf, q + 1))) {
                    *end = r;
                    *v = a | b << 7 | bank << 14;
                    return 1;
                }
            }
        }
    return 0;
}
static const usse_kind K_MSC_PAIR = { k_msc_pair_print, k_msc_pair_parse, NULL };
