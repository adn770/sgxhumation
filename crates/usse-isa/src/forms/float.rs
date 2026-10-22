// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The float family (tools/usse-dis/forms/float.inc): the multiply-add group,
//! single-source transcendentals, dot products, min/max, fdsx/fdsy, fmad16,
//! efo, fpma and the class-27 dual-issue group.
//!
//! The C table's encoder-side kinds (the operand tables that parse, and the
//! look-ahead kinds of the secondary mov) print as their base kinds; only the
//! printing side is here until the encoder is ported.

use crate::form::{Core, Ctx, Flags, Kind, Radix, sparse};
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
pub(crate) struct Src(&'static SrcArg);

impl Kind for Src {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        flt_src(self.0, v >> 7 & 7, v & 0x7f, v >> 10 & 1 != 0, v >> 11 & 1 != 0, out);
        true
    }
}

/// Source 0: v = number[6:0] | pa[7] | abs[8].
pub(crate) struct Src0(&'static SrcArg);

impl Kind for Src0 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        flt_src(self.0, if v >> 7 & 1 != 0 { 2 } else { 0 }, v & 0x7f, false, v >> 8 & 1 != 0, out);
        true
    }
}

/// .incsXYZ: v = off[0] (enc[53], set: nothing printed) | enc[40] << 1 |
/// enc[46] << 2 | enc[47] << 3, printed as enc[47] enc[46] enc[40].
pub(crate) struct Incs;

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
pub(crate) const K_FLT_SRC: Src = Src(&ARG_32);
pub(crate) const K_FLT_SRC0: Src0 = Src0(&ARG_32);
pub(crate) const K_FLT_SRC_MAD16: Src = Src(&ARG_MAD);
pub(crate) const K_FLT_SRC0_MAD16: Src0 = Src0(&ARG_MAD);
pub(crate) const K_FLT_SRC_FLR16: Src = Src(&ARG_FLR);
pub(crate) const K_FLT_SRC0_FLR16: Src0 = Src0(&ARG_FLR);

/// The flags of every float form. The SGX540 reads these classes differently
/// (a write mask, source-0 negation, no .incs), so the float forms are the
/// SGX545's only: on the 540 the flags refuse to print and the word stays
/// undecoded.
pub(crate) struct FltFlags(Flags);

impl Kind for FltFlags {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool {
        cx.core != Core::Sgx540 && self.0.print(v, cx, out)
    }
}
pub(crate) const K_FLT_FLAGS: FltFlags = FltFlags(Flags(&[".skipinv", ".syncs", ".nosched", ".end"]));

/* ==== class 1: single-source transcendentals ================================
 * The source modifiers: v = neg[0] | abs[1] | data type[3:2] | component[5:4]
 * (enc[37], enc[38], enc[40:39], enc[36:35]). Type 1 is a half float, its
 * component enc[35] printed as 0 or 2; type 2 a c10 value with the 2-bit
 * component; type 3 is invalid. */
pub(crate) struct Mods1;

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
pub(crate) const K_FLT_FLAGS1: FltFlags =
    FltFlags(Flags(&[".skipinv", ".syncs", ".nosched", ".end", ".tpres"]));

/* ==== class 6: fmad16 =======================================================
 * Source modifiers: v = neg[0] | abs[1] | swizzle[3:2], or, for source 0,
 * v = abs[0] | swizzle[2:1] (no neg bit). */
pub(crate) struct M16 {
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
pub(crate) struct Rep16;

impl Kind for Rep16 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v & 1 != 0 {
            let _ = write!(out, ".repeat2.incs{}{}{}", v >> 3 & 1, v >> 2 & 1, v >> 1 & 1);
        }
        true
    }
}
pub(crate) const K_FLT_FLAGS3: FltFlags = FltFlags(Flags(&[".skipinv", ".syncs", ".nosched"]));

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
pub(crate) struct EfoS2;

impl Kind for EfoS2 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        efo_src(v >> 7 & 7, v & 0x7f, v >> 10 & 1 != 0, out);
        true
    }
}

/// Sources 0 and 1: v = number[6:0] | bank[8:7] | neg[9]; the map takes the
/// bank to the source-1/2 code.
pub(crate) struct EfoS01(&'static [u32; 4]);

impl Kind for EfoS01 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        efo_src(self.0[(v >> 7 & 3) as usize], v & 0x7f, v >> 9 & 1 != 0, out);
        true
    }
}

/// v = skipinv | nosched << 1 | gpiext << 2 | repeat-1 << 3 (2 bits)
pub(crate) struct EfoFlags;

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
pub(crate) struct EfoIw;

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
pub(crate) struct EfoAdd;

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
pub(crate) struct C10Dst;

impl Kind for C10Dst {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const MAP: [u32; 8] = [0, 1, 2, 4, 3, 5, 8, 7];
        flt_c10(MAP[(v >> 7 & 7) as usize], v & 0x7f, false, out);
        true
    }
}

/// Source 0: v = number[6:0] | pa[7] | neg[8].
pub(crate) struct C10S0;

impl Kind for C10S0 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        flt_c10(if v >> 7 & 1 != 0 { 2 } else { 0 }, v & 0x7f, v >> 8 & 1 != 0, out);
        true
    }
}

/// Sources 1 and 2: v = number[6:0] | bank[9:7].
pub(crate) struct C10Src;

impl Kind for C10Src {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        flt_c10(v >> 7 & 7, v & 0x7f, false, out);
        true
    }
}

/// v = skipinv | nosched << 1 | end << 2 | repeat-1 << 3 (3 bits) | sat << 6
pub(crate) struct PmaFlags;

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
pub(crate) struct PmaCsel;

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
pub(crate) struct PmaAsel;

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
pub(crate) const K_FLT_SRCH: Src = Src(&ARG_32H);
pub(crate) const K_FLT_SRC_FLR16H: Src = Src(&ARG_FLRH);

/// A secondary operand. A primary operation (enc[51:50] | enc[58] << 2) on the
/// usual operands, then "+" and a secondary operation (enc[41:38]) on internal
/// registers. A secondary operand is i<sel> for a 2-bit selection, and
/// selection 3 names the primary's source of the same position (its register,
/// without .neg).
///
/// v = the referenced register (w bits) | selection (2 bits), or for mov (a
/// fixed `sel`) the enc[53] bit that picks the primary's source over i<sel>.
pub(crate) struct Ref {
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

/// The primary operation and the flags: v = operation[2:0] | skipinv[3] |
/// syncs[4] | nosched[5] | repeat2[6]; the table names the operations a form
/// takes.
pub(crate) struct Pop(&'static [Option<&'static str>; 8]);

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
pub(crate) const K_FLT_POP4: Pop =
    Pop(&[Some("fmad"), None, Some("fadm"), Some("fmsa"), None, Some("fminmax"), Some("ima32"), None]);
pub(crate) const K_FLT_POP4M: Pop =
    Pop(&[Some("fmad"), None, Some("fadm"), Some("fmsa"), None, Some("fminmax"), None, None]);
pub(crate) const K_FLT_POP4F: Pop = Pop(&sparse::<8>(&[(6, "ima32")]));
pub(crate) const K_FLT_POPMOV: Pop = Pop(&sparse::<8>(&[(1, "mov")]));
pub(crate) const K_FLT_POPDDP: Pop = Pop(&sparse::<8>(&[(4, "fddp")]));
pub(crate) const K_FLT_POPSSQ: Pop = Pop(&sparse::<8>(&[(7, "fssq")]));

/* The secondary mov names the primary's source <sel> (fixed per form) when
 * its source is not i<sel>. The C encoder gives the primary's source of those
 * forms look-ahead kinds (K_*_L<sel>) that print as their base kind, which is
 * what these forms use here. */

/* ==== the code kinds the table names (isa/usse/float.toml) ================= */
pub(crate) const K_FLT_SRC0_NP: Src0 = Src0(&ARG_32);
pub(crate) const K_FLT_DST: Dst = Dst;
pub(crate) const K_FLT_INCS: Incs = Incs;
pub(crate) const K_FLT_MODS1: Mods1 = Mods1;
pub(crate) const K_FLT_M16: M16 = M16 { src0: false };
pub(crate) const K_FLT_M16A: M16 = M16 { src0: true };
pub(crate) const K_FLT_REP16: Rep16 = Rep16;
pub(crate) const K_FLT_EFO_S0: EfoS01 = EfoS01(&EFO_S0MAP);
pub(crate) const K_FLT_EFO_S1: EfoS01 = EfoS01(&EFO_S1MAP);
pub(crate) const K_FLT_EFO_S2: EfoS2 = EfoS2;
pub(crate) const K_FLT_EFO_FLAGS: EfoFlags = EfoFlags;
pub(crate) const K_FLT_EFO_IW: EfoIw = EfoIw;
pub(crate) const K_FLT_EFO_ADD: EfoAdd = EfoAdd;
pub(crate) const K_FLT_C10_DST: C10Dst = C10Dst;
pub(crate) const K_FLT_C10_S0: C10S0 = C10S0;
pub(crate) const K_FLT_C10_SRC: C10Src = C10Src;
pub(crate) const K_FLT_PMA_FLAGS: PmaFlags = PmaFlags;
pub(crate) const K_FLT_PMA_CSEL: PmaCsel = PmaCsel;
pub(crate) const K_FLT_PMA_ASEL: PmaAsel = PmaAsel;

/// The secondary operands of one reading: sources 0-2 (the selection in v),
/// or the mov's fixed selections 0-2.
const fn src_ref(st: &'static SrcArg, w: u32, twice: bool) -> Ref {
    Ref { st, w, sel: None, twice }
}
const fn mov_ref(st: &'static SrcArg, w: u32, sel: u32) -> Ref {
    Ref { st, w, sel: Some(sel), twice: false }
}
pub(crate) const K_FLT_REF_S0_32: Ref = src_ref(&ARG_32, 8, true);
pub(crate) const K_FLT_REF_S1_32: Ref = src_ref(&ARG_32, 10, false);
pub(crate) const K_FLT_REF_S2_32: Ref = src_ref(&ARG_32, 10, false);
pub(crate) const K_FLT_REF_S0_MAD: Ref = src_ref(&ARG_MAD, 8, true);
pub(crate) const K_FLT_REF_S1_MAD: Ref = src_ref(&ARG_MAD, 10, false);
pub(crate) const K_FLT_REF_S2_MAD: Ref = src_ref(&ARG_MAD, 10, false);
pub(crate) const K_FLT_REF_S0_FLR: Ref = src_ref(&ARG_FLR, 8, true);
pub(crate) const K_FLT_REF_S1_FLR: Ref = src_ref(&ARG_FLR, 10, false);
pub(crate) const K_FLT_REF_S2_FLR: Ref = src_ref(&ARG_FLR, 10, false);
pub(crate) const K_FLT_REF_M0_32H: Ref = mov_ref(&ARG_32H, 8, 0);
pub(crate) const K_FLT_REF_M1_32H: Ref = mov_ref(&ARG_32H, 10, 1);
pub(crate) const K_FLT_REF_M2_32H: Ref = mov_ref(&ARG_32H, 10, 2);
pub(crate) const K_FLT_REF_M0_FLRH: Ref = mov_ref(&ARG_FLRH, 8, 0);
pub(crate) const K_FLT_REF_M1_FLRH: Ref = mov_ref(&ARG_FLRH, 10, 1);
pub(crate) const K_FLT_REF_M2_FLRH: Ref = mov_ref(&ARG_FLRH, 10, 2);

/* The secondary mov names the primary's source <sel> (fixed per form) when its
 * source is not i<sel>. The C encoder gives the primary's source of those forms
 * look-ahead kinds (K_*_L<sel>) that refuse a text whose reference names another
 * source; they print as their base kind, which is all the decoder needs. */
pub(crate) const K_FLT_SRC0_L0: Src0 = K_FLT_SRC0;
pub(crate) const K_FLT_SRC_L1: Src = K_FLT_SRC;
pub(crate) const K_FLT_SRC_L2: Src = K_FLT_SRC;
pub(crate) const K_FLT_SRCH_L1: Src = K_FLT_SRCH;
pub(crate) const K_FLT_SRC0_MAD16_L0: Src0 = K_FLT_SRC0_MAD16;
pub(crate) const K_FLT_SRC_MAD16_L1: Src = K_FLT_SRC_MAD16;
pub(crate) const K_FLT_SRC_MAD16_L2: Src = K_FLT_SRC_MAD16;
pub(crate) const K_FLT_SRC0_FLR16_L0: Src0 = K_FLT_SRC0_FLR16;
pub(crate) const K_FLT_SRC_FLR16_L1: Src = K_FLT_SRC_FLR16;
pub(crate) const K_FLT_SRC_FLR16_L2: Src = K_FLT_SRC_FLR16;
pub(crate) const K_FLT_SRC_FLR16H_L1: Src = K_FLT_SRC_FLR16H;
