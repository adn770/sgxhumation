// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! SGX Series-5 USSE instruction decoder: the whole SGX545 instruction set,
//! with the SGX540 readings where they differ.
//!
//! The USSE instruction word is 64-bit little-endian. The top 5 bits
//! (enc[63:59]) select the major opcode class; class 0x1F is the
//! control/special group with further sub-opcode bits. The form table was
//! derived by bit-correlation against the SGX540 ground-truth disassembly and
//! by black-box probing of the SGX545 reference disassembler (see
//! tools/usse-dis/README.md), not guessed. Its text is oracle-compatible, and
//! the C decoder in tools/usse-dis (which the radare2 plugin wraps) is kept in
//! step with this one by tests/c_crosscheck.rs.

pub mod form;
mod forms;
mod kinds;
mod table;

pub use form::{Core, Ctx, Form};

/// A decoded word.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Insn {
    /// The raw 64-bit instruction word.
    pub enc: u64,
    /// True if a form decoded it.
    pub valid: bool,
    /// The form that decoded it (its index in `forms()`).
    pub form: Option<usize>,
    /// Mnemonic and operands, as the oracle prints them; for an undecoded word
    /// a readable class hint or `.long 0x...`.
    pub text: String,
}

/// Every form of the table, in decoding order.
pub fn forms() -> impl Iterator<Item = &'static Form> {
    table::PARTS.iter().flat_map(|p| p.iter())
}

/// Major opcode class = enc[63:59]. Base mnemonic per class, enumerated from
/// the 540 oracle histogram; used only as a readable hint for undecoded words.
pub fn class_name(top5: u32) -> Option<&'static str> {
    match top5 {
        0 => Some("fmad"),
        5 => Some("mov"),
        7 => Some("efo"),
        9 | 10 => Some("and"),
        11 => Some("xor"),
        12 => Some("shl"),
        13 => Some("shr"),
        20 => Some("ima16"),
        21 => Some("imae"),
        29 => Some("ldad"),
        30 => Some("stad"),
        31 => Some("special"),
        _ => None,
    }
}

/// Decode one 64-bit USSE word.
pub fn decode(enc: u64, core: Core) -> Insn {
    let cx = Ctx { core };
    match form::decode_in(forms(), enc, &cx) {
        Some((i, text)) => Insn { enc, valid: true, form: Some(i), text },
        None => {
            let text = match class_name((enc >> 59) as u32) {
                Some(base) => format!("{base:<8}; TODO operands"),
                None => format!(".long 0x{enc:016X}"),
            };
            Insn { enc, valid: false, form: None, text }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dis(enc: u64) -> String {
        decode(enc, Core::Sgx545).text
    }

    #[test]
    fn decodes_the_ukernel_entry() {
        // the first words of the SGX545 ukernel (corpus golden)
        assert_eq!(dis(0xFC22B08002002600), "mov r16, #0xAD002600");
        assert_eq!(dis(0xF026000680004710), "stad.bpcache [sa1,+#14], r16");
        assert_eq!(dis(0xF820000000000000), "idf drc0, bif");
        assert_eq!(dis(0xF920000000000000), "wdf drc0");
        assert_eq!(dis(0xF80000000000173F), "ba #0x0000173F");
    }

    #[test]
    fn undecoded_words_keep_a_hint() {
        // the float classes decode on the SGX545 only
        let i = decode(0, Core::Sgx540);
        assert_eq!((i.valid, i.form, i.text.as_str()), (false, None, "fmad    ; TODO operands"));
        assert_eq!(decode(4 << 59, Core::Sgx540).text, ".long 0x2000000000000000");
    }
}
