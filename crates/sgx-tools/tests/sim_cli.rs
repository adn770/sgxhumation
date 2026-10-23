// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The sgx-sim command: runs a USSE program (source or binary) on the
//! simulator and prints what it wrote, with a trace on request; runs the
//! corpus vectors against the hardware readbacks.

use std::io::Write as _;
use std::path::PathBuf;
use std::process::{Command, Output, Stdio};

fn sim(args: &[&str], stdin: &str) -> Output {
    let mut c = Command::new(env!("CARGO_BIN_EXE_sgx-sim"))
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    c.stdin.take().unwrap().write_all(stdin.as_bytes()).unwrap();
    c.wait_with_output().unwrap()
}

fn out(o: &Output) -> String {
    String::from_utf8_lossy(&o.stdout).into_owned()
}

const SHL: &str = "mov r1, #0x00000002\nshl r2, r1, #3\nmov.end o0, r2\n";

#[test]
fn runs_a_source_and_prints_what_it_wrote() {
    let o = sim(&["-"], SHL);
    assert!(o.status.success(), "{o:?}");
    let s = out(&o);
    assert!(s.contains("ended after 3 steps"), "{s}");
    for l in ["r1 = 0x00000002", "r2 = 0x00000010", "o0 = 0x00000010"] {
        assert!(s.lines().any(|x| x == l), "{l} in {s}");
    }
}

#[test]
fn traces_each_instruction_and_its_writes() {
    let s = out(&sim(&["--trace", "-"], SHL));
    assert!(s.contains("0: mov r1, #0x00000002"), "{s}");
    assert!(s.contains("    r1 = 0x00000002"), "{s}");
    assert!(s.contains("1: shl r2, r1, #0x00000003"), "{s}");
}

#[test]
fn presets_registers() {
    let s = out(&sim(&["--set", "r1=0x10", "--set", "pa0=5", "-"], "or r2, r1, pa0\nmov.end o0, r2\n"));
    assert!(s.lines().any(|x| x == "o0 = 0x00000015"), "{s}");
}

#[test]
fn stops_where_the_model_stops() {
    let o = sim(&["-"], "mov.end o0, r5\n");
    assert_eq!(o.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&o.stderr).contains("unsupported"), "{o:?}");
}

#[test]
fn runs_a_quad() {
    // i3 is the next instance's i0: TR's pa0 for TL at (0, 0)
    let s = out(&sim(&["--quad", "0,0", "-"], "mov i0, pa0\nmov.end o0, i3\n"));
    assert!(s.lines().any(|x| x == "o0 = 0x00000001"), "{s}");
}

#[test]
fn checks_the_corpus_vectors() {
    let v = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../corpus/vectors/vec1.vec.json");
    if !v.is_file() {
        eprintln!("skip: no corpus");
        return;
    }
    let s = out(&sim(&["--vectors", v.to_str().unwrap()], ""));
    assert!(s.contains("533 match, 4 unsupported, 0 mismatch"), "{s}");
}

#[test]
fn lists_the_emits() {
    let s =
        out(&sim(&["-"], "mov r0, #0x00000011\nemitpix2.end.freep #0 /* incp */, r0, #0, r0, #0x00000200\n"));
    assert!(s.lines().any(|x| x == "emit emitpix2 0x00000011, 0x00000000, 0x00000011"), "{s}");
}
