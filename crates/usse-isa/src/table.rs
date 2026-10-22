// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The form table: the original groups (the second half of
//! tools/usse-dis/usse.c), then the instruction families of `forms/`, in the
//! C table's order so that form indices agree.
//!
//! Invariant: a form that decodes a word must print exactly what the oracle
//! prints, never a guess.

use crate::form::{CLSM, Form, b, cls, fld, form, m};
use crate::forms;
use crate::kinds::*;

/// The closed toolchain writes an unused source 2 as immediate 0 (bank 2 with
/// the extension bit): every class-5 mov and every load without a range
/// operand of the ukernels and the transfer programs does.
pub(crate) const UNUSED_SRC2: u64 = b(48) | b(29);

/// emit: the opcode bits around end (50), src0 ext (51) and the sideband
const EMIT_M: u64 = m(63, 56) | b(53) | b(52) | m(47, 46) | b(40);

/// The bitwise group: classes 10-13. and/or (10), xor (11), shl/rol (12),
/// shr/asr (13): one layout, with the operation in enc[35] and the class's
/// low bit. Source 2 is a rotated 16-bit immediate when its bank reads as an
/// immediate. With enc[53] set, the destination mask field is a repeat count
/// instead.
macro_rules! bitwise {
    ($hi:expr, $ops:expr, $pat:expr, $m:expr, $v:expr, $f4:expr, $src2:expr) => {
        form!(
            $pat,
            m(63, 60) | b(53) | ($m),
            ($hi as u64) << 60 | ($v),
            [
                fld!(PRED_ALL, (56, 3)),
                fld!($ops, (35, 1), (59, 1)),
                F_ALUFLAGS,
                F_DST,
                $f4,
                F_SRC1D,
                fld!(F_INV, (43, 1)),
                $src2,
            ]
        )
    };
}
macro_rules! bitwise4 {
    ($hi:expr, $ops:expr) => {
        [
            bitwise!(
                $hi,
                $ops,
                "{0}{1}{2} {3}{4}, {5}, {6}#{7}",
                b(48) | m(29, 28),
                b(48) | b(29),
                fld!(WMASK, (44, 4)),
                F_ROT
            ),
            bitwise!(
                $hi,
                $ops,
                "{0}{1}{2}{4} {3}, {5}, {6}#{7}",
                b(48) | m(29, 28),
                b(53) | b(48) | b(29),
                fld!(REPEAT, (44, 4)),
                F_ROT
            ),
            bitwise!($hi, $ops, "{0}{1}{2} {3}{4}, {5}, {6}{7}", 0, 0, fld!(WMASK, (44, 4)), F_SRC2D),
            bitwise!($hi, $ops, "{0}{1}{2}{4} {3}, {5}, {6}{7}", 0, b(53), fld!(REPEAT, (44, 4)), F_SRC2D),
        ]
    };
}

/// The test group: class 9. An ALU operation (enc[19:14]) whose result is
/// tested into predicate p<enc[35:34]>; enc[20] clear disables the register
/// write ("!" before the destination). One-source operations read source 1.
/// The bitwise operations print a source-1 immediate in hex, the others in
/// decimal. Only they read enc[50] as .partial; the others are left with it
/// clear.
macro_rules! test9 {
    ($ops:expr, $m:expr, $flags:expr, $src:literal, $s1:expr $(, $rest:expr)*) => {
        form!(concat!("{0}{1}{2}.{3} {4}{5}{6}, p{7}, {8}", $src), CLSM | ($m), cls(9), [
            fld!(PRED_ALL, (56, 3)), fld!($ops, (14, 6)), $flags, fld!(K_TEST, (36, 8)),
            fld!(F_WRITE, (20, 1)), F_DST, fld!(WMASK, (44, 4)), fld!(K_DEC, (34, 2)), $s1 $(, $rest)*
        ])
    };
}

/* Integer multiply-add: imae (21), ima16 (20), ima32 (26). Destination, a
 * source-0 operand (r or pa, plus o and sa for ima32) and the usual sources 1
 * and 2. Source and carry formats other than the plain ones (imae u32, ima16
 * u16, no carry or extra operand) are left to the intpack family: those bits
 * are fixed to their plain values here. */
const F_MULFLAGS: crate::form::Field = fld!(K_MULFLAGS, (55, 1), (54, 1), (50, 1), (44, 3));
const F_MSRC0: crate::form::Field = fld!(K_SRC0, (14, 7), (34, 1));

/* Load / store (classes 29, 30). ld/st + address mode (a/l/t) + data type
 * (d/w/b). The base is a source-0 operand and the offset a source-1 operand;
 * enc[41:40] picks plain (+/-), pre-increment (++/--) or post-increment (++/--
 * after it). A load with enc[38] set names a range operand (source 2).
 *
 * The cache mode is enc[33] | enc[51] << 1 on the SGX545 (.bpcache, .fcfill,
 * .bypassl1) and the two bits are swapped on the SGX540: see Core. */
macro_rules! ldst {
    ($cls:expr, $pat:expr, $m:expr, $v:expr, $flags:expr $(, $rest:expr)*) => {
        form!($pat, CLSM | ($m), cls($cls) | ($v), [
            fld!(PRED_ALL, (56, 3)), fld!(AMDT, (36, 2), (42, 2)), $flags $(, $rest)*
        ], UNUSED_SRC2)
    };
}
const F_LDFLAGS: crate::form::Field = fld!(K_LDFLAGS, (55, 1), (52, 1), (33, 1), (51, 1), (53, 1), (44, 4));
const F_STFLAGS: crate::form::Field =
    fld!(K_STFLAGS, (55, 1), (52, 1), (54, 1), (33, 1), (51, 1), (32, 1), (38, 1), (53, 1), (44, 4));
const F_BASE: crate::form::Field = fld!(K_SRC0, (14, 7), (34, 1), (50, 1));
const F_RANGE: crate::form::Field = fld!(K_RANGE, (0, 7), (28, 2), (48, 1), (38, 1));
const F_DRC: crate::form::Field = fld!(K_DEC, (32, 1));

/* Class 31 control: ldr/str, a register index in source 2 (an immediate
 * takes 14 bits). */
const fn ldst_r(mk: u64, v: u64) -> (u64, u64) {
    (m(63, 56) | m(54, 51) | mk, 0xFE20000000000000 | v)
}
const LDR_IMM: (u64, u64) = ldst_r(b(48) | m(29, 28), b(48) | b(29));
const LDR_REG: (u64, u64) = ldst_r(0, 0);
const STR_IMM: (u64, u64) = ldst_r(b(48) | m(29, 28), b(51) | b(48) | b(29));
const STR_REG: (u64, u64) = ldst_r(0, b(51));
const F_LDRFLAGS: crate::form::Field = fld!(K_LDRFLAGS, (55, 1), (43, 1));
const F_IDX14: crate::form::Field = fld!(K_DEC, (0, 7), (14, 7));

const BITWISE_ANDXOR: [Form; 4] = bitwise4!(5, OP_BW_ANDXOR);
const BITWISE_SHIFT: [Form; 4] = bitwise4!(6, OP_BW_SHIFT);

#[rustfmt::skip]
pub(crate) static MAIN: &[Form] = &[
    /* ---- class 31 moves: limm and pclink -------------------------------- */
    form!("{0}mov{1}{2}{3} {4}, #{5}", m(63, 56) | b(53) | b(52), 0xFC << 56 | b(53), [
        fld!(PRED_ALL, (41, 3)), fld!(F_SKIPINV, (55, 1)), fld!(F_NOSCHED, (54, 1)), fld!(F_END, (50, 1)),
        F_DST, F_LIMM,
    ]),
    /* ---- class 5 mov ----------------------------------------------------- */
    form!("{0}mov{1}{2}{3}{4} {5}{6}, {7}", CLSM | m(42, 40) | b(53), cls(5), [
        fld!(PRED_ALL, (56, 3)), fld!(F_SKIPINV, (55, 1)), fld!(F_SYNCS, (52, 1)), fld!(F_NOSCHED, (43, 1)),
        fld!(F_END, (50, 1)), F_DST, fld!(WMASK, (44, 4)), F_SRC1H,
    ], UNUSED_SRC2),
    form!("{0}mov{1}{2}{3}{4}{5} {6}, {7}", CLSM | m(42, 40) | b(53), cls(5) | b(53), [
        fld!(PRED_ALL, (56, 3)), fld!(F_SKIPINV, (55, 1)), fld!(F_SYNCS, (52, 1)), fld!(F_NOSCHED, (43, 1)),
        fld!(F_END, (50, 1)), fld!(REPEAT, (44, 4)), F_DST, F_SRC1H,
    ], UNUSED_SRC2),
    form!("mov pclink, {0}", !m(13, 7), 0xF80000C000000000, [fld!(K_TEMP, (7, 7))]),
    form!("mov {0}, pclink", !m(27, 21), 0xF800010000000000, [fld!(K_TEMP, (21, 7))]),
    /* ---- class 31 specials ---------------------------------------------- */
    form!("nop{0}{1}{2}{3}{4}", m(63, 56) | m(54, 51) | m(49, 44) | m(42, 38), 0xF800014000000000, [
        fld!(F_SYNCS, (55, 1)), fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), fld!(F_TOG, (0, 1)),
        fld!(F_TRIGGER, (1, 1)),
    ]),
    form!("idf drc{0}, bif", !b(32), 0xF820000000000000, [fld!(K_DEC, (32, 1))]),
    form!("wdf drc{0}", !b(32), 0xF920000000000000, [fld!(K_DEC, (32, 1))]),
    form!("phas{0} #{1}, #{2}, {3}, {4}, {5}", m(63, 50) & !b(51), 0xFA44000000000000, [
        fld!(F_END, (51, 1)), fld!(K_DEC, (0, 18)), fld!(K_X4, (32, 8)), fld!(PHAS_RATE, (43, 2)),
        fld!(PHAS_WAIT, (40, 3)), fld!(PHAS_MODE, (45, 1)),
    ]),
    form!("phas{0}{1} {2}, {3}", m(63, 50) & !b(51), 0xFA40000000000000, [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (51, 1)), F_SRC1D, F_SRC2D,
    ]),
    form!("emitpix1{0}{1}{2} #{3} /* incp */, {4}, {5}", EMIT_M, 0xFB20000000000000 | b(40), [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), fld!(F_FREEP, (21, 1)), fld!(K_DEC, (32, 2), (41, 1)),
        F_SRC0, F_SRC1D,
    ]),
    form!("emitpix2{0}{1}{2} #{3} /* incp */, {4}, {5}, {6}, #{7}", EMIT_M, 0xFB20000000000000, [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), fld!(F_FREEP, (21, 1)), fld!(K_DEC, (32, 2), (41, 1)),
        F_SRC0, F_SRC1D, F_SRC2D, fld!(K_HEX, (22, 6), (35, 6), (54, 2)),
    ]),
    /* ---- class 31 flow control: ba / br / bexception ---------------------- */
    form!("{0}ba{1} #{2}", CLSM | m(54, 51) | m(40, 38), cls(31), [
        fld!(PRED_ALL, (56, 3)), F_BRFLAGS, fld!(K_HEX, (0, 18)),
    ]),
    form!("{0}br{1} {2}", CLSM | m(54, 51) | m(40, 38), cls(31) | b(38), [
        fld!(PRED_ALL, (56, 3)), F_BRFLAGS, fld!(K_BR, (0, 18)),
    ]),
    form!("bexception{0}", CLSM | m(54, 51) | m(40, 39), cls(31) | b(51), [fld!(F_NOSCHED, (43, 1))]),
    /* ---- the bitwise group: classes 10-13 --------------------------------- */
    BITWISE_ANDXOR[0],
    BITWISE_ANDXOR[1],
    BITWISE_ANDXOR[2],
    BITWISE_ANDXOR[3],
    BITWISE_SHIFT[0],
    BITWISE_SHIFT[1],
    BITWISE_SHIFT[2],
    BITWISE_SHIFT[3],
    /* ---- the test group: class 9 ------------------------------------------- */
    test9!(TESTOP_BW, 0, F_TESTFLAGS, ", {9}", F_SRC1H, F_SRC2D),
    test9!(TESTOP_2, b(50), F_TESTFLAGS4, ", {9}", F_SRC1D, F_SRC2D),
    test9!(TESTOP_1, b(50), F_TESTFLAGS4, "", F_SRC1D),
    /* ---- integer multiply-add: imae (21), ima16 (20), ima32 (26) ---------- */
    form!("{0}imae{1}.{2}{3} {4}, {5}.{6}, {7}.{8}, {9}, u32", CLSM | b(47) | m(41, 38), cls(21) | b(39), [
        fld!(PRED_MUL, (57, 2)), F_MULFLAGS, fld!(SIGNSAT, (42, 1), (43, 1)), fld!(RS, (35, 3)), F_DST,
        F_MSRC0, fld!(LOWHIGH, (56, 1)), F_SRC1D, fld!(LOWHIGH, (53, 1)), F_SRC2D,
    ]),
    form!("{0}ima16{1}.{2}{3} {4}{5}, {6}, {7}, {8}{9}, u16, u16", CLSM | b(47) | m(41, 37), cls(20), [
        fld!(PRED_MUL, (57, 2)), F_MULFLAGS, fld!(SIGNSAT, (42, 1), (43, 1)), fld!(RS, (35, 2)), F_DST,
        fld!(F_ABS, (56, 1)), F_MSRC0, F_SRC1D, F_SRC2D, fld!(F_NEG, (53, 1)),
    ]),
    form!("{0}ima32{1}.{2} {3}, {4}, {5}{6}, {7}{8}", CLSM | m(53, 52) | m(43, 42) | b(38), cls(26), [
        fld!(PRED_ALL, (56, 3)), F_MULFLAGS, fld!(SU, (41, 1)), F_DST,
        fld!(K_SRC0, (14, 7), (34, 1), (47, 1)), F_SRC1D, fld!(F_NEG, (40, 1)), F_SRC2D, fld!(F_NEG, (39, 1)),
    ]),
    /* ---- load / store (classes 29, 30) ------------------------------------ */
    ldst!(29, "{0}ld{1}{2} {3}, [{4},{5}{6}{7}]{8}, drc{9}", b(54), 0, F_LDFLAGS, F_LSDST, F_BASE,
        fld!(IMODE_PLAIN, (40, 2)), fld!(F_SIGN, (35, 1)), F_SRC1D, F_RANGE, F_DRC),
    ldst!(29, "{0}ld{1}{2} {3}, [{4},{5}{6}]{7}, drc{8}", b(54) | m(41, 40), b(40), F_LDFLAGS, F_LSDST,
        F_BASE, fld!(F_SIGN2, (35, 1)), F_SRC1D, F_RANGE, F_DRC),
    ldst!(29, "{0}ld{1}{2} {3}, [{4},{6}{5}]{7}, drc{8}", b(54) | m(41, 40), b(41), F_LDFLAGS, F_LSDST,
        F_BASE, fld!(F_SIGN2, (35, 1)), F_SRC1D, F_RANGE, F_DRC),
    ldst!(30, "{0}st{1}{2} [{3},{4}{5}{6}], {7}", 0, 0, F_STFLAGS, F_BASE, fld!(IMODE_PLAIN, (40, 2)),
        fld!(F_SIGN, (35, 1)), F_SRC1D, F_SRC2D),
    ldst!(30, "{0}st{1}{2} [{3},{4}{5}], {6}", m(41, 40), b(40), F_STFLAGS, F_BASE, fld!(F_SIGN2, (35, 1)),
        F_SRC1D, F_SRC2D),
    ldst!(30, "{0}st{1}{2} [{3},{5}{4}], {6}", m(41, 40), b(41), F_STFLAGS, F_BASE, fld!(F_SIGN2, (35, 1)),
        F_SRC1D, F_SRC2D),
    /* ---- class 31 control: ldr / str -------------------------------------- */
    form!("ldr{0} {1}, #{2}, drc{3}", LDR_IMM.0, LDR_IMM.1, [
        F_LDRFLAGS, F_LSDST, F_IDX14, fld!(K_DEC, (32, 2)),
    ]),
    form!("ldr{0} {1}, {2}, drc{3}", LDR_REG.0, LDR_REG.1, [
        F_LDRFLAGS, F_LSDST, F_SRC2D, fld!(K_DEC, (32, 2)),
    ]),
    form!("str{0} #{1}, {2}", STR_IMM.0, STR_IMM.1, [F_LDRFLAGS, F_IDX14, F_SRC1D]),
    form!("str{0} {1}, {2}", STR_REG.0, STR_REG.1, [F_LDRFLAGS, F_SRC2D, F_SRC1D]),
];

/// The whole table, in order: the original groups, then the families.
pub(crate) static PARTS: &[&[Form]] =
    &[MAIN, forms::float::FORMS, forms::memctl::FORMS, forms::intpack::FORMS, forms::misc::FORMS];
