// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! sgx-asm: USSE, PDS and data sections in one source, each written as its
//! own image, plus map.json with the sections, the labels and the patch
//! records (design/sgx545_fw_format.md §4) for the values the driver fills in.

use sgx_sim::json::{self, Json};
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

fn dir(name: &str) -> PathBuf {
    let d = Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("sgx_asm.{name}"));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// Run a tool with `args` on `stdin`: status, stdout, stderr.
fn run(bin: &str, args: &[&str], stdin: &[u8]) -> (i32, String, String) {
    let mut c = Command::new(bin)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    c.stdin.take().unwrap().write_all(stdin).unwrap();
    let o = c.wait_with_output().unwrap();
    let s = |b: &[u8]| String::from_utf8_lossy(b).into_owned();
    (o.status.code().unwrap(), s(&o.stdout), s(&o.stderr))
}

const SGX_ASM: &str = env!("CARGO_BIN_EXE_sgx-asm");

/// What a single-ISA assembler makes of `src`.
fn single(tool: &str, d: &Path, src: &str) -> Vec<u8> {
    let bin = match tool {
        "usse-asm" => env!("CARGO_BIN_EXE_usse-asm"),
        _ => env!("CARGO_BIN_EXE_pds-asm"),
    };
    let out = d.join(format!("{tool}.bin"));
    let (s, _, e) = run(bin, &["-o", out.to_str().unwrap(), "-"], src.as_bytes());
    assert_eq!((s, e.as_str()), (0, ""), "{src}");
    std::fs::read(out).unwrap()
}

const SRC: &str = "\
        .equ P_COLOUR, 0
        .section fill, usse, 0
        phas #0, #0, pixel, end, parallel
        mov.skipinv.end o0, #param(P_COLOUR)

        .section prim, pds, 4
        .word doutu(fill), 4, 0, 0
        movs doutu, ds0[0], ds0[1], ds1[0], ds0[0]
next:   halt

        .section boot, usse, 10
start:  mov.skipinv r1, #addr(prim, 0x40)
        .patch word32, const(1 << 21)
        mov.skipinv r0, #0
        .patch emitpix_sb, param(5), 8
        ba #start

        .section ctl, data, 9
        .word addr_shr4(prim), code_word(start, 0xf), pds_ctl(prim, 0x100)
        .word pds_rows(prim), heap(2, 8), pd_phys(), 0x1234
";

fn num(j: &Json, k: &str) -> u64 {
    j.get(k).num().unwrap_or_else(|| panic!("{k} in {j:?}")) as u64
}

#[test]
fn sections_images_and_map() {
    let d = dir("map");
    let out = d.join("out");
    let (s, _, e) = run(SGX_ASM, &["-d", out.to_str().unwrap(), "-"], SRC.as_bytes());
    assert_eq!((s, e.as_str()), (0, ""));

    let fill = single("usse-asm", &d, "phas #0, #0, pixel, end, parallel\nmov.skipinv.end o0, #0\n");
    let prim = single("pds-asm", &d, ".word 0, 4, 0, 0\nmovs doutu, ds0[0], ds0[1], ds1[0], ds0[0]\nhalt\n");
    let boot = single("usse-asm", &d, "mov.skipinv r1, #0\nmov.skipinv r0, #0\nba #0\n");
    let ctl: Vec<u8> = [0u32, 0, 0, 0, 0, 0, 0x1234].iter().flat_map(|w| w.to_le_bytes()).collect();
    for (name, want) in [("fill", fill), ("prim", prim), ("boot", boot), ("ctl", ctl)] {
        assert_eq!(std::fs::read(out.join(format!("{name}.bin"))).unwrap(), want, "{name}");
    }

    let map = json::parse(&std::fs::read_to_string(out.join("map.json")).unwrap());
    let sections: Vec<_> = map
        .get("sections")
        .arr()
        .iter()
        .map(|s| {
            let name = s.get("name").str().unwrap().to_string();
            let isa = s.get("isa").str().unwrap().to_string();
            (name, isa, num(s, "id"), num(s, "size"), num(s, "data"), s.get("bin").str().unwrap().to_string())
        })
        .collect();
    let sec =
        |n: &str, i: &str, id, size, data| (n.to_string(), i.to_string(), id, size, data, format!("{n}.bin"));
    assert_eq!(
        sections,
        [
            sec("fill", "usse", 0, 16, 0),
            sec("prim", "pds", 4, 24, 4),
            sec("boot", "usse", 10, 24, 0),
            sec("ctl", "data", 9, 28, 0)
        ]
    );

    let symbols: Vec<_> = map
        .get("symbols")
        .arr()
        .iter()
        .map(|s| {
            let name = s.get("name").str().unwrap().to_string();
            (name, s.get("section").str().unwrap().to_string(), num(s, "index"), num(s, "offset"))
        })
        .collect();
    let sym = |n: &str, s: &str, i, o| (n.to_string(), s.to_string(), i, o);
    assert_eq!(symbols, [sym("next", "prim", 1, 20), sym("start", "boot", 0, 0)]);

    let patches: Vec<_> = map
        .get("patches")
        .arr()
        .iter()
        .map(|p| {
            let s = |k| p.get(k).str().unwrap().to_string();
            (
                s("section"),
                num(p, "id"),
                num(p, "offset"),
                s("encoding"),
                s("kind"),
                num(p, "target"),
                num(p, "addend"),
            )
        })
        .collect();
    let p = |s: &str, id, off, enc: &str, kind: &str, t, a| {
        (s.to_string(), id, off, enc.to_string(), kind.to_string(), t, a)
    };
    assert_eq!(
        patches,
        [
            p("fill", 0, 8, "usse_limm", "param", 0, 0),
            p("prim", 4, 0, "word32", "doutu", 0, 0),
            p("boot", 10, 0, "usse_limm", "addr", 4, 0x40),
            p("boot", 10, 8, "word32", "const", 0, 0x200000),
            p("boot", 10, 8, "emitpix_sb", "param", 5, 0),
            p("ctl", 9, 0, "word32", "addr_shr4", 4, 0),
            p("ctl", 9, 4, "word32", "code_word", 0, 0xf),
            p("ctl", 9, 8, "word32", "pds_ctl", 4, 0x100),
            p("ctl", 9, 12, "word32", "pds_rows", 4, 0),
            p("ctl", 9, 16, "word32", "heap", 2, 8),
            p("ctl", 9, 20, "word32", "pd_phys", 0, 0),
        ]
    );
}

#[test]
fn listing_names_each_section() {
    let (s, out, e) = run(SGX_ASM, &["-l", "-"], SRC.as_bytes());
    assert_eq!((s, e.as_str()), (0, ""));
    assert!(out.contains("section prim (pds, id 4)\n0: 0x00000000\tdata\n"), "{out}");
    assert!(out.contains("section ctl (data, id 9)\n0: 0x00000000\n"), "{out}");
}

#[test]
fn errors() {
    let src = "\
nop
.section a, usse, 0
x: nop
.long addr(a)
.section b, pds, 1
y: halt
movs doutu, ds0[param(0)], ds0[1], ds1[0], ds0[0]
.word code_word(y), addr(nowhere)
.word frob(1)
.word param()
.word const(1, 2, 3)
.section a, usse, 2
.section c, gpu, 3
.section d, data
.patch word32, const(1)
.patch blend, const(1)
.word 1
.patch word32, const(1)
";
    let (s, _, e) = run(SGX_ASM, &["-"], src.as_bytes());
    assert_eq!(s, 1);
    for want in [
        "<stdin>:1: statement outside a section",
        "<stdin>:4: a patch needs a 32-bit word here",
        "<stdin>:7: a patch in a pds instruction",
        "<stdin>:8: code_word needs a usse label, not 'y'",
        "<stdin>:8: unknown section 'nowhere'",
        "<stdin>:9: bad .word value",
        "<stdin>:10: param takes 1 or 2 arguments",
        "<stdin>:11: const takes 1 argument",
        "<stdin>:12: section 'a' defined twice",
        "<stdin>:13: section isa must be usse, pds or data",
        "<stdin>:14: .section needs a name, an isa and an id",
        "<stdin>:16: unknown patch encoding 'blend'",
        "<stdin>:18: .patch without a statement after it",
    ] {
        assert!(e.contains(want), "missing '{want}' in:\n{e}");
    }
}

/// The single-ISA tools do not know sections or patches.
#[test]
fn single_isa_tools_reject_sections() {
    let (s, _, e) = run(env!("CARGO_BIN_EXE_usse-asm"), &["-"], b".section a, usse, 0\n");
    assert_eq!(s, 1);
    assert!(e.contains("cannot assemble '.section a, usse, 0'"), "{e}");
}
