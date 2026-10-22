// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The operand kinds, enumerations and fields shared by the whole table (the
//! first half of tools/usse-dis/usse.c).
//!
//! The USSE names a register by a 7-bit number plus bank bits, and the banks
//! read differently for destinations and sources. These printers were derived
//! by sweeping every number and bank value through refdisasm545 (limm and mov
//! destinations, mov/emitpix/phas sources): the same rules hold everywhere they
//! have been checked, and the random cross-checks keep them honest per form.

use crate::form::{Core, Ctx, Enum, Field, Flags, Kind, Num, Radix, fld, names, sparse};
use std::fmt::Write as _;

/// Bank 0: temporaries r0-r123, then the internal registers i0-i3.
pub(crate) fn print_temp(out: &mut String, r: u32) {
    let _ = if r < 124 { write!(out, "r{r}") } else { write!(out, "i{}", r - 124) };
}

/// Indexed: [6:5] the bank (r, o, pa, sa), [4] il/ih, [3:0] the offset.
pub(crate) fn print_indexed(out: &mut String, r: u32) {
    const B: [&str; 4] = ["r", "o", "pa", "sa"];
    let _ =
        write!(out, "{}[{} + #{}]", B[(r >> 5) as usize & 3], if r & 16 != 0 { "ih" } else { "il" }, r & 15);
}

/// c0-c63 constants, then g0-g63 globals.
pub(crate) fn print_const(out: &mut String, r: u32) {
    let _ = if r < 64 { write!(out, "c{r}") } else { write!(out, "g{}", r - 64) };
}

/// Destination: v = number[6:0] | bank[8:7] | ext[9].
pub(crate) struct Dst;

impl Kind for Dst {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        dst(out, v);
        true
    }
}

pub(crate) fn dst(out: &mut String, v: u32) {
    let r = v & 0x7f;
    let _ = match v >> 7 {
        0 => {
            print_temp(out, r);
            Ok(())
        }
        1 => write!(out, "o{r}"),
        2 => write!(out, "pa{r}"),
        3 => {
            print_indexed(out, r);
            Ok(())
        }
        4 => write!(out, "sa{r}"),
        5 => {
            print_const(out, r);
            Ok(())
        }
        6 => write!(out, "i.{}{}", if r & 1 != 0 { "l" } else { "" }, if r & 2 != 0 { "h" } else { "" }),
        _ => write!(out, "i{r}"),
    };
}

/// Source 1 or 2: v = number[6:0] | bank[8:7] | ext[9]; the radix is the
/// format of the 7-bit immediate (bank 2 with ext), which differs per
/// instruction.
pub(crate) struct Src(pub Radix);

impl Kind for Src {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        src(out, self.0, v);
        true
    }
}

pub(crate) fn src(out: &mut String, imm: Radix, v: u32) {
    let r = v & 0x7f;
    let _ = match v >> 7 {
        0 => {
            print_temp(out, r);
            Ok(())
        }
        1 => write!(out, "o{r}"),
        2 => write!(out, "pa{r}"),
        3 => write!(out, "sa{r}"),
        4 => {
            print_indexed(out, r);
            Ok(())
        }
        5 => {
            print_const(out, r);
            Ok(())
        }
        6 => {
            out.push('#');
            imm.put(out, r);
            Ok(())
        }
        _ => write!(out, "i{r}"),
    };
}

/// A source 1/2 with a decimal immediate (K_SRCD).
pub(crate) fn srcd(out: &mut String, v: u32) {
    src(out, Radix::Dec, v)
}

/// Source 0: v = number[6:0] | bank[7] | ext[8].
pub(crate) struct Src0;

impl Kind for Src0 {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        src0(out, v);
        true
    }
}

pub(crate) fn src0(out: &mut String, v: u32) {
    const B: [&str; 4] = ["", "pa", "o", "sa"];
    let r = v & 0x7f;
    if v >> 7 == 0 {
        print_temp(out, r);
    } else {
        let _ = write!(out, "{}{r}", B[(v >> 7) as usize & 3]);
    }
}

/// Rotated immediate of and/or/xor: v = mantissa[15:0] | rotate[20:16].
pub(crate) struct RotImm;

impl Kind for RotImm {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = write!(out, "0x{:08X}", (v & 0xffff).rotate_left(v >> 16 & 31));
        true
    }
}

/// br target: a signed 18-bit instruction offset (the SGX545 PC width),
/// printed by magnitude.
pub(crate) struct BrTarget;

impl Kind for BrTarget {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = if v & 0x20000 != 0 {
            write!(out, "-#0x{:08X}", (1u32 << 18).wrapping_sub(v))
        } else {
            write!(out, "#0x{v:08X}")
        };
        true
    }
}

/* Load/store flags. Common part: skipinv, syncs, the 2-bit cache mode, and
 * enc[53] with the 4-bit count enc[47:44]: clear, a fetch of count+1
 * registers; set, a repeat count (count+1, nothing for 0). */

/// v = enc[33] | enc[51] << 1 on the SGX545; the SGX540 swaps the bits.
fn ls_cache(cx: &Ctx, mut v: u32) -> &'static str {
    const T: [&str; 4] = ["", ".bpcache", ".fcfill", ".bypassl1"];
    if cx.core == Core::Sgx540 {
        v = (v & 1) << 1 | v >> 1;
    }
    T[v as usize & 3]
}

fn ls_count(out: &mut String, moe: u32, cnt: u32) {
    if moe == 0 {
        let _ = write!(out, ".fetch{}", cnt + 1);
    } else if cnt != 0 {
        let _ = write!(out, ".repeat{}", cnt + 1);
    }
}

fn flag(out: &mut String, v: u32, bit: u32, name: &str) {
    if v >> bit & 1 != 0 {
        out.push_str(name);
    }
}

/// v = skipinv | syncs << 1 | cache << 2 | moe << 4 | count << 5
pub(crate) struct LdFlags;

impl Kind for LdFlags {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool {
        flag(out, v, 0, ".skipinv");
        flag(out, v, 1, ".syncs");
        out.push_str(ls_cache(cx, v >> 2 & 3));
        ls_count(out, v >> 4 & 1, v >> 5 & 15);
        true
    }
}

/// v = skipinv | syncs << 1 | nosched << 2 | cache << 3 | noread << 5 |
///     rangeenable << 6 | moe << 7 | count << 8
pub(crate) struct StFlags;

impl Kind for StFlags {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool {
        flag(out, v, 0, ".skipinv");
        flag(out, v, 1, ".syncs");
        flag(out, v, 2, ".nosched");
        out.push_str(ls_cache(cx, v >> 3 & 3));
        flag(out, v, 5, ".noread");
        flag(out, v, 6, ".rangeenable");
        ls_count(out, v >> 7 & 1, v >> 8 & 15);
        true
    }
}

/// Load destination: v = number[6:0] | pa[7]; r124-r127 keep their names.
pub(crate) struct LsDst;

impl Kind for LsDst {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = write!(out, "{}{}", if v >> 7 != 0 { "pa" } else { "r" }, v & 0x7f);
        true
    }
}

/// Load range: v = source-2 operand[9:0] | present[10].
pub(crate) struct Range;

impl Kind for Range {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        if v >> 10 != 0 {
            out.push_str(", ");
            srcd(out, v & 0x3ff);
        }
        true
    }
}

pub(crate) struct Temp;

impl Kind for Temp {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        print_temp(out, v);
        true
    }
}

/// Branch flags: v = nosched | sync << 1 | nt << 2 | savelink << 3 | mon << 4.
/// With sync set, enc[44] picks .syncent, which prints after the others.
pub(crate) struct BrFlags;

impl Kind for BrFlags {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let (sync, nt) = (v >> 1 & 1 != 0, v >> 2 & 1 != 0);
        flag(out, v, 0, ".nosched");
        if sync && !nt {
            out.push_str(".synce");
        }
        flag(out, v, 3, ".savelink");
        flag(out, v, 4, ".mon");
        if sync && nt {
            out.push_str(".syncent");
        }
        true
    }
}

/// Multiply flags: v = skipinv | nosched << 1 | end << 2 | repeat-1 << 3.
pub(crate) struct MulFlags;

impl Kind for MulFlags {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        flag(out, v, 0, ".skipinv");
        flag(out, v, 1, ".nosched");
        flag(out, v, 2, ".end");
        if v >> 3 != 0 {
            let _ = write!(out, ".repeat{}", (v >> 3) + 1);
        }
        true
    }
}

/// Test type enc[43:36]: sign test [7:6] (t/n/p), all-or-any [3], zero test
/// [5:4] (t/z/nz) and the channel selection [2:0].
pub(crate) struct Test;

impl Kind for Test {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        const SIGN: [Option<&str>; 4] = [Some("t"), Some("n"), Some("p"), None];
        const ZERO: [Option<&str>; 4] = [Some("t"), Some("z"), Some("nz"), None];
        const CHAN: [&str; 8] = ["chan0", "chan1", "chan2", "chan3", "andall", "orall", "and02", "or02"];
        let (Some(s), Some(z)) = (SIGN[(v >> 6) as usize & 3], ZERO[(v >> 4) as usize & 3]) else {
            return false;
        };
        let _ = write!(out, "test{s}{}{z}.{}", if v & 8 != 0 { "a" } else { "o" }, CHAN[v as usize & 7]);
        true
    }
}

pub(crate) const K_DST: Dst = Dst;
pub(crate) const K_SRCH: Src = Src(Radix::Hex);
pub(crate) const K_SRCD: Src = Src(Radix::Dec);
pub(crate) const K_SRC0: Src0 = Src0;
pub(crate) const K_ROT: RotImm = RotImm;
pub(crate) const K_BR: BrTarget = BrTarget;
pub(crate) const K_LDFLAGS: LdFlags = LdFlags;
pub(crate) const K_STFLAGS: StFlags = StFlags;
pub(crate) const K_LSDST: LsDst = LsDst;
pub(crate) const K_RANGE: Range = Range;
pub(crate) const K_TEMP: Temp = Temp;
pub(crate) const K_BRFLAGS: BrFlags = BrFlags;
pub(crate) const K_MULFLAGS: MulFlags = MulFlags;
pub(crate) const K_TEST: Test = Test;
pub(crate) const K_ALUFLAGS: Flags = Flags(&[".skipinv", ".syncs", ".nosched", ".end", ".partial"]);
pub(crate) const K_LDRFLAGS: Flags = Flags(&[".skipinv", ".nosched"]);
pub(crate) const K_TESTFLAGS: Flags = Flags(&[".skipinv", ".syncs", ".nosched", ".partial", ".onceonly"]);
pub(crate) const K_TESTFLAGS4: Flags = Flags(&[".skipinv", ".syncs", ".nosched", ".onceonly"]);

pub(crate) const K_DEC: Num = Num { radix: Radix::Dec, mul: 1, add: 0 };
pub(crate) const K_HEX: Num = Num { radix: Radix::Hex, mul: 1, add: 0 };
pub(crate) const K_X4: Num = Num { radix: Radix::Dec, mul: 4, add: 0 };

/* Predicates. The full set is what refdisasm545 prints for the 3-bit field of
 * limm and mov; the narrower sets are the values the older, corpus-derived
 * forms have been checked with (an unchecked value is left undecoded). */
pub(crate) const PRED_ALL: Enum = Enum(names!["", "p0 ", "p1 ", "p2 ", "p3 ", "!p0 ", "!p1 ", "Pn "]);
pub(crate) const PRED_MUL: Enum = Enum(names!["", "p0 ", "p1 ", "!p0 "]);

pub(crate) const F_SKIPINV: Enum = Enum(names!["", ".skipinv"]);
pub(crate) const F_NOSCHED: Enum = Enum(names!["", ".nosched"]);
pub(crate) const F_SYNCS: Enum = Enum(names!["", ".syncs"]);
pub(crate) const F_END: Enum = Enum(names!["", ".end"]);
pub(crate) const F_FREEP: Enum = Enum(names!["", ".freep"]);
pub(crate) const F_TOG: Enum = Enum(names!["", ".tog"]);
pub(crate) const F_TRIGGER: Enum = Enum(names!["", ".trigger"]);
pub(crate) const F_INV: Enum = Enum(names!["", "~"]);
pub(crate) const F_SIGN: Enum = Enum(names!["+", "-"]);
pub(crate) const SU: Enum = Enum(names!["u", "s"]);
/// enc[42] sat, enc[43] s
pub(crate) const SIGNSAT: Enum = Enum(names!["u", "usat", "s", "ssat"]);
pub(crate) const RS: Enum = Enum(names!["", ".rs1", ".rs2", ".rs3", ".rs4", ".rs5", ".rs6", ".rs7"]);
pub(crate) const F_ABS: Enum = Enum(names!["", ".abs"]);
pub(crate) const F_NEG: Enum = Enum(names!["", ".neg"]);
pub(crate) const LOWHIGH: Enum = Enum(names!["low", "high"]);
/// enc[35], enc[59]
pub(crate) const OP_BW_ANDXOR: Enum = Enum(names!["and", "or", "xor", None]);
pub(crate) const OP_BW_SHIFT: Enum = Enum(names!["shl", "rol", "shr", "asr"]);
/// load/store: v = data type[1:0] | address mode[3:2]
pub(crate) const AMDT: Enum = Enum(names![
    "ad", "aw", "ab", None, "ld", "lw", "lb", None, "td", "tw", "tb", None, None, None, None, None
]);
/// enc[41:40]: 0 and 3
pub(crate) const IMODE_PLAIN: Enum = Enum(names!["", None, None, ""]);
pub(crate) const F_SIGN2: Enum = Enum(names!["++", "--"]);

/* Test-group operations enc[19:14], by operand count and immediate style. */
pub(crate) const TESTOP_2: Enum = Enum(names![
    "fadd", None, None, "fsubflr", None, None, None, None, "fdp", "fmin", "fmax", "fdsx", "fdsy", "fmul",
    "fsub", None, None, None, None, None, None, None, "iadd16", "isub16", "imul16", "iaddu16", "isubu16",
    "imulu16", "iadd32", "iaddu32", None, None, "iadd8", "isub8", "iaddu8", "isubu8", "imul8", "fpmul8",
    "imulu8", "fpadd8", "fpsub8"
]);
pub(crate) const TESTOP_1: Enum = Enum(&sparse::<64>(&[(4, "frcp"), (5, "frsq"), (6, "flog"), (7, "fexp")]));
pub(crate) const TESTOP_BW: Enum = Enum(&sparse::<64>(&[
    (48, "and"),
    (49, "or"),
    (50, "xor"),
    (51, "shl"),
    (52, "shr"),
    (53, "rol"),
    (55, "asr"),
]));
pub(crate) const F_WRITE: Enum = Enum(names!["!", ""]);

/// mov destination write mask (enc[47:44], wzyx): x alone prints nothing.
pub(crate) const WMASK: Enum = Enum(names![
    ".", "", ".y", ".xy", ".z", ".xz", ".yz", ".xyz", ".w", ".xw", ".yw", ".xyw", ".zw", ".xzw", ".yzw",
    ".xyzw"
]);
/// ... or, with enc[53] set, the repeat count minus one.
pub(crate) const REPEAT: Enum = Enum(names![
    "",
    ".repeat2",
    ".repeat3",
    ".repeat4",
    ".repeat5",
    ".repeat6",
    ".repeat7",
    ".repeat8",
    ".repeat9",
    ".repeat10",
    ".repeat11",
    ".repeat12",
    ".repeat13",
    ".repeat14",
    ".repeat15",
    ".repeat16"
]);

/* phas */
pub(crate) const PHAS_RATE: Enum = Enum(names!["pixel", "selective", "sample", None]);
pub(crate) const PHAS_WAIT: Enum = Enum(names!["none", "pt", "vcull", None, None, None, None, "end"]);
pub(crate) const PHAS_MODE: Enum = Enum(names!["parallel", "perinstance"]);

/* Shared operand fields. */
pub(crate) const F_DST: Field = fld!(K_DST, (21, 7), (32, 2), (51, 1));
pub(crate) const F_SRC1H: Field = fld!(K_SRCH, (7, 7), (30, 2), (49, 1));
pub(crate) const F_SRC1D: Field = fld!(K_SRCD, (7, 7), (30, 2), (49, 1));
pub(crate) const F_SRC2D: Field = fld!(K_SRCD, (0, 7), (28, 2), (48, 1));
pub(crate) const F_SRC0: Field = fld!(K_SRC0, (14, 7), (34, 1), (51, 1));
pub(crate) const F_LIMM: Field = fld!(K_HEX, (0, 21), (36, 5), (44, 6));
pub(crate) const F_ALUFLAGS: Field = fld!(K_ALUFLAGS, (55, 1), (52, 1), (54, 1), (50, 1), (34, 1));
pub(crate) const F_BRFLAGS: Field = fld!(K_BRFLAGS, (43, 1), (55, 1), (44, 1), (41, 1), (42, 1));
pub(crate) const F_LSDST: Field = fld!(K_LSDST, (21, 7), (39, 1));
pub(crate) const F_TESTFLAGS: Field = fld!(K_TESTFLAGS, (55, 1), (52, 1), (54, 1), (50, 1), (53, 1));
pub(crate) const F_TESTFLAGS4: Field = fld!(K_TESTFLAGS4, (55, 1), (52, 1), (54, 1), (53, 1));
pub(crate) const F_ROT: Field = fld!(K_ROT, (0, 7), (14, 6), (20, 1), (36, 2), (38, 5));
