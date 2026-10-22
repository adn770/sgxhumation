// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! SGX Series-5 PDS (Programmable Data Sequencer) instruction decoder.
//!
//! A PDS program is a data segment (constants, in two banks ds0/ds1) followed
//! by 32-bit little-endian instructions; the EDM is entered through such
//! programs. The form table is generated from isa/pds/ (isa/README.md) and
//! walked with the form engine of `usse_isa::form`, as the C decoder in
//! tools/usse-dis/pds.c does; its text is refpds545-compatible.

mod kinds;
#[rustfmt::skip]
mod table;

use usse_isa::form::{Ctx, Form, decode_in};

/// A decoded word.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Insn {
    pub word: u32,
    /// True if a form decoded it.
    pub valid: bool,
    /// The form that decoded it (its index in `forms()`).
    pub form: Option<usize>,
    /// The text, as the oracle prints it; `.word 0x...` for an undecoded word.
    pub text: String,
}

/// Every form of the table, in decoding order.
pub fn forms() -> impl Iterator<Item = &'static Form> {
    table::FORMS.iter()
}

/// Decode one PDS instruction.
pub fn decode(word: u32) -> Insn {
    match decode_in(forms(), word as u64, &Ctx::default()) {
        Some((i, text)) => Insn { word, valid: true, form: Some(i), text },
        None => Insn { word, valid: false, form: None, text: format!(".word 0x{word:08X}") },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_edm_words() {
        // from the boot PDS program (corpus golden fw_pds10)
        assert_eq!(decode(0xAF000000).text, "halt");
        assert_eq!(decode(0xB0000000).text, "nop");
        assert!(!decode(0x77000000).valid); // opcode 0x0e is reserved
        assert_eq!(decode(0x77000000).text, ".word 0x77000000");
    }
}
