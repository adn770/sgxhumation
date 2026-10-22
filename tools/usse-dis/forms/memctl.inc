/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Josep Torra */
/* forms/memctl.inc — the code kinds of the memctl family of USSE forms, included
 * by usse.c before the generated table; the family's data kinds and forms are
 * in isa/usse/memctl.toml (isa/README.md). Clean-room like the rest of the table:
 * layouts come from probing the refdisasm545 binary and are held to 0 wrong by
 * random_oracle_check.py. */

/* Signed decimal of arg's width (a usse_num whose mul is the width). */
static int mem_k_snum_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    unsigned w = ((const usse_num *)k->arg)->mul;
    int32_t x = (v & 1u << (w - 1)) ? (int32_t)v - (int32_t)(1u << w) : (int32_t)v;
    snprintf(out, n, "%d", (int)x);
    return 1;
}

static int mem_parse_int(const char *s, const char **end, int64_t *x) {
    uint64_t u;
    int neg = *s == '-';
    if (!usse_parse_uint(s + neg, end, &u)) return 0;
    *x = neg ? -(int64_t)u : (int64_t)u;
    return 1;
}

static int mem_k_snum_parse(const usse_kind *k, const char *s, const char **end, uint32_t *v) {
    unsigned w = ((const usse_num *)k->arg)->mul;
    int64_t x;
    if (!mem_parse_int(s, end, &x) || x < -(1ll << (w - 1)) || x >= 1ll << (w - 1)) return 0;
    *v = (uint32_t)x & ((1u << w) - 1);
    return 1;
}

static const usse_num MEM_W10 = { NULL, 10, 0 };
static const usse_kind MEM_K_S10 = { mem_k_snum_print, mem_k_snum_parse, &MEM_W10 };

/* imo's 12-bit immediates print as unsigned while bit 6 is clear and as the
 * sign-extended 7-bit value -64..-1 when it is set (the bits above it do not
 * show then; the encoder writes them as 0). */
static int mem_k_imo_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    if (v & 0x40u) snprintf(out, n, "%d", (int)(v & 0x7fu) - 128);
    else           snprintf(out, n, "%u", v);
    return 1;
}

static int mem_k_imo_parse(const usse_kind *k, const char *s, const char **end, uint32_t *v) {
    int64_t x;
    (void)k;
    if (!mem_parse_int(s, end, &x) || x < -64 || x >= 4096 || (x >= 0 && (x & 0x40))) return 0;
    *v = x < 0 ? (uint32_t)(x + 128) : (uint32_t)x;
    return 1;
}
static const usse_kind MEM_K_IMO = { mem_k_imo_print, mem_k_imo_parse, NULL };

/* smlsi operand: v = value[7:0] | swizzle mode[8]: a signed increment, or
 * four 2-bit components, x first in the low bits. */
static int mem_k_smlsi_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    static const char XYZW[] = "xyzw";
    (void)k;
    if (v >> 8)
        snprintf(out, n, "swizzle(%c%c%c%c)", XYZW[v & 3], XYZW[v >> 2 & 3], XYZW[v >> 4 & 3],
                 XYZW[v >> 6 & 3]);
    else
        snprintf(out, n, "#%d", (v & 0x80) ? (int)v - 256 : (int)v);
    return 1;
}
static const usse_kind MEM_K_SMLSI = { mem_k_smlsi_print, NULL, NULL };
