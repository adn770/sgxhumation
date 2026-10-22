// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The PDS decoder against the corpus goldens (`*.pds-dis.txt`, equal to
//! refpds545) and against the C decoder (tools/usse-dis/pds-dis) on random
//! words and on words matching each form.

use pds_isa::{decode, forms};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    for e in fs::read_dir(dir).unwrap() {
        let p = e.unwrap().path();
        if p.is_dir() {
            walk(&p, out);
        } else if p.to_string_lossy().ends_with(".pds-dis.txt") {
            out.push(p);
        }
    }
}

#[test]
fn corpus_goldens_decode_byte_for_byte() {
    let corpus = std::env::var_os("SGX_CORPUS").map(PathBuf::from).unwrap_or(repo().join("corpus"));
    if !corpus.is_dir() {
        eprintln!("skip: no corpus (set SGX_CORPUS)");
        return;
    }
    let mut files = Vec::new();
    walk(&corpus.join("golden"), &mut files);
    assert!(!files.is_empty());
    let (mut lines, mut bad) = (0, Vec::new());
    for f in &files {
        for line in fs::read_to_string(f).unwrap().lines() {
            // "<index>: 0x<HEX8>\t<text>", "data" for the data segment
            let (head, want) = line.split_once('\t').unwrap();
            if want == "data" {
                continue;
            }
            let w = u32::from_str_radix(head.split_once("0x").unwrap().1, 16).unwrap();
            let got = decode(w).text;
            lines += 1;
            if got != want && bad.len() < 20 {
                bad.push(format!("{}: {w:08X}\n  want {want}\n  got  {got}", f.display()));
            }
        }
    }
    assert!(bad.is_empty(), "differ:\n{}", bad.join("\n"));
    eprintln!("{} files, {lines} instructions equal", files.len());
}

#[test]
fn random_words_match_c() {
    let c = std::env::var_os("PDS_DIS_C").map(PathBuf::from).unwrap_or(repo().join("tools/usse-dis/pds-dis"));
    if !c.is_file() {
        eprintln!("skip: no C pds-dis (make -C tools/usse-dis)");
        return;
    }
    let mut x = 0x2545_f491_4f6c_dd1du64;
    let mut next = || {
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        (x.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 32) as u32
    };
    let mut ws: Vec<u32> = (0..300_000).map(|_| next()).collect();
    for f in forms() {
        for _ in 0..2000 {
            ws.push((f.val as u32) | (next() & !(f.mask as u32)));
        }
    }
    let path = Path::new(env!("CARGO_TARGET_TMPDIR")).join("pds_words.bin");
    fs::write(&path, ws.iter().flat_map(|w| w.to_le_bytes()).collect::<Vec<u8>>()).unwrap();
    let out = Command::new(c).arg(&path).arg("0").output().unwrap();
    assert!(out.status.success());
    let want = String::from_utf8(out.stdout).unwrap();
    let mut n = 0;
    for (i, (line, &w)) in want.lines().zip(&ws).enumerate() {
        assert_eq!(format!("{i}: 0x{w:08X}\t{}", decode(w).text), line);
        n += 1;
    }
    assert_eq!(n, ws.len());
}

#[test]
fn decoded_texts_assemble_as_in_c() {
    let asm = repo().join("tools/usse-asm/pds-asm");
    if !asm.is_file() {
        eprintln!("skip: no C pds-asm (make -C tools/usse-asm)");
        return;
    }
    let mut x = 0x9e37_79b9u32;
    let mut next = || {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        x
    };
    let mut ws: Vec<u32> = (0..30_000).map(|_| next()).collect();
    for f in forms() {
        for _ in 0..300 {
            ws.push((f.val as u32) | (next() & !(f.mask as u32)));
        }
    }
    let texts: Vec<String> = ws.iter().map(|&w| decode(w)).filter(|i| i.valid).map(|i| i.text).collect();
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR"));
    let (s, o) = (dir.join("pds_asm.s"), dir.join("pds_asm.bin"));
    fs::write(&s, texts.join("\n") + "\n").unwrap();
    let r = Command::new(&asm).arg("-o").arg(&o).arg(&s).output().unwrap();
    assert!(r.status.success(), "{}", String::from_utf8_lossy(&r.stderr));
    let want: Vec<u32> =
        fs::read(&o).unwrap().chunks_exact(4).map(|b| u32::from_le_bytes(b.try_into().unwrap())).collect();
    assert_eq!(want.len(), texts.len());
    for (t, &w) in texts.iter().zip(&want) {
        assert_eq!(pds_isa::encode(t), Some(w), "{t}");
        assert_eq!(decode(w).text, *t);
    }
}
