// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The USSE interpreter.

use std::fmt;
use usse_isa::{Core, Ctx, Form, decode_fields};

/// Memory and the SGX registers, as the program sees them.
pub trait Bus {
    fn load32(&mut self, addr: u32) -> Result<u32, Error>;
    fn store32(&mut self, addr: u32, v: u32) -> Result<(), Error>;
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
        if !matches!(op, "br" | "ba") && !op.starts_with("ld") && !op.starts_with("st") {
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
        let next = match op {
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
                if !flags.is_empty() {
                    return Err(why(format!("branch flags {flags}")));
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
            "ld" | "ld.pre" | "ld.post" | "st" | "st.pre" | "st.post" => {
                self.load_store(insn, op, bus)?;
                None
            }
            _ => return Err(why(format!("op {op} not modelled yet"))),
        };
        Ok(match (next, end) {
            (_, true) => Next::End,
            (Some(t), false) => Next::Jump(t),
            (None, false) => Next::Step,
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
        let count = 1;
        for f in flags.split('.').filter(|f| !f.is_empty()) {
            match f {
                "skipinv" | "syncs" | "nosched" | "bpcache" | "fcfill" | "bypassl1" => {}
                // Hardware: ldad.fetch4 r30 left r31 unchanged, so the fetch
                // does not fill consecutive registers; what it fills is open.
                _ if f.starts_with("fetch") => return Err(why(format!("flag .{f} (destinations open)"))),
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
            let r = (d & 0x7f) as usize + k as usize;
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

    /// Whether the instruction ends the program (its flags print .end).
    fn ends(&self, insn: &Insn) -> bool {
        ["end", "flags"].iter().any(|r| insn.name(r, self.core).is_some_and(|t| t.contains(".end")))
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

    /// Write the destination (number[6:0] | bank[9:7]), honouring the write
    /// mask and repeat fields where the form has them.
    fn write(&mut self, insn: &Insn, v: u32) -> Result<(), Error> {
        if let Some(m) = insn.name("mask", self.core)
            && !m.is_empty()
        {
            return Err(why(format!("write mask {m}")));
        }
        if let Some(r) = insn.name("repeat", self.core)
            && !r.is_empty()
        {
            return Err(why(format!("repeat {r}")));
        }
        let d = insn.get("dst").unwrap();
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
/// lanes at chan0 and chan2, its unwrapped signed sum; chan1 and chan3 test
/// false).
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
    // 32-bit: the full result, chan0 only
    let word = |r: u32| (r, Tested([Some(r as i32 as i64), None, None, None]));
    Ok(match op {
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
        _ => return Err(why(format!("test op {op}"))),
    })
}

/// A test type: sign test t/n/p, all (a) or any (o), zero test t/z/nz, and the
/// channel.
fn predicate(test: &str, t: Tested) -> Result<bool, Error> {
    let bad = || why(format!("test {test}"));
    let (kind, chan) = test.split_once(".chan").ok_or_else(bad)?;
    let kind = kind.strip_prefix("test").ok_or_else(bad)?;
    let chan: usize = chan.parse().map_err(|_| bad())?;
    let (sign, rest) = kind.split_at(1);
    let (all, zero) = rest.split_at(1);
    let Some(v) = t.0[chan] else {
        // no lane: established for the zero tests only
        return if zero == "z" && sign == "t" { Ok(false) } else { Err(bad()) };
    };
    let s = match sign {
        "t" => true,
        "n" => v < 0,
        "p" => v >= 0, // hardware: a zero result tests positive
        _ => return Err(bad()),
    };
    let z = match zero {
        "t" => true,
        "z" => v == 0,
        "nz" => v != 0,
        _ => return Err(bad()),
    };
    Ok(if all == "a" { s && z } else { s || z })
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
}
