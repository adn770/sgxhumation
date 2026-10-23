// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Josep Torra
//! The single-source float functions frcp, frsq, fsqrt, flog, fexp, fsin and
//! fcos. The hardware's algorithms are not known: its results are measured
//! only to a bound (doc/isa/content/usse/probe/frcp-frsq-precision.md and
//! float-functions.md). What the hardware gives exactly (the specials, and
//! the cases the probe pages call exact) is a value; anything else is the
//! nearest value with the bound the hardware was measured within, for a
//! caller that accepts that (Machine::near), and unsupported otherwise.

/// How far the hardware's result may be from a near value.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Bound {
    /// Within n units in the last place.
    Ulps(u32),
    /// Within 2^-n.
    Abs(i32),
    /// flog's: within one ulp, or within 2^-22 near 1.
    Log,
    /// Computed from a near value: no bound known.
    Derived,
}

impl Bound {
    /// Whether the hardware's `want` is within the bound of `got`.
    pub fn admits(self, got: u32, want: u32) -> bool {
        let ulps = |n: u32| got >> 31 == want >> 31 && (got & 0x7fff_ffff).abs_diff(want & 0x7fff_ffff) <= n;
        let abs = |k: i32| {
            let d = (f32::from_bits(got) as f64 - f32::from_bits(want) as f64).abs();
            d <= 2f64.powi(-k)
        };
        match self {
            Bound::Ulps(n) => ulps(n),
            Bound::Abs(k) => abs(k),
            Bound::Log => ulps(1) || abs(22),
            Bound::Derived => false,
        }
    }
}

impl std::fmt::Display for Bound {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            Bound::Ulps(1) => write!(f, "within 1 ulp"),
            Bound::Ulps(n) => write!(f, "within {n} ulps"),
            Bound::Abs(k) => write!(f, "within 2^-{k}"),
            Bound::Log => write!(f, "within 1 ulp or 2^-22"),
            Bound::Derived => write!(f, "computed from a near value"),
        }
    }
}

const NAN: u32 = 0x7fff_ffff;
const INF: u32 = 0x7f80_0000;
const NEG_INF: u32 = 0xff80_0000;

/// What the function gives for `x` (its source, after the modifiers): the
/// value and None where the hardware is exact, the nearest value and its
/// bound, or Err(why) where the hardware is not measured.
pub fn eval(name: &str, x: u32) -> Result<(u32, Option<Bound>), String> {
    let (e, neg) = (x >> 23 & 0xff, x >> 31 != 0);
    let nan = e == 0xff && x & 0x7f_ffff != 0;
    let inf = e == 0xff && !nan;
    // denormal sources flush to a zero of their sign (hardware, denormals)
    let zero = e == 0;
    let v = if zero { 0.0 } else { f32::from_bits(x) as f64 };
    let exact = |b: u32| Ok((b, None));
    let open = |what: &str| Err(format!("{name} of {what} (not measured)"));
    match name {
        // hardware (frcp-frsq-precision): within one ulp, at most 0.65 ulp
        // from the true value, so a representable true value is the result
        "frcp" | "frsq" | "fsqrt" => {
            if nan {
                return open("a NaN");
            }
            if inf {
                return open("an infinity");
            }
            if zero {
                return match (name, neg) {
                    ("frcp", false) => exact(INF),
                    ("frcp", true) => exact(NEG_INF),
                    ("frsq", false) => exact(INF),
                    ("fsqrt", false) => exact(0),
                    _ => open("-0"),
                };
            }
            if neg && name != "frcp" {
                return exact(NAN);
            }
            let t = match name {
                "frcp" => 1.0 / v,
                "frsq" => 1.0 / v.sqrt(),
                _ => v.sqrt(),
            };
            Ok(nearest(t, Bound::Ulps(1)))
        }
        // hardware (float-functions): log2, exact on powers of two
        "flog" => {
            if nan || neg && !zero {
                return exact(NAN);
            }
            // hardware (fsingle-specials): -0 too
            if zero {
                return exact(NEG_INF);
            }
            if inf {
                return exact(INF);
            }
            Ok(nearest(v.log2(), Bound::Log))
        }
        // hardware (float-functions): 2^x, exact on integers, results below
        // 2^-126 are 0, from 128 up +inf
        "fexp" => {
            // hardware (fsingle-specials)
            if nan {
                return exact(NAN);
            }
            if inf {
                return exact(if neg { 0 } else { INF });
            }
            if v >= 128.0 {
                return exact(INF);
            }
            let t = v.exp2();
            let min = 2f64.powi(-126);
            // where two ulps could cross 2^-126 the result is 0 or not
            if t < min * (1.0 - 2f64.powi(-21)) {
                return exact(0);
            }
            if t < min * (1.0 + 2f64.powi(-21)) && v.fract() != 0.0 {
                return Err("fexp near 2^-126 (0 or not)".to_string());
            }
            if t > f32::MAX as f64 * (1.0 - 2f64.powi(-21)) {
                return Err("fexp near the largest float (inf or not)".to_string());
            }
            Ok(nearest(t, Bound::Ulps(2)))
        }
        // hardware (float-functions): quarter turns, period 4, a fixed-point
        // result with 15 fraction bits, exact on integers; fsin is odd and
        // fcos even (fsin(-0) is -0, fcos(-1) is fcos(1), -0)
        "fsin" | "fcos" => {
            if e == 0xff {
                return exact(NAN);
            }
            let r = v.abs() % 4.0;
            let a = r * std::f64::consts::FRAC_PI_2;
            let odd = name == "fsin" && neg;
            let t = if name == "fsin" { a.sin() } else { a.cos() };
            let q = (t * 32768.0).round() / 32768.0;
            if q == 0.0 {
                // the zeros at integers: fsin(0), fsin(4) +0, fsin(2) -0,
                // fcos(1) -0, fcos(3) +0; others' signs are not measured
                if r.fract() != 0.0 {
                    return Err(format!("{name} of a near-zero result (its sign not measured)"));
                }
                let minus = matches!((name, r as u32), ("fsin", 2) | ("fcos", 1));
                return exact(((minus != odd) as u32) << 31);
            }
            let b = (if odd { -q } else { q } as f32).to_bits();
            Ok((b, (r.fract() != 0.0).then_some(Bound::Abs(15))))
        }
        _ => Err(format!("no function {name}")),
    }
}

/// The nearest float to t: exact when t is a float (and a normal one).
fn nearest(t: f64, b: Bound) -> (u32, Option<Bound>) {
    let f = t as f32;
    let exact = f as f64 == t && f.is_normal();
    (f.to_bits(), (!exact).then_some(b))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn f(x: f32) -> u32 {
        x.to_bits()
    }

    #[test]
    fn exact_where_the_hardware_is() {
        assert_eq!(eval("frcp", f(4.0)), Ok((f(0.25), None)));
        assert_eq!(eval("frcp", 0), Ok((INF, None)));
        assert_eq!(eval("frcp", 0x8000_0000), Ok((NEG_INF, None)));
        assert_eq!(eval("frcp", 1), Ok((INF, None)), "a denormal flushes");
        assert_eq!(eval("frsq", f(16.0)), Ok((f(0.25), None)));
        assert_eq!(eval("fsqrt", f(-1.0)), Ok((NAN, None)));
        assert_eq!(eval("flog", f(8.0)), Ok((f(3.0), None)));
        assert_eq!(eval("flog", 0), Ok((NEG_INF, None)));
        assert_eq!(eval("flog", 0x8000_0000), Ok((NEG_INF, None)));
        assert_eq!(eval("flog", 0x7fc0_0000), Ok((NAN, None)));
        assert_eq!(eval("fexp", 0x7fc0_0000), Ok((NAN, None)));
        assert_eq!(eval("flog", f(-2.0)), Ok((NAN, None)));
        assert_eq!(eval("fexp", f(-3.0)), Ok((f(0.125), None)));
        assert_eq!(eval("fexp", f(-200.0)), Ok((0, None)));
        assert_eq!(eval("fexp", f(128.0)), Ok((INF, None)));
        assert_eq!(eval("fsin", f(1.0)), Ok((f(1.0), None)));
        assert_eq!(eval("fsin", f(-5.0)), Ok((f(-1.0), None)));
        assert_eq!(eval("fcos", f(1.0)), Ok((0x8000_0000, None)));
        assert_eq!(eval("fcos", f(-1.0)), Ok((0x8000_0000, None)));
        assert_eq!(eval("fcos", f(3.0)), Ok((0, None)));
        assert_eq!(eval("fsin", f(2.0)), Ok((0x8000_0000, None)));
        assert_eq!(eval("fsin", f(100.0)), Ok((0, None)));
        assert_eq!(eval("fsin", 0x8000_0000), Ok((0x8000_0000, None)));
        assert_eq!(eval("fcos", 0x40_0000), Ok((f(1.0), None)), "a denormal flushes");
        assert_eq!(eval("fsin", INF), Ok((NAN, None)));
    }

    #[test]
    fn near_elsewhere_with_the_measured_bound() {
        assert_eq!(eval("frcp", f(3.0)), Ok((0x3eaa_aaab, Some(Bound::Ulps(1)))));
        assert_eq!(eval("fexp", f(0.5)), Ok((f(std::f32::consts::SQRT_2), Some(Bound::Ulps(2)))));
        assert_eq!(eval("flog", f(3.0)).unwrap().1, Some(Bound::Log));
        assert_eq!(eval("fsin", f(0.25)).unwrap().1, Some(Bound::Abs(15)));
        // hardware (x-fcos): 23171 / 2^15, the nearest is 23170 / 2^15
        assert_eq!(eval("fcos", f(0.5)), Ok((f(23170.0 / 32768.0), Some(Bound::Abs(15)))));
        assert_eq!(eval("fsin", f(-0.5)).unwrap().0, f(-23170.0 / 32768.0));
    }

    #[test]
    fn unmeasured_inputs_are_refused() {
        assert!(eval("frcp", INF).is_err());
        assert!(eval("fsqrt", 0x8000_0000).is_err());
        assert!(eval("fsin", f(1e-6)).is_err(), "the sign of a tiny result");
    }

    #[test]
    fn bounds_admit_what_the_hardware_may_give() {
        assert!(Bound::Ulps(1).admits(0x3eaa_aaab, 0x3eaa_aaaa));
        assert!(!Bound::Ulps(1).admits(0x3eaa_aaab, 0x3eaa_aaa9));
        assert!(Bound::Abs(15).admits(f(0.5), f(0.5 + 1.0 / 32768.0)));
        assert!(!Bound::Abs(15).admits(f(0.5), f(0.5 + 2.0 / 32768.0)));
        assert!(Bound::Log.admits(f(2f32.powi(-23)), f(2f32.powi(-22))));
        assert!(!Bound::Derived.admits(1, 1 + 1));
    }
}
