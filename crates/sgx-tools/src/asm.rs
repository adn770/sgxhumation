// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The source-file driver shared by usse-asm and pds-asm (the Rust side of
//! tools/usse-asm/asm.c, with the same behaviour, messages included).
//!
//! One statement per line:
//!
//!   label:                       a label (may share the line with a statement)
//!   <instruction>                as the disassembler prints it
//!   .long 0x0123456789abcdef     a raw USSE word (.word for a PDS dword); several
//!                                values may follow, separated by commas
//!   ; comment                    to the end of the line (also C comments)
//!
//! Disassembler listings ("<index>: 0x<hex>\t<text>") are accepted too: the
//! index and hex columns are ignored, except that a PDS "data" line takes its
//! value from the hex column. In PDS mode the raw words before the first
//! instruction form the data segment, and a label's value is its instruction
//! index counted from the first instruction (as bra/call count).

use crate::expr;
use std::collections::HashMap;
use std::fmt::Write as _;
use std::io::{BufRead, Read, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::rc::Rc;
use usse_isa::Core;

/// What differs between the two assemblers.
pub struct Isa {
    /// program name for messages
    pub name: &'static str,
    /// bytes per word: 8 (USSE) or 4 (PDS)
    pub width: usize,
    /// raw-word directive: ".long" or ".word"
    pub raw: &'static str,
    /// raw words before the first instruction are data, and labels count from
    /// the first instruction
    pub data_segment: bool,
    pub encode: fn(&str, &usse_isa::Ctx) -> Option<u64>,
    /// the form table, for --explain
    pub table: fn() -> &'static usse_isa::Table,
    /// one listing line, as the matching disassembler prints it
    pub list: fn(idx: usize, w: u64, is_data: bool, core: Core) -> String,
}

fn usse_encode(text: &str, cx: &usse_isa::Ctx) -> Option<u64> {
    usse_isa::encode_with(text, cx)
}

fn usse_list(idx: usize, w: u64, _: bool, core: Core) -> String {
    format!("{idx:6}: 0x{w:016X}\t{}", usse_isa::decode(w, core).text)
}

fn pds_encode(text: &str, cx: &usse_isa::Ctx) -> Option<u64> {
    pds_isa::encode_with(text, cx).map(u64::from)
}

fn pds_list(idx: usize, w: u64, is_data: bool, _: Core) -> String {
    let text = if is_data { "data".to_string() } else { pds_isa::decode(w as u32).text };
    format!("{idx}: 0x{w:08X}\t{text}")
}

fn data_list(idx: usize, w: u64, _: bool, _: Core) -> String {
    format!("{idx}: 0x{w:08X}")
}

pub const USSE: Isa = Isa {
    name: "usse-asm",
    width: 8,
    raw: ".long",
    data_segment: false,
    encode: usse_encode,
    table: usse_isa::table,
    list: usse_list,
};

pub const PDS: Isa = Isa {
    name: "pds-asm",
    width: 4,
    raw: ".word",
    data_segment: true,
    encode: pds_encode,
    table: pds_isa::table,
    list: pds_list,
};

/// sgx-asm's data sections: 32-bit words only, no instructions.
pub const DATA: Isa = Isa {
    name: "sgx-asm",
    width: 4,
    raw: ".word",
    data_segment: false,
    encode: |_, _| None,
    table: pds_isa::table,
    list: data_list,
};

/// A section's ISA as sgx-asm names it.
fn isa_name(isa: &Isa) -> &'static str {
    match (isa.width, isa.data_segment) {
        (8, _) => "usse",
        (_, true) => "pds",
        _ => "data",
    }
}

/// Patch encodings and kinds, by their numbers in the firmware format
/// (design/sgx545_fw_format.md §4, §9.4).
pub const ENCODINGS: [&str; 4] = ["word32", "usse_limm", "code_base", "emitpix_sb"];
const ENC_WORD32: usize = 0;
const ENC_USSE_LIMM: usize = 1;

pub struct PatchKind {
    pub name: &'static str,
    /// how many arguments it takes
    min: usize,
    max: usize,
    /// what the first of two arguments names (one argument of `const` and
    /// `pd_phys` is the addend)
    target: Target,
}

#[derive(Clone, Copy, PartialEq)]
enum Target {
    /// no target: the argument is the addend
    None,
    /// a section (its id) or a number
    Section,
    /// a USSE label (its instruction index) or a number
    Label,
    /// a number
    Number,
}

const fn kind(name: &'static str, min: usize, target: Target) -> PatchKind {
    // a target and an optional addend, or only the addend
    let max = if matches!(target, Target::None) { 1 } else { 2 };
    PatchKind { name, min, max, target }
}

pub const KINDS: [PatchKind; 10] = [
    kind("const", 1, Target::None),
    kind("addr", 1, Target::Section),
    kind("addr_shr4", 1, Target::Section),
    kind("heap", 1, Target::Number),
    kind("pd_phys", 0, Target::None),
    kind("code_word", 1, Target::Label),
    kind("pds_ctl", 1, Target::Section),
    kind("pds_rows", 1, Target::Section),
    kind("param", 1, Target::Number),
    kind("doutu", 1, Target::Section),
];

impl PatchKind {
    fn counts(&self) -> &'static str {
        match (self.min, self.max) {
            (1, 1) => "1 argument",
            (0, 1) => "0 or 1 arguments",
            _ => "1 or 2 arguments",
        }
    }
}

/// How deep .include and macro calls may nest.
const MAX_INCLUDES: usize = 16;
const MAX_MACROS: usize = 64;

/// Where a statement comes from, for messages: a macro's statements are
/// placed at the line that called it.
#[derive(Clone)]
struct Pos {
    file: Rc<str>,
    line: usize,
}

struct Item {
    text: Option<String>,
    raw: u64,
    pos: Pos,
}

/// One program or data block: the single-ISA tools have one, unnamed;
/// sgx-asm one per .section.
struct Section<'a> {
    name: String,
    isa: &'a Isa,
    id: u32,
    items: Vec<Item>,
}

struct Label {
    name: String,
    section: usize,
    item: usize,
}

/// Where a patch record lands: on an item, or at a byte offset.
enum At {
    Item(usize),
    Byte(u64),
}

/// A patch as written; its arguments are resolved once every label and
/// section is known.
struct Patch {
    section: usize,
    at: At,
    encoding: usize,
    kind: usize,
    args: Vec<String>,
    pos: Pos,
}

struct Macro {
    /// each parameter with its default
    params: Vec<(String, Option<String>)>,
    body: Vec<String>,
}

/// A .macro whose body is being read.
struct Recording {
    name: String,
    /// false when the name was taken: the body is read and dropped
    keep: bool,
    params: Vec<(String, Option<String>)>,
    body: Vec<String>,
    /// .macro lines inside the body still open
    nested: usize,
    pos: Pos,
}

struct State<'a> {
    sections: Vec<Section<'a>>,
    /// the section statements go to (always 0 in the single-ISA tools)
    cur: Option<usize>,
    /// sgx-asm: .section, .patch and patch calls
    multi: bool,
    labels: Vec<Label>,
    patches: Vec<Patch>,
    /// .patch records waiting for the next item
    pending: Vec<Patch>,
    equs: HashMap<String, i128>,
    macros: HashMap<String, Rc<Macro>>,
    recording: Option<Recording>,
    /// macro calls so far, for \@
    calls: usize,
    errors: usize,
    err: Vec<u8>,
}

fn is_space(c: u8) -> bool {
    c.is_ascii_whitespace() || c == 0x0b
}

fn trim(s: &str) -> &str {
    s.trim_matches(|c: char| c.is_ascii() && is_space(c as u8))
}

fn ident_start(c: u8) -> bool {
    c.is_ascii_alphabetic() || c == b'_'
}

fn ident_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_' || c == b'.'
}

/// The first word of a statement and the rest, trimmed.
fn split_word(s: &str) -> (&str, &str) {
    let n = s.bytes().take_while(|&c| !is_space(c)).count();
    (&s[..n], trim(&s[n..]))
}

/// Comma-separated values, commas inside () or [] not counting; an empty
/// text is one empty value.
fn split_args(s: &str) -> Vec<&str> {
    let (mut out, mut depth, mut from) = (Vec::new(), 0i32, 0);
    for (i, c) in s.bytes().enumerate() {
        match c {
            b'(' | b'[' => depth += 1,
            b')' | b']' => depth -= 1,
            b',' if depth == 0 => {
                out.push(trim(&s[from..i]));
                from = i + 1;
            }
            _ => {}
        }
    }
    out.push(trim(&s[from..]));
    out
}

/// The index of the ')' closing the '(' at `open`.
fn closing(b: &[u8], open: usize) -> Option<usize> {
    let mut depth = 0;
    for (i, &c) in b.iter().enumerate().skip(open) {
        match c {
            b'(' => depth += 1,
            b')' if depth == 1 => return Some(i),
            b')' => depth -= 1,
            _ => {}
        }
    }
    None
}

/// A macro body line with its arguments in place: \name, and \@ for the
/// number of the call.
fn substitute(line: &str, names: &[(String, Option<String>)], values: &[String], call: usize) -> String {
    let mut out = String::with_capacity(line.len());
    let mut rest = line;
    while let Some(i) = rest.find('\\') {
        out.push_str(&rest[..i]);
        let after = &rest[i + 1..];
        if let Some(r) = after.strip_prefix('@') {
            out.push_str(&call.to_string());
            rest = r;
            continue;
        }
        let n = after.bytes().take_while(|&c| c.is_ascii_alphanumeric() || c == b'_').count();
        match names.iter().position(|(p, _)| *p == after[..n]) {
            Some(k) => out.push_str(&values[k]),
            None => out.push_str(&rest[i..i + 1 + n]),
        }
        rest = &after[n..];
    }
    out.push_str(rest);
    out
}

impl State<'_> {
    fn error(&mut self, pos: &Pos, msg: String) {
        let _ = writeln!(self.err, "{}:{}: {msg}", pos.file, pos.line);
        self.errors += 1;
    }

    /// Read a source: `dir` is where its .include names are looked up.
    fn feed(&mut self, text: &[u8], file: Rc<str>, dir: &Path, includes: usize) {
        let mut lines: Vec<&[u8]> = text.split(|&c| c == b'\n').collect();
        if lines.last().is_some_and(|l| l.is_empty()) {
            lines.pop();
        }
        for (n, line) in lines.iter().enumerate() {
            let pos = Pos { file: file.clone(), line: n + 1 };
            self.statement(&String::from_utf8_lossy(line), &pos, dir, includes, 0);
        }
    }

    fn statement(&mut self, raw_line: &str, pos: &Pos, dir: &Path, includes: usize, calls: usize) {
        let mut s = strip_c_comments(raw_line);
        if let Some(c) = s.find(';') {
            s.truncate(c);
        }
        let mut s = trim(&s);
        if self.recording.is_some() {
            self.record(s);
            return;
        }
        if let Some((hex, rest)) = listing(s) {
            if rest == "data" {
                self.push(Item { text: None, raw: hex, pos: pos.clone() }, pos, Vec::new());
            } else if !rest.is_empty() {
                self.push(Item { text: Some(rest.to_string()), raw: 0, pos: pos.clone() }, pos, Vec::new());
            }
            return;
        }
        // labels
        while s.as_bytes().first().is_some_and(|&c| ident_start(c)) {
            let n = s.bytes().take_while(|&c| ident_char(c)).count();
            if s.as_bytes().get(n) != Some(&b':') {
                break;
            }
            let name = &s[..n];
            if self.labels.iter().any(|l| l.name == name) {
                self.error(pos, format!("label '{name}' defined twice"));
            }
            match self.cur {
                Some(section) => {
                    let item = self.sections[section].items.len();
                    self.labels.push(Label { name: name.to_string(), section, item });
                }
                None => self.error(pos, "statement outside a section".to_string()),
            }
            s = trim(&s[n + 1..]);
        }
        if s.is_empty() {
            return;
        }
        if let Some(isa) = self.isa() {
            let raw = isa.raw;
            if s.starts_with(raw) && s.as_bytes().get(raw.len()).is_some_and(|&c| is_space(c)) {
                self.raw_words(trim(&s[raw.len()..]), pos);
                return;
            }
        }
        let (word, rest) = split_word(s);
        match word {
            ".include" => self.include(rest, pos, dir, includes),
            ".equ" => self.equ(rest, pos),
            ".macro" => self.start_macro(rest, pos),
            ".endm" => self.error(pos, ".endm without .macro".to_string()),
            ".section" if self.multi => self.section(rest, pos),
            ".patch" if self.multi => self.patch_directive(rest, pos),
            w if self.macros.contains_key(w) => self.call(w, rest, pos, dir, includes, calls),
            _ if self.cur.is_none() => self.error(pos, "statement outside a section".to_string()),
            _ => {
                let mut inline = Vec::new();
                let text = self.operands(s, pos, &mut inline);
                self.push(Item { text: Some(text), raw: 0, pos: pos.clone() }, pos, inline);
            }
        }
    }

    /// The current section's ISA.
    fn isa(&self) -> Option<&Isa> {
        self.cur.map(|c| self.sections[c].isa)
    }

    /// Add an item to the current section, with the .patch records waiting
    /// for it, then its own (`inline`, as (encoding, kind, args)).
    fn push(&mut self, item: Item, pos: &Pos, inline: Vec<(usize, usize, Vec<String>)>) {
        let section = self.cur.expect("in a section");
        let idx = self.sections[section].items.len();
        for mut p in std::mem::take(&mut self.pending) {
            p.at = At::Item(idx);
            self.patches.push(p);
        }
        for (encoding, kind, args) in inline {
            self.patches.push(Patch { section, at: At::Item(idx), encoding, kind, args, pos: pos.clone() });
        }
        self.sections[section].items.push(item);
    }

    /// `kind(args)` at the start of `s`, `kind` a patch kind: the kind, its
    /// arguments and the bytes read. Wrong argument counts are reported.
    fn patch_call(&mut self, s: &str, pos: &Pos) -> Option<(usize, Vec<String>, usize)> {
        let n = s.bytes().take_while(|&c| c.is_ascii_alphanumeric() || c == b'_').count();
        let kind = KINDS.iter().position(|k| k.name == &s[..n])?;
        if s.as_bytes().get(n) != Some(&b'(') {
            return None;
        }
        let close = closing(s.as_bytes(), n)?;
        let inner = trim(&s[n + 1..close]);
        let args: Vec<String> = if inner.is_empty() {
            Vec::new()
        } else {
            split_args(inner).into_iter().map(String::from).collect()
        };
        let k = &KINDS[kind];
        if args.len() < k.min || args.len() > k.max {
            self.error(pos, format!("{} takes {}", k.name, k.counts()));
        }
        Some((kind, args, close + 1))
    }

    /// .section name, usse|pds|data, id
    fn section(&mut self, rest: &str, pos: &Pos) {
        if !self.pending.is_empty() {
            self.dangling();
        }
        let args = split_args(rest);
        let [name, isa, id] = args[..] else {
            self.error(pos, ".section needs a name, an isa and an id".to_string());
            return;
        };
        let isa = match isa {
            "usse" => &USSE,
            "pds" => &PDS,
            "data" => &DATA,
            _ => {
                self.error(pos, "section isa must be usse, pds or data".to_string());
                return;
            }
        };
        if !expr::is_name(name) {
            self.error(pos, format!("bad section name '{name}'"));
            return;
        }
        if self.sections.iter().any(|s| s.name == name) {
            self.error(pos, format!("section '{name}' defined twice"));
            return;
        }
        let Some(id) = expr::eval(id, |n| self.equs.get(n).copied()).ok().and_then(|v| u32::try_from(v).ok())
        else {
            self.error(pos, format!("bad section id '{id}'"));
            return;
        };
        self.sections.push(Section { name: name.to_string(), isa, id, items: Vec::new() });
        self.cur = Some(self.sections.len() - 1);
    }

    /// .patch encoding, kind(args) [, byte offset]: without an offset, the
    /// record goes on the next item.
    fn patch_directive(&mut self, rest: &str, pos: &Pos) {
        let Some(section) = self.cur else {
            self.error(pos, "statement outside a section".to_string());
            return;
        };
        let args = split_args(rest);
        let Some(encoding) = ENCODINGS.iter().position(|&e| e == args[0]) else {
            self.error(pos, format!("unknown patch encoding '{}'", args[0]));
            return;
        };
        let call = args.get(1).copied().unwrap_or("");
        let Some((kind, kargs, _)) = self.patch_call(call, pos).filter(|c| c.2 == call.len()) else {
            self.error(pos, format!("bad .patch value '{call}'"));
            return;
        };
        let mut p = Patch { section, at: At::Item(0), encoding, kind, args: kargs, pos: pos.clone() };
        match args.get(2) {
            None => self.pending.push(p),
            Some(off) => {
                match expr::eval(off, |n| self.equs.get(n).copied()).ok().and_then(|v| u64::try_from(v).ok())
                {
                    Some(b) => {
                        p.at = At::Byte(b);
                        self.patches.push(p);
                    }
                    None => self.error(pos, format!("bad .patch offset '{off}'")),
                }
            }
        }
    }

    fn dangling(&mut self) {
        for p in std::mem::take(&mut self.pending) {
            self.error(&p.pos, ".patch without a statement after it".to_string());
        }
    }

    /// .long / .word: values, numbers or expressions, separated by commas.
    fn raw_words(&mut self, list: &str, pos: &Pos) {
        let (width, raw) = self.isa().map(|i| (i.width, i.raw)).expect("in a section");
        for v in split_args(list) {
            if self.multi
                && let Some((kind, args, n)) = self.patch_call(v, pos)
                && n == v.len()
            {
                if width != 4 {
                    self.error(pos, "a patch needs a 32-bit word here".to_string());
                    return;
                }
                self.push(Item { text: None, raw: 0, pos: pos.clone() }, pos, vec![(ENC_WORD32, kind, args)]);
                continue;
            }
            let value = match parse_raw(v, width) {
                Some((x, n)) if n == v.len() => Some(x),
                _ => expr::eval(v, |n| self.equs.get(n).copied()).ok().and_then(|x| {
                    let bits = 8 * width as u32;
                    (x >= -(1i128 << (bits - 1)) && x < 1i128 << bits)
                        .then_some((x as u64) & (u64::MAX >> (64 - bits)))
                }),
            };
            match value {
                Some(x) => self.push(Item { text: None, raw: x, pos: pos.clone() }, pos, Vec::new()),
                None => {
                    self.error(pos, format!("bad {raw} value"));
                    return;
                }
            }
        }
    }

    /// An instruction's text with each .equ name, and each parenthesised
    /// expression, replaced by its value. Parentheses that are not an
    /// expression (a predicate) stay. In sgx-asm a patch call becomes 0 and a
    /// USSE_LIMM record in `inline`.
    fn operands(&mut self, s: &str, pos: &Pos, inline: &mut Vec<(usize, usize, Vec<String>)>) -> String {
        let b = s.as_bytes();
        let mut out = Vec::with_capacity(b.len());
        let mut i = 0;
        while i < b.len() {
            let c = b[i];
            if c == b'('
                && let Some(j) = closing(b, i)
            {
                match expr::eval(&s[i + 1..j], |n| self.equs.get(n).copied()) {
                    Ok(v) => {
                        out.extend_from_slice(v.to_string().as_bytes());
                        i = j + 1;
                        continue;
                    }
                    Err(expr::Error::DivZero) => self.error(pos, "division by zero".to_string()),
                    Err(_) => {}
                }
            } else if ident_start(c) && (i == 0 || !ident_char(b[i - 1])) {
                if self.multi
                    && let Some((kind, args, n)) = self.patch_call(&s[i..], pos)
                {
                    if self.isa().is_some_and(|isa| isa.width == 8) {
                        inline.push((ENC_USSE_LIMM, kind, args));
                    } else {
                        let isa = isa_name(self.sections[self.cur.unwrap()].isa);
                        self.error(pos, format!("a patch in a {isa} instruction"));
                    }
                    out.push(b'0');
                    i += n;
                    continue;
                }
                let n = b[i..].iter().take_while(|&&c| c.is_ascii_alphanumeric() || c == b'_').count();
                match self.equs.get(&s[i..i + n]) {
                    Some(v) => out.extend_from_slice(v.to_string().as_bytes()),
                    None => out.extend_from_slice(&b[i..i + n]),
                }
                i += n;
                continue;
            }
            out.push(c);
            i += 1;
        }
        String::from_utf8(out).expect("only ASCII replaced")
    }

    fn include(&mut self, rest: &str, pos: &Pos, dir: &Path, includes: usize) {
        let Some(name) = rest.strip_prefix('"').and_then(|r| r.strip_suffix('"')) else {
            self.error(pos, "bad .include: the name goes in double quotes".to_string());
            return;
        };
        if includes >= MAX_INCLUDES {
            self.error(pos, "includes nested too deeply".to_string());
            return;
        }
        let path = dir.join(name);
        match std::fs::read(&path) {
            Ok(text) => {
                let sub = path.parent().map(Path::to_path_buf).unwrap_or_default();
                self.feed(&text, path.display().to_string().into(), &sub, includes + 1);
            }
            Err(e) => self.error(pos, format!("cannot include '{name}': {}", strerror(&e))),
        }
    }

    fn equ(&mut self, rest: &str, pos: &Pos) {
        let Some((name, value)) = rest.split_once(',').map(|(n, v)| (trim(n), trim(v))) else {
            self.error(pos, ".equ needs a name and a value".to_string());
            return;
        };
        if name.is_empty() || value.is_empty() {
            self.error(pos, ".equ needs a name and a value".to_string());
        } else if !expr::is_name(name) {
            self.error(pos, format!("bad .equ name '{name}'"));
        } else if self.equs.contains_key(name) {
            self.error(pos, format!("symbol '{name}' defined twice"));
        } else {
            match expr::eval(value, |n| self.equs.get(n).copied()) {
                Ok(v) => {
                    self.equs.insert(name.to_string(), v);
                }
                Err(expr::Error::Undefined(n)) => self.error(pos, format!("undefined symbol '{n}'")),
                Err(expr::Error::DivZero) => self.error(pos, "division by zero".to_string()),
                Err(expr::Error::Syntax) => self.error(pos, "bad .equ value".to_string()),
            }
        }
    }

    /// .macro name [param[=default], ...]
    fn start_macro(&mut self, rest: &str, pos: &Pos) {
        let (name, params) = split_word(rest);
        let mut ok = expr::is_name(name);
        if !ok {
            self.error(pos, format!("bad .macro name '{name}'"));
        } else if self.macros.contains_key(name) {
            self.error(pos, format!("macro '{name}' defined twice"));
            ok = false;
        }
        let mut list = Vec::new();
        for p in params.split(|c: char| c == ',' || c.is_ascii_whitespace()).filter(|p| !p.is_empty()) {
            let (p, default) = match p.split_once('=') {
                Some((p, d)) => (p, Some(d.to_string())),
                None => (p, None),
            };
            if !expr::is_name(p) {
                self.error(pos, format!("bad .macro parameter '{p}'"));
                ok = false;
            }
            list.push((p.to_string(), default));
        }
        self.recording = Some(Recording {
            name: name.to_string(),
            keep: ok,
            params: list,
            body: Vec::new(),
            nested: 0,
            pos: pos.clone(),
        });
    }

    /// A line of the macro being read; .endm ends it.
    fn record(&mut self, s: &str) {
        let rec = self.recording.as_mut().expect("recording");
        match split_word(s).0 {
            ".macro" => rec.nested += 1,
            ".endm" if rec.nested == 0 => {
                let rec = self.recording.take().expect("recording");
                if rec.keep {
                    let m = Macro { params: rec.params, body: rec.body };
                    self.macros.insert(rec.name, Rc::new(m));
                }
                return;
            }
            ".endm" => rec.nested -= 1,
            _ => {}
        }
        rec.body.push(s.to_string());
    }

    fn call(&mut self, name: &str, rest: &str, pos: &Pos, dir: &Path, includes: usize, calls: usize) {
        if calls >= MAX_MACROS {
            self.error(pos, "macros nested too deeply".to_string());
            return;
        }
        let m = self.macros[name].clone();
        let args = if rest.is_empty() { Vec::new() } else { split_args(rest) };
        if args.len() > m.params.len() {
            let msg = format!("macro '{name}' takes {} argument(s), got {}", m.params.len(), args.len());
            self.error(pos, msg);
            return;
        }
        let mut values = Vec::new();
        for (k, (p, default)) in m.params.iter().enumerate() {
            match args.get(k).filter(|a| !a.is_empty()).map(|a| a.to_string()).or(default.clone()) {
                Some(v) => values.push(v),
                None => {
                    self.error(pos, format!("macro '{name}' needs a value for '{p}'"));
                    return;
                }
            }
        }
        self.calls += 1;
        let call = self.calls;
        for line in &m.body {
            let text = substitute(line, &m.params, &values, call);
            self.statement(&text, pos, dir, includes, calls + 1);
        }
    }
}

/// Blank out C comments, so that a ';' inside one does not end the line.
fn strip_c_comments(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut t = s;
    while let Some(c) = t.find("/*") {
        out.push_str(&t[..c]);
        let end = t[c + 2..].find("*/").map_or(t.len(), |e| c + 2 + e + 2);
        out.extend(std::iter::repeat_n(' ', end - c));
        t = &t[end..];
    }
    out.push_str(t);
    out
}

/// A disassembler listing prefix "<n>: 0x<hex>" followed by blanks: the hex
/// value and the rest of the line.
fn listing(s: &str) -> Option<(u64, &str)> {
    let b = s.as_bytes();
    let mut p = b.iter().take_while(|c| c.is_ascii_digit()).count();
    if p == 0 || b.get(p) != Some(&b':') {
        return None;
    }
    p += 1;
    while matches!(b.get(p), Some(b' ' | b'\t')) {
        p += 1;
    }
    if b.get(p) != Some(&b'0') || !matches!(b.get(p + 1), Some(b'x' | b'X')) {
        return None;
    }
    let h = p + 2;
    let e = h + b[h..].iter().take_while(|c| c.is_ascii_hexdigit()).count();
    if e == h || !b.get(e).is_some_and(|&c| is_space(c)) {
        return None;
    }
    let v = u64::from_str_radix(&s[h..e], 16).ok()?;
    Some((v, trim(&s[e..])))
}

/// A raw word: decimal or 0x hex, at most `width` bytes; the value and the
/// bytes read.
fn parse_raw(p: &str, width: usize) -> Option<(u64, usize)> {
    let b = p.as_bytes();
    let hex = b.first() == Some(&b'0') && matches!(b.get(1), Some(b'x' | b'X'));
    let at = if hex { 2 } else { 0 };
    let first_ok = b.get(at).is_some_and(|c| if hex { c.is_ascii_hexdigit() } else { c.is_ascii_digit() });
    if !first_ok {
        return None;
    }
    let radix = if hex { 16 } else { 10 };
    let e = at + b[at..].iter().take_while(|&&c| (c as char).is_digit(radix)).count();
    let v = u64::from_str_radix(&p[at..e], radix).ok()?;
    if b.get(e).is_some_and(|&c| ident_char(c)) {
        return None;
    }
    (width == 8 || v <= 0xffff_ffff).then_some((v, e))
}

fn usage(name: &str) -> ExitCode {
    eprintln!("usage: {name} [-l] [-o out.bin] [--core 540|545] <source.s | ->");
    ExitCode::from(2)
}

/// C's perror text for an I/O error: strerror(), without Rust's "(os error N)".
fn strerror(e: &std::io::Error) -> String {
    let s = e.to_string();
    match s.find(" (os error") {
        Some(i) => s[..i].to_string(),
        None => s,
    }
}

/// C's atoi, as far as picking a core needs.
fn atoi(s: &str) -> i64 {
    let t = trim(s);
    let n = t
        .bytes()
        .enumerate()
        .take_while(|&(i, c)| c.is_ascii_digit() || (i == 0 && (c == b'-' || c == b'+')))
        .count();
    t[..n].parse().unwrap_or(0)
}

/// An assembled source: its words, how many lead as data (PDS), the
/// messages (as the C assembler prints them) and the number of errors.
pub struct Assembled {
    pub words: Vec<u64>,
    pub ndata: usize,
    pub messages: Vec<u8>,
    pub errors: usize,
}

/// The --explain lines under a "cannot assemble" message (the Rust tools'
/// own): the forms that matched the most of the text, or that none read its
/// mnemonic.
fn explain(isa: &Isa, text: &str, cx: &usse_isa::Ctx) -> String {
    let mnemonic = text.split_whitespace().next().unwrap_or("");
    let near: Vec<_> = usse_isa::encode::explain_in((isa.table)(), text, cx, 3)
        .into_iter()
        .filter(|n| n.1 >= mnemonic.len())
        .collect();
    if near.is_empty() {
        return format!("  no form reads the mnemonic '{mnemonic}'\n");
    }
    near.iter()
        .map(|(f, n, rest)| format!("  form #{f} matched '{}', expected '{rest}'\n", &text[..*n]))
        .collect()
}

/// Assemble a source; `file` names it in the messages. With `hints`, each
/// "cannot assemble" message is followed by the closest forms (--explain).
pub fn assemble(isa: &Isa, text: &[u8], file: &str, core: Core) -> Assembled {
    assemble_hinted(isa, text, file, core, false)
}

pub fn assemble_hinted(isa: &Isa, text: &[u8], file: &str, core: Core, hints: bool) -> Assembled {
    let mut p = run(Some(isa), text, file, core, hints);
    let s = p.sections.pop().expect("one section");
    Assembled { words: s.words, ndata: s.ndata, messages: p.messages, errors: p.errors }
}

/// One assembled section of a program.
pub struct SectionOut<'a> {
    pub name: String,
    pub isa: &'a Isa,
    pub id: u32,
    pub words: Vec<u64>,
    /// the leading data words (PDS)
    pub ndata: usize,
}

impl SectionOut<'_> {
    /// "usse", "pds" or "data"
    pub fn isa_name(&self) -> &'static str {
        isa_name(self.isa)
    }
}

/// A label: its section, its value as branches count it (none for a label
/// on PDS data) and its byte offset in the section.
pub struct Symbol {
    pub name: String,
    pub section: usize,
    pub index: Option<u64>,
    pub offset: u64,
}

/// A patch record (design/sgx545_fw_format.md §4): in `section` at byte
/// `offset`, an `ENCODINGS` and a `KINDS` entry by index.
pub struct PatchOut {
    pub section: usize,
    pub offset: u64,
    pub encoding: usize,
    pub kind: usize,
    pub target: u32,
    pub addend: u32,
}

/// An sgx-asm program: sections, labels, patch records, and the messages.
pub struct Program<'a> {
    pub sections: Vec<SectionOut<'a>>,
    pub symbols: Vec<Symbol>,
    pub patches: Vec<PatchOut>,
    pub messages: Vec<u8>,
    pub errors: usize,
}

/// Assemble an sgx-asm source (.section, .patch and patch calls).
pub fn assemble_program(text: &[u8], file: &str, core: Core, hints: bool) -> Program<'static> {
    run(None, text, file, core, hints)
}

/// With `single`, the source is one section of that ISA and sgx-asm's
/// directives are unknown.
fn run<'a>(single: Option<&'a Isa>, text: &[u8], file: &str, core: Core, hints: bool) -> Program<'a> {
    let mut st = State {
        sections: Vec::new(),
        cur: None,
        multi: single.is_none(),
        labels: Vec::new(),
        patches: Vec::new(),
        pending: Vec::new(),
        equs: HashMap::new(),
        macros: HashMap::new(),
        recording: None,
        calls: 0,
        errors: 0,
        err: Vec::new(),
    };
    if let Some(isa) = single {
        st.sections.push(Section { name: String::new(), isa, id: 0, items: Vec::new() });
        st.cur = Some(0);
    }
    // .include names are relative to the including file; from stdin, to the
    // working directory
    let dir = match file {
        "<stdin>" => PathBuf::new(),
        f => Path::new(f).parent().map(Path::to_path_buf).unwrap_or_default(),
    };
    st.feed(text, file.into(), &dir, 0);
    if let Some(rec) = st.recording.take() {
        st.error(&rec.pos, format!(".macro '{}' without .endm", rec.name));
    }
    st.dangling();

    let labels = std::mem::take(&mut st.labels);
    let sections = std::mem::take(&mut st.sections);
    let mut out = Vec::new();
    for (si, sec) in sections.into_iter().enumerate() {
        let isa = sec.isa;
        let ndata =
            if isa.data_segment { sec.items.iter().take_while(|it| it.text.is_none()).count() } else { 0 };
        let resolve = |name: &str| -> Option<u64> {
            let l = labels.iter().find(|l| l.name == name && l.section == si)?;
            (l.item >= ndata).then(|| (l.item - ndata) as u64)
        };
        let cx = usse_isa::Ctx { core, resolve: Some(&resolve) };
        let mut words = vec![0u64; sec.items.len()];
        for (w, it) in words.iter_mut().zip(&sec.items) {
            match &it.text {
                None => *w = it.raw,
                Some(t) => match (isa.encode)(t, &cx) {
                    Some(v) => *w = v,
                    None => {
                        st.error(&it.pos, format!("cannot assemble '{t}'"));
                        if hints {
                            let h = explain(isa, t, &cx);
                            st.err.extend_from_slice(h.as_bytes());
                        }
                    }
                },
            }
        }
        out.push(SectionOut { name: sec.name, isa, id: sec.id, words, ndata });
    }

    let symbols = labels
        .iter()
        .map(|l| {
            let s = &out[l.section];
            Symbol {
                name: l.name.clone(),
                section: l.section,
                index: (l.item >= s.ndata).then(|| (l.item - s.ndata) as u64),
                offset: (l.item * s.isa.width) as u64,
            }
        })
        .collect();
    let mut patches = Vec::new();
    for p in std::mem::take(&mut st.patches) {
        if let Some(r) = st.resolve_patch(&p, &out, &labels) {
            patches.push(r);
        }
    }
    Program { sections: out, symbols, patches, messages: st.err, errors: st.errors }
}

impl State<'_> {
    fn resolve_patch(&mut self, p: &Patch, sections: &[SectionOut], labels: &[Label]) -> Option<PatchOut> {
        let k = &KINDS[p.kind];
        let (target, addend) = match k.target {
            Target::None => (None, p.args.first()),
            _ => (p.args.first(), p.args.get(1)),
        };
        let target = match target {
            None => 0,
            Some(a) => self.target(k, a, sections, labels, &p.pos)?,
        };
        let addend = match addend {
            None => 0,
            Some(a) => self.word32(a, &p.pos)?,
        };
        let offset = match p.at {
            At::Item(i) => (i * sections[p.section].isa.width) as u64,
            At::Byte(b) => b,
        };
        Some(PatchOut { section: p.section, offset, encoding: p.encoding, kind: p.kind, target, addend })
    }

    fn target(
        &mut self,
        k: &PatchKind,
        a: &str,
        sections: &[SectionOut],
        labels: &[Label],
        pos: &Pos,
    ) -> Option<u32> {
        let unknown = expr::is_name(a) && !self.equs.contains_key(a);
        match k.target {
            Target::Section => {
                if let Some(s) = sections.iter().find(|s| s.name == a) {
                    return Some(s.id);
                }
                if unknown {
                    self.error(pos, format!("unknown section '{a}'"));
                    return None;
                }
            }
            Target::Label => {
                if let Some(l) = labels.iter().find(|l| l.name == a) {
                    if sections[l.section].isa.width != 8 {
                        self.error(pos, format!("{} needs a usse label, not '{a}'", k.name));
                        return None;
                    }
                    return u32::try_from(l.item).ok();
                }
                if unknown {
                    self.error(pos, format!("unknown label '{a}'"));
                    return None;
                }
            }
            _ => {}
        }
        self.word32(a, pos)
    }

    /// A patch argument: an expression that fits 32 bits (negative values
    /// in two's complement).
    fn word32(&mut self, a: &str, pos: &Pos) -> Option<u32> {
        match expr::eval(a, |n| self.equs.get(n).copied()) {
            Ok(v) if (-(1i128 << 31)..1i128 << 32).contains(&v) => Some(v as u32),
            _ => {
                self.error(pos, format!("bad patch argument '{a}'"));
                None
            }
        }
    }
}

pub fn main(isa: &Isa) -> ExitCode {
    let argv: Vec<String> = std::env::args().collect();
    let (mut out, mut input, mut list, mut core) = (None, None, false, Core::Sgx545);
    // Rust only, not in the C tool's usage: --explain
    let mut hints = false;
    let mut i = 1;
    while i < argv.len() {
        let a = argv[i].as_str();
        if a == "-o" && i + 1 < argv.len() {
            i += 1;
            out = Some(argv[i].clone());
        } else if a == "-l" {
            list = true;
        } else if a == "--explain" {
            hints = true;
        } else if a == "--core" && i + 1 < argv.len() {
            i += 1;
            core = if atoi(&argv[i]) == 540 { Core::Sgx540 } else { Core::Sgx545 };
        } else if input.is_none() && (!a.starts_with('-') || a == "-") {
            input = Some(a.to_string());
        } else {
            return usage(isa.name);
        }
        i += 1;
    }
    let Some(input) = input else { return usage(isa.name) };

    let reader: Box<dyn BufRead> = if input == "-" {
        Box::new(std::io::stdin().lock())
    } else {
        match std::fs::File::open(&input) {
            Ok(f) => Box::new(std::io::BufReader::new(f)),
            Err(e) => {
                eprintln!("{input}: {}", strerror(&e));
                return ExitCode::from(2);
            }
        }
    };
    let file = if input == "-" { "<stdin>".to_string() } else { input.clone() };
    let mut text = Vec::new();
    let mut reader = reader;
    let _ = reader.read_to_end(&mut text);
    let a = assemble_hinted(isa, &text, &file, core, hints);
    let (words, ndata) = (a.words, a.ndata);
    let _ = std::io::stderr().write_all(&a.messages);
    if a.errors > 0 {
        return ExitCode::from(1);
    }
    if let Some(out) = out {
        let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()[..isa.width].to_vec()).collect();
        if let Err(e) = std::fs::write(&out, bytes) {
            eprintln!("{out}: {}", strerror(&e));
            return ExitCode::from(2);
        }
    }
    if list {
        let stdout = std::io::stdout();
        let mut o = std::io::BufWriter::new(stdout.lock());
        for (i, &w) in words.iter().enumerate() {
            let _ = writeln!(o, "{}", (isa.list)(i, w, i < ndata, core));
        }
    }
    ExitCode::SUCCESS
}

/// map.json for an sgx-asm program: its sections (with their image files),
/// labels and patch records.
pub fn map_json(p: &Program) -> String {
    let mut o = String::from("{\n  \"sections\": [");
    for (i, s) in p.sections.iter().enumerate() {
        let _ = write!(
            o,
            "{}\n    {{\"name\": \"{}\", \"isa\": \"{}\", \"id\": {}, \"bin\": \"{}.bin\", \"size\": {}, \"data\": {}}}",
            if i > 0 { "," } else { "" },
            s.name,
            s.isa_name(),
            s.id,
            s.name,
            s.words.len() * s.isa.width,
            s.ndata
        );
    }
    o.push_str("\n  ],\n  \"symbols\": [");
    for (i, y) in p.symbols.iter().enumerate() {
        let index = y.index.map_or("null".to_string(), |x| x.to_string());
        let _ = write!(
            o,
            "{}\n    {{\"name\": \"{}\", \"section\": \"{}\", \"index\": {index}, \"offset\": {}}}",
            if i > 0 { "," } else { "" },
            y.name,
            p.sections[y.section].name,
            y.offset
        );
    }
    o.push_str("\n  ],\n  \"patches\": [");
    for (i, r) in p.patches.iter().enumerate() {
        let s = &p.sections[r.section];
        let _ = write!(
            o,
            "{}\n    {{\"section\": \"{}\", \"id\": {}, \"offset\": {}, \"encoding\": \"{}\", \"kind\": \"{}\", \
             \"target\": {}, \"addend\": {}}}",
            if i > 0 { "," } else { "" },
            s.name,
            s.id,
            r.offset,
            ENCODINGS[r.encoding],
            KINDS[r.kind].name,
            r.target,
            r.addend
        );
    }
    o.push_str("\n  ]\n}\n");
    o
}

/// sgx-asm: `-d DIR` writes each section's image as DIR/<name>.bin and the
/// map as DIR/map.json; `-l` lists every section.
pub fn main_program() -> ExitCode {
    const USAGE: &str = "usage: sgx-asm [-l] [-d outdir] [--core 540|545] [--explain] <source.s | ->";
    let argv: Vec<String> = std::env::args().collect();
    let (mut dir, mut input, mut list, mut core, mut hints) = (None, None, false, Core::Sgx545, false);
    let mut i = 1;
    while i < argv.len() {
        let a = argv[i].as_str();
        if a == "-d" && i + 1 < argv.len() {
            i += 1;
            dir = Some(PathBuf::from(&argv[i]));
        } else if a == "-l" {
            list = true;
        } else if a == "--explain" {
            hints = true;
        } else if a == "--core" && i + 1 < argv.len() {
            i += 1;
            core = if atoi(&argv[i]) == 540 { Core::Sgx540 } else { Core::Sgx545 };
        } else if input.is_none() && (!a.starts_with('-') || a == "-") {
            input = Some(a.to_string());
        } else {
            eprintln!("{USAGE}");
            return ExitCode::from(2);
        }
        i += 1;
    }
    let Some(input) = input else {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    };
    let (text, file) = if input == "-" {
        let mut t = Vec::new();
        let _ = std::io::stdin().read_to_end(&mut t);
        (t, "<stdin>".to_string())
    } else {
        match std::fs::read(&input) {
            Ok(t) => (t, input.clone()),
            Err(e) => {
                eprintln!("{input}: {}", strerror(&e));
                return ExitCode::from(2);
            }
        }
    };
    let p = assemble_program(&text, &file, core, hints);
    let _ = std::io::stderr().write_all(&p.messages);
    if p.errors > 0 {
        return ExitCode::from(1);
    }
    if let Some(dir) = dir {
        let mut files: Vec<(PathBuf, Vec<u8>)> = p
            .sections
            .iter()
            .map(|s| {
                let bytes = s.words.iter().flat_map(|w| w.to_le_bytes()[..s.isa.width].to_vec()).collect();
                (dir.join(format!("{}.bin", s.name)), bytes)
            })
            .collect();
        files.push((dir.join("map.json"), map_json(&p).into_bytes()));
        if let Err(e) = std::fs::create_dir_all(&dir) {
            eprintln!("{}: {}", dir.display(), strerror(&e));
            return ExitCode::from(2);
        }
        for (path, bytes) in files {
            if let Err(e) = std::fs::write(&path, bytes) {
                eprintln!("{}: {}", path.display(), strerror(&e));
                return ExitCode::from(2);
            }
        }
    }
    if list {
        let stdout = std::io::stdout();
        let mut o = std::io::BufWriter::new(stdout.lock());
        for s in &p.sections {
            let _ = writeln!(o, "section {} ({}, id {})", s.name, s.isa_name(), s.id);
            for (i, &w) in s.words.iter().enumerate() {
                let _ = writeln!(o, "{}", (s.isa.list)(i, w, i < s.ndata, core));
            }
        }
    }
    ExitCode::SUCCESS
}
