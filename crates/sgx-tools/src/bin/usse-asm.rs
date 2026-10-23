// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! usse-asm: assemble USSE source into a raw little-endian blob of 64-bit
//! words, the inverse of usse-dis (tools/usse-asm/README.md).

use sgx_tools::asm::{Isa, main as asm_main};
use std::process::ExitCode;
use usse_isa::{Core, decode, encode_with};

fn list(idx: usize, w: u64, _: bool, core: Core) -> String {
    format!("{idx:6}: 0x{w:016X}\t{}", decode(w, core).text)
}

fn main() -> ExitCode {
    asm_main(&Isa {
        name: "usse-asm",
        width: 8,
        raw: ".long",
        data_segment: false,
        encode: encode_with,
        table: usse_isa::table,
        list,
    })
}
