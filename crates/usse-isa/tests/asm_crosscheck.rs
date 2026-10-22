// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The encoder against the C assembler (tools/usse-asm/usse-asm): the text of
//! decoded words (random, and per form) must assemble to the same word, and
//! that word must decode back to the text.

mod common;

use std::fmt::Write as _;
use std::process::Command;
use usse_isa::{Core, decode, encode, forms};

/// Assemble `texts` with the C assembler, one per line: the words, None for a
/// line it refuses (found from its error messages, then left out).
fn c_asm(texts: &[String], flag: &str) -> Option<Vec<Option<u64>>> {
    let asm = common::repo().join("tools/usse-asm/usse-asm");
    if !asm.is_file() {
        eprintln!("skip: no C usse-asm (make -C tools/usse-asm)");
        return None;
    }
    let dir = std::path::Path::new(env!("CARGO_TARGET_TMPDIR"));
    let (s, o) = (dir.join(format!("asm{flag}.s")), dir.join(format!("asm{flag}.bin")));
    let run = |lines: &[usize]| {
        let mut src = String::new();
        for &i in lines {
            writeln!(src, "{}", texts[i]).unwrap();
        }
        std::fs::write(&s, &src).unwrap();
        Command::new(&asm).args(["--core", flag, "-o"]).arg(&o).arg(&s).output().unwrap()
    };
    let all: Vec<usize> = (0..texts.len()).collect();
    let r = run(&all);
    let refused: std::collections::HashSet<usize> = String::from_utf8_lossy(&r.stderr)
        .lines()
        .filter_map(|l| l.split(':').nth(1)?.parse::<usize>().ok().map(|n| n - 1))
        .collect();
    let kept: Vec<usize> = all.iter().copied().filter(|i| !refused.contains(i)).collect();
    let r = run(&kept);
    assert!(r.status.success(), "C usse-asm: {}", String::from_utf8_lossy(&r.stderr));
    let words = std::fs::read(&o).unwrap();
    let mut out = vec![None; texts.len()];
    for (k, b) in kept.iter().zip(words.chunks_exact(8)) {
        out[*k] = Some(u64::from_le_bytes(b.try_into().unwrap()));
    }
    Some(out)
}

/// The same instructions spelled as a person might: other case, decimal
/// numbers, without the disassembler's comments, odd blanks around commas.
fn respell(t: &str, i: usize) -> String {
    let mut s = match i % 3 {
        0 => t.to_ascii_uppercase(),
        1 => t.replace(" /* incp */", "").replace(", ", " ,  "),
        _ => t.to_string(),
    };
    while let Some(at) = s.find("#0x").or_else(|| s.find("#0X")) {
        let hex: String = s[at + 3..].chars().take_while(|c| c.is_ascii_hexdigit()).collect();
        let v = u64::from_str_radix(&hex, 16).unwrap_or(0);
        s = format!("{}#{v}{}", &s[..at], &s[at + 3 + hex.len()..]);
    }
    s
}

#[test]
fn respelled_texts_assemble_as_in_c() {
    let mut rng = common::Rng::new(0x5ee_d0c);
    let mut words: Vec<u64> = (0..3000).map(|_| rng.next()).collect();
    for f in forms() {
        for _ in 0..30 {
            words.push(f.val | (rng.next() & !f.mask));
        }
    }
    let texts: Vec<String> = words
        .iter()
        .map(|&w| decode(w, Core::Sgx545))
        .filter(|i| i.valid)
        .enumerate()
        .map(|(i, d)| respell(&d.text, i))
        .collect();
    let Some(want) = c_asm(&texts, "545") else { return };
    let mut bad = Vec::new();
    for (t, w) in texts.iter().zip(&want) {
        let got = encode(t, Core::Sgx545);
        if got != *w && bad.len() < 20 {
            bad.push(format!("{t}\n  want {w:016X?}\n  got  {got:016X?}"));
        }
    }
    assert!(bad.is_empty(), "{} texts:\n{}", texts.len(), bad.join("\n"));
}

fn crosscheck(core: Core, flag: &str) {
    let asm = common::repo().join("tools/usse-asm/usse-asm");
    if !asm.is_file() {
        eprintln!("skip: no C usse-asm (make -C tools/usse-asm)");
        return;
    }
    let mut rng = common::Rng::new(0xa55e_0b1e);
    let mut words: Vec<u64> = (0..20_000).map(|_| rng.next()).collect();
    for f in forms() {
        for _ in 0..150 {
            words.push(f.val | (rng.next() & !f.mask));
        }
    }
    let texts: Vec<String> =
        words.iter().map(|&w| decode(w, core)).filter(|i| i.valid).map(|i| i.text).collect();
    let mut src = String::new();
    for t in &texts {
        writeln!(src, "{t}").unwrap();
    }
    let dir = std::path::Path::new(env!("CARGO_TARGET_TMPDIR"));
    let (s, o) = (dir.join(format!("asm{flag}.s")), dir.join(format!("asm{flag}.bin")));
    std::fs::write(&s, &src).unwrap();
    let r = Command::new(&asm).args(["--core", flag, "-o"]).arg(&o).arg(&s).output().unwrap();
    assert!(r.status.success(), "C usse-asm: {}", String::from_utf8_lossy(&r.stderr));
    let want: Vec<u64> = std::fs::read(&o)
        .unwrap()
        .chunks_exact(8)
        .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
        .collect();
    assert_eq!(want.len(), texts.len());
    let mut bad = Vec::new();
    for (t, &w) in texts.iter().zip(&want) {
        let got = encode(t, core);
        if got != Some(w) && bad.len() < 20 {
            bad.push(format!("{t}\n  want {w:016X}\n  got  {got:016X?}"));
        }
        if let Some(g) = got {
            assert_eq!(&decode(g, core).text, t, "round trip of {t}");
        }
    }
    assert!(bad.is_empty(), "core {flag}, {} texts:\n{}", texts.len(), bad.join("\n"));
}

#[test]
fn decoded_texts_assemble_as_in_c_545() {
    crosscheck(Core::Sgx545, "545");
}

#[test]
fn decoded_texts_assemble_as_in_c_540() {
    crosscheck(Core::Sgx540, "540");
}

#[test]
fn spelling_is_lenient_as_in_c() {
    // blanks, case, number formats and the disassembler's comments are free
    let w = encode("mov.skipinv r0, #0x00000000", Core::Sgx545).unwrap();
    assert_eq!(encode("MOV.SKIPINV  r0 , #0", Core::Sgx545), Some(w));
    let e = encode("emitpix1 #0 /* incp */, r0, r1", Core::Sgx545).unwrap();
    assert_eq!(encode("emitpix1 #0, r0, r1", Core::Sgx545), Some(e));
    assert_eq!(encode("not an instruction", Core::Sgx545), None);
}
