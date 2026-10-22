// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The intpack family (tools/usse-dis/forms/intpack.inc): pack/unpack, the
//! colour/alpha selection ops (sop2, sop3, lrp, sop2wm), ima8 and the integer
//! multiply-adds in every format, dot products and rlp.

use crate::form::{CLSM, Ctx, Enum, Field, Form, Kind, b, cls, fld, form, m, names, sparse};
use crate::kinds::*;
use std::fmt::Write as _;

/// A source operand with a component (byte or half) select after it:
/// v = operand[9:0] | select[11:10]. With `zero_plain`, select 0 prints
/// nothing.
struct SrcC {
    zero_plain: bool,
}

impl Kind for SrcC {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        srcd(out, v & 0x3ff);
        if !(self.zero_plain && v >> 10 == 0) {
            let _ = write!(out, ".{}", v >> 10 & 3);
        }
        true
    }
}
const K_SRCC: SrcC = SrcC { zero_plain: false };
const K_SRCC0: SrcC = SrcC { zero_plain: true };

/* ---- class 8: pack / unpack ----------------------------------------------
 * enc[43:38] names the destination and source formats together (value 8 is
 * statomic, whose component selects print nothing for 0). enc[37:34] is the
 * destination byte mask (all four bytes print nothing), enc[20:19] the
 * rounding mode, enc[18] .scale, enc[17:16] and enc[15:14] the source
 * components. */
const PCKFMT: [Option<&str>; 64] = *names![
    "unpcku8u8",
    "unpcku8s8",
    None,
    "unpcku16u8",
    "unpcks16u8",
    "unpckf16u8",
    "unpckf32u8",
    None,
    None,
    None,
    None,
    "unpcku16s8",
    "unpcks16s8",
    "unpckf16s8",
    "unpckf32s8",
    None,
    None,
    None,
    None,
    "unpcku16o8",
    "unpcks16o8",
    "unpckf16o8",
    "unpckf32o8",
    None,
    "pcku8u16",
    "pcks8u16",
    "pcko8u16",
    "pcku16u16",
    "pcks16u16",
    "pckf16u16",
    "unpckf32u16",
    "pckc10u16",
    "pcku8s16",
    "pcks8s16",
    "pcko8s16",
    "pcku16s16",
    "pcks16s16",
    "pckf16s16",
    "unpckf32s16",
    "pckc10s16",
    "pcku8f16",
    "pcks8f16",
    "pcko8f16",
    "pcku16f16",
    "pcks16f16",
    "pckf16f16",
    "unpckf32f16",
    "pckc10f16",
    "pcku8f32",
    "pcks8f32",
    "pcko8f32",
    "pcku16f32",
    "pcks16f32",
    "pckf16f32",
    "unpckf32f32",
    "pckc10f32",
    None,
    None,
    None,
    "unpcku16c10",
    "unpcks16c10",
    "unpckf16c10",
    "unpckf32c10",
    "unpckc10c10"
];
const BYTEMASK: Enum = Enum(names![
    ".bytemask0000",
    ".bytemask0001",
    ".bytemask0010",
    ".bytemask0011",
    ".bytemask0100",
    ".bytemask0101",
    ".bytemask0110",
    ".bytemask0111",
    ".bytemask1000",
    ".bytemask1001",
    ".bytemask1010",
    ".bytemask1011",
    ".bytemask1100",
    ".bytemask1101",
    ".bytemask1110",
    ""
]);
const ROUND: Enum = Enum(names!["nearest", "down", "up", "zero"]);

/// The operation and its flags: v = format[5:0] | skipinv | syncs << 1 |
/// nosched << 2 | end << 3 | enc[18] << 4, which reads .renable for the
/// float-to-float conversions and .scale for the others.
struct PckOp(&'static [Option<&'static str>; 64]);

impl Kind for PckOp {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const F: [&str; 4] = [".skipinv", ".syncs", ".nosched", ".end"];
        let fmt = v & 63;
        let Some(name) = self.0[fmt as usize] else { return false };
        out.push_str(name);
        for (i, f) in F.iter().enumerate() {
            if v >> (6 + i) & 1 != 0 {
                out.push_str(f);
            }
        }
        if v >> 10 & 1 != 0 {
            out.push_str(if matches!(fmt, 45 | 46 | 53 | 54) { ".renable" } else { ".scale" });
        }
        true
    }
}
const STATOMIC: [Option<&str>; 64] = sparse::<64>(&[(8, "statomic")]);
const K_PCKOP: PckOp = PckOp(&PCKFMT);
const K_STATOMIC: PckOp = PckOp(&STATOMIC);

/* ---- classes 16-19, 24: sop2, sop3, lrp, sop2wm, ima8, dot products -------
 * These name registers with six bits and a seventh, .c10, after the name:
 * temporaries r0-r59 then i0-i3, indexed [5:4] bank, [3] il/ih, [2:0]
 * offset. Constants, immediates and i.l/i.h keep all seven bits. */

/// The bank numbering of a c10 operand.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Banks {
    /// that of K_DST
    Dst,
    /// that of K_SRCD
    Src,
    /// a source 0 of r and pa
    Src0,
}

/// A modifier bit above the operand (bit 8 for a source 0, 10 otherwise),
/// printed before .c10.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Mod {
    None,
    Neg,
    Comp,
}

struct C10(Banks, Mod);

impl Kind for C10 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const D: [&str; 8] = ["", "o", "pa", "", "sa", "", "", "i"];
        const S: [&str; 8] = ["", "o", "pa", "sa", "", "", "", "i"];
        const IB: [&str; 4] = ["r", "o", "pa", "sa"];
        const Z: [&str; 2] = ["", "pa"];
        let C10(banks, md) = *self;
        let dst = banks == Banks::Dst;
        let nb = if banks == Banks::Src0 { 8 } else { 10 };
        let op = v & ((1 << nb) - 1);
        let modbit = v >> nb & 1 != 0;
        let (bk, r) = (op >> 7, op & 63);
        let mut c10 = op >> 6 & 1 != 0;
        if banks != Banks::Src0 && (bk == 5 || bk == 6) {
            if dst {
                dst_print(out, op)
            } else {
                srcd(out, op)
            }
            c10 = false;
        } else if bk == 0 {
            let _ = if r < 60 { write!(out, "r{r}") } else { write!(out, "i{}", r - 60) };
        } else if banks != Banks::Src0 && bk == if dst { 3 } else { 4 } {
            let _ = write!(
                out,
                "{}[{} + #{}]",
                IB[(r >> 4) as usize],
                if r & 8 != 0 { "ih" } else { "il" },
                r & 7
            );
        } else {
            let name = match banks {
                Banks::Dst => D[bk as usize],
                Banks::Src => S[bk as usize],
                Banks::Src0 => Z[bk as usize & 1],
            };
            let _ = write!(out, "{name}{r}");
        }
        if modbit {
            out.push_str(if md == Mod::Neg { ".neg" } else { ".comp" });
        }
        if c10 {
            out.push_str(".c10");
        }
        true
    }
}

fn dst_print(out: &mut String, v: u32) {
    crate::kinds::dst(out, v)
}

const K_D10: C10 = C10(Banks::Dst, Mod::None);
const K_S10: C10 = C10(Banks::Src, Mod::None);
const K_S0_10: C10 = C10(Banks::Src0, Mod::None);
const K_S0N_10: C10 = C10(Banks::Src0, Mod::Neg);
const K_SC10: C10 = C10(Banks::Src, Mod::Comp);
const F_D10: Field = fld!(K_D10, (21, 7), (32, 2), (51, 1));
const F_S1_10: Field = fld!(K_S10, (7, 7), (30, 2), (49, 1));
const F_S2_10: Field = fld!(K_S10, (0, 7), (28, 2), (48, 1));
const F_S0_10: Field = fld!(K_S0_10, (14, 7), (34, 1));

/// One item of a blend selection: a selector (w bits, its names) followed by
/// its complement bit when `comp` is set (".comp").
struct Item {
    t: &'static [&'static str],
    w: u32,
    comp: bool,
}

/// A blend selection: items printed ", "-separated. v = [complement-all] |
/// items in order | [neg]; `pre` adds a leading bit printed as "comp, ",
/// `neg` a trailing one printed as ", neg".
struct Sel {
    pre: bool,
    neg: bool,
    it: &'static [Item],
}

impl Kind for Sel {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let mut sh = 0;
        if self.pre {
            if v & 1 != 0 {
                out.push_str("comp, ");
            }
            sh += 1;
        }
        for (i, it) in self.it.iter().enumerate() {
            let Some(name) = it.t.get((v >> sh & ((1 << it.w) - 1)) as usize) else { return false };
            let c = it.comp && v >> (sh + it.w) & 1 != 0;
            sh += it.w + it.comp as u32;
            let _ = write!(out, "{}{name}{}", if i > 0 { ", " } else { "" }, if c { ".comp" } else { "" });
        }
        if self.neg && v >> sh & 1 != 0 {
            out.push_str(", neg");
        }
        true
    }
}

const fn item(t: &'static [&'static str], w: u32, comp: bool) -> Item {
    Item { t, w, comp }
}

const OP4: &[&str] = &["add", "sub", "min", "max"];
const OP2: &[&str] = &["add", "sub"];
/* sop2 (a None entry of a C table: the selector table ends before it) */
const SOP2C1: &[&str] = &["zero", "s1", "s2", "s1a", "s2a", "asat", "s2scale"];
const SOP2C2: &[&str] = &["zero", "s1", "s2", "s1a", "s2a", "asat", "zeros"];
const SOP2A1: &[&str] = &["zero", "s1a", "s2a", "s2scale"];
const SOP2A2: &[&str] = &["zero", "s1a", "s2a", "zeros"];
const K_SOP2C: Sel =
    Sel { pre: true, neg: false, it: &[item(SOP2C1, 3, true), item(SOP2C2, 3, true), item(OP4, 2, false)] };
const K_SOP2A: Sel =
    Sel { pre: true, neg: true, it: &[item(SOP2A1, 2, true), item(SOP2A2, 2, true), item(OP4, 2, false)] };
/* sop3 and lrp */
const SEL3: &[&str] = &["zero", "asat", "s1", "s1a", "s0", "s0a", "s2", "s2a"];
const ASEL: &[&str] = &["zero", "s0a", "s1a", "s2a"];
const Z1A: &[&str] = &["zero", "s1a"];
const Z2A: &[&str] = &["zero", "s2a"];
const Z1: &[&str] = &["zero", "s1"];
const Z2: &[&str] = &["zero", "s2"];
const S0: &[&str] = &["s0", "s0a"];
const LRP2: &[&str] = &["s1, s2", "s1, s2.comp", "one, s2", "s1.comp, s2"];
const K_SOP3C: Sel =
    Sel { pre: false, neg: false, it: &[item(SEL3, 3, true), item(SEL3, 3, true), item(OP2, 1, false)] };
const K_SOP3CN: Sel =
    Sel { pre: false, neg: true, it: &[item(SEL3, 3, true), item(SEL3, 3, true), item(OP2, 1, false)] };
const K_ASOP: Sel =
    Sel { pre: false, neg: false, it: &[item(ASEL, 2, true), item(SEL3, 3, true), item(OP2, 1, false)] };
const K_ARSOP: Sel = Sel { pre: false, neg: false, it: &[item(ASEL, 2, true), item(OP2, 1, false)] };
const K_ALRP: Sel = Sel { pre: false, neg: false, it: &[item(Z1A, 1, true), item(Z2A, 1, true)] };
const K_LRP1C: Sel =
    Sel { pre: false, neg: false, it: &[item(Z1, 1, true), item(Z2, 1, true), item(S0, 1, false)] };
const K_LRP1A: Sel = K_SOP3C;
const K_LRP2C: Sel =
    Sel { pre: false, neg: false, it: &[item(SEL3, 3, true), item(SEL3, 3, true), item(LRP2, 2, false)] };
/* sop2wm: values 4 and 5 are invalid */
struct SelWm;

impl Kind for SelWm {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool {
        const T: &[&str] = &["zero", "asat", "s1", "s1a", "", "", "s2", "s2a"];
        const SEL: Sel = Sel {
            pre: false,
            neg: false,
            it: &[item(T, 3, true), item(T, 3, true), item(OP4, 2, false), item(OP4, 2, false)],
        };
        let (a, c) = (v & 7, v >> 4 & 7);
        if matches!(a, 4 | 5) || matches!(c, 4 | 5) {
            return false;
        }
        SEL.print(v, cx, out)
    }
}
const BYTEMASK4: Enum = Enum(names![
    ".bytemask0000",
    ".bytemask0001",
    ".bytemask0010",
    ".bytemask0011",
    ".bytemask0100",
    ".bytemask0101",
    ".bytemask0110",
    ".bytemask0111",
    ".bytemask1000",
    ".bytemask1001",
    ".bytemask1010",
    ".bytemask1011",
    ".bytemask1100",
    ".bytemask1101",
    ".bytemask1110",
    ".bytemask1111"
]);
const F_ALRP: Field = fld!(K_ALRP, (45, 1), (41, 1), (44, 1), (46, 1));
const F_FLAGS3: Field = fld!(K_MULFLAGS, (55, 1), (54, 1), (50, 1));
const F_MULFLAGS: Field = fld!(K_MULFLAGS, (55, 1), (54, 1), (50, 1), (44, 3));

/* ---- integer multiply-add: ima8 (19), ima16 (20), imae (21), ima32/idiv (26)
 * The older forms in the main table decode the plain formats; these take the
 * rest. */

/// Which suffixes a source with suffixes has.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Sfx {
    Neg,
    LowHigh,
    Both,
}

/// A source with a suffix: v = operand | neg << nb | low/high << (nb + 1),
/// where the operand (nb bits) is a source 0 or a source 1/2.
struct SrcX {
    src0: bool,
    sfx: Sfx,
}

impl Kind for SrcX {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let nb = if self.src0 { 8 } else { 10 };
        let hasneg = self.sfx != Sfx::LowHigh;
        let haslh = self.sfx != Sfx::Neg;
        let (op, rest) = (v & ((1 << nb) - 1), v >> nb);
        let neg = hasneg && rest & 1 != 0;
        let lh = haslh && rest >> hasneg as u32 & 1 != 0;
        if self.src0 {
            src0(out, op)
        } else {
            srcd(out, op)
        }
        if neg {
            out.push_str(".neg");
        }
        if haslh {
            out.push_str(if lh { ".high" } else { ".low" });
        }
        true
    }
}
const K_S0LH: SrcX = SrcX { src0: true, sfx: Sfx::LowHigh };
const K_SLH: SrcX = SrcX { src0: false, sfx: Sfx::LowHigh };
const K_SNEG: SrcX = SrcX { src0: false, sfx: Sfx::Neg };
const K_SNLH: SrcX = SrcX { src0: false, sfx: Sfx::Both };

/// ima16 source formats: v = format 1 [1:0] | format 2 [3:2]; bit i of the
/// argument set means format i+1 is an 8-bit one (u8, s8, o8), clear that it
/// is u16.
struct Fmt16(u32);

impl Kind for Fmt16 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const F: [&str; 4] = ["u16", "u8", "s8", "o8"];
        let (f1, f2) = (v & 3, v >> 2 & 3);
        if (f1 != 0) != (self.0 & 1 != 0) || (f2 != 0) != (self.0 >> 1 & 1 != 0) {
            return false;
        }
        let _ = write!(out, "{}, {}", F[f1 as usize], F[f2 as usize]);
        true
    }
}

/// imae source-2 type and carries: v = type[1:0] | coe | cie | register; the
/// carry register prints when either carry is used.
struct ImaeC;

impl Kind for ImaeC {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const T: [Option<&str>; 4] = [Some("z16"), Some("u16"), Some("u32"), None];
        let Some(t) = T[(v & 3) as usize] else { return false };
        out.push_str(t);
        if v & 4 != 0 {
            out.push_str(", coe");
        }
        if v & 8 != 0 {
            out.push_str(", cie");
        }
        if v & 12 != 0 {
            let _ = write!(out, ", i{}", v >> 4 & 1);
        }
        true
    }
}

/// ima32 carry-in: 0 none, else register i(v-1).
struct Cie32;

impl Kind for Cie32 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v != 0 {
            let _ = write!(out, ", cie, i{}", v - 1);
        }
        true
    }
}

/// ima32 extra operand after the destination: v = present | register << 1.
struct Ima32X;

impl Kind for Ima32X {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v & 1 != 0 {
            let _ = write!(out, ", i{}", v >> 1);
        }
        true
    }
}

/* ima8: six selectors */
const K_IMA8SEL: Sel = Sel {
    pre: false,
    neg: false,
    it: &[
        item(&["s0", "s1", "s0a", "s1a"], 2, true),
        item(&["s1", "s1a"], 1, true),
        item(&["s2", "s2a"], 1, true),
        item(&["s0a", "s1a"], 1, true),
        item(&["s1a"], 0, true),
        item(&["s2a"], 0, true),
    ],
};
const SAT: Enum = Enum(names!["", ".sat"]);
const F_MSRC0: Field = fld!(K_SRC0, (14, 7), (34, 1));

/* ---- class 24: u8dot3, u8dot4, u16dot3, u16dot4 (and their off forms) */
const DOTFMT: Enum = Enum(names!["u8", "u8", "u8", "u16"]);
const X4: Enum = Enum(names!["1", "2", "4", "8"]);
const OFF: Enum = Enum(names!["", "off"]);
const NOT3: Enum = Enum(names!["", "", "", None]);
const AADD: Enum = Enum(names!["aadd", "asub"]);
const AINTRP: Enum = Enum(names!["aintrp1", "aintrp2"]);
const DOTSEL: &[&str] = &["zero", "s1a", "s2a"];
const K_DOTADD: Sel = Sel {
    pre: false,
    neg: false,
    it: &[item(DOTSEL, 2, true), item(&["s1a"], 0, true), item(DOTSEL, 2, true), item(&["s2a"], 0, false)],
};
const K_DOTINTRP: Sel = Sel { pre: false, neg: false, it: &[item(DOTSEL, 2, false), item(DOTSEL, 2, true)] };

const fn pckop(k: &'static PckOp) -> Field {
    Field {
        k,
        c: &[
            crate::form::Chunk { lo: 38, w: 6 },
            crate::form::Chunk { lo: 55, w: 1 },
            crate::form::Chunk { lo: 52, w: 1 },
            crate::form::Chunk { lo: 54, w: 1 },
            crate::form::Chunk { lo: 50, w: 1 },
            crate::form::Chunk { lo: 18, w: 1 },
        ],
    }
}

macro_rules! pck {
    ($op:expr, $srcc:expr) => {
        [
            form!(
                "{0}{1} {3}{4}{2}, {5}, {6}, {7}",
                CLSM | b(53),
                cls(8),
                [
                    fld!(PRED_ALL, (56, 3)),
                    pckop(&$op),
                    fld!(WMASK, (44, 4)),
                    F_DST,
                    fld!(BYTEMASK, (34, 4)),
                    fld!($srcc, (7, 7), (30, 2), (49, 1), (16, 2)),
                    fld!($srcc, (0, 7), (28, 2), (48, 1), (14, 2)),
                    fld!(ROUND, (19, 2)),
                ]
            ),
            form!(
                "{0}{1}{2} {3}{4}, {5}, {6}, {7}",
                CLSM | b(53),
                cls(8) | b(53),
                [
                    fld!(PRED_ALL, (56, 3)),
                    pckop(&$op),
                    fld!(REPEAT, (44, 4)),
                    F_DST,
                    fld!(BYTEMASK, (34, 4)),
                    fld!($srcc, (7, 7), (30, 2), (49, 1), (16, 2)),
                    fld!($srcc, (0, 7), (28, 2), (48, 1), (14, 2)),
                    fld!(ROUND, (19, 2)),
                ]
            ),
        ]
    };
}
const PCK: [Form; 2] = pck!(K_PCKOP, K_SRCC);
const PCK_ATOMIC: [Form; 2] = pck!(K_STATOMIC, K_SRCC0);

/* class 17: sop3, lrp1, lrp2. enc[53] picks sop3 or lrp, enc[43:42] the alpha
 * operation. The pair of 3-bit selectors enc[40:38], enc[37:35] (complements
 * enc[56], enc[47]) is the colour of sop3 and lrp2 and the alpha of lrp1; the
 * sop3 asop alpha repeats the second colour selector. */
macro_rules! sop3 {
    ($pat:expr, $m:expr, $v:expr, $col:expr, $alpha:expr) => {
        form!(
            $pat,
            CLSM | b(53) | ($m),
            cls(17) | ($v),
            [fld!(PRED_MUL, (57, 2)), F_FLAGS3, F_D10, F_S0_10, F_S1_10, F_S2_10, $col, $alpha,]
        )
    };
}

/* class 20: ima16 with every source format. Formats enc[39:38] (source 1) and
 * enc[41:40] (source 2): u16, u8, s8, o8. An 8-bit source prints its half:
 * enc[52] for source 1, enc[37] for source 2. The oracle reads neither
 * enc[47] nor, for a u16 source 2, enc[37]. */
const FMT16: [Fmt16; 4] = [Fmt16(0), Fmt16(1), Fmt16(2), Fmt16(3)];
macro_rules! ima16 {
    ($m:expr, $s1:expr, $s2:expr, $f:expr) => {
        form!(
            "{0}ima16{1}.{2}{3} {4}{5}, {6}, {7}, {8}, {9}",
            CLSM | ($m),
            cls(20),
            [
                fld!(PRED_MUL, (57, 2)),
                F_MULFLAGS,
                fld!(SIGNSAT, (42, 1), (43, 1)),
                fld!(RS, (35, 2)),
                F_DST,
                fld!(F_ABS, (56, 1)),
                F_MSRC0,
                $s1,
                $s2,
                fld!(FMT16[$f], (38, 4)),
            ]
        )
    };
}
const S16_1: Field = fld!(K_SRCD, (7, 7), (30, 2), (49, 1));
const S8_1: Field = fld!(K_SLH, (7, 7), (30, 2), (49, 1), (52, 1));
const S16_2: Field = fld!(K_SNEG, (0, 7), (28, 2), (48, 1), (53, 1));
const S8_2: Field = fld!(K_SNLH, (0, 7), (28, 2), (48, 1), (53, 1), (37, 1));

/* class 21: imae with every source-2 type and carry. Source-2 type enc[39:38]
 * (z16, u16, u32; the 16-bit ones print the half in enc[52]), carry out
 * enc[40], carry in enc[41] and the carry register enc[47]. */
macro_rules! imae {
    ($m:expr, $v:expr, $s2:expr) => {
        form!(
            "{0}imae{1}.{2}{3} {4}, {5}, {6}, {7}, {8}",
            CLSM | ($m),
            cls(21) | ($v),
            [
                fld!(PRED_MUL, (57, 2)),
                F_MULFLAGS,
                fld!(SIGNSAT, (42, 1), (43, 1)),
                fld!(RS, (35, 3)),
                F_DST,
                fld!(K_S0LH, (14, 7), (34, 1), (56, 1)),
                fld!(K_SLH, (7, 7), (30, 2), (49, 1), (53, 1)),
                $s2,
                fld!(ImaeC, (38, 2), (40, 2), (47, 1)),
            ]
        )
    };
}

/* class 24: dot products. enc[56] dot4, enc[38] off, Cx enc[40:39] and Ax
 * enc[17:16] (1, 2, 4, 8), sources 1 and 2 complemented by enc[43] and
 * enc[47]. A dot3 has an alpha operation unless enc[53:52] (u16) or
 * enc[42:41] is 3; then, as for dot4, Ax follows Cx. The u8dot3 alpha:
 * aadd/asub (enc[37] clear, enc[36]) with Ax and four selectors, or
 * aintrp1/2 with a source-0 operand and two; selectors enc[53:52],
 * enc[42:41]. */
macro_rules! dot {
    ($pat:expr, $m:expr, $v:expr, [$($f:expr),* $(,)?]) => {
        form!($pat, CLSM | ($m), cls(24) | ($v), [fld!(PRED_MUL, (57, 2)), $($f),*])
    };
}
const DOT_OFF: Field = fld!(OFF, (38, 1));
const DOT_CX: Field = fld!(X4, (39, 2));
const DOT_S1: Field = fld!(K_SC10, (7, 7), (30, 2), (49, 1), (43, 1));
const DOT_S2: Field = fld!(K_SC10, (0, 7), (28, 2), (48, 1), (47, 1));

#[rustfmt::skip]
pub(crate) static FORMS: &[Form] = &[
    PCK[0],
    PCK[1],
    PCK_ATOMIC[0],
    PCK_ATOMIC[1],
    /* class 16: sop2. Colour selectors enc[40:38] and enc[37:35] (complemented
     * by enc[56] and enc[47], the whole colour by enc[20]) and operation
     * enc[19:18]; alpha selectors enc[53:52], enc[42:41] (enc[43], enc[34],
     * enc[15]), operation enc[17:16] and enc[14] neg. */
    form!("{0}sop2{1} {2}, {3}, {4}, {5}+asop2 {6}", CLSM, cls(16), [
        fld!(PRED_MUL, (57, 2)), F_MULFLAGS, F_D10, F_S1_10, F_S2_10,
        fld!(K_SOP2C, (20, 1), (38, 3), (56, 1), (35, 3), (47, 1), (18, 2)),
        fld!(K_SOP2A, (15, 1), (52, 2), (43, 1), (41, 2), (34, 1), (16, 2), (14, 1)),
    ]),
    sop3!("{0}sop3{1} {2}, {3}, {4}, {5}, {6}+asop {7}", m(43, 42), 0,
        fld!(K_SOP3C, (38, 3), (56, 1), (35, 3), (47, 1), (52, 1)),
        fld!(K_ASOP, (44, 2), (46, 1), (35, 3), (47, 1), (41, 1))),
    sop3!("{0}sop3{1} {2}, {3}, {4}, {5}, {6}+arsop {7}", m(43, 42), b(43),
        fld!(K_SOP3C, (38, 3), (56, 1), (35, 3), (47, 1), (52, 1)), fld!(K_ARSOP, (44, 2), (46, 1), (41, 1))),
    sop3!("{0}sop3{1} {2}, {3}, {4}, {5}, {6}+alrp {7}", b(42), b(42),
        fld!(K_SOP3CN, (38, 3), (56, 1), (35, 3), (47, 1), (52, 1), (43, 1)), F_ALRP),
    sop3!("{0}lrp1{1} {2}, {3}, {4}, {5}, {6}+asop {7}", b(42), b(53),
        fld!(K_LRP1C, (45, 1), (52, 1), (44, 1), (46, 1), (43, 1)),
        fld!(K_LRP1A, (38, 3), (56, 1), (35, 3), (47, 1), (41, 1))),
    sop3!("{0}lrp2{1} {2}, {3}, {4}, {5}, {6}+alrp {7}", b(42), b(53) | b(42),
        fld!(K_LRP2C, (38, 3), (56, 1), (35, 3), (47, 1), (43, 1), (52, 1)), F_ALRP),
    /* class 18: sop2wm. The selector pair of sop3 (values 4 and 5 invalid), the
     * colour and alpha operations enc[53:52] and enc[42:41], and a byte mask in
     * enc[46:44] (bits 0-2) and enc[43] (bit 3), always printed. The oracle
     * reads none of enc[20:14] and enc[34]. */
    form!("{0}sop2wm{1} {2}{3}, {4}, {5}, {6}", CLSM, cls(18), [
        fld!(PRED_MUL, (57, 2)), F_FLAGS3, F_D10, fld!(BYTEMASK4, (44, 3), (43, 1)), F_S1_10, F_S2_10,
        fld!(SelWm, (38, 3), (56, 1), (35, 3), (47, 1), (52, 2), (41, 2)),
    ]),
    /* class 19: ima8. Source 0 with .neg (enc[35]); six selectors: enc[53:52],
     * enc[37], enc[36], enc[41] and two fixed ones, complemented by enc[42],
     * enc[56], enc[47], enc[38], enc[39], enc[40]; .sat is enc[43]. */
    form!("{0}ima8{1}{2} {3}, {4}, {5}, {6}, {7}", CLSM, cls(19), [
        fld!(PRED_MUL, (57, 2)), F_MULFLAGS, fld!(SAT, (43, 1)), F_D10,
        fld!(K_S0N_10, (14, 7), (34, 1), (35, 1)), F_S1_10, F_S2_10,
        fld!(K_IMA8SEL, (52, 2), (42, 1), (37, 1), (56, 1), (36, 1), (47, 1), (41, 1), (38, 1), (39, 1), (40, 1)),
    ]),
    ima16!(m(41, 38), S16_1, S16_2, 0),
    ima16!(m(41, 40), S8_1, S16_2, 1),
    ima16!(m(39, 38), S16_1, S8_2, 2),
    ima16!(0, S8_1, S8_2, 3),
    imae!(b(39), b(39), F_SRC2D),
    imae!(b(39), 0, fld!(K_SLH, (0, 7), (28, 2), (48, 1), (52, 1))),
    /* class 26: ima32 with carry and extra operand, idiv. ima32: enc[38] adds the
     * operand i<enc[37:36]> after the destination, enc[43:42] the carry in
     * (i0-i2). idiv (enc[52]): destination r or pa (enc[39]), sources 1 and 2,
     * drc enc[32]. */
    form!("{0}ima32{1}.{2} {3}{4}, {5}, {6}, {7}{8}", CLSM | m(53, 52), cls(26), [
        fld!(PRED_ALL, (56, 3)), F_MULFLAGS, fld!(SU, (41, 1)), F_DST, fld!(Ima32X, (38, 1), (36, 2)),
        fld!(K_SRC0, (14, 7), (34, 1), (47, 1)), fld!(K_SNEG, (7, 7), (30, 2), (49, 1), (40, 1)),
        fld!(K_SNEG, (0, 7), (28, 2), (48, 1), (39, 1)), fld!(Cie32, (42, 2)),
    ]),
    form!("{0}idiv{1} {2}, {3}, {4}, drc{5}", CLSM | m(53, 52), cls(26) | b(52), [
        fld!(PRED_ALL, (56, 3)), F_MULFLAGS, F_LSDST, F_SRC1D, F_SRC2D, fld!(K_DEC, (32, 1)),
    ]),
    dot!("{0}{1}dot4{2}{3} {4}, Cx{5}, Ax{6}, {7}, {8}", b(56), b(56), [
        fld!(DOTFMT, (52, 2)), DOT_OFF, F_MULFLAGS, F_D10, DOT_CX, fld!(X4, (16, 2)), DOT_S1, DOT_S2,
    ]),
    dot!("{0}{1}dot3{2}{3} {4}, Cx{5}, Ax{6}, {7}, {8}", b(56) | m(42, 41), m(42, 41), [
        fld!(DOTFMT, (52, 2)), DOT_OFF, F_MULFLAGS, F_D10, DOT_CX, fld!(X4, (16, 2)), DOT_S1, DOT_S2,
    ]),
    dot!("{0}u16dot3{1}{2} {3}, Cx{4}, {5}, {6}{7}", b(56) | m(53, 52), m(53, 52), [
        DOT_OFF, F_MULFLAGS, F_D10, DOT_CX, DOT_S1, DOT_S2, fld!(NOT3, (41, 2)),
    ]),
    dot!("{0}u8dot3{1}{2} {3}, Cx{4}, {5}, {6}+{7} Ax{8}, {9}", b(56) | b(37), 0, [
        DOT_OFF, F_MULFLAGS, F_D10, DOT_CX, DOT_S1, DOT_S2, fld!(AADD, (36, 1)), fld!(X4, (16, 2)),
        fld!(K_DOTADD, (52, 2), (34, 1), (15, 1), (41, 2), (35, 1)),
    ]),
    dot!("{0}u8dot3{1}{2} {3}, Cx{4}, {5}, {6}+{7} {8}, {9}", b(56) | b(37), b(37), [
        DOT_OFF, F_MULFLAGS, F_D10, DOT_CX, DOT_S1, DOT_S2, fld!(AINTRP, (36, 1)), F_S0_10,
        fld!(K_DOTINTRP, (52, 2), (41, 2), (35, 1)),
    ]),
    /* class 14: rlp. The bitwise-group flags, a write mask or (enc[53]) a repeat
     * count, a source 1 and p<enc[20:14]>; the oracle reads no source 2. */
    form!("{0}rlp{1} {2}{3}, {4}, p{5}", CLSM | b(53), cls(14), [
        fld!(PRED_ALL, (56, 3)), F_ALUFLAGS, F_DST, fld!(WMASK, (44, 4)), F_SRC1D, fld!(K_DEC, (14, 7)),
    ]),
    form!("{0}rlp{1}{2} {3}, {4}, p{5}", CLSM | b(53), cls(14) | b(53), [
        fld!(PRED_ALL, (56, 3)), F_ALUFLAGS, fld!(REPEAT, (44, 4)), F_DST, F_SRC1D, fld!(K_DEC, (14, 7)),
    ]),
];
