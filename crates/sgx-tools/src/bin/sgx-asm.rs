// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! sgx-asm: USSE, PDS and data sections in one source; an image per section
//! and map.json with the labels and the patch records (tools/usse-asm/README.md).

use std::process::ExitCode;

fn main() -> ExitCode {
    sgx_tools::asm::main_program()
}
