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

use std::io::{BufRead, Write};
use std::process::ExitCode;
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
    /// one listing line, as the matching disassembler prints it
    pub list: fn(idx: usize, w: u64, is_data: bool, core: Core) -> String,
}

struct Item {
    text: Option<String>,
    raw: u64,
    line: usize,
}

struct State<'a> {
    items: Vec<Item>,
    labels: Vec<(String, usize)>,
    errors: usize,
    file: String,
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

impl State<'_> {
    fn error(&mut self, line: usize, msg: String) {
        let _ = writeln!(self.err, "{}:{line}: {msg}", self.file);
        self.errors += 1;
    }

    fn parse_line(&mut self, raw_line: &str, line: usize) {
        let mut s = strip_c_comments(raw_line);
        if let Some(c) = s.find(';') {
            s.truncate(c);
        }
        let mut s = trim(&s);
        if let Some((hex, rest)) = listing(s) {
            if rest == "data" {
                self.items.push(Item { text: None, raw: hex, line });
            } else if !rest.is_empty() {
                self.items.push(Item { text: Some(rest.to_string()), raw: 0, line });
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
                self.error(line, format!("label '{name}' defined twice"));
            }
            self.labels.push((name.to_string(), self.items.len()));
            s = trim(&s[n + 1..]);
        }
        if s.is_empty() {
            return;
        }
        let raw = self.isa.raw;
        if s.starts_with(raw) && s.as_bytes().get(raw.len()).is_some_and(|&c| is_space(c)) {
            let mut p = &s[raw.len()..];
            loop {
                p = trim(p);
                let Some((v, n)) = parse_raw(p, self.isa.width) else {
                    self.error(line, format!("bad {raw} value"));
                    return;
                };
                self.items.push(Item { text: None, raw: v, line });
                p = trim(&p[n..]);
                if p.is_empty() {
                    break;
                }
                match p.strip_prefix(',') {
                    Some(rest) => p = rest,
                    None => {
                        self.error(line, format!("bad {raw} value"));
                        return;
                    }
                }
            }
            return;
        }
        self.items.push(Item { text: Some(s.to_string()), raw: 0, line });
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

pub fn main(isa: &Isa) -> ExitCode {
    let argv: Vec<String> = std::env::args().collect();
    let (mut out, mut input, mut list, mut core) = (None, None, false, Core::Sgx545);
    let mut i = 1;
    while i < argv.len() {
        let a = argv[i].as_str();
        if a == "-o" && i + 1 < argv.len() {
            i += 1;
            out = Some(argv[i].clone());
        } else if a == "-l" {
            list = true;
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
    let mut st = State { items: Vec::new(), labels: Vec::new(), errors: 0, file, isa, err: Vec::new() };
    for (n, line) in reader.split(b'\n').enumerate() {
        let Ok(line) = line else { break };
        st.parse_line(&String::from_utf8_lossy(&line), n + 1);
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
                None => st.error(it.line, format!("cannot assemble '{t}'")),
            },
        }
    }
    let _ = std::io::stderr().write_all(&st.err);
    if st.errors > 0 {
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
