// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! Integer expressions for the assemblers' .equ, raw words and operands.
//!
//! C syntax and precedence, on 128-bit integers: unary `- ~ +`, then
//! `* / %`, `+ -`, `<< >>`, `&`, `^`, `|`, and parentheses. Numbers are
//! decimal, 0x hex or 0b binary; names are looked up in the caller's symbols.

/// Why a text is not a value.
#[derive(Debug, PartialEq)]
pub enum Error {
    /// not an expression at all
    Syntax,
    /// a name the symbols do not know
    Undefined(String),
    DivZero,
}

struct Parser<'a, F> {
    s: &'a [u8],
    p: usize,
    lookup: F,
}

fn is_ident_start(c: u8) -> bool {
    c.is_ascii_alphabetic() || c == b'_'
}

fn is_ident(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

/// A symbol name: a letter or '_', then letters, digits and '_'.
pub fn is_name(s: &str) -> bool {
    let b = s.as_bytes();
    b.first().is_some_and(|&c| is_ident_start(c)) && b.iter().all(|&c| is_ident(c))
}

/// Binary operators by precedence level, lowest first.
const LEVELS: &[&[&str]] = &[&["|"], &["^"], &["&"], &["<<", ">>"], &["+", "-"], &["*", "/", "%"]];

impl<F: Fn(&str) -> Option<i128>> Parser<'_, F> {
    fn skip(&mut self) {
        while self.s.get(self.p).is_some_and(|c| c.is_ascii_whitespace()) {
            self.p += 1;
        }
    }

    fn eat(&mut self, op: &str) -> bool {
        self.skip();
        let o = op.as_bytes();
        if !self.s[self.p..].starts_with(o) {
            return false;
        }
        // '<' alone, or '|' of '||', is not this operator
        let next = self.s.get(self.p + o.len());
        if o.len() == 1 && next == Some(&o[0]) && matches!(o[0], b'<' | b'>' | b'|' | b'&') {
            return false;
        }
        self.p += o.len();
        true
    }

    fn binary(&mut self, level: usize) -> Result<i128, Error> {
        if level == LEVELS.len() {
            return self.unary();
        }
        let mut x = self.binary(level + 1)?;
        'more: loop {
            for &op in LEVELS[level] {
                if self.eat(op) {
                    let y = self.binary(level + 1)?;
                    x = apply(op, x, y)?;
                    continue 'more;
                }
            }
            return Ok(x);
        }
    }

    fn unary(&mut self) -> Result<i128, Error> {
        if self.eat("-") {
            return self.unary().map(i128::wrapping_neg);
        }
        if self.eat("~") {
            return self.unary().map(|x| !x);
        }
        if self.eat("+") {
            return self.unary();
        }
        if self.eat("(") {
            let x = self.binary(0)?;
            return if self.eat(")") { Ok(x) } else { Err(Error::Syntax) };
        }
        self.skip();
        let b = &self.s[self.p..];
        let n = b.iter().take_while(|&&c| is_ident(c)).count();
        if n == 0 {
            return Err(Error::Syntax);
        }
        let word = std::str::from_utf8(&b[..n]).map_err(|_| Error::Syntax)?;
        self.p += n;
        if is_ident_start(b[0]) {
            return (self.lookup)(word).ok_or_else(|| Error::Undefined(word.to_string()));
        }
        number(word).ok_or(Error::Syntax)
    }
}

fn apply(op: &str, x: i128, y: i128) -> Result<i128, Error> {
    let shift = |y: i128| u32::try_from(y).ok().filter(|&s| s < 128).ok_or(Error::Syntax);
    Ok(match op {
        "|" => x | y,
        "^" => x ^ y,
        "&" => x & y,
        "<<" => x.wrapping_shl(shift(y)?),
        ">>" => x >> shift(y)?,
        "+" => x.wrapping_add(y),
        "-" => x.wrapping_sub(y),
        "*" => x.wrapping_mul(y),
        "/" | "%" if y == 0 => return Err(Error::DivZero),
        "/" => x.wrapping_div(y),
        _ => x.wrapping_rem(y),
    })
}

fn number(w: &str) -> Option<i128> {
    let (digits, radix) = match w.get(..2) {
        Some("0x" | "0X") => (&w[2..], 16),
        Some("0b" | "0B") => (&w[2..], 2),
        _ => (w, 10),
    };
    if digits.is_empty() || !digits.bytes().all(|c| (c as char).is_digit(radix)) {
        return None;
    }
    i128::from_str_radix(digits, radix).ok()
}

/// The value of `text`, all of it an expression.
pub fn eval(text: &str, lookup: impl Fn(&str) -> Option<i128>) -> Result<i128, Error> {
    let mut p = Parser { s: text.as_bytes(), p: 0, lookup };
    let x = p.binary(0)?;
    p.skip();
    if p.p != p.s.len() {
        return Err(Error::Syntax);
    }
    Ok(x)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ev(s: &str) -> Result<i128, Error> {
        eval(s, |n| match n {
            "A" => Some(10),
            "b_2" => Some(3),
            _ => None,
        })
    }

    #[test]
    fn numbers_and_names() {
        assert_eq!(ev("42"), Ok(42));
        assert_eq!(ev(" 0x1F "), Ok(31));
        assert_eq!(ev("0b101"), Ok(5));
        assert_eq!(ev("A + b_2"), Ok(13));
        assert_eq!(ev("C"), Err(Error::Undefined("C".into())));
    }

    #[test]
    fn c_precedence() {
        assert_eq!(ev("1 + 2 * 3"), Ok(7));
        assert_eq!(ev("(1 + 2) * 3"), Ok(9));
        assert_eq!(ev("1 << 2 + 1"), Ok(8));
        assert_eq!(ev("40 | 1 << 8"), Ok(296));
        assert_eq!(ev("6 & 3 ^ 1 | 8"), Ok(11));
        assert_eq!(ev("~0 & 0xff"), Ok(255));
        assert_eq!(ev("-A - -2"), Ok(-8));
        assert_eq!(ev("17 % 5 / 2"), Ok(1));
        assert_eq!(ev("0x100 >> 4"), Ok(16));
    }

    #[test]
    fn not_expressions() {
        for s in ["", "1 +", "(1", "1)", "1 2", "0x", "9x", "1 < 2", "1 || 2", "r0.l", "1 << -1"] {
            assert!(ev(s).is_err(), "{s:?}");
        }
        assert_eq!(ev("1 / (A - 10)"), Err(Error::DivZero));
        assert_eq!(ev("1 % 0"), Err(Error::DivZero));
    }

    #[test]
    fn names() {
        assert!(is_name("_x9") && is_name("BASE"));
        assert!(!is_name("9x") && !is_name("a.b") && !is_name(""));
    }
}
