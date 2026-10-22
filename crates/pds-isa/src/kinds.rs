// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The PDS operand kinds written as code (the Rust twins of those in
//! tools/usse-dis/pds.c, under the same names).

use std::fmt::Write as _;
use usse_isa::form::{Ctx, Kind};

const PRED: [Option<&str>; 8] =
    [Some("p0"), Some("p1"), Some("p2"), Some("if0"), Some("if1"), Some("aluz"), Some("alun"), None];

/// movs/movsa destinations; 10-14 are reserved.
const MOVS_DST: [Option<&str>; 16] = [
    Some("mr"),
    Some("slc"),
    Some("douti"),
    Some("doutd"),
    Some("doutt"),
    Some("doutu"),
    Some("douta"),
    Some("tim"),
    Some("doutc"),
    Some("douts"),
    None,
    None,
    None,
    None,
    None,
    Some("ir0"),
];

/// Input/special register by index. The mov16/mov32 source fields also have
/// pc, which shifts the numbering; other groups expose only a prefix.
fn special(i: u32, with_pc: bool) -> Option<&'static str> {
    const PLAIN: [&str; 4] = ["ir0", "ir1", "tim", "ir2"];
    const WITHPC: [&str; 5] = ["ir0", "ir1", "pc", "tim", "ir2"];
    if with_pc { WITHPC.get(i as usize).copied() } else { PLAIN.get(i as usize).copied() }
}

fn hl(v: u32) -> char {
    if v & 1 != 0 { 'h' } else { 'l' }
}

/// Predicate w[26:24] as a "(p0) " prefix; 7 (always) prints nothing.
pub(crate) struct Pred;

impl Kind for Pred {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if let Some(p) = PRED[v as usize & 7] {
            let _ = write!(out, "({p}) ");
        }
        true
    }
}

pub(crate) struct Port;

impl Kind for Port {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        match MOVS_DST[v as usize & 15] {
            Some(p) => {
                out.push_str(p);
                true
            }
            None => false,
        }
    }
}

/// movs/movsa sources, v = w[23:5]: four 2-bit selectors [7:0] (slot s at 2s:
/// bit 0 the high half, bit 1 ds1), the ds1 pair [12:8], the ds0 pair or input
/// register [17:13], and [18] which of the two that field names. Slots print
/// from 3 down to 0.
pub(crate) struct Movs;

impl Kind for Movs {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let (b1, b0, ir) = ((v >> 8 & 31) * 2, v >> 13 & 31, v >> 18 & 1 != 0);
        let uses_base = (0..4).any(|s| v >> (2 * s + 1) & 1 == 0);
        if ir && uses_base && special(b0, false).is_none() {
            return false;
        }
        for s in (0..4).rev() {
            let (sel, hi) = (v >> (2 * s) & 3, v >> (2 * s) & 1);
            if s != 3 {
                out.push_str(", ");
            }
            let _ = if sel & 2 != 0 {
                write!(out, "ds1[{}]", b1 + hi)
            } else if ir {
                write!(out, "{}.{}", special(b0, false).unwrap_or(""), hl(hi))
            } else {
                write!(out, "ds0[{}]", b0 * 2 + hi)
            };
        }
        true
    }
}

/// 7-bit destination ds0[0-63], ds1[0-63].
pub(crate) struct Dst7;

impl Kind for Dst7 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = write!(out, "ds{}[{}]", v >> 6, v & 63);
        true
    }
}

/// 7-bit first source: ds0[0-63], then the first n input registers.
pub(crate) struct Src7(u32);

impl Kind for Src7 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v < 64 {
            let _ = write!(out, "ds0[{v}]");
        } else if v - 64 < self.0 {
            out.push_str(special(v - 64, false).unwrap_or(""));
        } else {
            return false;
        }
        true
    }
}

/// mov16 source, 9 bits in 16-bit halves: ds0, ds1, then specials with pc.
pub(crate) struct Src9h;

impl Kind for Src9h {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let r = v >> 1;
        if r < 128 {
            let _ = write!(out, "ds{}[{}].{}", r >> 6, r & 63, hl(v));
        } else if let Some(s) = special(r - 128, true) {
            let _ = write!(out, "{s}.{}", hl(v));
        } else {
            return false;
        }
        true
    }
}

/// mov32 source, 8 bits: ds0, ds1, then specials with pc.
pub(crate) struct Src8;

impl Kind for Src8 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v < 128 {
            let _ = write!(out, "ds{}[{}]", v >> 6, v & 63);
        } else if let Some(s) = special(v - 128, true) {
            out.push_str(s);
        } else {
            return false;
        }
        true
    }
}

/// mov64 source, 7 bits: even pairs of ds0/ds1, then mr01, mr23.
pub(crate) struct Src64;

impl Kind for Src64 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        match v {
            0..64 => {
                let _ = write!(out, "ds{}[{}]", v >> 5, (v & 31) * 2);
            }
            64 => out.push_str("mr01"),
            65 => out.push_str("mr23"),
            _ => return false,
        }
        true
    }
}

/// mov128 source pair, 5 bits, below 16.
pub(crate) struct Src128;

impl Kind for Src128 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v >= 16 {
            return false;
        }
        let _ = write!(out, "{}", v * 2);
        true
    }
}

/// load/store registers: v = first[3:0] (from ds48) | count[6:4] (1-8, 0 is 8).
pub(crate) struct LdRange;

impl Kind for LdRange {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let (r, cnt) = (48 + (v & 15), (v >> 4).wrapping_sub(1) & 7);
        let _ = if cnt != 0 { write!(out, "{r}-{}]", r + cnt) } else { write!(out, "{r}]") };
        true
    }
}

/// mul first source, 8 bits in halves: ds0, then ir0/ir1/tim.
pub(crate) struct MulA;

impl Kind for MulA {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v < 128 {
            let _ = write!(out, "ds0[{}].{}", v >> 1, hl(v));
        } else if (v - 128) >> 1 < 3 {
            let _ = write!(out, "{}.{}", special((v - 128) >> 1, false).unwrap_or(""), hl(v));
        } else {
            return false;
        }
        true
    }
}

pub(crate) struct Half;

impl Kind for Half {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = write!(out, "ds1[{}].{}", v >> 1, hl(v));
        true
    }
}

/// shl/shr source, 8 bits: ds0/ds1, then ir0, ir1.
pub(crate) struct ShSrc;

impl Kind for ShSrc {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v < 128 {
            let _ = write!(out, "ds{}[{}]", v >> 6, v & 63);
        } else if v - 128 < 2 {
            out.push_str(special(v - 128, false).unwrap_or(""));
        } else {
            return false;
        }
        true
    }
}

/// Signed 7-bit shift amount, printed as an unsigned 32-bit number.
pub(crate) struct ShAmt;

impl Kind for ShAmt {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = write!(out, "{}", (((v << 25) as i32) >> 25) as u32);
        true
    }
}

pub(crate) const K_PRED: Pred = Pred;
pub(crate) const K_PORT: Port = Port;
pub(crate) const K_MOVS: Movs = Movs;
pub(crate) const K_DST7: Dst7 = Dst7;
pub(crate) const K_SRC7_2: Src7 = Src7(2);
pub(crate) const K_SRC7_3: Src7 = Src7(3);
pub(crate) const K_SRC9H: Src9h = Src9h;
pub(crate) const K_SRC8: Src8 = Src8;
pub(crate) const K_SRC64: Src64 = Src64;
pub(crate) const K_SRC128: Src128 = Src128;
pub(crate) const K_LDRANGE: LdRange = LdRange;
pub(crate) const K_MULA: MulA = MulA;
pub(crate) const K_HALF: Half = Half;
pub(crate) const K_SHSRC: ShSrc = ShSrc;
pub(crate) const K_SHAMT: ShAmt = ShAmt;
