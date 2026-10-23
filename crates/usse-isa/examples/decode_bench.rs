// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! Decoder throughput: `cargo run --release -p usse-isa --example decode_bench`.
//! Words: uniform random, and words matching each form (so every class is hit).

use std::hint::black_box;
use std::time::Instant;
use usse_isa::{Core, Ctx, decode, decode_fields, form, forms};

fn words() -> Vec<u64> {
    let mut x = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x
    };
    let mut w: Vec<u64> = (0..1_000_000).map(|_| next()).collect();
    let fs: Vec<_> = forms().collect();
    for i in 0..1_000_000 {
        let f = fs[i % fs.len()];
        w.push(f.val | (next() & !f.mask));
    }
    w
}

fn bench(name: &str, ws: &[u64], f: impl Fn(u64) -> usize) {
    let t = Instant::now();
    let mut acc = 0usize;
    for &w in ws {
        acc = acc.wrapping_add(f(w));
    }
    let s = t.elapsed().as_secs_f64();
    black_box(acc);
    println!(
        "{name:<28} {:>8.2} Mwords/s  ({:.0} ns/word)",
        ws.len() as f64 / s / 1e6,
        s * 1e9 / ws.len() as f64
    );
}

fn main() {
    let ws = words();
    let cx = Ctx::default();
    bench("linear walk (text)", &ws, |w| form::decode_in(forms(), w, &cx).map_or(0, |d| d.1.len()));
    bench("decode (text)", &ws, |w| decode(w, Core::Sgx545).text.len());
    bench("decode_fields (no text)", &ws, |w| decode_fields(w, Core::Sgx545).map_or(0, |f| f.form));
}
