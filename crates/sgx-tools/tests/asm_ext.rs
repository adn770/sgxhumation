// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! Rust only: the source extensions of usse-asm and pds-asm (.include, .equ
//! with expressions, .macro). A source using them assembles to the same words
//! as the same source written out by hand.

use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// A fresh directory for one test's files.
fn dir(name: &str) -> PathBuf {
    let d = Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("asm_ext.{name}"));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// Run a tool on `src` (a file in `d`, or stdin when None): status, the
/// output words as bytes, stderr.
fn asm(tool: &str, d: &Path, src: Option<&str>, stdin: &[u8]) -> (i32, Vec<u8>, String) {
    let bin = match tool {
        "usse-asm" => env!("CARGO_BIN_EXE_usse-asm"),
        _ => env!("CARGO_BIN_EXE_pds-asm"),
    };
    let out = d.join(format!("{}.bin", src.unwrap_or("stdin")));
    let _ = std::fs::remove_file(&out);
    let input = src.map_or("-".to_string(), |s| d.join(s).to_str().unwrap().to_string());
    let mut c = Command::new(bin)
        .args(["-o", out.to_str().unwrap(), &input])
        .current_dir(d)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    c.stdin.take().unwrap().write_all(stdin).unwrap();
    let o = c.wait_with_output().unwrap();
    let words = std::fs::read(&out).unwrap_or_default();
    (o.status.code().unwrap(), words, String::from_utf8_lossy(&o.stderr).into_owned())
}

fn write(d: &Path, name: &str, text: &str) {
    let p = d.join(name);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, text).unwrap();
}

/// `src` assembles cleanly and gives the same words as `plain`.
fn same_as(tool: &str, d: &Path, src: &str, plain: &str) {
    write(d, "plain.s", plain);
    let (s0, want, e0) = asm(tool, d, Some("plain.s"), b"");
    assert_eq!((s0, e0.as_str()), (0, ""), "plain source");
    let (s1, got, e1) = asm(tool, d, Some(src), b"");
    assert_eq!((s1, e1.as_str()), (0, ""), "{src}");
    assert_eq!(got, want, "{src}");
}

#[test]
fn include_is_read_relative_to_the_including_file() {
    let d = dir("include");
    write(&d, "inc/regs.s", "        mov.skipinv r1, #0x00000002\n        .include \"deeper.s\"\n");
    write(&d, "inc/deeper.s", "        nop\n");
    write(&d, "main.s", "start:  .include \"inc/regs.s\"\n        ba #start\n");
    same_as(
        "usse-asm",
        &d,
        "main.s",
        "start:  mov.skipinv r1, #0x00000002\n        nop\n        ba #start\n",
    );
}

#[test]
fn include_errors_name_the_included_file() {
    let d = dir("include_err");
    write(&d, "bad.s", "        nop\n        frob r1\n");
    write(&d, "main.s", "        .include \"bad.s\"\n        .include \"missing.s\"\n");
    let (s, _, e) = asm("usse-asm", &d, Some("main.s"), b"");
    assert_eq!(s, 1);
    let bad = d.join("bad.s");
    let main = d.join("main.s");
    assert!(e.contains(&format!("{}:2: cannot assemble 'frob r1'", bad.display())), "{e}");
    assert!(e.contains(&format!("{}:2: cannot include 'missing.s'", main.display())), "{e}");
}

#[test]
fn include_loops_stop() {
    let d = dir("include_loop");
    write(&d, "a.s", ".include \"a.s\"\n");
    let (s, _, e) = asm("usse-asm", &d, Some("a.s"), b"");
    assert_eq!(s, 1);
    assert!(e.contains("includes nested too deeply"), "{e}");
}

#[test]
fn include_from_stdin_is_relative_to_the_working_directory() {
    let d = dir("include_stdin");
    write(&d, "x.s", "nop\n");
    let (s, words, e) = asm("usse-asm", &d, None, b".include \"x.s\"\n");
    assert_eq!((s, e.as_str()), (0, ""));
    assert_eq!(words.len(), 8);
}

#[test]
fn equ_names_and_expressions() {
    let d = dir("equ");
    write(
        &d,
        "main.s",
        "        .equ BASE, 0x10\n\
         \x20       .equ NEXT, (BASE + 4) * 2   ; 40\n\
         \x20       .equ MASK, ~0 & 0xff\n\
         \x20       mov.skipinv r1, #BASE\n\
         \x20       mov.skipinv r2, #(NEXT | 1 << 8)\n\
         \x20       mov.skipinv r3, #(MASK - (1 << 4))\n\
         \x20       .long BASE, NEXT % 7\n",
    );
    same_as(
        "usse-asm",
        &d,
        "main.s",
        "        mov.skipinv r1, #16\n\
         \x20       mov.skipinv r2, #296\n\
         \x20       mov.skipinv r3, #239\n\
         \x20       .long 16, 5\n",
    );
}

#[test]
fn equ_in_pds_registers_and_data() {
    let d = dir("equ_pds");
    write(
        &d,
        "main.s",
        "        .equ U0, 2\n\
         \x20       .word (1 << 31) | U0, -1\n\
         \x20       movs doutu, ds0[U0], ds0[(U0 + 1)], ds1[0], ds1[0]\n\
         \x20       halt\n",
    );
    same_as(
        "pds-asm",
        &d,
        "main.s",
        "        .word 0x80000002, 0xffffffff\n\
         \x20       movs doutu, ds0[2], ds0[3], ds1[0], ds1[0]\n\
         \x20       halt\n",
    );
}

/// Parentheses that are not an expression (predicates, comments) are left
/// as they are.
#[test]
fn predicates_are_not_expressions() {
    let d = dir("pred");
    write(&d, "main.s", ".equ N, 1\nloop: nop\n(p0) bra loop\nhalt\n");
    same_as("pds-asm", &d, "main.s", "loop: nop\n(p0) bra loop\nhalt\n");
}

#[test]
fn equ_errors() {
    let d = dir("equ_err");
    let (s, _, e) = asm(
        "usse-asm",
        &d,
        None,
        b".equ A, B + 1\n.equ 9x, 1\n.equ C\n.equ D, 1 / 0\n.equ E, 1\n.equ E, 2\n.long (1 <<)\n",
    );
    assert_eq!(s, 1);
    for want in [
        "<stdin>:1: undefined symbol 'B'",
        "<stdin>:2: bad .equ name '9x'",
        "<stdin>:3: .equ needs a name and a value",
        "<stdin>:4: division by zero",
        "<stdin>:6: symbol 'E' defined twice",
        "<stdin>:7: bad .long value",
    ] {
        assert!(e.contains(want), "missing '{want}' in:\n{e}");
    }
}

#[test]
fn macros_with_arguments_defaults_and_unique_labels() {
    let d = dir("macro");
    write(
        &d,
        "main.s",
        "        .macro setr reg, val=0\n\
         \x20       mov.skipinv \\reg, #\\val\n\
         \x20       .endm\n\
         \x20       .macro spin\n\
         l\\@:    nop\n\
         \x20       ba #l\\@\n\
         \x20       .endm\n\
         top:    setr r1, 5\n\
         \x20       setr r2\n\
         \x20       spin\n\
         \x20       spin\n\
         \x20       ba #top\n",
    );
    same_as(
        "usse-asm",
        &d,
        "main.s",
        "top:    mov.skipinv r1, #5\n\
         \x20       mov.skipinv r2, #0\n\
         a:      nop\n\
         \x20       ba #a\n\
         b:      nop\n\
         \x20       ba #b\n\
         \x20       ba #top\n",
    );
}

#[test]
fn macros_can_call_macros_and_use_equs() {
    let d = dir("macro_nest");
    write(
        &d,
        "main.s",
        "        .equ R, 3\n\
         \x20       .macro one n, v\n\
         \x20       mov.skipinv r\\n, #(\\v * 2)\n\
         \x20       .endm\n\
         \x20       .macro two a, b\n\
         \x20       one 1, \\a\n\
         \x20       one 3, \\b\n\
         \x20       .endm\n\
         \x20       two 1, R\n",
    );
    same_as("usse-asm", &d, "main.s", "mov.skipinv r1, #2\nmov.skipinv r3, #6\n");
}

#[test]
fn macro_errors() {
    let d = dir("macro_err");
    let (s, _, e) = asm(
        "usse-asm",
        &d,
        None,
        b".macro m a\nnop\n.endm\nm\nm 1, 2\n.macro m\n.endm\n.macro r\nr\n.endm\nr\n.endm\n.macro open\nnop\n",
    );
    assert_eq!(s, 1);
    for want in [
        "<stdin>:4: macro 'm' needs a value for 'a'",
        "<stdin>:5: macro 'm' takes 1 argument(s), got 2",
        "<stdin>:6: macro 'm' defined twice",
        "<stdin>:11: macros nested too deeply",
        "<stdin>:12: .endm without .macro",
        "<stdin>:13: .macro 'open' without .endm",
    ] {
        assert!(e.contains(want), "missing '{want}' in:\n{e}");
    }
}
