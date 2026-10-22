// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The Rust pds-dis prints what the C one (tools/usse-dis/pds-dis) prints.

use std::path::PathBuf;
use std::process::Command;

#[test]
fn output_matches_c() {
    let c = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tools/usse-dis/pds-dis");
    if !c.is_file() {
        eprintln!("skip: no C pds-dis (make -C tools/usse-dis)");
        return;
    }
    let rs = PathBuf::from(env!("CARGO_BIN_EXE_pds-dis"));
    let mut x = 0x1234_5678u32;
    let mut bytes = Vec::new();
    for _ in 0..50_000 {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        bytes.extend(x.to_le_bytes());
    }
    bytes.extend([9, 9]);
    let f = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("pds_cli.bin");
    std::fs::write(&f, &bytes).unwrap();
    let f = f.to_str().unwrap();
    for args in [vec![f, "0"], vec![f, "12"], vec![f, "0x10"]] {
        let want = Command::new(&c).args(&args).output().unwrap();
        let got = Command::new(&rs).args(&args).output().unwrap();
        assert_eq!(got.status.code(), want.status.code(), "{args:?}");
        assert!(got.stdout == want.stdout, "{args:?}: output differs");
    }
    for args in [vec![], vec![f]] {
        assert_eq!(Command::new(&rs).args(&args).output().unwrap().status.code(), Some(2));
    }
}
