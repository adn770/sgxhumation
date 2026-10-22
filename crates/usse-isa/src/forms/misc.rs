// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The misc family (tools/usse-dis/forms/misc.inc): movc/movmsk, the rest of
//! the test group (classes 9 and 15), adif/adifsum/bilin/firv/firh.

use crate::form::{Ctx, Kind};
use crate::kinds::*;
use std::fmt::Write as _;

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
pub(crate) struct TestMsk;

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
pub(crate) enum FMode {
    Flt16,
    /// a source prints .0 for the first component
    Flt16_0,
    C10,
}

/// A float operand with enc[50] set: v = number[6:0] | bank[8:7] | ext[9],
/// banks numbered as for K_DST (dst) or K_SRCD. Temporaries past r59 (r29 for
/// .flt16) print as i0-i3; an indexed operand takes a 6-bit index ([5:4]
/// bank, [3] il/ih, [2:0] offset) and no component.
pub(crate) struct FOp {
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
pub(crate) const K_MSC_FDST: FOp = FOp { dst: true, mode: FMode::Flt16 };
pub(crate) const K_MSC_FSRC: FOp = FOp { dst: false, mode: FMode::Flt16 };
pub(crate) const K_MSC_FSRC0: FOp = FOp { dst: false, mode: FMode::Flt16_0 };
pub(crate) const K_MSC_CDST: FOp = FOp { dst: true, mode: FMode::C10 };
pub(crate) const K_MSC_CSRC: FOp = FOp { dst: false, mode: FMode::C10 };

/// firh: v = edge mode[1:0] | filter coefficient set[4:2]
pub(crate) struct Firh;

impl Kind for Firh {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const EDGE: [&str; 4] = ["rep", "msc", "mdc", "reserved"];
        let _ = write!(out, "{}, fcs{}", EDGE[(v & 3) as usize], v >> 2);
        true
    }
}

/// A 5-bit two's-complement number.
pub(crate) struct S5;

impl Kind for S5 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = write!(out, "{}", v as i32 - if v & 16 != 0 { 32 } else { 0 });
        true
    }
}

/// firv/firh operands 2 and 3: v = number[6:0] (enc[20:14]) | source-1
/// number[13:7] | the shared bank[16:14], printed as two K_SRCD operands. One
/// field, so that the assembler cannot pick two different banks.
pub(crate) struct Pair;

impl Kind for Pair {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let bank = v >> 14 << 7;
        srcd(out, bank | (v & 0x7f));
        out.push_str(", ");
        srcd(out, bank | (v >> 7 & 0x7f));
        true
    }
}
pub(crate) const K_MSC_TESTMSK: TestMsk = TestMsk;
pub(crate) const K_MSC_FIRH: Firh = Firh;
pub(crate) const K_MSC_S5: S5 = S5;
pub(crate) const K_MSC_PAIR: Pair = Pair;
