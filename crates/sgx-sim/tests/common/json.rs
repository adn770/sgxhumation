// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! A minimal JSON reader for the vector files (the crates stay dependency-free).

#[derive(Debug, Clone)]
pub enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn get(&self, k: &str) -> &Json {
        match self {
            Json::Obj(m) => m.iter().find(|(n, _)| n == k).map_or(&Json::Null, |(_, v)| v),
            _ => &Json::Null,
        }
    }
    pub fn str(&self) -> Option<&str> {
        if let Json::Str(s) = self { Some(s) } else { None }
    }
    pub fn arr(&self) -> &[Json] {
        if let Json::Arr(a) = self { a } else { &[] }
    }
    pub fn obj(&self) -> &[(String, Json)] {
        if let Json::Obj(m) = self { m } else { &[] }
    }
    pub fn hex(&self) -> Option<u64> {
        self.str().and_then(|s| u64::from_str_radix(s.trim_start_matches("0x"), 16).ok())
    }
}

pub fn parse(s: &str) -> Json {
    let b = s.as_bytes();
    let mut i = 0;
    value(b, &mut i)
}

fn ws(b: &[u8], i: &mut usize) {
    while *i < b.len() && b[*i].is_ascii_whitespace() {
        *i += 1;
    }
}

fn value(b: &[u8], i: &mut usize) -> Json {
    ws(b, i);
    match b[*i] {
        b'{' => {
            *i += 1;
            let mut m = Vec::new();
            loop {
                ws(b, i);
                if b[*i] == b'}' {
                    *i += 1;
                    return Json::Obj(m);
                }
                let Json::Str(k) = value(b, i) else { panic!("key") };
                ws(b, i);
                *i += 1; // ':'
                m.push((k, value(b, i)));
                ws(b, i);
                if b[*i] == b',' {
                    *i += 1;
                }
            }
        }
        b'[' => {
            *i += 1;
            let mut a = Vec::new();
            loop {
                ws(b, i);
                if b[*i] == b']' {
                    *i += 1;
                    return Json::Arr(a);
                }
                a.push(value(b, i));
                ws(b, i);
                if b[*i] == b',' {
                    *i += 1;
                }
            }
        }
        b'"' => {
            *i += 1;
            let mut s = String::new();
            while b[*i] != b'"' {
                if b[*i] == b'\\' {
                    *i += 1;
                    match b[*i] {
                        b'n' => s.push('\n'),
                        b't' => s.push('\t'),
                        b'u' => {
                            let h = std::str::from_utf8(&b[*i + 1..*i + 5]).unwrap();
                            s.push(char::from_u32(u32::from_str_radix(h, 16).unwrap()).unwrap_or('?'));
                            *i += 4;
                        }
                        c => s.push(c as char),
                    }
                    *i += 1;
                } else {
                    let st = *i;
                    while b[*i] != b'"' && b[*i] != b'\\' {
                        *i += 1;
                    }
                    s.push_str(std::str::from_utf8(&b[st..*i]).unwrap());
                }
            }
            *i += 1;
            Json::Str(s)
        }
        b't' => {
            *i += 4;
            Json::Bool(true)
        }
        b'f' => {
            *i += 5;
            Json::Bool(false)
        }
        b'n' => {
            *i += 4;
            Json::Null
        }
        _ => {
            let st = *i;
            while *i < b.len() && matches!(b[*i], b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9') {
                *i += 1;
            }
            Json::Num(std::str::from_utf8(&b[st..*i]).unwrap().parse().unwrap())
        }
    }
}
