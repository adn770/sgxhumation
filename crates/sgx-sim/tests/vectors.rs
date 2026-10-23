// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The simulator against the hardware: every recorded instruction vector in the
//! offline corpus (corpus/vectors, design/sgx545_vectors.md). Each runs as one
//! instance, the scratch dwords and o0 equal to the readback; a vector that
//! reports every pixel runs as the fill's 2x2 quads, every pixel's o0 equal.

use sgx_sim::json;
use sgx_sim::vectors::{SKIP, corpus, run_pixels, run_scalar as run};
use std::collections::BTreeMap;
use std::path::PathBuf;
use usse_isa::Core;

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
            Ok(_) => pass += 1,
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
                Ok(_) => pass += 1,
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

/// The probe rounds' vectors that report every pixel, run as quads.
#[test]
fn pixel_vectors_match_where_modelled() {
    let Some(corpus) = corpus() else {
        eprintln!("skip: no corpus (set SGX_CORPUS)");
        return;
    };
    let Ok(rd) = std::fs::read_dir(corpus.join("vectors/probes2026-10-22")) else { return };
    let (mut pass, mut unsup, mut fail) = (0, 0, Vec::new());
    let mut whys = BTreeMap::<String, usize>::new();
    for e in rd {
        let d = json::parse(&std::fs::read_to_string(e.unwrap().path()).unwrap());
        let raw = d.get("run").get("raw").str().unwrap_or("").to_string();
        for v in d.get("vectors").arr() {
            let rb = v.get("readback");
            if rb.get("failed").str().is_some()
                || rb.get("pixels").arr().is_empty()
                || v.get("init").get("pa0").str() != Some("coords")
            {
                continue;
            }
            let probe = v.get("provenance").get("probe").str().unwrap_or("");
            if SKIP.iter().any(|(r, p, _)| *p == probe && raw.starts_with(r)) {
                continue;
            }
            match run_pixels(v) {
                Ok(_) => pass += 1,
                Err(e) if e.starts_with("unsupported") => {
                    unsup += 1;
                    let why = e.rsplit(": ").next().unwrap();
                    *whys
                        .entry(why.split(|c: char| c.is_ascii_digit()).next().unwrap().to_string())
                        .or_default() += 1;
                }
                Err(e) => fail.push(format!("{} [{raw}]: {e}", v.get("id").str().unwrap())),
            }
        }
    }
    eprintln!("pixel vectors: {pass} pass, {unsup} unsupported, {} fail", fail.len());
    let mut top: Vec<_> = whys.into_iter().collect();
    top.sort_by_key(|x| std::cmp::Reverse(x.1));
    for (w, n) in top.iter().take(20) {
        eprintln!("  unsupported x{n}  {w}");
    }
    assert!(fail.is_empty(), "{} mismatches:\n{}", fail.len(), fail[..fail.len().min(40)].join("\n"));
}

/// doc/isa/sim-coverage.json, which the ISA documents render per group, says
/// what the simulator does with today's vectors.
#[test]
fn the_coverage_report_is_fresh() {
    let Some(corpus) = corpus() else {
        eprintln!("skip: no corpus (set SGX_CORPUS)");
        return;
    };
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../doc/isa/sim-coverage.json");
    let committed = std::fs::read_to_string(&path).unwrap_or_default();
    assert!(
        committed == sgx_sim::vectors::coverage_json(&corpus),
        "doc/isa/sim-coverage.json is stale: cargo run --release -p sgx-sim --example coverage, then \
         python3 doc/isa/build.py"
    );
}
