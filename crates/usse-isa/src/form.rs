// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The instruction-form engine (the Rust side of tools/usse-dis/usse_form.h).
//!
//! A form is a text pattern plus the bits that identify it: a word matches when
//! `word & mask == val`, and its text is the pattern with each placeholder `{i}`
//! replaced by field i, printed by the field's kind. Bits outside the mask and
//! outside every field are ignored by the decoder; `dflt` records how the closed
//! toolchain fills them (the encoder's preference). Forms are tried in table
//! order and the first whose fields all print wins.
//!
//! Every layout is clean-room: derived by bit-correlation over the oracle
//! corpora and by black-box probing of refdisasm545, then checked against it on
//! random words.

/// The core whose reading the decoder follows. They differ in the load/store
/// cache bits, and the float classes decode on the SGX545 only.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Core {
    #[default]
    Sgx545,
    Sgx540,
}

/// What a kind may look at besides the field value.
#[derive(Clone, Copy, Debug, Default)]
pub struct Ctx {
    pub core: Core,
}

/// How a field value prints. `print` appends the text to `out` and returns
/// false if the value is not a valid one of this kind (the form then does not
/// decode the word).
pub trait Kind: Sync {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool;
}

/// A bit range of the word: `w` bits from bit `lo`.
#[derive(Clone, Copy, Debug)]
pub struct Chunk {
    pub lo: u8,
    pub w: u8,
}

/// A field: bit ranges of the word, concatenated LSB first, and their kind.
#[derive(Clone, Copy)]
pub struct Field {
    pub k: &'static dyn Kind,
    pub c: &'static [Chunk],
}

impl Field {
    pub fn width(&self) -> u32 {
        self.c.iter().map(|c| c.w as u32).sum()
    }

    pub fn get(&self, e: u64) -> u32 {
        let mut v = 0u32;
        let mut at = 0;
        for c in self.c {
            v |= (((e >> c.lo) & ((1u64 << c.w) - 1)) as u32) << at;
            at += c.w;
        }
        v
    }

    pub fn put(&self, mut e: u64, mut v: u32) -> u64 {
        for c in self.c {
            let m = (1u64 << c.w) - 1;
            e = (e & !(m << c.lo)) | ((v as u64 & m) << c.lo);
            v = v.checked_shr(c.w as u32).unwrap_or(0);
        }
        e
    }

    pub fn bits(&self) -> u64 {
        self.c.iter().fold(0, |b, c| b | ((1u64 << c.w) - 1) << c.lo)
    }
}

/// An instruction form; `pat: None` is a reject entry (the word stays undecoded).
#[derive(Clone, Copy)]
pub struct Form {
    pub pat: Option<&'static str>,
    pub mask: u64,
    pub val: u64,
    pub f: &'static [Field],
    pub dflt: u64,
}

impl Form {
    /// The bits the form reads: its mask and all its fields.
    pub fn bits(&self) -> u64 {
        self.f.iter().fold(self.mask, |b, f| b | f.bits())
    }

    /// Render word `e` with this form; None if a field does not print.
    pub fn render(&self, e: u64, cx: &Ctx) -> Option<String> {
        let pat = self.pat?.as_bytes();
        let mut out = String::new();
        let mut i = 0;
        while i < pat.len() {
            if pat[i] == b'{' && i + 2 < pat.len() && pat[i + 1].is_ascii_digit() && pat[i + 2] == b'}' {
                let f = self.f.get((pat[i + 1] - b'0') as usize)?;
                if !f.k.print(f.get(e), cx, &mut out) {
                    return None;
                }
                i += 3;
            } else {
                // patterns are ASCII
                out.push(pat[i] as char);
                i += 1;
            }
        }
        Some(out)
    }
}

/// Decode with a table: the index of the first matching form and its text, or
/// None (also for a reject entry).
pub fn decode_in<'a>(t: impl IntoIterator<Item = &'a Form>, e: u64, cx: &Ctx) -> Option<(usize, String)> {
    for (i, fm) in t.into_iter().enumerate() {
        if e & fm.mask != fm.val {
            continue;
        }
        fm.pat?;
        if let Some(s) = fm.render(e, cx) {
            return Some((i, s));
        }
    }
    None
}

/// Number formats of the generic kinds (C's "%u" and "0x%08X").
#[derive(Clone, Copy, Debug)]
pub enum Radix {
    Dec,
    Hex,
}

impl Radix {
    pub fn put(self, out: &mut String, v: u32) {
        use std::fmt::Write as _;
        let _ = match self {
            Radix::Dec => write!(out, "{v}"),
            Radix::Hex => write!(out, "0x{v:08X}"),
        };
    }
}

/// An enumeration: the name of each value, None for an invalid one (values
/// past the end are invalid too).
pub struct Enum(pub &'static [Option<&'static str>]);

impl Kind for Enum {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        match self.0.get(v as usize) {
            Some(Some(s)) => {
                out.push_str(s);
                true
            }
            _ => false,
        }
    }
}

/// A number, printed as `v * mul + add`.
pub struct Num {
    pub radix: Radix,
    pub mul: u32,
    pub add: u32,
}

impl Kind for Num {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        self.radix.put(out, v.wrapping_mul(self.mul).wrapping_add(self.add));
        true
    }
}

/// Several one-bit flags, printed in field order: each bit's name.
pub struct Flags(pub &'static [&'static str]);

impl Kind for Flags {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        for (i, name) in self.0.iter().enumerate() {
            if v >> i & 1 != 0 {
                out.push_str(name);
            }
        }
        v >> self.0.len() == 0
    }
}

/// An enumeration given by its valid entries, for sparse tables.
pub const fn sparse<const N: usize>(e: &[(usize, &'static str)]) -> [Option<&'static str>; N] {
    let mut t = [None; N];
    let mut i = 0;
    while i < e.len() {
        t[e[i].0] = Some(e[i].1);
        i += 1;
    }
    t
}

pub const fn b(n: u32) -> u64 {
    1 << n
}

/// Bits hi..lo.
pub const fn m(hi: u32, lo: u32) -> u64 {
    (!0u64 >> (63 - hi)) & (!0u64 << lo)
}

/// The major opcode class, enc[63:59].
pub const fn cls(c: u64) -> u64 {
    c << 59
}

pub const CLSM: u64 = m(63, 59);

/// `fld!(KIND, (lo, w), ...)`: a field of the given chunks.
macro_rules! fld {
    ($k:expr $(, ($lo:expr, $w:expr))* $(,)?) => {
        $crate::form::Field { k: &$k, c: &[$($crate::form::Chunk { lo: $lo, w: $w }),*] }
    };
}

/// `form!(pat, mask, val, [fields] [, dflt])`.
macro_rules! form {
    ($pat:expr, $mask:expr, $val:expr, [$($f:expr),* $(,)?] $(, $dflt:expr)?) => {
        $crate::form::Form { pat: Some($pat), mask: $mask, val: $val, f: &[$($f),*], dflt: 0 $(| $dflt)? }
    };
}

/// `names!["a", None, "b"]`: an enumeration table.
macro_rules! names {
    (@one None) => { None };
    (@one $s:literal) => { Some($s) };
    ($($x:tt),* $(,)?) => { &[$(names!(@one $x)),*] };
}

pub(crate) use {fld, form, names};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fields_scatter_and_gather_lsb_first() {
        static K: Num = Num { radix: Radix::Dec, mul: 1, add: 0 };
        let f = Field { k: &K, c: &[Chunk { lo: 4, w: 2 }, Chunk { lo: 60, w: 3 }] };
        assert_eq!(f.width(), 5);
        let e = f.put(0, 0b10111);
        assert_eq!(e, 0b11 << 4 | 0b101 << 60);
        assert_eq!(f.get(e), 0b10111);
        assert_eq!(f.bits(), 0b11 << 4 | 0b111 << 60);
    }

    #[test]
    fn masks() {
        assert_eq!(m(63, 59), 0xF800_0000_0000_0000);
        assert_eq!(m(0, 0), 1);
        assert_eq!(cls(31), 0xF800_0000_0000_0000);
    }

    #[test]
    fn flags_reject_bits_past_their_names() {
        let mut s = String::new();
        assert!(Flags(&[".a", ".b"]).print(0b11, &Ctx::default(), &mut s));
        assert_eq!(s, ".a.b");
        assert!(!Flags(&[".a"]).print(0b10, &Ctx::default(), &mut s));
    }
}
