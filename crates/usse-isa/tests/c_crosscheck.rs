// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! Keeps this decoder and the C one (tools/usse-dis, which the radare2 plugin
//! wraps) in step: the same form table, and the same text and form for every
//! word, on both cores.

mod common;

use std::fmt::Write as _;
use std::fs;
use std::process::Command;
use usse_isa::{Core, decode, forms};

fn run_c(args: &[&str]) -> Option<String> {
    let bin = common::c_usse_dis()?;
    let out = Command::new(bin).args(args).output().unwrap();
    assert!(out.status.success(), "usse-dis {args:?} failed");
    Some(String::from_utf8(out.stdout).unwrap())
}

#[test]
fn form_table_matches_c() {
    let Some(want) = run_c(&["--forms"]) else {
        eprintln!("skip: no C usse-dis (make -C tools/usse-dis)");
        return;
    };
    let mut got = String::new();
    for (i, f) in forms().enumerate() {
        writeln!(
            got,
            "{i}\t{:016X}\t{:016X}\t{:016X}\t{}",
            f.mask,
            f.val,
            f.bits(),
            f.pat.unwrap_or("(reject)")
        )
        .unwrap();
    }
    for (n, (w, g)) in want.lines().zip(got.lines()).enumerate() {
        assert_eq!(g, w, "form {n}");
    }
    assert_eq!(got.lines().count(), want.lines().count(), "form count");
}

/// Uniform random words, then for every form words that match it (its value,
/// random elsewhere), so that forms with wide masks are reached too.
fn words() -> Vec<u64> {
    let mut rng = common::Rng::new(0x5ec5_4500);
    let mut w: Vec<u64> = (0..200_000).map(|_| rng.next()).collect();
    for f in forms() {
        for _ in 0..1000 {
            w.push(f.val | (rng.next() & !f.mask));
        }
    }
    w
}

fn crosscheck(core: Core, flag: &str) {
    let ws = words();
    let path = std::path::Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("words{flag}.bin"));
    fs::write(&path, ws.iter().flat_map(|w| w.to_le_bytes()).collect::<Vec<u8>>()).unwrap();
    let p = path.to_str().unwrap();
    let Some(want) = run_c(&["--which", "--core", flag, p]) else {
        eprintln!("skip: no C usse-dis (make -C tools/usse-dis)");
        return;
    };
    let mut bad = Vec::new();
    let mut n = 0;
    for (pc, (line, &enc)) in want.lines().zip(&ws).enumerate() {
        let insn = decode(enc, core);
        let form = insn.form.map_or(-1, |i| i as i64);
        let got = format!("{pc:6}: 0x{enc:016X}\t{}\t#{form}", insn.text);
        n += 1;
        if got != line && bad.len() < 20 {
            bad.push(format!("want {line}\ngot  {got}"));
        }
    }
    assert_eq!(n, ws.len(), "C printed {n} of {} words", ws.len());
    assert!(bad.is_empty(), "core {flag}:\n{}", bad.join("\n"));
}

#[test]
fn random_words_match_c_545() {
    crosscheck(Core::Sgx545, "545");
}

#[test]
fn random_words_match_c_540() {
    crosscheck(Core::Sgx540, "540");
}
