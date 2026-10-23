// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The USSE interpreter.

use std::fmt;
use usse_isa::{Core, Ctx, Form, decode_fields};

/// Memory and the SGX registers, as the program sees them.
pub trait Bus {
    fn load32(&mut self, addr: u32) -> Result<u32, Error>;
    fn store32(&mut self, addr: u32, v: u32) -> Result<(), Error>;

    /// ldr: read SGX register `index` (dword index; its MMIO offset is index * 4).
    fn reg_read(&mut self, index: u32) -> Result<u32, Error> {
        Err(why(format!("ldr #{index}: no SGX registers on this bus")))
    }

    /// str: write SGX register `index`.
    fn reg_write(&mut self, index: u32, _v: u32) -> Result<(), Error> {
        Err(why(format!("str #{index}: no SGX registers on this bus")))
    }

    /// emitpds and its kin: the instruction and its three source values.
    fn emit(&mut self, text: &str, _srcs: [u32; 3]) -> Result<(), Error> {
        Err(why(format!("{text}: no task emission on this bus")))
    }
}

/// A flat memory window: `bytes` at `base`, little endian. Anything else faults.
pub struct Memory {
    pub base: u32,
    pub words: Vec<u32>,
}

impl Bus for Memory {
    /// An unaligned load reads the four bytes at addr (hardware: a register
    /// offset of 1 byte returns the bytes across two dwords).
    fn load32(&mut self, addr: u32) -> Result<u32, Error> {
        let mut v = 0;
        for k in (0..4).rev() {
            let a = addr.wrapping_add(k);
            v = v << 8 | (self.words[self.index(a & !3, addr)?] >> (8 * (a & 3)) & 0xff);
        }
        Ok(v)
    }

    /// An unaligned store is dropped (hardware: the buffer stays unchanged).
    fn store32(&mut self, addr: u32, v: u32) -> Result<(), Error> {
        let i = self.index(addr & !3, addr)?;
        if addr.is_multiple_of(4) {
            self.words[i] = v;
        }
        Ok(())
    }
}

impl Memory {
    fn index(&self, aligned: u32, addr: u32) -> Result<usize, Error> {
        let off = aligned.wrapping_sub(self.base);
        if off as usize >= self.words.len() * 4 {
            return Err(Error::Fault(addr));
        }
        Ok(off as usize / 4)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// What the simulator does not model (yet): the instruction and why.
    Unsupported { pc: usize, text: String, why: String },
    /// A memory access outside the bus's memory.
    Fault(u32),
    /// The program counter left the program.
    PcOutOfRange(usize),
    /// No `.end` within the step limit.
    StepLimit,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Error::Unsupported { pc, text, why } => write!(f, "{pc}: {text}: {why}"),
            Error::Fault(a) => write!(f, "fault at 0x{a:08X}"),
            Error::PcOutOfRange(pc) => write!(f, "pc {pc} out of range"),
            Error::StepLimit => write!(f, "step limit"),
        }
    }
}

/// A decoded instruction: its form and field values, and the text of the
/// fields read by name (flags, operation selectors).
#[derive(Clone)]
struct Insn {
    form: &'static Form,
    v: [u32; 10],
    text: String,
}

impl Insn {
    fn get(&self, role: &str) -> Option<u32> {
        self.form.role(role).map(|i| self.v[i])
    }

    /// The text a field prints (an enumeration or flags).
    fn name(&self, role: &str, core: Core) -> Option<String> {
        let i = self.form.role(role)?;
        let mut s = String::new();
        self.form.f[i].k.print(self.v[i], &Ctx { core, resolve: None }, &mut s);
        Some(s)
    }
}

/// A program, decoded once.
pub struct Program {
    insns: Vec<Result<Insn, String>>,
}

impl Program {
    pub fn new(words: &[u64], core: Core) -> Program {
        let insns = words
            .iter()
            .map(|&w| {
                let text = usse_isa::decode(w, core).text;
                match decode_fields(w, core) {
                    Some(f) => Ok(Insn { form: &usse_isa::table().forms[f.form], v: f.v, text }),
                    None => Err(text),
                }
            })
            .collect();
        Program { insns }
    }
}

/// The state of one instance.
pub struct Machine {
    pub core: Core,
    pub r: [u32; 128],
    pub i: [u32; 4],
    pub o: [u32; 128],
    pub pa: [u32; 128],
    pub sa: [u32; 128],
    pub p: [bool; 4],
    pub pc: usize,
    pub link: u32,
    pub steps: usize,
    /// What the task has written, per bank (a bit per register): a read of
    /// anything else is the environment's (stale or set up by the job), not the
    /// ISA's, and stops the program.
    pub written: Written,
    /// Results of DRC operations (loads, idiv) not yet visible: (drc, temp or
    /// pa register, value). They land at a wdf on their DRC; reading such a
    /// register before is timing-dependent on the hardware (x-misc: a store
    /// right after idiv saw the old value), so it stops the program.
    pub pending: Vec<(u32, Reg, u32)>,
    /// The MOE increments smlsi set, in its operand order; None until a
    /// program sets them (the state a task starts with is not known: the vec1
    /// probe's ldad.fetch4 r30 left r31 unchanged).
    pub moe_inc: Option<[i32; 4]>,
}

/// A register a pending result goes to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reg {
    R(usize),
    Pa(usize),
}

/// A bit per register of each bank.
#[derive(Clone, Copy, Default)]
pub struct Written {
    pub r: u128,
    pub i: u8,
    pub o: u128,
    pub pa: u128,
    pub sa: u128,
}

impl Default for Machine {
    fn default() -> Self {
        Machine {
            core: Core::Sgx545,
            r: [0; 128],
            i: [0; 4],
            o: [0; 128],
            pa: [0; 128],
            sa: [0; 128],
            p: [false; 4],
            pc: 0,
            link: 0,
            steps: 0,
            written: Written::default(),
            pending: Vec::new(),
            moe_inc: None,
        }
    }
}

/// What an instruction asks the loop to do next.
enum Next {
    Step,
    Jump(usize),
    End,
}

impl Machine {
    /// Run from `pc` until an instruction with `.end` retires.
    pub fn run(&mut self, prog: &Program, bus: &mut dyn Bus, max_steps: usize) -> Result<(), Error> {
        loop {
            if self.steps >= max_steps {
                return Err(Error::StepLimit);
            }
            self.steps += 1;
            let pc = self.pc;
            let insn = match prog.insns.get(pc) {
                None => return Err(Error::PcOutOfRange(pc)),
                Some(Err(text)) => return Err(unsupported(pc, text, "undecoded")),
                Some(Ok(i)) => i,
            };
            match self.exec(insn, bus).map_err(|e| match e {
                Error::Unsupported { why, .. } => unsupported(pc, &insn.text, &why),
                e => e,
            })? {
                Next::Step => self.pc += 1,
                Next::Jump(t) => self.pc = t,
                Next::End => return Ok(()),
            }
        }
    }

    fn pred(&self, insn: &Insn) -> Result<bool, Error> {
        let Some(p) = insn.name("pred", self.core) else { return Ok(true) };
        Ok(match p.trim() {
            "" => true,
            "p0" => self.p[0],
            "p1" => self.p[1],
            "p2" => self.p[2],
            "p3" => self.p[3],
            "!p0" => !self.p[0],
            "!p1" => !self.p[1],
            other => return Err(why(format!("predicate {other}"))),
        })
    }

    fn exec(&mut self, insn: &Insn, bus: &mut dyn Bus) -> Result<Next, Error> {
        let op = insn.form.op;
        if op.is_empty() {
            return Err(why("no semantics for this form yet"));
        }
        let end = self.ends(insn);
        if !matches!(op, "br" | "ba" | "lapc" | "cache") && !op.starts_with("ld") && !op.starts_with("st") {
            // flags whose effect is not modelled stop the program
            if let Some(f) = insn.name("flags", self.core) {
                for t in f.split('.').filter(|t| !t.is_empty()) {
                    if !matches!(t, "skipinv" | "syncs" | "nosched" | "end") {
                        return Err(why(format!("flag .{t}")));
                    }
                }
            }
        }
        if !self.pred(insn)? {
            return Ok(if end { Next::End } else { Next::Step });
        }
        let its = self.iterations(insn)?;
        let its = its.as_deref().unwrap_or(std::slice::from_ref(insn));
        let mut next = None;
        if matches!(op, "fdp" | "fdpc" | "fddp" | "fddpc") {
            self.dot(its)?;
        } else {
            for it in its {
                next = self.op(it, bus)?;
            }
        }
        Ok(match (next, end) {
            (_, true) => Next::End,
            (Some(t), false) => Next::Jump(t),
            (None, false) => Next::Step,
        })
    }

    /// The iterations of a repeated instruction, each with its register
    /// operands advanced, or None when it does not repeat. Hardware
    /// (repeat-order, repeat-pred): the iterations run one after another, under
    /// the instance's one predicate; every register operand advances by 1 per
    /// iteration, .incsXYZ (incs-fmad) leaves sources 0, 1, 2 in place where its
    /// bit is clear; after smlsi (repeat-smlsi) the destination advances by its
    /// increment 0 and source 1 by its increment 2.
    fn iterations(&self, insn: &Insn) -> Result<Option<Vec<Insn>>, Error> {
        let Some(rep) = ["repeat", "rep"].into_iter().find_map(|r| insn.form.role(r)) else {
            return Ok(None);
        };
        let n = insn.v[rep] + 1;
        if n == 1 {
            return Ok(None);
        }
        let op = insn.form.op;
        if !matches!(
            op,
            "mov" | "bitwise" | "fmad" | "fsubflr" | "fminmax" | "fdp" | "fdpc" | "fddp" | "fddpc"
        ) {
            return Err(why(format!("{op} repeated")));
        }
        // the incs field: enc[53] set advances every source, else bits 3-1
        // advance sources 0-2; fddpc has none and its sources stay (fddpc)
        let incs = insn.get("incs").unwrap_or(if op == "fddpc" { 0 } else { 1 });
        let advances = |k: u32| incs & 1 != 0 || incs >> (4 - k) & 1 != 0;
        let mut step = [("dst", 1), ("src0", 1), ("src1", 1), ("src2", 1)];
        if let Some(inc) = self.moe_inc {
            // which increment source 0 and source 2 take is not measured
            let unknown = |k: usize| inc[k] != inc[0] || inc[k] != inc[2];
            let used = |k: u32| insn.form.role(step[k as usize].0).is_some() && advances(k);
            if (used(1) && unknown(1)) || (used(3) && unknown(3)) {
                return Err(why("source 0/2 increment after smlsi"));
            }
            (step[0].1, step[1].1, step[2].1, step[3].1) = (inc[0], inc[1], inc[2], inc[3]);
        }
        let mut its = Vec::with_capacity(n as usize);
        for k in 0..n as i32 {
            let mut it = insn.clone();
            it.v[rep] = 0;
            for (s, &(role, inc)) in step.iter().enumerate() {
                let Some(i) = insn.form.role(role) else { continue };
                if k == 0 || (s > 0 && !advances(s as u32)) || inc == 0 {
                    continue;
                }
                let v = insn.v[i];
                let f = &insn.form.f[i];
                // register banks only: an immediate, the rotated immediate or
                // an internal register does not step (not measured)
                let bank = if role == "src0" { v >> 7 & 1 } else { v >> 7 & 7 };
                if f.width() == 21 || bank >= 6 {
                    return Err(why(format!("{role} immediate repeated")));
                }
                let r = (v & 0x7f) as i32 + k * inc;
                let top = if bank == 0 && v & 0x7f < 124 { 124 } else { 128 };
                if !(0..top).contains(&r) {
                    return Err(why(format!("{role} repeated past its bank")));
                }
                it.v[i] = v & !0x7f | r as u32;
            }
            its.push(it);
        }
        Ok(Some(its))
    }

    /// The dot products over their iterations. Hardware: fdp (fdp-count,
    /// fdp-rounding) sums the products in turn, each product and each sum
    /// rounded, into the last iteration's destination only; fdpc (fdpc,
    /// fdpc-clip) gives fdp's value and leaves p0-p3 and i0-i2 alone (its clip
    /// result is not visible); fddp and fddpc (fddp, fddpc) sum s0 * s1 into
    /// the destination and s0 * s2 into i<n>. The rounding of all but fdp is
    /// not measured (their probes are exact), so an inexact step stops.
    fn dot(&mut self, its: &[Insn]) -> Result<(), Error> {
        let op = its[0].form.op;
        let double = op.starts_with("fddp");
        let mut acc: [Option<u32>; 2] = [None, None];
        for it in its {
            let pairs: &[(&str, &str)] =
                if double { &[("src0", "src1"), ("src0", "src2")] } else { &[("src1", "src2")] };
            for (k, &(x, y)) in pairs.iter().enumerate() {
                let read = |r: &str| {
                    let v = it.get(r).unwrap();
                    if r == "src0" { self.fsrc0(v) } else { self.fsrc(v) }
                };
                let (a, b) = (read(x)?, read(y)?);
                let p = flt::mul(a, b)?;
                let s = match acc[k] {
                    None => p,
                    Some(s) => flt::add(Ok(s), p)?,
                };
                if op != "fdp" {
                    let exact = |r: u32, e: f64| f32::from_bits(r) as f64 == e;
                    let pe = f32::from_bits(a) as f64 * f32::from_bits(b) as f64;
                    let se = acc[k].map_or(0.0, |s| f32::from_bits(s) as f64) + f32::from_bits(p) as f64;
                    if !exact(p, pe) || !exact(s, se) {
                        return Err(why(format!("{op} rounding (not measured)")));
                    }
                }
                acc[k] = Some(s);
            }
        }
        let last = its.last().unwrap();
        if double {
            self.set_i(last.get("idst").unwrap() as usize, acc[1].unwrap())?;
        }
        self.write(last, acc[0].unwrap())
    }

    /// The pack group: the packs of f32 to 8- and 16-bit components and the
    /// unpacks back to f32.
    fn pack(&mut self, insn: &Insn) -> Result<(), Error> {
        let text = insn.name("op", self.core).unwrap();
        let mut parts = text.split('.');
        let fmt = parts.next().unwrap();
        let mut scale = false;
        for f in parts {
            match f {
                "skipinv" | "syncs" | "nosched" | "end" => {}
                "scale" => scale = true,
                _ => return Err(why(format!("{fmt} flag .{f}"))),
            }
        }
        let round = insn.name("round", self.core).unwrap();
        if let Some(from) = fmt.strip_prefix("unpckf32") {
            if insn.get("bytemask") != Some(15) {
                return Err(why(format!("{fmt} with a byte mask")));
            }
            if round != "nearest" {
                return Err(why(format!("{fmt} rounding {round}")));
            }
            return self.unpack_f32(insn, fmt, from, scale);
        }
        match fmt.strip_prefix("pck").and_then(|f| f.strip_suffix("f32")) {
            Some(to @ ("u8" | "s8" | "o8" | "u16" | "s16")) => self.pack_f32(insn, fmt, to, scale, &round),
            _ => Err(why(fmt.to_string())),
        }
    }

    /// An unpack to f32. A source is operand[9:0] | select[11:10]: byte
    /// `select`, or the 16 bits at that byte (hardware, unpack-select). With
    /// .scale (unpack-*-scale) a u8 is divided by 255, an s8 by 127 clamped at
    /// -1, an o8 is the s8 of the byte ^ 0x80, a u16 divided by 65535 and an
    /// s16 by 32767 clamped at -1, each rounded once to nearest.
    fn unpack_f32(&mut self, insn: &Insn, fmt: &str, from: &str, scale: bool) -> Result<(), Error> {
        let v = insn.get("src1").unwrap();
        let (w, sel) = (self.src(v & 0x3ff, 6)?, v >> 10 & 3);
        let x = match from {
            "u8" | "s8" | "o8" => w >> (8 * sel) & 0xff,
            "u16" | "s16" if sel < 3 => w >> (8 * sel) & 0xffff,
            _ => return Err(why(format!("{fmt} of component {sel}"))),
        };
        // The quotients of an integer by 255, 127, 65535 or 32767 are never
        // f32 ties, and f64 holds them closely enough to round once.
        let f = match (from, scale) {
            ("u8" | "u16", false) => x as f64,
            ("u8", true) => x as f64 / 255.0,
            ("s8", true) => (x as u8 as i8 as f64 / 127.0).max(-1.0),
            ("o8", true) => ((x ^ 0x80) as u8 as i8 as f64 / 127.0).max(-1.0),
            ("u16", true) => x as f64 / 65535.0,
            ("s16", true) => (x as u16 as i16 as f64 / 32767.0).max(-1.0),
            _ => return Err(why(format!("{fmt} without .scale"))),
        };
        self.write(insn, (f as f32).to_bits())
    }

    /// A pack of two f32 sources to 8- or 16-bit values. Hardware:
    /// - pack-*-int: rounded by the mode (nearest even, down, up, zero) and
    ///   saturated; o8 is the s8 value + 128;
    /// - pack-*-scale: with .scale, x * 255, 127, 65535 or 32767 exactly,
    ///   rounded to nearest whatever the mode, saturated (s8/s16 at -K);
    /// - 8-bit values go to bytes 0-3 as s1, s2, s1, s2, 16-bit ones to the
    ///   low and high halves; pack-u8-bytemask and pack-u16-bytemask show
    ///   which value each enabled byte takes under every byte mask.
    fn pack_f32(&mut self, insn: &Insn, fmt: &str, to: &str, scale: bool, round: &str) -> Result<(), Error> {
        let (k, wide) = match to {
            "u8" => (255.0, false),
            "s8" | "o8" => (127.0, false),
            "u16" => (65535.0, true),
            _ => (32767.0, true),
        };
        let (mut lo, hi) = match to {
            "u8" | "u16" => (0.0, k),
            "s8" | "o8" => (-128.0, 127.0),
            _ => (-32768.0, 32767.0),
        };
        if scale && lo < 0.0 {
            lo = -k;
        }
        let conv = |role: &str| -> Result<u32, Error> {
            let v = insn.get(role).unwrap();
            if v >> 10 != 0 {
                return Err(why(format!("{fmt} of a component select")));
            }
            let b = self.src(v & 0x3ff, 6)?;
            if b >> 23 & 0xff == 0xff || (b >> 23 & 0xff == 0 && b & 0x7f_ffff != 0) {
                return Err(why(format!("{fmt} of an infinity, NaN or denormal")));
            }
            let x = f32::from_bits(b) as f64;
            let n = if scale {
                // x * k is exact in f64; the one measured tie (127.5) went
                // up to the even 128, so a tie the two readings differ on stops
                let q = x * k;
                let n = q.round_ties_even();
                if n != (q + 0.5).floor() {
                    return Err(why(format!("{fmt}.scale tie")));
                }
                n
            } else {
                match round {
                    "nearest" => x.round_ties_even(),
                    "down" => x.floor(),
                    "up" => x.ceil(),
                    _ => x.trunc(),
                }
            };
            let n = n.clamp(lo, hi) as i32 + if to == "o8" { 128 } else { 0 };
            Ok(n as u32)
        };
        let (a, b) = (conv("src1")?, conv("src2")?);
        let m = insn.get("bytemask").unwrap();
        if m != 15 && to != "u8" && to != "u16" {
            return Err(why(format!("{fmt} with a byte mask")));
        }
        let en = |i: u32| m >> i & 1 != 0;
        let bytes = if wide {
            // the upper half takes s2 where byte 0 is enabled, else s1
            let up = if en(0) { b } else { a };
            [a & 0xff, a >> 8 & 0xff, up & 0xff, up >> 8 & 0xff]
        } else {
            let pick = |second: bool| if second { b } else { a } & 0xff;
            [a & 0xff, pick(en(0)), pick((en(0) as u8 + en(1) as u8) % 2 == 1), pick(en(0) || en(1) || en(2))]
        };
        let mut w = 0;
        for (i, &x) in bytes.iter().enumerate() {
            w |= x << (8 * i);
        }
        if m != 15 {
            if m == 0 {
                return Ok(());
            }
            let d = insn.get("dst").unwrap();
            if d >> 7 != 0 || d & 0x7f >= 124 {
                return Err(why(format!("{fmt} byte mask on a non-temporary")));
            }
            let keep = (0..4).filter(|&i| !en(i)).fold(0u32, |k, i| k | 0xff << (8 * i));
            w = self.temp(d as usize)? & keep | w & !keep;
        }
        self.write(insn, w)
    }

    /// A colour operand (six-bit registers, a .c10 bit and the bank above:
    /// usse_isa's C10 kind) as an ordinary one, number[6:0] | bank[9:7]. Only
    /// plain registers so far: .c10 lanes, indexed operands and immediates
    /// stop.
    fn colour_operand(v: u32, dst: bool) -> Result<u32, Error> {
        let (bank, r) = (v >> 7 & 7, v & 63);
        if v >> 6 & 1 != 0 {
            return Err(why(".c10 operand"));
        }
        if v >> 10 != 0 || bank == 5 || bank == 6 || bank == if dst { 3 } else { 4 } {
            return Err(why("colour operand (indexed, immediate or modified)"));
        }
        Ok(match bank {
            0 if r >= 60 => 7 << 7 | (r - 60),
            b => b << 7 | r,
        })
    }

    /// sop2, per byte: bytes 0-2 by the colour selection, byte 3 by the alpha
    /// one. Hardware (sop2-selectors, sop2-ops, sop2-random):
    /// - add/sub: round((x1 * f1 +/- x2 * f2) / 255) once, half up, clamped
    ///   to 0-255; min/max compare x1 and x2 themselves, whatever the factors;
    /// - factors: zero, s1, s2 (this byte), s1a, s2a (the alpha bytes), asat =
    ///   min(s1a, 1 - s2a), and .comp = 1 - factor, except asat.comp =
    ///   min(1 - s1a, s2a);
    /// - s2scale (first factor) is s2 and doubles the result; zeros (second
    ///   factor) is 0, or 1 with .comp, and for add/sub reads x2 as x2 - 128;
    /// - for add/sub a leading comp complements x1, and neg (alpha) negates.
    ///
    /// sop2wm (sop2wm) takes one selector pair for all four bytes, the colour
    /// operation for bytes 0-2 and the alpha one for byte 3, and writes the
    /// bytes its mask enables (bit n, byte n).
    fn sop2(&mut self, insn: &Insn) -> Result<(), Error> {
        let s1 = self.src(Self::colour_operand(insn.get("src1").unwrap(), false)?, 6)?;
        let s2 = self.src(Self::colour_operand(insn.get("src2").unwrap(), false)?, 6)?;
        let d = Self::colour_operand(insn.get("dst").unwrap(), true)?;
        let (csel, asel, mask) = match insn.name("sel", self.core) {
            None => (insn.name("csel", self.core).unwrap(), insn.name("asel", self.core).unwrap(), 15),
            Some(sel) => {
                // "f1, f2, colour op, alpha op"
                let (pair, aop) = sel.rsplit_once(", ").unwrap();
                let (f12, cop) = pair.rsplit_once(", ").unwrap();
                (format!("{f12}, {cop}"), format!("{f12}, {aop}"), insn.get("bytemask").unwrap())
            }
        };
        let mut w = 0;
        for ch in 0..4 {
            w |= blend(if ch == 3 { &asel } else { &csel }, s1, s2, ch)? << (8 * ch);
        }
        if mask != 15 {
            if d >> 7 != 0 || d & 0x7f >= 124 {
                return Err(why("sop2wm byte mask on a non-temporary"));
            }
            let keep = (0..4).filter(|&i| mask >> i & 1 == 0).fold(0u32, |k, i| k | 0xff << (8 * i));
            w = self.temp(d as usize)? & keep | w & !keep;
        }
        self.write_dst(d, w)
    }

    /// A colour source 0 (six-bit registers, .c10 bit 6, pa bit 7, a
    /// modifier bit 8) as an ordinary source 0, number[6:0] | pa[7].
    fn colour_operand0(v: u32) -> Result<u32, Error> {
        let (pa, r) = (v >> 7 & 1, v & 63);
        if v >> 6 & 1 != 0 || v >> 8 != 0 {
            return Err(why(".c10 or modified source 0"));
        }
        Ok(if pa == 0 && r >= 60 { 124 + r - 60 } else { pa << 7 | r })
    }

    /// sop3, lrp1 and lrp2 on three sources, per byte, with sop2's rounding:
    /// round(sum / 255) half up, clamped. Hardware (sop3-lrp, sop3-random,
    /// x-sop3, x-arsop):
    /// - sop3's colour is sop2's add/sub of s1 * f1 and s2 * f2, the factors
    ///   including s0 and s0a; lrp1's colour is f1 * (1 - w) + f2 * w, w being
    ///   s0 or s0a; lrp2's is x1 * f1 + x2 * f2, x1 and x2 named by its third
    ///   selection (s1, s1.comp or one; s2 or s2.comp);
    /// - alpha: asop as sop2's; arsop sub is s2a * g - s1a * (the second
    ///   colour factor), arsop add 0; alrp is g1 * (1 - s0a) + g2 * s0a.
    ///
    /// The colour neg of the alrp form fits no reading yet and stops.
    fn sop3(&mut self, insn: &Insn) -> Result<(), Error> {
        let s = [
            self.src0(Self::colour_operand0(insn.get("src0").unwrap())?)?,
            self.src(Self::colour_operand(insn.get("src1").unwrap(), false)?, 6)?,
            self.src(Self::colour_operand(insn.get("src2").unwrap(), false)?, 6)?,
        ];
        let d = Self::colour_operand(insn.get("dst").unwrap(), true)?;
        let csel = insn.name("csel", self.core).unwrap();
        if csel.ends_with(", neg") {
            return Err(why("sop3 colour neg"));
        }
        let alpha =
            ["asop", "arsop", "alrp"].into_iter().find_map(|r| Some((r, insn.name(r, self.core)?))).unwrap();
        let w = blend3(insn.form.op, &csel, (alpha.0, &alpha.1), s)?;
        self.write_dst(d, w)
    }

    /// movmsk d, s1, s2.c (and .smsk d, s0, s1, s2.c): s1 into d where
    /// component c of s2 passes, else d stays. Hardware (movmsk-mask, -u8,
    /// -flt, -flt16): .mask tests bit 0 of byte c, .u8 bit 7 of byte c, .flt
    /// s2 >= 0.5 (a NaN fails), .flt16 the half c >> 1 >= 0.5 (c = 1 fits no
    /// reading and stops); .smsk (movmsk-smsk, -smsk2) also needs bit 0 of s0
    /// for .u8 and .flt, while .mask ignores it.
    fn movmsk(&mut self, insn: &Insn) -> Result<(), Error> {
        let fmt = insn.name("fmt", self.core).unwrap_or(".u8".into());
        let c = insn.get("comp").unwrap();
        let s2 = self.src(insn.get("src2").unwrap(), 6)?;
        let mut pass = match fmt.as_str() {
            ".mask" => s2 >> (8 * c) & 1 != 0,
            ".u8" => s2 >> (8 * c + 7) & 1 != 0,
            ".flt" if c == 0 => flt::at_least_half(s2, false)?,
            ".flt16" if c != 1 => flt::at_least_half(s2 >> (16 * (c >> 1)) & 0xffff, true)?,
            f => return Err(why(format!("movmsk{f} of component {c}"))),
        };
        if let Some(v) = insn.get("src0")
            && fmt != ".mask"
        {
            pass &= self.src0(v)? & 1 != 0;
        }
        let s1 = self.src(insn.get("src1").unwrap(), 6)?;
        if pass { self.write(insn, s1) } else { Ok(()) }
    }

    /// One iteration of an instruction: where it jumps, if it does.
    fn op(&mut self, insn: &Insn, bus: &mut dyn Bus) -> Result<Option<usize>, Error> {
        let op = insn.form.op;
        Ok(match op {
            "nop" | "idf" | "phas" => None,
            "wdf" => {
                let drc = insn.get("drc").unwrap();
                let (land, keep): (Vec<_>, Vec<_>) = self.pending.drain(..).partition(|p| p.0 == drc);
                self.pending = keep;
                for (_, reg, v) in land {
                    match reg {
                        Reg::R(n) => self.set_r(n, v),
                        Reg::Pa(n) => self.set_pa(n, v),
                    }
                }
                None
            }
            "mov_imm" => {
                self.write(insn, insn.get("imm").unwrap())?;
                None
            }
            "mov" => {
                let v = self.src(insn.get("src1").unwrap(), 6)?;
                self.write(insn, v)?;
                None
            }
            "br" | "ba" => {
                let t = insn.get("target").unwrap();
                let flags = insn.name("flags", self.core).unwrap_or_default();
                for f in flags.split('.').filter(|f| !f.is_empty()) {
                    match f {
                        "nosched" => {}
                        // the return address, as an instruction index: the
                        // firmware only saves and restores it (not measured)
                        "savelink" => self.link = self.pc as u32 + 1,
                        _ => return Err(why(format!("branch flag .{f}"))),
                    }
                }
                Some(if op == "ba" {
                    t as usize
                } else {
                    let off = ((t << 14) as i32 >> 14) as isize;
                    (self.pc as isize + off) as usize
                })
            }
            "bitwise" => {
                let name = insn.name("op", self.core).unwrap();
                let a = self.src(insn.get("src1").unwrap(), 6)?;
                let mut b = self.src2_or_rot(insn)?;
                if insn.name("inv", self.core).as_deref() == Some("~") {
                    b = !b;
                }
                let v = bitwise(&name, a, b)?;
                self.write(insn, v)?;
                None
            }
            "ima16" | "ima32" => {
                if let Some(t) = insn.name("sign", self.core)
                    && t != "u"
                    && t != "s"
                {
                    return Err(why(format!("{op}.{t}")));
                }
                for r in ["rs", "abs", "neg", "neg1", "neg2"] {
                    if let Some(t) = insn.name(r, self.core)
                        && !t.is_empty()
                    {
                        return Err(why(format!("{op} modifier {t}")));
                    }
                }
                let a = self.src0(insn.get("src0").unwrap())?;
                let (bv, cv) = (insn.get("src1").unwrap(), insn.get("src2").unwrap());
                let (mut b, mut c) = (self.src(bv, 6)?, self.src(cv, 6)?);
                if op == "ima16" {
                    (b, c) = (lane_imm(bv, b), lane_imm(cv, c));
                }
                let v = if op == "ima32" {
                    a.wrapping_mul(b).wrapping_add(c)
                } else {
                    // Hardware: a multiply-add per 16-bit lane, mod 2^16
                    // (0x80000000 * 0xffffffff + 0x5a5a5a5a = 0xda5a5a5a).
                    let lane = |s: u32| {
                        (a >> s & 0xffff).wrapping_mul(b >> s & 0xffff).wrapping_add(c >> s) & 0xffff
                    };
                    lane(0) | lane(16) << 16
                };
                self.write(insn, v)?;
                None
            }
            "imae" => {
                // Hardware (imae-modes): the selected halves multiplied, signed
                // for .s/.ssat, unsigned for .u/.usat; source 2 added at full
                // precision; .rsN shifts that sum right; then it wraps or
                // saturates to 32 bits.
                let sign = insn.name("sign", self.core).unwrap();
                let rs: u32 =
                    insn.name("rs", self.core).unwrap().strip_prefix(".rs").map_or(0, |n| n.parse().unwrap());
                let sat = sign.ends_with("sat");
                if rs != 0 && sat {
                    return Err(why("imae with both .rsN and saturation"));
                }
                let half = |v: u32, h: &str| if h == "high" { v >> 16 } else { v & 0xffff };
                let a = half(self.src0(insn.get("src0").unwrap())?, &insn.name("half0", self.core).unwrap());
                let b =
                    half(self.src(insn.get("src1").unwrap(), 6)?, &insn.name("half1", self.core).unwrap());
                let c = self.src(insn.get("src2").unwrap(), 6)?;
                let v = if sign.starts_with('s') {
                    let sum = (sext16(a) * sext16(b) + c as i32 as i64) >> rs;
                    if sat { sum.clamp(i32::MIN as i64, i32::MAX as i64) as u32 } else { sum as u32 }
                } else {
                    let sum = ((a as u64) * (b as u64) + c as u64) >> rs;
                    if sat { sum.min(u32::MAX as u64) as u32 } else { sum as u32 }
                };
                self.write(insn, v)?;
                None
            }
            "fminmax" => {
                let a = self.fsrc0(insn.get("src0").unwrap())?;
                let b = self.fsrc(insn.get("src1").unwrap())?;
                let c = self.fsrc(insn.get("src2").unwrap())?;
                // hardware (fminmax-roles, fmaxmin-roles): fminmax is
                // min(max(a, b), c), fmaxmin max(min(a, c), b); a NaN operand is
                // ignored (fminmax-specials)
                let v = match insn.name("op", self.core).unwrap().as_str() {
                    "fminmax" => flt::min(flt::max(a, b)?, c),
                    _ => flt::max(flt::min(a, c)?, b),
                };
                self.write(insn, v?)?;
                None
            }
            "fmad" | "fsubflr" => {
                let b = self.fsrc(insn.get("src1").unwrap())?;
                let c = self.fsrc(insn.get("src2").unwrap())?;
                let v = if op == "fsubflr" {
                    // hardware (fadm-fmsa-fsubflr): a - floor(b)
                    flt::sub(b, flt::floor(c))
                } else {
                    let a = self.fsrc0(insn.get("src0").unwrap())?;
                    match insn.name("op", self.core).unwrap().as_str() {
                        // hardware (sweep-fmad, fmad-fused): the product rounded,
                        // then the sum, both to nearest even
                        "fmad" => flt::add(flt::mul(a, b), c),
                        // hardware: a * a + b * c, each product rounded
                        "fmsa" => flt::add(flt::mul(a, a), flt::mul(b, c)?),
                        // (a + b) * c, but its rounding is not yet understood:
                        // one rounding of the exact result fits 640 of 864 probes
                        o => return Err(why(format!("{o} (rounding open)"))),
                    }
                };
                self.write(insn, v?)?;
                None
            }
            "idiv" => {
                // Hardware: unsigned; the quotient to the destination and the
                // remainder to the next register; by zero both are 0xffffffff.
                let a = self.src(insn.get("src1").unwrap(), 6)?;
                let b = self.src(insn.get("src2").unwrap(), 6)?;
                let (q, r) = a.checked_div(b).zip(a.checked_rem(b)).unwrap_or((u32::MAX, u32::MAX));
                let d = insn.get("dst").unwrap();
                let n = (d & 0x7f) as usize;
                if n + 1 >= 124 {
                    return Err(why("idiv into the last temporaries"));
                }
                let drc = insn.get("drc").unwrap();
                let reg = |k| if d >> 7 != 0 { Reg::Pa(n + k) } else { Reg::R(n + k) };
                self.pending.push((drc, reg(0), q));
                self.pending.push((drc, reg(1), r));
                None
            }
            "test" => {
                self.test(insn)?;
                None
            }
            "lapc" => {
                let flags = insn.name("flags", self.core).unwrap_or_default();
                if !flags.is_empty() && flags != ".nosched" {
                    return Err(why(format!("lapc flags {flags}")));
                }
                Some(self.link as usize)
            }
            "mov_to_link" => {
                let v = insn.get("src").unwrap();
                self.link =
                    if self.width(insn, "src") == 7 { self.temp(v as usize)? } else { self.src(v, 6)? };
                None
            }
            "mov_from_link" => {
                let link = self.link;
                if self.width(insn, "dst") == 7 {
                    let r = insn.get("dst").unwrap() as usize;
                    if r < 124 { self.set_r(r, link) } else { self.set_i(r - 124, link)? }
                } else {
                    self.write(insn, link)?;
                }
                None
            }
            "ldr" | "str" => {
                let iv = insn.get("index").unwrap();
                let index = if self.width(insn, "index") == 14 { iv } else { self.src(iv, 6)? };
                if op == "str" {
                    let v = self.src(insn.get("src").unwrap(), 6)?;
                    bus.reg_write(index, v)?;
                } else {
                    let v = bus.reg_read(index)?;
                    let d = insn.get("dst").unwrap();
                    let r = (d & 0x7f) as usize;
                    let drc = insn.get("drc").unwrap();
                    self.pending.push((drc, if d >> 7 != 0 { Reg::Pa(r) } else { Reg::R(r) }, v));
                }
                None
            }
            "smlsi" => {
                let modes = insn.name("modes", self.core).unwrap();
                if modes.contains("swizzle") {
                    return Err(why("smlsi swizzle mode"));
                }
                if ["l0", "l1", "l2"].iter().any(|r| insn.get(r) != Some(0)) {
                    return Err(why("smlsi with non-zero limits"));
                }
                let inc = |r: &str| insn.get(r).unwrap() as u8 as i8 as i32;
                self.moe_inc = Some([inc("inc0"), inc("inc1"), inc("inc2"), inc("inc3")]);
                None
            }
            // one instance holds every mutex; no caches are modelled
            "lock" | "cache" => None,
            "emitpds" => {
                let srcs = [
                    self.src0(insn.get("src0").unwrap())?,
                    self.src(insn.get("src1").unwrap(), 6)?,
                    self.src(insn.get("src2").unwrap(), 6)?,
                ];
                bus.emit(&insn.text, srcs)?;
                None
            }
            "ld" | "ld.pre" | "ld.post" | "st" | "st.pre" | "st.post" => {
                self.load_store(insn, op, bus)?;
                None
            }
            "pack" => {
                self.pack(insn)?;
                None
            }
            "sop2" | "sop2wm" => {
                self.sop2(insn)?;
                None
            }
            "movc" => {
                let (s0, s1, s2) = (
                    self.src0(insn.get("src0").unwrap())?,
                    self.src(insn.get("src1").unwrap(), 6)?,
                    self.src(insn.get("src2").unwrap(), 6)?,
                );
                let ty = insn.name("type", self.core).unwrap_or(".i8".into());
                let test = insn.name("test", self.core).unwrap();
                self.write(insn, movc(&ty, &test, s0, s1, s2)?)?;
                None
            }
            "movmsk" => {
                self.movmsk(insn)?;
                None
            }
            "sop3" | "lrp1" | "lrp2" => {
                self.sop3(insn)?;
                None
            }
            _ => return Err(why(format!("op {op} not modelled yet"))),
        })
    }

    /// The test group: an ALU operation whose result (unless the destination is
    /// "!") is written and tested into p<n>.
    fn test(&mut self, insn: &Insn) -> Result<(), Error> {
        let name = insn.name("op", self.core).unwrap();
        let av = insn.get("src1").unwrap();
        let Some(bv) = insn.get("src2") else { return Err(why(format!("one-source test op {name}"))) };
        let (mut a, mut b) = (self.src(av, 6)?, self.src(bv, 6)?);
        if name.ends_with("16") {
            (a, b) = (lane_imm(av, a), lane_imm(bv, b));
        }
        if name.ends_with('8') && (av >> 7 == 6 || bv >> 7 == 6) {
            return Err(why(format!("{name} with an immediate (its lanes not measured)")));
        }
        let (result, tested) = alu_test_value(&name, a, b)?;
        let test = insn.name("test", self.core).unwrap();
        let p = predicate(&test, tested)?;
        if insn.name("write", self.core).as_deref() != Some("!") {
            self.write(insn, result)?;
        }
        self.p[insn.get("p").unwrap() as usize] = p;
        Ok(())
    }

    /// ld/st: [base, +/-offset] with the offset in units of the data size; the
    /// .pre/.post forms write the moved base back before/after the access.
    fn load_store(&mut self, insn: &Insn, op: &str, bus: &mut dyn Bus) -> Result<(), Error> {
        let amdt = insn.name("amdt", self.core).unwrap();
        if amdt != "ad" {
            return Err(why(format!("access {amdt}")));
        }
        let flags = insn.name("flags", self.core).unwrap();
        let (mut count, mut dst_inc) = (1u32, 1i32);
        for f in flags.split('.').filter(|f| !f.is_empty()) {
            match f {
                "skipinv" | "syncs" | "nosched" | "bpcache" | "fcfill" | "bypassl1" => {}
                // A fetch of N dwords steps the destination by the MOE
                // increment smlsi set (the last of its four, inferred from the
                // firmware: it sets only that one, to 1, before its fetches;
                // not measured). With the MOE state unknown it stops: the vec1
                // probe's ldad.fetch4 r30 left r31 unchanged.
                _ if f.starts_with("fetch") && op.starts_with("ld") => match self.moe_inc {
                    Some(inc) => {
                        count = f[5..].parse().unwrap();
                        dst_inc = inc[3];
                    }
                    None => return Err(why(format!("flag .{f} with the MOE state unknown"))),
                },
                _ if f.starts_with("fetch") => return Err(why(format!("flag .{f} on a store"))),
                _ => return Err(why(format!("flag .{f}"))),
            }
        }
        if insn.get("range").is_some_and(|r| r >> 10 != 0) {
            return Err(why("load range"));
        }
        let neg = insn.name("sign", self.core).is_some_and(|s| s.starts_with('-'));
        // An immediate offset counts data-size units; a register one holds a
        // stride and an index, (high 16 bits) * (low 16 bits) bytes (hardware:
        // 0x00010001 moves by 1 byte, 0x00020002 and 0x00040001 by 4).
        let ov = insn.get("offset").unwrap();
        let o = self.src(ov, 6)?;
        let off = if ov >> 7 == 6 { o.wrapping_mul(4) } else { (o >> 16).wrapping_mul(o & 0xffff) };
        let base_v = insn.get("base").unwrap();
        let base = self.src0(base_v)?;
        let moved = if neg { base.wrapping_sub(off) } else { base.wrapping_add(off) };
        let addr = match op {
            "ld" | "st" => moved,
            o if o.ends_with(".pre") => moved,
            _ => base,
        };
        if op.contains('.') {
            self.write_src0(base_v, moved)?;
        }
        if op.starts_with("st") {
            let v = self.src(insn.get("src").unwrap(), 6)?;
            return bus.store32(addr, v);
        }
        let d = insn.get("dst").unwrap();
        for k in 0..count {
            let v = bus.load32(addr.wrapping_add(4 * k))?;
            let r = ((d & 0x7f) as i32 + k as i32 * dst_inc) as usize;
            if r >= 124 {
                return Err(why("fetch past the temporaries"));
            }
            let drc = insn.get("drc").unwrap();
            self.pending.push((drc, if d >> 7 != 0 { Reg::Pa(r) } else { Reg::R(r) }, v));
        }
        Ok(())
    }

    /// A source-0 operand: number[6:0] | bank (0 r, 1 pa, 2 o, 3 sa).
    fn src0(&self, v: u32) -> Result<u32, Error> {
        let r = (v & 0x7f) as usize;
        Ok(match v >> 7 {
            0 => self.temp(r)?,
            1 => {
                self.not_pending(Reg::Pa(r))?;
                read(self.pa[r], self.written.pa, r, "pa")?
            }
            2 => read(self.o[r], self.written.o, r, "o")?,
            3 => read(self.sa[r], self.written.sa, r, "sa")?,
            b => return Err(why(format!("source-0 bank {b}"))),
        })
    }

    fn write_src0(&mut self, v: u32, x: u32) -> Result<(), Error> {
        let r = (v & 0x7f) as usize;
        match v >> 7 {
            0 if r < 124 => self.set_r(r, x),
            1 => self.set_pa(r, x),
            2 => (self.o[r], self.written.o) = (x, self.written.o | 1 << r),
            3 => (self.sa[r], self.written.sa) = (x, self.written.sa | 1 << r),
            b => return Err(why(format!("source-0 bank {b} written"))),
        }
        Ok(())
    }

    /// A float source 1/2: number[6:0] | bank[9:7] | neg[10] | abs[11].
    fn fsrc(&self, v: u32) -> Result<u32, Error> {
        Ok(flt::modify(self.src(v & 0x3ff, 6)?, v >> 10 & 1 != 0, v >> 11 & 1 != 0))
    }

    /// A float source 0: number[6:0] | pa[7] | abs[8].
    fn fsrc0(&self, v: u32) -> Result<u32, Error> {
        Ok(flt::modify(self.src0(v & 0xff)?, false, v >> 8 & 1 != 0))
    }

    fn width(&self, insn: &Insn, role: &str) -> u32 {
        insn.form.role(role).map_or(0, |i| insn.form.f[i].width())
    }

    /// Whether the instruction ends the program (its flags print .end).
    fn ends(&self, insn: &Insn) -> bool {
        ["end", "flags", "op"].iter().any(|r| insn.name(r, self.core).is_some_and(|t| t.contains(".end")))
    }

    /// Source 2, or the rotated immediate of the bitwise group.
    fn src2_or_rot(&self, insn: &Insn) -> Result<u32, Error> {
        let i = insn.form.role("src2").unwrap();
        let f = &insn.form.f[i];
        if f.width() == 21 {
            // the rotated immediate: mantissa[15:0] | rotate[20:16]
            let v = insn.v[i];
            return Ok((v & 0xffff).rotate_left(v >> 16 & 31));
        }
        self.src(insn.v[i], 6)
    }

    /// A source-1/2 operand: number[6:0] | bank[9:7].
    fn src(&self, v: u32, _imm_bank: u32) -> Result<u32, Error> {
        let r = (v & 0x7f) as usize;
        Ok(match v >> 7 {
            0 => self.temp(r)?,
            1 => read(self.o[r], self.written.o, r, "o")?,
            2 => {
                self.not_pending(Reg::Pa(r))?;
                read(self.pa[r], self.written.pa, r, "pa")?
            }
            3 => read(self.sa[r], self.written.sa, r, "sa")?,
            6 => r as u32,
            7 if r < 4 => self.internal(r)?,
            b => return Err(why(format!("source bank {b}"))),
        })
    }

    fn temp(&self, r: usize) -> Result<u32, Error> {
        if r >= 124 {
            return self.internal(r - 124);
        }
        self.not_pending(Reg::R(r))?;
        read(self.r[r], self.written.r, r, "r")
    }

    fn not_pending(&self, reg: Reg) -> Result<(), Error> {
        if self.pending.iter().any(|p| p.1 == reg) {
            return Err(why(format!("reads {reg:?} before its DRC result lands (wdf)")));
        }
        Ok(())
    }

    fn internal(&self, r: usize) -> Result<u32, Error> {
        if r == 3 {
            // hardware: i3 is the next quad pixel's i0 (one instance cannot say)
            return Err(why("i3 is the quad neighbour's i0"));
        }
        read(self.i[r], self.written.i as u128, r, "i")
    }

    /// Write the destination (number[6:0] | bank[9:7]), refusing a write mask.
    fn write(&mut self, insn: &Insn, v: u32) -> Result<(), Error> {
        if let Some(m) = insn.name("mask", self.core)
            && !m.is_empty()
        {
            return Err(why(format!("write mask {m}")));
        }
        self.write_dst(insn.get("dst").unwrap(), v)
    }

    /// Write a destination operand (number[6:0] | bank[9:7]).
    fn write_dst(&mut self, d: u32, v: u32) -> Result<(), Error> {
        let r = (d & 0x7f) as usize;
        match d >> 7 {
            0 if r < 124 => self.set_r(r, v),
            0 => self.set_i(r - 124, v)?,
            1 => (self.o[r], self.written.o) = (v, self.written.o | 1 << r),
            2 => (self.pa[r], self.written.pa) = (v, self.written.pa | 1 << r),
            4 => (self.sa[r], self.written.sa) = (v, self.written.sa | 1 << r),
            7 if r < 4 => self.set_i(r, v)?,
            b => return Err(why(format!("destination bank {b}"))),
        }
        Ok(())
    }

    fn set_r(&mut self, r: usize, v: u32) {
        self.r[r] = v;
        self.written.r |= 1 << r;
    }

    fn set_pa(&mut self, r: usize, v: u32) {
        self.pa[r] = v;
        self.written.pa |= 1 << r;
    }

    fn set_i(&mut self, r: usize, v: u32) -> Result<(), Error> {
        if r == 3 {
            return Err(why("i3 is the quad neighbour's i0"));
        }
        self.i[r] = v;
        self.written.i |= 1 << r;
        Ok(())
    }
}

/// The byte a colour factor selects for byte `ch` from sources s0-s2 (see
/// Machine::sop2): zero, s0-s2 (this byte), s0a-s2a (the alpha bytes), asat,
/// sop2's s2scale (s2) and zeros (0), and .comp for 1 - factor.
fn colour_factor(sel: &str, ch: u32, s: [u32; 3]) -> Result<i64, Error> {
    let byte = |w: u32, c: u32| (w >> (8 * c) & 0xff) as i64;
    let (name, comp) = sel.strip_suffix(".comp").map_or((sel, false), |n| (n, true));
    let (a1, a2) = (byte(s[1], 3), byte(s[2], 3));
    let f = match name {
        "zero" | "zeros" => 0,
        "s0" => byte(s[0], ch),
        "s1" => byte(s[1], ch),
        "s2" | "s2scale" => byte(s[2], ch),
        "s0a" => byte(s[0], 3),
        "s1a" => a1,
        "s2a" => a2,
        // hardware (sop2wm): on the alpha byte asat is 1, with or without
        // .comp (sop2's alpha selectors have no asat)
        "asat" if ch == 3 => return Ok(255),
        "asat" if comp => return Ok(a2.min(255 - a1)),
        "asat" => a1.min(255 - a2),
        _ => return Err(why(format!("colour factor {sel}"))),
    };
    Ok(if comp { 255 - f } else { f })
}

/// A sum of byte products over 255, rounded once (half up) and clamped.
fn colour_round(t: i64) -> u32 {
    (2 * t + 255).div_euclid(510).clamp(0, 255) as u32
}

/// sop3, lrp1 and lrp2, all four bytes (see Machine::sop3).
fn blend3(op: &str, csel: &str, alpha: (&str, &str), s: [u32; 3]) -> Result<u32, Error> {
    let byte = |w: u32, c: u32| (w >> (8 * c) & 0xff) as i64;
    let f = |sel: &str, ch: u32| colour_factor(sel, ch, s);
    let c: Vec<&str> = csel.split(", ").collect();
    let sum = |op: &str, a: i64, b: i64| if op == "sub" { a - b } else { a + b };
    let mut w = 0;
    for ch in 0..3 {
        let (x1, x2) = (byte(s[1], ch), byte(s[2], ch));
        let t = match (op, &c[..]) {
            ("sop3", [f1, f2, o @ ("add" | "sub")]) => sum(o, x1 * f(f1, ch)?, x2 * f(f2, ch)?),
            ("lrp1", [f1, f2, t]) => {
                let w = if *t == "s0" { byte(s[0], ch) } else { byte(s[0], 3) };
                f(f1, ch)? * (255 - w) + f(f2, ch)? * w
            }
            ("lrp2", [f1, f2, a, b]) => {
                let x = |n: &str| match n {
                    "one" => 255,
                    "s1" => x1,
                    "s1.comp" => 255 - x1,
                    "s2" => x2,
                    _ => 255 - x2,
                };
                x(a) * f(f1, ch)? + x(b) * f(f2, ch)?
            }
            _ => return Err(why(format!("{op} colour {csel}"))),
        };
        w |= colour_round(t) << (8 * ch);
    }
    let (a0, a1, a2) = (byte(s[0], 3), byte(s[1], 3), byte(s[2], 3));
    let a: Vec<&str> = alpha.1.split(", ").collect();
    let t = match (alpha.0, &a[..]) {
        ("asop", [g1, g2, o]) => sum(o, a1 * f(g1, 3)?, a2 * f(g2, 3)?),
        // arsop: sub is s2a * g - s1a * (the second colour factor); add
        // gave 0 in every case measured
        ("arsop", [g, "sub"]) => a2 * f(g, 3)? - a1 * f(c[1], 3)?,
        ("arsop", [_, "add"]) => 0,
        ("alrp", [g1, g2]) => f(g1, 3)? * (255 - a0) + f(g2, 3)? * a0,
        _ => return Err(why(format!("{op} alpha {}", alpha.1))),
    };
    Ok(w | colour_round(t) << 24)
}

/// movc: per lane of the type, s1 where the lane of s0 passes the test, else
/// s2. Hardware (movc-i8, -i16, -i32, -i10, -flt): the lanes are signed; i10
/// has three 10-bit lanes and writes bits 30-31 as 0; flt compares as f32
/// with denormals as zero (a NaN is non-zero, neither negative nor positive).
fn movc(ty: &str, test: &str, s0: u32, s1: u32, s2: u32) -> Result<u32, Error> {
    let pass = |x: f64| match test {
        ".tz" => x == 0.0,
        ".tnz" => x != 0.0,
        ".tn" => x < 0.0,
        _ => x >= 0.0,
    };
    if ty == ".flt" {
        let x = if s0 >> 23 & 0xff == 0 { 0.0 } else { f32::from_bits(s0) as f64 };
        return Ok(if pass(x) { s1 } else { s2 });
    }
    let (w, n) = match ty {
        ".i8" => (8, 4),
        ".i16" => (16, 2),
        ".i32" => (32, 1),
        ".i10" => (10, 3),
        _ => return Err(why(format!("movc{ty}"))),
    };
    let mut r = 0;
    for k in 0..n {
        let m = (u64::MAX >> (64 - w)) << (w * k);
        let lane = ((s0 as u64 & m) >> (w * k)) as i64;
        let x = if lane >> (w - 1) & 1 != 0 { lane - (1 << w) } else { lane };
        r |= (if pass(x as f64) { s1 } else { s2 }) as u64 & m;
    }
    Ok(r as u32)
}

/// One byte of a sop2 blend: `spec` is the printed selection ("[comp, ]f1,
/// f2, op[, neg]"), see Machine::sop2.
fn blend(spec: &str, s1: u32, s2: u32, ch: u32) -> Result<u32, Error> {
    let byte = |w: u32, c: u32| (w >> (8 * c) & 0xff) as i64;
    let mut parts: Vec<&str> = spec.split(", ").collect();
    let neg = parts.last() == Some(&"neg");
    if neg {
        parts.pop();
    }
    let pre = parts.first() == Some(&"comp");
    if pre {
        parts.remove(0);
    }
    let [f1, f2, op] = parts[..] else { return Err(why(format!("sop2 selection {spec}"))) };
    let factor = |sel: &str| colour_factor(sel, ch, [0, s1, s2]);
    let (g1, g2) = (factor(f1)?, factor(f2)?);
    let (mut x1, mut x2) = (byte(s1, ch), byte(s2, ch));
    let k = if f1.starts_with("s2scale") { 2 } else { 1 };
    let mut t = match op {
        "min" => x1.min(x2) * 255,
        "max" => x1.max(x2) * 255,
        _ => {
            if pre {
                x1 = 255 - x1;
            }
            if f2.starts_with("zeros") {
                x2 -= 128;
            }
            if op == "add" { x1 * g1 + x2 * g2 } else { x1 * g1 - x2 * g2 }
        }
    };
    if neg {
        t = -t;
    }
    Ok(colour_round(k * t))
}

/// A read of register n of a bank: only what the task wrote is defined.
fn read(v: u32, written: u128, n: usize, bank: &str) -> Result<u32, Error> {
    if written >> n & 1 == 0 {
        return Err(why(format!("reads {bank}{n} before the program writes it")));
    }
    Ok(v)
}

/// A 16-bit lane operation takes an immediate in both lanes (hardware:
/// isub16 r4, r1, #1 subtracts 1 from each half).
fn lane_imm(field: u32, v: u32) -> u32 {
    if field >> 7 == 6 { v | v << 16 } else { v }
}

fn why(s: impl Into<String>) -> Error {
    Error::Unsupported { pc: 0, text: String::new(), why: s.into() }
}

fn unsupported(pc: usize, text: &str, why: &str) -> Error {
    Error::Unsupported { pc, text: text.to_string(), why: why.to_string() }
}

fn bitwise(op: &str, a: u32, b: u32) -> Result<u32, Error> {
    Ok(match op {
        "and" => a & b,
        "or" => a | b,
        "xor" => a ^ b,
        "shl" => a.checked_shl(b).unwrap_or(0),
        "shr" => a.checked_shr(b).unwrap_or(0),
        "asr" => ((a as i32).checked_shr(b).unwrap_or(if (a as i32) < 0 { -1 } else { 0 })) as u32,
        "rol" => a.rotate_left(b & 31),
        _ => return Err(why(format!("bitwise {op}"))),
    })
}

/// What a test-group operation tests: per channel, the value the test sees, or
/// None where the operation has no lane (hardware: a 16-bit operation tests its
/// lanes at chan0 and chan2, its unwrapped signed sum).
struct Tested([Option<i64>; 4]);

fn sext16(x: u32) -> i64 {
    x as u16 as i16 as i64
}

fn alu_test_value(op: &str, a: u32, b: u32) -> Result<(u32, Tested), Error> {
    let lanes = |f: fn(i64, i64) -> i64, signed: bool| {
        let ext = |x: u32| if signed { sext16(x) } else { (x & 0xffff) as i64 };
        let lo = f(ext(a), ext(b));
        let hi = f(ext(a >> 16), ext(b >> 16));
        ((lo as u32 & 0xffff) | (hi as u32) << 16, Tested([Some(lo), None, Some(hi), None]))
    };
    // 8-bit: four byte lanes, chanN tests byte N's unwrapped sum (hardware:
    // iadd8 of 0x80 in byte 0 tests negative at chan0, 0xffffff00 zero)
    let bytes = |f: fn(i64, i64) -> i64, signed: bool| {
        let ext = |x: u32, k: u32| {
            let v = x >> (8 * k) & 0xff;
            if signed { v as u8 as i8 as i64 } else { v as i64 }
        };
        let mut r = 0u32;
        let mut t = [None; 4];
        for k in 0..4 {
            let v = f(ext(a, k), ext(b, k));
            r |= (v as u32 & 0xff) << (8 * k);
            t[k as usize] = Some(v);
        }
        (r, Tested(t))
    };
    // 32-bit: the full result, chan0 only
    let word = |r: u32| (r, Tested([Some(r as i32 as i64), None, None, None]));
    Ok(match op {
        "iadd8" => bytes(|x, y| x + y, true),
        "isub8" => bytes(|x, y| x - y, true),
        "iaddu8" => bytes(|x, y| x + y, false),
        "isubu8" => bytes(|x, y| x - y, false),
        "iadd16" => lanes(|x, y| x + y, true),
        "isub16" => lanes(|x, y| x - y, true),
        "iaddu16" => lanes(|x, y| x + y, false),
        "isubu16" => lanes(|x, y| x - y, false),
        // Hardware: iadd32 adds the sign-extended low half of source 1 to
        // source 2 (0x337a70a2 + 0xb01b9c1d = 0xb01c0cbf).
        "iadd32" => word((sext16(a) as u32).wrapping_add(b)),
        // unsigned: the test sees the unsigned sum (a sum with bit 31 set is
        // not negative)
        "iaddu32" => {
            let r = (a & 0xffff).wrapping_add(b);
            (r, Tested([Some(r as i64), None, None, None]))
        }
        "and" | "or" | "xor" | "shl" | "shr" | "asr" | "rol" => word(bitwise(op, a, b)?),
        // float: the result tests by its sign and zero at chan0 (test-channels)
        "fadd" | "fsub" | "fmul" => {
            let mut r = match op {
                "fadd" => flt::add(Ok(a), b),
                "fsub" => flt::sub(a, Ok(b)),
                _ => flt::mul(a, b),
            }?;
            // hardware (x-misc): fmul of 0 and -3 is +0, not IEEE's -0 (the
            // only signed-zero case measured)
            if op == "fmul" && r == 0x8000_0000 {
                r = 0;
            }
            if r == 0x8000_0000 {
                return Err(why(format!("{op} test of -0")));
            }
            let t = if r == 0 {
                0
            } else if r >> 31 != 0 {
                -1
            } else {
                1
            };
            (r, Tested([Some(t), None, None, None]))
        }
        _ => return Err(why(format!("test op {op}"))),
    })
}

/// A test type: sign test t/n/p, all (a) or any (o), zero test t/z/nz, and the
/// channel.
fn predicate(test: &str, t: Tested) -> Result<bool, Error> {
    let bad = || why(format!("test {test}"));
    let (kind, sel) = test.split_once('.').ok_or_else(bad)?;
    let kind = kind.strip_prefix("test").ok_or_else(bad)?;
    let (sign, rest) = kind.split_at(1);
    let (all, zero) = rest.split_at(1);
    // the channel selection: one channel, or several combined (andall/orall:
    // all four; and02/or02: chan0 and chan2)
    let (chans, every): (Vec<usize>, bool) = match sel {
        "andall" => ((0..4).collect(), true),
        "orall" => ((0..4).collect(), false),
        "and02" => (vec![0, 2], true),
        "or02" => (vec![0, 2], false),
        c => (vec![c.strip_prefix("chan").and_then(|n| n.parse().ok()).ok_or_else(bad)?], true),
    };
    // A channel the operation has no lane for tests as a positive non-zero
    // value (hardware: chan1 of iadd16 fails taz; andall of iadd16/iadd32
    // fails taz and nat, orall passes pat and tanz).
    let one =
        |c: usize| -> Result<bool, Error> { test_one(sign, all, zero, t.0[c].unwrap_or(1)).ok_or_else(bad) };
    let mut r = every;
    for c in chans {
        let x = one(c)?;
        r = if every { r && x } else { r || x };
    }
    Ok(r)
}

fn test_one(sign: &str, all: &str, zero: &str, v: i64) -> Option<bool> {
    let s = match sign {
        "t" => true,
        "n" => v < 0,
        "p" => v >= 0, // hardware: a zero result tests positive
        _ => return None,
    };
    let z = match zero {
        "t" => true,
        "z" => v == 0,
        "nz" => v != 0,
        _ => return None,
    };
    Some(if all == "a" { s && z } else { s || z })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn prog(src: &[&str]) -> Program {
        let words: Vec<u64> =
            src.iter().map(|t| usse_isa::encode(t, Core::Sgx545).unwrap_or_else(|| panic!("{t}"))).collect();
        Program::new(&words, Core::Sgx545)
    }

    fn run(src: &[&str]) -> (Machine, Result<(), Error>) {
        let mut m = Machine::default();
        let mut mem = Memory { base: 0x1000, words: vec![0; 64] };
        let r = m.run(&prog(src), &mut mem, 1000);
        (m, r)
    }

    #[test]
    fn br_counts_from_the_branch_itself() {
        // br #1 is the next instruction: the or still runs
        let (m, r) =
            run(&["mov r1, #0x00000000", "br #0x00000001", "or r1, r1, #0x00000001", "mov.end o0, r1"]);
        assert_eq!((r, m.o[0]), (Ok(()), 1));
        let (m, r) =
            run(&["mov r1, #0x00000000", "br #0x00000002", "or r1, r1, #0x00000001", "mov.end o0, r1"]);
        assert_eq!((r, m.o[0]), (Ok(()), 0));
    }

    #[test]
    fn what_the_task_did_not_write_stops_it() {
        let (_, r) = run(&["mov.end o0, r5"]);
        assert!(matches!(r, Err(Error::Unsupported { why, .. }) if why.contains("r5 before")));
        let (_, r) = run(&["mov i3, r1"]);
        assert!(matches!(r, Err(Error::Unsupported { .. })));
    }

    #[test]
    fn drc_results_land_at_wdf() {
        let div = ["mov r1, #0x00000064", "mov r2, #0x00000007", "idiv r10, r1, r2, drc0"];
        let (_, r) = run(&[div[0], div[1], div[2], "mov.end o0, r10"]);
        assert!(matches!(r, Err(Error::Unsupported { why, .. }) if why.contains("DRC")));
        let (m, r) = run(&[div[0], div[1], div[2], "wdf drc0", "mov.end o0, r11"]);
        assert_eq!((r, m.r[10], m.o[0]), (Ok(()), 14, 2));
    }

    /// SGX registers as a map, logging the accesses.
    struct Regs(std::collections::BTreeMap<u32, u32>, Vec<String>);

    impl Bus for Regs {
        fn load32(&mut self, a: u32) -> Result<u32, Error> {
            Err(Error::Fault(a))
        }
        fn store32(&mut self, a: u32, _: u32) -> Result<(), Error> {
            Err(Error::Fault(a))
        }
        fn reg_read(&mut self, i: u32) -> Result<u32, Error> {
            self.1.push(format!("ldr {i}"));
            Ok(*self.0.get(&i).unwrap_or(&0))
        }
        fn reg_write(&mut self, i: u32, v: u32) -> Result<(), Error> {
            self.1.push(format!("str {i} {v:#x}"));
            self.0.insert(i, v);
            Ok(())
        }
    }

    #[test]
    fn calls_return_through_the_link() {
        // 0: call 3; 1: r1 |= 2; 2: end; 3: r1 = 1; 4: return
        let (m, r) = run(&[
            "ba.savelink #0x00000003",
            "or r1, r1, #0x00000002",
            "mov.end o0, r1",
            "mov r1, #0x00000001",
            "lapc",
        ]);
        assert_eq!((r, m.o[0]), (Ok(()), 3));
        // the link moves through a register and back
        let (m, r) = run(&[
            "ba.savelink #0x00000002",
            "mov.end o0, r1",
            "mov r5, pclink",
            "mov pclink, r5",
            "mov r1, #0x00000007",
            "lapc",
        ]);
        assert_eq!((r, m.o[0]), (Ok(()), 7));
    }

    #[test]
    fn ldr_and_str_reach_the_sgx_registers() {
        let p =
            prog(&["mov r1, #0x00000005", "str #10, r1", "ldr r2, #10, drc0", "wdf drc0", "mov.end o0, r2"]);
        let mut bus = Regs(Default::default(), vec![]);
        let mut m = Machine::default();
        assert_eq!(m.run(&p, &mut bus, 100), Ok(()));
        assert_eq!((m.o[0], bus.1), (5, vec!["str 10 0x5".to_string(), "ldr 10".to_string()]));
    }

    const R1_R7: [&str; 7] = [
        "mov r1, #0x00000011",
        "mov r2, #0x00000022",
        "mov r3, #0x00000033",
        "mov r4, #0x00000044",
        "mov r5, #0x00000055",
        "mov r6, #0x00000066",
        "mov r7, #0x00000077",
    ];

    fn run_after(init: &[&str], src: &[&str]) -> (Machine, Result<(), Error>) {
        run(&[init, src, &["nop.end"]].concat())
    }

    #[test]
    fn repeats_run_one_iteration_after_another() {
        // hardware (repeat-order): mov.repeat3 r2, r1 copies r1 into r2-r4
        let (m, r) = run_after(&R1_R7, &["mov.repeat3 r2, r1"]);
        assert_eq!((r, &m.r[1..5]), (Ok(()), &[0x11; 4][..]));
        let (m, r) = run_after(&R1_R7, &["mov.repeat3 r1, r2"]);
        assert_eq!((r, &m.r[1..5]), (Ok(()), &[0x22, 0x33, 0x44, 0x44][..]));
    }

    #[test]
    fn a_repeat_is_predicated_once() {
        // hardware (repeat-pred): both iterations or neither
        let (m, r) = run_after(&R1_R7, &["mov r10, #0x00000001", "and.testtaz.chan0 !c0, p0, r10, r10"]);
        assert_eq!(r, Ok(()));
        let p = m.p;
        let (m, r) = run_after(
            &R1_R7,
            &["mov r10, #0x00000000", "and.testtaz.chan0 !c0, p0, r10, r10", "p0 mov.repeat2 r1, r5"],
        );
        assert_eq!((r, p[0], &m.r[1..4]), (Ok(()), false, &[0x55, 0x66, 0x33][..]));
    }

    const ABC: [&str; 6] = [
        "mov r1, #0x3f800000", // 1
        "mov r2, #0x40400000", // 3
        "mov r3, #0x40a00000", // 5
        "mov r4, #0x40e00000", // 7
        "mov r5, #0x42c80000", // 100
        "mov r6, #0x43480000", // 200
    ];

    #[test]
    fn incs_picks_the_sources_that_advance() {
        // hardware (incs-fmad): .incsXYZ advances source 0, 1, 2; the
        // destination always advances; no .incs advances every source
        for (incs, second) in [("", 221.0), (".incs000", 105.0), (".incs100", 115.0), (".incs010", 107.0)]
            .into_iter()
            .chain([(".incs001", 205.0), (".incs110", 121.0), (".incs111", 221.0)])
        {
            let (m, r) = run_after(&ABC, &[&format!("fmad.repeat2{incs} r10, r1, r3, r5")]);
            assert_eq!(
                (r, m.r[10], m.r[11]),
                (Ok(()), 105f32.to_bits(), (second as f32).to_bits()),
                "{incs}"
            );
        }
    }

    #[test]
    fn fdp_sums_its_products_in_turn() {
        // hardware (fdp-count): fdp.repeatN d, a, b sums a[k] * b[k] into
        // d + N - 1 only
        let (m, r) = run_after(&ABC, &["fdp.repeat3 r10, r1, r4"]);
        assert_eq!((r, m.r[12], m.written.r >> 10 & 7), (Ok(()), 1307f32.to_bits(), 0b100));
        let (m, r) = run_after(&ABC, &["fdp.repeat2.incs010 r10, r1, r4"]);
        assert_eq!((r, m.r[11]), (Ok(()), 28f32.to_bits()));
        // fdp-rounding: each product and each sum rounded, in order, so
        // 1 + 2^-24 - 1 + 2^-24 is 2^-24
        let (m, r) = run_after(
            &[
                "mov r1, #0x3f800000",
                "mov r2, #0x33800000",
                "mov r3, #0x3f800000",
                "mov r4, #0x33800000",
                "mov r5, #0x3f800000",
                "mov r6, #0x3f800000",
                "mov r7, #0xbf800000",
                "mov r8, #0x3f800000",
            ],
            &["fdp.repeat4 r10, r1, r5"],
        );
        assert_eq!((r, m.r[13]), (Ok(()), 0x33800000));
    }

    #[test]
    fn float_tests_read_the_result_as_a_float() {
        // hardware (test-channels): fadd tests its float result at chan0;
        // other channels test as positive non-zero
        let t = |op: &str, a: &str, b: &str| {
            let (m, r) = run_after(&[&format!("mov r1, #{a}"), &format!("mov r2, #{b}")], &[op]);
            r.map(|_| m.p[0])
        };
        let (two, three, zero) = ("0x40000000", "0x40400000", "0x00000000");
        assert_eq!(t("fadd.testnat.chan0 !c0, p0, r1, r2", "0xc0000000", zero), Ok(true));
        assert_eq!(t("fadd.testnat.chan0 !c0, p0, r1, r2", three, zero), Ok(false));
        assert_eq!(t("fadd.testtaz.chan0 !c0, p0, r1, r2", zero, zero), Ok(true));
        assert_eq!(t("fadd.testpat.chan1 !c0, p0, r1, r2", "0xc0000000", zero), Ok(true));
        assert_eq!(t("fadd.testtaz.chan0 r3, p0, r1, r2", two, three), Ok(false));
        // x-misc: fmul of 0 and -3 is +0, and tests zero
        let (m, r) = run_after(
            &["mov r1, #0x00000000", "mov r2, #0xc0400000"],
            &["fmul.testtaz.chan0 r3, p0, r1, r2"],
        );
        assert_eq!((r, m.r[3], m.p[0]), (Ok(()), 0, true));
        // fadd's -0 is not measured
        assert!(t("fadd.testtaz.chan0 !c0, p0, r1, r2", "0x80000000", "0x80000000").is_err());
    }

    #[test]
    fn unpacks_to_f32_scale_each_component() {
        let u = |op: &str, w: &str| {
            let (m, r) = run_after(&[&format!("mov r1, #{w}")], &[&format!("{op}, r1.0, nearest")]);
            r.map(|_| m.r[10])
        };
        let f = |x: f32| Ok(x.to_bits());
        // hardware (unpack-*-scale): u8 / 255, s8 / 127 clamped at -1, o8 is
        // s8 of the byte ^ 0x80, each rounded once to nearest
        assert_eq!(u("unpckf32u8.scale r10, r1.1", "0x00000100"), Ok(0x3b808081));
        assert_eq!(u("unpckf32s8.scale r10, r1.0", "0x00000080"), f(-1.0));
        assert_eq!(u("unpckf32s8.scale r10, r1.0", "0x00000001"), Ok(0x3c010204));
        assert_eq!(u("unpckf32o8.scale r10, r1.0", "0x00000008"), f(-120.0 / 127.0));
        assert_eq!(u("unpckf32s16.scale r10, r1.2", "0x80000000"), f(-1.0));
        assert_eq!(u("unpckf32u16.scale r10, r1.2", "0x00010000"), Ok(0x37800080));
        // unpack-select: without .scale the integer; a 16-bit select is a
        // byte offset
        assert_eq!(u("unpckf32u8 r10, r1.3", "0x84838281"), f(132.0));
        assert_eq!(u("unpckf32u16 r10, r1.1", "0x84838281"), f(33666.0));
        assert!(u("unpckf32u16 r10, r1.3", "0x84838281").is_err());
        assert!(u("unpckf32u8.scale r10.bytemask0011, r1.0", "0x00000001").is_err());
    }

    #[test]
    fn packs_round_saturate_and_place_their_two_values() {
        let pk = |op: &str, a: f32, b: f32| {
            let (m, r) = run_after(
                &[
                    &format!("mov r1, #0x{:08x}", a.to_bits()),
                    &format!("mov r2, #0x{:08x}", b.to_bits()),
                    "mov r10, #0xa0a1a2a3",
                ],
                &[op],
            );
            r.map(|_| m.r[10])
        };
        // pack-*-int: rounded by the mode, saturated; 8-bit values go to bytes
        // 0-3 as s1, s2, s1, s2, 16-bit ones to the low and high halves
        assert_eq!(pk("pcku8f32 r10, r1.0, r2.0, nearest", 300.0, -5.0), Ok(0x00ff00ff));
        assert_eq!(pk("pcko8f32 r10, r1.0, r2.0, nearest", 300.0, -5.0), Ok(0x7bff7bff));
        assert_eq!(pk("pcks16f32 r10, r1.0, r2.0, nearest", -300.0, 5.0), Ok(0x0005fed4));
        // pack-round-*: nearest even, down, up, zero
        assert_eq!(pk("pcks8f32 r10, r1.0, r2.0, nearest", 2.5, -3.5), Ok(0xfc02fc02));
        assert_eq!(pk("pcks8f32 r10, r1.0, r2.0, down", 2.5, -2.5), Ok(0xfd02fd02));
        assert_eq!(pk("pcks8f32 r10, r1.0, r2.0, up", 2.4, -2.6), Ok(0xfe03fe03));
        assert_eq!(pk("pcks8f32 r10, r1.0, r2.0, zero", 3.5, -3.5), Ok(0xfd03fd03));
        // pack-*-scale: x * K rounded once to nearest whatever the mode; s8
        // saturates at -127
        assert_eq!(pk("pcku8f32.scale r10, r1.0, r2.0, down", 0.5, 0.3), Ok(0x4d804d80));
        assert_eq!(pk("pcks8f32.scale r10, r1.0, r2.0, nearest", 0.75, -1.5), Ok(0x815f815f));
        assert_eq!(pk("pcku16f32.scale r10, r1.0, r2.0, nearest", 0.3, -1.0), Ok(0x00004ccd));
        // a tie the measured cases cannot settle (half even or half up)
        assert!(pk("pcks8f32.scale r10, r1.0, r2.0, nearest", -0.5, 0.0).is_err());
        // pack-u8-bytemask / pack-u16-bytemask
        assert_eq!(pk("pcku8f32 r10.bytemask1101, r1.0, r2.0, nearest", 1.0, 2.0), Ok(0x0202a201));
        assert_eq!(pk("pcku8f32 r10.bytemask0111, r1.0, r2.0, nearest", 1.0, 2.0), Ok(0xa0010201));
        assert_eq!(pk("pcku16f32 r10.bytemask1110, r1.0, r2.0, nearest", 258.0, 772.0), Ok(0x010201a3));
        assert_eq!(pk("pcku16f32 r10.bytemask1001, r1.0, r2.0, nearest", 258.0, 772.0), Ok(0x03a1a202));
    }

    const POW2: [&str; 8] = [
        "mov r1, #0x3f800000", // 1
        "mov r2, #0x40000000", // 2
        "mov r3, #0x40800000", // 4
        "mov r4, #0x41000000", // 8
        "mov r5, #0x41800000", // 16
        "mov r6, #0x42000000", // 32
        "mov r7, #0x42800000", // 64
        "mov r8, #0x43000000", // 128
    ];

    #[test]
    fn the_double_dot_products_write_d_and_an_internal() {
        // hardware (fddp): s0 . s1 into d + N - 1, s0 . s2 into iN
        let (m, r) = run_after(&POW2, &["fddp.repeat2 r10, i1, r5, r1, r3"]);
        assert_eq!((r, m.r[11], m.i[1]), (Ok(()), 80f32.to_bits(), 320f32.to_bits()));
        // fddpc: its sources do not advance
        let (m, r) = run_after(&POW2, &["fddpc.repeat2 r10, i0, cp1, cp2, r1, r3, r5"]);
        assert_eq!((r, m.r[11], m.i[0]), (Ok(()), 8f32.to_bits(), 32f32.to_bits()));
        // fdpc (fdpc, fdpc-clip): fdp's value; p0-p3 and i0-i2 untouched
        let (m, r) = run_after(&POW2, &["fdpc.repeat3 r10, cp3, r1, r5"]);
        assert_eq!((r, m.r[12], m.written.i), (Ok(()), (16f32 + 64.0 + 256.0).to_bits(), 0));
        // their rounding is not measured
        let (_, r) = run_after(&ABC, &["mov r7, #0x3eaaaaab", "fdpc r10, cp0, r7, r7"]);
        assert!(matches!(r, Err(Error::Unsupported { why, .. }) if why.contains("rounding")));
    }

    #[test]
    fn sop2_blends_each_byte() {
        let blend = |a: u32, b: u32, ops: &str| {
            let (m, r) = run_after(
                &[&format!("mov r1, #0x{a:08x}"), &format!("mov r2, #0x{b:08x}")],
                &[&format!("sop2 r10, r1, r2, {ops}")],
            );
            r.map(|_| m.r[10])
        };
        // hardware (sop2-selectors): a product of bytes over 255, rounded to
        // nearest; alpha from the alpha selectors
        assert_eq!(blend(0x864d3896, 0xc0e628e4, "s1, zero, add+asop2 s1a.comp, zero, add"), Ok(0x40170c58));
        // add rounds the sum of both products once: 64.25 + 63.25 is 128
        assert_eq!(blend(0x80808080, 0x7f7f7f7f, "s1, s2, add+asop2 s1a, s2a, add"), Ok(0x80808080));
        // min and max compare the sources themselves
        assert_eq!(
            blend(0xdd867ff1, 0x4e0bf742, "s1.comp, s1.comp, min+asop2 zero, zero, add"),
            Ok(0x000b7f42)
        );
        // asat is min(s1a, 1 - s2a), and .comp min(1 - s1a, s2a)
        assert_eq!(
            blend(0x01ff00fe, 0xff00ff01, "asat.comp, zero, add+asop2 zero, zero, add"),
            Ok(0x00fe00fd)
        );
        assert_eq!(blend(0xffffffff, 0x80808080, "asat, zero, add+asop2 zero, zero, add"), Ok(0x007f7f7f));
        // sop2-ops: s2scale doubles the result; zeros reads s2 as s2 - 128
        assert_eq!(blend(0x7f7f7f7f, 0x01ff00fe, "s2scale, zero, add+asop2 zero, zero, add"), Ok(0x00fe00fd));
        assert_eq!(
            blend(0x0000560a, 0x00004f21, "zero.comp, zeros.comp, sub+asop2 zero, zero, add"),
            Ok(0x00808769)
        );
        // the leading comp complements s1 for add and sub; alpha neg negates
        assert_eq!(
            blend(0x00000040, 0, "comp, zero.comp, zero, add+asop2 zero.comp, zero, add, neg"),
            Ok(0x00ffffbf)
        );
        // c10 operands are not modelled yet
        assert!(blend(0, 0, "zero, zero, add+asop2 zero, zero, add").is_ok());
        let (_, r) = run_after(
            &["mov r1, #0x00000000"],
            &["sop2 r10, r1.c10, r1, zero, zero, add+asop2 zero, zero, add"],
        );
        assert!(r.is_err());
    }

    #[test]
    fn sop2wm_blends_with_a_byte_mask() {
        let blend = |a: u32, b: u32, ops: &str| {
            let (m, r) = run_after(
                &[&format!("mov r1, #0x{a:08x}"), &format!("mov r2, #0x{b:08x}"), "mov r10, #0xa0a1a2a3"],
                &[&format!("sop2wm r10{ops}")],
            );
            r.map(|_| m.r[10])
        };
        // hardware (sop2wm): sop2's blend with one selector pair, the colour
        // operation on bytes 0-2, the alpha one on byte 3, masked bytes kept
        assert_eq!(
            blend(0x00000040, 0x00000020, ".bytemask0001, r1, r2, zero.comp, zero.comp, add, sub"),
            Ok(0xa0a1a260)
        );
        // on the alpha byte asat is 1, with or without .comp
        assert_eq!(
            blend(0xffffffff, 0x80808080, ".bytemask1111, r1, r2, asat.comp, s2, sub, sub"),
            Ok(0xbf000000)
        );
    }

    #[test]
    fn sop3_and_the_lerps_blend_three_sources() {
        let blend = |s: [u32; 3], text: &str| {
            let (m, r) = run_after(
                &[
                    &format!("mov r1, #0x{:08x}", s[0]),
                    &format!("mov r2, #0x{:08x}", s[1]),
                    &format!("mov r3, #0x{:08x}", s[2]),
                ],
                &[text],
            );
            r.map(|_| m.r[10])
        };
        let s = [0x80ff8040, 0xff80ff80, 0];
        // sop3 (sop3-lrp): sop2's blend of s1 and s2, with s0 and s0a as factors
        assert_eq!(blend(s, "sop3 r10, r1, r2, r3, s0, zero, add+asop s0a, zero, add"), Ok(0x80808020));
        let s = [0x7f7f7f7f, 0x01ff00fe, 0xff00ff01];
        // arsop: sub is s2a * g - s1a * (the second colour factor); add is 0
        assert_eq!(
            blend(s, "sop3 r10, r1, r2, r3, zero, zero.comp, add+arsop zero.comp, sub"),
            Ok(0xfe00ff01)
        );
        assert_eq!(blend(s, "sop3 r10, r1, r2, r3, zero, zero.comp, add+arsop s1a, add"), Ok(0x0000ff01));
        // alrp: g1 * (1 - s0a) + g2 * s0a
        let s = [0x831d45d0, 0xeec09b5f, 0x62508640];
        assert_eq!(
            blend(s, "sop3 r10, r1, r2, r3, s2.comp, s0.comp, sub+alrp s1a.comp, s2a.comp"),
            Ok(0x593d003b)
        );
        // lrp1: f1 * (1 - w) + f2 * w by s0 (or s0a); lrp2: x1 * f1 + x2 * f2
        let s = [0x80808080, 0x7f7f7f7f, 0x01ff00fe];
        assert_eq!(
            blend(s, "lrp1 r10, r1, r2, r3, zero.comp, zero, s0+asop s1.comp, s0a, add"),
            Ok(0x407f7f7f)
        );
        assert_eq!(blend(s, "lrp2 r10, r1, r2, r3, s0, s0a.comp, s1, s2+alrp s1a, s2a.comp"), Ok(0xbfbf40be));
        // the colour neg of the alrp form is not understood yet
        assert!(blend(s, "sop3 r10, r1, r2, r3, s1, s2a.comp, sub, neg+alrp zero, zero").is_err());
    }

    #[test]
    fn movc_selects_per_lane_and_movmsk_by_a_condition() {
        let run3 = |s: [u32; 3], text: &str| {
            let (m, r) = run_after(
                &[
                    &format!("mov r1, #0x{:08x}", s[0]),
                    &format!("mov r2, #0x{:08x}", s[1]),
                    &format!("mov r3, #0x{:08x}", s[2]),
                    "mov r10, #0xa0a1a2a3",
                ],
                &[text],
            );
            r.map(|_| m.r[10])
        };
        let (a, b) = (0x62222222, 0xb3333333);
        // hardware (movc-*): s1 where the lane of s0 passes, else s2; i10
        // lanes are bits 0-9, 10-19, 20-29 and bits 30-31 come out 0
        assert_eq!(run3([0x8000, a, b], "movc.i16.tn r10, r1, r2, r3"), Ok(0xb3332222));
        assert_eq!(run3([0x8000, a, b], "movc.i32.tp|z r10, r1, r2, r3"), Ok(a));
        assert_eq!(run3([0x200, a, b], "movc.i10.tz r10, r1, r2, r3"), Ok(0x22222333));
        // floats: a denormal tests as zero
        assert_eq!(run3([1, a, b], "movc.flt.tz r10, r1, r2, r3"), Ok(a));
        // movmsk-*: s1 into d when bit 0 (mask) or bit 7 (u8) of byte c of s2
        // is set, or s2 >= 0.5 (flt); else d stays
        let v = 0x11223344;
        assert_eq!(run3([0x01ff8000, v, 0], "movmsk.u8 r10, r2, r1.1"), Ok(v));
        assert_eq!(run3([0x01ff8000, v, 0], "movmsk.mask r10, r2, r1.1"), Ok(0xa0a1a2a3));
        assert_eq!(run3([0x3f000000, v, 0], "movmsk.flt r10, r2, r1"), Ok(v));
        assert_eq!(run3([0x3effffff, v, 0], "movmsk.flt r10, r2, r1"), Ok(0xa0a1a2a3));
        assert_eq!(run3([0x3c000000, v, 0], "movmsk.flt16 r10, r2, r1.2"), Ok(v));
        assert!(run3([0x3c000000, v, 0], "movmsk.flt16 r10, r2, r1.1").is_err());
        // movmsk-smsk: u8 and flt also need bit 0 of s0; mask ignores s0
        assert_eq!(run3([2, v, 0x8000], "movmsk.smsk.u8 r10, r1, r2, r3.1"), Ok(0xa0a1a2a3));
        assert_eq!(run3([3, v, 0x8000], "movmsk.smsk.u8 r10, r1, r2, r3.1"), Ok(v));
        assert_eq!(run3([2, v, 0x100], "movmsk.mask.smsk r10, r1, r2, r3.1"), Ok(v));
    }

    const SMLSI: &str = "incrementmode, incrementmode, incrementmode, incrementmode, #0, #0, #0";

    #[test]
    fn smlsi_sets_the_destination_and_source_increments() {
        // hardware (repeat-smlsi): increment 0 is the destination's, 2 the
        // source 1's (mov's source)
        let (m, r) = run_after(&R1_R7, &[&format!("smlsi #2, #1, #1, #1, {SMLSI}"), "mov.repeat2 r1, r5"]);
        assert_eq!((r, &m.r[1..4]), (Ok(()), &[0x55, 0x22, 0x66][..]));
        let (m, r) = run_after(&R1_R7, &[&format!("smlsi #1, #1, #0, #1, {SMLSI}"), "mov.repeat2 r1, r5"]);
        assert_eq!((r, &m.r[1..4]), (Ok(()), &[0x55, 0x55, 0x33][..]));
        // the source 0 and 2 increments are not measured
        let (_, r) =
            run_after(&ABC, &[&format!("smlsi #1, #2, #1, #1, {SMLSI}"), "fmad.repeat2 r10, r1, r3, r5"]);
        assert!(matches!(r, Err(Error::Unsupported { why, .. }) if why.contains("increment")));
    }
}

/// The float datapath, as the hardware vectors show it: f32 operations
/// rounded to nearest even (Rust's f32 add and mul are correctly rounded),
/// denormal inputs and results flushed to a zero of the same sign. Infinities
/// and NaNs are not modelled yet.
mod flt {
    use super::{Error, why};

    pub fn modify(v: u32, neg: bool, abs: bool) -> u32 {
        let v = if abs { v & 0x7fff_ffff } else { v };
        if neg { v ^ 0x8000_0000 } else { v }
    }

    fn input(v: u32) -> Result<f32, Error> {
        if v >> 23 & 0xff == 0xff {
            return Err(why("an infinity or NaN operand"));
        }
        // a denormal flushes to a zero of its sign
        Ok(f32::from_bits(if v >> 23 & 0xff == 0 { v & 0x8000_0000 } else { v }))
    }

    fn output(x: f32) -> Result<u32, Error> {
        if !x.is_finite() {
            return Err(why("an infinite or NaN result"));
        }
        let b = x.to_bits();
        Ok(if b >> 23 & 0xff == 0 { b & 0x8000_0000 } else { b })
    }

    pub fn mul(a: u32, b: u32) -> Result<u32, Error> {
        output(input(a)? * input(b)?)
    }

    pub fn add(a: Result<u32, Error>, b: u32) -> Result<u32, Error> {
        output(input(a?)? + input(b)?)
    }

    pub fn sub(a: u32, b: Result<u32, Error>) -> Result<u32, Error> {
        output(input(a)? - input(b?)?)
    }

    /// An operand of min/max: a NaN is kept (and ignored by the comparison).
    fn operand(v: u32) -> Result<f32, Error> {
        if v >> 23 & 0xff == 0xff && v & 0x7f_ffff != 0 { Ok(f32::NAN) } else { input(v) }
    }

    pub fn min(a: u32, b: u32) -> Result<u32, Error> {
        output(operand(a)?.min(operand(b)?))
    }

    pub fn max(a: u32, b: u32) -> Result<u32, Error> {
        output(operand(a)?.max(operand(b)?))
    }

    /// Whether an f32 (or f16) value is at least 0.5; a NaN is not.
    pub fn at_least_half(b: u32, half: bool) -> Result<bool, Error> {
        let x = if half {
            let (e, m) = (b >> 10 & 31, b & 1023);
            let mag = match e {
                31 if m != 0 => return Ok(false),
                31 => f64::INFINITY,
                0 => m as f64 * 2f64.powi(-24),
                _ => (1024 + m) as f64 * 2f64.powi(e as i32 - 25),
            };
            if b >> 15 != 0 { -mag } else { mag }
        } else {
            f32::from_bits(b) as f64
        };
        Ok(x >= 0.5)
    }

    pub fn floor(a: u32) -> Result<u32, Error> {
        output(input(a)?.floor())
    }
}
