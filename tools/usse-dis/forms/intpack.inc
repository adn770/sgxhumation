/* forms/intpack.inc — the intpack family of USSE forms, included twice by usse.c:
 * with USSE_PART_DEFS for this family's operand kinds and field macros (name
 * them with a family prefix), and with USSE_PART_FORMS for its table entries.
 * Clean-room like the rest of the table: layouts come from probing the
 * refdisasm545 binary and are held to 0 wrong by random_oracle_check.py. */
#ifdef USSE_PART_DEFS

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

/* ---- class 8: pack / unpack ----------------------------------------------
 * enc[43:38] names the destination and source formats together (value 8 is
 * statomic, whose component selects print nothing for 0). enc[37:34] is the destination byte mask
 * (all four bytes print nothing), enc[20:19] the rounding mode, enc[18]
 * .scale, enc[17:16] and enc[15:14] the source components. */
ENUM(INT_PCKFMT, 64,
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
     "unpckc10c10");
ENUM(INT_BYTEMASK, 16, ".bytemask0000", ".bytemask0001", ".bytemask0010", ".bytemask0011",
     ".bytemask0100", ".bytemask0101", ".bytemask0110", ".bytemask0111", ".bytemask1000",
     ".bytemask1001", ".bytemask1010", ".bytemask1011", ".bytemask1100", ".bytemask1101",
     ".bytemask1110", "");
ENUM(INT_ROUND, 4, "nearest", "down", "up", "zero");

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
static const char *const INT_STATOMIC_T[64] = { [8] = "statomic" };
static const usse_kind K_INT_PCKOP = { k_int_pckop_print, NULL, INT_PCKFMT_T };
static const usse_kind K_INT_STATOMIC = { k_int_pckop_print, NULL, INT_STATOMIC_T };
#define INT_PCKOP(k) FLD(k, C(38, 6), C(55, 1), C(52, 1), C(54, 1), C(50, 1), C(18, 1))
#define INT_S1C(k) FLD(k, C(7, 7), C(30, 2), C(49, 1), C(16, 2))
#define INT_S2C(k) FLD(k, C(0, 7), C(28, 2), C(48, 1), C(14, 2))

#endif
#ifdef USSE_PART_FORMS

    /* ---- class 8: pack / unpack, statomic -------------------------------- */
#define INT_PCK(op, srcc) \
    { "{0}{1} {3}{4}{2}, {5}, {6}, {7}", CLSM | B(53), CLS(8), \
      { FLD(PRED_ALL, C(56, 3)), INT_PCKOP(op), FLD(WMASK, C(44, 4)), F_DST, \
        FLD(INT_BYTEMASK, C(34, 4)), INT_S1C(srcc), INT_S2C(srcc), \
        FLD(INT_ROUND, C(19, 2)) } }, \
    { "{0}{1}{2} {3}{4}, {5}, {6}, {7}", CLSM | B(53), CLS(8) | B(53), \
      { FLD(PRED_ALL, C(56, 3)), INT_PCKOP(op), FLD(REPEAT, C(44, 4)), F_DST, \
        FLD(INT_BYTEMASK, C(34, 4)), INT_S1C(srcc), INT_S2C(srcc), \
        FLD(INT_ROUND, C(19, 2)) } }
    INT_PCK(K_INT_PCKOP, K_INT_SRCC),
    INT_PCK(K_INT_STATOMIC, K_INT_SRCC0),
#undef INT_PCK

#endif
