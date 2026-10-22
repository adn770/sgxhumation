/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Josep Torra */
/* forms/intpack.inc — the code kinds of the intpack family of USSE forms, included
 * by usse.c before the generated table; the family's data kinds and forms are
 * in isa/usse/intpack.toml (isa/README.md). Clean-room like the rest of the table:
 * layouts come from probing the refdisasm545 binary and are held to 0 wrong by
 * random_oracle_check.py. */

/* A source operand with a component (byte or half) select after it:
 * v = operand[9:0] | select[11:10]. With a non-NULL arg, select 0 prints
 * nothing. */
static int k_int_srcc_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    char op[48];
    if (!k_src_print(&K_SRCD, v & 0x3ff, op, sizeof op)) return 0;
    if (k->arg && !(v >> 10)) snprintf(out, n, "%s", op);
    else                      snprintf(out, n, "%s.%u", op, v >> 10 & 3);
    return 1;
}
static const usse_kind K_INT_SRCC = { k_int_srcc_print, NULL, NULL };
static const usse_kind K_INT_SRCC0 = { k_int_srcc_print, NULL, "" };

/* The operation and its flags: v = format[5:0] | skipinv | syncs << 1 |
 * nosched << 2 | end << 3 | enc[18] << 4, which reads .renable for the
 * float-to-float conversions and .scale for the others. */
static int k_int_pckop_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    static const char *const F[] = { ".skipinv", ".syncs", ".nosched", ".end" };
    const char *const *name = k->arg;
    unsigned fmt = v & 63;
    size_t at;
    if (!name[fmt]) return 0;
    at = (size_t)snprintf(out, n, "%s", name[fmt]);
    for (unsigned i = 0; i < 4; i++)
        if (v >> (6 + i) & 1) at += (size_t)snprintf(out + at, at < n ? n - at : 0, "%s", F[i]);
    if (v >> 10 & 1) {
        int ff = fmt == 45 || fmt == 46 || fmt == 53 || fmt == 54;
        at += (size_t)snprintf(out + at, at < n ? n - at : 0, "%s", ff ? ".renable" : ".scale");
    }
    return at < n;
}
/* The formats by enc[43:38]: destination and source together. */
static const char *const INT_PCKFMT_T[64] = {
     "unpcku8u8", "unpcku8s8", NULL, "unpcku16u8", "unpcks16u8", "unpckf16u8", "unpckf32u8",
     NULL, NULL, NULL, NULL, "unpcku16s8", "unpcks16s8", "unpckf16s8", "unpckf32s8", NULL,
     NULL, NULL, NULL, "unpcku16o8", "unpcks16o8", "unpckf16o8", "unpckf32o8", NULL,
     "pcku8u16", "pcks8u16", "pcko8u16", "pcku16u16", "pcks16u16", "pckf16u16",
     "unpckf32u16", "pckc10u16",
     "pcku8s16", "pcks8s16", "pcko8s16", "pcku16s16", "pcks16s16", "pckf16s16",
     "unpckf32s16", "pckc10s16",
     "pcku8f16", "pcks8f16", "pcko8f16", "pcku16f16", "pcks16f16", "pckf16f16",
     "unpckf32f16", "pckc10f16",
     "pcku8f32", "pcks8f32", "pcko8f32", "pcku16f32", "pcks16f32", "pckf16f32",
     "unpckf32f32", "pckc10f32",
     NULL, NULL, NULL, "unpcku16c10", "unpcks16c10", "unpckf16c10", "unpckf32c10",
     "unpckc10c10" };
static const char *const INT_STATOMIC_T[64] = { [8] = "statomic" };
static const usse_kind K_INT_PCKOP = { k_int_pckop_print, NULL, INT_PCKFMT_T };
static const usse_kind K_INT_STATOMIC = { k_int_pckop_print, NULL, INT_STATOMIC_T };

/* ---- classes 16-19, 24: sop2, sop3, lrp, sop2wm, ima8, dot products -------
 * These name registers with six bits and a seventh, .c10, after the name:
 * temporaries r0-r59 then i0-i3, indexed [5:4] bank, [3] il/ih, [2:0]
 * offset. Constants, immediates and i.l/i.h keep all seven bits. arg[0] is
 * the bank numbering: that of K_DST ('d'), K_SRCD ('s') or a source 0 of r
 * and pa ('0'). arg[1], if any, names a modifier bit above the operand
 * (bit 8 for a source 0, 10 otherwise), printed before .c10: 'n' .neg,
 * 'c' .comp. */
static int k_int_c10_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    static const char *const D[8] = { NULL, "o", "pa", NULL, "sa", NULL, NULL, "i" };
    static const char *const S[8] = { NULL, "o", "pa", "sa", NULL, NULL, NULL, "i" };
    static const char *const IB[4] = { "r", "o", "pa", "sa" };
    static const char *const Z[8] = { NULL, "pa" };
    const char *arg = k->arg;
    char kind = arg[0], base[48];
    int dst = kind == 'd';
    unsigned nb = kind == '0' ? 8 : 10, op = v & ((1u << nb) - 1), mod = v >> nb & 1;
    unsigned b = op >> 7, r = op & 63, c10 = op >> 6 & 1;
    const char *name = (dst ? D : kind == 's' ? S : Z)[b];
    if (kind != '0' && (b == 5 || b == 6)) {
        if (dst) k_dst_print(k, op, base, sizeof base);
        else     k_src_print(&K_SRCD, op, base, sizeof base);
        c10 = 0;
    } else if (b == 0) {
        snprintf(base, sizeof base, r < 60 ? "r%u" : "i%u", r < 60 ? r : r - 60);
    } else if (kind != '0' && b == (dst ? 3u : 4u)) {
        snprintf(base, sizeof base, "%s[%s + #%u]", IB[r >> 4], (r & 8) ? "ih" : "il", r & 7);
    } else {
        snprintf(base, sizeof base, "%s%u", name, r);
    }
    snprintf(out, n, "%s%s%s", base, mod ? (arg[1] == 'n' ? ".neg" : ".comp") : "",
             c10 ? ".c10" : "");
    return 1;
}
static const usse_kind K_INT_D10 = { k_int_c10_print, NULL, "d" };
static const usse_kind K_INT_S10 = { k_int_c10_print, NULL, "s" };

static const usse_kind K_INT_S0_10 = { k_int_c10_print, NULL, "0" };

/* ---- integer multiply-add: ima8 (19), ima16 (20), imae (21), ima32/idiv (26)
 * The older forms in usse.c decode the plain formats; these take the rest. */

/* A source with a suffix: v = operand | neg << nb | low/high << (nb + 1),
 * where the operand (nb bits) is a source 0 (arg "0") or source 1/2 (arg
 * "s"); arg[1] says which suffixes exist ('n' neg, 'l' low/high, 'b' both). */
static int k_int_srcx_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    const char *arg = k->arg;
    unsigned nb = arg[0] == '0' ? 8 : 10;
    int hasneg = arg[1] == 'n' || arg[1] == 'b', haslh = arg[1] == 'l' || arg[1] == 'b';
    uint32_t op = v & ((1u << nb) - 1), rest = v >> nb;
    unsigned neg = hasneg ? rest & 1 : 0, lh = haslh ? rest >> hasneg & 1 : 0;
    char buf[48];
    if (arg[0] == '0') k_src0_print(&K_SRC0, op, buf, sizeof buf);
    else               k_src_print(&K_SRCD, op, buf, sizeof buf);
    snprintf(out, n, "%s%s%s", buf, neg ? ".neg" : "", haslh ? (lh ? ".high" : ".low") : "");
    return 1;
}
static const usse_kind K_INT_S0LH = { k_int_srcx_print, NULL, "0l" };
static const usse_kind K_INT_SLH  = { k_int_srcx_print, NULL, "sl" };
static const usse_kind K_INT_SNEG = { k_int_srcx_print, NULL, "sn" };
static const usse_kind K_INT_SNLH = { k_int_srcx_print, NULL, "sb" };

/* ima16 source formats: v = format 1 [1:0] | format 2 [3:2]; arg bit i set
 * means format i+1 is an 8-bit one (u8, s8, o8), clear that it is u16. */
static int k_int_fmt16_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    static const char *const F[4] = { "u16", "u8", "s8", "o8" };
    unsigned want = *(const uint8_t *)k->arg, f1 = v & 3, f2 = v >> 2 & 3;
    if ((f1 != 0) != (want & 1) || (f2 != 0) != (want >> 1 & 1)) return 0;
    snprintf(out, n, "%s, %s", F[f1], F[f2]);
    return 1;
}
static const uint8_t INT_FMT16_W[4] = { 0, 1, 2, 3 };
static const usse_kind K_INT_FMT16_0 = { k_int_fmt16_print, NULL, &INT_FMT16_W[0] };
static const usse_kind K_INT_FMT16_1 = { k_int_fmt16_print, NULL, &INT_FMT16_W[1] };
static const usse_kind K_INT_FMT16_2 = { k_int_fmt16_print, NULL, &INT_FMT16_W[2] };
static const usse_kind K_INT_FMT16_3 = { k_int_fmt16_print, NULL, &INT_FMT16_W[3] };

/* imae source-2 type and carries: v = type[1:0] | coe | cie | register; the
 * carry register prints when either carry is used. */
static int k_int_imaec_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    static const char *const T[4] = { "z16", "u16", "u32", NULL };
    char reg[8] = "";
    (void)k;
    if (!T[v & 3]) return 0;
    if (v & 12) snprintf(reg, sizeof reg, ", i%u", v >> 4 & 1);
    snprintf(out, n, "%s%s%s%s", T[v & 3], (v & 4) ? ", coe" : "", (v & 8) ? ", cie" : "", reg);
    return 1;
}
static const usse_kind K_INT_IMAEC = { k_int_imaec_print, NULL, NULL };

/* ima32 carry-in: 0 none, else register i(v-1). */
static int k_int_cie32_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    if (v) snprintf(out, n, ", cie, i%u", v - 1);
    else   out[0] = '\0';
    return 1;
}
static const usse_kind K_INT_CIE32 = { k_int_cie32_print, NULL, NULL };
/* ima32 extra operand after the destination: v = present | register << 1. */
static int k_int_ima32x_print(const usse_kind *k, uint32_t v, char *out, size_t n) {
    (void)k;
    if (v & 1) snprintf(out, n, ", i%u", v >> 1);
    else       out[0] = '\0';
    return 1;
}
static const usse_kind K_INT_IMA32X = { k_int_ima32x_print, NULL, NULL };

static const usse_kind K_INT_S0N_10 = { k_int_c10_print, NULL, "0n" };
static const usse_kind K_INT_SC10 = { k_int_c10_print, NULL, "sc" };

