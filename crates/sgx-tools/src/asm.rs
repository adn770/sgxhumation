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
    items: Vec<Item>,
    labels: Vec<(String, usize)>,
    equs: HashMap<String, i128>,
    macros: HashMap<String, Rc<Macro>>,
    recording: Option<Recording>,
    /// macro calls so far, for \@
    calls: usize,
    errors: usize,
    isa: &'a Isa,
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
                self.items.push(Item { text: None, raw: hex, pos: pos.clone() });
            } else if !rest.is_empty() {
                self.items.push(Item { text: Some(rest.to_string()), raw: 0, pos: pos.clone() });
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
            if self.labels.iter().any(|(l, _)| l == name) {
                self.error(pos, format!("label '{name}' defined twice"));
            }
            self.labels.push((name.to_string(), self.items.len()));
            s = trim(&s[n + 1..]);
        }
        if s.is_empty() {
            return;
        }
        let raw = self.isa.raw;
        if s.starts_with(raw) && s.as_bytes().get(raw.len()).is_some_and(|&c| is_space(c)) {
            self.raw_words(trim(&s[raw.len()..]), pos);
            return;
        }
        let (word, rest) = split_word(s);
        match word {
            ".include" => self.include(rest, pos, dir, includes),
            ".equ" => self.equ(rest, pos),
            ".macro" => self.start_macro(rest, pos),
            ".endm" => self.error(pos, ".endm without .macro".to_string()),
            w if self.macros.contains_key(w) => self.call(w, rest, pos, dir, includes, calls),
            _ => {
                let text = self.operands(s, pos);
                self.items.push(Item { text: Some(text), raw: 0, pos: pos.clone() });
            }
        }
    }

    /// .long / .word: values, numbers or expressions, separated by commas.
    fn raw_words(&mut self, list: &str, pos: &Pos) {
        let width = self.isa.width;
        for v in split_args(list) {
            let value = match parse_raw(v, width) {
                Some((x, n)) if n == v.len() => Some(x),
                _ => expr::eval(v, |n| self.equs.get(n).copied()).ok().and_then(|x| {
                    let bits = 8 * width as u32;
                    (x >= -(1i128 << (bits - 1)) && x < 1i128 << bits)
                        .then_some((x as u64) & (u64::MAX >> (64 - bits)))
                }),
            };
            match value {
                Some(x) => self.items.push(Item { text: None, raw: x, pos: pos.clone() }),
                None => {
                    self.error(pos, format!("bad {} value", self.isa.raw));
                    return;
                }
            }
        }
    }

    /// An instruction's text with each .equ name, and each parenthesised
    /// expression, replaced by its value. Parentheses that are not an
    /// expression (a predicate) stay.
    fn operands(&mut self, s: &str, pos: &Pos) -> String {
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
    let mut st = State {
        items: Vec::new(),
        labels: Vec::new(),
        equs: HashMap::new(),
        macros: HashMap::new(),
        recording: None,
        calls: 0,
        errors: 0,
        isa,
        err: Vec::new(),
    };
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

    let ndata = if isa.data_segment { st.items.iter().take_while(|it| it.text.is_none()).count() } else { 0 };
    let labels = std::mem::take(&mut st.labels);
    let resolve = |name: &str| -> Option<u64> {
        let &(_, idx) = labels.iter().find(|(l, _)| l == name)?;
        (idx >= ndata).then(|| (idx - ndata) as u64)
    };
    let cx = usse_isa::Ctx { core, resolve: Some(&resolve) };
    let mut words = vec![0u64; st.items.len()];
    let items = std::mem::take(&mut st.items);
    for (w, it) in words.iter_mut().zip(&items) {
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
    Assembled { words, ndata, messages: st.err, errors: st.errors }
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
