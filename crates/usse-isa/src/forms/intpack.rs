// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The intpack family (tools/usse-dis/forms/intpack.inc): pack/unpack, the
//! colour/alpha selection ops (sop2, sop3, lrp, sop2wm), ima8 and the integer
//! multiply-adds in every format, dot products and rlp.

use crate::form::{Ctx, Kind, names, sparse};
use crate::kinds::*;
use std::fmt::Write as _;

/// A source operand with a component (byte or half) select after it:
/// v = operand[9:0] | select[11:10]. With `zero_plain`, select 0 prints
/// nothing.
pub(crate) struct SrcC {
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
pub(crate) const K_INT_SRCC: SrcC = SrcC { zero_plain: false };
pub(crate) const K_INT_SRCC0: SrcC = SrcC { zero_plain: true };

/// The operation and its flags: v = format[5:0] | skipinv | syncs << 1 |
/// nosched << 2 | end << 3 | enc[18] << 4, which reads .renable for the
/// float-to-float conversions and .scale for the others.
pub(crate) struct PckOp(&'static [Option<&'static str>; 64]);

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
/// The formats by enc[43:38]: destination and source together.
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
const STATOMIC: [Option<&str>; 64] = sparse::<64>(&[(8, "statomic")]);
pub(crate) const K_INT_PCKOP: PckOp = PckOp(&PCKFMT);
pub(crate) const K_INT_STATOMIC: PckOp = PckOp(&STATOMIC);

/* ---- classes 16-19, 24: sop2, sop3, lrp, sop2wm, ima8, dot products -------
 * These name registers with six bits and a seventh, .c10, after the name:
 * temporaries r0-r59 then i0-i3, indexed [5:4] bank, [3] il/ih, [2:0]
 * offset. Constants, immediates and i.l/i.h keep all seven bits. */

/// The bank numbering of a c10 operand.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Banks {
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
pub(crate) enum Mod {
    None,
    Neg,
    Comp,
}

pub(crate) struct C10(Banks, Mod);

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

pub(crate) const K_INT_D10: C10 = C10(Banks::Dst, Mod::None);
pub(crate) const K_INT_S10: C10 = C10(Banks::Src, Mod::None);
pub(crate) const K_INT_S0_10: C10 = C10(Banks::Src0, Mod::None);
pub(crate) const K_INT_S0N_10: C10 = C10(Banks::Src0, Mod::Neg);
pub(crate) const K_INT_SC10: C10 = C10(Banks::Src, Mod::Comp);

/* ---- integer multiply-add: ima8 (19), ima16 (20), imae (21), ima32/idiv (26)
 * The older forms in the main table decode the plain formats; these take the
 * rest. */

/// Which suffixes a source with suffixes has.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Sfx {
    Neg,
    LowHigh,
    Both,
}

/// A source with a suffix: v = operand | neg << nb | low/high << (nb + 1),
/// where the operand (nb bits) is a source 0 or a source 1/2.
pub(crate) struct SrcX {
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
pub(crate) const K_INT_S0LH: SrcX = SrcX { src0: true, sfx: Sfx::LowHigh };
pub(crate) const K_INT_SLH: SrcX = SrcX { src0: false, sfx: Sfx::LowHigh };
pub(crate) const K_INT_SNEG: SrcX = SrcX { src0: false, sfx: Sfx::Neg };
pub(crate) const K_INT_SNLH: SrcX = SrcX { src0: false, sfx: Sfx::Both };

/// ima16 source formats: v = format 1 [1:0] | format 2 [3:2]; bit i of the
/// argument set means format i+1 is an 8-bit one (u8, s8, o8), clear that it
/// is u16.
pub(crate) struct Fmt16(u32);

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
pub(crate) struct ImaeC;

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
pub(crate) struct Cie32;

impl Kind for Cie32 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v != 0 {
            let _ = write!(out, ", cie, i{}", v - 1);
        }
        true
    }
}

/// ima32 extra operand after the destination: v = present | register << 1.
pub(crate) struct Ima32X;

impl Kind for Ima32X {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v & 1 != 0 {
            let _ = write!(out, ", i{}", v >> 1);
        }
        true
    }
}
pub(crate) const K_INT_FMT16_0: Fmt16 = Fmt16(0);
pub(crate) const K_INT_FMT16_1: Fmt16 = Fmt16(1);
pub(crate) const K_INT_FMT16_2: Fmt16 = Fmt16(2);
pub(crate) const K_INT_FMT16_3: Fmt16 = Fmt16(3);
pub(crate) const K_INT_IMAEC: ImaeC = ImaeC;
pub(crate) const K_INT_CIE32: Cie32 = Cie32;
pub(crate) const K_INT_IMA32X: Ima32X = Ima32X;
