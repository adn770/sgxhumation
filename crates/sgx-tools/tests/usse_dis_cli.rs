// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The Rust usse-dis prints what the C one (tools/usse-dis/usse-dis) prints,
//! byte for byte, for every option.

use std::path::PathBuf;
use std::process::Command;

fn c_usse_dis() -> Option<PathBuf> {
    let p = std::env::var_os("USSE_DIS_C")
        .map(PathBuf::from)
        .unwrap_or(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tools/usse-dis/usse-dis"));
    p.is_file().then_some(p)
}

fn out(bin: &std::path::Path, args: &[&str]) -> (bool, Vec<u8>) {
    let o = Command::new(bin).args(args).output().unwrap();
    (o.status.success(), o.stdout)
}

#[test]
fn output_matches_c() {
    let Some(c) = c_usse_dis() else {
        eprintln!("skip: no C usse-dis (make -C tools/usse-dis)");
        return;
    };
    let rs = PathBuf::from(env!("CARGO_BIN_EXE_usse-dis"));
    // every 8-byte pattern of a few bit densities, plus a trailing partial word
    let mut bytes = Vec::new();
    let mut x = 0x9e37_79b9_7f4a_7c15u64;
    for i in 0..20_000u64 {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        bytes.extend((x & if i % 3 == 0 { !0 } else { x.rotate_left(i as u32) }).to_le_bytes());
    }
    bytes.extend([1, 2, 3]);
    let f = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("cli_words.bin");
    std::fs::write(&f, &bytes).unwrap();
    let f = f.to_str().unwrap();
    for args in [
        vec!["--forms"],
        vec![f],
        vec![f, "100"],
        vec![f, "0x10"],
        vec!["--which", f],
        vec!["--core", "540", f],
        vec!["--which", "--core", "540", f, "500"],
    ] {
        let (want, got) = (out(&c, &args), out(&rs, &args));
        assert_eq!(got.0, want.0, "{args:?}: exit status");
        assert!(got.1 == want.1, "{args:?}: output differs");
    }
    // usage errors fail in both
    for args in [vec![], vec!["--bogus", f]] {
        assert!(!out(&rs, &args).0 && !out(&c, &args).0, "{args:?}");
    }
}
