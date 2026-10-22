// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The float family (tools/usse-dis/forms/float.inc): the multiply-add group,
//! single-source transcendentals, dot products, min/max, fdsx/fdsy, fmad16,
//! efo, fpma and the class-27 dual-issue group.
//!
//! The C table's encoder-side kinds (the operand tables that parse, and the
//! look-ahead kinds of the secondary mov) print as their base kinds; only the
//! printing side is here until the encoder is ported.

use crate::form::{
    CLSM, Core, Ctx, Enum, Field, Flags, Form, Kind, Radix, b, cls, fld, form, m, names, sparse,
};
use crate::kinds::*;
use std::fmt::Write as _;

/* ==== float operands =======================================================
 * A float source is a register operand plus .neg/.abs and, with the
 * instruction's f16 bit (enc[54] in the multiply-add group) set, a half-float
 * reading of the register number: numbers 64-127 then name a half of a 32-bit
 * register, (n - 64) >> 1, printed with a component suffix chosen by n & 1
 * (".flt16.0"/".flt16.2", or ".flt16"/".flt16.2" for fsubflr). The
 * temporaries reach r0-r29 and then i0-i1. Indexed operands read the f16 bit
 * too: the bank is n[5:4], il/ih n[3] and the offset n[2:0], with the
 * first-component suffix when n[6] is set. Constants and immediates are not
 * affected. Swept bank by bank and number by number through refdisasm545. */
pub(crate) struct SrcArg {
    /// format of the immediate bank (after its '#')
    imm: Radix,
    /// the half-float reading
    f16: bool,
    /// its component suffixes
    comp: [&'static str; 2],
}

/// bank: the source-1/2 bank code (0 r, 1 o, 2 pa, 3 sa, 4 indexed, 5 c/g,
/// 6 immediate, 7 i).
fn flt_src(a: &SrcArg, bank: u32, r: u32, neg: bool, abs: bool, out: &mut String) {
    const NAME: [&str; 8] = ["r", "o", "pa", "sa", "", "", "", "i"];
    const IDX: [&str; 4] = ["r", "o", "pa", "sa"];
    let mut suffix = "";
    if a.f16 && bank == 0 && (60..64).contains(&r) {
        let _ = write!(out, "i{}", r - 60);
    } else if !a.f16 || bank == 5 || bank == 6 || (bank != 4 && r < 64) {
        if bank == 6 {
            out.push('#');
            a.imm.put(out, r);
        } else {
            srcd(out, r | bank << 7);
        }
    } else if bank == 4 {
        let _ = write!(
            out,
            "{}[{} + #{}]",
            IDX[(r >> 4 & 3) as usize],
            if r & 8 != 0 { "ih" } else { "il" },
            r & 7
        );
        if r >= 64 {
            suffix = a.comp[0];
        }
    } else {
        let m = (r - 64) >> 1;
        let _ = if bank == 0 && m >= 30 {
            write!(out, "i{}", m - 30)
        } else {
            write!(out, "{}{m}", NAME[bank as usize])
        };
        suffix = a.comp[(r & 1) as usize];
    }
    if neg {
        out.push_str(".neg");
    }
    if abs {
        out.push_str(".abs");
    }
    out.push_str(suffix);
}

/// Source 1 or 2: v = number[6:0] | bank[9:7] | neg[10] | abs[11].
struct Src(&'static SrcArg);

impl Kind for Src {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        flt_src(self.0, v >> 7 & 7, v & 0x7f, v >> 10 & 1 != 0, v >> 11 & 1 != 0, out);
        true
    }
}

/// Source 0: v = number[6:0] | pa[7] | abs[8].
struct Src0(&'static SrcArg);

impl Kind for Src0 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        flt_src(self.0, if v >> 7 & 1 != 0 { 2 } else { 0 }, v & 0x7f, false, v >> 8 & 1 != 0, out);
        true
    }
}

/// .incsXYZ: v = off[0] (enc[53], set: nothing printed) | enc[40] << 1 |
/// enc[46] << 2 | enc[47] << 3, printed as enc[47] enc[46] enc[40].
struct Incs;

impl Kind for Incs {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v & 1 == 0 {
            let _ = write!(out, ".incs{}{}{}", v >> 3 & 1, v >> 2 & 1, v >> 1 & 1);
        }
        true
    }
}

const ARG_32: SrcArg = SrcArg { imm: Radix::Dec, f16: false, comp: ["", ""] };
const ARG_MAD: SrcArg = SrcArg { imm: Radix::Dec, f16: true, comp: [".flt16.0", ".flt16.2"] };
const ARG_FLR: SrcArg = SrcArg { imm: Radix::Dec, f16: true, comp: [".flt16", ".flt16.2"] };
const K_FLT_SRC: Src = Src(&ARG_32);
const K_FLT_SRC0: Src0 = Src0(&ARG_32);
const K_FLT_SRC_MAD16: Src = Src(&ARG_MAD);
const K_FLT_SRC0_MAD16: Src0 = Src0(&ARG_MAD);
const K_FLT_SRC_FLR16: Src = Src(&ARG_FLR);
const K_FLT_SRC0_FLR16: Src0 = Src0(&ARG_FLR);

/// The flags of every float form. The SGX540 reads these classes differently
/// (a write mask, source-0 negation, no .incs), so the float forms are the
/// SGX545's only: on the 540 the flags refuse to print and the word stays
/// undecoded.
struct FltFlags(Flags);

impl Kind for FltFlags {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool {
        cx.core != Core::Sgx540 && self.0.print(v, cx, out)
    }
}
const K_FLT_FLAGS: FltFlags = FltFlags(Flags(&[".skipinv", ".syncs", ".nosched", ".end"]));

const OP_MAD: Enum = Enum(names!["fmad", "fadm", "fmsa", None]); // enc[42:41]
const OP_MINMAX: Enum = Enum(names!["fmaxmin", "fminmax", None, None]);
const OP_DS: Enum = Enum(names!["fdsx", "fdsy", None, None]);
const REPEAT4: Enum = Enum(names!["", ".repeat2", ".repeat3", ".repeat4"]);

const F_FLT_FLAGS: Field = fld!(K_FLT_FLAGS, (55, 1), (52, 1), (43, 1), (50, 1));
const F_FLT_REP2: Field = fld!(REPEAT4, (44, 2));
const F_FLT_INCS: Field = fld!(Incs, (53, 1), (40, 1), (46, 2));
macro_rules! src0 {
    ($k:expr) => {
        fld!($k, (14, 7), (34, 1), (39, 1))
    };
}
macro_rules! src1 {
    ($k:expr) => {
        fld!($k, (7, 7), (30, 2), (49, 1), (37, 1), (38, 1))
    };
}
macro_rules! src2 {
    ($k:expr) => {
        fld!($k, (0, 7), (28, 2), (48, 1), (35, 1), (36, 1))
    };
}
/* ... the same operands without the modifier bits (which the kinds read as 0) */
macro_rules! src0p {
    ($k:expr) => {
        fld!($k, (14, 7), (34, 1))
    };
}
macro_rules! src1p {
    ($k:expr) => {
        fld!($k, (7, 7), (30, 2), (49, 1))
    };
}
macro_rules! src2p {
    ($k:expr) => {
        fld!($k, (0, 7), (28, 2), (48, 1))
    };
}
/// a destination without the bank extension bit (enc[51] used otherwise)
const F_FLT_DST9: Field = fld!(K_DST, (21, 7), (32, 2));
const F_FLT_DST: Field = fld!(K_DST, (21, 7), (32, 2), (51, 1));

/* ==== class 1: single-source transcendentals ================================
 * The source modifiers: v = neg[0] | abs[1] | data type[3:2] | component[5:4]
 * (enc[37], enc[38], enc[40:39], enc[36:35]). Type 1 is a half float, its
 * component enc[35] printed as 0 or 2; type 2 a c10 value with the 2-bit
 * component; type 3 is invalid. */
struct Mods1;

impl Kind for Mods1 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let (t, c) = (v >> 2 & 3, v >> 4 & 3);
        if t == 3 {
            return false;
        }
        if v & 1 != 0 {
            out.push_str(".neg");
        }
        if v & 2 != 0 {
            out.push_str(".abs");
        }
        let _ = match t {
            1 => write!(out, ".flt16.{}", (c & 1) * 2),
            2 => write!(out, ".c10.{c}"),
            _ => Ok(()),
        };
        true
    }
}
const K_FLT_FLAGS1: FltFlags = FltFlags(Flags(&[".skipinv", ".syncs", ".nosched", ".end", ".tpres"]));
const OP_SC: Enum = Enum(names!["frcp", "frsq", "flog", "fexp", "fsqrt", "fsin", "fcos", None]);
const F_FLT_FLAGS1: Field = fld!(K_FLT_FLAGS1, (55, 1), (52, 1), (43, 1), (50, 1), (34, 1));
const F_FLT_MODS1: Field = fld!(Mods1, (37, 2), (39, 2), (35, 2));

/* ==== class 6: fmad16 =======================================================
 * Source modifiers: v = neg[0] | abs[1] | swizzle[3:2], or, for source 0,
 * v = abs[0] | swizzle[2:1] (no neg bit). */
struct M16 {
    src0: bool,
}

impl Kind for M16 {
    fn print(&self, mut v: u32, _: &Ctx, out: &mut String) -> bool {
        const SWZ: [&str; 4] = ["", ".swzll", ".swzhh", ".swzf32"];
        if self.src0 {
            v <<= 1;
        }
        if v & 1 != 0 {
            out.push_str(".neg");
        }
        if v & 2 != 0 {
            out.push_str(".abs");
        }
        out.push_str(SWZ[(v >> 2 & 3) as usize]);
        true
    }
}

/// fmad16's repeat: v = repeat2[0] | enc[40] << 1 | enc[46] << 2 | enc[47] << 3;
/// the .incs field prints only with the repeat.
struct Rep16;

impl Kind for Rep16 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v & 1 != 0 {
            let _ = write!(out, ".repeat2.incs{}{}{}", v >> 3 & 1, v >> 2 & 1, v >> 1 & 1);
        }
        true
    }
}
const K_FLT_FLAGS3: FltFlags = FltFlags(Flags(&[".skipinv", ".syncs", ".nosched"]));

/* ==== class 7: efo ==========================================================
 * The sources print both readings, "plain/half", whenever the half-float
 * reading (as in fmad's .flt16.0/.flt16.2) differs from the plain one, each
 * with its .neg. */
fn efo_src(bank: u32, r: u32, neg: bool, out: &mut String) {
    let (mut a, mut h) = (String::new(), String::new());
    flt_src(&ARG_32, bank, r, neg, false, &mut a);
    flt_src(&ARG_MAD, bank, r, neg, false, &mut h);
    out.push_str(&a);
    if a != h {
        out.push('/');
        out.push_str(&h);
    }
}

/// Source 2: v = number[6:0] | bank[9:7] | neg[10].
struct EfoS2;

impl Kind for EfoS2 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        efo_src(v >> 7 & 7, v & 0x7f, v >> 10 & 1 != 0, out);
        true
    }
}

/// Sources 0 and 1: v = number[6:0] | bank[8:7] | neg[9]; the map takes the
/// bank to the source-1/2 code.
struct EfoS01(&'static [u32; 4]);

impl Kind for EfoS01 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        efo_src(self.0[(v >> 7 & 3) as usize], v & 0x7f, v >> 9 & 1 != 0, out);
        true
    }
}

/// v = skipinv | nosched << 1 | gpiext << 2 | repeat-1 << 3 (2 bits)
struct EfoFlags;

impl Kind for EfoFlags {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool {
        if cx.core == Core::Sgx540 {
            return false;
        }
        for (bit, name) in [(1, ".skipinv"), (2, ".nosched"), (4, ".gpiext")] {
            if v & bit != 0 {
                out.push_str(name);
            }
        }
        if v >> 3 != 0 {
            let _ = write!(out, ".repeat{}", (v >> 3) + 1);
        }
        true
    }
}

/// The internal-register writes: v = select[1:0] (enc[51:50]) | i0 write[2]
/// (enc[42]) | i1 write[3] (enc[41]); a clear write bit prints "!".
struct EfoIw;

impl Kind for EfoIw {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const SEL: [[&str; 2]; 4] = [["a0", "a1"], ["a1", "a0"], ["m0", "m1"], ["a0", "m1"]];
        let s = SEL[(v & 3) as usize];
        let w = |bit: u32| if v & bit != 0 { "" } else { "!" };
        let _ = write!(out, "{}i0 = {}, {}i1 = {}", w(4), s[0], w(8), s[1]);
        true
    }
}

/// The adders: v = select[1:0] (enc[49:48]) | a1 negate[2] (enc[54]) |
/// a0 subtract[3] (enc[58]). Select 3 prints no comma between the two.
struct EfoAdd;

impl Kind for EfoAdd {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const A: [[&str; 4]; 4] = [
            ["m0", "m1", "i1", "i0"],
            ["m0", "src0", "i1", "i0"],
            ["m0", "i0", "i1", "m1"],
            ["src0", "src2", "src1", "src2"],
        ];
        let a = A[(v & 3) as usize];
        let _ = write!(
            out,
            "a0={}{}{}{} a1={}{}+{}",
            a[0],
            if v & 8 != 0 { '-' } else { '+' },
            a[1],
            if v & 3 == 3 { "" } else { "," },
            if v & 4 != 0 { "-" } else { "" },
            a[2],
            a[3]
        );
        true
    }
}
const EFO_S0MAP: [u32; 4] = [0, 2, 1, 3]; // r, pa, o, sa
const EFO_S1MAP: [u32; 4] = [0, 1, 2, 3]; // r, o, pa, sa
const EFO_DSEL: Enum = Enum(names!["i0", "i1", "a0", "a1"]); // enc[53:52]
const EFO_MSEL: Enum = Enum(names![
    "m0=src0*src2, m1=src1*src2",
    "m0=src2*src1, m1=src2*src2",
    "m0=src1*src0, m1=src2*src2",
    "m0=src1*i0, m1=src2*i1"
]); // enc[47:46]

/* ==== class 25: fpma ========================================================
 * Operands read as c10 values: number bit 6 adds .c10 (after .neg) to the
 * register n & 63 (temporaries r0-r59, then i0-i3); indexed operands take the
 * bank from n[5:4], il/ih from n[3] and the offset from n[2:0], as in the
 * half-float reading. Constants, immediates and the destination's i.l/i.h
 * names are not affected. bank: the source-1/2 code, or 8 for i.l/i.h. */
fn flt_c10(bank: u32, r: u32, neg: bool, out: &mut String) {
    const NAME: [&str; 8] = ["r", "o", "pa", "sa", "", "", "", "i"];
    const IDX: [&str; 4] = ["r", "o", "pa", "sa"];
    let m = r & 63;
    match bank {
        8 => return dst(out, 6 << 7 | r),
        5 | 6 => return srcd(out, bank << 7 | r),
        4 => {
            let _ = write!(
                out,
                "{}[{} + #{}]",
                IDX[(r >> 4 & 3) as usize],
                if r & 8 != 0 { "ih" } else { "il" },
                r & 7
            );
        }
        0 if m >= 60 => {
            let _ = write!(out, "i{}", m - 60);
        }
        _ => {
            let _ = write!(out, "{}{m}", NAME[bank as usize]);
        }
    }
    if neg {
        out.push_str(".neg");
    }
    if r & 64 != 0 {
        out.push_str(".c10");
    }
}

/// Destination: v = number[6:0] | bank[9:7], the K_DST banks.
struct C10Dst;

impl Kind for C10Dst {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const MAP: [u32; 8] = [0, 1, 2, 4, 3, 5, 8, 7];
        flt_c10(MAP[(v >> 7 & 7) as usize], v & 0x7f, false, out);
        true
    }
}

/// Source 0: v = number[6:0] | pa[7] | neg[8].
struct C10S0;

impl Kind for C10S0 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        flt_c10(if v >> 7 & 1 != 0 { 2 } else { 0 }, v & 0x7f, v >> 8 & 1 != 0, out);
        true
    }
}

/// Sources 1 and 2: v = number[6:0] | bank[9:7].
struct C10Src;

impl Kind for C10Src {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        flt_c10(v >> 7 & 7, v & 0x7f, false, out);
        true
    }
}

/// v = skipinv | nosched << 1 | end << 2 | repeat-1 << 3 (3 bits) | sat << 6
struct PmaFlags;

impl Kind for PmaFlags {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool {
        if cx.core == Core::Sgx540 {
            return false;
        }
        for (bit, name) in [(1, ".skipinv"), (2, ".nosched"), (4, ".end")] {
            if v & bit != 0 {
                out.push_str(name);
            }
        }
        if v >> 3 & 7 != 0 {
            let _ = write!(out, ".repeat{}", (v >> 3 & 7) + 1);
        }
        if v & 64 != 0 {
            out.push_str(".sat");
        }
        true
    }
}

fn comp(v: u32, bit: u32) -> &'static str {
    if v & bit != 0 { ".comp" } else { "" }
}

/// The colour selections: v = sel0[1:0] (enc[53:52]: s0 s1 s0a s1a) |
/// comp0[2] (enc[42]) | sel1[3] (enc[37]: s1 s1a) | comp1[4] (enc[56]) |
/// sel2[5] (enc[36]: s2 s2a) | comp2[6] (enc[47]).
struct PmaCsel;

impl Kind for PmaCsel {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const S0: [&str; 4] = ["s0", "s1", "s0a", "s1a"];
        let _ = write!(
            out,
            "{}{}, {}{}, {}{}",
            S0[(v & 3) as usize],
            comp(v, 4),
            if v & 8 != 0 { "s1a" } else { "s1" },
            comp(v, 16),
            if v & 32 != 0 { "s2a" } else { "s2" },
            comp(v, 64)
        );
        true
    }
}

/// The alpha selections: v = sel0[0] (enc[41]: s0a s1a) | comp0[1] (enc[38]) |
/// comp1[2] (enc[39]) | comp2[3] (enc[40]).
struct PmaAsel;

impl Kind for PmaAsel {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = write!(
            out,
            "{}{}, s1a{}, s2a{}",
            if v & 1 != 0 { "s1a" } else { "s0a" },
            comp(v, 2),
            comp(v, 4),
            comp(v, 8)
        );
        true
    }
}

/* ==== class 27: dual issue ==================================================
 * (mov, primary or secondary, prints a source immediate in hex) */
const ARG_32H: SrcArg = SrcArg { imm: Radix::Hex, f16: false, comp: ["", ""] };
const ARG_FLRH: SrcArg = SrcArg { imm: Radix::Hex, f16: true, comp: [".flt16", ".flt16.2"] };
const K_FLT_SRCH: Src = Src(&ARG_32H);
const K_FLT_SRC_FLR16H: Src = Src(&ARG_FLRH);

/// A secondary operand. A primary operation (enc[51:50] | enc[58] << 2) on the
/// usual operands, then "+" and a secondary operation (enc[41:38]) on internal
/// registers. A secondary operand is i<sel> for a 2-bit selection, and
/// selection 3 names the primary's source of the same position (its register,
/// without .neg).
///
/// v = the referenced register (w bits) | selection (2 bits), or for mov (a
/// fixed `sel`) the enc[53] bit that picks the primary's source over i<sel>.
struct Ref {
    /// the half-float reading of the reference
    st: &'static SrcArg,
    /// width of the register bits: 8 (source 0) or 10
    w: u32,
    /// mov: the fixed selection
    sel: Option<u32>,
    /// source 0: the destination shares the selection
    twice: bool,
}

impl Kind for Ref {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let (r, s) = (v & ((1 << self.w) - 1), v >> self.w);
        let bank = if self.w == 8 { if r >> 7 & 1 != 0 { 2 } else { 0 } } else { r >> 7 & 7 };
        match self.sel {
            Some(sel) if s & 1 == 0 => {
                let _ = write!(out, "i{sel}");
            }
            Some(_) => flt_src(self.st, bank, r & 0x7f, false, false, out),
            None if s < 3 => {
                let _ = if self.twice { write!(out, "i{s}, i{s}") } else { write!(out, "i{s}") };
            }
            None => {
                if self.twice {
                    out.push_str("i0, ");
                }
                flt_src(self.st, bank, r & 0x7f, false, false, out);
            }
        }
        true
    }
}

/// The six reference kinds of one reading: sources 0-2 and the mov's fixed
/// selections 0-2 (each reading defines all six, as in C; the table uses a
/// subset).
macro_rules! refs {
    ($st:expr, $s0:ident, $s1:ident, $s2:ident, $m0:ident, $m1:ident, $m2:ident) => {
        #[allow(dead_code)]
        const $s0: Ref = Ref { st: &$st, w: 8, sel: None, twice: true };
        #[allow(dead_code)]
        const $s1: Ref = Ref { st: &$st, w: 10, sel: None, twice: false };
        #[allow(dead_code)]
        const $s2: Ref = Ref { st: &$st, w: 10, sel: None, twice: false };
        #[allow(dead_code)]
        const $m0: Ref = Ref { st: &$st, w: 8, sel: Some(0), twice: false };
        #[allow(dead_code)]
        const $m1: Ref = Ref { st: &$st, w: 10, sel: Some(1), twice: false };
        #[allow(dead_code)]
        const $m2: Ref = Ref { st: &$st, w: 10, sel: Some(2), twice: false };
    };
}
refs!(ARG_32, R0_32, R1_32, R2_32, RM0_32, RM1_32, RM2_32);
refs!(ARG_MAD, R0_MAD, R1_MAD, R2_MAD, RM0_MAD, RM1_MAD, RM2_MAD);
refs!(ARG_FLR, R0_FLR, R1_FLR, R2_FLR, RM0_FLR, RM1_FLR, RM2_FLR);
refs!(ARG_32H, R0_32H, R1_32H, R2_32H, RM0_32H, RM1_32H, RM2_32H);
refs!(ARG_FLRH, R0_FLRH, R1_FLRH, R2_FLRH, RM0_FLRH, RM1_FLRH, RM2_FLRH);

/// The primary operation and the flags: v = operation[2:0] | skipinv[3] |
/// syncs[4] | nosched[5] | repeat2[6]; the table names the operations a form
/// takes.
struct Pop(&'static [Option<&'static str>; 8]);

impl Kind for Pop {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool {
        let Some(name) = self.0[(v & 7) as usize] else { return false };
        if cx.core == Core::Sgx540 {
            return false;
        }
        out.push_str(name);
        for (bit, f) in [(8, ".skipinv"), (16, ".syncs"), (32, ".nosched"), (64, ".repeat2")] {
            if v & bit != 0 {
                out.push_str(f);
            }
        }
        true
    }
}
const K_POP4: Pop =
    Pop(&[Some("fmad"), None, Some("fadm"), Some("fmsa"), None, Some("fminmax"), Some("ima32"), None]);
const K_POP4M: Pop =
    Pop(&[Some("fmad"), None, Some("fadm"), Some("fmsa"), None, Some("fminmax"), None, None]);
const K_POP4F: Pop = Pop(&sparse::<8>(&[(6, "ima32")]));
const K_POPMOV: Pop = Pop(&sparse::<8>(&[(1, "mov")]));
const K_POPDDP: Pop = Pop(&sparse::<8>(&[(4, "fddp")]));
const K_POPSSQ: Pop = Pop(&sparse::<8>(&[(7, "fssq")]));

/* The secondary operations, by operand shape and half-float reading. */
const SOP4: Enum =
    Enum(&sparse::<16>(&[(0, "fmad"), (1, "fmad16"), (2, "fmsa"), (4, "fmaxmin"), (14, "ima32")]));
const SOP4M: Enum = Enum(&sparse::<16>(&[(0, "fmad"), (1, "fmad16"), (2, "fmsa"), (4, "fmaxmin")]));
const SOP4F: Enum = Enum(&sparse::<16>(&[(14, "ima32")]));
const SOP3: Enum = Enum(&sparse::<16>(&[(3, "fsubflr"), (11, "fadd"), (12, "fsub"), (13, "fmul")]));
const SOP2: Enum =
    Enum(&sparse::<16>(&[(5, "frcp"), (6, "frsq"), (7, "fexp"), (8, "flog"), (9, "fsin"), (10, "fcos")]));
const SOPMOV: Enum = Enum(&sparse::<16>(&[(15, "mov")]));
const SDST: Enum = Enum(names!["i0", "i1", "i2", "i0"]); // enc[37] | enc[45] << 1

/* The secondary mov names the primary's source <sel> (fixed per form) when
 * its source is not i<sel>. The C encoder gives the primary's source of those
 * forms look-ahead kinds (K_*_L<sel>) that print as their base kind, which is
 * what these forms use here. */

/* Secondary fields (first, so that the primary's sources are written last). */
macro_rules! fd_sop {
    ($e:expr) => {
        fld!($e, (38, 4))
    };
}
const FD_SDST: Field = fld!(SDST, (37, 1), (45, 1));
macro_rules! fd_r0 {
    ($k:expr) => {
        fld!($k, (14, 7), (34, 1), (37, 1), (45, 1))
    };
}
macro_rules! fd_r1 {
    ($k:expr) => {
        fld!($k, (7, 7), (30, 2), (49, 1), (46, 2))
    };
}
macro_rules! fd_r2 {
    ($k:expr) => {
        fld!($k, (0, 7), (28, 2), (48, 1), (42, 1), (53, 1))
    };
}
macro_rules! fd_rm0 {
    ($k:expr) => {
        fld!($k, (14, 7), (34, 1), (53, 1))
    };
}
macro_rules! fd_rm1 {
    ($k:expr) => {
        fld!($k, (7, 7), (30, 2), (49, 1), (53, 1))
    };
}
macro_rules! fd_rm2 {
    ($k:expr) => {
        fld!($k, (0, 7), (28, 2), (48, 1), (53, 1))
    };
}
/* Primary fields */
const FD_PRED: Field = fld!(PRED_MUL, (56, 2));
/// secondary mov: a fixed selection
const FD_MOVM: u64 = CLSM | b(54) | m(47, 46);
macro_rules! fd_pop {
    ($k:expr) => {
        fld!($k, (50, 2), (58, 1), (55, 1), (52, 1), (43, 1), (44, 1))
    };
}
macro_rules! fd_ps0 {
    ($k:expr) => {
        fld!($k, (14, 7), (34, 1))
    };
}
macro_rules! fd_ps1 {
    ($k:expr) => {
        fld!($k, (7, 7), (30, 2), (49, 1), (36, 1))
    };
}
macro_rules! fd_ps2 {
    ($k:expr) => {
        fld!($k, (0, 7), (28, 2), (48, 1), (35, 1))
    };
}

/* Patterns: secondary with three operands (or a destination and two) and a
 * primary with three sources; then with two (or a destination and one); the
 * other primaries' shapes: mov (one source), fddp (i0 and three sources) and
 * fssq (two sources). */
const P4: &str = "{4}{5} {6}, {7}, {8}, {9}+{0} {1}, {2}, {3}";
const P3: &str = "{3}{4} {5}, {6}, {7}, {8}+{0} {1}, {2}";
const M4: &str = "{4}{5} {6}, {7}+{0} {1}, {2}, {3}";
const M3: &str = "{3}{4} {5}, {6}+{0} {1}, {2}";
const D4: &str = "{4}{5} {6}, i0, {7}, {8}, {9}+{0} {1}, {2}, {3}";
const D3: &str = "{3}{4} {5}, i0, {6}, {7}, {8}+{0} {1}, {2}";
const S4: &str = "{4}{5} {6}, {7}, {8}+{0} {1}, {2}, {3}";
const S3: &str = "{3}{4} {5}, {6}, {7}+{0} {1}, {2}";

/// A secondary with three sources (FLT_SOP4*).
macro_rules! d4 {
    ($pat:expr, $v:expr, $sop:expr, $r0:expr, $r1:expr, $r2:expr, $pop:expr $(, $p:expr)*) => {
        form!($pat, CLSM | b(54), cls(27) | ($v), [
            fd_sop!($sop), fd_r0!($r0), fd_r1!($r1), fd_r2!($r2), FD_PRED, fd_pop!($pop), F_FLT_DST9 $(, $p)*
        ])
    };
}
/// A secondary with a destination and two sources (FLT_SOP3).
macro_rules! d3 {
    ($pat:expr, $v:expr, $r1:expr, $r2:expr, $pop:expr $(, $p:expr)*) => {
        form!($pat, CLSM | b(54), cls(27) | ($v), [
            fd_sop!(SOP3), FD_SDST, fd_r1!($r1), fd_r2!($r2), FD_PRED, fd_pop!($pop), F_FLT_DST9 $(, $p)*
        ])
    };
}
/// A secondary with a destination and one source (FLT_SOP2).
macro_rules! d2 {
    ($pat:expr, $v:expr, $r1:expr, $pop:expr $(, $p:expr)*) => {
        form!($pat, CLSM | b(54), cls(27) | ($v), [
            fd_sop!(SOP2), FD_SDST, fd_r1!($r1), FD_PRED, fd_pop!($pop), F_FLT_DST9 $(, $p)*
        ])
    };
}
/// The secondary mov, with its fixed selection's reference field.
macro_rules! dmov {
    ($pat:expr, $v:expr, $rm:expr, $pop:expr $(, $p:expr)*) => {
        form!($pat, FD_MOVM, cls(27) | ($v), [
            fd_sop!(SOPMOV), FD_SDST, $rm, FD_PRED, fd_pop!($pop), F_FLT_DST9 $(, $p)*
        ])
    };
}

#[rustfmt::skip]
pub(crate) static FORMS: &[Form] = &[
    /* ---- class 0: the float multiply-add group ---------------------------- *
     * fmad/fadm/fmsa (enc[42:41] 0-2) read three sources, fsubflr (3) two.
     * enc[54] reads the sources as half floats; enc[53] clear adds .incs. */
    form!("{0}{1}{2}{3}{4} {5}, {6}, {7}, {8}", CLSM | b(54), cls(0), [
        fld!(PRED_ALL, (56, 3)), fld!(OP_MAD, (41, 2)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST,
        src0!(K_FLT_SRC0), src1!(K_FLT_SRC), src2!(K_FLT_SRC),
    ], b(53)),
    form!("{0}fsubflr{1}{2}{3} {4}, {5}, {6}", CLSM | b(54) | m(42, 41), cls(0) | m(42, 41), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST, src1!(K_FLT_SRC),
        src2!(K_FLT_SRC),
    ], b(53)),
    form!("{0}{1}{2}{3}{4} {5}, {6}, {7}, {8}", CLSM | b(54), cls(0) | b(54), [
        fld!(PRED_ALL, (56, 3)), fld!(OP_MAD, (41, 2)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST,
        src0!(K_FLT_SRC0_MAD16), src1!(K_FLT_SRC_MAD16), src2!(K_FLT_SRC_MAD16),
    ], b(53)),
    form!("{0}fsubflr{1}{2}{3} {4}, {5}, {6}", CLSM | b(54) | m(42, 41), cls(0) | b(54) | m(42, 41), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST, src1!(K_FLT_SRC_FLR16),
        src2!(K_FLT_SRC_FLR16),
    ], b(53)),
    /* ---- class 1: frcp frsq flog fexp fsqrt fsin fcos --------------------- *
     * The operation is enc[42:41] | enc[48] << 2; source 1 is the operand.
     * enc[53] clear: a destination write mask in enc[47:44]; set: a repeat
     * count there. */
    form!("{0}{1}{2} {3}{4}, {5}{6}", CLSM | b(53), cls(1), [
        fld!(PRED_ALL, (56, 3)), fld!(OP_SC, (41, 2), (48, 1)), F_FLT_FLAGS1, F_DST, fld!(WMASK, (44, 4)),
        F_SRC1D, F_FLT_MODS1,
    ]),
    form!("{0}{1}{2}{3} {4}, {5}{6}", CLSM | b(53), cls(1) | b(53), [
        fld!(PRED_ALL, (56, 3)), fld!(OP_SC, (41, 2), (48, 1)), F_FLT_FLAGS1, fld!(REPEAT, (44, 4)), F_DST,
        F_SRC1D, F_FLT_MODS1,
    ]),
    /* ---- class 2: the dot products ----------------------------------------- *
     * enc[42:41]: fdp (0; enc[14] set makes it fdpc, with a cp operand in
     * enc[17:15]), fddp (1), fddpc (2); 3 is invalid. fddp/fddpc name an
     * internal register i<enc[51]> and so lose the destination's bank
     * extension; fddpc's two cp operands take the source modifier bits and
     * enc[40], and it has no .incs. enc[54] reads the sources as half floats:
     * fdp prints the first component as .flt16.0, the others as .flt16. */
    form!("{0}fdp{1}{2}{3} {4}, {5}, {6}", CLSM | m(42, 41) | b(54) | b(14), cls(2), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST, src1!(K_FLT_SRC),
        src2!(K_FLT_SRC),
    ], b(53)),
    form!("{0}fdpc{1}{2}{3} {4}, cp{5}, {6}, {7}", CLSM | m(42, 41) | b(54) | b(14), cls(2) | b(14), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST, fld!(K_DEC, (15, 3)),
        src1!(K_FLT_SRC), src2!(K_FLT_SRC),
    ], b(53)),
    form!("{0}fddp{1}{2}{3} {4}, i{5}, {6}, {7}, {8}", CLSM | m(42, 41) | b(54), cls(2) | b(41), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST9, fld!(K_DEC, (51, 1)),
        src0!(K_FLT_SRC0), src1!(K_FLT_SRC), src2!(K_FLT_SRC),
    ], b(53)),
    form!("{0}fddpc{1}{2} {3}, i{4}, cp{5}, cp{6}, {7}, {8}, {9}", CLSM | m(42, 41) | b(54), cls(2) | b(42), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_DST9, fld!(K_DEC, (51, 1)),
        fld!(K_DEC, (35, 3)), fld!(K_DEC, (38, 3)), src0p!(K_FLT_SRC0), src1p!(K_FLT_SRC), src2p!(K_FLT_SRC),
    ]),
    form!("{0}fdp{1}{2}{3} {4}, {5}, {6}", CLSM | m(42, 41) | b(54) | b(14), cls(2) | b(54), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST, src1!(K_FLT_SRC_MAD16),
        src2!(K_FLT_SRC_MAD16),
    ], b(53)),
    form!("{0}fdpc{1}{2}{3} {4}, cp{5}, {6}, {7}", CLSM | m(42, 41) | b(54) | b(14), cls(2) | b(54) | b(14), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST, fld!(K_DEC, (15, 3)),
        src1!(K_FLT_SRC_FLR16), src2!(K_FLT_SRC_FLR16),
    ], b(53)),
    form!("{0}fddp{1}{2}{3} {4}, i{5}, {6}, {7}, {8}", CLSM | m(42, 41) | b(54), cls(2) | b(54) | b(41), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST9, fld!(K_DEC, (51, 1)),
        src0!(K_FLT_SRC0_FLR16), src1!(K_FLT_SRC_FLR16), src2!(K_FLT_SRC_FLR16),
    ], b(53)),
    form!("{0}fddpc{1}{2} {3}, i{4}, cp{5}, cp{6}, {7}, {8}, {9}", CLSM | m(42, 41) | b(54), cls(2) | b(54) | b(42), [
        fld!(PRED_ALL, (56, 3)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_DST9, fld!(K_DEC, (51, 1)),
        fld!(K_DEC, (35, 3)), fld!(K_DEC, (38, 3)), src0p!(K_FLT_SRC0_FLR16), src1p!(K_FLT_SRC_FLR16),
        src2p!(K_FLT_SRC_FLR16),
    ]),
    /* ---- class 3: fmaxmin / fminmax ---------------------------------------- *
     * The class-0 layout, with the operation in enc[41] (enc[42] invalid). */
    form!("{0}{1}{2}{3}{4} {5}, {6}, {7}, {8}", CLSM | b(54), cls(3), [
        fld!(PRED_ALL, (56, 3)), fld!(OP_MINMAX, (41, 2)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST,
        src0!(K_FLT_SRC0), src1!(K_FLT_SRC), src2!(K_FLT_SRC),
    ], b(53)),
    form!("{0}{1}{2}{3}{4} {5}, {6}, {7}, {8}", CLSM | b(54), cls(3) | b(54), [
        fld!(PRED_ALL, (56, 3)), fld!(OP_MINMAX, (41, 2)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST,
        src0!(K_FLT_SRC0_MAD16), src1!(K_FLT_SRC_MAD16), src2!(K_FLT_SRC_MAD16),
    ], b(53)),
    /* ---- class 4: fdsx / fdsy ---------------------------------------------- *
     * Two sources (1 and 2) in the class-0 layout, the operation in enc[41]. */
    form!("{0}{1}{2}{3}{4} {5}, {6}, {7}", CLSM | b(54), cls(4), [
        fld!(PRED_ALL, (56, 3)), fld!(OP_DS, (41, 2)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST,
        src1!(K_FLT_SRC), src2!(K_FLT_SRC),
    ], b(53)),
    form!("{0}{1}{2}{3}{4} {5}, {6}, {7}", CLSM | b(54), cls(4) | b(54), [
        fld!(PRED_ALL, (56, 3)), fld!(OP_DS, (41, 2)), F_FLT_FLAGS, F_FLT_REP2, F_FLT_INCS, F_FLT_DST,
        src1!(K_FLT_SRC_MAD16), src2!(K_FLT_SRC_MAD16),
    ], b(53)),
    /* ---- class 6: fmad16 --------------------------------------------------- *
     * Three sources, each with a swizzle (source 0 in enc[42:41], 1 in
     * enc[45] | enc[50] << 1, 2 in enc[54:53]); enc[44] is a repeat of 2,
     * which brings the .incs field. No .end. */
    form!("{0}fmad16{1}{2} {3}, {4}{5}, {6}{7}, {8}{9}", CLSM, cls(6), [
        fld!(PRED_ALL, (56, 3)), fld!(K_FLT_FLAGS3, (55, 1), (52, 1), (43, 1)),
        fld!(Rep16, (44, 1), (40, 1), (46, 2)), F_DST, src0p!(K_FLT_SRC0),
        fld!(M16 { src0: true }, (39, 1), (41, 2)), F_SRC1D,
        fld!(M16 { src0: false }, (37, 2), (45, 1), (50, 1)), F_SRC2D,
        fld!(M16 { src0: false }, (35, 2), (53, 2)),
    ]),
    /* ---- class 7: efo ------------------------------------------------------ *
     * Every bit is a field: the destination (no bank extension) takes one of
     * i0/i1/a0/a1, the two multipliers, two adders and the internal-register
     * writes are selections, and the three sources have a .neg each. */
    form!("{0}efo{1} {2}= {3}, {4}, {5}, {6}, {7}, {8}, {9}", CLSM, cls(7), [
        fld!(PRED_MUL, (56, 2)), fld!(EfoFlags, (55, 1), (43, 1), (40, 1), (44, 2)), F_FLT_DST9,
        fld!(EFO_DSEL, (52, 2)), fld!(EfoIw, (50, 2), (42, 1), (41, 1)),
        fld!(EfoAdd, (48, 2), (54, 1), (58, 1)), fld!(EFO_MSEL, (46, 2)),
        fld!(EfoS01(&EFO_S0MAP), (14, 7), (34, 1), (38, 1), (37, 1)),
        fld!(EfoS01(&EFO_S1MAP), (7, 7), (30, 2), (36, 1)), fld!(EfoS2, (0, 7), (28, 2), (39, 1), (35, 1)),
    ]),
    /* ---- class 25: fpma ----------------------------------------------------- *
     * Destination and three sources in the c10 reading, then the colour and
     * alpha source selections with their complement (.comp) bits. */
    form!("{0}fpma{1} {2}, {3}, {4}, {5}, {6}, {7}", CLSM, cls(25), [
        fld!(PRED_MUL, (57, 2)), fld!(PmaFlags, (55, 1), (54, 1), (50, 1), (44, 3), (43, 1)),
        fld!(C10Dst, (21, 7), (32, 2), (51, 1)), fld!(C10S0, (14, 7), (34, 1), (35, 1)),
        fld!(C10Src, (7, 7), (30, 2), (49, 1)), fld!(C10Src, (0, 7), (28, 2), (48, 1)),
        fld!(PmaCsel, (52, 2), (42, 1), (37, 1), (56, 1), (36, 1), (47, 1)), fld!(PmaAsel, (41, 1), (38, 3)),
    ]),
    /* ---- class 27: the dual-issue group ------------------------------------ *
     * One form per primary and secondary operand shape and half-float reading
     * (enc[54]); the primary's operation and flags print as one field. The
     * secondary mov's source is i<sel> or, with enc[53] set, the primary's
     * source <sel> (selection 3 is invalid), so it takes one form per
     * selection. */
    d4!(P4, 0, SOP4, R0_32, R1_32, R2_32, K_POP4, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    d3!(P4, 0, R1_32, R2_32, K_POP4, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    d2!(P3, 0, R1_32, K_POP4, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    dmov!(P3, 0, fd_rm0!(RM0_32H), K_POP4, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    dmov!(P3, b(46), fd_rm1!(RM1_32H), K_POP4, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    dmov!(P3, b(47), fd_rm2!(RM2_32H), K_POP4, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    d4!(M4, 0, SOP4, R0_32, R1_32, R2_32, K_POPMOV, fd_ps1!(K_FLT_SRCH)),
    d3!(M4, 0, R1_32, R2_32, K_POPMOV, fd_ps1!(K_FLT_SRCH)),
    d2!(M3, 0, R1_32, K_POPMOV, fd_ps1!(K_FLT_SRCH)),
    dmov!(M3, 0, fd_rm0!(RM0_32H), K_POPMOV, fd_ps1!(K_FLT_SRCH)),
    dmov!(M3, b(46), fd_rm1!(RM1_32H), K_POPMOV, fd_ps1!(K_FLT_SRCH)),
    dmov!(M3, b(47), fd_rm2!(RM2_32H), K_POPMOV, fd_ps1!(K_FLT_SRCH)),
    d4!(D4, 0, SOP4, R0_32, R1_32, R2_32, K_POPDDP, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC),
        fd_ps2!(K_FLT_SRC)),
    d3!(D4, 0, R1_32, R2_32, K_POPDDP, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    d2!(D3, 0, R1_32, K_POPDDP, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    dmov!(D3, 0, fd_rm0!(RM0_32H), K_POPDDP, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    dmov!(D3, b(46), fd_rm1!(RM1_32H), K_POPDDP, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    dmov!(D3, b(47), fd_rm2!(RM2_32H), K_POPDDP, fd_ps0!(K_FLT_SRC0), fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    d4!(S4, 0, SOP4, R0_32, R1_32, R2_32, K_POPSSQ, fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    d3!(S4, 0, R1_32, R2_32, K_POPSSQ, fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    d2!(S3, 0, R1_32, K_POPSSQ, fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    dmov!(S3, 0, fd_rm0!(RM0_32H), K_POPSSQ, fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    dmov!(S3, b(46), fd_rm1!(RM1_32H), K_POPSSQ, fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    dmov!(S3, b(47), fd_rm2!(RM2_32H), K_POPSSQ, fd_ps1!(K_FLT_SRC), fd_ps2!(K_FLT_SRC)),
    /* the half-float reading (enc[54] set) */
    d4!(P4, b(54), SOP4M, R0_MAD, R1_MAD, R2_MAD, K_POP4M, fd_ps0!(K_FLT_SRC0_MAD16),
        fd_ps1!(K_FLT_SRC_MAD16), fd_ps2!(K_FLT_SRC_MAD16)),
    d4!(P4, b(54), SOP4F, R0_FLR, R1_FLR, R2_FLR, K_POP4M, fd_ps0!(K_FLT_SRC0_MAD16),
        fd_ps1!(K_FLT_SRC_MAD16), fd_ps2!(K_FLT_SRC_MAD16)),
    d3!(P4, b(54), R1_FLR, R2_FLR, K_POP4M, fd_ps0!(K_FLT_SRC0_MAD16), fd_ps1!(K_FLT_SRC_MAD16),
        fd_ps2!(K_FLT_SRC_MAD16)),
    d2!(P3, b(54), R1_MAD, K_POP4M, fd_ps0!(K_FLT_SRC0_MAD16), fd_ps1!(K_FLT_SRC_MAD16),
        fd_ps2!(K_FLT_SRC_MAD16)),
    dmov!(P3, b(54), fd_rm0!(RM0_FLRH), K_POP4M, fd_ps0!(K_FLT_SRC0_MAD16), fd_ps1!(K_FLT_SRC_MAD16),
        fd_ps2!(K_FLT_SRC_MAD16)),
    dmov!(P3, b(54) | b(46), fd_rm1!(RM1_FLRH), K_POP4M, fd_ps0!(K_FLT_SRC0_MAD16), fd_ps1!(K_FLT_SRC_MAD16),
        fd_ps2!(K_FLT_SRC_MAD16)),
    dmov!(P3, b(54) | b(47), fd_rm2!(RM2_FLRH), K_POP4M, fd_ps0!(K_FLT_SRC0_MAD16), fd_ps1!(K_FLT_SRC_MAD16),
        fd_ps2!(K_FLT_SRC_MAD16)),
    d4!(P4, b(54), SOP4M, R0_MAD, R1_MAD, R2_MAD, K_POP4F, fd_ps0!(K_FLT_SRC0_FLR16),
        fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    d4!(P4, b(54), SOP4F, R0_FLR, R1_FLR, R2_FLR, K_POP4F, fd_ps0!(K_FLT_SRC0_FLR16),
        fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    d3!(P4, b(54), R1_FLR, R2_FLR, K_POP4F, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    d2!(P3, b(54), R1_MAD, K_POP4F, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    dmov!(P3, b(54), fd_rm0!(RM0_FLRH), K_POP4F, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    dmov!(P3, b(54) | b(46), fd_rm1!(RM1_FLRH), K_POP4F, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    dmov!(P3, b(54) | b(47), fd_rm2!(RM2_FLRH), K_POP4F, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    d4!(M4, b(54), SOP4M, R0_MAD, R1_MAD, R2_MAD, K_POPMOV, fd_ps1!(K_FLT_SRC_FLR16H)),
    d4!(M4, b(54), SOP4F, R0_FLR, R1_FLR, R2_FLR, K_POPMOV, fd_ps1!(K_FLT_SRC_FLR16H)),
    d3!(M4, b(54), R1_FLR, R2_FLR, K_POPMOV, fd_ps1!(K_FLT_SRC_FLR16H)),
    d2!(M3, b(54), R1_MAD, K_POPMOV, fd_ps1!(K_FLT_SRC_FLR16H)),
    dmov!(M3, b(54), fd_rm0!(RM0_FLRH), K_POPMOV, fd_ps1!(K_FLT_SRC_FLR16H)),
    dmov!(M3, b(54) | b(46), fd_rm1!(RM1_FLRH), K_POPMOV, fd_ps1!(K_FLT_SRC_FLR16H)),
    dmov!(M3, b(54) | b(47), fd_rm2!(RM2_FLRH), K_POPMOV, fd_ps1!(K_FLT_SRC_FLR16H)),
    d4!(D4, b(54), SOP4M, R0_MAD, R1_MAD, R2_MAD, K_POPDDP, fd_ps0!(K_FLT_SRC0_FLR16),
        fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    d4!(D4, b(54), SOP4F, R0_FLR, R1_FLR, R2_FLR, K_POPDDP, fd_ps0!(K_FLT_SRC0_FLR16),
        fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    d3!(D4, b(54), R1_FLR, R2_FLR, K_POPDDP, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    d2!(D3, b(54), R1_MAD, K_POPDDP, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    dmov!(D3, b(54), fd_rm0!(RM0_FLRH), K_POPDDP, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    dmov!(D3, b(54) | b(46), fd_rm1!(RM1_FLRH), K_POPDDP, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    dmov!(D3, b(54) | b(47), fd_rm2!(RM2_FLRH), K_POPDDP, fd_ps0!(K_FLT_SRC0_FLR16), fd_ps1!(K_FLT_SRC_FLR16),
        fd_ps2!(K_FLT_SRC_FLR16)),
    d4!(S4, b(54), SOP4M, R0_MAD, R1_MAD, R2_MAD, K_POPSSQ, fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    d4!(S4, b(54), SOP4F, R0_FLR, R1_FLR, R2_FLR, K_POPSSQ, fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    d3!(S4, b(54), R1_FLR, R2_FLR, K_POPSSQ, fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    d2!(S3, b(54), R1_MAD, K_POPSSQ, fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    dmov!(S3, b(54), fd_rm0!(RM0_FLRH), K_POPSSQ, fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    dmov!(S3, b(54) | b(46), fd_rm1!(RM1_FLRH), K_POPSSQ, fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
    dmov!(S3, b(54) | b(47), fd_rm2!(RM2_FLRH), K_POPSSQ, fd_ps1!(K_FLT_SRC_FLR16), fd_ps2!(K_FLT_SRC_FLR16)),
];
