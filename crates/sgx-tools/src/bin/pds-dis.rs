// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! pds-dis: disassemble a raw PDS program, little-endian 32-bit words, the
//! first <data_dwords> of which are the data segment, in the format of
//! tools/usse-dis/pds-dis (byte for byte), which lines up with refpds545:
//!
//!   <index>: 0x<HEX8>\t<text>
//!
//! Usage: pds-dis <program.bin> <data_dwords>

use std::io::{BufWriter, Write};
use std::process::ExitCode;

/// C's strtoul(s, 0, 0) for the data count: hex (0x), octal (0) or decimal.
fn strtoul(s: &str) -> u64 {
    let s = s.trim_start();
    let (radix, d) = match s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        Some(h) if h.starts_with(|c: char| c.is_ascii_hexdigit()) => (16, h),
        _ if s.starts_with('0') => (8, s),
        _ => (10, s),
    };
    let end = d.find(|c: char| !c.is_digit(radix)).unwrap_or(d.len());
    u64::from_str_radix(&d[..end], radix).unwrap_or(0)
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() < 3 {
        eprintln!("usage: {} <program.bin> <data_dwords>", argv[0]);
        return ExitCode::from(2);
    }
    let data = match std::fs::read(&argv[1]) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("{}: {e}", argv[1]);
            return ExitCode::from(1);
        }
    };
    let ndata = strtoul(&argv[2]);
    let stdout = std::io::stdout();
    let mut out = BufWriter::new(stdout.lock());
    for (i, b) in data.chunks_exact(4).enumerate() {
        let w = u32::from_le_bytes(b.try_into().unwrap());
        let text = if (i as u64) < ndata { "data".to_string() } else { pds_isa::decode(w).text };
        let _ = writeln!(out, "{i}: 0x{w:08X}\t{text}");
    }
    ExitCode::SUCCESS
}
