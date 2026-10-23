// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The hardware instruction vectors (design/sgx545_vectors.md, files in
//! data/vectors): how one runs on the simulator and compares with its
//! readback. The tests, the coverage report and the tools share this.

use crate::json::Json;
use crate::{Bound, Error, Machine, Memory, Program, run_quad};
use std::collections::BTreeMap;
use std::path::PathBuf;
use usse_isa::Core;

pub const SCRATCH_BASE: u32 = 0x3e00_3000;
pub const SCRATCH_DWORDS: usize = 0x4000;
pub const LIMM_MASK: u64 = 0xFF30_0000_0000_0000;
pub const LIMM_VAL: u64 = 0xFC20_0000_0000_0000;

/// The published vectors: data/vectors at the repository root (data/README.md).
pub fn data() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../data/vectors")
}

/// sgxfive-test's patch: a limm of 0x5CAFnnnn loads the scratch address + nnnn.
pub fn patch_scratch(w: u64) -> u64 {
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

/// How a vector matches: exactly, or with near values (Machine::near) in
/// it, each within the bound the hardware was measured to.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Outcome {
    Exact,
    Near,
}

/// Run one vector: how it matches, else why not ("unsupported: ..." for
/// what the simulator does not model yet). A near value that differs from the
/// readback within its bound matches; one computed from a near value
/// (Bound::Derived) has no bound and stops it.
pub fn run_scalar(v: &Json) -> Result<Outcome, String> {
    let words: Vec<u64> =
        v.get("program").get("words").arr().iter().map(|w| patch_scratch(w.hex().unwrap())).collect();
    let prog = Program::new(&words, Core::Sgx545);
    let mut mem =
        Memory { base: SCRATCH_BASE, words: (0..SCRATCH_DWORDS as u32).map(|i| 0xD000_0000 | i).collect() };
    let temps = v.get("init").get("temps").num().map_or(124, |t| t as usize);
    let mut m = Machine { temps, near: true, ..Machine::default() };
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
    let (mut diffs, mut near) = (Vec::new(), false);
    let mut check = |what: String, got: u32, want: u32, bound: Option<&Bound>| -> Result<(), String> {
        near |= bound.is_some();
        match bound {
            _ if got == want => {}
            Some(Bound::Derived) => return Err(format!("unsupported: {what} is computed from a near value")),
            Some(b) if b.admits(got, want) => {}
            _ => diffs.push(format!("{what} got 0x{got:08x} want 0x{want:08x}")),
        }
        Ok(())
    };
    for (i, &got) in mem.words.iter().enumerate() {
        let w = want.get(&i).copied().unwrap_or(0xD000_0000 | i as u32);
        check(format!("scratch[0x{:03x}]", i * 4), got, w, m.near_mem.get(&(SCRATCH_BASE + 4 * i as u32)))?;
    }
    if let Some(o0) = rb.get("o0").hex() {
        check("o0".to_string(), m.o[0], o0 as u32, m.bounds.get(&("o", 0)))?;
    }
    if !diffs.is_empty() {
        return Err(diffs[..diffs.len().min(4)].join("; "));
    }
    Ok(if near { Outcome::Near } else { Outcome::Exact })
}

/// Probes whose readback a single instance cannot reproduce, and why.
pub const SKIP: &[(&str, &str, &str)] = &[
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
    ("runs/probes/isa2/f1/", "fmad16-lanes", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "fmad16-rounding", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "fpma", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "statomic", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "colour-c10", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "sweep-flog", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "sweep-fexp", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "sweep-fsin", "rebuilt with other inputs"),
    ("runs/probes/isa2/f1/", "sweep-fcos", "rebuilt with other inputs"),
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
    // cases 4-8 read back their neighbours' values: the table gained a case
    // after these runs (fg4 and the soak runs match)
    ("runs/probes/isa3/fg/", "efo-gpiext-quad", "rebuilt with the cases shifted"),
    ("runs/probes/isa3/fg1/", "efo-gpiext-quad", "rebuilt with the cases shifted"),
    ("runs/probes/isa3/fg2/", "efo-gpiext-quad", "rebuilt with the cases shifted"),
    ("runs/probes/isa3/fg3/", "efo-gpiext-quad", "rebuilt with the cases shifted"),
    // the run's table began with a case (1, 1, 1, 1) that db9941a dropped
    // (appending (1, 1, 2, 2)): each readback is the previous case's
    ("runs/probes/isa4/left4/", "smlsi-three-sources", "rebuilt with the cases shifted"),
    // the run's p0-p3 tests compared r1-r4 with themselves; db9941a compares
    // them with r6 = 0xffffffff, which the dump reads back as 0
    ("runs/probes/isa4/left4/", "predicate-pn", "rebuilt with other inputs"),
    // a load before the store reads the stored value: another of the 2048
    // instances, sharing the scratch buffer, stored it first
    ("", "x-cache", "instances share the scratch buffer"),
];

/// The fill's width: instances are in raster order, 64 x 32.
pub const WIDTH: usize = 64;

/// Run a vector that reports every pixel, with pa0 = (y << 16) | x, as the
/// fill's aligned 2x2 quads (TL, TR, BL, BR); every pixel's o0 must match.
/// The scratch dwords are not compared: every instance stores to the same ones.
pub fn run_pixels(v: &Json) -> Result<Outcome, String> {
    let words: Vec<u64> =
        v.get("program").get("words").arr().iter().map(|w| patch_scratch(w.hex().unwrap())).collect();
    let prog = Program::new(&words, Core::Sgx545);
    let temps = v.get("init").get("temps").num().map_or(124, |t| t as usize);
    let pixels: Vec<u32> =
        v.get("readback").get("pixels").arr().iter().map(|p| p.hex().unwrap() as u32).collect();
    let height = pixels.len() / WIDTH;
    for y0 in (0..height).step_by(2) {
        for x0 in (0..WIDTH).step_by(2) {
            let at = [(x0, y0), (x0 + 1, y0), (x0, y0 + 1), (x0 + 1, y0 + 1)];
            let mut quad: [Machine; 4] = Default::default();
            for (m, &(x, y)) in quad.iter_mut().zip(&at) {
                m.temps = temps;
                m.pa[0] = (y as u32) << 16 | x as u32;
                m.written.pa |= 1;
            }
            let mut mem = Memory {
                base: SCRATCH_BASE,
                words: (0..SCRATCH_DWORDS as u32).map(|i| 0xD000_0000 | i).collect(),
            };
            match run_quad(&mut quad, &prog, &mut mem, 10_000) {
                Ok(()) => {}
                Err(e @ Error::Unsupported { .. }) => return Err(format!("unsupported: {e}")),
                Err(e) => return Err(format!("error: {e}")),
            }
            for (m, &(x, y)) in quad.iter().zip(&at) {
                let want = pixels[y * WIDTH + x];
                if m.o[0] != want {
                    return Err(format!("pixel ({x}, {y}) o0 got 0x{:08x} want 0x{want:08x}", m.o[0]));
                }
            }
        }
    }
    Ok(Outcome::Exact)
}

/// Whether a probe's run is on the skip list, and why.
pub fn skipped(raw: &str, probe: &str) -> Option<&'static str> {
    SKIP.iter().find(|(r, p, _)| *p == probe && raw.starts_with(r)).map(|s| s.2)
}

/// What a vector needs: one instance (the uniform o0 and the scratch), every
/// pixel as quads, or nothing a simulator can compare (failed jobs, PDS,
/// pixels that differ without being reported).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Scalar,
    Pixels,
    None,
}

pub fn kind(v: &Json) -> Kind {
    let rb = v.get("readback");
    if rb.get("failed").str().is_some() || v.get("program").get("isa").str() != Some("usse") {
        return Kind::None;
    }
    let coords = v.get("init").get("pa0").str() == Some("coords");
    if !rb.get("pixels").arr().is_empty() && coords {
        return Kind::Pixels;
    }
    if rb.get("o0").str().is_none() || coords { Kind::None } else { Kind::Scalar }
}

/// Run a vector of either kind: Ok, or "unsupported: ..." / a mismatch.
pub fn run(v: &Json) -> Option<Result<Outcome, String>> {
    match kind(v) {
        Kind::Scalar => Some(run_scalar(v)),
        Kind::Pixels => Some(run_pixels(v)),
        Kind::None => None,
    }
}

/// The forms a probe tests: its OP text (the provenance inputs, or the
/// parameters with {NAME} replaced from the inputs), assembled line by line;
/// lines that do not assemble on their own are left out, and so are the
/// set-up moves, tests and waits when the OP has another form.
pub fn under_test(v: &Json) -> Vec<usize> {
    let p = v.get("provenance");
    let (inputs, params) = (p.get("inputs"), p.get("params"));
    let Some(mut op) = inputs.get("OP").str().or(params.get("OP").str()).map(str::to_string) else {
        return vec![];
    };
    for (k, val) in inputs.obj().iter().chain(params.obj()) {
        if let Some(s) = val.str() {
            op = op.replace(&format!("{{{k}}}"), s);
        }
    }
    let mut forms = vec![];
    for line in op.lines() {
        if let Some(w) = usse_isa::encode(line.trim(), Core::Sgx545)
            && let Some(f) = usse_isa::decode_fields(w, Core::Sgx545)
            && !forms.contains(&f.form)
        {
            forms.push(f.form);
        }
    }
    let setup =
        |f: &usize| matches!(usse_isa::table().forms[*f].op, "mov" | "mov_imm" | "test" | "nop" | "wdf");
    if forms.iter().any(|f| !setup(f)) {
        forms.retain(|f| !setup(f));
    }
    forms
}

/// The probe-round vector files under `dir` (data()): every `*.vec.json` in its
/// subdirectories, in path order; vec1, recorded with its programs, has a test of its own.
pub fn probe_files(dir: &std::path::Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .flat_map(|d| files(&d))
        .collect();
    v.sort();
    v
}

/// Every vector file of a directory, in name order.
pub fn files(dir: &std::path::Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> =
        std::fs::read_dir(dir).into_iter().flatten().flatten().map(|e| e.path()).collect();
    v.retain(|p| p.extension().is_some_and(|e| e == "json"));
    v.sort();
    v
}

/// The reason of an unsupported outcome, with numbers taken out, so that one
/// reason is counted once ("reads r5 before ..." and "reads r9 ..." alike).
pub fn reason(e: &str) -> String {
    let why = e.rsplit(": ").next().unwrap_or(e);
    // "unsupported: <pc>: <text>: no semantics ...": by mnemonic
    if why.starts_with("no semantics") {
        let parts: Vec<&str> = e.splitn(4, ": ").collect();
        let text = parts.get(2).copied().unwrap_or("");
        let m = text.split_whitespace().find(|t| !(t.starts_with('p') || t.starts_with('!')) || t.len() > 3);
        return format!("no semantics yet: {}", m.unwrap_or("").split('.').next().unwrap());
    }
    let mut out = String::new();
    for c in why.chars() {
        if c.is_ascii_digit() {
            if !out.ends_with('N') {
                out.push('N');
            }
        } else {
            out.push(c);
        }
    }
    out
}

fn esc(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

/// The simulator's coverage of the probe vectors in `dir` (data(); the report is `doc/isa/sim-coverage.json`,
/// which `cargo run --release -p sgx-sim --example coverage` writes): for
/// each form of the USSE table, its op and, by the probes whose instructions
/// under test use it, how many vectors match, stop unsupported or mismatch,
/// with the unsupported reasons.
pub fn coverage_json(dir: &std::path::Path) -> String {
    type Counts = [usize; 3];
    let table = usse_isa::table();
    let mut probes: Vec<BTreeMap<String, Counts>> = vec![BTreeMap::new(); table.forms.len()];
    let mut whys: Vec<BTreeMap<String, usize>> = vec![BTreeMap::new(); table.forms.len()];
    let mut totals: BTreeMap<&str, Counts> = BTreeMap::new();
    let mut all_whys: BTreeMap<String, usize> = BTreeMap::new();
    let mut nears: Vec<BTreeMap<String, usize>> = vec![BTreeMap::new(); table.forms.len()];
    let mut all_near = 0;
    for path in probe_files(dir) {
        let d = crate::json::parse(&std::fs::read_to_string(&path).unwrap());
        let raw = d.get("run").get("raw").str().unwrap_or("").to_string();
        for v in d.get("vectors").arr() {
            let probe = v.get("provenance").get("probe").str().unwrap_or("").to_string();
            if skipped(&raw, &probe).is_some() {
                continue;
            }
            let k = kind(v);
            let Some(r) = run(v) else { continue };
            let near = r == Ok(Outcome::Near);
            all_near += near as usize;
            let slot = match &r {
                Ok(_) => 0,
                Err(e) if e.starts_with("unsupported") => 1,
                Err(_) => 2,
            };
            totals.entry(if k == Kind::Pixels { "pixels" } else { "scalar" }).or_default()[slot] += 1;
            if let Err(e) = &r
                && slot == 1
            {
                *all_whys.entry(reason(e)).or_default() += 1;
            }
            for f in under_test(v) {
                probes[f].entry(probe.clone()).or_default()[slot] += 1;
                if near {
                    *nears[f].entry(probe.clone()).or_default() += 1;
                }
                if let Err(e) = &r
                    && slot == 1
                {
                    *whys[f].entry(reason(e)).or_default() += 1;
                }
            }
        }
    }
    let mut o = String::from("{\n  \"format\": \"sgx-sim-coverage\",\n  \"version\": 1,\n");
    o += "  \"generated\": \"cargo run --release -p sgx-sim --example coverage\",\n  \"totals\": {";
    o += &totals.iter().map(|(k, c)| format!("\"{k}\": {c:?}")).collect::<Vec<_>>().join(", ");
    o += &format!(", \"near\": {all_near}}},\n  \"unsupported\": {{");
    o += &all_whys.iter().map(|(w, n)| format!("\"{}\": {n}", esc(w))).collect::<Vec<_>>().join(", ");
    o += "},\n  \"forms\": [\n";
    let rows: Vec<String> = table
        .forms
        .iter()
        .enumerate()
        .map(|(i, f)| {
            let op = if f.op.is_empty() { "null".to_string() } else { format!("\"{}\"", f.op) };
            let pr: Vec<String> = probes[i].iter().map(|(p, c)| format!("\"{}\": {c:?}", esc(p))).collect();
            let wh: Vec<String> = whys[i].iter().map(|(w, n)| format!("\"{}\": {n}", esc(w))).collect();
            let ne: Vec<String> = nears[i].iter().map(|(p, n)| format!("\"{}\": {n}", esc(p))).collect();
            format!(
                "    {{\"form\": {i}, \"op\": {op}, \"probes\": {{{}}}, \"near\": {{{}}}, \"unsupported\": {{{}}}}}",
                pr.join(", "),
                ne.join(", "),
                wh.join(", ")
            )
        })
        .collect();
    o += &rows.join(",\n");
    o += "\n  ]\n}\n";
    o
}
