// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! usse-asm: assemble USSE source into a raw little-endian blob of 64-bit
//! words, the inverse of usse-dis (tools/usse-asm/README.md).

use sgx_tools::asm::{USSE, main as asm_main};
use std::process::ExitCode;

fn main() -> ExitCode {
    asm_main(&USSE)
}
