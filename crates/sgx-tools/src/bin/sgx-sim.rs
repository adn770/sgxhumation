// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! sgx-sim: run a USSE program on the simulator (crates/sgx-sim) and print what
//! it wrote, or run corpus vectors against their hardware readbacks.
//!
//!   sgx-sim [--core 540|545] [--set REG=VALUE]... [--temps N] [--max N]
//!           [--trace | --quad X,Y] <program.s | program.bin | ->
//!   sgx-sim --vectors FILE.vec.json [--id ID]
//!
//! A program is assembly source (usse-asm's syntax) or, with the .bin suffix, raw
//! little-endian 64-bit words. It runs as one instance, or with --quad as the
//! 2x2 quad holding pixel (X, Y) with pa0 = (y << 16) | x in each instance.
//! Memory is the probe harness's scratch buffer: 64 KiB at 0x3e003000 filled
//! with 0xd0000000 | i, and a limm of 0x5cafnnnn loads its address + nnnn.
//! REG is rN, iN, paN, saN, oN or pN; VALUE is decimal or 0x-hex.

use sgx_sim::vectors::{self, SCRATCH_BASE, SCRATCH_DWORDS};
use sgx_sim::{Bus, Error, Machine, Memory, Program, run_quad};
use sgx_tools::asm::{Isa, assemble};
use std::fmt::Write as _;
use std::io::Read as _;
use std::process::ExitCode;
use usse_isa::{Core, decode, encode_with};

const USAGE: &str = "usage: sgx-sim [--core 540|545] [--set REG=VALUE]... [--temps N] [--max N]
               [--trace | --quad X,Y] <program.s | program.bin | ->
       sgx-sim --vectors FILE.vec.json [--id ID]";

fn list(idx: usize, w: u64, _: bool, core: Core) -> String {
    format!("{idx:6}: 0x{w:016X}\t{}", decode(w, core).text)
}

const USSE: Isa =
    Isa { name: "sgx-sim", width: 8, raw: ".long", data_segment: false, encode: encode_with, list };

fn num(s: &str) -> Option<u32> {
    match s.strip_prefix("0x").or(s.strip_prefix("0X")) {
        Some(h) => u32::from_str_radix(h, 16).ok(),
        None => s.parse().ok(),
    }
}

/// Preset a register (and mark it written): rN, iN, paN, saN, oN or pN.
fn set(m: &mut Machine, reg: &str, v: u32) -> Option<()> {
    let split = reg.find(|c: char| c.is_ascii_digit())?;
    let (bank, n) = (&reg[..split], reg[split..].parse::<usize>().ok()?);
    match bank {
        "r" if n < 128 => (m.r[n], m.written.r) = (v, m.written.r | 1 << n),
        "i" if n < 3 => (m.i[n], m.written.i) = (v, m.written.i | 1 << n),
        "pa" if n < 128 => (m.pa[n], m.written.pa) = (v, m.written.pa | 1 << n),
        "sa" if n < 128 => (m.sa[n], m.written.sa) = (v, m.written.sa | 1 << n),
        "o" if n < 128 => (m.o[n], m.written.o) = (v, m.written.o | 1 << n),
        "p" if n < 4 => m.p[n] = v != 0,
        _ => return None,
    }
    Some(())
}

/// The registers an instance has written, one per line.
fn state(m: &Machine) -> String {
    let mut s = String::new();
    let banks: [(&str, &[u32], u128); 4] = [
        ("r", &m.r, m.written.r),
        ("pa", &m.pa, m.written.pa),
        ("sa", &m.sa, m.written.sa),
        ("o", &m.o, m.written.o),
    ];
    for (name, regs, written) in banks {
        for (n, v) in regs.iter().enumerate() {
            if written >> n & 1 != 0 {
                let _ = writeln!(s, "{name}{n} = 0x{v:08x}");
            }
        }
        if name == "r" {
            for n in 0..3 {
                if m.written.i >> n & 1 != 0 {
                    let _ = writeln!(s, "i{n} = 0x{:08x}", m.i[n]);
                }
            }
        }
    }
    let _ = writeln!(s, "p = {}{}{}{} (p3-p0)", m.p[3] as u8, m.p[2] as u8, m.p[1] as u8, m.p[0] as u8);
    s
}

/// What changed between two states of an instance, indented for a trace.
fn changes(a: &Machine, b: &Machine) -> String {
    let (x, y) = (state(a), state(b));
    let old: Vec<&str> = x.lines().collect();
    y.lines().filter(|l| !old.contains(l)).map(|l| format!("    {l}\n")).collect()
}

/// The scratch memory, logging stores for a trace.
struct Scratch {
    mem: Memory,
    stores: Vec<(u32, u32)>,
}

impl Bus for Scratch {
    fn load32(&mut self, addr: u32) -> Result<u32, Error> {
        self.mem.load32(addr)
    }
    fn store32(&mut self, addr: u32, v: u32) -> Result<(), Error> {
        self.stores.push((addr, v));
        self.mem.store32(addr, v)
    }
}

fn scratch() -> Scratch {
    Scratch {
        mem: Memory {
            base: SCRATCH_BASE,
            words: (0..SCRATCH_DWORDS as u32).map(|i| 0xD000_0000 | i).collect(),
        },
        stores: Vec::new(),
    }
}

fn memory_changes(s: &Scratch) -> String {
    let mut o = String::new();
    for (i, &w) in s.mem.words.iter().enumerate() {
        let was = 0xD000_0000 | i as u32;
        if w != was {
            let _ = writeln!(o, "[0x{:08x}] = 0x{w:08x} (was 0x{was:08x})", SCRATCH_BASE + 4 * i as u32);
        }
    }
    o
}

fn load(path: &str, core: Core) -> Result<Vec<u64>, String> {
    let mut bytes = Vec::new();
    if path == "-" {
        let _ = std::io::stdin().read_to_end(&mut bytes);
    } else {
        bytes = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
    }
    if path.ends_with(".bin") {
        if bytes.len() % 8 != 0 {
            return Err(format!("{path}: not a whole number of 64-bit words"));
        }
        return Ok(bytes.chunks(8).map(|c| u64::from_le_bytes(c.try_into().unwrap())).collect());
    }
    let a = assemble(&USSE, &bytes, if path == "-" { "<stdin>" } else { path }, core);
    if a.errors > 0 {
        return Err(String::from_utf8_lossy(&a.messages).trim_end().to_string());
    }
    Ok(a.words)
}

fn run_vectors(path: &str, id: Option<&str>) -> ExitCode {
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("{path}: {e}");
            return ExitCode::from(2);
        }
    };
    let d = sgx_sim::json::parse(&text);
    let raw = d.get("run").get("raw").str().unwrap_or("").to_string();
    let (mut pass, mut unsup, mut fail, mut skip) = (0, 0, 0, 0);
    for v in d.get("vectors").arr() {
        let vid = v.get("id").str().unwrap_or("");
        if id.is_some_and(|i| i != vid) {
            continue;
        }
        let probe = v.get("provenance").get("probe").str().unwrap_or("");
        if let Some(why) = vectors::skipped(&raw, probe) {
            skip += 1;
            if id.is_some() {
                println!("{vid}: skipped ({why})");
            }
            continue;
        }
        match vectors::run(v) {
            None => skip += 1,
            Some(Ok(())) => pass += 1,
            Some(Err(e)) if e.starts_with("unsupported") => {
                unsup += 1;
                if id.is_some() {
                    println!("{vid}: {e}");
                }
            }
            Some(Err(e)) => {
                fail += 1;
                println!("{vid}: {e}");
            }
        }
    }
    println!("{pass} match, {unsup} unsupported, {fail} mismatch, {skip} skipped");
    if fail > 0 { ExitCode::from(1) } else { ExitCode::SUCCESS }
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let (mut core, mut sets, mut temps, mut max) = (Core::Sgx545, Vec::new(), 124, 100_000);
    let (mut trace, mut quad, mut input, mut vecs, mut id) = (false, None, None, None, None);
    let mut i = 0;
    let usage = || {
        eprintln!("{USAGE}");
        ExitCode::from(2)
    };
    while i < argv.len() {
        let a = argv[i].as_str();
        let mut arg = || {
            i += 1;
            argv.get(i).cloned()
        };
        match a {
            "--core" => core = if arg().as_deref() == Some("540") { Core::Sgx540 } else { Core::Sgx545 },
            "--set" => match arg().and_then(|s| s.split_once('=').map(|(r, v)| (r.to_string(), num(v)))) {
                Some((r, Some(v))) => sets.push((r, v)),
                _ => return usage(),
            },
            "--temps" => match arg().and_then(|s| s.parse().ok()) {
                Some(n) => temps = n,
                None => return usage(),
            },
            "--max" => match arg().and_then(|s| s.parse().ok()) {
                Some(n) => max = n,
                None => return usage(),
            },
            "--trace" => trace = true,
            "--quad" => match arg().and_then(|s| {
                let (x, y) = s.split_once(',')?;
                Some((x.parse::<u32>().ok()?, y.parse::<u32>().ok()?))
            }) {
                Some(p) => quad = Some(p),
                None => return usage(),
            },
            "--vectors" => vecs = arg(),
            "--id" => id = arg(),
            _ if input.is_none() && (!a.starts_with('-') || a == "-") => input = Some(a.to_string()),
            _ => return usage(),
        }
        i += 1;
    }
    if let Some(v) = vecs {
        return run_vectors(&v, id.as_deref());
    }
    let Some(input) = input else { return usage() };
    if trace && quad.is_some() {
        eprintln!("sgx-sim: --trace runs one instance, not a quad");
        return ExitCode::from(2);
    }
    let words = match load(&input, core) {
        Ok(w) => w.into_iter().map(vectors::patch_scratch).collect::<Vec<_>>(),
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::from(2);
        }
    };
    let prog = Program::new(&words, core);
    let new = || -> Option<Machine> {
        let mut m = Machine { core, temps, ..Machine::default() };
        for (r, v) in &sets {
            set(&mut m, r, *v)?;
        }
        Some(m)
    };
    let Some(m0) = new() else { return usage() };
    let mut bus = scratch();
    let (result, shown) = match quad {
        Some((x, y)) => {
            let (x0, y0) = (x & !1, y & !1);
            let mut q: [Machine; 4] = [m0.clone(), m0.clone(), m0.clone(), m0];
            for (k, m) in q.iter_mut().enumerate() {
                let (px, py) = (x0 + (k as u32 & 1), y0 + (k as u32 >> 1));
                (m.pa[0], m.written.pa) = (py << 16 | px, m.written.pa | 1);
            }
            let r = run_quad(&mut q, &prog, &mut bus, max);
            let k = ((y - y0) * 2 + (x - x0)) as usize;
            (r, q[k].clone())
        }
        None => {
            let mut m = m0;
            let r = loop {
                if m.steps >= max {
                    break Err(Error::StepLimit);
                }
                let before = trace.then(|| m.clone());
                let pc = m.pc;
                bus.stores.clear();
                let r = m.step(&prog, &mut bus);
                if let Some(b) = before {
                    print!("{pc}: {}\n{}", Machine::text(&prog, pc).unwrap_or("?"), changes(&b, &m));
                    for (a, v) in &bus.stores {
                        println!("    [0x{a:08x}] = 0x{v:08x}");
                    }
                }
                match r {
                    Ok(true) => break Ok(()),
                    Ok(false) => {}
                    Err(e) => break Err(e),
                }
            };
            (r, m)
        }
    };
    match &result {
        Ok(()) => println!("ended after {} steps", shown.steps),
        Err(e) => {
            println!("stopped after {} steps", shown.steps);
            eprintln!(
                "{}: {e}",
                if matches!(e, Error::Unsupported { .. }) { "unsupported" } else { "error" }
            );
        }
    }
    print!("{}{}", state(&shown), memory_changes(&bus));
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(Error::Unsupported { .. }) => ExitCode::from(1),
        Err(_) => ExitCode::from(2),
    }
}
