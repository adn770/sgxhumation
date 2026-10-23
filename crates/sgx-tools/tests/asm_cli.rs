// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The Rust usse-asm and pds-asm behave as the C ones (tools/usse-asm): the
//! same binary, listing, messages and exit status for every source in asm/ and
//! tools/usse-asm/progs, for listings on stdin, and for broken sources.

use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};

fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn run(bin: &Path, args: &[&str], stdin: Option<&[u8]>, out: &Path) -> (Output, Option<Vec<u8>>) {
    let _ = std::fs::remove_file(out);
    let mut c = Command::new(bin)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    c.stdin.take().unwrap().write_all(stdin.unwrap_or(b"")).unwrap();
    let o = c.wait_with_output().unwrap();
    (o, std::fs::read(out).ok())
}

/// Run the C and the Rust tool with the same arguments; "OUT" stands for the
/// output file, which each writes to its own path.
fn same(tool: &str, args: &[&str], stdin: Option<&[u8]>) -> bool {
    let c = repo().join("tools/usse-asm").join(tool);
    if !c.is_file() {
        eprintln!("skip: no C {tool} (make -C tools/usse-asm)");
        return false;
    }
    let rs = match tool {
        "usse-asm" => PathBuf::from(env!("CARGO_BIN_EXE_usse-asm")),
        _ => PathBuf::from(env!("CARGO_BIN_EXE_pds-asm")),
    };
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR"));
    // the tests run in parallel: one pair of output files per call
    static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let n = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let (oc, or) = (dir.join(format!("{tool}.{n}.c.bin")), dir.join(format!("{tool}.{n}.rs.bin")));
    let sub = |o: &Path| -> Vec<String> {
        args.iter()
            .map(|a| if *a == "OUT" { o.to_str().unwrap().to_string() } else { a.to_string() })
            .collect()
    };
    let (ac, ar) = (sub(&oc), sub(&or));
    let (want, wb) = run(&c, &ac.iter().map(String::as_str).collect::<Vec<_>>(), stdin, &oc);
    let (got, gb) = run(&rs, &ar.iter().map(String::as_str).collect::<Vec<_>>(), stdin, &or);
    assert_eq!(got.status.code(), want.status.code(), "{tool} {args:?}: status");
    assert_eq!(
        String::from_utf8_lossy(&got.stdout),
        String::from_utf8_lossy(&want.stdout),
        "{tool} {args:?}"
    );
    assert_eq!(
        String::from_utf8_lossy(&got.stderr),
        String::from_utf8_lossy(&want.stderr),
        "{tool} {args:?}"
    );
    assert_eq!(gb, wb, "{tool} {args:?}: output file");
    true
}

#[test]
fn every_program_assembles_as_in_c() {
    let mut n = 0;
    // our programs (asm/) and the probes; sgx-asm sources (with .section) are not for the
    // single-ISA tools
    let mut stack = vec![repo().join("asm"), repo().join("tools/usse-asm/progs")];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(d).unwrap() {
            let p = e.unwrap().path();
            if p.is_dir() {
                stack.push(p);
                continue;
            }
            if p.extension().is_none_or(|x| x != "s")
                || std::fs::read_to_string(&p)
                    .unwrap()
                    .lines()
                    .any(|l| l.trim_start().starts_with(".section"))
            {
                continue;
            }
            let tool = if p.file_name().unwrap().to_string_lossy().starts_with("pds_") {
                "pds-asm"
            } else {
                "usse-asm"
            };
            if !same(tool, &["-l", "-o", "OUT", p.to_str().unwrap()], None) {
                return;
            }
            n += 1;
        }
    }
    assert!(n > 20, "{n} programs");
}

#[test]
fn listings_on_stdin_and_options() {
    let src = b"start: mov.skipinv r0, #0x00000000\n     1: 0xF800014000000000\tnop\n.long 0x1, 2\nba #start ; done\n";
    for args in
        [&["-l", "-"][..], &["-o", "OUT", "-"], &["--core", "540", "-l", "-"], &["-l", "-o", "OUT", "-"]]
    {
        if !same("usse-asm", args, Some(src)) {
            return;
        }
    }
    let pds = b".word 0x1, 0x2\nloop: nop\n(p0) bra loop\n0: 0x00000005\tdata\nhalt\n";
    same("pds-asm", &["-l", "-o", "OUT", "-"], Some(pds));
}

#[test]
fn errors_as_in_c() {
    let bad = b"x: nop\nx: nop\nnot an instruction\n.long zz\n.long 1 2\n.long 0x\n";
    for args in [&["-o", "OUT", "-"][..], &["-l", "-"]] {
        if !same("usse-asm", args, Some(bad)) {
            return;
        }
    }
    same("pds-asm", &["-l", "-"], Some(b".word 0x100000000\nnop\n"));
    same("usse-asm", &[], None);
    same("usse-asm", &["-q", "-"], None);
    same("usse-asm", &["a.s", "b.s"], None);
    same("usse-asm", &["/nonexistent/file.s"], None);
    same("pds-asm", &["-o"], None);
}

/// Rust only: --explain adds, under an error, the forms that came closest.
#[test]
fn explain_shows_the_closest_forms() {
    let bin = PathBuf::from(env!("CARGO_BIN_EXE_usse-asm"));
    let out = Path::new(env!("CARGO_TARGET_TMPDIR")).join("explain.bin");
    let (o, _) = run(
        &bin,
        &["--explain", "-o", out.to_str().unwrap(), "-"],
        Some(b"fmad r10, r1, r2\nfrob r1\n"),
        &out,
    );
    let err = String::from_utf8_lossy(&o.stderr);
    assert_eq!(o.status.code(), Some(1));
    assert!(err.contains("<stdin>:1: cannot assemble 'fmad r10, r1, r2'"), "{err}");
    assert!(err.contains("matched 'fmad r10, r1, r2', expected ', <src2>'"), "{err}");
    assert!(
        err.contains("<stdin>:2: cannot assemble 'frob r1'\n  no form reads the mnemonic 'frob'"),
        "{err}"
    );
    // without it the messages stay C's
    let (o, _) = run(&bin, &["-o", out.to_str().unwrap(), "-"], Some(b"fmad r10, r1, r2\n"), &out);
    assert!(!String::from_utf8_lossy(&o.stderr).contains("matched"));
}
