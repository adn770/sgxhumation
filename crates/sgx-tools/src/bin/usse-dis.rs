// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! usse-dis: decode a raw little-endian USSE blob (e.g. the extracted ukernel),
//! one instruction per 8-byte word, in the format of tools/usse-dis/usse-dis
//! (byte for byte, options included), which lines up with the oracle
//! disassembly for diffing:
//!
//!   <index>: 0x<ENC16>\t<mnemonic>
//!
//! Usage: usse-dis [--which] [--core 540|545] <file.bin> [max_words]
//!        usse-dis --forms     list the form table: index, mask, value, the bits
//!                             the form reads (mask and fields), pattern
//!
//! --which appends "\t#<n>", the index of the form that decoded each word.
//! --core 540 follows the SGX540's reading where it differs (default 545).

use std::io::{BufWriter, Write};
use std::process::ExitCode;
use usse_isa::{Core, decode, forms};

/// C's strtol(s, 0, 0): sign, then hex (0x), octal (0) or decimal; the
/// longest valid prefix, 0 if none.
fn strtol(s: &str) -> i64 {
    let s = s.trim_start();
    let (neg, s) = match s.as_bytes().first() {
        Some(b'-') => (true, &s[1..]),
        Some(b'+') => (false, &s[1..]),
        _ => (false, s),
    };
    let (radix, digits) = if (s.starts_with("0x") || s.starts_with("0X"))
        && s[2..].starts_with(|c: char| c.is_ascii_hexdigit())
    {
        (16, &s[2..])
    } else if s.starts_with('0') {
        (8, s)
    } else {
        (10, s)
    };
    let end = digits.find(|c: char| !c.is_digit(radix)).unwrap_or(digits.len());
    let v = i64::from_str_radix(&digits[..end], radix).unwrap_or(if end > 0 { i64::MAX } else { 0 });
    if neg { -v } else { v }
}

/// C's atoi, as far as picking a core needs.
fn atoi(s: &str) -> i64 {
    let t = s.trim_start();
    let end = t.find(|c: char| !c.is_ascii_digit() && c != '-' && c != '+').unwrap_or(t.len());
    t[..end].parse().unwrap_or(0)
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().collect();
    let prog = argv.first().map_or("usse-dis", String::as_str);
    let mut args = &argv[1.min(argv.len())..];
    if args.is_empty() {
        eprintln!("usage: {prog} <file.bin> [max] | --forms");
        return ExitCode::from(2);
    }
    let stdout = std::io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    if args[0] == "--forms" {
        for (i, f) in forms().enumerate() {
            let _ = writeln!(
                out,
                "{i}\t{:016X}\t{:016X}\t{:016X}\t{}",
                f.mask,
                f.val,
                f.bits(),
                f.pat.unwrap_or("(reject)")
            );
        }
        return ExitCode::SUCCESS;
    }
    let (mut which, mut core) = (false, Core::Sgx545);
    while args.len() > 1 && args[0].starts_with("--") {
        match args[0].as_str() {
            "--which" => which = true,
            "--core" if args.len() > 2 => {
                core = if atoi(&args[1]) == 540 { Core::Sgx540 } else { Core::Sgx545 };
                args = &args[1..];
            }
            opt => {
                eprintln!("{prog}: unknown option {opt}");
                return ExitCode::from(2);
            }
        }
        args = &args[1..];
    }
    let data = match std::fs::read(&args[0]) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("open: {e}");
            return ExitCode::from(1);
        }
    };
    let max = args.get(1).map_or(-1, |s| strtol(s));
    for (pc, w) in data.chunks_exact(8).enumerate() {
        let enc = u64::from_le_bytes(w.try_into().unwrap());
        let insn = decode(enc, core);
        let _ = write!(out, "{pc:6}: 0x{enc:016X}\t{}", insn.text);
        if which {
            let _ = write!(out, "\t#{}", insn.form.map_or(-1, |i| i as i64));
        }
        let _ = writeln!(out);
        if max >= 0 && (pc as i64 + 1) >= max {
            break;
        }
    }
    ExitCode::SUCCESS
}
