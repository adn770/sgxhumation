// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The encoder: a form table read backwards (the Rust side of the encoding half
//! of tools/usse-dis/usse_form.c, which it follows step for step so that both
//! pick the same word for a text).
//!
//! The forms are tried in table order. A pattern matches the text literally
//! (blanks flexible, letters in either case, the pattern's comments optional);
//! each field is parsed by its kind or, for a kind without a parser, found by
//! searching its values for one that prints the text, trying the form's
//! preferred value (from `dflt`) first. The search backtracks. A word is
//! accepted only if the table decodes it with the form's text.

use crate::form::{Ctx, Field, Form, Kind, Table};
use std::cell::RefCell;
use std::rc::Rc;

fn skip_ws(s: &str) -> &str {
    s.trim_start_matches(|c: char| c.is_ascii_whitespace() || c == '\x0b')
}

/// Match expected text `x` (a pattern literal or a printed field) at the start
/// of `s`: a space in x matches any run of blanks, blanks may precede a comma,
/// a comment in x counts as a blank, and letters match in either case.
/// Return the rest of `s`.
fn lit<'s>(x: &str, mut s: &'s str) -> Option<&'s str> {
    let xb = x.as_bytes();
    let mut i = 0;
    while i < xb.len() {
        if xb[i] == b'/' && xb.get(i + 1) == Some(&b'*') {
            i = x[i + 2..].find("*/").map_or(xb.len(), |c| i + 2 + c + 2);
            s = skip_ws(s);
        } else if xb[i] == b' ' {
            s = skip_ws(s);
            i += 1;
        } else if xb[i] == b',' {
            s = skip_ws(s).strip_prefix(',')?;
            i += 1;
        } else {
            let c = *s.as_bytes().first()?;
            if !c.eq_ignore_ascii_case(&xb[i]) {
                return None;
            }
            s = &s[1..];
            i += 1;
        }
    }
    Some(s)
}

/* ---- searched fields: printed once, indexed by first character ------------
 * A field whose kind has no parser is inverted by searching its values for one
 * that prints the input text. Each (kind, width) is printed once and its values
 * grouped by the lowercased first character of their text, in ascending order;
 * empty texts, and texts that begin with a blank, a comma or a comment, which
 * lit() matches loosely, go in a bucket tried at every character. */
const KCACHE_MAX_W: u32 = 16;
const ANY: usize = 256;

struct KCache {
    text: Vec<Option<String>>,
    order: Vec<u32>,
    start: Vec<u32>, // bucket b is order[start[b] .. start[b + 1]]
}

fn bucket_of(t: &str) -> usize {
    match t.as_bytes().first() {
        None | Some(b' ' | b',' | b'/') => ANY,
        Some(c) => c.to_ascii_lowercase() as usize,
    }
}

/// The caches built so far, by kind and field width.
type KCaches = Vec<(&'static dyn Kind, u32, Rc<KCache>)>;

thread_local! {
    static KCACHES: RefCell<KCaches> = const { RefCell::new(Vec::new()) };
}

fn kcache(k: &'static dyn Kind, w: u32, cx: &Ctx) -> Rc<KCache> {
    KCACHES.with(|c| {
        if let Some((_, _, kc)) = c.borrow().iter().find(|(k2, w2, _)| std::ptr::eq(*k2, k) && *w2 == w) {
            return kc.clone();
        }
        let n = 1u32 << w;
        let mut text = Vec::with_capacity(n as usize);
        let mut count = vec![0u32; ANY + 1];
        for v in 0..n {
            let mut t = String::new();
            let t = k.print(v, cx, &mut t).then_some(t);
            if let Some(t) = &t {
                count[bucket_of(t)] += 1;
            }
            text.push(t);
        }
        let mut start = vec![0u32; ANY + 2];
        for b in 0..=ANY {
            start[b + 1] = start[b] + count[b];
        }
        let mut fill = start.clone();
        let mut order = vec![0u32; start[ANY + 1] as usize];
        for (v, t) in text.iter().enumerate() {
            if let Some(t) = t {
                let b = bucket_of(t);
                order[fill[b] as usize] = v as u32;
                fill[b] += 1;
            }
        }
        let kc = Rc::new(KCache { text, order, start });
        c.borrow_mut().push((k, w, kc.clone()));
        kc
    })
}

struct Match<'c, 'r> {
    table: &'static Table,
    fm: &'static Form,
    v: [u32; 10],
    out: u64,
    cx: &'c Ctx<'r>,
}

fn print(k: &dyn Kind, v: u32, cx: &Ctx) -> Option<String> {
    let mut s = String::new();
    k.print(v, cx, &mut s).then_some(s)
}

impl Match<'_, '_> {
    fn finish(&mut self) -> bool {
        let fm = self.fm;
        let mut e = fm.val | (fm.dflt & !fm.bits());
        for (f, &v) in fm.f.iter().zip(&self.v) {
            e = f.put(e, v);
        }
        // Fields that share bits can overwrite each other's values; a field may
        // read back another value only if it prints the same text.
        for (f, &v) in fm.f.iter().zip(&self.v) {
            let got = f.get(e);
            if got != v {
                match (print(f.k, v, self.cx), print(f.k, got, self.cx)) {
                    (Some(a), Some(b)) if a == b => {}
                    _ => return false,
                }
            }
        }
        // The word must decode with this form's text (an earlier form may claim
        // it, or a field may not print); otherwise keep searching.
        let Some(want) = fm.render(e, self.cx) else { return false };
        match self.table.decode(e, self.cx) {
            Some((_, got)) if got == want => {
                self.out = e;
                true
            }
            _ => false,
        }
    }

    fn matches(&mut self, mut p: &str, mut s: &str) -> bool {
        loop {
            let pb = p.as_bytes();
            if pb.is_empty() {
                return skip_ws(s).is_empty() && self.finish();
            }
            if pb[0] == b'{' && pb.len() > 2 && pb[1].is_ascii_digit() && pb[2] == b'}' {
                let i = (pb[1] - b'0') as usize;
                return self.field(i, &p[3..], s);
            }
            // one literal character (or a comment)
            let len = match p.strip_prefix("/*") {
                Some(rest) => rest.find("*/").map_or(p.len(), |c| c + 4),
                None => 1,
            };
            match lit(&p[..len], s) {
                Some(rest) => s = rest,
                None => return false,
            }
            p = &p[len..];
        }
    }

    fn field(&mut self, i: usize, p: &str, s: &str) -> bool {
        let f: &Field = &self.fm.f[i];
        let w = f.width();
        let lim = if w >= 32 { u32::MAX } else { (1u32 << w) - 1 };
        if f.k.parses() {
            // a kind with a parser: one reading of the text
            return match f.k.parse(s, self.cx) {
                Some((v, n)) if v <= lim => {
                    self.v[i] = v;
                    self.matches(p, &s[n..])
                }
                _ => false,
            };
        }
        let pref = f.get(self.fm.dflt);
        if w <= KCACHE_MAX_W {
            let kc = kcache(f.k, w, self.cx);
            if pref <= lim
                && let Some(t) = &kc.text[pref as usize]
                && let Some(q) = lit(t, s)
            {
                self.v[i] = pref;
                if self.matches(p, q) {
                    return true;
                }
            }
            let b0 = s.as_bytes().first().map_or(0, |c| c.to_ascii_lowercase() as usize);
            for b in [b0, ANY] {
                for o in kc.start[b]..kc.start[b + 1] {
                    let v = kc.order[o as usize];
                    if v == pref {
                        continue;
                    }
                    if let Some(q) = lit(kc.text[v as usize].as_deref().unwrap(), s) {
                        self.v[i] = v;
                        if self.matches(p, q) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }
        for n in 0..=(lim as u64 + 1) {
            let v = if n == 0 { pref } else { (n - 1) as u32 };
            if n != 0 && v == pref {
                continue;
            }
            if let Some(t) = print(f.k, v, self.cx)
                && let Some(q) = lit(&t, s)
            {
                self.v[i] = v;
                if self.matches(p, q) {
                    return true;
                }
            }
        }
        false
    }
}

/// Encode `text` with a table: the word of the first form, in table order,
/// whose pattern matches and whose word the table decodes with that form's
/// text. Bits outside the fields come from the form's `dflt`.
pub fn encode_in(table: &'static Table, text: &str, cx: &Ctx) -> Option<u64> {
    // drop comments from the input; the patterns' own comments match blanks
    let mut buf = String::with_capacity(text.len());
    let mut t = text;
    while let Some(c) = t.find("/*") {
        buf.push_str(&t[..c]);
        buf.push(' ');
        t = t[c + 2..].find("*/").map_or("", |e| &t[c + 2 + e + 2..]);
    }
    buf.push_str(t);
    let s = skip_ws(&buf);
    for fm in table.forms {
        let Some(pat) = fm.pat else { continue };
        let mut m = Match { table, fm, v: [0; 10], out: 0, cx };
        if m.matches(pat, s) {
            return Some(m.out);
        }
    }
    None
}
