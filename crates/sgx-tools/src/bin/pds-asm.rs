// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! pds-asm: assemble PDS source (a data segment, then the program) into a raw
//! little-endian blob of 32-bit words, the inverse of pds-dis
//! (tools/usse-asm/README.md).

use sgx_tools::asm::{Isa, main as asm_main};
use std::process::ExitCode;
use usse_isa::{Core, Ctx};

fn encode(text: &str, cx: &Ctx) -> Option<u64> {
    pds_isa::encode_with(text, cx).map(u64::from)
}

fn list(idx: usize, w: u64, is_data: bool, _: Core) -> String {
    let text = if is_data { "data".to_string() } else { pds_isa::decode(w as u32).text };
    format!("{idx}: 0x{w:08X}\t{text}")
}

fn main() -> ExitCode {
    asm_main(&Isa {
        name: "pds-asm",
        width: 4,
        raw: ".word",
        data_segment: true,
        encode,
        table: pds_isa::table,
        list,
    })
}
