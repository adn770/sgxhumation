// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The texture unit, as far as it is measured (doc/isa/content/usse/probe/
//! sampling.md and sampler-state.md): an smp's four state words T0-T3 (the
//! PDS doutt words, design/sgx545_transfer.md §4.4.3) and its coordinates
//! give the registers the sample writes. Texels are read through `load`.
//! Mipmaps, the LOD values, border modes, tiled layouts, PCF and the formats
//! the probes only recorded are not established and stop with Err(why).

/// One sample: the state words, the normalized coordinates, whether the
/// texture is read as 1D (row 0), and .pcf's reference.
#[derive(Clone, Copy, Debug)]
pub struct Sample {
    pub state: [u32; 4],
    pub u: f32,
    pub v: f32,
    pub one_d: bool,
    pub pcf: Option<f32>,
}

/// A load of the dword at an address (4-aligned): the texels' memory.
pub type Load<'a> = dyn FnMut(u32) -> Result<u32, crate::Error> + 'a;

/// What a sample does, or why it is not known.
#[derive(Debug, PartialEq)]
pub enum Fail {
    Open(String),
    Load(crate::Error),
}

fn open<T>(s: impl Into<String>) -> Result<T, Fail> {
    Err(Fail::Open(s.into()))
}

/// The registers the sample writes, in order.
pub fn sample(s: &Sample, load: &mut Load) -> Result<Vec<u32>, Fail> {
    let [t0, t1, t2, t3] = s.state;
    let tex = Texture::new(t0, t1, t2)?;
    // T0 bits 13:12: 1 and 2 bilinear, 0 and 3 point (smp-filter-modes)
    let bilinear = matches!(t0 >> 12 & 3, 1 | 2);
    let (u, v) = (s.u as f64, if s.one_d { 0.0 } else { s.v as f64 });
    let rows = |y: f64| if s.one_d { 0 } else { (y * tex.h as f64).floor() as i64 };
    // the four texels of a bilinear sample and their weights in 1/4096,
    // truncated to 1/64 per axis (smp-bilinear)
    let quad = |load: &mut Load| -> Result<([u32; 4], [u32; 4]), Fail> {
        let (x, y) = (u * tex.w as f64 - 0.5, v * tex.h as f64 - 0.5);
        let (x0, y0) = (x.floor(), y.floor());
        let fx = ((x - x0) * 64.0).floor() as u32;
        let fy = if s.one_d { 0 } else { ((y - y0) * 64.0).floor() as u32 };
        let (x0, y0) = (x0 as i64, if s.one_d { 0 } else { y0 as i64 });
        let mut t = [0u32; 4];
        for (k, (dx, dy)) in [(0, 0), (1, 0), (0, 1), (1, 1)].into_iter().enumerate() {
            t[k] = tex.fetch(x0 + dx, y0 + dy, load)?;
        }
        Ok((t, [(64 - fx) * (64 - fy), fx * (64 - fy), (64 - fx) * fy, fx * fy]))
    };
    if let Some(r) = s.pcf
        && !tex.colour()
    {
        check_t3(t3)?;
        if t3 >> 16 & 3 != 0 {
            return open("a conversion with .pcf");
        }
        let (t, w) = if bilinear {
            quad(load)?
        } else {
            ([tex.fetch((u * tex.w as f64).floor() as i64, rows(v), load)?; 4], [1024; 4])
        };
        return tex.pcf(t, w, r, t3 >> 29).map(|v| vec![v]);
    }
    let colour = if bilinear {
        if !tex.colour() {
            return open(format!("bilinear filtering of format {}", tex.format));
        }
        let (raw, w) = quad(load)?;
        let mut t = [0u32; 4];
        for k in 0..4 {
            t[k] = tex.colour_texel(raw[k])?;
        }
        let mut out = 0;
        for c in 0..4 {
            let sum: u32 = (0..4).map(|k| (t[k] >> (8 * c) & 0xff) * w[k]).sum();
            // each 8-bit channel rounds to nearest, halves up
            out |= (sum + 2048) >> 12 << (8 * c);
        }
        out
    } else {
        let raw = tex.fetch((u * tex.w as f64).floor() as i64, rows(v), load)?;
        if tex.colour() { tex.colour_texel(raw)? } else { tex.plain_texel(raw)? }
    };
    convert(colour, t3, tex.format)
}

/// T3 bits 17:16 convert the texel, bit 15 normalizing (smp-convert): none,
/// c10, f16 pairs or f32 channels, from the B8G8R8A8 channels. Without alpha
/// (565) c10 and f32 leave its register out and f16 gives 1.0. T3's other
/// fields: the compare mode 31:29 (for .pcf) and the data size 26:23, which
/// changes nothing seen; any other bit is not measured.
fn convert(texel: u32, t3: u32, format: u32) -> Result<Vec<u32>, Fail> {
    check_t3(t3)?;
    let conv = t3 >> 16 & 3;
    if conv == 0 {
        return Ok(vec![texel]);
    }
    let norm = t3 >> 15 & 1 != 0;
    // the other formats' channels, in order (smp-convert): u8 (0), two u8
    // (7, the low byte first), u16 (9; c10 takes its top byte)
    let (chans, max): (Vec<u32>, f32) = match format {
        0 => (vec![texel & 0xff], 255.0),
        7 => (vec![texel & 0xff, texel >> 8 & 0xff], 255.0),
        9 => (vec![texel & 0xffff], 65535.0),
        2..=5 | 12 | 13 => (vec![], 0.0),
        f => return open(format!("a conversion of format {f}")),
    };
    if !chans.is_empty() {
        let value = |c: u32| if norm { c as f32 / max } else { c as f32 };
        return Ok(match conv {
            1 if norm => return open("c10 with normalization"),
            2 if !norm && chans.iter().any(|&c| c > 65504) => return open("f16 past the largest half"),
            1 => vec![
                chans
                    .iter()
                    .enumerate()
                    .map(|(k, &c)| (if max > 255.0 { c >> 8 } else { c }) << (10 * k))
                    .sum(),
            ],
            2 => chans
                .chunks(2)
                .map(|p| p.iter().enumerate().map(|(k, &c)| half(value(c)) << (16 * k)).sum())
                .collect(),
            _ => chans.iter().map(|&c| value(c).to_bits()).collect(),
        });
    }
    let alpha = format != 5;
    let [b, g, r, a] = [0, 1, 2, 3].map(|c| texel >> (8 * c) & 0xff);
    let value = |c: u32| if norm { c as f32 / 255.0 } else { c as f32 };
    Ok(match conv {
        1 => {
            if norm {
                return open("c10 with normalization");
            }
            let mut v = vec![b | g << 10 | r << 20, a];
            v.truncate(1 + alpha as usize);
            v
        }
        2 => {
            let h = |c: u32| half(value(c));
            vec![h(r) | h(g) << 16, h(b) | if alpha { h(a) } else { 0x3c00 } << 16]
        }
        _ => [b, g, r, a][..3 + alpha as usize].iter().map(|&c| value(c).to_bits()).collect(),
    })
}

fn check_t3(t3: u32) -> Result<(), Fail> {
    if t3 & 0x187c_7fff != 0 {
        return open(format!("T3 bits 0x{:08x}", t3 & 0x187c_7fff));
    }
    Ok(())
}

/// A float in [0, 65535] as a half, rounded to nearest even.
fn half(x: f32) -> u32 {
    if x == 0.0 {
        return 0;
    }
    let e = (x.log2().floor() as i32).max(-14);
    if e == -14 && x < 2f32.powi(-14) {
        // a subnormal: m * 2^-24 (1024 is the smallest normal, 0x0400)
        return (x as f64 * 2f64.powi(24)).round_ties_even() as u32;
    }
    let m = (x as f64 / 2f64.powi(e - 10)).round_ties_even() as u32;
    // a mantissa that rounded up to 2048 is the next exponent's 1024
    let (m, e) = if m == 2048 { (1024, e + 1) } else { (m, e) };
    ((e + 15) as u32) << 10 | (m & 0x3ff)
}

struct Texture {
    w: u32,
    h: u32,
    format: u32,
    base: u32,
    /// Bytes per row of a stride texture, None for a twiddled one.
    row: Option<u32>,
    address: [u32; 2],
}

impl Texture {
    /// T0's measured fields: the stride or (twiddled) the mip fields 26:17,
    /// filters 13:12, the min filter or STRIDELO 11:9, the address modes 8:0;
    /// any other bit, and T2's low two, is not.
    fn new(t0: u32, t1: u32, t2: u32) -> Result<Texture, Fail> {
        if t0 & 0xf801_c000 != 0 || t2 & 3 != 0 {
            return open(format!("T0 bits 0x{:08x}, T2 bits {}", t0 & 0xf801_c000, t2 & 3));
        }
        let (w, h) = ((t1 >> 12 & 0xfff) + 1, (t1 & 0xfff) + 1);
        let row = match t1 >> 29 {
            // NP2_STRIDE_EXT: the stride in dwords minus 1, STRIDELO T0[11:9]
            // and STRIDEHI T0[26:17]
            6 => Some(((t0 >> 9 & 7 | (t0 >> 17 & 0x3ff) << 3) + 1) * 4),
            0 if w.is_power_of_two() && h.is_power_of_two() => None,
            0 => return open("a twiddled texture of a size other than a power of two"),
            t => return open(format!("texture type {t}")),
        };
        let tex =
            Texture { w, h, format: t1 >> 24 & 31, base: t2 & !3, row, address: [t0 >> 6 & 7, t0 >> 3 & 7] };
        tex.bytes()?;
        Ok(tex)
    }

    /// Bytes per texel (smp-formats).
    fn bytes(&self) -> Result<u32, Fail> {
        Ok(match self.format {
            0 | 1 => 1,
            2..=5 | 7..=11 => 2,
            12 | 13 | 15..=19 | 21 | 22 | 30 | 31 => 4,
            f => return open(format!("texture format {f}")),
        })
    }

    /// The formats returned as B8G8R8A8 channels.
    fn colour(&self) -> bool {
        matches!(self.format, 2..=5 | 12 | 13)
    }

    /// Texel (x, y) addressed by the modes (smp-address): a stride texture
    /// clamps to the edge; a twiddled one repeats (0), mirrors (1), clamps
    /// (2) or mirrors once (3).
    fn fetch(&self, x: i64, y: i64, load: &mut Load) -> Result<u32, Fail> {
        let axis = |t: i64, n: u32, mode: u32| -> Result<u32, Fail> {
            let n = n as i64;
            let clamp = |t: i64| t.clamp(0, n - 1);
            Ok(if self.row.is_some() {
                clamp(t)
            } else {
                match mode {
                    0 => t.rem_euclid(n),
                    1 => {
                        let m = t.rem_euclid(2 * n);
                        if m >= n { 2 * n - 1 - m } else { m }
                    }
                    2 => clamp(t),
                    3 => clamp(if t < 0 { -t - 1 } else { t }),
                    m => return open(format!("address mode {m}")),
                }
            } as u32)
        };
        let (x, y) = (axis(x, self.w, self.address[0])?, axis(y, self.h, self.address[1])?);
        let bytes = self.bytes()?;
        let off = match self.row {
            Some(row) => y * row + x * bytes,
            None => morton(x, y, self.w, self.h) * bytes,
        };
        let a = self.base.wrapping_add(off);
        let dw = load(a & !3).map_err(Fail::Load)?;
        Ok(if bytes == 4 { dw } else { dw >> (8 * (a & 3)) & ((1 << (8 * bytes)) - 1) })
    }

    /// A texel of a colour format as B8G8R8A8: the 16-bit ones expand each
    /// field by repeating its bits, alpha 0xff without one; 13 is R8G8B8A8.
    fn colour_texel(&self, t: u32) -> Result<u32, Fail> {
        let f = |v: u32, lo: u32, n: u32| {
            let x = v >> lo & ((1 << n) - 1);
            let mut out = 0;
            let mut have = 0;
            while have < 8 {
                out = out << n | x;
                have += n;
            }
            out >> (have - 8)
        };
        let bgra = |b: u32, g: u32, r: u32, a: u32| b | g << 8 | r << 16 | a << 24;
        Ok(match self.format {
            2 => bgra(f(t, 0, 4), f(t, 4, 4), f(t, 8, 4), f(t, 12, 4)),
            3 => bgra(f(t, 0, 2), f(t, 2, 3), f(t, 5, 3), f(t, 8, 8)),
            4 => bgra(f(t, 0, 5), f(t, 5, 5), f(t, 10, 5), f(t, 15, 1)),
            5 => bgra(f(t, 0, 5), f(t, 5, 6), f(t, 11, 5), 0xff),
            12 => t,
            13 => t & 0xff00_ff00 | (t & 0xff) << 16 | (t >> 16 & 0xff),
            _ => unreachable!(),
        })
    }

    /// .pcf (smp-pcf): texel OP reference by the compare mode (never, <,
    /// ==, <=, >, >=, !=, always) on an f32 texture (format 18) or a u16 one
    /// (9, read as texel / 65535), giving 1.0 or 0xffff when it holds, else
    /// 0. Filtered, the results average: measured where they agree and for
    /// one of two texels at half weight.
    fn pcf(&self, t: [u32; 4], w: [u32; 4], r: f32, mode: u32) -> Result<u32, Fail> {
        let value = |t: u32| -> Result<f64, Fail> {
            Ok(match self.format {
                18 if f32::from_bits(t).is_nan() => return open("a NaN texel with .pcf"),
                18 => f32::from_bits(t) as f64,
                9 => t as f64 / 65535.0,
                f => return open(format!(".pcf on format {f}")),
            })
        };
        let r = r as f64;
        let mut held = 0;
        for k in 0..4 {
            let x = value(t[k])?;
            let ok = match mode {
                0 => false,
                1 => x < r,
                2 => x == r,
                3 => x <= r,
                4 => x > r,
                5 => x >= r,
                6 => x != r,
                _ => true,
            };
            held += ok as u32 * w[k];
        }
        let one = if self.format == 18 { 0x3f80_0000 } else { 0xffff };
        Ok(match held {
            0 => 0,
            4096 => one,
            2048 if w.iter().filter(|&&w| w == 2048).count() == 2 => {
                if self.format == 18 {
                    0x3f00_0000
                } else {
                    0x8000
                }
            }
            _ => return open("filtered .pcf at weights other than one of two halves"),
        })
    }

    /// A texel of the other formats, as stored (smp-formats): 19 clears the
    /// sign bit, 21 the top byte, 30 sets it.
    fn plain_texel(&self, t: u32) -> Result<u32, Fail> {
        Ok(match self.format {
            19 => t & 0x7fff_ffff,
            21 => t & 0x00ff_ffff,
            30 => t | 0xff00_0000,
            _ => t,
        })
    }
}

/// The twiddled index of (x, y) (smp-twiddled): y in bit 0, x in bit 1, and
/// so on; the longer side's extra bits above.
fn morton(x: u32, y: u32, w: u32, h: u32) -> u32 {
    let (bw, bh) = (w.trailing_zeros(), h.trailing_zeros());
    let common = bw.min(bh);
    let mut i = 0;
    for b in 0..common {
        i |= (y >> b & 1) << (2 * b) | (x >> b & 1) << (2 * b + 1);
    }
    let rest = if bw > bh { x } else { y } >> common;
    i | rest << (2 * common)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A texture whose dwords count up from 0x100 at 0x1000.
    fn counting(a: u32) -> Result<u32, crate::Error> {
        Ok(0x100 + (a - 0x1000) / 4)
    }

    /// T0-T3 of a texture at 0x1000: `ty` 6 (stride, `stride` dwords) or 0.
    fn state(ty: u32, fmt: u32, w: u32, h: u32, stride: u32, t0: u32, t3: u32) -> [u32; 4] {
        let s = stride - 1;
        let t0 = t0 | (s & 7) << 9 | (s >> 3) << 17;
        [t0, ty << 29 | fmt << 24 | (w - 1) << 12 | (h - 1), 0x1000, t3]
    }

    fn at(st: [u32; 4], u: f32, v: f32) -> Result<Vec<u32>, Fail> {
        sample(&Sample { state: st, u, v, one_d: false, pcf: None }, &mut counting)
    }

    #[test]
    fn point_sampling_picks_and_clamps() {
        let st = state(6, 12, 4, 4, 4, 0, 0);
        assert_eq!(at(st, 0.0, 0.0), Ok(vec![0x100]));
        assert_eq!(at(st, 0.6, 0.3), Ok(vec![0x100 + 4 + 2]));
        // outside [0, 1) clamps to the edge
        assert_eq!(at(st, 1.5, -0.2), Ok(vec![0x103]));
        assert_eq!(
            sample(&Sample { state: st, u: 0.6, v: 0.9, one_d: true, pcf: None }, &mut counting),
            Ok(vec![0x102])
        );
    }

    #[test]
    fn twiddled_textures_interleave_y_then_x() {
        let st = state(0, 12, 4, 4, 1, 2 << 6 | 2 << 3, 0);
        // (x, y) = (1, 0) is index 2, (0, 1) index 1, (3, 3) 15
        assert_eq!(at(st, 0.3, 0.0), Ok(vec![0x102]));
        assert_eq!(at(st, 0.0, 0.3), Ok(vec![0x101]));
        assert_eq!(at(st, 0.9, 0.9), Ok(vec![0x10f]));
        assert_eq!(morton(5, 1, 8, 2), 0b1011);
    }

    #[test]
    fn twiddled_address_modes() {
        let x = |mode: u32, u: f32| at(state(0, 12, 4, 4, 1, mode << 6 | 2 << 3, 0), u, 0.0);
        // on row 0, texels 1, 2, 3 are indices 2, 8, 10 (y in bit 0)
        assert_eq!(x(0, 1.3), Ok(vec![0x102]), "repeat");
        assert_eq!(x(1, 1.3), Ok(vec![0x108]), "mirror: 5 is 2");
        assert_eq!(x(2, 1.3), Ok(vec![0x10a]), "clamp");
        assert_eq!(x(3, -0.3), Ok(vec![0x102]), "mirror once: |-1.2| is 1");
        assert!(x(4, 0.0).is_err());
    }

    #[test]
    fn bilinear_truncates_the_weights_and_rounds_each_channel() {
        // between 0x...ff and 0x...0f in byte 0, two rows: fy = k/2 (truncated)
        let st = state(6, 12, 2, 2, 2, 1 << 12, 0);
        let mut mem = |a: u32| Ok(if a < 0x1008 { 0xff } else { 0x0f });
        let mut y = |k: f32| {
            sample(&Sample { state: st, u: 0.25, v: 0.25 + k / 256.0, one_d: false, pcf: None }, &mut mem)
        };
        assert_eq!(y(0.0), Ok(vec![0xff]));
        assert_eq!(y(1.0), Ok(vec![0xff]));
        assert_eq!(y(3.0), Ok(vec![251]));
        // fy = 2/64: (0xff * 62 + 0x0f * 2) * 64 / 4096 = 247.5, halves up
        assert_eq!(y(4.0), Ok(vec![248]));
    }

    #[test]
    fn colour_formats_expand_and_convert() {
        let st = |fmt: u32, t3: u32| state(6, fmt, 2, 2, 1, 0, t3);
        let one = |fmt: u32, t3: u32, texel: u32| {
            sample(&Sample { state: st(fmt, t3), u: 0.0, v: 0.0, one_d: false, pcf: None }, &mut |_| {
                Ok(texel)
            })
        };
        assert_eq!(one(5, 0, 0xf81f), Ok(vec![0xffff_00ff]), "565: R and B full, alpha 0xff");
        assert_eq!(one(2, 0, 0x1234), Ok(vec![0x1122_3344]), "4444");
        assert_eq!(one(13, 0, 0x4433_2211), Ok(vec![0x4411_2233]), "R8G8B8A8");
        assert_eq!(one(0, 0, 0x4433_2211), Ok(vec![0x11]), "a byte");
        // B, G, R, A = 1, 2, 3, 4
        let t = 0x0403_0201;
        assert_eq!(one(12, 1 << 16, t), Ok(vec![1 | 2 << 10 | 3 << 20, 4]), "c10");
        assert_eq!(one(12, 2 << 16, t), Ok(vec![0x4200 | 0x4000 << 16, 0x3c00 | 0x4400 << 16]), "f16");
        let f32s = [1.0f32, 2.0, 3.0, 4.0].map(f32::to_bits).to_vec();
        assert_eq!(one(12, 3 << 16, t), Ok(f32s), "f32");
        let norm = [1.0f32 / 255.0, 2.0 / 255.0, 3.0 / 255.0, 4.0 / 255.0].map(f32::to_bits).to_vec();
        assert_eq!(one(12, 3 << 16 | 1 << 15, t), Ok(norm), "normalized");
        assert_eq!((half(1.0), half(255.0), half(1.0 / 255.0)), (0x3c00, 0x5bf8, 0x1c04));
        assert_eq!(half(1.0 / 65535.0), 0x0100);
    }

    #[test]
    fn without_alpha_the_conversions_drop_it_or_give_one() {
        // 565 (smp-convert): c10 writes one register, f32 three, f16 alpha 1.0
        let one = |t3: u32| {
            let st = state(6, 5, 2, 2, 1, 0, t3);
            sample(&Sample { state: st, u: 0.0, v: 0.0, one_d: false, pcf: None }, &mut |_| Ok(0xffff))
        };
        assert_eq!(one(1 << 16), Ok(vec![0xff | 0xff << 10 | 0xff << 20]));
        assert_eq!(one(2 << 16), Ok(vec![0x5bf8 | 0x5bf8 << 16, 0x5bf8 | 0x3c00 << 16]));
        assert_eq!(one(3 << 16).map(|v| v.len()), Ok(3));
    }

    #[test]
    fn the_other_formats_convert_their_channels() {
        // smp-convert: u8 (0), two u8 (7, low byte first), u16 (9)
        let one = |fmt: u32, t3: u32, texel: u32| {
            let st = state(6, fmt, 2, 2, 1, 0, t3);
            sample(&Sample { state: st, u: 0.0, v: 0.0, one_d: false, pcf: None }, &mut |_| Ok(texel))
        };
        let f = |x: f32| x.to_bits();
        assert_eq!(one(0, 1 << 16, 0xd0), Ok(vec![0xd0]));
        assert_eq!(one(0, 2 << 16, 0xd0), Ok(vec![0x5a80]));
        assert_eq!(one(0, 3 << 16, 0xd0), Ok(vec![f(208.0)]));
        assert_eq!(one(0, 3 << 16 | 1 << 15, 0xd0), Ok(vec![0x3f50_d0d1]));
        assert_eq!(one(7, 1 << 16, 0x0451), Ok(vec![0x1051]));
        assert_eq!(one(7, 2 << 16, 0x0451), Ok(vec![0x4400_5510]));
        assert_eq!(one(7, 3 << 16, 0x0451), Ok(vec![f(81.0), f(4.0)]));
        assert_eq!(one(7, 3 << 16 | 1 << 15, 0x0451), Ok(vec![0x3ea2_a2a3, 0x3c80_8081]));
        assert_eq!(one(9, 1 << 16, 0xd000), Ok(vec![0xd0]));
        assert_eq!(one(9, 2 << 16, 0x0451), Ok(vec![0x6451]));
        assert_eq!(one(9, 3 << 16, 0xd000), Ok(vec![f(53248.0)]));
        assert_eq!(one(9, 3 << 16 | 1 << 15, 0x0451), Ok(vec![0x3c8a_208a]));
        assert!(one(1, 3 << 16, 0x80).is_err(), "s8");
        assert!(one(9, 2 << 16, 0xfff0).is_err(), "past the largest half");
    }

    #[test]
    fn pcf_compares_the_texel_with_the_reference() {
        let pcf = |fmt: u32, mode: u32, t0: u32, r: f32, texels: [u32; 2], u: f32| {
            let st = state(6, fmt, 4, 4, 4, t0, mode << 29);
            // v = 1/8: y = 0, fy = 0
            let s = Sample { state: st, u, v: 0.125, one_d: false, pcf: Some(r) };
            sample(&s, &mut |a| Ok(if a & 4 == 0 { texels[0] } else { texels[1] }))
        };
        let f = |x: f32| [x.to_bits(); 2];
        // f32 (format 18): 1.0 when texel OP reference
        assert_eq!(pcf(18, 1, 0, 0.5, f(0.25), 0.0), Ok(vec![0x3f80_0000]), "<");
        assert_eq!(pcf(18, 4, 0, 0.5, f(0.25), 0.0), Ok(vec![0]), ">");
        assert_eq!(pcf(18, 7, 0, 0.5, f(0.25), 0.0), Ok(vec![0x3f80_0000]), "always");
        // u16 (format 9): the texel / 65535, 0xffff when it holds
        assert_eq!(pcf(9, 4, 0, 0.5, [0xd000; 2], 0.0), Ok(vec![0xffff]));
        // bilinear: two texels at half weight, one holding
        // texels 0x0451 and 0xd000 share a dword
        assert_eq!(pcf(9, 4, 1 << 12, 0.5, [0xd000_0451; 2], 0.25), Ok(vec![0x8000]));
        assert_eq!(pcf(18, 1, 1 << 12, 0.5, [0, 0x4000_0000], 0.25), Ok(vec![0x3f00_0000]));
        // a colour format ignores it
        assert_eq!(pcf(12, 4, 0, 0.5, [0xd000_0453; 2], 0.0), Ok(vec![0xd000_0453]));
        // other weights are not measured
        assert!(pcf(9, 4, 1 << 12, 0.5, [0xd000_0451; 2], 0.2).is_err());
    }

    #[test]
    fn what_is_not_measured_stops() {
        assert!(at(state(6, 12, 4, 4, 4, 1 << 27, 0), 0.0, 0.0).is_err(), "T0 bit 27");
        assert!(at(state(6, 12, 4, 4, 4, 0, 1 << 18), 0.0, 0.0).is_err(), "T3 bit 18");
        assert!(at(state(6, 12, 4, 4, 4, 0, 0xa080_0000), 0.0, 0.0).is_ok(), "the copies' T3");
        assert!(at(state(4, 12, 4, 4, 1, 0, 0), 0.0, 0.0).is_err(), "tiled");
        assert!(at(state(6, 14, 4, 4, 4, 0, 0), 0.0, 0.0).is_err(), "format 14");
        assert!(at(state(6, 22, 4, 4, 4, 0, 3 << 16), 0.0, 0.0).is_err(), "conversion of a dword format");
    }
}
