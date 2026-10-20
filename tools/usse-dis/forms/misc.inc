/* forms/misc.inc — the misc family of USSE forms, included twice by usse.c:
 * with USSE_PART_DEFS for this family's operand kinds and field macros (name
 * them with a family prefix), and with USSE_PART_FORMS for its table entries.
 * Clean-room like the rest of the table: layouts come from probing the
 * refdisasm545 binary and are held to 0 wrong by random_oracle_check.py. */
#ifdef USSE_PART_DEFS

/* ---- class 5: movc / movmsk --------------------------------------------- *
 * enc[42:40] is the class-5 type: 0 the plain mov (usse.c), 1-5 movc with
 * data type i8, i16, i32, flt, i10, 6 movmsk and 7 movmsk.smsk (which adds
 * a source). enc[39] | enc[54] << 1 is the movc test and the movmsk format;
 * u8 is the one movmsk format printed after .smsk and the repeat count. */
ENUM(MSC_MOVC_T, 8, NULL, ".i8", ".i16", ".i32", ".flt", ".i10", NULL, NULL);
ENUM(MSC_MOVC_TR, 8, NULL, NULL, ".i16", ".i32", ".flt", ".i10", NULL, NULL);
ENUM(MSC_MOVC_TEST, 4, ".tz", ".tnz", ".tn", ".tp|z");
ENUM(MSC_MOVMSK_F, 4, ".mask", NULL, ".flt16", ".flt");
/* the component of movmsk's last source, enc[38:37] */
ENUM(MSC_COMP, 4, "", ".1", ".2", ".3");

#define MSC_F_SRC0 FLD(K_SRC0, C(14, 7), C(34, 1))
#define MSC_F_SRC2 F_SRC2D
static const char *const MSC_C5FLAGS_T[] = { ".skipinv", ".syncs", ".nosched", ".end" };
static const usse_kind K_MSC_C5FLAGS = { k_flags_print, NULL, MSC_C5FLAGS_T };
#define MSC_C5FLAGS FLD(K_MSC_C5FLAGS, C(55, 1), C(52, 1), C(43, 1), C(50, 1))

#endif
#ifdef USSE_PART_FORMS
    /* ---- class 5: movc, movmsk --------------------------------------------- */
    { "{0}movc{1}{2}{3} {4}{5}, {6}, {7}, {8}", CLSM | B(53), CLS(5),
      { FLD(PRED_ALL, C(56, 3)), MSC_C5FLAGS, FLD(MSC_MOVC_T, C(40, 3)),
        FLD(MSC_MOVC_TEST, C(39, 1), C(54, 1)), F_DST, FLD(WMASK, C(44, 4)), MSC_F_SRC0,
        F_SRC1D, MSC_F_SRC2 } },
    /* with a repeat count: it prints before i8 and after the other types */
    { "{0}movc{1}{2}{3}{4} {5}, {6}, {7}, {8}", CLSM | B(53), CLS(5) | B(53),
      { FLD(PRED_ALL, C(56, 3)), MSC_C5FLAGS, FLD(MSC_MOVC_TR, C(40, 3)),
        FLD(REPEAT, C(44, 4)), FLD(MSC_MOVC_TEST, C(39, 1), C(54, 1)), F_DST,
        MSC_F_SRC0, F_SRC1D, MSC_F_SRC2 } },
    { "{0}movc{1}{2}.i8{3} {4}, {5}, {6}, {7}", CLSM | B(53) | M(42, 40),
      CLS(5) | B(53) | B(40),
      { FLD(PRED_ALL, C(56, 3)), MSC_C5FLAGS, FLD(REPEAT, C(44, 4)),
        FLD(MSC_MOVC_TEST, C(39, 1), C(54, 1)), F_DST, MSC_F_SRC0, F_SRC1D, MSC_F_SRC2 } },
    /* movmsk: masked format, without and with .smsk (enc[40]) */
#define MSC_MOVMSK(pat, m, v, f, ...) \
    { pat, CLSM | M(42, 41) | (m), CLS(5) | M(42, 41) | (v), \
      { FLD(PRED_ALL, C(56, 3)), MSC_C5FLAGS, f, F_DST, __VA_ARGS__ } }
#define MSC_FMT FLD(MSC_MOVMSK_F, C(39, 1), C(54, 1))
#define MSC_LAST FLD(K_SRCD, C(0, 7), C(28, 2), C(48, 1)), FLD(MSC_COMP, C(37, 2))
    MSC_MOVMSK("{0}movmsk{1}{2} {3}{4}, {5}, {6}{7}", B(53) | B(40), 0, MSC_FMT,
               FLD(WMASK, C(44, 4)), F_SRC1D, MSC_LAST),
    MSC_MOVMSK("{0}movmsk{1}{2}{4} {3}, {5}, {6}{7}", B(53) | B(40), B(53), MSC_FMT,
               FLD(REPEAT, C(44, 4)), F_SRC1D, MSC_LAST),
    MSC_MOVMSK("{0}movmsk{1}{2}.smsk {3}{4}, {5}, {6}, {7}{8}", B(53) | B(40), B(40),
               MSC_FMT, FLD(WMASK, C(44, 4)), MSC_F_SRC0, F_SRC1D, MSC_LAST),
    MSC_MOVMSK("{0}movmsk{1}{2}.smsk{4} {3}, {5}, {6}, {7}{8}", B(53) | B(40),
               B(53) | B(40), MSC_FMT, FLD(REPEAT, C(44, 4)), MSC_F_SRC0, F_SRC1D, MSC_LAST),
    /* u8 prints after .smsk and the repeat count */
    MSC_MOVMSK("{0}movmsk{1}.u8 {3}{2}, {4}, {5}{6}", B(53) | B(40) | B(54) | B(39),
               B(39), FLD(WMASK, C(44, 4)), F_SRC1D, MSC_LAST),
    MSC_MOVMSK("{0}movmsk{1}{2}.u8 {3}, {4}, {5}{6}", B(53) | B(40) | B(54) | B(39),
               B(53) | B(39), FLD(REPEAT, C(44, 4)), F_SRC1D, MSC_LAST),
    MSC_MOVMSK("{0}movmsk{1}.smsk.u8 {3}{2}, {4}, {5}, {6}{7}",
               B(53) | B(40) | B(54) | B(39), B(40) | B(39), FLD(WMASK, C(44, 4)), MSC_F_SRC0,
               F_SRC1D, MSC_LAST),
    MSC_MOVMSK("{0}movmsk{1}.smsk{2}.u8 {3}, {4}, {5}, {6}{7}",
               B(53) | B(40) | B(54) | B(39), B(53) | B(40) | B(39), FLD(REPEAT, C(44, 4)),
               MSC_F_SRC0, F_SRC1D, MSC_LAST),
#undef MSC_LAST
#undef MSC_FMT
#undef MSC_MOVMSK
#endif
