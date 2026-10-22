// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The memctl family (tools/usse-dis/forms/memctl.inc): texture sampling,
//! eld, the class-31 flow, MOE, other, visibility and cache-control groups.

use crate::form::{Ctx, Kind, parse_uint};

/// A signed number: an optional '-' and an unsigned one.
fn parse_int(s: &str, cx: &Ctx) -> Option<(i64, usize)> {
    let neg = s.starts_with('-');
    let (u, n) = parse_uint(&s[neg as usize..], cx)?;
    Some((if neg { -(u as i64) } else { u as i64 }, n + neg as usize))
}
use std::fmt::Write as _;

/// Signed decimal of the given width.
pub(crate) struct SNum(u32);

impl Kind for SNum {
    fn parses(&self) -> bool {
        true
    }

    fn parse(&self, s: &str, cx: &Ctx) -> Option<(u32, usize)> {
        let w = self.0;
        let (x, n) = parse_int(s, cx)?;
        let half = 1i64 << (w - 1);
        (-half..half).contains(&x).then_some(((x as u32) & ((1 << w) - 1), n))
    }

    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let w = self.0;
        let x = if v & 1 << (w - 1) != 0 { v as i32 - (1i32 << w) } else { v as i32 };
        let _ = write!(out, "{x}");
        true
    }
}
pub(crate) const MEM_K_S10: SNum = SNum(10);

/// imo's 12-bit immediates print as unsigned while bit 6 is clear and as the
/// sign-extended 7-bit value -64..-1 when it is set (the bits above it do not
/// show then; the encoder writes them as 0).
pub(crate) struct Imo;

impl Kind for Imo {
    fn parses(&self) -> bool {
        true
    }

    fn parse(&self, s: &str, cx: &Ctx) -> Option<(u32, usize)> {
        let (x, n) = parse_int(s, cx)?;
        if !(-64..4096).contains(&x) || (x >= 0 && x & 0x40 != 0) {
            return None;
        }
        Some((if x < 0 { (x + 128) as u32 } else { x as u32 }, n))
    }

    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = if v & 0x40 != 0 { write!(out, "{}", (v & 0x7f) as i32 - 128) } else { write!(out, "{v}") };
        true
    }
}

/// smlsi operand: v = value[7:0] | swizzle mode[8]: a signed increment, or
/// four 2-bit components, x first in the low bits.
pub(crate) struct Smlsi;

impl Kind for Smlsi {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const XYZW: [char; 4] = ['x', 'y', 'z', 'w'];
        let c = |s: u32| XYZW[(v >> s & 3) as usize];
        let _ = if v >> 8 != 0 {
            write!(out, "swizzle({}{}{}{})", c(0), c(2), c(4), c(6))
        } else {
            write!(out, "#{}", if v & 0x80 != 0 { v as i32 - 256 } else { v as i32 })
        };
        true
    }
}
pub(crate) const MEM_K_IMO: Imo = Imo;
pub(crate) const MEM_K_SMLSI: Smlsi = Smlsi;
