// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The memctl family (tools/usse-dis/forms/memctl.inc): texture sampling,
//! eld, the class-31 flow, MOE, other, visibility and cache-control groups.

use crate::form::{CLSM, Ctx, Enum, Field, Form, Kind, b, cls, fld, form, m, names};
use crate::kinds::*;
use std::fmt::Write as _;

/// A sequence of small enumerations in one field, printed one after another:
/// the field's chunks, LSB first, are the subfields in print order, so a run
/// of flags and modes costs a single field. A None name marks an invalid
/// value.
pub(crate) struct Seq(pub &'static [(u8, &'static [Option<&'static str>])]);

impl Kind for Seq {
    fn print(&self, mut v: u32, _: &Ctx, out: &mut String) -> bool {
        for &(w, t) in self.0 {
            match t[(v & ((1 << w) - 1)) as usize] {
                Some(s) => out.push_str(s),
                None => return false,
            }
            v >>= w;
        }
        true
    }
}

/* ---- class 28: texture sampling ------------------------------------------
 * Probed by single-bit flips and field sweeps of random class-28 words: the
 * dimension enc[43:42] (1d, 2d, 3d) and the LOD mode enc[41:40] (none, bias,
 * replace, grad; dimension 3 prints invalid_smp_lod_mode); the flags skipinv
 * enc[55], syncs enc[52], nosched enc[54], PCF enc[38:37] and SRD enc[47:46];
 * the coordinate format enc[36:35]. The value 3 of PCF, SRD and the format is
 * invalid (invalid_smp_pcf_mode, _srd_mode, _data_type). */
const K_SMP0: Seq = Seq(&[(2, names!["1d", "2d", "3d", None]), (2, names!["", None, None, None])]);
const K_SMPL: Seq = Seq(&[(2, names!["1d", "2d", "3d", None]), (2, names![None, "bias", "replace", "grad"])]);
const K_SMPFLAGS: Seq = Seq(&[
    (1, names!["", ".skipinv"]),
    (1, names!["", ".syncs"]),
    (1, names!["", ".nosched"]),
    (2, names!["", ".pcf", ".pcff16", None]),
    (2, names!["", ".sinf", ".rsd", None]),
]);
const K_SMPFMT: Seq = Seq(&[(2, names!["", ".flt16", ".c10", None])]);

/* ---- class 29 with enc[54] set: eldd / eldq --------------------------------
 * Probed like the samplers: every bit of the word is read. The flags are
 * skipinv enc[55], syncs enc[52], the cache mode enc[33] | enc[53] << 1 and
 * the fetch count enc[47:44] (count + 1, nothing for 0). */
const K_ELDFLAGS: Seq = Seq(&[
    (1, names!["", ".skipinv"]),
    (1, names!["", ".syncs"]),
    (2, names!["", ".bpcache", ".fcfill", ".bypassl1"]),
    (
        4,
        names![
            "", ".fetch2", ".fetch3", ".fetch4", ".fetch5", ".fetch6", ".fetch7", ".fetch8", ".fetch9",
            ".fetch10", ".fetch11", ".fetch12", ".fetch13", ".fetch14", ".fetch15", ".fetch16"
        ],
    ),
]);
const ELDTYPE: Enum = Enum(names!["d", "q"]);

/// Signed decimal of the given width.
struct SNum(u32);

impl Kind for SNum {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let w = self.0;
        let x = if v & 1 << (w - 1) != 0 { v as i32 - (1i32 << w) } else { v as i32 };
        let _ = write!(out, "{x}");
        true
    }
}
const K_S10: SNum = SNum(10);

/// imo's 12-bit immediates print as unsigned while bit 6 is clear and as the
/// sign-extended 7-bit value -64..-1 when it is set (the bits above it do not
/// show then; the encoder writes them as 0).
struct Imo;

impl Kind for Imo {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let _ = if v & 0x40 != 0 { write!(out, "{}", (v & 0x7f) as i32 - 128) } else { write!(out, "{v}") };
        true
    }
}

/// smlsi operand: v = value[7:0] | swizzle mode[8]: a signed increment, or
/// four 2-bit components, x first in the low bits.
struct Smlsi;

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
const SMLSIMODE: (u8, &[Option<&str>]) = (1, names![", incrementmode", ", swizzlemode"]);
const K_SMLSIMODES: Seq = Seq(&[SMLSIMODE, SMLSIMODE, SMLSIMODE, SMLSIMODE]);

/* ---- class 31, enc[53:52] = 1: the MOE group, operation enc[58:56] -------- */
const SMOAMODE: Enum = Enum(names!["none", "repeat", "clamp", "mirror"]);
const moe_state: Enum = Enum(names![
    "0 /* SMOA low data */",
    "1 /* SMOA high data */",
    "2 /* SMLSI low data */",
    "3 /* SMLSI high data */",
    "4 /* SMBO low data */",
    "5 /* SMBO high data */",
    "6 /* SMR low data */",
    "7 /* SMR high data */",
    "8 /* Index data */",
    "9 /* Reserved */",
    "10 /* Reserved */",
    "11 /* Reserved */",
    "12 /* Reserved */",
    "13 /* Reserved */",
    "14 /* Reserved */",
    "15 /* Reserved */"
]);

/* ---- class 31, enc[53:52] = 2: the other group ---------------------------- */
const IDFMODE: Enum = Enum(names!["bif", "pixelbe", "reserved(=2)", "reserved(=3)"]);
const LOCK: Enum = Enum(names!["lock", "release"]);
const F_TWOPART: Enum = Enum(names!["", ".twopart"]);
const F_THREEPART: Enum = Enum(names!["", ".threepart"]);
const F_CUT: Enum = Enum(names!["", ".cut"]);
const F_TASKS: Enum = Enum(names!["", ".tasks"]);
const F_TASKE: Enum = Enum(names!["", ".taske"]);

/* ---- class 31, enc[53:52] = 3: the visibility test group ------------------- */
const K_VISFLAGS: Seq = Seq(&[
    (1, names!["", ".syncs"]),
    (1, names!["", ".nosched"]),
    (1, names!["", ".end"]),
    (1, names!["", ".c10"]),
]);
const TWOSIDED: Enum = Enum(names![", ", ", twosided, "]);
const VISMODE: Enum = Enum(names!["fb", "nfb", "optdwd", "optdwd"]);

/* ---- class 31, enc[54:52] = 4: cache control ------------------------------- */
const CFOP: Enum = Enum(names![None, "ci", "cf", "cfi"]);
const K_CFFLAGS: Seq =
    Seq(&[(1, names!["", ".nosched"]), (1, names!["", ".global"]), (1, names!["", ".dm_nomatch"])]);
const CFLEVEL: Enum =
    Enum(names!["0 /* Reserved */", "1 /* L0 and L1 */", "2 /* L2 */", "3 /* L0, L1 and L2 */"]);

/* ---- class 28: smp1d/smp2d/smp3d[bias|replace|grad] -------------------- *
 * The destination is r or pa (enc[39]) with a 2-bit write mask enc[45:44],
 * or with enc[53] set a repeat count in the same bits. The coordinates are a
 * source-0 operand (bank enc[34] | enc[50] << 1) with their format, the state
 * a source-1 operand, and a LOD mode other than none adds its value as a
 * source-2 operand (whose bits are otherwise ignored). */
macro_rules! smp {
    ($pat:expr, $v:expr, $op:expr, $cnt:expr $(, $rest:expr)*) => {
        form!($pat, CLSM | b(53), cls(28) | ($v), [
            fld!(PRED_ALL, (56, 3)), fld!($op, (42, 2), (40, 2)),
            fld!(K_SMPFLAGS, (55, 1), (52, 1), (54, 1), (37, 2), (46, 2)), $cnt,
            F_LSDST, fld!(K_SRC0, (14, 7), (34, 1), (50, 1)), fld!(K_SMPFMT, (35, 2)),
            F_SRC1D $(, $rest)*
        ])
    };
}

/* ---- class 31 flow control (enc[54:52] = 0): lapc, mov pclink, nop ----- *
 * enc[40:38] picks the operation as for ba (0) and br (1): lapc (2), a move to
 * pclink from a source-1 operand (3), a move from pclink to a destination (4)
 * and nop (5), which prints no predicate. lapc reads nosched and the sync
 * flags as the branches do; the other bits these forms leave out are
 * ignored. */
const fn flow(op: u64) -> (u64, u64) {
    (CLSM | m(54, 52) | m(40, 38), cls(31) | op << 38)
}

/* ---- class 31 MOE group (enc[53:52] = 1, operation enc[58:56]) --------- *
 * No predicate; nosched is enc[50] throughout. smoa, smr, smlsi and smbo take
 * two source operands with enc[51] set, or immediates with it clear: smoa four
 * signed 10-bit offsets and four 2-bit address modes, smr and smbo four 12-bit
 * values, smlsi four 8-bit operands (an increment or a swizzle, by the mode
 * bits enc[35:32]) and three 4-bit values times 4. imo (enc[51] ignored) takes
 * four 12-bit immediates, setfc two bits, moest a destination and a 4-bit
 * selector the oracle names. */
const fn moe(op: u64, mk: u64, v: u64) -> (u64, u64) {
    (CLSM | m(58, 56) | m(53, 52) | mk, cls(31) | op << 56 | b(52) | v)
}
macro_rules! moereg {
    ($name:literal, $op:expr) => {
        form!(
            concat!($name, "{0} {1}, {2}"),
            moe($op, b(51), b(51)).0,
            moe($op, b(51), b(51)).1,
            [fld!(F_NOSCHED, (50, 1)), F_SRC1D, F_SRC2D,]
        )
    };
}
macro_rules! moe12 {
    ($name:literal, $op:expr) => {
        form!(
            concat!($name, "{0} #{1}, #{2}, #{3}, #{4}"),
            moe($op, b(51), 0).0,
            moe($op, b(51), 0).1,
            [
                fld!(F_NOSCHED, (50, 1)),
                fld!(K_DEC, (36, 12)),
                fld!(K_DEC, (24, 12)),
                fld!(K_DEC, (12, 12)),
                fld!(K_DEC, (0, 12)),
            ]
        )
    };
}

/* ---- class 31 other group (enc[53:52] = 2, operation enc[58:56]) ------- *
 * No predicate; nosched is enc[43]. idf (0) and wdf (1) name a 2-bit DRC and
 * idf a 2-bit mode, setm (2) and wop (7) a small number (wop with twopart
 * enc[3]), lock/release (5, by enc[0]) a 3-bit mutex number enc[6:4] (enc[7]
 * set is invalid_mutex_number). ldr/str (6, by enc[51]) read the same fields
 * as the forms above them, which fix the ignored enc[54] and enc[50]. The
 * emits (3) are picked by enc[47:46] and enc[44]: emitpds has the layout of
 * emitpix2 with tasks/taske flags; the emitmt and emitvcb forms take two or
 * one source operands and a parts flag enc[42] (emitmtevtx also cut,
 * enc[45]). */
const fn oth(op: u64, mk: u64, v: u64) -> (u64, u64) {
    (CLSM | m(58, 56) | m(53, 52) | mk, cls(31) | op << 56 | b(53) | v)
}
const fn ldstr(mk: u64, v: u64) -> (u64, u64) {
    oth(6, b(51) | mk, v)
}
const fn emit(mk: u64, v: u64) -> (u64, u64) {
    oth(3, m(47, 46) | mk, v)
}
const LDRFLAGS: Field = fld!(K_LDRFLAGS, (55, 1), (43, 1));
const IDX14: Field = fld!(K_DEC, (0, 7), (14, 7));
/// The closed compilers' vertex and state programs write the operand an emit
/// does not print as immediate 0 (bank 2 with the extension bit): src1 of
/// emitvcbst and emitvcbvtx, the extension bit of src0 for emitmtest (the 3D
/// captures, design/sgx545_3d.md 9.2). The encoder prefers that.
const EMIT_UNUSED_SRC1: u64 = b(49) | b(31);
const EMIT_UNUSED_SRC0: u64 = b(51);

/* ---- class 31 visibility test group (enc[53:52] = 3) ------------------- *
 * pcoeff and ptoff (operation 0, by enc[47]) print no predicate; atst8 (1)
 * and depthf (3) a 2-bit one at enc[42:41]. atst8 writes a destination and a
 * predicate p<enc[40:39]> (enc[38] clear prints "!"), with c10 at enc[54].
 * Both test source 0, 1 and 2 with twosided enc[37] and the feedback mode
 * enc[36:35] (fb, nfb, optdwd twice). */
const fn vis(op: u64, mk: u64, v: u64) -> (u64, u64) {
    (CLSM | m(58, 56) | m(53, 52) | mk, cls(31) | op << 56 | m(53, 52) | v)
}

/* ---- class 31 enc[54:52] = 4: cache control and phas ------------------- *
 * Operation 0 is ci, cf or cfi by enc[45:44] (0 is invalid_cfi_mode), with
 * nosched enc[43], global enc[47], dm_nomatch enc[46], two sources and the
 * cache levels enc[41:40]. Operation 2 is phas; the older phas forms fix
 * enc[55], which it ignores. */
const fn ot2(op: u64, mk: u64, v: u64) -> (u64, u64) {
    (CLSM | m(58, 56) | m(54, 52) | mk, cls(31) | op << 56 | b(54) | v)
}

/// `form!` with a (mask, val) pair.
macro_rules! formp {
    ($pat:expr, $mv:expr, [$($f:expr),* $(,)?] $(, $dflt:expr)?) => {
        form!($pat, $mv.0, $mv.1, [$($f),*] $(, $dflt)?)
    };
}

#[rustfmt::skip]
pub(crate) static FORMS: &[Form] = &[
    smp!("{0}smp{1}{2} {4}{3}, {5}{6}, {7}, drc{8}", 0, K_SMP0, fld!(WMASK, (44, 2)), fld!(K_DEC, (32, 2))),
    smp!("{0}smp{1}{2}{3} {4}, {5}{6}, {7}, drc{8}", b(53), K_SMP0, fld!(REPEAT, (44, 2)),
        fld!(K_DEC, (32, 2))),
    smp!("{0}smp{1}{2} {4}{3}, {5}{6}, {7}, {8}, drc{9}", 0, K_SMPL, fld!(WMASK, (44, 2)), F_SRC2D,
        fld!(K_DEC, (32, 2))),
    smp!("{0}smp{1}{2}{3} {4}, {5}{6}, {7}, {8}, drc{9}", b(53), K_SMPL, fld!(REPEAT, (44, 2)), F_SRC2D,
        fld!(K_DEC, (32, 2))),
    /* ---- class 29 with enc[54] set: eldd / eldq ---------------------------- *
     * eld + d/q (enc[35]): an r/pa destination (enc[36]), then the base, a
     * source-1-like operand of the r, o, pa or sa bank (enc[31:30]), an r/pa
     * index (enc[34]) and an 11-bit immediate offset enc[43:37] | enc[51:48] << 7,
     * then a source-2-like operand of the same four banks (enc[29:28]) and a
     * 1-bit DRC. The SGX540 oracle reads these words as ld; the SGX540 corpus
     * has none. */
    form!("{0}eld{1}{2} {3}, [{4}, {5}, #{6}], {7}, drc{8}", CLSM | b(54), cls(29) | b(54), [
        fld!(PRED_ALL, (56, 3)), fld!(ELDTYPE, (35, 1)),
        fld!(K_ELDFLAGS, (55, 1), (52, 1), (33, 1), (53, 1), (44, 4)), fld!(K_LSDST, (21, 7), (36, 1)),
        fld!(K_SRCD, (7, 7), (30, 2)), fld!(K_SRC0, (14, 7), (34, 1)), fld!(K_DEC, (37, 7), (48, 4)),
        fld!(K_SRCD, (0, 7), (28, 2)), fld!(K_DEC, (32, 1)),
    ]),
    formp!("{0}lapc{1}", flow(2), [fld!(PRED_ALL, (56, 3)), fld!(K_BRFLAGS, (43, 1), (55, 1), (44, 1))]),
    formp!("{0}mov{1} pclink, {2}", flow(3), [fld!(PRED_ALL, (56, 3)), fld!(F_NOSCHED, (43, 1)), F_SRC1H]),
    formp!("{0}mov{1} {2}, pclink", flow(4), [fld!(PRED_ALL, (56, 3)), fld!(F_NOSCHED, (43, 1)), F_DST]),
    formp!("nop{0}{1}{2}{3}{4}", flow(5), [
        fld!(F_SYNCS, (55, 1)), fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), fld!(F_TOG, (0, 1)),
        fld!(F_TRIGGER, (1, 1)),
    ]),
    moereg!("smoa", 0),
    moereg!("smr", 1),
    moereg!("smlsi", 2),
    moereg!("smbo", 3),
    formp!("smoa{0} #{1}, #{2}, #{3}, #{4}, {5}, {6}, {7}, {8}", moe(0, b(51), 0), [
        fld!(F_NOSCHED, (50, 1)), fld!(K_S10, (30, 10)), fld!(K_S10, (20, 10)), fld!(K_S10, (10, 10)),
        fld!(K_S10, (0, 10)), fld!(SMOAMODE, (46, 2)), fld!(SMOAMODE, (44, 2)), fld!(SMOAMODE, (42, 2)),
        fld!(SMOAMODE, (40, 2)),
    ]),
    moe12!("smr", 1),
    moe12!("smbo", 3),
    formp!("smlsi{0} {1}, {2}, {3}, {4}{5}, #{6}, #{7}, #{8}", moe(2, b(51), 0), [
        fld!(F_NOSCHED, (50, 1)), fld!(Smlsi, (24, 8), (35, 1)), fld!(Smlsi, (16, 8), (34, 1)),
        fld!(Smlsi, (8, 8), (33, 1)), fld!(Smlsi, (0, 8), (32, 1)),
        fld!(K_SMLSIMODES, (35, 1), (34, 1), (33, 1), (32, 1)), fld!(K_X4, (44, 4)), fld!(K_X4, (40, 4)),
        fld!(K_X4, (36, 4)),
    ]),
    formp!("imo{0} #{1}, #{2}, #{3}, #{4}", moe(4, 0, 0), [
        fld!(F_NOSCHED, (50, 1)), fld!(Imo, (36, 12)), fld!(Imo, (24, 12)), fld!(Imo, (12, 12)),
        fld!(Imo, (0, 12)),
    ]),
    formp!("setfc{0} #{1}, #{2}", moe(5, 0, 0), [
        fld!(F_NOSCHED, (50, 1)), fld!(K_DEC, (0, 1)), fld!(K_DEC, (8, 1)),
    ]),
    formp!("moest{0}{1} {2}, #{3}", moe(6, 0, 0), [
        fld!(F_SKIPINV, (55, 1)), fld!(F_NOSCHED, (50, 1)), F_DST, fld!(moe_state, (43, 4)),
    ]),
    formp!("idf{0} drc{1}, {2}", oth(0, 0, 0), [
        fld!(F_NOSCHED, (43, 1)), fld!(K_DEC, (32, 2)), fld!(IDFMODE, (46, 2)),
    ]),
    formp!("wdf{0} drc{1}", oth(1, 0, 0), [fld!(F_NOSCHED, (43, 1)), fld!(K_DEC, (32, 2))]),
    formp!("setm{0} #{1}", oth(2, 0, 0), [fld!(F_NOSCHED, (43, 1)), fld!(K_DEC, (0, 4))]),
    formp!("wop{0}{1} #{2}", oth(7, 0, 0), [
        fld!(F_NOSCHED, (43, 1)), fld!(F_TWOPART, (3, 1)), fld!(K_DEC, (0, 3)),
    ]),
    formp!("{0}{1} #{2}", oth(5, b(7), 0), [
        fld!(LOCK, (0, 1)), fld!(F_NOSCHED, (43, 1)), fld!(K_DEC, (4, 3)),
    ]),
    formp!("ldr{0} {1}, #{2}, drc{3}", ldstr(b(48) | m(29, 28), b(48) | b(29)), [
        LDRFLAGS, F_LSDST, IDX14, fld!(K_DEC, (32, 2)),
    ]),
    formp!("ldr{0} {1}, {2}, drc{3}", ldstr(0, 0), [LDRFLAGS, F_LSDST, F_SRC2D, fld!(K_DEC, (32, 2))]),
    formp!("str{0} #{1}, {2}", ldstr(b(48) | m(29, 28), b(51) | b(48) | b(29)), [LDRFLAGS, IDX14, F_SRC1D]),
    formp!("str{0} {1}, {2}", ldstr(0, b(51)), [LDRFLAGS, F_SRC2D, F_SRC1D]),
    formp!("emitpds{0}{1}{2}{3}{4} #{5} /* incp */, {6}, {7}, {8}, #{9}", emit(0, b(47)), [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), fld!(F_TASKS, (45, 1)), fld!(F_TASKE, (44, 1)),
        fld!(F_FREEP, (21, 1)), fld!(K_DEC, (32, 2), (41, 1)), F_SRC0, F_SRC1D, F_SRC2D,
        fld!(K_HEX, (22, 6), (35, 6), (54, 2)),
    ]),
    formp!("emitmtest{0}{1}{2}{3} #{4} /* incp */, {5}, {6}", emit(b(44), b(46)), [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), fld!(F_THREEPART, (42, 1)), fld!(F_FREEP, (21, 1)),
        fld!(K_DEC, (32, 2), (41, 1)), F_SRC1D, F_SRC2D,
    ], EMIT_UNUSED_SRC0),
    formp!("emitmtevtx{0}{1}{2}{3}{4} #{5} /* incp */, {6}, {7}", emit(b(44), b(46) | b(44)), [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), fld!(F_TWOPART, (42, 1)), fld!(F_CUT, (45, 1)),
        fld!(F_FREEP, (21, 1)), fld!(K_DEC, (32, 2), (41, 1)), F_SRC1D, F_SRC2D,
    ]),
    formp!("emitvcbst{0}{1}{2}{3} #{4} /* incp */, {5}", emit(b(44), b(47) | b(46)), [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), fld!(F_THREEPART, (42, 1)), fld!(F_FREEP, (21, 1)),
        fld!(K_DEC, (32, 2), (41, 1)), F_SRC2D,
    ], EMIT_UNUSED_SRC1),
    formp!("emitvcbvtx{0}{1}{2}{3} #{4} /* incp */, {5}", emit(b(44), b(47) | b(46) | b(44)), [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), fld!(F_TWOPART, (42, 1)), fld!(F_FREEP, (21, 1)),
        fld!(K_DEC, (32, 2), (41, 1)), F_SRC2D,
    ], EMIT_UNUSED_SRC1),
    formp!("pcoeff{0}{1} {2}, {3}, {4}", vis(0, b(47), 0), [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1)), F_SRC0, F_SRC1D, F_SRC2D,
    ]),
    formp!("ptoff{0}{1}", vis(0, b(47), b(47)), [fld!(F_NOSCHED, (43, 1)), fld!(F_END, (50, 1))]),
    formp!("{0}atst8{1} {2}, {3}p{4}, {5}, {6}, {7}{8}{9}", vis(1, 0, 0), [
        fld!(PRED_MUL, (41, 2)), fld!(K_VISFLAGS, (55, 1), (43, 1), (50, 1), (54, 1)),
        fld!(K_DST, (21, 7), (32, 2)), fld!(F_WRITE, (38, 1)), fld!(K_DEC, (39, 2)), F_SRC0, F_SRC1D, F_SRC2D,
        fld!(TWOSIDED, (37, 1)), fld!(VISMODE, (35, 2)),
    ]),
    formp!("{0}depthf{1} {2}, {3}, {4}{5}{6}", vis(3, 0, 0), [
        fld!(PRED_MUL, (41, 2)), fld!(K_VISFLAGS, (55, 1), (43, 1), (50, 1)), F_SRC0, F_SRC1D, F_SRC2D,
        fld!(TWOSIDED, (37, 1)), fld!(VISMODE, (35, 2)),
    ]),
    formp!("{0}{1} {2}, {3}, #{4}", ot2(0, 0, 0), [
        fld!(CFOP, (44, 2)), fld!(K_CFFLAGS, (43, 1), (47, 1), (46, 1)), F_SRC1D, F_SRC2D,
        fld!(CFLEVEL, (40, 2)),
    ]),
    formp!("phas{0} #{1}, #{2}, {3}, {4}, {5}", ot2(2, b(50), b(50)), [
        fld!(F_END, (51, 1)), fld!(K_DEC, (0, 18)), fld!(K_X4, (32, 8)), fld!(PHAS_RATE, (43, 2)),
        fld!(PHAS_WAIT, (40, 3)), fld!(PHAS_MODE, (45, 1)),
    ]),
    formp!("phas{0}{1} {2}, {3}", ot2(2, b(50), 0), [
        fld!(F_NOSCHED, (43, 1)), fld!(F_END, (51, 1)), F_SRC1D, F_SRC2D,
    ]),
];
