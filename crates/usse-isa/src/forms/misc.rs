// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The misc family (tools/usse-dis/forms/misc.inc): movc/movmsk, the rest of
//! the test group (classes 9 and 15), adif/adifsum/bilin/firv/firh.

use crate::form::{CLSM, Ctx, Enum, Field, Flags, Form, Kind, b, cls, fld, form, m, names, sparse};
use crate::kinds::*;
use std::fmt::Write as _;

/* ---- class 5: movc / movmsk --------------------------------------------- *
 * enc[42:40] is the class-5 type: 0 the plain mov (main table), 1-5 movc with
 * data type i8, i16, i32, flt, i10, 6 movmsk and 7 movmsk.smsk (which adds a
 * source). enc[39] | enc[54] << 1 is the movc test and the movmsk format; u8
 * is the one movmsk format printed after .smsk and the repeat count. */
const MOVC_T: Enum = Enum(names![None, ".i8", ".i16", ".i32", ".flt", ".i10", None, None]);
const MOVC_TR: Enum = Enum(names![None, None, ".i16", ".i32", ".flt", ".i10", None, None]);
const MOVC_TEST: Enum = Enum(names![".tz", ".tnz", ".tn", ".tp|z"]);
const MOVMSK_F: Enum = Enum(names![".mask", None, ".flt16", ".flt"]);
/// the component of movmsk's last source, enc[38:37]
const COMP: Enum = Enum(names!["", ".1", ".2", ".3"]);

const F_SRC0P: Field = fld!(K_SRC0, (14, 7), (34, 1));
const K_C5FLAGS: Flags = Flags(&[".skipinv", ".syncs", ".nosched", ".end"]);
const C5FLAGS: Field = fld!(K_C5FLAGS, (55, 1), (52, 1), (43, 1), (50, 1));

/* ---- classes 9 and 15: the test group ---------------------------------- *
 * Class 9 tests the result into a predicate, class 15 into a mask (b, w or d,
 * enc[37:36]; enc[38] and the predicate number enc[35:34] are ignored). The
 * main table decodes class 9 with enc[50] clear and the bitwise operations;
 * the forms here add enc[50] set for the others: .sat on the 8-bit integer
 * operations, nothing on the 16- and 32-bit ones, and a per-operand format on
 * the float operations: bit 6 of the operand number selects .flt16 (number
 * [5:1], component [0], printed .2 when set) or, for fpmul8/fpadd8/fpsub8,
 * .c10 (number [5:0]). Constants and immediates keep their 7-bit numbers. */

/// Class 15 test: v = mask type[1:0] (enc[37:36]) | rest[6:2] (enc[43:39]:
/// all-or-any, zero test, sign test, as in K_TEST).
struct TestMsk;

impl Kind for TestMsk {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const SIGN: [Option<&str>; 4] = [Some("t"), Some("n"), Some("p"), None];
        const ZERO: [Option<&str>; 4] = [Some("t"), Some("z"), Some("nz"), None];
        const MSK: [Option<&str>; 4] = [Some("bmsk"), Some("wmsk"), Some("dmsk"), None];
        let (Some(s), Some(z), Some(k)) =
            (SIGN[(v >> 5) as usize & 3], ZERO[(v >> 3 & 3) as usize], MSK[(v & 3) as usize])
        else {
            return false;
        };
        let _ = write!(out, "test{s}{}{z}.{k}", if v & 4 != 0 { "a" } else { "o" });
        true
    }
}

/// The format a float operand with enc[50] set reads.
#[derive(Clone, Copy, PartialEq, Eq)]
enum FMode {
    Flt16,
    /// a source prints .0 for the first component
    Flt16_0,
    C10,
}

/// A float operand with enc[50] set: v = number[6:0] | bank[8:7] | ext[9],
/// banks numbered as for K_DST (dst) or K_SRCD. Temporaries past r59 (r29 for
/// .flt16) print as i0-i3; an indexed operand takes a 6-bit index ([5:4]
/// bank, [3] il/ih, [2:0] offset) and no component.
struct FOp {
    dst: bool,
    mode: FMode,
}

impl Kind for FOp {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const SB: [&str; 8] = ["r", "o", "pa", "sa", "[", "c", "#", "i"];
        const DB: [&str; 8] = ["r", "o", "pa", "[", "sa", "c", "i.", "i"];
        const IB: [&str; 4] = ["r", "o", "pa", "sa"];
        let bk = (if self.dst { DB } else { SB })[(v >> 7) as usize & 7];
        let r = v & 0x7f;
        let (fmt, n6) = (r >> 6, r & 63);
        let h = fmt != 0 && self.mode != FMode::C10;
        let num = if h { n6 >> 1 } else { n6 };
        let comp = h && n6 & 1 != 0 && bk != "[";
        let lim = if h { 30 } else { 60 };
        match bk {
            "c" => {
                print_const(out, r);
                return true;
            }
            "#" => {
                let _ = write!(out, "#{r}");
                return true;
            }
            "i." => {
                dst(out, 6 << 7 | r);
                return true;
            }
            "[" => {
                let _ = write!(
                    out,
                    "{}[{} + #{}]",
                    IB[(n6 >> 4) as usize],
                    if n6 & 8 != 0 { "ih" } else { "il" },
                    n6 & 7
                );
            }
            "r" if num >= lim => {
                let _ = write!(out, "i{}", num - lim);
            }
            _ => {
                let _ = write!(out, "{bk}{num}");
            }
        }
        if fmt != 0 {
            if self.mode == FMode::C10 {
                out.push_str(".c10");
            } else {
                out.push_str(".flt16");
                if comp {
                    out.push_str(".2");
                } else if !self.dst && self.mode == FMode::Flt16_0 {
                    out.push_str(".0");
                }
            }
        }
        true
    }
}
const K_FDST: FOp = FOp { dst: true, mode: FMode::Flt16 };
const K_FSRC: FOp = FOp { dst: false, mode: FMode::Flt16 };
const K_FSRC0: FOp = FOp { dst: false, mode: FMode::Flt16_0 };
const K_CDST: FOp = FOp { dst: true, mode: FMode::C10 };
const K_CSRC: FOp = FOp { dst: false, mode: FMode::C10 };
macro_rules! mdst {
    ($k:expr) => {
        fld!($k, (21, 7), (32, 2), (51, 1))
    };
}
macro_rules! msrc1 {
    ($k:expr) => {
        fld!($k, (7, 7), (30, 2), (49, 1))
    };
}
macro_rules! msrc2 {
    ($k:expr) => {
        fld!($k, (0, 7), (28, 2), (48, 1))
    };
}

/* Test-group operations enc[19:14], grouped by how they read enc[50] set. */
const TOP_I: Enum = Enum(&sparse::<64>(&[
    (22, "iadd16"),
    (23, "isub16"),
    (24, "imul16"),
    (25, "iaddu16"),
    (26, "isubu16"),
    (27, "imulu16"),
    (28, "iadd32"),
    (29, "iaddu32"),
]));
const TOP_I8: Enum = Enum(&sparse::<64>(&[
    (32, "iadd8"),
    (33, "isub8"),
    (34, "iaddu8"),
    (35, "isubu8"),
    (36, "imul8"),
    (38, "imulu8"),
]));
const TOP_F2: Enum = Enum(&sparse::<64>(&[(0, "fadd"), (3, "fsubflr"), (13, "fmul"), (14, "fsub")]));
const TOP_F2Z: Enum =
    Enum(&sparse::<64>(&[(8, "fdp"), (9, "fmin"), (10, "fmax"), (11, "fdsx"), (12, "fdsy")]));
const TOP_C10: Enum = Enum(&sparse::<64>(&[(37, "fpmul8"), (39, "fpadd8"), (40, "fpsub8")]));

/* ---- classes 22 and 23: adif, adifsum, bilin, firv, firh ---------------- *
 * Class 22 selects the operation with enc[53:52] (0 adif, or adifsum with
 * enc[38]; 2 bilin; 3 firv); class 23 is firh. All take the multiply flags
 * (skipinv, nosched, end, repeat count) and a 2-bit predicate enc[58:57].
 * firv and firh read their second operand's number from enc[20:14] with
 * source 1's bank. */
const FMT8: Enum = Enum(names!["u8", "s8", "o8", None]);
const BILIN_L: Enum = Enum(names!["planar", "interleaved"]);
const BILIN_SEL: Enum = Enum(names!["src01, dst01", "src23, dst01", "src01, dst23", "src23, dst23"]);
const RND: Enum = Enum(names!["", ", rnd"]);
const K_FIRV: Flags = Flags(&[", +src2.2", ", iadd", ", scale"]);

/// firh: v = edge mode[1:0] | filter coefficient set[4:2]
struct Firh;

impl Kind for Firh {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const EDGE: [&str; 4] = ["rep", "msc", "mdc", "reserved"];
        let _ = write!(out, "{}, fcs{}", EDGE[(v & 3) as usize], v >> 2);
        true
    }
}

/// A 5-bit two's-complement number.
struct S5;

impl Kind for S5 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = write!(out, "{}", v as i32 - if v & 16 != 0 { 32 } else { 0 });
        true
    }
}

/// firv/firh operands 2 and 3: v = number[6:0] (enc[20:14]) | source-1
/// number[13:7] | the shared bank[16:14], printed as two K_SRCD operands. One
/// field, so that the assembler cannot pick two different banks.
struct Pair;

impl Kind for Pair {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let bank = v >> 14 << 7;
        srcd(out, bank | (v & 0x7f));
        out.push_str(", ");
        srcd(out, bank | (v >> 7 & 0x7f));
        true
    }
}

macro_rules! mulflags {
    ($rw:expr) => {
        fld!(K_MULFLAGS, (55, 1), (54, 1), (50, 1), (44, $rw))
    };
}
const F_PAIR: Field = fld!(Pair, (14, 7), (7, 7), (30, 2), (49, 1));

/* movmsk: masked format, without and with .smsk (enc[40]) */
macro_rules! movmsk {
    ($pat:expr, $m:expr, $v:expr, $f:expr $(, $rest:expr)*) => {
        form!($pat, CLSM | m(42, 41) | ($m), cls(5) | m(42, 41) | ($v), [
            fld!(PRED_ALL, (56, 3)), C5FLAGS, $f, F_DST $(, $rest)*
        ])
    };
}
const MOVMSK_FMT: Field = fld!(MOVMSK_F, (39, 1), (54, 1));
const LAST_SRC: Field = fld!(K_SRCD, (0, 7), (28, 2), (48, 1));
const LAST_COMP: Field = fld!(COMP, (37, 2));

macro_rules! test9 {
    ($ops:expr, $m:expr, $v:expr, $flags:expr, $sat:literal, $src:literal, $d:expr $(, $rest:expr)*) => {
        form!(concat!("{0}{1}{2}", $sat, ".{3} {4}{5}{6}, p{7}, {8}", $src), CLSM | ($m), cls(9) | ($v), [
            fld!(PRED_ALL, (56, 3)), fld!($ops, (14, 6)), $flags, fld!(K_TEST, (36, 8)),
            fld!(F_WRITE, (20, 1)), $d, fld!(WMASK, (44, 4)), fld!(K_DEC, (34, 2)) $(, $rest)*
        ])
    };
}
macro_rules! test15 {
    ($ops:expr, $m:expr, $v:expr, $flags:expr, $sat:literal, $src:literal, $d:expr $(, $rest:expr)*) => {
        form!(concat!("{0}{1}{2}", $sat, ".{3} {4}{5}{6}, {7}", $src), CLSM | ($m), cls(15) | ($v), [
            fld!(PRED_ALL, (56, 3)), fld!($ops, (14, 6)), $flags,
            fld!(TestMsk, (36, 2), (39, 5)), fld!(F_WRITE, (20, 1)), $d,
            fld!(WMASK, (44, 4)) $(, $rest)*
        ])
    };
}
/// enc[50] set (both classes)
macro_rules! tests50 {
    ($t:ident, $s2:literal) => {
        [
            $t!(TOP_I, b(50), b(50), F_TESTFLAGS4, "", $s2, F_DST, F_SRC1D, F_SRC2D),
            $t!(TOP_I8, b(50), b(50), F_TESTFLAGS4, ".sat", $s2, F_DST, F_SRC1D, F_SRC2D),
            $t!(TOP_F2, b(50), b(50), F_TESTFLAGS4, "", $s2, mdst!(K_FDST), msrc1!(K_FSRC), msrc2!(K_FSRC)),
            $t!(
                TOP_F2Z,
                b(50),
                b(50),
                F_TESTFLAGS4,
                "",
                $s2,
                mdst!(K_FDST),
                msrc1!(K_FSRC0),
                msrc2!(K_FSRC0)
            ),
            $t!(TESTOP_1, b(50), b(50), F_TESTFLAGS4, "", "", mdst!(K_FDST), msrc1!(K_FSRC0)),
            $t!(TOP_C10, b(50), b(50), F_TESTFLAGS4, "", $s2, mdst!(K_CDST), msrc1!(K_CSRC), msrc2!(K_CSRC)),
        ]
    };
}
const TESTS50_9: [Form; 6] = tests50!(test9, ", {9}");
const TESTS50_15: [Form; 6] = tests50!(test15, ", {8}");

#[rustfmt::skip]
pub(crate) static FORMS: &[Form] = &[
    form!("{0}movc{1}{2}{3} {4}{5}, {6}, {7}, {8}", CLSM | b(53), cls(5), [
        fld!(PRED_ALL, (56, 3)), C5FLAGS, fld!(MOVC_T, (40, 3)), fld!(MOVC_TEST, (39, 1), (54, 1)), F_DST,
        fld!(WMASK, (44, 4)), F_SRC0P, F_SRC1D, F_SRC2D,
    ]),
    /* with a repeat count: it prints before i8 and after the other types */
    form!("{0}movc{1}{2}{3}{4} {5}, {6}, {7}, {8}", CLSM | b(53), cls(5) | b(53), [
        fld!(PRED_ALL, (56, 3)), C5FLAGS, fld!(MOVC_TR, (40, 3)), fld!(REPEAT, (44, 4)),
        fld!(MOVC_TEST, (39, 1), (54, 1)), F_DST, F_SRC0P, F_SRC1D, F_SRC2D,
    ]),
    form!("{0}movc{1}{2}.i8{3} {4}, {5}, {6}, {7}", CLSM | b(53) | m(42, 40), cls(5) | b(53) | b(40), [
        fld!(PRED_ALL, (56, 3)), C5FLAGS, fld!(REPEAT, (44, 4)), fld!(MOVC_TEST, (39, 1), (54, 1)), F_DST,
        F_SRC0P, F_SRC1D, F_SRC2D,
    ]),
    movmsk!("{0}movmsk{1}{2} {3}{4}, {5}, {6}{7}", b(53) | b(40), 0, MOVMSK_FMT, fld!(WMASK, (44, 4)),
        F_SRC1D, LAST_SRC, LAST_COMP),
    movmsk!("{0}movmsk{1}{2}{4} {3}, {5}, {6}{7}", b(53) | b(40), b(53), MOVMSK_FMT, fld!(REPEAT, (44, 4)),
        F_SRC1D, LAST_SRC, LAST_COMP),
    movmsk!("{0}movmsk{1}{2}.smsk {3}{4}, {5}, {6}, {7}{8}", b(53) | b(40), b(40), MOVMSK_FMT,
        fld!(WMASK, (44, 4)), F_SRC0P, F_SRC1D, LAST_SRC, LAST_COMP),
    movmsk!("{0}movmsk{1}{2}.smsk{4} {3}, {5}, {6}, {7}{8}", b(53) | b(40), b(53) | b(40), MOVMSK_FMT,
        fld!(REPEAT, (44, 4)), F_SRC0P, F_SRC1D, LAST_SRC, LAST_COMP),
    /* u8 prints after .smsk and the repeat count */
    movmsk!("{0}movmsk{1}.u8 {3}{2}, {4}, {5}{6}", b(53) | b(40) | b(54) | b(39), b(39), fld!(WMASK, (44, 4)),
        F_SRC1D, LAST_SRC, LAST_COMP),
    movmsk!("{0}movmsk{1}{2}.u8 {3}, {4}, {5}{6}", b(53) | b(40) | b(54) | b(39), b(53) | b(39),
        fld!(REPEAT, (44, 4)), F_SRC1D, LAST_SRC, LAST_COMP),
    movmsk!("{0}movmsk{1}.smsk.u8 {3}{2}, {4}, {5}, {6}{7}", b(53) | b(40) | b(54) | b(39), b(40) | b(39),
        fld!(WMASK, (44, 4)), F_SRC0P, F_SRC1D, LAST_SRC, LAST_COMP),
    movmsk!("{0}movmsk{1}.smsk{2}.u8 {3}, {4}, {5}, {6}{7}", b(53) | b(40) | b(54) | b(39),
        b(53) | b(40) | b(39), fld!(REPEAT, (44, 4)), F_SRC0P, F_SRC1D, LAST_SRC, LAST_COMP),
    TESTS50_9[0],
    TESTS50_9[1],
    TESTS50_9[2],
    TESTS50_9[3],
    TESTS50_9[4],
    TESTS50_9[5],
    /* class 15 with enc[50] clear and the bitwise operations, as the main
     * table's class 9, except that a bitwise operation prints a source-2
     * immediate in hex and a source-1 one in decimal */
    test15!(TESTOP_BW, 0, 0, F_TESTFLAGS, "", ", {8}", F_DST, F_SRC1D, fld!(K_SRCH, (0, 7), (28, 2), (48, 1))),
    test15!(TESTOP_2, b(50), 0, F_TESTFLAGS4, "", ", {8}", F_DST, F_SRC1D, F_SRC2D),
    test15!(TESTOP_1, b(50), 0, F_TESTFLAGS4, "", "", F_DST, F_SRC1D),
    TESTS50_15[0],
    TESTS50_15[1],
    TESTS50_15[2],
    TESTS50_15[3],
    TESTS50_15[4],
    TESTS50_15[5],
    form!("{0}adif{1} {2}, {3}, {4}", CLSM | m(53, 52) | b(38), cls(22), [
        fld!(PRED_MUL, (57, 2)), mulflags!(3), F_DST, F_SRC1D, F_SRC2D,
    ]),
    form!("{0}adifsum{1} {2}, {3}.{4}, {5}, {6}", CLSM | m(53, 52) | b(38), cls(22) | b(38), [
        fld!(PRED_MUL, (57, 2)), mulflags!(3), F_DST, F_SRC0P, fld!(LOWHIGH, (47, 1)), F_SRC1D, F_SRC2D,
    ]),
    form!("{0}bilin{1} {2}, {3}, {4}, {5}, {6}, {7}, {8}{9}", CLSM | m(53, 52), cls(22) | b(53), [
        fld!(PRED_MUL, (57, 2)), mulflags!(3), F_DST, F_SRC0P, F_SRC1D, F_SRC2D, fld!(FMT8, (40, 2)),
        fld!(BILIN_L, (56, 1)), fld!(BILIN_SEL, (43, 1), (42, 1)), fld!(RND, (47, 1)),
    ]),
    form!("{0}firv{1} {2}{3}, {4}, {5}, {6}{7}", CLSM | m(53, 52), cls(22) | m(53, 52), [
        fld!(PRED_MUL, (57, 2)), mulflags!(3), fld!(F_WRITE, (47, 1)), F_DST, F_PAIR, F_SRC2D,
        fld!(FMT8, (40, 2)), fld!(K_FIRV, (56, 1), (39, 1), (42, 1)),
    ]),
    form!("{0}firh{1} {2}, {3}, {4}, {5}, {6}, #{7}, #{8}", CLSM, cls(23), [
        fld!(PRED_MUL, (57, 2)), mulflags!(2), F_DST, F_PAIR, F_SRC2D, fld!(FMT8, (40, 2)),
        fld!(Firh, (38, 2), (35, 3)), fld!(S5, (42, 2), (46, 2), (56, 1)), fld!(K_DEC, (52, 2)),
    ]),
];
