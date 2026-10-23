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

/// A name resolver: the value of a known name (the assemblers' labels).
pub type Resolver<'a> = &'a dyn Fn(&str) -> Option<u64>;

/// What a kind may look at besides the field value.
#[derive(Clone, Copy, Default)]
pub struct Ctx<'a> {
    pub core: Core,
    /// Numeric fields accept a name instead of a number when set: it returns
    /// the value of a known name. (The assemblers use it for labels.)
    pub resolve: Option<Resolver<'a>>,
}

/// How a field value prints. `print` appends the text to `out` and returns
/// false if the value is not a valid one of this kind (the form then does not
/// decode the word).
pub trait Kind: Sync {
    fn print(&self, v: u32, cx: &Ctx, out: &mut String) -> bool;

    /// Whether value v prints (print returns true), without the text. The
    /// default prints; kinds whose answer is cheaper override it, and a test
    /// holds every override to print.
    fn valid(&self, v: u32, cx: &Ctx) -> bool {
        thread_local! {
            static SCRATCH: std::cell::RefCell<String> = const { std::cell::RefCell::new(String::new()) };
        }
        SCRATCH.with(|t| match t.try_borrow_mut() {
            Ok(mut t) => {
                t.clear();
                self.print(v, cx, &mut t)
            }
            // a kind validating another while printing: no shared buffer
            Err(_) => self.print(v, cx, &mut String::new()),
        })
    }

    /// Whether the kind has a parser. One without is inverted by searching its
    /// values for one that prints the text (see `encode::encode_in`).
    fn parses(&self) -> bool {
        false
    }

    /// Parse a value at the start of `s`: the value and the bytes read.
    fn parse(&self, _s: &str, _cx: &Ctx) -> Option<(u32, usize)> {
        None
    }
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
    /// What the field is to the simulator ("dst", "src1", ...), "" if unnamed.
    pub r: &'static str,
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
    /// What the form does, for the simulator ("" if not described yet).
    pub op: &'static str,
}

impl Form {
    /// The index of the field with role `r`.
    pub fn role(&self, r: &str) -> Option<usize> {
        self.f.iter().position(|f| f.r == r)
    }

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

/// A form table with its forms bucketed by the major class (5 bits at
/// `shift`: enc[63:59] for USSE, w[31:27] for PDS), each bucket in table order,
/// so that a word tries only the forms that can match its class.
pub struct Table {
    pub forms: &'static [Form],
    shift: u32,
    buckets: Vec<Vec<u16>>,
    /// per form, the fields its pattern prints (bit i: field i)
    used: Vec<u16>,
}

/// A word decoded to its form and field values, without text.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fields {
    /// The index of the form in its table.
    pub form: usize,
    /// The value of each field, in the form's field order (unused ones 0).
    pub v: [u32; 10],
}

impl Table {
    pub fn new(forms: &'static [Form], shift: u32) -> Table {
        let key_mask = 31u64 << shift;
        let buckets = (0..32u64)
            .map(|c| {
                let key = c << shift;
                (0..forms.len())
                    .filter(|&i| (key ^ forms[i].val) & forms[i].mask & key_mask == 0)
                    .map(|i| i as u16)
                    .collect()
            })
            .collect();
        let used = forms
            .iter()
            .map(|f| {
                let p = f.pat.unwrap_or("").as_bytes();
                p.windows(3)
                    .filter(|w| w[0] == b'{' && w[1].is_ascii_digit() && w[2] == b'}')
                    .fold(0u16, |m, w| m | 1 << (w[1] - b'0'))
            })
            .collect();
        Table { forms, shift, buckets, used }
    }

    /// The forms that can match `e`, in table order.
    fn candidates(&self, e: u64) -> impl Iterator<Item = (usize, &'static Form)> + '_ {
        self.buckets[(e >> self.shift & 31) as usize].iter().map(|&i| (i as usize, &self.forms[i as usize]))
    }

    /// The first form whose mask matches and whose fields all print, or None
    /// (also for a reject entry): the form `decode` prints with.
    pub fn form_of(&self, e: u64, cx: &Ctx) -> Option<usize> {
        for (i, fm) in self.candidates(e) {
            if e & fm.mask != fm.val {
                continue;
            }
            fm.pat?;
            let used = self.used[i];
            if fm
                .f
                .iter()
                .enumerate()
                .filter(|(j, _)| used >> j & 1 != 0)
                .all(|(_, f)| f.k.valid(f.get(e), cx))
            {
                return Some(i);
            }
        }
        None
    }

    /// Decode to the form and field values, without text.
    pub fn fields(&self, e: u64, cx: &Ctx) -> Option<Fields> {
        let form = self.form_of(e, cx)?;
        let mut v = [0u32; 10];
        for (x, f) in v.iter_mut().zip(self.forms[form].f) {
            *x = f.get(e);
        }
        Some(Fields { form, v })
    }

    /// Decode: the index of the first matching form and its text.
    pub fn decode(&self, e: u64, cx: &Ctx) -> Option<(usize, String)> {
        for (i, fm) in self.candidates(e) {
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
    fn valid(&self, v: u32, _: &Ctx) -> bool {
        matches!(self.0.get(v as usize), Some(Some(_)))
    }

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
    fn valid(&self, _: u32, _: &Ctx) -> bool {
        true
    }

    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        self.radix.put(out, v.wrapping_mul(self.mul).wrapping_add(self.add));
        true
    }

    fn parses(&self) -> bool {
        true
    }

    fn parse(&self, s: &str, cx: &Ctx) -> Option<(u32, usize)> {
        let (x, n) = parse_uint(s, cx)?;
        let (mul, add) = (self.mul as u64, self.add as u64);
        if x < add || (x - add) % mul != 0 {
            return None;
        }
        Some((((x - add) / mul) as u32, n))
    }
}

/// An unsigned 32-bit number at the start of `s`: decimal, hex with a 0x
/// prefix, or a name the context's resolver knows. The value and the bytes read.
pub fn parse_uint(s: &str, cx: &Ctx) -> Option<(u64, usize)> {
    let b = s.as_bytes();
    let word = |c: u8| c.is_ascii_alphanumeric() || c == b'_';
    if let (Some(r), Some(&c)) = (cx.resolve, b.first())
        && (c.is_ascii_alphabetic() || c == b'_')
    {
        let n = b.iter().take_while(|&&c| word(c) || c == b'.').count();
        let x = r(&s[..n])?;
        return (x <= 0xffff_ffff).then_some((x, n));
    }
    let (radix, at) = match b {
        [b'0', b'x' | b'X', c, ..] if c.is_ascii_hexdigit() => (16, 2),
        [c, ..] if c.is_ascii_digit() => (10, 0),
        _ => return None,
    };
    let mut x = 0u64;
    let mut n = at;
    while let Some(d) = b.get(n).and_then(|&c| (c as char).to_digit(radix)) {
        x = x * radix as u64 + d as u64;
        if x > 0xffff_ffff {
            return None;
        }
        n += 1;
    }
    if b.get(n).is_some_and(|&c| word(c)) {
        return None;
    }
    Some((x, n))
}

/// Several one-bit flags, printed in field order: each bit's name.
pub struct Flags(pub &'static [&'static str]);

impl Kind for Flags {
    fn valid(&self, v: u32, _: &Ctx) -> bool {
        v >> self.0.len() == 0
    }

    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        for (i, name) in self.0.iter().enumerate() {
            if v >> i & 1 != 0 {
                out.push_str(name);
            }
        }
        v >> self.0.len() == 0
    }
}

/// A sequence of small enumerations in one field, printed one after another:
/// the field's chunks, LSB first, are the subfields in print order. Each
/// subfield names its 2^w values (None marks an invalid one).
pub struct Seq(pub &'static [(u32, &'static [Option<&'static str>])]);

impl Kind for Seq {
    fn valid(&self, mut v: u32, _: &Ctx) -> bool {
        self.0.iter().all(|&(w, t)| {
            let ok = t[(v & ((1 << w) - 1)) as usize].is_some();
            v >>= w;
            ok
        })
    }

    fn print(&self, mut v: u32, _: &Ctx, out: &mut String) -> bool {
        for &(w, t) in self.0 {
            match t[(v & ((1 << w) - 1)) as usize] {
                Some(s) => out.push_str(s),
                None => return false,
            }
            v >>= w;
        }
        true
    }
}

/// One item of a blend selection: a selector (w bits, its 2^w names, None
/// invalid) followed by its complement bit when `comp` is set (".comp").
pub struct SelItem {
    pub t: &'static [Option<&'static str>],
    pub w: u32,
    pub comp: bool,
}

/// A blend selection: items printed ", "-separated. v = [complement-all] |
/// items in order | [neg]; `pre` adds a leading bit printed as "comp, ",
/// `neg` a trailing one printed as ", neg".
pub struct Sel {
    pub pre: bool,
    pub neg: bool,
    pub it: &'static [SelItem],
}

impl Kind for Sel {
    fn print(&self, v: u32, _: &Ctx, out: &mut String) -> bool {
        let mut sh = 0;
        if self.pre {
            if v & 1 != 0 {
                out.push_str("comp, ");
            }
            sh += 1;
        }
        for (i, it) in self.it.iter().enumerate() {
            let Some(name) = it.t[(v >> sh & ((1 << it.w) - 1)) as usize] else { return false };
            let c = it.comp && v >> (sh + it.w) & 1 != 0;
            sh += it.w + it.comp as u32;
            if i > 0 {
                out.push_str(", ");
            }
            out.push_str(name);
            if c {
                out.push_str(".comp");
            }
        }
        if self.neg && v >> sh & 1 != 0 {
            out.push_str(", neg");
        }
        true
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

/// `names!["a", None, "b"]`: an enumeration table.
macro_rules! names {
    (@one None) => { None };
    (@one $s:literal) => { Some($s) };
    ($($x:tt),* $(,)?) => { &[$(names!(@one $x)),*] };
}

pub(crate) use names;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fields_scatter_and_gather_lsb_first() {
        static K: Num = Num { radix: Radix::Dec, mul: 1, add: 0 };
        let f = Field { k: &K, c: &[Chunk { lo: 4, w: 2 }, Chunk { lo: 60, w: 3 }], r: "" };
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
