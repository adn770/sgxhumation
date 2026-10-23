// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The USSE interpreter.

use crate::func::{self, Bound};
use crate::tex;
use std::cell::Cell;
use std::collections::BTreeMap;
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

    /// smp: the registers a texture sample writes (see tex).
    fn sample(&mut self, _s: &tex::Sample) -> Result<Vec<u32>, Error> {
        Err(why("no texture unit on this bus"))
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

    /// The texture unit's model over this memory.
    fn sample(&mut self, s: &tex::Sample) -> Result<Vec<u32>, Error> {
        tex::sample(s, &mut |a| self.load32(a)).map_err(|f| match f {
            tex::Fail::Open(w) => why(w),
            tex::Fail::Load(e) => e,
        })
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
#[derive(Clone)]
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
    /// The index registers i.l and i.h.
    pub index: [u32; 2],
    /// Set while the machine runs as one instance of a 2x2 quad (run_quad):
    /// its position and the four instances' i0-i2 before the instruction.
    pub quad: Option<QuadView>,
    /// efo.gpiext's write of i1, which run_quad delivers to the top-right i0.
    pub quad_i1: Option<u32>,
    /// The temporaries allocated to the task: those past it are other
    /// instances' and stop the program.
    pub temps: usize,
    /// Whether the float functions the hardware gives only to a bound (see
    /// func) give their nearest value; without it they stop the program.
    pub near: bool,
    /// The registers holding a near value (bank, number), with its bound:
    /// a move keeps it, anything else computed from one is Bound::Derived.
    pub bounds: BTreeMap<(&'static str, usize), Bound>,
    /// The addresses a near value was stored at.
    pub near_mem: BTreeMap<u32, Bound>,
    /// The current operation, for bounds.
    pub now: Now,
}

/// What the current operation read and writes, for Machine::bounds.
#[derive(Clone, Default)]
pub struct Now {
    /// The bound of what it read (Derived if two).
    read: Cell<Option<Bound>>,
    out: Out,
}

#[derive(Clone, Copy, Default)]
enum Out {
    #[default]
    /// A value computed from what it read.
    Reads,
    /// What it read, unchanged.
    Move,
    /// A float function's result, with its bound if near.
    Fn(Option<Bound>),
}

/// What an instance of a quad sees of the others (see Machine::quad).
#[derive(Clone, Debug, Default)]
pub struct QuadView {
    /// TL, TR, BL, BR = 0-3
    pub pos: usize,
    pub i: [[u32; 3]; 4],
    pub written: [u8; 4],
}

/// Run a 2x2 quad of instances (TL, TR, BL, BR) of one program in lockstep:
/// what i3, the bank-7 numbers past it and fdsx/fdsy read of the others.
/// Instances that would take different paths stop the quad.
pub fn run_quad(
    quad: &mut [Machine; 4],
    prog: &Program,
    bus: &mut dyn Bus,
    max_steps: usize,
) -> Result<(), Error> {
    loop {
        let pc = quad[0].pc;
        if quad.iter().any(|m| m.pc != pc) {
            return Err(unsupported(pc, "", "the quad's instances diverged"));
        }
        if quad[0].steps >= max_steps {
            return Err(Error::StepLimit);
        }
        let insn = match prog.insns.get(pc) {
            None => return Err(Error::PcOutOfRange(pc)),
            Some(Err(text)) => return Err(unsupported(pc, text, "undecoded")),
            Some(Ok(i)) => i,
        };
        let view = QuadView {
            pos: 0,
            i: [0, 1, 2, 3].map(|k| [quad[k].i[0], quad[k].i[1], quad[k].i[2]]),
            written: [0, 1, 2, 3].map(|k| quad[k].written.i & 7),
        };
        for (k, m) in quad.iter_mut().enumerate() {
            m.steps += 1;
            m.quad = Some(QuadView { pos: k, ..view.clone() });
        }
        let at = |e: Error| match e {
            Error::Unsupported { why, .. } => unsupported(pc, &insn.text, &why),
            e => e,
        };
        let mut nexts = Vec::with_capacity(4);
        if insn.form.op == "fds" {
            // hardware (fds-quad): fdsx = a(TR) - b(TL), fdsy = a(BL) - b(TL),
            // the same for all four
            let mut ab = Vec::with_capacity(4);
            for m in quad.iter() {
                ab.push(m.fds_sources(insn).map_err(at)?);
            }
            let y = insn.name("op", quad[0].core).as_deref() == Some("fdsy");
            let v = flt::sub(ab[if y { 2 } else { 1 }].0, Ok(ab[0].1)).map_err(at)?;
            for m in quad.iter_mut() {
                m.write(insn, v).map_err(at)?;
                nexts.push(if m.ends(insn) { Next::End } else { Next::Step });
            }
        } else {
            for m in quad.iter_mut() {
                nexts.push(m.exec(insn, bus).map_err(at)?);
            }
        }
        for m in quad.iter_mut() {
            m.quad = None;
        }
        // efo.gpiext's i1 writes: the top-right i0 holds the bottom-right's
        // (efo-gpiext-quad; whether the others land first is not settled)
        let i1: Vec<Option<u32>> = quad.iter_mut().map(|m| m.quad_i1.take()).collect();
        if i1.iter().any(|w| w.is_some()) {
            let Some(v) = i1[3] else {
                return Err(unsupported(pc, &insn.text, "efo.gpiext i1 writes without the bottom-right's"));
            };
            quad[1].i[0] = v;
            quad[1].written.i |= 1;
        }
        let next = &nexts[0];
        if nexts.iter().any(|n| n != next) {
            return Err(unsupported(pc, &insn.text, "the quad's instances diverged"));
        }
        match *next {
            Next::Step => quad.iter_mut().for_each(|m| m.pc += 1),
            Next::Jump(t) => quad.iter_mut().for_each(|m| m.pc = t),
            Next::End => return Ok(()),
        }
    }
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
    pub index: u8,
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
            index: [0; 2],
            quad: None,
            quad_i1: None,
            temps: 124,
            near: false,
            bounds: BTreeMap::new(),
            near_mem: BTreeMap::new(),
            now: Now::default(),
        }
    }
}

/// What an instruction asks the loop to do next.
#[derive(PartialEq, Eq)]
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
            if self.step(prog, bus)? {
                return Ok(());
            }
        }
    }

    /// Run the instruction at `pc`: true when it ends the program.
    pub fn step(&mut self, prog: &Program, bus: &mut dyn Bus) -> Result<bool, Error> {
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
            Next::End => return Ok(true),
        }
        Ok(false)
    }

    /// The text of the instruction at `pc` (for traces).
    pub fn text(prog: &Program, pc: usize) -> Option<&str> {
        prog.insns.get(pc).map(|i| match i {
            Ok(i) => i.text.as_str(),
            Err(t) => t.as_str(),
        })
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
            // hardware (predicate-pn): never runs
            "Pn" => false,
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
                    // fpma.sat: its 8-bit results saturate anyway (fpma-sat)
                    // hardware (flags-other): .tpres and .partial change nothing
                    // a pixel sees
                    if !(matches!(t, "skipinv" | "syncs" | "nosched" | "end" | "tpres" | "partial")
                        || op == "fpma" && t == "sat"
                        || COLOUR_REPEATS.contains(&op) && t.starts_with("repeat")
                        || op == "efo" && t == "gpiext"
                        || op == "smp" && matches!(t, "pcf" | "pcff16"))
                    {
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
            self.now = Now::default();
            self.dot(its)?;
        } else {
            for it in its {
                self.now = Now::default();
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
        let op = insn.form.op;
        // The colour and dot products (and adif) carry the repeat in their
        // flags: the multiply flags' bits 3 and up, fpma's bits 3-5
        // (hardware, colour-dot-repeats, video-adif: every operand advances by
        // one, the colour ones in their six-bit numbering).
        if let Some(fi) = insn.form.role("flags")
            && COLOUR_REPEATS.contains(&op)
        {
            let v = insn.v[fi];
            let (n, plain) =
                if op == "fpma" { ((v >> 3 & 7) + 1, v & !(7 << 3)) } else { ((v >> 3) + 1, v & 7) };
            if n > 1 {
                let comps: Vec<i32> = (0..n as i32).collect();
                let bits = if op.starts_with("adif") { 7 } else { 6 };
                return self.steps_in(insn, fi, plain, &comps, 1, false, bits).map(Some);
            }
            return Ok(None);
        }
        // A write mask other than .x (hardware, wmask-mov, wmask-or,
        // wmask-and-imm, wmask-other, wmask-frcp): component k writes d + k
        // from sources advanced by k, as iteration k of a repeat; immediates
        // stay.
        if let Some(mi) = insn.form.role("mask").filter(|&i| insn.v[i] != 1) {
            if !matches!(op, "mov" | "bitwise" | "movc" | "pack" | "fsc") {
                let m = insn.name("mask", self.core).unwrap();
                return Err(why(format!("write mask {m} on {op}")));
            }
            let comps: Vec<i32> = (0..4).filter(|k| insn.v[mi] >> k & 1 != 0).collect();
            return self.steps(insn, mi, 1, &comps, 1, true).map(Some);
        }
        let Some(rep) = ["repeat", "rep", "rep16"].into_iter().find_map(|r| insn.form.role(r)) else {
            return Ok(None);
        };
        // fmad16's field is repeat2[0] | incs[3:1], always with the .incs bits
        let rep16 = insn.form.role("rep16") == Some(rep);
        let n = if rep16 { 1 + (insn.v[rep] & 1) } else { insn.v[rep] + 1 };
        if n == 1 {
            return Ok(None);
        }
        if !matches!(
            op,
            "mov" | "bitwise" | "fmad" | "fsubflr" | "fminmax" | "fdp" | "fdpc" | "fddp" | "fddpc" | "fmad16"
        ) {
            return Err(why(format!("{op} repeated")));
        }
        // the incs field: enc[53] set advances every source, else bits 3-1
        // advance sources 0-2; fddpc has none and its sources stay (fddpc)
        let incs = if rep16 {
            insn.v[rep] & !1
        } else {
            insn.get("incs").unwrap_or(if op == "fddpc" { 0 } else { 1 })
        };
        let comps: Vec<i32> = (0..n as i32).collect();
        self.steps(insn, rep, 0, &comps, incs, false).map(Some)
    }

    /// The iterations `comps` of a repeat or write mask, with the field `rep`
    /// set to `plain` in each and the register operands advanced (see
    /// iterations); `fixed_imm`: immediates stay instead of stopping.
    fn steps(
        &self,
        insn: &Insn,
        rep: usize,
        plain: u32,
        comps: &[i32],
        incs: u32,
        fixed_imm: bool,
    ) -> Result<Vec<Insn>, Error> {
        self.steps_in(insn, rep, plain, comps, incs, fixed_imm, 7)
    }

    /// steps, with register numbers of `bits` bits (6 for the colour
    /// operands: r0-r59, then i0-i3; 7 for the others).
    #[allow(clippy::too_many_arguments)]
    fn steps_in(
        &self,
        insn: &Insn,
        rep: usize,
        plain: u32,
        comps: &[i32],
        incs: u32,
        fixed_imm: bool,
        bits: u32,
    ) -> Result<Vec<Insn>, Error> {
        let mask = (1u32 << bits) - 1;
        let advances = |k: u32| incs & 1 != 0 || incs >> (4 - k) & 1 != 0;
        let mut step = [("dst", 1), ("src0", 1), ("src1", 1), ("src2", 1)];
        if let Some(inc) = self.moe_inc {
            // hardware (repeat-smlsi, smlsi-three-sources): increments 0-3 are
            // the destination's and sources 0-2's
            (step[0].1, step[1].1, step[2].1, step[3].1) = (inc[0], inc[1], inc[2], inc[3]);
        }
        let mut its = Vec::with_capacity(comps.len());
        for &k in comps {
            let mut it = insn.clone();
            it.v[rep] = plain;
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
                    if fixed_imm {
                        continue;
                    }
                    return Err(why(format!("{role} immediate repeated")));
                }
                let r = (v & mask) as i32 + k * inc;
                let top = match bits {
                    6 if bank == 0 && v & 63 < 60 => 60,
                    6 => 64,
                    _ if bank == 0 && v & 0x7f < 124 => 124,
                    _ => 128,
                };
                if !(0..top).contains(&r) {
                    return Err(why(format!("{role} repeated past its bank")));
                }
                it.v[i] = v & !mask | r as u32;
            }
            its.push(it);
        }
        Ok(its)
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
        let (mut scale, mut renable) = (false, false);
        for f in parts {
            match f {
                "skipinv" | "syncs" | "nosched" | "end" => {}
                "scale" => scale = true,
                "renable" => renable = true,
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
            if renable {
                return Err(why(format!("{fmt}.renable")));
            }
            return self.unpack_f32(insn, fmt, from, scale);
        }
        if fmt == "pckf16f32" {
            return self.pack_f16(insn, renable, &round);
        }
        if fmt == "pckc10f32" {
            return self.pack_c10(insn, &round);
        }
        if let Some(r) = self.unpack_int(insn, fmt, scale)? {
            return self.write(insn, r);
        }
        if fmt == "statomic" {
            // hardware (statomic): bytes s1.c, s2.c, s1.c, s2.c, each read as
            // signed and clamped at 0, whatever the mode and .scale
            if insn.get("bytemask") != Some(15) {
                return Err(why("statomic with a byte mask"));
            }
            let byte = |m: &Self, role: &str| -> Result<u32, Error> {
                let v = insn.get(role).unwrap();
                let x = m.src(v & 0x3ff, 6)? >> (8 * (v >> 10 & 3)) & 0xff;
                Ok(if x < 128 { x } else { 0 })
            };
            let (x, y) = (byte(self, "src1")?, byte(self, "src2")?);
            return self.write(insn, x | y << 8 | x << 16 | y << 24);
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
        if from == "c10" {
            // hardware (unpack-c10): lane c / 255 (.3 is 0), lanes clamped to
            // +/-510
            let lane = if sel == 3 { 0 } else { chans(w, true)[sel as usize] };
            return self.write(insn, ((lane as f64 / 255.0) as f32).to_bits());
        }
        if from == "f16" {
            // hardware (unpack-f16): exact, infinities kept, a NaN 0x7fffffff;
            // (unpack-select) .3 of a 16-bit format reads nothing
            if sel == 3 {
                return self.write(insn, 0);
            }
            let h = w >> (8 * sel) & 0xffff;
            let (sign, e, f) = (h >> 15, h >> 10 & 31, h & 1023);
            let v = match (e, f) {
                (31, 0) => sign << 31 | 0x7f80_0000,
                (31, _) => 0x7fff_ffff,
                _ => {
                    let mag = if e == 0 {
                        f as f32 * 2f32.powi(-24)
                    } else {
                        (1024 + f) as f32 * 2f32.powi(e as i32 - 25)
                    };
                    (if sign != 0 { -mag } else { mag }).to_bits()
                }
            };
            return self.write(insn, v);
        }
        let x = match from {
            "u8" | "s8" | "o8" => w >> (8 * sel) & 0xff,
            "u16" | "s16" if sel < 3 => w >> (8 * sel) & 0xffff,
            // hardware (unpack-select): .3 of a 16-bit format reads nothing
            "u16" | "s16" => 0,
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

    /// pckf16f32: s1 to the low half, s2 to the high one. Hardware
    /// (pack-f16): without .renable truncated toward zero whatever the mode
    /// (nearest and up measured), saturated at 65504, half denormals kept;
    /// .renable first rounds to an integer by the mode.
    fn pack_f16(&mut self, insn: &Insn, renable: bool, round: &str) -> Result<(), Error> {
        if !renable && round != "nearest" && round != "up" {
            return Err(why(format!("pckf16f32 rounding {round} (not measured)")));
        }
        let conv = |role: &str| -> Result<u32, Error> {
            let v = insn.get(role).unwrap();
            if v >> 10 != 0 {
                return Err(why("pckf16f32 of a component select"));
            }
            let b = self.src(v & 0x3ff, 6)?;
            if b >> 23 & 0xff == 0xff || (b >> 23 & 0xff == 0 && b & 0x7f_ffff != 0) {
                return Err(why("pckf16f32 of an infinity, NaN or denormal"));
            }
            let mut x = f32::from_bits(b) as f64;
            if renable {
                x = match round {
                    "nearest" => x.round_ties_even(),
                    "down" => x.floor(),
                    "up" => x.ceil(),
                    _ => x.trunc(),
                };
            }
            let (sign, a) = ((b >> 31) << 15, x.abs());
            if a >= 65504.0 {
                return Ok(sign | 0x7bff);
            }
            if a < 2f64.powi(-14) {
                return Ok(sign | (a * 2f64.powi(24)).floor() as u32);
            }
            let e = a.log2().floor() as i32;
            let e = if 2f64.powi(e) > a {
                e - 1
            } else if 2f64.powi(e + 1) <= a {
                e + 1
            } else {
                e
            };
            let n = (a * 2f64.powi(10 - e)).floor() as u32;
            if renable && n as f64 != a * 2f64.powi(10 - e) {
                return Err(why("pckf16f32.renable of an integer a half cannot hold"));
            }
            Ok(sign | ((e + 15) as u32) << 10 | (n - 1024))
        };
        let (lo, hi) = (conv("src1")?, conv("src2")?);
        self.write(insn, lo | hi << 16)
    }

    /// pckc10f32 (hardware, pack-c10): x * 255 to nearest even, clamped to
    /// +/-510, in 10-bit lanes taking s1, s2, s1; bits 30-31 are 0.
    fn pack_c10(&mut self, insn: &Insn, round: &str) -> Result<(), Error> {
        if round != "nearest" {
            return Err(why(format!("pckc10f32 rounding {round} (not measured)")));
        }
        let lane = |role: &str| -> Result<u32, Error> {
            let v = insn.get(role).unwrap();
            let b = self.src(v & 0x3ff, 6)?;
            if b >> 23 & 0xff == 0xff || (b >> 23 & 0xff == 0 && b & 0x7f_ffff != 0) {
                return Err(why("pckc10f32 of an infinity, NaN or denormal"));
            }
            let x = (f32::from_bits(b) as f64 * 255.0).round_ties_even().clamp(-510.0, 510.0);
            Ok(x as i32 as u32 & 0x3ff)
        };
        let (a, b) = (lane("src1")?, lane("src2")?);
        self.write(insn, a | b << 10 | a << 20)
    }

    /// The unpacks that convert no float (hardware, unpack-u8s8, unpack-s16u8,
    /// unpack-u16s8, unpack-s16s8, unpack-u16u8, unpack-c10-16), or None for
    /// another format: unpcks16s8 sign-extends, unpcks16u8 and unpcku16u8
    /// zero-extend, unpcku16s8 makes a negative 0 and unpcku8s8 0x7f, laid
    /// out as packs (bytes s1, s2, s1, s2 or halves s1, s2); unpcku16c10 and
    /// unpcks16c10 copy bits 4, 6 and 8 of lane 0 of source 1 into bits 15,
    /// 1-14 and 0, whatever the selects.
    fn unpack_int(&self, insn: &Insn, fmt: &str, scale: bool) -> Result<Option<u32>, Error> {
        if !matches!(
            fmt,
            "unpcku8s8"
                | "unpcks16u8"
                | "unpcku16u8"
                | "unpcks16s8"
                | "unpcku16s8"
                | "unpcku16c10"
                | "unpcks16c10"
        ) {
            return Ok(None);
        }
        if insn.get("bytemask") != Some(15) {
            return Err(why(format!("{fmt} with a byte mask")));
        }
        let byte = |role: &str| -> Result<u32, Error> {
            let v = insn.get(role).unwrap();
            Ok(self.src(v & 0x3ff, 6)? >> (8 * (v >> 10 & 3)) & 0xff)
        };
        if fmt.ends_with("c10") {
            let w = self.src(insn.get("src1").unwrap() & 0x3ff, 6)?;
            let bit = |n: u32| w >> n & 1;
            return Ok(Some(bit(4) << 15 | if bit(6) != 0 { 0x7ffe } else { 0 } | bit(8)));
        }
        if scale {
            return Err(why(format!("{fmt}.scale (not measured)")));
        }
        let conv = |x: u32| -> u32 {
            let neg = x >= 128;
            match fmt {
                "unpcku8s8" if neg => 0x7f,
                "unpcku16s8" if neg => 0,
                "unpcks16s8" if neg => (x | 0xff00) & 0xffff,
                _ => x,
            }
        };
        let (a, b) = (conv(byte("src1")?), conv(byte("src2")?));
        Ok(Some(if fmt == "unpcku8s8" { a | b << 8 | a << 16 | b << 24 } else { a | b << 16 }))
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
                // x * k is exact in f64, rounded to nearest even
                // (pack-conversions: 96 random values agree)
                (x * k).round_ties_even()
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
    /// usse_isa's C10 kind) as an ordinary one, number[6:0] | bank[9:7], and
    /// its .c10 bit. Indexed operands and immediates stop.
    fn colour_operand(v: u32, dst: bool) -> Result<(u32, bool), Error> {
        let (bank, r) = (v >> 7 & 7, v & 63);
        if v >> 10 != 0 || bank == 5 || bank == 6 || bank == if dst { 3 } else { 4 } {
            return Err(why("colour operand (indexed, immediate or modified)"));
        }
        let reg = match bank {
            0 if r >= 60 => 7 << 7 | (r - 60),
            b => b << 7 | r,
        };
        Ok((reg, v >> 6 & 1 != 0))
    }

    /// A colour source's channels (see chans).
    fn colour_src(&self, v: u32) -> Result<Chans, Error> {
        let (reg, c10) = Self::colour_operand(v, false)?;
        Ok(chans(self.src(reg, 6)?, c10))
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
        let s1 = self.colour_src(insn.get("src1").unwrap())?;
        let s2 = self.colour_src(insn.get("src2").unwrap())?;
        let (d, dc10) = Self::colour_operand(insn.get("dst").unwrap(), true)?;
        let (csel, asel, mask) = match insn.name("sel", self.core) {
            None => (insn.name("csel", self.core).unwrap(), insn.name("asel", self.core).unwrap(), 15),
            Some(sel) => {
                // "f1, f2, colour op, alpha op"
                let (pair, aop) = sel.rsplit_once(", ").unwrap();
                let (f12, cop) = pair.rsplit_once(", ").unwrap();
                (format!("{f12}, {cop}"), format!("{f12}, {aop}"), insn.get("bytemask").unwrap())
            }
        };
        let mut v = [0; 4];
        for ch in 0..4 {
            v[ch as usize] = blend(if ch == 3 { &asel } else { &csel }, s1, s2, ch)?;
        }
        let mut w = colour_out(v, dc10);
        if mask != 15 {
            if d >> 7 != 0 || d & 0x7f >= 124 || dc10 {
                return Err(why("sop2wm byte mask on a non-temporary"));
            }
            let keep = (0..4).filter(|&i| mask >> i & 1 == 0).fold(0u32, |k, i| k | 0xff << (8 * i));
            w = self.temp(d as usize)? & keep | w & !keep;
        }
        self.write_dst(d, w)
    }

    /// A colour source 0 (six-bit registers, .c10 bit 6, pa bit 7, a
    /// modifier bit 8) as an ordinary source 0, number[6:0] | pa[7], and its
    /// .c10 bit.
    fn colour_operand0(v: u32) -> Result<(u32, bool), Error> {
        let (pa, r) = (v >> 7 & 1, v & 63);
        if v >> 8 != 0 {
            return Err(why("modified colour source 0"));
        }
        Ok((if pa == 0 && r >= 60 { 124 + r - 60 } else { pa << 7 | r }, v >> 6 & 1 != 0))
    }

    fn colour_src0(&self, v: u32) -> Result<Chans, Error> {
        let (reg, c10) = Self::colour_operand0(v)?;
        Ok(chans(self.src0(reg)?, c10))
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
            self.colour_src0(insn.get("src0").unwrap())?,
            self.colour_src(insn.get("src1").unwrap())?,
            self.colour_src(insn.get("src2").unwrap())?,
        ];
        let (d, dc10) = Self::colour_operand(insn.get("dst").unwrap(), true)?;
        if insn.form.op == "lrp2"
            && (dc10 || ["src0", "src1", "src2"].iter().any(|r| insn.get(r).unwrap() >> 6 & 1 != 0))
        {
            return Err(why("lrp2 on .c10 operands (not measured)"));
        }
        let csel = insn.name("csel", self.core).unwrap();
        let alpha =
            ["asop", "arsop", "alrp"].into_iter().find_map(|r| Some((r, insn.name(r, self.core)?))).unwrap();
        let v = blend3(insn.form.op, &csel, (alpha.0, &alpha.1), s)?;
        self.write_dst(d, colour_out(v, dc10))
    }

    /// movmsk d, s1, s2.c (and .smsk d, s0, s1, s2.c): s1 into d where
    /// component c of s2 passes, else d stays. Hardware (movmsk-mask, -u8,
    /// -flt, -flt16): .mask tests bit 0 of byte c, .u8 bit 7 of byte c, .flt
    /// s2 >= 0.5 (a NaN fails), .flt16 the half at bits 0-15, 8-23, 16-31
    /// or 16-31 for c = 0-3 >= 0.5; .smsk (movmsk-smsk, -smsk2) also needs bit 0 of s0
    /// for .u8 and .flt, while .mask ignores it.
    fn movmsk(&mut self, insn: &Insn) -> Result<(), Error> {
        let fmt = insn.name("fmt", self.core).unwrap_or(".u8".into());
        let c = insn.get("comp").unwrap();
        let s2 = self.src(insn.get("src2").unwrap(), 6)?;
        let mut pass = match fmt.as_str() {
            ".mask" => s2 >> (8 * c) & 1 != 0,
            ".u8" => s2 >> (8 * c + 7) & 1 != 0,
            ".flt" if c == 0 => flt::at_least_half(s2, false)?,
            // the half at bits 0-15, 8-23, 16-31, 16-31 (conditional-moves)
            ".flt16" => flt::at_least_half(s2 >> [0, 8, 16, 16][c as usize] & 0xffff, true)?,
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

    /// The integer dot products. Hardware (intdot-lanes, intdot-alpha):
    /// - a source byte is x, 255 - x with .comp, and with off x - 128; S is
    ///   the sum of the products of the first three (dot3) or four bytes;
    /// - u8: bytes 0-2 are round(S * Cx / 255), half up, saturated, and the
    ///   alpha byte round(S * Ax / 255); u8dot3's alpha operations take the
    ///   raw source alphas: aadd/asub round((f1 f2 +/- f3 f4) * Ax / 255),
    ///   aintrp1 lerps g1 to g2 by s0a, aintrp2 lerps 1 - g1 to g2;
    /// - u16: round(S * Cx * 32767 / 65025), saturated at +/-32767, in both
    ///   halves.
    fn idot(&mut self, insn: &Insn) -> Result<(), Error> {
        let op = insn.form.op;
        let byte = |w: u32, c: u32| (w >> (8 * c) & 0xff) as i64;
        let read = |m: &mut Self, role: &str| -> Result<(u32, bool), Error> {
            let v = insn.get(role).unwrap();
            let (reg, c10) = Self::colour_operand(v & 0x3ff, false)?;
            if c10 {
                return Err(why("integer dot product of a .c10 operand"));
            }
            Ok((m.src(reg, 6)?, v >> 10 & 1 != 0))
        };
        let ((s1, c1), (s2, c2)) = (read(self, "src1")?, read(self, "src2")?);
        let off = insn.name("off", self.core).as_deref() == Some("off");
        let lanes = |w: u32, comp: bool| {
            (0..4).map(move |c| {
                let x = if comp { 255 - byte(w, c) } else { byte(w, c) };
                if off { x - 128 } else { x }
            })
        };
        let p: Vec<i64> = lanes(s1, c1).zip(lanes(s2, c2)).map(|(a, b)| a * b).collect();
        let s = if op == "idot4" { p.iter().sum() } else { p[..3].iter().sum::<i64>() };
        let scale = |r: &str| 1i64 << insn.get(r).unwrap();
        let wide = op == "u16dot3" || insn.name("fmt", self.core).as_deref() == Some("u16");
        let (d, dc10) = Self::colour_operand(insn.get("dst").unwrap(), true)?;
        if dc10 {
            return Err(why("integer dot product into a .c10 destination"));
        }
        if wide {
            let t = 2 * s * scale("cx") * 32767;
            let v = (t + 65025).div_euclid(2 * 65025).clamp(-32767, 32767) as u32 & 0xffff;
            return self.write_dst(d, v | v << 16);
        }
        let c = colour_round(s * scale("cx"));
        let alpha = match insn.name("aop", self.core).as_deref() {
            None => colour_round(s * scale("ax")),
            Some(aop) => {
                let sel = insn.name("asel", self.core).unwrap();
                let g: Vec<&str> = sel.split(", ").collect();
                let s0 = match insn.get("src0") {
                    Some(v) => match Self::colour_operand0(v)? {
                        (reg, false) => self.src0(reg)?,
                        _ => return Err(why("integer dot product of a .c10 source 0")),
                    },
                    None => 0,
                };
                let f =
                    |k: usize| colour_factor(g[k], 3, [chans(s0, false), chans(s1, false), chans(s2, false)]);
                let a0 = byte(s0, 3);
                colour_round(match (aop, &g[..]) {
                    ("aadd", [_, _, _, _]) => (f(0)? * f(1)? + f(2)? * f(3)?) * scale("ax"),
                    ("asub", [_, _, _, _]) => (f(0)? * f(1)? - f(2)? * f(3)?) * scale("ax"),
                    ("aintrp1", [_, _]) => f(0)? * (255 - a0) + f(1)? * a0,
                    ("aintrp2", [_, _]) => (255 - f(0)?) * (255 - a0) + f(1)? * a0,
                    _ => return Err(why(format!("u8dot3 alpha {aop} {sel}"))),
                })
            }
        };
        self.write_dst(d, colour_out([c, c, c, alpha], false))
    }

    /// efo: two multipliers, two adders, the destination one of i0, i1, a0, a1
    /// and optional writes of i0/i1. Hardware (efo-dataflow, efo-rounding):
    /// each product and sum is rounded (fmad's arithmetic), and the
    /// destination reads i0/i1 as they were before this instruction's writes.
    fn efo(&mut self, insn: &Insn) -> Result<(), Error> {
        let gp = insn.name("flags", self.core).is_some_and(|f| f.contains(".gpiext"));
        if gp && self.quad.is_none() {
            return Err(why("efo.gpiext reaches the other instances of the quad"));
        }
        let read_i0 = Cell::new(false);
        // its infinite and NaN inputs are not measured
        for r in ["src0", "src1", "src2", "i0", "i1"] {
            if let Ok(v) = self.efo_value(insn, r, false, &Cell::new(false))
                && flt::special(v)
            {
                return Err(why("efo of an infinity or NaN"));
            }
        }
        let dsel = ["i0", "i1", "a0", "a1"][insn.get("dsel").unwrap() as usize];
        let iw = insn.get("iw").unwrap();
        let w = [["a0", "a1"], ["a1", "a0"], ["m0", "m1"], ["a0", "m1"]][(iw & 3) as usize];
        let d = self.efo_value(insn, dsel, gp, &read_i0)?;
        let w0 = if iw & 4 != 0 { Some(self.efo_value(insn, w[0], gp, &read_i0)?) } else { None };
        let w1 = if iw & 8 != 0 { Some(self.efo_value(insn, w[1], gp, &read_i0)?) } else { None };
        if gp {
            // hardware (efo-gpiext-quad, efo-gpiext-race): a write of i0 goes
            // to the instance's own i2; the four writes of i1 end in the
            // top-right i0 (run_quad), whose reads of i0 meanwhile race
            let q = self.quad.as_ref().unwrap();
            if w1.is_some() && q.pos == 1 && read_i0.get() {
                return Err(why("efo.gpiext: the top-right i0 read races the i1 writes"));
            }
            if let Some(v) = w0 {
                self.set_i(2, v)?;
            }
            self.quad_i1 = w1;
        } else {
            if let Some(v) = w0 {
                self.set_i(0, v)?;
            }
            if let Some(v) = w1 {
                self.set_i(1, v)?;
            }
        }
        self.write(insn, d)
    }

    /// A value of efo's dataflow, computed from what it reads (so that a
    /// source or internal register it does not use is never read).
    fn efo_value(&self, insn: &Insn, name: &str, gp: bool, read_i0: &Cell<bool>) -> Result<u32, Error> {
        let neg = |x: u32, n: bool| if n { x ^ 0x8000_0000 } else { x };
        let add = insn.get("add").unwrap();
        let a = [
            ["m0", "m1", "i1", "i0"],
            ["m0", "src0", "i1", "i0"],
            ["m0", "i0", "i1", "m1"],
            ["src0", "src2", "src1", "src2"],
        ][(add & 3) as usize];
        let m = [
            [["src0", "src2"], ["src1", "src2"]],
            [["src2", "src1"], ["src2", "src2"]],
            [["src1", "src0"], ["src2", "src2"]],
            [["src1", "i0"], ["src2", "i1"]],
        ][insn.get("msel").unwrap() as usize];
        let v = |n: &str| self.efo_value(insn, n, gp, read_i0);
        // .gpiext: the multipliers read own i2 for i0 and own i0 for i1
        let mul = |n: &str| match (gp, n) {
            (true, "i0") => v("i2"),
            (true, "i1") => v("i0"),
            _ => v(n),
        };
        Ok(match name {
            "src0" | "src1" => {
                let x = insn.get(name).unwrap();
                // source 0 numbers its banks r, pa, o, sa
                let bank = if name == "src0" { [0, 2, 1, 3][(x >> 7 & 3) as usize] } else { x >> 7 & 3 };
                neg(self.src(bank << 7 | x & 0x7f, 6)?, x >> 9 & 1 != 0)
            }
            "src2" => {
                let x = insn.get(name).unwrap();
                neg(self.src(x & 0x3ff, 6)?, x >> 10 & 1 != 0)
            }
            "i0" | "i1" | "i2" => {
                let k = ["i0", "i1", "i2"].iter().position(|&x| x == name).unwrap();
                if k == 0 {
                    read_i0.set(true);
                }
                self.internal(k)?
            }
            "m0" => flt::mul(mul(m[0][0])?, mul(m[0][1])?)?,
            "m1" => flt::mul(mul(m[1][0])?, mul(m[1][1])?)?,
            "a0" => flt::add(v(a[0]), neg(v(a[1])?, add & 8 != 0))?,
            _ => flt::add(Ok(neg(v(a[2])?, add & 4 != 0)), v(a[3])?)?,
        })
    }

    /// fpma per byte: a + b * c / 255 of its three selections (the alpha byte
    /// by the alpha ones), rounded half up and saturated; .neg on source 0
    /// subtracts a (hardware: fpma, fpma-sat, x-fpma, every non-c10 byte).
    /// Where a selects s1 the probes cannot tell whether .neg applies.
    fn fpma(&mut self, insn: &Insn) -> Result<(), Error> {
        let v0 = insn.get("src0").unwrap();
        let s = [
            self.colour_src0(v0 & 0xff)?,
            self.colour_src(insn.get("src1").unwrap())?,
            self.colour_src(insn.get("src2").unwrap())?,
        ];
        let neg = v0 >> 8 & 1 != 0;
        let (d, dc10) = Self::colour_operand(insn.get("dst").unwrap(), true)?;
        let (csel, asel) = (insn.name("csel", self.core).unwrap(), insn.name("asel", self.core).unwrap());
        let mut v = [0; 4];
        for ch in 0..4 {
            let sel: Vec<&str> = if ch == 3 { &asel } else { &csel }.split(", ").collect();
            if neg && sel[0].starts_with("s1") {
                return Err(why("fpma .neg with s1 added"));
            }
            let f = |k: usize| colour_factor(sel[k], ch, s);
            let a = if neg { -f(0)? } else { f(0)? };
            v[ch as usize] = colour_round(a * 255 + f(1)? * f(2)?);
        }
        self.write_dst(d, colour_out(v, dc10))
    }

    /// The dual-issue group. Hardware (dual-issue, dual-issue-predicates,
    /// dual-issue-ima32): the primary computes as alone, the secondary on
    /// i0-i2 or, for a register operand, the primary's source in that position
    /// without its .neg; both read before either writes, and the secondary
    /// wins a shared destination. The pairs that share a unit
    /// (dual-issue-shared): a multiplying secondary of a two-product primary
    /// gets its second product (fmad adding its own c), an fmsa secondary of
    /// fmad, fadm or fmsa (and ima32 of ima32) the primary's result, the
    /// add-type and fmad secondaries of fminmax max(a, b) - c, and fmad +
    /// fmaxmin gives a*b - c and b - c, fmad + fmad16 a primary 0 and the
    /// secondary as alone. .repeat2 repeats both: the primary's
    /// operands advance, the secondary runs again on the internal registers
    /// (a secondary register operand stops). fddp and the undecided pairs
    /// stop.
    fn dual(&mut self, insn: &Insn) -> Result<(), Error> {
        let pi = insn.form.role("pop").unwrap();
        if insn.v[pi] & 64 == 0 {
            return self.dual_once(insn);
        }
        for (r, top) in [("r0", 3), ("r1", 3), ("r2", 3), ("rm0", 1), ("rm1", 1), ("rm2", 1)] {
            if let Some(i) = insn.form.role(r) {
                let w = insn.form.f[i].width() - if top == 3 { 2 } else { 1 };
                if insn.v[i] >> w == top {
                    return Err(why("dual issue .repeat2 with a secondary register operand"));
                }
            }
        }
        for it in self.steps(insn, pi, insn.v[pi] & !64, &[0, 1], 1, false)? {
            self.dual_once(&it)?;
        }
        Ok(())
    }

    fn dual_once(&mut self, insn: &Insn) -> Result<(), Error> {
        let pop = insn.name("pop", self.core).unwrap();
        let mut parts = pop.split('.');
        let p = parts.next().unwrap();
        for f in parts {
            if !matches!(f, "skipinv" | "syncs" | "nosched") {
                return Err(why(format!("dual issue .{f}")));
            }
        }
        let sop = insn.name("sop", self.core).unwrap();
        let func = matches!(sop.as_str(), "frcp" | "frsq" | "fexp" | "flog" | "fsin" | "fcos");
        // how the secondary runs: as alone, or on a value the shared unit gives
        #[derive(PartialEq)]
        enum Pair {
            Alone,
            SecondProduct,
            Result,
            MaxMinusC,
            MaxMin,
            NoPrimary,
        }
        let pair = match (p, sop.as_str()) {
            (_, "mov") => Pair::Alone,
            (_, _) if func => Pair::Alone,
            ("mov", _) => Pair::Alone,
            ("ima32", "fmad16") => Pair::Alone,
            ("fmad", "fmad16") => Pair::NoPrimary,
            ("fmad", "fmad" | "fadd" | "fsub" | "fmul" | "fsubflr" | "ima32") => Pair::Alone,
            ("ima32", "fmad" | "fadd" | "fsub" | "fmul" | "fsubflr" | "fmsa") => Pair::Alone,
            ("fadm" | "fmsa" | "fssq", "fadd" | "fsub" | "ima32") => Pair::Alone,
            ("fadm" | "fmsa" | "fssq", "fmul" | "fmad") => Pair::SecondProduct,
            ("fmad" | "fadm" | "fmsa", "fmsa") | ("ima32", "ima32") => Pair::Result,
            ("fminmax", "fmul") => Pair::Alone,
            ("fminmax", "fadd" | "fsub" | "fmad") => Pair::MaxMinusC,
            ("fmad", "fmaxmin") => Pair::MaxMin,
            _ => return Err(why(format!("dual issue {p} + {sop}"))),
        };
        // the primary's sources, as values without modifiers and with them
        let plain = |m: &Self, k: usize| -> Result<u32, Error> {
            let v = insn.get(["src0", "src1", "src2"][k]).unwrap();
            let abs = if k == 0 { v >> 8 & 1 } else { v >> 11 & 1 };
            if abs != 0 {
                return Err(why("dual issue with .abs on a primary source"));
            }
            if k == 0 { m.src0(v & 0xff) } else { m.src(v & 0x3ff, 6) }
        };
        let fs = |m: &Self, k: usize| -> Result<u32, Error> {
            let v = insn.get(["src0", "src1", "src2"][k]).unwrap();
            if k == 0 { m.fsrc0(v) } else { m.fsrc(v) }
        };
        let primary = match p {
            "mov" => {
                let v = insn.get("src1").unwrap();
                if v >> 10 != 0 {
                    return Err(why("dual issue mov with a modifier"));
                }
                self.src(v, 6)?
            }
            _ if pair == Pair::NoPrimary => 0,
            "fmad" if pair == Pair::MaxMin => {
                flt::sub(flt::mul(fs(self, 0)?, fs(self, 1)?)?, Ok(fs(self, 2)?))?
            }
            "fmad" => flt::add(flt::mul(fs(self, 0)?, fs(self, 1)?), fs(self, 2)?)?,
            "fadm" => flt::add(flt::mul(fs(self, 0)?, fs(self, 2)?), flt::mul(fs(self, 1)?, fs(self, 2)?)?)?,
            "fmsa" => flt::add(flt::mul(fs(self, 0)?, fs(self, 0)?), flt::mul(fs(self, 1)?, fs(self, 2)?)?)?,
            "fssq" => flt::add(flt::mul(fs(self, 1)?, fs(self, 1)?), flt::mul(fs(self, 2)?, fs(self, 2)?)?)?,
            "fminmax" => flt::min(flt::max(fs(self, 0)?, fs(self, 1)?)?, fs(self, 2)?)?,
            "ima32" => plain(self, 0)?.wrapping_mul(plain(self, 1)?).wrapping_add(plain(self, 2)?),
            _ => return Err(why(format!("dual issue {p} + {sop}"))),
        };
        // the secondary's operand k: i<sel>, or the register its field shares
        // with the primary's source k (read without modifiers)
        let shared = |m: &Self, k: usize, reg: u32| -> Result<u32, Error> {
            if insn.get(["src0", "src1", "src2"][k]).is_some() {
                plain(m, k)
            } else if k == 0 {
                m.src0(reg & 0xff)
            } else {
                m.src(reg & 0x3ff, 6)
            }
        };
        let operand = |m: &Self, k: usize| -> Result<u32, Error> {
            if let Some(i) = insn.form.role(["rm0", "rm1", "rm2"][k]) {
                let w = insn.form.f[i].width() - 1;
                let v = insn.v[i];
                return if v >> w == 0 { m.internal(k) } else { shared(m, k, v & ((1 << w) - 1)) };
            }
            let i = insn.form.role(["r0", "r1", "r2"][k]).unwrap();
            let w = insn.form.f[i].width() - 2;
            let v = insn.v[i];
            match v >> w {
                3 => shared(m, k, v & ((1 << w) - 1)),
                sel => m.internal(sel as usize),
            }
        };
        // the destination of the three-operand secondaries shares source
        // 0's selection (i0 for a register source 0)
        let sel0 = || {
            let i = insn.form.role("r0").unwrap();
            let sel = insn.v[i] >> (insn.form.f[i].width() - 2);
            if sel == 3 { 0 } else { sel }
        };
        let mut bound = None;
        let (sdst, value) = match sop.as_str() {
            _ if pair == Pair::Result => (sel0(), primary),
            _ if pair == Pair::SecondProduct => {
                let second = match p {
                    "fssq" => flt::mul(fs(self, 2)?, fs(self, 2)?)?,
                    _ => flt::mul(fs(self, 1)?, fs(self, 2)?)?,
                };
                if sop == "fmad" {
                    (sel0(), flt::add(Ok(second), operand(self, 2)?)?)
                } else {
                    (insn.get("sdst").unwrap(), second)
                }
            }
            _ if pair == Pair::MaxMinusC => {
                let v = flt::sub(flt::max(fs(self, 0)?, fs(self, 1)?)?, Ok(fs(self, 2)?))?;
                (if sop == "fmad" { sel0() } else { insn.get("sdst").unwrap() }, v)
            }
            "fmaxmin" => {
                let (a, b, c) = (operand(self, 0)?, operand(self, 1)?, operand(self, 2)?);
                // hardware (fmaxmin-roles): max(min(a, c), b) alone
                (
                    sel0(),
                    if pair == Pair::MaxMin { flt::sub(b, Ok(c))? } else { flt::max(flt::min(a, c)?, b)? },
                )
            }
            "fmad16" => {
                // a * b + c per half lane, as fmad16 alone
                let lanes = |v: u32| -> Result<[half::Exact; 2], Error> {
                    Ok([half::from_half(v as u16)?, half::from_half((v >> 16) as u16)?])
                };
                let (a, b, c) =
                    (lanes(operand(self, 0)?)?, lanes(operand(self, 1)?)?, lanes(operand(self, 2)?)?);
                let (lo, hi) = (half::fma(a[0], b[0], c[0])?, half::fma(a[1], b[1], c[1])?);
                (sel0(), lo as u32 | (hi as u32) << 16)
            }
            "ima32" => {
                // two 16-bit lanes, a * b + c each, wrapping; b is the primary's
                // source 1 next to a float primary
                let a = operand(self, 0)?;
                let b = if p == "mov" { operand(self, 1)? } else { plain(self, 1)? };
                let c = operand(self, 2)?;
                let lane = |k: u32| {
                    (a >> k & 0xffff).wrapping_mul(b >> k & 0xffff).wrapping_add(c >> k & 0xffff) & 0xffff
                };
                (sel0(), lane(0) | lane(16) << 16)
            }
            _ if func => {
                let (v, b) = func::eval(&sop, operand(self, 1)?).map_err(why)?;
                if let Some(b) = b {
                    if !self.near {
                        return Err(why(format!("{sop} is near on the hardware ({b:?}): a near value")));
                    }
                    if self.quad.is_some() {
                        return Err(why(format!("{sop}: a near value in a quad")));
                    }
                }
                bound = Some(b);
                (insn.get("sdst").unwrap(), v)
            }
            "mov" => {
                let k = (0..3).find(|&k| insn.form.role(["rm0", "rm1", "rm2"][k]).is_some()).unwrap();
                (insn.get("sdst").unwrap(), operand(self, k)?)
            }
            "fadd" | "fsub" | "fmul" | "fsubflr" => {
                let (a, b) = (operand(self, 1)?, operand(self, 2)?);
                let v = match sop.as_str() {
                    "fadd" => flt::add(Ok(a), b)?,
                    "fsub" => flt::sub(a, Ok(b))?,
                    "fmul" => flt::mul(a, b)?,
                    _ => flt::sub(a, flt::floor(b))?,
                };
                (insn.get("sdst").unwrap(), v)
            }
            "fmad" | "fmsa" => {
                let (a, b, c) = (operand(self, 0)?, operand(self, 1)?, operand(self, 2)?);
                let v = if sop == "fmad" {
                    flt::add(flt::mul(a, b), c)?
                } else {
                    flt::add(flt::mul(a, a), flt::mul(b, c)?)?
                };
                (sel0(), v)
            }
            s => return Err(why(format!("dual issue secondary {s}"))),
        };
        let sdst = [0, 1, 2, 0][sdst as usize] as usize;
        self.write(insn, primary)?;
        if let Some(b) = bound {
            self.now.out = Out::Fn(b);
        }
        self.set_i(sdst, value)
    }

    /// fdsx/fdsy's sources in this instance (the quad combines them); a
    /// predicate that leaves the instance out, flags, repeats and .incs stop.
    fn fds_sources(&self, insn: &Insn) -> Result<(u32, u32), Error> {
        if let Some(f) = insn.name("flags", self.core)
            && f.split('.').any(|t| !t.is_empty() && !matches!(t, "skipinv" | "syncs" | "nosched" | "end"))
        {
            return Err(why(format!("fds flags {f}")));
        }
        if insn.get("rep").is_some_and(|r| r != 0) {
            return Err(why("fds repeated"));
        }
        if !self.pred(insn)? {
            return Err(why("fds with an instance predicated off"));
        }
        Ok((self.fsrc(insn.get("src1").unwrap())?, self.fsrc(insn.get("src2").unwrap())?))
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
                        Reg::R(n) => self.set_r(n, v)?,
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
                self.now.out = Out::Move;
                self.write(insn, v)?;
                None
            }
            "fsc" => {
                self.fsc(insn)?;
                None
            }
            "smp" => {
                self.smp(insn, bus)?;
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
            "ima8" => {
                self.ima8(insn)?;
                None
            }
            "ima16" if insn.form.role("fmt").is_some() => {
                self.ima16_formats(insn)?;
                None
            }
            "imae" if insn.form.role("carry").is_some() => {
                self.imae_carry(insn)?;
                None
            }
            "ima32" if insn.form.role("cie").is_some() => {
                // hardware (imae-modes): cie, iN adds iN; the extra operand
                // and the source negations are not measured
                if insn.get("x").unwrap() & 1 != 0 {
                    return Err(why("ima32 with an extra operand"));
                }
                let (bv, cv) = (insn.get("src1x").unwrap(), insn.get("src2x").unwrap());
                if (bv | cv) >> 10 != 0 {
                    return Err(why("ima32 with a negated source"));
                }
                let a = self.src0(insn.get("src0").unwrap())?;
                let (b, c) = (self.src(bv, 6)?, self.src(cv, 6)?);
                let cie = insn.get("cie").unwrap();
                let cin = if cie == 0 { 0 } else { self.internal(cie as usize - 1)? };
                self.write(insn, a.wrapping_mul(b).wrapping_add(c).wrapping_add(cin))?;
                None
            }
            "ima16" | "ima32" => {
                if let Some(t) = insn.name("sign", self.core)
                    && t != "u"
                    && t != "s"
                {
                    return Err(why(format!("{op}.{t}")));
                }
                // hardware (ima16): .neg on source 2 subtracts it, per lane
                let neg = op == "ima16" && insn.name("neg", self.core).is_some_and(|t| !t.is_empty());
                for r in ["rs", "abs", "neg", "neg1", "neg2"] {
                    if let Some(t) = insn.name(r, self.core)
                        && !t.is_empty()
                        && !(r == "neg" && neg)
                    {
                        return Err(why(format!("{op} modifier {t}")));
                    }
                }
                let a = self.src0(insn.get("src0").unwrap())?;
                let (bv, cv) = (insn.get("src1").unwrap(), insn.get("src2").unwrap());
                let (mut b, mut c) = (self.src(bv, 6)?, self.src(cv, 6)?);
                if op == "ima16" {
                    (b, c) = (lane_imm(bv, b), lane_imm(cv, c));
                    if neg {
                        // two's complement per lane
                        c = (0x1_0000 - (c & 0xffff)) & 0xffff | ((0x1_0000 - (c >> 16)) & 0xffff) << 16;
                    }
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
                        // hardware (fadm-fmsa-fsubflr): a * c + b * c, each
                        // product rounded, then the sum
                        "fadm" => flt::add(flt::mul(a, c), flt::mul(b, c)?),
                        o => return Err(why(format!("fmad-group op {o}"))),
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
                    if r < 124 { self.set_r(r, link)? } else { self.set_i(r - 124, link)? }
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
            // the emits hand the instruction and its sources (0 where it has
            // none) to the unit behind the bus; what that unit does is not
            // the USSE's
            "emitpds" | "emit" => {
                let srcs = [
                    insn.get("src0").map_or(Ok(0), |v| self.src0(v))?,
                    insn.get("src1").map_or(Ok(0), |v| self.src(v, 6))?,
                    insn.get("src2").map_or(Ok(0), |v| self.src(v, 6))?,
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
            "idot4" | "idot3" | "u16dot3" | "u8dot3a" => {
                self.idot(insn)?;
                None
            }
            "fmad16" => {
                let lanes =
                    |m: &Self, role: &str, mods: &str, src0: bool| -> Result<[half::Exact; 2], Error> {
                        let v = insn.get(role).unwrap();
                        let w = if src0 { m.src0(v & 0xff)? } else { m.src(v, 6)? };
                        // neg[0] | abs[1] | swizzle[3:2]; source 0 has no neg bit
                        let k = insn.get(mods).unwrap() << src0 as u32;
                        let (lo, hi) = (w as u16, (w >> 16) as u16);
                        let modify = |h: u16| {
                            let h = if k & 2 != 0 { h & 0x7fff } else { h };
                            if k & 1 != 0 { h ^ 0x8000 } else { h }
                        };
                        let [a, b] = match k >> 2 & 3 {
                            0 => [lo, hi],
                            1 => [lo, lo],
                            2 => [hi, hi],
                            _ => {
                                // .swzf32: the whole register as a binary32, in both lanes
                                let w = if k & 2 != 0 { w & 0x7fff_ffff } else { w };
                                let x = half::from_f32(if k & 1 != 0 { w ^ 0x8000_0000 } else { w })?;
                                return Ok([x, x]);
                            }
                        };
                        Ok([half::from_half(modify(a))?, half::from_half(modify(b))?])
                    };
                let (a, b, c) = (
                    lanes(self, "src0", "m0", true)?,
                    lanes(self, "src1", "m1", false)?,
                    lanes(self, "src2", "m2", false)?,
                );
                let lo = half::fma(a[0], b[0], c[0])?;
                let hi = half::fma(a[1], b[1], c[1])?;
                self.write(insn, lo as u32 | (hi as u32) << 16)?;
                None
            }
            "fpma" => {
                self.fpma(insn)?;
                None
            }
            "rlp" => {
                // hardware (rlp): d = s << 1 | pN, then pN = bit 31 of s
                // rlp-high-predicates: numbers from 4 up alias p0-p3 (mod 4)
                let n = insn.get("p").unwrap() as usize % 4;
                let s = self.src(insn.get("src1").unwrap(), 6)?;
                self.decide()?;
                self.write(insn, s << 1 | self.p[n] as u32)?;
                self.p[n] = s >> 31 != 0;
                None
            }
            "adif" | "adifsum" => {
                // hardware (video-adif): |a - b| per byte; adifsum sums the
                // four and adds a 16-bit half of s0
                let (a, b) =
                    (self.src(insn.get("src1").unwrap(), 6)?, self.src(insn.get("src2").unwrap(), 6)?);
                let d = |k: u32| (a >> (8 * k) & 0xff).abs_diff(b >> (8 * k) & 0xff);
                let v = if op == "adif" {
                    (0..4).fold(0, |w, k| w | d(k) << (8 * k))
                } else {
                    let s0 = self.src0(insn.get("src0").unwrap())?;
                    let half = if insn.name("half", self.core).as_deref() == Some("high") {
                        s0 >> 16
                    } else {
                        s0 & 0xffff
                    };
                    (0..4).map(d).sum::<u32>() + half
                };
                self.write(insn, v)?;
                None
            }
            "dual" => {
                self.dual(insn)?;
                None
            }
            "fds" => return Err(why("fdsx/fdsy read the other instances of the quad")),
            "efo" => {
                self.efo(insn)?;
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
    /// frcp, frsq, flog, fexp, fsqrt, fsin, fcos (see func): the source with
    /// its .neg and .abs; half-float and c10 sources are not measured.
    fn fsc(&mut self, insn: &Insn) -> Result<(), Error> {
        let name = insn.name("op", self.core).unwrap();
        let mods = insn.get("mods").unwrap();
        if mods >> 2 & 3 != 0 {
            return Err(why(format!(
                "{name} of a {} source",
                if mods >> 2 & 3 == 1 { "half" } else { "c10" }
            )));
        }
        let x = flt::modify(self.src(insn.get("src1").unwrap(), 6)?, mods & 1 != 0, mods >> 1 & 1 != 0);
        let (v, b) = func::eval(&name, x).map_err(why)?;
        if let Some(b) = b {
            if !self.near {
                return Err(why(format!("{name} is near on the hardware ({b:?}): a near value")));
            }
            if self.quad.is_some() {
                return Err(why(format!("{name}: a near value in a quad")));
            }
        }
        self.now.out = Out::Fn(b);
        self.write(insn, v)
    }

    /// smp1d/smp2d d, c, s, drcN (see tex): the state words in temporaries s
    /// to s + 3, the coordinates in c and c + 1 (.flt16: the halves of c, u
    /// low); the texels land in d, d + 1, ... at a wdf on drcN, as loads do.
    /// Hardware (smp-point, smp-lod): the LOD forms with a LOD of 0 sample as
    /// the plain ones.
    fn smp(&mut self, insn: &Insn, bus: &mut dyn Bus) -> Result<(), Error> {
        let dim = insn.name("dim", self.core).unwrap();
        let (d, lod) = dim.split_at(2);
        if d == "3d" {
            return Err(why("smp3d"));
        }
        if !lod.is_empty() {
            let l = self.src(insn.get("src2").unwrap(), 6)?;
            if l != 0 {
                return Err(why(format!("smp{dim} with a LOD value of 0x{l:08x}")));
            }
        }
        let c = insn.get("src0").unwrap();
        let (u, v) = match insn.name("fmt", self.core).unwrap().as_str() {
            "" => {
                if c & 0x7f == 0x7f && d == "2d" {
                    return Err(why("coordinates past the bank"));
                }
                (self.src0(c)?, if d == "1d" { 0 } else { self.src0(c + 1)? })
            }
            ".flt16" => {
                let w = self.src0(c)?;
                (half_f32(w & 0xffff)?, half_f32(w >> 16)?)
            }
            f => return Err(why(format!("smp with {f} coordinates"))),
        };
        // .pcf's reference: the f32 after the coordinates (.pcff16: a half,
        // the low one)
        let flags = insn.name("flags", self.core).unwrap_or_default();
        let pcf = if flags.contains(".pcf") {
            let fmt = insn.name("fmt", self.core).unwrap();
            if d != "2d" || !fmt.is_empty() || c & 0x7f > 0x7d {
                return Err(why(format!("smp{d}{fmt} with .pcf")));
            }
            let r = self.src0(c + 2)?;
            let r = if flags.contains(".pcff16") { half_f32(r & 0xffff)? } else { r };
            if !f32::from_bits(r).is_finite() {
                return Err(why("an infinite or NaN .pcf reference"));
            }
            Some(f32::from_bits(r))
        } else {
            None
        };
        let st = insn.get("src1").unwrap();
        if st >> 7 != 0 {
            return Err(why("texture state outside the temporaries"));
        }
        let mut state = [0; 4];
        for (k, w) in state.iter_mut().enumerate() {
            *w = self.temp((st & 0x7f) as usize + k)?;
        }
        if self.now.read.get().is_some() {
            return Err(why("a sample from a near value"));
        }
        let (u, v) = (f32::from_bits(u), f32::from_bits(v));
        if !u.is_finite() || !v.is_finite() {
            return Err(why("smp at an infinite or NaN coordinate"));
        }
        let texels = bus.sample(&tex::Sample { state, u, v, one_d: d == "1d", pcf })?;
        let (dst, drc) = (insn.get("dst").unwrap(), insn.get("drc").unwrap());
        for (k, t) in texels.into_iter().enumerate() {
            let r = (dst & 0x7f) as usize + k;
            if r >= 124 {
                return Err(why("a sample past the temporaries"));
            }
            self.pending.push((drc, if dst >> 7 != 0 { Reg::Pa(r) } else { Reg::R(r) }, t));
        }
        Ok(())
    }

    /// ima8 d, a, b, c, x, y, z, xa, ya, za: x + y * z per byte, the colour
    /// selectors for bytes 0-2 and the alpha ones for byte 3 (s0-s2 = a-c, sNa
    /// the alpha byte, .comp 255 - the byte), wrapping, or clamped to 255 with
    /// .sat. Hardware (ima8).
    fn ima8(&mut self, insn: &Insn) -> Result<(), Error> {
        let (d, dc10) = Self::colour_operand(insn.get("dst").unwrap(), true)?;
        let (r0, c0) = Self::colour_operand0(insn.get("src0").unwrap())?;
        let (r1, c1) = Self::colour_operand(insn.get("src1").unwrap(), false)?;
        let (r2, c2) = Self::colour_operand(insn.get("src2").unwrap(), false)?;
        if dc10 || c0 || c1 || c2 {
            return Err(why("ima8 with c10 operands"));
        }
        let s = [self.src0(r0)?, self.src(r1, 6)?, self.src(r2, 6)?];
        let sat = insn.name("sat", self.core).is_some_and(|t| !t.is_empty());
        let sel = insn.name("sel", self.core).unwrap();
        let sel: Vec<&str> = sel.split(", ").collect();
        let byte = |name: &str, k: u32| -> u32 {
            let (name, comp) = name.strip_suffix(".comp").map_or((name, false), |n| (n, true));
            let src = (name.as_bytes()[1] - b'0') as usize;
            let k = if name.ends_with('a') { 3 } else { k };
            let b = s[src] >> (8 * k) & 0xff;
            if comp { 255 - b } else { b }
        };
        let mut v = 0;
        for k in 0..4 {
            let t = if k < 3 { &sel[0..3] } else { &sel[3..6] };
            let r = byte(t[0], k) + byte(t[1], k) * byte(t[2], k);
            v |= (if sat { r.min(255) } else { r & 0xff }) << (8 * k);
        }
        self.write_dst(d, v)
    }

    /// ima16 with its source formats (fmt: source 1's [1:0], source 2's
    /// [3:2]: u16, u8, s8, o8): a * b + c per 16-bit lane, wrapping; an 8-bit
    /// source is the byte .low or .high of that lane's half, u8 unsigned, s8
    /// signed, o8 the byte - 128. Hardware (ima16, ima16-8bit).
    fn ima16_formats(&mut self, insn: &Insn) -> Result<(), Error> {
        for r in ["rs", "abs"] {
            if let Some(t) = insn.name(r, self.core)
                && !t.is_empty()
            {
                return Err(why(format!("ima16 modifier {t}")));
            }
        }
        if let Some(t) = insn.name("sign", self.core)
            && t != "u"
            && t != "s"
        {
            return Err(why(format!("ima16.{t}")));
        }
        let fmt = insn.get("fmt").unwrap();
        let a = self.src0(insn.get("src0").unwrap())?;
        // (operand, .high, format)
        let (bv, bh) = match insn.get("src1") {
            Some(v) => (v, false),
            None => {
                let v = insn.get("src1x").unwrap();
                (v & 0x3ff, v >> 10 & 1 != 0)
            }
        };
        let cx = insn.get("src2x").unwrap();
        let (cv, neg, ch) = (cx & 0x3ff, cx >> 10 & 1 != 0, fmt >> 2 != 0 && cx >> 11 & 1 != 0);
        let mut ops = [(bv, bh, fmt & 3), (cv, ch, fmt >> 2 & 3)].map(|(v, h, f)| (v, h, f, 0u32));
        for o in ops.iter_mut() {
            if o.2 != 0 && o.0 >> 7 == 6 {
                return Err(why("ima16 with an 8-bit immediate"));
            }
            o.3 = lane_imm(o.0, self.src(o.0, 6)?);
        }
        let lane = |(_, high, f, x): (u32, bool, u32, u32), k: u32| -> i64 {
            let h = x >> (16 * k) & 0xffff;
            let b = if high { h >> 8 } else { h & 0xff };
            match f {
                0 => h as i64,
                1 => b as i64,
                2 => b as u8 as i8 as i64,
                _ => b as i64 - 128,
            }
        };
        let mut v = 0;
        for k in 0..2 {
            let (b, c) = (lane(ops[0], k), lane(ops[1], k));
            let r = (a >> (16 * k) & 0xffff) as i64 * b + if neg { -c } else { c };
            v |= (r as u32 & 0xffff) << (16 * k);
        }
        self.write(insn, v)
    }

    /// imae with its source-2 type and carries (carry: type [1:0] z16, u16,
    /// u32 | coe | cie | register): the selected halves multiplied, source 2
    /// added (a u16 half sign-extended, z16 zero-extended), cie adding iN,
    /// .rsN shifting, saturating or wrapping as plain imae; coe writes the
    /// carry out to iN. Hardware (imae-modes).
    fn imae_carry(&mut self, insn: &Insn) -> Result<(), Error> {
        let sign = insn.name("sign", self.core).unwrap();
        let rs: u32 =
            insn.name("rs", self.core).unwrap().strip_prefix(".rs").map_or(0, |n| n.parse().unwrap());
        let sat = sign.ends_with("sat");
        if rs != 0 && sat {
            return Err(why("imae with both .rsN and saturation"));
        }
        let half = |v: u32, high: bool| if high { v >> 16 } else { v & 0xffff };
        let (x0, x1) = (insn.get("src0x").unwrap(), insn.get("src1x").unwrap());
        let a = half(self.src0(x0 & 0xff)?, x0 >> 8 & 1 != 0);
        let b = half(self.src(x1 & 0x3ff, 6)?, x1 >> 10 & 1 != 0);
        let carry = insn.get("carry").unwrap();
        let c: i64 = match insn.get("src2") {
            Some(v) => self.src(v, 6)? as i64,
            None => {
                let x = insn.get("src2x").unwrap();
                let h = half(self.src(x & 0x3ff, 6)?, x >> 10 & 1 != 0);
                if carry & 3 == 0 { h as i64 } else { sext16(h) }
            }
        };
        let reg = (carry >> 4 & 1) as usize;
        let (coe, cie) = (carry & 4 != 0, carry & 8 != 0);
        if coe && cie {
            return Err(why("imae with both carries"));
        }
        let cin = if cie { self.internal(reg)? as i64 } else { 0 };
        let signed = sign.starts_with('s');
        let c = if signed && insn.get("src2").is_some() { c as u32 as i32 as i64 } else { c };
        let prod = if signed { sext16(a) * sext16(b) } else { a as i64 * b as i64 };
        let sum = prod + c + cin;
        let v = sum >> rs;
        let v = match (sat, signed) {
            (true, true) => v.clamp(i32::MIN as i64, i32::MAX as i64) as u32,
            (true, false) => v.clamp(0, u32::MAX as i64) as u32,
            _ => v as u32,
        };
        self.write(insn, v)?;
        if coe {
            self.set_i(reg, (sum >> 32) as u32)?;
        }
        Ok(())
    }

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
        self.decide()?;
        let (result, tested) = alu_test_value(&name, a, b)?;
        let test = insn.name("test", self.core).unwrap();
        // only isubu16's zero test is measured (vec-isubu16-taz)
        if name == "isubu16" && !test.starts_with("testt") {
            return Err(why(format!("{name} sign test {test}")));
        }
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
        if self.now.read.get().is_some() {
            return Err(why("an address from a near value"));
        }
        if op.starts_with("st") {
            let v = self.src(insn.get("src").unwrap(), 6)?;
            match self.now.read.get() {
                Some(b) => self.near_mem.insert(addr, b),
                None => self.near_mem.remove(&addr),
            };
            return bus.store32(addr, v);
        }
        let d = insn.get("dst").unwrap();
        for k in 0..count {
            if self.near_mem.contains_key(&addr.wrapping_add(4 * k)) {
                return Err(why("a load of a near value"));
            }
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
                self.read(self.pa[r], self.written.pa, r, "pa")?
            }
            2 => self.read(self.o[r], self.written.o, r, "o")?,
            3 => self.read(self.sa[r], self.written.sa, r, "sa")?,
            b => return Err(why(format!("source-0 bank {b}"))),
        })
    }

    fn write_src0(&mut self, v: u32, x: u32) -> Result<(), Error> {
        let r = (v & 0x7f) as usize;
        match v >> 7 {
            0 if r < 124 => self.set_r(r, x)?,
            1 => self.set_pa(r, x),
            2 => {
                (self.o[r], self.written.o) = (x, self.written.o | 1 << r);
                self.mark("o", r);
            }
            3 => {
                (self.sa[r], self.written.sa) = (x, self.written.sa | 1 << r);
                self.mark("sa", r);
            }
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
            1 => self.read(self.o[r], self.written.o, r, "o")?,
            2 => {
                self.not_pending(Reg::Pa(r))?;
                self.read(self.pa[r], self.written.pa, r, "pa")?
            }
            3 => self.read(self.sa[r], self.written.sa, r, "sa")?,
            6 => r as u32,
            7 => self.internal(r)?,
            4 => return self.src(self.indexed(r as u32, [0, 1, 2, 3])?, 6),
            b => return Err(why(format!("source bank {b}"))),
        })
    }

    /// An indexed operand, bank[6:5] (r, o, pa, sa) | i.h[4] | offset[3:0],
    /// as an ordinary one with `banks` numbering r, o, pa and sa. Hardware
    /// (index-unit, index-width3): the index counts registers and only bits
    /// 0-9 of i.l count (i.h is assumed alike: its probes stay below 0x400).
    fn indexed(&self, r: u32, banks: [u32; 4]) -> Result<u32, Error> {
        let h = (r >> 4 & 1) as usize;
        if self.written.index >> h & 1 == 0 {
            return Err(why(format!(
                "reads i.{} before the program writes it",
                if h == 1 { "h" } else { "l" }
            )));
        }
        let n = (self.index[h] & 0x3ff) + (r & 15);
        if n > 127 {
            return Err(why("index past the bank"));
        }
        Ok(banks[(r >> 5 & 3) as usize] << 7 | n)
    }

    fn temp(&self, r: usize) -> Result<u32, Error> {
        if r >= 124 {
            return self.internal(r - 124);
        }
        self.own_temp(r)?;
        self.not_pending(Reg::R(r))?;
        self.read(self.r[r], self.written.r, r, "r")
    }

    /// A read of register n of a bank (see read), noting a near value's bound.
    fn read(&self, v: u32, written: u128, n: usize, bank: &'static str) -> Result<u32, Error> {
        let v = read(v, written, n, bank)?;
        if let Some(&b) = self.bounds.get(&(bank, n)) {
            self.now.read.set(Some(if self.now.read.get().is_some() { Bound::Derived } else { b }));
        }
        Ok(v)
    }

    /// Note what a write of register n of a bank holds (see Machine::bounds).
    fn mark(&mut self, bank: &'static str, n: usize) {
        let r = self.now.read.get();
        let b = match self.now.out {
            Out::Reads => r.map(|_| Bound::Derived),
            Out::Move => r,
            Out::Fn(b) => r.map(|_| Bound::Derived).or(b),
        };
        match b {
            Some(b) => self.bounds.insert((bank, n), b),
            None => self.bounds.remove(&(bank, n)),
        };
    }

    /// A decision (a predicate) from what the operation read: one on a near
    /// value is not known.
    fn decide(&self) -> Result<(), Error> {
        if self.now.read.get().is_some() {
            return Err(why("a predicate from a near value"));
        }
        Ok(())
    }

    fn not_pending(&self, reg: Reg) -> Result<(), Error> {
        if self.pending.iter().any(|p| p.1 == reg) {
            return Err(why(format!("reads {reg:?} before its DRC result lands (wdf)")));
        }
        Ok(())
    }

    fn internal(&self, r: usize) -> Result<u32, Error> {
        if r < 3 {
            return self.read(self.i[r], self.written.i as u128, r, "i");
        }
        // hardware (internal-registers-per-pixel, internal-registers-bank7):
        // number n mod 16 is 4m + j, ij of the instance m places on (TL, TR,
        // BL, BR, round again), j = 3 being i2 there, but i3 itself the next
        // instance's i0
        let Some(q) = &self.quad else { return Err(why(format!("i{r} reads another instance of the quad"))) };
        let n = r % 16;
        let (m, j) = if n == 3 { (1, 0) } else { (n / 4, (n % 4).min(2)) };
        let k = (q.pos + m) % 4;
        read(q.i[k][j], q.written[k] as u128, j, "a quad neighbour's i")
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
            0 if r < 124 => self.set_r(r, v)?,
            0 => self.set_i(r - 124, v)?,
            1 => {
                (self.o[r], self.written.o) = (v, self.written.o | 1 << r);
                self.mark("o", r);
            }
            2 => self.set_pa(r, v),
            4 => {
                (self.sa[r], self.written.sa) = (v, self.written.sa | 1 << r);
                self.mark("sa", r);
            }
            7 => self.set_i(r, v)?,
            3 => return self.write_dst(self.indexed(r as u32, [0, 1, 2, 4])?, v),
            // i.l, i.h, i.lh: hardware (index-high) i.lh takes i.l from the
            // low half and i.h from the high one
            6 if r & 3 != 0 => {
                let both = r & 3 == 3;
                if r & 1 != 0 {
                    self.index[0] = v & 0xffff;
                }
                if r & 2 != 0 {
                    self.index[1] = if both { v >> 16 } else { v & 0xffff };
                }
                self.written.index |= (r & 3) as u8;
            }
            b => return Err(why(format!("destination bank {b}"))),
        }
        Ok(())
    }

    fn set_r(&mut self, r: usize, v: u32) -> Result<(), Error> {
        self.own_temp(r)?;
        self.r[r] = v;
        self.written.r |= 1 << r;
        self.mark("r", r);
        Ok(())
    }

    /// Temporaries past the task's allocation are other instances' (hardware,
    /// index-width: such reads return the next instances' registers).
    fn own_temp(&self, r: usize) -> Result<(), Error> {
        if r >= self.temps {
            return Err(why(format!("r{r} is past the task's {} temporaries", self.temps)));
        }
        Ok(())
    }

    fn set_pa(&mut self, r: usize, v: u32) {
        self.pa[r] = v;
        self.written.pa |= 1 << r;
        self.mark("pa", r);
    }

    fn set_i(&mut self, r: usize, v: u32) -> Result<(), Error> {
        if r >= 3 {
            // hardware (internal-registers-per-pixel, -bank7): writes to i3-i7
            // are dropped, in no instance of the quad
            if r < 8 {
                return Ok(());
            }
            return Err(why(format!("a write of i{r}")));
        }
        self.i[r] = v;
        self.written.i |= 1 << r;
        self.mark("i", r);
        Ok(())
    }
}

/// The byte a colour factor selects for byte `ch` from sources s0-s2 (see
/// Machine::sop2): zero, s0-s2 (this byte), s0a-s2a (the alpha bytes), asat,
/// sop2's s2scale (s2) and zeros (0), and .comp for 1 - factor.
fn colour_factor(sel: &str, ch: u32, s: [Chans; 3]) -> Result<i64, Error> {
    let c = ch as usize;
    let (name, comp) = sel.strip_suffix(".comp").map_or((sel, false), |n| (n, true));
    let (a1, a2) = (s[1][3], s[2][3]);
    let f = match name {
        "zero" | "zeros" => 0,
        "s0" => s[0][c],
        "s1" => s[1][c],
        "s2" | "s2scale" => s[2][c],
        "s0a" => s[0][3],
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
fn colour_round(t: i64) -> i64 {
    (2 * t + 255).div_euclid(510)
}

/// A colour source's four channel values, 1.0 = 255: its bytes, or with .c10
/// (hardware, colour-c10) the signed 10-bit lanes at bits 0, 10 and 20 clamped
/// to +/-510, the alpha being lane 0.
type Chans = [i64; 4];

fn chans(w: u32, c10: bool) -> Chans {
    if !c10 {
        return [0, 1, 2, 3].map(|c| (w >> (8 * c) & 0xff) as i64);
    }
    let lane = |k: u32| {
        let x = (w >> (10 * k) & 0x3ff) as i64;
        (if x >= 512 { x - 1024 } else { x }).clamp(-510, 510)
    };
    [lane(0), lane(1), lane(2), lane(0)]
}

/// Channel results to a destination: saturated bytes, or with .c10 the
/// three colour results in 10-bit lanes clamped to +/-510, bits 30-31 clear
/// (the alpha result is dropped).
fn colour_out(v: Chans, c10: bool) -> u32 {
    if c10 {
        return (0..3).fold(0, |w, k| w | ((v[k].clamp(-510, 510) as u32) & 0x3ff) << (10 * k));
    }
    (0..4).fold(0, |w, k| w | (v[k].clamp(0, 255) as u32) << (8 * k))
}

/// sop3, lrp1 and lrp2, all four bytes (see Machine::sop3).
fn blend3(op: &str, csel: &str, alpha: (&str, &str), s: [Chans; 3]) -> Result<Chans, Error> {
    let f = |sel: &str, ch: u32| colour_factor(sel, ch, s);
    let mut c: Vec<&str> = csel.split(", ").collect();
    // sop3's alrp form: with neg, -op(s1 * f2, s2 * f1), the factors swapped
    let neg = c.last() == Some(&"neg");
    if neg {
        c.pop();
    }
    let sum = |op: &str, a: i64, b: i64| if op == "sub" { a - b } else { a + b };
    let mut out = [0; 4];
    for ch in 0..3 {
        let (x1, x2) = (s[1][ch as usize], s[2][ch as usize]);
        let t = match (op, &c[..]) {
            ("sop3", [f1, f2, o @ ("add" | "sub")]) if neg => -sum(o, x1 * f(f2, ch)?, x2 * f(f1, ch)?),
            ("sop3", [f1, f2, o @ ("add" | "sub")]) => sum(o, x1 * f(f1, ch)?, x2 * f(f2, ch)?),
            ("lrp1", [f1, f2, t]) if !neg => {
                let w = if *t == "s0" { s[0][ch as usize] } else { s[0][3] };
                f(f1, ch)? * (255 - w) + f(f2, ch)? * w
            }
            ("lrp2", [f1, f2, a, b]) if !neg => {
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
        out[ch as usize] = colour_round(t);
    }
    let (a0, a1, a2) = (s[0][3], s[1][3], s[2][3]);
    let a: Vec<&str> = alpha.1.split(", ").collect();
    let t = match (alpha.0, &a[..]) {
        ("asop", [g1, g2, o]) => sum(o, a1 * f(g1, 3)?, a2 * f(g2, 3)?),
        // arsop g, op: -op(s1a * (the second colour factor), s2a * g); so add
        // is never positive (sop3-lrp, x-arsop)
        ("arsop", [g, o]) => -sum(o, a1 * f(c[1], 3)?, a2 * f(g, 3)?),
        ("alrp", [g1, g2]) => f(g1, 3)? * (255 - a0) + f(g2, 3)? * a0,
        _ => return Err(why(format!("{op} alpha {}", alpha.1))),
    };
    out[3] = colour_round(t);
    Ok(out)
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
fn blend(spec: &str, s1: Chans, s2: Chans, ch: u32) -> Result<i64, Error> {
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
    let factor = |sel: &str| colour_factor(sel, ch, [[0; 4], s1, s2]);
    let (g1, g2) = (factor(f1)?, factor(f2)?);
    let (mut x1, mut x2) = (s1[ch as usize], s2[ch as usize]);
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

/// The ops whose flags carry a repeat count (see Machine::iterations).
const COLOUR_REPEATS: [&str; 8] = ["sop2", "fpma", "idot4", "idot3", "u16dot3", "u8dot3a", "adif", "adifsum"];

/// A half as the f32 of the same value (exact); infinities and NaNs stop.
fn half_f32(h: u32) -> Result<u32, Error> {
    let (e, f) = (h >> 10 & 31, h & 1023);
    if e == 31 {
        return Err(why("a half infinity or NaN coordinate"));
    }
    let mag = if e == 0 { f as f32 * 2f32.powi(-24) } else { (1024 + f) as f32 * 2f32.powi(e as i32 - 25) };
    Ok(if h >> 15 != 0 { -mag } else { mag }.to_bits())
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
        "iadd16" => lanes(|x, y| x + y, true),
        "isub16" => lanes(|x, y| x - y, true),
        "iaddu16" => lanes(|x, y| x + y, false),
        // hardware (test-lanes-other): the multiplies test their full
        // products, before they are cut to the lane
        "imul16" => lanes(|x, y| x * y, true),
        "imulu16" => lanes(|x, y| x * y, false),
        "imul8" => bytes(|x, y| x * y, true),
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
            if flt::special(r) || flt::special(a) || flt::special(b) {
                return Err(why(format!("{op} test of an infinity or NaN")));
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
        fn emit(&mut self, text: &str, srcs: [u32; 3]) -> Result<(), Error> {
            self.1.push(format!("{text} {srcs:x?}"));
            Ok(())
        }
    }

    #[test]
    fn emits_hand_their_sources_to_the_bus() {
        let p = prog(&[
            "mov r0, #0x00000011",
            "mov r1, #0x00000022",
            "emitpix1.freep #0 /* incp */, r0, r1",
            "emitpix2.end.freep #0 /* incp */, r1, r0, r0, #0x00000200",
        ]);
        let mut bus = Regs(Default::default(), vec![]);
        let mut m = Machine::default();
        assert_eq!(m.run(&p, &mut bus, 100), Ok(()));
        assert_eq!(bus.1.len(), 2);
        assert!(bus.1[0].starts_with("emitpix1") && bus.1[0].ends_with("[11, 22, 0]"), "{:?}", bus.1);
        assert!(bus.1[1].ends_with("[22, 11, 11]"), "{:?}", bus.1);
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
    fn smp_samples_through_the_bus_at_the_fence() {
        // a 4x4 B8G8R8A8 stride texture at 0x1000 whose dwords count up
        let src = |smp: &str| {
            prog(&[
                "mov r20, #0x00000600",
                "mov r21, #0xcc003003",
                "mov r22, #0x00001000",
                "mov r23, #0x00000000",
                "mov r24, #0x3f19999a",
                "mov r25, #0x3e99999a",
                smp,
                "wdf drc0",
                "mov.end o0, r10",
            ])
        };
        let mut mem = Memory { base: 0x1000, words: (0..64).map(|i| 0x100 + i).collect() };
        let mut m = Machine::default();
        // (0.6, 0.3): texel (2, 1), dword 4 + 2
        assert_eq!(m.run(&src("smp2d r10, r24, r20, drc0"), &mut mem, 100), Ok(()));
        assert_eq!(m.o[0], 0x106);
        // the result is not there before the fence
        let p = prog(&[
            "mov r20, #0x00000600",
            "mov r21, #0xcc003003",
            "mov r22, #0x00001000",
            "mov r23, #0x00000000",
            "mov r24, #0x3f19999a",
            "mov r25, #0x3e99999a",
            "smp2d r10, r24, r20, drc0",
            "mov.end o0, r10",
        ]);
        let r = Machine::default().run(&p, &mut mem, 100);
        assert!(matches!(r, Err(Error::Unsupported { .. })), "{r:?}");
        // a bus without a texture unit stops it
        struct NoTex;
        impl Bus for NoTex {
            fn load32(&mut self, _: u32) -> Result<u32, Error> {
                Ok(0)
            }
            fn store32(&mut self, _: u32, _: u32) -> Result<(), Error> {
                Ok(())
            }
        }
        let r = Machine::default().run(&src("smp2d r10, r24, r20, drc0"), &mut NoTex, 100);
        assert!(matches!(&r, Err(Error::Unsupported { why, .. }) if why.contains("texture unit")), "{r:?}");
    }

    #[test]
    fn float_functions_give_near_values_only_when_asked() {
        use crate::func::Bound;
        let src = |x: &str| {
            prog(&[
                "mov r1, #0x40800000",
                "mov r3, #0x40400000",
                "mov r6, #0x00000000",
                "mov r7, #0x00001000",
                x,
                "mov r4, r2",
                "fmad r5, r2, r1, r6",
                "stad [r7,+#0], r4",
                "stad [r7,+#1], r5",
                "nop.end",
            ])
        };
        let mut mem = Memory { base: 0x1000, words: vec![0; 64] };
        // frcp of 4 is exact, of 3 within one ulp: refused unless near
        let mut m = Machine::default();
        assert_eq!(m.run(&src("frcp r2, r1"), &mut mem, 100), Ok(()));
        assert_eq!((m.r[4], mem.words[0]), (0x3e80_0000, 0x3e80_0000));
        assert!(m.bounds.is_empty() && m.near_mem.is_empty());
        let mut m = Machine::default();
        let r = m.run(&src("frcp r2, r3"), &mut mem, 100);
        assert!(matches!(r, Err(Error::Unsupported { .. })), "{r:?}");
        // near: the nearest value; a move keeps its bound, anything else
        // computed from it has none
        let mut m = Machine { near: true, ..Machine::default() };
        assert_eq!(m.run(&src("frcp r2, r3.neg"), &mut mem, 100), Ok(()));
        assert_eq!(m.r[2], 0xbeaa_aaab);
        assert_eq!(m.near_mem.get(&0x1000), Some(&Bound::Ulps(1)));
        assert_eq!(m.near_mem.get(&0x1004), Some(&Bound::Derived));
        assert_eq!(m.bounds.get(&("r", 2)), Some(&Bound::Ulps(1)));
        assert_eq!(m.bounds.get(&("r", 4)), Some(&Bound::Ulps(1)));
        assert_eq!(m.bounds.get(&("r", 5)), Some(&Bound::Derived));
        assert_eq!(m.bounds.get(&("r", 1)), None);
    }

    #[test]
    fn fadm_rounds_both_products_then_the_sum() {
        // hardware (fadm-fmsa-fsubflr): a*c + b*c; one rounding of (a + b)*c
        // would give 0x4070d8a5
        let (m, r) = run_after(
            &["mov r1, #0x3fd36766", "mov r2, #0x3fe4f4e3", "mov r3, #0x3f8c0397"],
            &["fadm r4, r1, r2, r3"],
        );
        assert_eq!((r, m.r[4]), (Ok(()), 0x4070_d8a4));
    }

    #[test]
    fn the_integer_multiply_add_formats_and_carries() {
        let m = |init: &[&str], src: &str| {
            let (m, r) = run_after(init, &[src]);
            assert_eq!(r, Ok(()), "{src}");
            m
        };
        // hardware (ima16-8bit): per lane a * b + c, b the high byte as s8,
        // c the low byte as o8
        let i = ["mov r1, #0x82ff0381", "mov r2, #0x7f05fe02", "mov r3, #0x00100020"];
        assert_eq!(m(&i, "ima16.u r10, r1, r2.high, r3.low, s8, o8").r[10], 0xfc11f89e);
        // hardware (ima8): x + y * z per byte, clamped with .sat
        let i = ["mov r1, #0x80ff10f0", "mov r2, #0x10200302", "mov r3, #0x10101080"];
        assert_eq!(m(&i, "ima8.sat r10, r1, r2, r3, s0, s1, s2, s0a, s1a, s2a").r[10], 0xffff40ff);
        let src = "ima8 r10, r1, r2, r3, s0.comp, s1, s2, s0a.comp, s1a, s2a";
        assert_eq!(m(&i, src).r[10], 0x7f001f0f);
        // hardware (imae-modes): z16 zero-extends; coe writes the carry out
        let i = ["mov r1, #0xffff0003", "mov r2, #0xffff0005", "mov r3, #0xfffffff0"];
        assert_eq!(m(&i, "imae.u r10, r1.low, r2.low, r3.high, z16").r[10], 0x0001000e);
        let x = m(&i, "imae.u r10, r1.high, r2.high, r3, u32, coe, i0");
        assert_eq!((x.r[10], x.i[0]), (0xfffdfff1, 1));
        // ima32's cie adds iN
        let i = ["mov r1, #0xfffffff0", "mov r2, #0xffff0005", "mov r3, #0xffffffff", "mov i0, #0x00000001"];
        assert_eq!(m(&i, "ima32.u r10, r1, r2, r3, cie, i0").r[10], 0x000fffb0);
    }

    #[test]
    fn writes_of_i3_to_i7_are_dropped() {
        // hardware (internal-registers, internal-registers-bank7): no
        // instance's i0-i2 change, so one instance alone drops them too
        let (m, r) = run_after(&["mov i0, #0x00000005"], &["mov i3, #0x00000001", "mov i7, #0x00000002"]);
        assert_eq!((r, m.i[0], m.written.i), (Ok(()), 5, 1));
        assert!(run_after(&[], &["mov i8, #0x00000001"]).1.is_err());
    }

    #[test]
    fn pn_never_runs() {
        // hardware (predicate-pn): whatever p0-p3 hold
        let (m, r) = run_after(&["mov r2, #0x00000005"], &["Pn mov r2, #0x00000001"]);
        assert_eq!((r, m.r[2]), (Ok(()), 5));
    }

    #[test]
    fn only_measured_tests_and_flags_run() {
        let i = ["mov r1, #0x00000005", "mov r2, #0x00000003"];
        // hardware (vec-isubu16-taz): isubu16's zero test; its sign tests and
        // the unsigned 8-bit tests are not measured
        assert_eq!(run_after(&i, &["isubu16.testtaz.chan0 r10, p0, r1, r2"]).1, Ok(()));
        for t in ["isubu16.testnaz.chan0 r10, p0, r1, r2", "iaddu8.testtaz.chan0 r10, p0, r1, r2"] {
            assert!(matches!(run_after(&i, &[t]).1, Err(Error::Unsupported { .. })), "{t}");
        }
        // hardware (flags-other): .tpres and .partial change nothing a pixel sees
        let i = ["mov r1, #0x40800000"];
        let (m, r) = run_after(&i, &["frcp.tpres r2, r1", "and.partial r3, r1, r1"]);
        assert_eq!((r, m.r[2], m.r[3]), (Ok(()), 0x3e80_0000, 0x4080_0000));
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
        // .3 of a 16-bit format reads nothing
        assert_eq!(u("unpckf32u16 r10, r1.3", "0x84838281"), f(0.0));
        assert_eq!(u("unpckf32s16.scale r10, r1.3", "0x84838281"), f(0.0));
        assert_eq!(u("unpckf32f16 r10, r1.3", "0x84838281"), f(0.0));
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
        // ties to even: -0.5 * 127 is -63.5, so -64
        assert_eq!(pk("pcks8f32.scale r10, r1.0, r2.0, nearest", -0.5, 0.0), Ok(0x00c000c0));
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
        // the alrp form's neg: -op(s1 * f2, s2 * f1), the factors swapped
        let s = [0, 0xffffffff, 0x80808080];
        assert_eq!(blend(s, "sop3 r10, r1, r2, r3, s1, s2a.comp, sub, neg+alrp zero, zero"), Ok(0x00010101));
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
        // .1 is bits 8-23: 0x003c00 there is 1.0
        assert_eq!(run3([0x003c0000, v, 0], "movmsk.flt16 r10, r2, r1.1"), Ok(v));
        // movmsk-smsk: u8 and flt also need bit 0 of s0; mask ignores s0
        assert_eq!(run3([2, v, 0x8000], "movmsk.smsk.u8 r10, r1, r2, r3.1"), Ok(0xa0a1a2a3));
        assert_eq!(run3([3, v, 0x8000], "movmsk.smsk.u8 r10, r1, r2, r3.1"), Ok(v));
        assert_eq!(run3([2, v, 0x100], "movmsk.mask.smsk r10, r1, r2, r3.1"), Ok(v));
    }

    #[test]
    fn index_registers_offset_register_numbers() {
        let regs: Vec<String> = (0..8).map(|n| format!("mov r{}, #0x{:08x}", 30 + n, 0x30 + n)).collect();
        let regs: Vec<&str> = regs.iter().map(|s| s.as_str()).collect();
        let r10 = |src: &[&str]| {
            let (m, r) = run_after(&regs, src);
            r.map(|_| m.r[10])
        };
        // hardware (index-unit, index-width3): r[il + #n] is r(il + n), bits
        // 10 and up of i.l ignored
        assert_eq!(r10(&["mov r1, #0x0000001e", "mov i.l, r1", "mov r10, r[il + #3]"]), Ok(0x33));
        assert_eq!(r10(&["mov r1, #0xfffffc20", "mov i.l, r1", "mov r10, r[il + #0]"]), Ok(0x32));
        // index-high: i.lh sets i.l from the low half and i.h from the high one
        assert_eq!(r10(&["mov r1, #0x001f001e", "mov i.lh, r1", "mov r10, r[ih + #0]"]), Ok(0x31));
        // index-store: the destination form mirrors the source one
        assert_eq!(
            r10(&[
                "mov r1, #0x0000001e",
                "mov r2, #0x12345678",
                "mov i.h, r1",
                "mov r[ih + #1], r2",
                "mov r10, r31"
            ]),
            Ok(0x12345678)
        );
        // an index register the task has not written stops
        assert!(r10(&["mov r10, r[il + #0]"]).is_err());
        // registers past the task's temporaries belong to other instances
        let mut m = Machine { temps: 32, ..Machine::default() };
        let r =
            m.run(&prog(&["mov r33, #0x00000001", "nop.end"]), &mut Memory { base: 0, words: vec![] }, 10);
        assert!(matches!(r, Err(Error::Unsupported { why, .. }) if why.contains("temporaries")));
    }

    #[test]
    fn the_integer_dot_products() {
        let dot = |s: [u32; 3], text: &str| {
            let (m, r) = run_after(
                &[
                    &format!("mov r1, #0x{:08x}", s[0]),
                    &format!("mov r2, #0x{:08x}", s[1]),
                    &format!("mov r9, #0x{:08x}", s[2]),
                ],
                &[text],
            );
            r.map(|_| m.r[10])
        };
        let (a, b) = (0x05060708, 0x10203040);
        // hardware (intdot-lanes): sum(a * b) * Cx / 255 rounded, saturated,
        // in bytes 0-2, the alpha byte by Ax; off reads each byte as x - 128
        assert_eq!(dot([a, b, 0], "u8dot4 r10, Cx2, Ax8, r1, r2"), Ok(0x23090909));
        assert_eq!(dot([0x80808080, 0x7f7f7f7f, 0], "u8dot4 r10, Cx1, Ax1, r1.comp, r2"), Ok(0xfdfdfdfd));
        assert_eq!(dot([0x05060708, 0x10203040, 0], "u8dot4off r10, Cx1, Ax1, r1, r2"), Ok(0xa8a8a8a8));
        // the 16-bit result is x * 32767 / 65025, saturated at +/-32767, twice
        assert_eq!(dot([0, 0xffffffff, 0], "u16dot4off r10, Cx1, Ax1, r1, r2"), Ok(0x80028002));
        assert_eq!(dot([0, 0xffffffff, 0], "u16dot3off r10, Cx1, r1, r2"), Ok(0xa001a001));
        // intdot-alpha: aadd/asub of two products of the source alphas, by Ax;
        // aintrp1 lerps g1 to g2 by s0a, aintrp2 1 - g1 to g2
        let (a, b) = (0xff000000, 0x80000000);
        assert_eq!(
            dot([a, b, 0], "u8dot3 r10, Cx1, r1, r2+aadd Ax2, s1a, s1a.comp, s2a.comp, s2a"),
            Ok(0x7f000000)
        );
        assert_eq!(dot([a, b, 0x40112233], "u8dot3 r10, Cx1, r1, r2+aintrp1 r9, s1a, s2a"), Ok(0xdf000000));
        assert_eq!(dot([a, b, 0x40112233], "u8dot3 r10, Cx1, r1, r2+aintrp2 r9, s1a, s2a"), Ok(0x20000000));
    }

    #[test]
    fn efo_runs_its_dataflow() {
        let init = [
            "mov r1, #0x40400000", // 3
            "mov r2, #0x40a00000", // 5
            "mov r3, #0x40e00000", // 7
            "mov r4, #0x41300000", // 11
            "mov r5, #0x41500000", // 13
            "mov i0, r4",
            "mov i1, r5",
        ];
        let f = |x: f32| x.to_bits();
        // hardware (efo-dataflow): m0 = 3 * 7, m1 = 5 * 7, a0 = m0 + m1,
        // a1 = i1 + i0; d reads the internals before this instruction's writes
        let (m, r) = run_after(
            &init,
            &[
                "efo r10= a0, i0 = a0, i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3",
                "efo r11= a1, !i0 = a0, !i1 = a1, a0=m0+m1, a1=i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3",
                "efo r12= i0, i0 = a0, !i1 = a1, a0=m0-m1, a1=-i1+i0, m0=src0*src2, m1=src1*src2, r1, r2, r3",
            ],
        );
        assert_eq!((r, m.r[10], m.r[11], m.r[12]), (Ok(()), f(56.0), f(80.0), f(56.0)));
        assert_eq!((m.i[0], m.i[1]), (f(-14.0), f(24.0)));
    }

    #[test]
    fn fmad16_works_on_half_lanes() {
        let mad = |s: [u32; 3], text: &str| {
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
        // hardware (fmad16-specials): an infinity stays, an invalid result
        // is 0x7fff: (inf, 0) * (2, inf) + (1, 1), (NaN, -inf) * 1 + (1, inf)
        assert_eq!(mad([0x7c00_0000, 0x4000_7c00, 0x3c00_3c00], "fmad16 r10, r1, r2, r3"), Ok(0x7c00_7fff));
        assert_eq!(mad([0x7e00_fc00, 0x3c00_3c00, 0x3c00_7c00], "fmad16 r10, r1, r2, r3"), Ok(0x7fff_7fff));
        let s = [0x3c004000, 0x40004200, 0x3c003c00]; // (2, 1), (3, 2), (1, 1)
        // hardware (fmad16-lanes): a * b + c per half; .swzll reads the low
        // half twice; .neg/.abs per source
        assert_eq!(mad(s, "fmad16 r10, r1, r2, r3"), Ok(0x42004700));
        assert_eq!(mad(s, "fmad16 r10, r1.swzll, r2, r3"), Ok(0x45004700));
        assert_eq!(mad(s, "fmad16 r10, r1, r2.neg, r3"), Ok(0xbc00c500));
        // fmad16-rounding: one rounding of the exact a * b + c
        assert_eq!(mad([0x42c80000, 0xbf400000, 0x477fe000], "fmad16 r10, r1, r2, r3"), Ok(0x3d67e000));
        // .swzf32 (fmad16 page): the whole register as a binary32, exactly:
        // 1.5 + 2^-20 times (3, 2) plus (1, 1) rounds to (5.5, 4)
        assert_eq!(
            mad([0x3fc00008, 0x40004200, 0x3c003c00], "fmad16 r10, r1.swzf32, r2, r3"),
            Ok(0x44004580)
        );
    }

    #[test]
    fn write_masks_write_consecutive_registers() {
        let regs =
            ["mov r1, #0x11121314", "mov r2, #0x21222324", "mov r3, #0x31323334", "mov r4, #0x41424344"];
        let pre =
            ["mov r10, #0x000000a0", "mov r11, #0x000000a1", "mov r12, #0x000000a2", "mov r13, #0x000000a3"];
        let run4 = |op: &str| {
            let (m, r) = run_after(&[&regs[..], &pre[..]].concat(), &[op]);
            r.map(|_| [m.r[10], m.r[11], m.r[12], m.r[13]])
        };
        // hardware (wmask-mov, wmask-or): component k writes d + k from s + k
        assert_eq!(run4("mov r10.yw, r1"), Ok([0xa0, 0x21222324, 0xa2, 0x41424344]));
        assert_eq!(run4("and r10.xz, r1, #0x000000ff"), Ok([0x14, 0xa1, 0x34, 0xa3]));
        assert_eq!(run4("mov r10., r1"), Ok([0xa0, 0xa1, 0xa2, 0xa3]));
    }

    #[test]
    fn half_float_packs_and_unpacks() {
        let pk = |a: u32, b: u32, op: &str| {
            let (m, r) = run_after(&[&format!("mov r1, #0x{a:08x}"), &format!("mov r2, #0x{b:08x}")], &[op]);
            r.map(|_| m.r[10])
        };
        // hardware (pack-f16): truncated toward zero whatever the mode,
        // saturated at 65504, half denormals kept; s1 low, s2 high
        assert_eq!(pk(0x3f801000, 0x3f803000, "pckf16f32 r10, r1.0, r2.0, nearest"), Ok(0x3c013c00));
        assert_eq!(pk(0x4788b800, 0x33d6bf95, "pckf16f32 r10, r1.0, r2.0, nearest"), Ok(0x00017bff));
        assert_eq!(pk(0xbfd9999a, 0x33000000, "pckf16f32 r10, r1.0, r2.0, up"), Ok(0x0000becc));
        // .renable rounds to an integer by the mode first
        assert_eq!(pk(0x40200000, 0xc0600000, "pckf16f32.renable r10, r1.0, r2.0, nearest"), Ok(0xc4004000));
        assert_eq!(pk(0xbfc00000, 0x3fc00000, "pckf16f32.renable r10, r1.0, r2.0, down"), Ok(0x3c00c000));
        // unpack-f16: exact, infinities kept, a NaN as 0x7fffffff
        assert_eq!(pk(0x7bff03ff, 0, "unpckf32f16 r10, r1.0, r1.0, nearest"), Ok(0x387fc000));
        assert_eq!(pk(0xfc007c00, 0, "unpckf32f16 r10, r1.2, r1.0, nearest"), Ok(0xff800000));
        assert_eq!(pk(0x00017e00, 0, "unpckf32f16 r10, r1.0, r1.0, nearest"), Ok(0x7fffffff));
    }

    #[test]
    fn fpma_adds_a_product_per_byte() {
        let pma = |s: [u32; 3], text: &str| {
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
        let s = [0x80808080, 0x7f7f7f7f, 0x01ff00fe];
        // hardware (fpma): the first selection plus the product of the other
        // two over 255, rounded half up, saturated; alpha by its own three
        assert_eq!(pma(s, "fpma r10, r1, r2, r3, s0, s1, s2, s0a, s1a, s2a"), Ok(0x80ff80ff));
        assert_eq!(pma(s, "fpma.sat r10, r1, r2, r3, s0.comp, s1, s2, s0a, s1a, s2a"), Ok(0x80fe7ffe));
        // .neg on source 0 subtracts the first selection
        assert_eq!(pma(s, "fpma r10, r1.neg, r2, r3, s0, s1, s2, s0a, s1a, s2a"), Ok(0x00000000));
        // fpma on .c10 lanes (fpma-sat)
        assert_eq!(pma(s, "fpma r10.c10, r1.c10, r2.c10, r3.c10, s0, s1, s2, s0a, s1a, s2a"), Ok(0x0070a000));
    }

    #[test]
    fn rlp_rotates_through_a_predicate() {
        let clear = ["mov r7, #0x00000000", "and.testtanz.chan0 !c0, p0, r7, r7"];
        // hardware (rlp): d = s << 1 | pN, then pN = bit 31 of s
        let (m, r) = run_after(&[&clear[..], &["mov r1, #0x80000001"]].concat(), &["rlp r10, r1, p0"]);
        assert_eq!((r, m.r[10], m.p[0]), (Ok(()), 2, true));
        let (m, r) = run_after(
            &[&clear[..], &["mov r1, #0xffffffff"]].concat(),
            &["rlp r10, r1, p0", "rlp r11, r1, p0"],
        );
        assert_eq!((r, m.r[10], m.r[11]), (Ok(()), 0xfffffffe, 0xffffffff));
        // rlp-high-predicates: numbers from 4 up alias p0-p3 (mod 4): p5 is p1
        let (m, r) = run_after(
            &[&clear[..], &["mov r1, #0x80000000", "mov r2, #0x00000000"]].concat(),
            &["rlp r10, r1, p5", "rlp r15, r2, p5"],
        );
        assert_eq!((r, m.r[10], m.r[15], m.p[1]), (Ok(()), 0, 1, false));
    }

    #[test]
    fn statomic_packs_clamped_signed_bytes() {
        let (m, r) = run_after(
            &["mov r1, #0xf14ed442", "mov r2, #0x65283b7d", "mov r3, #0x535b6075"],
            &["statomic r10, r1.1, r2.2, up", "statomic.scale r11, r3, r2, zero"],
        );
        // hardware (statomic): bytes s1.c, s2.c, s1.c, s2.c, each read as
        // signed and clamped at 0; the mode and .scale change nothing
        assert_eq!((r, m.r[10], m.r[11]), (Ok(()), 0x28002800, 0x7d757d75));
    }

    #[test]
    fn c10_operands_are_signed_10_bit_lanes() {
        let op = |a: u32, b: u32, text: &str| {
            let (m, r) =
                run_after(&[&format!("mov r1, #0x{a:08x}"), &format!("mov r2, #0x{b:08x}")], &[text]);
            r.map(|_| m.r[10])
        };
        // hardware (colour-c10): lanes at bits 0, 10, 20, 1.0 = 255, clamped to
        // +/-510; the alpha of a c10 source is lane 0; a c10 destination
        // takes the three colour results, an 8-bit one saturates
        let plain = "zero.comp, zero, add+asop2 zero.comp, zero, add";
        assert_eq!(op(0x3fffffff, 0, &format!("sop2 r10.c10, r1, r2, {plain}")), Ok(0x0ff3fcff));
        assert_eq!(op(0x3fffffff, 0, &format!("sop2 r10, r1.c10, r2, {plain}")), Ok(0));
        let both = "zero.comp, zero.comp, add+asop2 zero.comp, zero.comp, add";
        assert_eq!(
            op(0x3fffffff, 0x00080200, &format!("sop2 r10.c10, r1.c10, r2.c10, {both}")),
            Ok(0x3ff80a02)
        );
    }

    #[test]
    fn c10_and_integer_packs() {
        let pk = |a: u32, b: u32, op: &str| {
            let (m, r) = run_after(&[&format!("mov r1, #0x{a:08x}"), &format!("mov r2, #0x{b:08x}")], &[op]);
            r.map(|_| m.r[10])
        };
        // hardware (pack-c10): x * 255 to nearest even in lanes s1, s2, s1
        assert_eq!(pk(0x3f000000, 0xbf000000, "pckc10f32 r10, r1.0, r2.0, nearest"), Ok(0x080e0080));
        // unpack-c10: lane c / 255, the lanes clamped to +/-510
        assert_eq!(pk(0xe007f801, 0, "unpckf32c10 r10, r1.2, r1.0, nearest"), Ok(0xc0000000));
        // unpack-c10-16: three bits of lane 0 of source 1, nothing converted
        assert_eq!(pk(0x19a24d55, 0, "unpcku16c10 r10, r1.0, r1.1, nearest"), Ok(0x0000ffff));
        assert_eq!(pk(0x266fc109, 0, "unpcks16c10 r10, r1.0, r1.1, nearest"), Ok(0x00000001));
        // unpack-*: unpcku8s8 makes a negative 0x7f, unpcku16s8 0;
        // unpcks16s8 sign-extends; values laid out as packs do
        assert_eq!(pk(0x807f0100, 0, "unpcku8s8 r10, r1.3, r1.1, nearest"), Ok(0x017f017f));
        assert_eq!(pk(0x807f0100, 0, "unpcku16s8 r10, r1.3, r1.2, nearest"), Ok(0x007f0000));
        assert_eq!(pk(0x807f0100, 0, "unpcks16s8 r10, r1.3, r1.2, nearest"), Ok(0x007fff80));
    }

    #[test]
    fn float_specials() {
        let (inf, nan, one, max) = (0x7f800000, 0x7fc00001, 0x3f800000, 0x7f7fffff);
        // hardware (fmad-move, fdp-specials): infinities stay, an invalid
        // result or a NaN input gives 0x7fc00000, an overflow +inf
        assert_eq!(flt::mul(inf, 0), Ok(0x7fc00000));
        assert_eq!(flt::add(Ok(nan), one), Ok(0x7fc00000));
        assert_eq!(flt::mul(max, 0x40000000), Ok(inf));
        // fminmax-specials, minmax-allnan: minNum/maxNum, -0 below +0, of two
        // NaNs the second
        assert_eq!(flt::min(nan, one), Ok(one));
        assert_eq!(flt::max(0x80000000, 0), Ok(0));
        assert_eq!(flt::min(nan, 0x7fc00002), Ok(0x7fc00002));
    }

    #[test]
    fn ima16_neg_subtracts_source_2() {
        // hardware (ima16): 0x00020005 * #3 - 0x00010007 is 0x00050008
        let (m, r) = run_after(
            &["mov r1, #0x00020005", "mov r2, #0x00010007"],
            &["ima16.s r10, r1, #3, r2.neg, u16, u16"],
        );
        assert_eq!((r, m.r[10]), (Ok(()), 0x00050008));
    }

    #[test]
    fn integer_multiply_tests_and_adif() {
        // hardware (test-lanes-other): imul16 tests the full product, so
        // 0x100 * 0x100 writes 0 but does not test as zero
        let (m, r) = run_after(&["mov r1, #0x00000100"], &["imul16.testtaz.chan0 r10, p0, r1, r1"]);
        assert_eq!((r, m.r[10] & 0xffff, m.p[0]), (Ok(()), 0, false));
        // video-adif: |a - b| per byte; adifsum adds the four and a half of s0
        let (m, r) = run_after(
            &["mov r1, #0x10203040", "mov r2, #0x30201008", "mov r3, #0x00050007"],
            &["adif r10, r1, r2", "adifsum r11, r3.high, r1, r2"],
        );
        assert_eq!((r, m.r[10], m.r[11]), (Ok(()), 0x20002038, 0x20 + 0x20 + 0x38 + 5));
    }

    #[test]
    fn dual_issue_runs_two_operations() {
        let init = [
            "mov r1, #0x3fc00000", // 1.5
            "mov r2, #0x40100000", // 2.25
            "mov r3, #0x40400000", // 3
            "mov r4, #0x40a00000", // 5
            "mov r5, #0x40e00000", // 7
            "mov r6, #0x41300000", // 11
            "mov i0, r4",
            "mov i1, r5",
            "mov i2, r6",
        ];
        let f = |x: f32| x.to_bits();
        // hardware (dual-issue): the primary as alone, the secondary on i0-i2
        let (m, r) = run_after(&init, &["fmad r10, r1, r2, r3+fmad i0, i0, i1, i2"]);
        assert_eq!((r, m.r[10], m.i[0]), (Ok(()), f(6.375), f(46.0)));
        let (m, r) = run_after(&init, &["mov r10, r1+fmsa i0, i0, i1, i2"]);
        assert_eq!((r, m.r[10], m.i[0]), (Ok(()), f(1.5), f(102.0)));
        // a register source is the primary's source in that position; both
        // read before either writes
        let (m, r) = run_after(&init, &["fmad i0, r1, r2, r3+fadd i1, r2, i0"]);
        assert_eq!((r, m.i[0], m.i[1]), (Ok(()), f(6.375), f(7.25)));
        // pairs that share a unit (dual-issue-shared): an fmsa secondary
        // writes the primary's result, a multiplying secondary of a
        // two-product primary gets its second product
        let (m, r) = run_after(&init, &["fmad r10, r1, r2, r3+fmsa i0, i0, i1, i2"]);
        assert_eq!((r, m.r[10], m.i[0]), (Ok(()), f(6.375), f(6.375)));
        // fadm: a*c + b*c
        let (m, r) = run_after(&init, &["fadm r10, r1, r2, r3+fadd i1, i1, i2"]);
        assert_eq!((r, m.r[10], m.i[1]), (Ok(()), f(11.25), f(18.0)));
        let (m, r) = run_after(&init, &["fadm r10, r1, r2, r3+fmul i1, i1, i2"]);
        assert_eq!((r, m.i[1]), (Ok(()), f(6.75)));
        // a single-source secondary
        let (m, r) = run_after(&[&init[..], &["mov r7, #0x40800000"]].concat(), &["mov r10, r7+frcp i0, r7"]);
        assert_eq!((r, m.i[0]), (Ok(()), f(0.25)));
        // hardware (dual-issue): .repeat2 repeats both, the primary's operands
        // advancing, the secondary again on the internal registers
        let (m, r) = run_after(&init, &["fmad.repeat2 r10, r1, r2, r3+fadd i0, i0, i1"]);
        assert_eq!((r, m.r[10], m.r[11], m.i[0]), (Ok(()), f(6.375), f(11.75), f(19.0)));
        // fmad16 secondaries run on half lanes (dual-issue-shared: next to
        // fmad the primary gives 0): (2, 1) * (3, 2) + (1, 1)
        let h = [
            "mov i0, #0x3c004000",
            "mov i1, #0x40004200",
            "mov i2, #0x3c003c00",
            "mov r1, #0x3fc00000",
            "mov r2, #0x40100000",
            "mov r3, #0x40400000",
        ];
        let (m, r) = run_after(&h, &["mov r10, r1+fmad16 i0, i0, i1, i2"]);
        assert_eq!((r, m.r[10], m.i[0]), (Ok(()), f(1.5), 0x42004700));
        let (m, r) = run_after(&h, &["fmad r10, r1, r2, r3+fmad16 i0, i0, i1, i2"]);
        assert_eq!((r, m.r[10], m.i[0]), (Ok(()), 0, 0x42004700));
        // fminmax + ima32 is not settled
        assert!(run_after(&init, &["fminmax r10, r1, r2, r3+ima32 i0, i0, i1, i2"]).1.is_err());
    }

    #[test]
    fn colour_and_dot_repeats_advance_every_operand() {
        let init = ["mov r1, #0xaa170513", "mov r2, #0xe433837f", "mov r3, #0x23e66c17"];
        // hardware (colour-dot-repeats): each iteration on the next registers
        let (m, r) = run_after(&init, &["sop2.repeat2 r10, r1, r2, s1, s2, add+asop2 s1a, s2a, add"]);
        assert_eq!((r, m.r[10], m.r[11]), (Ok(()), 0xff0c4341, 0xd1da7141));
        let (m, r) = run_after(&init, &["u8dot3.repeat2 r10, Cx1, Ax1, r1, r2"]);
        assert_eq!((r, m.r[10], m.r[11]), (Ok(()), 0x11111111, 0x71717171));
    }

    #[test]
    fn a_quad_shares_i3_and_the_derivatives() {
        let p = prog(&[
            "mov i0, r1",
            "mov i2, r1",
            "mov r2, i3",
            "fdsx r3, r1, r1",
            "fdsy r4, r1, r1",
            "mov r5, i7",
            "nop.end",
        ]);
        let mut quad: [Machine; 4] = Default::default();
        for (k, m) in quad.iter_mut().enumerate() {
            m.r[1] = (10.0 * (k + 1) as f32).to_bits();
            m.written.r |= 2;
        }
        let mut mem = Memory { base: 0, words: vec![] };
        assert_eq!(run_quad(&mut quad, &p, &mut mem, 100), Ok(()));
        let f = |x: f32| x.to_bits();
        for (k, m) in quad.iter().enumerate() {
            // hardware (internal-registers-per-pixel): i3 is the next
            // instance's i0 (TL, TR, BL, BR, round again)
            assert_eq!(m.r[2], quad[(k + 1) % 4].r[1], "i3 of {k}");
            // fds-quad: TR - TL and BL - TL, the same in all four
            assert_eq!((m.r[3], m.r[4]), (f(10.0), f(20.0)), "fds of {k}");
            // internal-registers-bank7: i7 is i2 of the next instance
            assert_eq!(m.r[5], quad[(k + 1) % 4].r[1], "i7 of {k}");
        }
        // one instance alone cannot say
        let (_, r) = run(&["mov r1, #0x00000000", "fdsx r3, r1, r1"]);
        assert!(r.is_err());
    }

    #[test]
    fn efo_gpiext_reaches_across_the_quad() {
        // each instance k: i0 = k + 1, i1 = 10 (k + 1), i2 = 100 (k + 1), r1 = 1
        let p = prog(&[
            "efo.gpiext r10= a0, i0 = a0, !i1 = a1, a0=m0+m1, a1=i1+i0, m0=src1*i0, m1=src2*i1, r1, r1, r1",
            "mov r11, i2",
            "efo.gpiext r12= a1, !i0 = a0, i1 = a1, a0=m0+i0, a1=i1+m1, m0=src0*src2, m1=src1*src2, r1, r1, r1",
            "nop.end",
        ]);
        let f = |x: f32| x.to_bits();
        let mut quad: [Machine; 4] = Default::default();
        for (k, m) in quad.iter_mut().enumerate() {
            let n = (k + 1) as f32;
            m.i = [f(n), f(10.0 * n), f(100.0 * n), 0];
            m.written.i = 7;
            m.r[1] = f(1.0);
            m.written.r |= 2;
        }
        let mut mem = Memory { base: 0, words: vec![] };
        assert_eq!(run_quad(&mut quad, &p, &mut mem, 100), Ok(()));
        for (k, m) in quad.iter().enumerate() {
            let n = (k + 1) as f32;
            // hardware (efo-gpiext-quad): the multipliers read own i2 for i0
            // and own i0 for i1; a write of i0 goes to own i2
            assert_eq!((m.r[10], m.r[11]), (f(101.0 * n), f(101.0 * n)), "instance {k}");
            assert_eq!(m.r[12], f(10.0 * n + 1.0));
        }
        // the four i1 writes end in the top-right i0, as the bottom-right's
        assert_eq!(quad[1].i[0], f(41.0));
        assert_eq!((quad[0].i[0], quad[2].i[0], quad[3].i[0]), (f(1.0), f(3.0), f(4.0)));
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
        // hardware (smlsi-three-sources): on three sources increment 1 is
        // source 0's, 2 source 1's and 3 source 2's: r1 * r3 + r5, then
        // r3 * r4 + r6
        let (m, r) =
            run_after(&ABC, &[&format!("smlsi #1, #2, #1, #1, {SMLSI}"), "fmad.repeat2 r10, r1, r3, r5"]);
        let f = |x: f32| x.to_bits();
        assert_eq!((r, m.r[10], m.r[11]), (Ok(()), f(105.0), f(235.0)));
    }
}

/// The float datapath, as the hardware vectors show it: f32 operations
/// rounded to nearest even (Rust's f32 add and mul are correctly rounded),
/// denormal inputs and results flushed to a zero of the same sign, infinities
/// kept and NaN results 0x7fc00000.
mod flt {
    use super::Error;

    pub fn modify(v: u32, neg: bool, abs: bool) -> u32 {
        let v = if abs { v & 0x7fff_ffff } else { v };
        if neg { v ^ 0x8000_0000 } else { v }
    }

    /// An operand: a denormal flushes to a zero of its sign; infinities and
    /// NaNs pass (hardware, fmad-move, fdp-specials, fadm-fmsa-fsubflr).
    fn input(v: u32) -> Result<f32, Error> {
        Ok(f32::from_bits(if v >> 23 & 0xff == 0 { v & 0x8000_0000 } else { v }))
    }

    /// A result: denormals flush, an infinity stays, and every NaN is
    /// 0x7fc00000 (hardware: fmad of a NaN, inf * 0, inf - inf).
    fn output(x: f32) -> Result<u32, Error> {
        if x.is_nan() {
            return Ok(0x7fc0_0000);
        }
        let b = x.to_bits();
        Ok(if b >> 23 & 0xff == 0 { b & 0x8000_0000 } else { b })
    }

    /// Whether a value is an infinity or a NaN (for the ops whose specials
    /// are not measured).
    pub fn special(v: u32) -> bool {
        v >> 23 & 0xff == 0xff
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

    /// IEEE minNum/maxNum (hardware, fminmax-specials, minmax-allnan): a NaN
    /// loses to a number, -0 is below +0, and of two NaNs the second stays
    /// with its payload.
    fn minmax(a: u32, b: u32, max: bool) -> Result<u32, Error> {
        let nan = |v: u32| v >> 23 & 0xff == 0xff && v & 0x7f_ffff != 0;
        let flush = |v: u32| if v >> 23 & 0xff == 0 { v & 0x8000_0000 } else { v };
        let (a, b) = (flush(a), flush(b));
        Ok(match (nan(a), nan(b)) {
            (_, true) if nan(a) => b,
            (true, false) => b,
            (false, true) => a,
            _ => {
                // order as numbers, -0 below +0: compare the total order key
                let key = |v: u32| if v >> 31 != 0 { !v as i64 } else { v as i64 | 1 << 32 };
                if (key(a) < key(b)) == max { b } else { a }
            }
        })
    }

    pub fn min(a: u32, b: u32) -> Result<u32, Error> {
        minmax(a, b, false)
    }

    pub fn max(a: u32, b: u32) -> Result<u32, Error> {
        minmax(a, b, true)
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

/// Half floats, as fmad16 computes them: hardware (fmad16-lanes,
/// fmad16-rounding, fmad16-specials, x-fmad16) rounds the product to binary32
/// and a * b + c to a half, both to nearest even (one rounding for two
/// halves, whose products fit a binary32), keeps half denormals and gives
/// zeros IEEE signs; a .swzf32 source is its binary32 value. Infinities and
/// NaNs are not modelled.
mod half {
    use super::{Error, why};

    /// An exact value m * 2^e, and the sign of a zero; or a half infinity
    /// or NaN (special).
    #[derive(Clone, Copy)]
    pub struct Exact {
        m: i128,
        e: i32,
        neg: bool,
        special: Option<f32>,
    }

    impl Exact {
        fn value(&self) -> f64 {
            self.special.map_or(self.m as f64 * 2f64.powi(self.e), |s| s as f64)
        }
    }

    pub fn from_half(h: u16) -> Result<Exact, Error> {
        let (e, f) = ((h >> 10 & 31) as i32, (h & 1023) as i128);
        let neg = h >> 15 != 0;
        let m = match e {
            31 => {
                let s = if f != 0 {
                    f32::NAN
                } else if neg {
                    f32::NEG_INFINITY
                } else {
                    f32::INFINITY
                };
                return Ok(Exact { m: 0, e: 0, neg, special: Some(s) });
            }
            0 => f,
            _ => 1024 + f,
        };
        Ok(Exact { m: if neg { -m } else { m }, e: if e == 0 { -24 } else { e - 25 }, neg, special: None })
    }

    /// A .swzf32 source; its denormals, infinities and NaNs are undecided.
    pub fn from_f32(b: u32) -> Result<Exact, Error> {
        let (e, f) = ((b >> 23 & 0xff) as i32, (b & 0x7f_ffff) as i128);
        if e == 0xff || (e == 0 && f != 0) {
            return Err(why("fmad16 .swzf32 of a denormal, infinity or NaN"));
        }
        let neg = b >> 31 != 0;
        let m = if e == 0 { 0 } else { (1 << 23) + f };
        Ok(Exact { m: if neg { -m } else { m }, e: e - 150, neg, special: None })
    }

    /// m * 2^e rounded to 24 significant bits, to nearest even (a binary32
    /// product; one outside the normal range stops).
    fn to_f32(m: i128, e: i32) -> Result<(i128, i32), Error> {
        let a = m.unsigned_abs();
        let bits = 128 - a.leading_zeros() as i32;
        if a == 0 || bits <= 24 {
            return Ok((m, e));
        }
        let shift = (bits - 24) as u32;
        let (n, r) = (a >> shift, a & ((1u128 << shift) - 1));
        let half = 1u128 << (shift - 1);
        let n = n + (r > half || (r == half && n & 1 != 0)) as u128;
        let e = e + shift as i32;
        if !(-149..=104).contains(&(e + 23)) {
            return Err(why("fmad16 product outside the binary32 range (not modelled)"));
        }
        Ok((if m < 0 { -(n as i128) } else { n as i128 }, e))
    }

    pub fn fma(a: Exact, b: Exact, c: Exact) -> Result<u16, Error> {
        // hardware (fmad16-specials): with an infinity or NaN operand, an
        // infinity stays and an invalid result is the half NaN 0x7fff
        if a.special.is_some() || b.special.is_some() || c.special.is_some() {
            let r = a.value() * b.value() + c.value();
            return Ok(if r.is_nan() { 0x7fff } else { if r < 0.0 { 0xfc00 } else { 0x7c00 } });
        }
        // hardware (x-fmad16, fmad16-lanes): the product rounded to binary32,
        // then the sum to a half (fused for two halves, whose products fit)
        let (pm, pe) = to_f32(a.m * b.m, a.e + b.e)?;
        let p = Exact { m: pm, e: pe, neg: a.neg != b.neg, special: None };
        // align to the smaller exponent (the spans fit an i128 for every
        // finite half and binary32)
        let e = p.e.min(c.e);
        let (dp, dc) = ((p.e - e) as u32, (c.e - e) as u32);
        if dp > 60 || dc > 60 {
            return Err(why("fmad16 terms too far apart (not modelled)"));
        }
        let s = (p.m << dp) + (c.m << dc);
        if s == 0 {
            // an exact zero: -0 only when both addends are negative zeros
            return Ok(if p.m == 0 && p.neg && c.m == 0 && c.neg { 0x8000 } else { 0 });
        }
        let sign = if s < 0 { 0x8000 } else { 0 };
        let m = s.unsigned_abs();
        let lg = 127 - m.leading_zeros() as i32 + e; // floor(log2(value))
        // the quantum: 2^-24 for denormals, else 2^(lg - 10)
        let q = if lg < -14 { -24 } else { lg - 10 };
        let n = if q >= e {
            let shift = (q - e) as u32;
            if shift >= 127 {
                0
            } else {
                let (n, r) = (m >> shift, m & ((1u128 << shift) - 1));
                let half = if shift == 0 { 0 } else { 1u128 << (shift - 1) };
                n + (shift > 0 && (r > half || (r == half && n & 1 != 0))) as u128
            }
        } else {
            m << (e - q) as u32
        };
        if lg < -14 {
            // a denormal; 1024 is the smallest normal, encoded alike
            return Ok(sign | n as u16);
        }
        let (mut n, mut lg) = (n, lg);
        if n == 2048 {
            n = 1024;
            lg += 1;
        }
        if lg > 15 {
            return Ok(sign | 0x7c00);
        }
        Ok(sign | ((lg + 15) as u16) << 10 | (n - 1024) as u16)
    }
}
