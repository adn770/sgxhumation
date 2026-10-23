// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The simulator against the hardware: every recorded instruction vector in the
//! offline corpus (corpus/vectors, design/sgx545_vectors.md). Each runs as one
//! instance; the scratch dwords and o0 must equal the readback.

mod common;

use common::json::{self, Json};
use sgx_sim::{Error, Machine, Memory, Program};
use std::collections::BTreeMap;
use std::path::PathBuf;
use usse_isa::Core;

const SCRATCH_BASE: u32 = 0x3e00_3000;
const SCRATCH_DWORDS: usize = 0x4000;
const LIMM_MASK: u64 = 0xFF30_0000_0000_0000;
const LIMM_VAL: u64 = 0xFC20_0000_0000_0000;

fn corpus() -> Option<PathBuf> {
    let p = std::env::var_os("SGX_CORPUS")
        .map(PathBuf::from)
        .unwrap_or(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../corpus"));
    p.is_dir().then_some(p)
}

/// sgxfive-test's patch: a limm of 0x5CAFnnnn loads the scratch address + nnnn.
fn patch_scratch(w: u64) -> u64 {
    let limm = usse_isa::form::Field {
        k: &usse_isa::form::Num { radix: usse_isa::form::Radix::Hex, mul: 1, add: 0 },
        c: &[
            usse_isa::form::Chunk { lo: 0, w: 21 },
            usse_isa::form::Chunk { lo: 36, w: 5 },
            usse_isa::form::Chunk { lo: 44, w: 6 },
        ],
        r: "",
    };
    let imm = limm.get(w);
    if w & LIMM_MASK == LIMM_VAL && imm & !0xffff == 0x5CAF_0000 {
        limm.put(w, SCRATCH_BASE + (imm & 0xffff))
    } else {
        w
    }
}

/// Run one vector: None if it passes, else why not ("unsupported: ..." for
/// what the simulator does not model yet).
fn run(v: &Json) -> Result<(), String> {
    let words: Vec<u64> =
        v.get("program").get("words").arr().iter().map(|w| patch_scratch(w.hex().unwrap())).collect();
    let prog = Program::new(&words, Core::Sgx545);
    let mut mem =
        Memory { base: SCRATCH_BASE, words: (0..SCRATCH_DWORDS as u32).map(|i| 0xD000_0000 | i).collect() };
    let temps = v.get("init").get("temps").num().map_or(124, |t| t as usize);
    let mut m = Machine { temps, ..Machine::default() };
    match m.run(&prog, &mut mem, 10_000) {
        Ok(()) => {}
        Err(e @ Error::Unsupported { .. }) => return Err(format!("unsupported: {e}")),
        Err(e) => return Err(format!("error: {e}")),
    }
    let rb = v.get("readback");
    let mut want: BTreeMap<usize, u32> = BTreeMap::new();
    for (off, val) in rb.get("scratch").obj() {
        let off = usize::from_str_radix(off.trim_start_matches("0x"), 16).unwrap();
        want.insert(off / 4, val.hex().unwrap() as u32);
    }
    let mut diffs = Vec::new();
    for (i, &got) in mem.words.iter().enumerate() {
        let w = want.get(&i).copied().unwrap_or(0xD000_0000 | i as u32);
        if got != w {
            diffs.push(format!("scratch[0x{:03x}] got 0x{got:08x} want 0x{w:08x}", i * 4));
        }
    }
    if let Some(o0) = rb.get("o0").hex()
        && m.o[0] != o0 as u32
    {
        diffs.push(format!("o0 got 0x{:08x} want 0x{o0:08x}", m.o[0]));
    }
    if diffs.is_empty() { Ok(()) } else { Err(diffs[..diffs.len().min(4)].join("; ")) }
}

#[test]
fn recorded_vectors_match_the_hardware() {
    let Some(corpus) = corpus() else {
        eprintln!("skip: no corpus (set SGX_CORPUS)");
        return;
    };
    let d = json::parse(&std::fs::read_to_string(corpus.join("vectors/vec1.vec.json")).unwrap());
    let (mut pass, mut unsup, mut fail) = (0, BTreeMap::<String, usize>::new(), Vec::new());
    for v in d.get("vectors").arr() {
        if v.get("readback").get("failed").str().is_some() {
            continue; // a job the hardware refused
        }
        let id = v.get("id").str().unwrap().to_string();
        match run(v) {
            Ok(()) => pass += 1,
            Err(e) if e.starts_with("unsupported") => {
                let probe = v.get("provenance").get("probe").str().unwrap().to_string();
                *unsup.entry(format!("{probe}: {}", e.split(": ").last().unwrap())).or_default() += 1;
            }
            Err(e) => fail.push(format!("{id}: {e}")),
        }
    }
    eprintln!("{pass} pass, {} unsupported, {} fail", unsup.values().sum::<usize>(), fail.len());
    for (k, n) in &unsup {
        eprintln!("  unsupported x{n}  {k}");
    }
    assert!(fail.is_empty(), "{} mismatches:\n{}", fail.len(), fail[..fail.len().min(30)].join("\n"));
}

/// Probes whose readback a single instance cannot reproduce, and why.
const SKIP: &[(&str, &str, &str)] = &[
    // run (raw path prefix), probe, why. The early runs used an uncommitted
    // memoff.s with its base at scratch + 0x100 (the readbacks show it); the
    // rebuilt programs use + 0x8000, as vec1 (recorded) does, with the same rule.
    ("runs/probes/probes/run2/", "ldad-reg-off", "rebuilt with another base"),
    ("runs/probes/probes/run2/", "ldad-reg-off-neg", "rebuilt with another base"),
    ("runs/probes/probes/run3/", "ldad-reg-off-sign", "rebuilt with another base"),
    ("runs/probes/probes/run4/", "ldad-reg-off-sign", "rebuilt with another base"),
    ("runs/probes/probes/run2/", "stad-reg-off", "rebuilt with another base"),
    ("runs/probes/probes/run4/", "stad-reg-off", "rebuilt with another base"),
    // the runs had other inputs (the readback of r1 differs from the table's)
    ("runs/probes/isa2/f1/", "fadm-fmsa-fsubflr", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "sop2wm", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "sop3-lrp", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "sop3-random", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "intdot-lanes", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "intdot-alpha", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "efo-dataflow", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "efo-rounding", "rebuilt with other inputs"),
    ("runs/probes/isa4/left4/", "test-lanes-other", "rebuilt with other inputs"),
    // the run read the pixel from r10 and wrote r10 for case 0, the table
    // reads r11 and writes it (as isa1/f2 and later runs show)
    ("runs/probes/isa1/f1/", "fdp-specials", "rebuilt with another OUT"),
    // the run had other inputs (r2 read back 0x22222222, the table's is 0x62222222)
    ("runs/probes/isa1/f1/", "movc-i8", "rebuilt with other inputs"),
    ("runs/probes/isa1/f1/", "movc-i16", "rebuilt with other inputs"),
    ("runs/probes/isa1/f1/", "movc-i32", "rebuilt with other inputs"),
    ("runs/probes/isa1/f1/", "movc-i10", "rebuilt with other inputs"),
    ("runs/probes/isa1/f1/", "movc-flt", "rebuilt with other inputs"),
    // a load before the store reads the stored value: another of the 2048
    // instances, sharing the scratch buffer, stored it first
    ("", "x-cache", "instances share the scratch buffer"),
];

/// The probe rounds' vectors (rebuilt from the probe tables, so a program may
/// differ from what ran): every one the simulator models must match; the
/// summary says how many it models.
#[test]
fn probe_vectors_match_where_modelled() {
    let Some(corpus) = corpus() else {
        eprintln!("skip: no corpus (set SGX_CORPUS)");
        return;
    };
    let dir = corpus.join("vectors/probes2026-10-22");
    let Ok(rd) = std::fs::read_dir(&dir) else { return };
    let (mut pass, mut unsup, mut fail) = (0, 0, Vec::new());
    let mut whys = BTreeMap::<String, usize>::new();
    // SGX_SIM_SHOW=<text>: list the unsupported cases whose reason contains it
    let show = std::env::var("SGX_SIM_SHOW").ok();
    for e in rd {
        let d = json::parse(&std::fs::read_to_string(e.unwrap().path()).unwrap());
        for v in d.get("vectors").arr() {
            let rb = v.get("readback");
            if rb.get("failed").str().is_some() || v.get("program").get("isa").str() != Some("usse") {
                continue;
            }
            let probe = v.get("provenance").get("probe").str().unwrap_or("");
            let raw = d.get("run").get("raw").str().unwrap_or("");
            if SKIP.iter().any(|(r, p, _)| *p == probe && raw.starts_with(r)) {
                continue;
            }
            // pixels other than the uniform o0, and coordinates, need every instance
            if rb.get("o0").str().is_none() || v.get("init").get("pa0").str() == Some("coords") {
                continue;
            }
            match run(v) {
                Ok(()) => pass += 1,
                Err(e) if e.starts_with("unsupported") => {
                    unsup += 1;
                    if show.as_deref().is_some_and(|t| e.contains(t)) {
                        eprintln!("{} [{raw}]: {e}", v.get("id").str().unwrap());
                    }
                    // "unsupported: <pc>: <text>: <why>": group by why, and the
                    // forms without semantics by mnemonic
                    let parts: Vec<&str> = e.splitn(4, ": ").collect();
                    let why = parts.last().unwrap();
                    let w: String = if why.starts_with("no semantics") {
                        let text = parts.get(2).unwrap_or(&"");
                        let m = text
                            .split_whitespace()
                            .find(|t| !t.starts_with('p') && !t.starts_with('!') || t.len() > 3);
                        format!("no semantics: {}", m.unwrap_or("").split('.').next().unwrap())
                    } else if why.starts_with("test") {
                        why.to_string()
                    } else {
                        why.split(|c: char| c.is_ascii_digit()).next().unwrap().to_string()
                    };
                    *whys.entry(w).or_default() += 1;
                }
                Err(e) => fail.push(format!(
                    "{} [{}]: {e}",
                    v.get("id").str().unwrap(),
                    d.get("run").get("raw").str().unwrap_or("")
                )),
            }
        }
    }
    eprintln!("probes: {pass} pass, {unsup} unsupported, {} fail", fail.len());
    let mut top: Vec<_> = whys.into_iter().collect();
    top.sort_by_key(|x| std::cmp::Reverse(x.1));
    for (w, n) in top.iter().take(60) {
        eprintln!("  unsupported x{n}  {w}");
    }
    assert!(fail.is_empty(), "{} mismatches:\n{}", fail.len(), fail[..fail.len().min(200)].join("\n"));
}

/// Every word of the extracted ukernel (harness/ukernel-extract/extracted,
/// gitignored, from the user's own library) has semantics in the simulator,
/// except the all-zero words (padding the firmware never runs).
#[test]
fn the_ukernel_is_covered() {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../harness/ukernel-extract/extracted/sgx545_ukernel_template.bin");
    let Ok(bytes) = std::fs::read(&p) else {
        eprintln!("skip: no extracted ukernel");
        return;
    };
    let mut missing = Vec::new();
    for (i, b) in bytes.chunks_exact(8).enumerate() {
        let w = u64::from_le_bytes(b.try_into().unwrap());
        let f = usse_isa::decode_fields(w, Core::Sgx545);
        let op = f.map_or("", |f| usse_isa::table().forms[f.form].op);
        if op.is_empty() && w != 0 {
            missing.push(format!("{i}: {}", usse_isa::decode(w, Core::Sgx545).text));
        }
    }
    assert!(missing.is_empty(), "{} words without semantics:\n{}", missing.len(), missing.join("\n"));
}
