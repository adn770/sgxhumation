// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! pds-asm: assemble PDS source (a data segment, then the program) into a raw
//! little-endian blob of 32-bit words, the inverse of pds-dis
//! (tools/usse-asm/README.md).

use sgx_tools::asm::{PDS, main as asm_main};
use std::process::ExitCode;

fn main() -> ExitCode {
    asm_main(&PDS)
}
