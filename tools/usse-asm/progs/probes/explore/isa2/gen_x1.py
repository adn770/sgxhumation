"""Exploration table 1 for round 2: recorded cases (no hypotheses), 8 results per fill."""
import random, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
R = random.Random(2545)
def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def h(x): return struct.unpack('<H', struct.pack('<e', x))[0]
P = [HDR]
def rb(): return R.getrandbits(32)
EDGE = [0x00000000, 0xffffffff, 0x80808080, 0x7f7f7f7f, 0x01ff00fe, 0xff00ff01, 0x40c02080, 0x12345678]
def rand8(): return [rb() for _ in range(8)]
def xp(name, q, ops_list, inputs_list, out='r10'):
    """ops_list: list of OP strings; each combined with each input set of inputs_list"""
    cases = []
    for op in ops_list:
        for ins in inputs_list:
            cases.append([op] + list(ins))
    P.append(probe(name, q, 'dump.s', dparams(OUT=out), ['OP'] + list('ABCDEFGH'), cases, {}))
PAIRS = [(1, 2), (2, 3), (3, 4), (4, 5), (5, 6), (6, 7), (7, 8), (8, 1)]
TRIPS = [(1, 2, 3), (2, 3, 4), (3, 4, 5), (4, 5, 6), (5, 6, 7), (6, 7, 8), (7, 8, 1), (8, 1, 2)]
def eight2(tmpl):   # tmpl with {d},{a},{b}
    return "\n".join(tmpl.format(d=f"r{10+i}", a=f"r{a}", b=f"r{b}") for i, (a, b) in enumerate(PAIRS))
def eight3(tmpl):
    return "\n".join(tmpl.format(d=f"r{10+i}", a=f"r{a}", b=f"r{b}", c=f"r{c}")
                     for i, (a, b, c) in enumerate(TRIPS))
INS3 = [EDGE, rand8(), rand8()]

# ---- sop2 ----------------------------------------------------------------------------------
C1 = ["zero", "s1", "s2", "s1a", "s2a", "asat", "s2scale"]
C2 = ["zero", "s1", "s2", "s1a", "s2a", "asat", "zeros"]
A1 = ["zero", "s1a", "s2a", "s2scale"]
A2 = ["zero", "s1a", "s2a", "zeros"]
ops = []
for x in C1:
    for c in ["", ".comp"]:
        ops.append(f"sop2 {{d}}, {{a}}, {{b}}, {x}{c}, zero, add+asop2 zero, zero, add")
for y in C2:
    for c in ["", ".comp"]:
        ops.append(f"sop2 {{d}}, {{a}}, {{b}}, zero, {y}{c}, add+asop2 zero, zero, add")
for x in A1:
    for c in ["", ".comp"]:
        ops.append(f"sop2 {{d}}, {{a}}, {{b}}, zero, zero, add+asop2 {x}{c}, zero, add")
for y in A2:
    for c in ["", ".comp"]:
        ops.append(f"sop2 {{d}}, {{a}}, {{b}}, zero, zero, add+asop2 zero, {y}{c}, add")
for o in ["add", "sub", "min", "max"]:
    ops.append(f"sop2 {{d}}, {{a}}, {{b}}, zero.comp, zero.comp, {o}+asop2 zero.comp, zero.comp, {o}")
    ops.append(f"sop2 {{d}}, {{a}}, {{b}}, s1, s2, {o}+asop2 s1a, s2a, {o}")
ops += ["sop2 {d}, {a}, {b}, comp, zero.comp, zero, add+asop2 comp, zero.comp, zero, add",
        "sop2 {d}, {a}, {b}, comp, s1, s2, add+asop2 comp, s1a, s2a, add",
        "sop2 {d}, {a}, {b}, zero.comp, zero, add+asop2 zero.comp, zero, add, neg",
        "sop2 {d}, {a}, {b}, zero.comp, zero.comp, sub+asop2 zero.comp, zero.comp, sub, neg",
        "sop2 {d}, {a}, {b}, s1, s1, add+asop2 s1a, s1a, add",
        "sop2 {d}, {a}, {b}, s2a, s1a.comp, add+asop2 s2a, s1a, add"]
xp("x-sop2", "sop2 exploration", [eight2(o) for o in ops], INS3)

# ---- sop3 / lrp1 / lrp2 ---------------------------------------------------------------------
S3 = ["zero", "asat", "s1", "s1a", "s0", "s0a", "s2", "s2a"]
ops = []
for x in S3:
    for c in ["", ".comp"]:
        ops.append(f"sop3 {{d}}, {{a}}, {{b}}, {{c}}, {x}{c}, zero, add+arsop zero, add")
        ops.append(f"sop3 {{d}}, {{a}}, {{b}}, {{c}}, zero, {x}{c}, add+arsop zero, add")
for x in ["zero", "s0a", "s1a", "s2a"]:
    for c in ["", ".comp"]:
        ops.append(f"sop3 {{d}}, {{a}}, {{b}}, {{c}}, zero, zero, add+arsop {x}{c}, add")
        ops.append(f"sop3 {{d}}, {{a}}, {{b}}, {{c}}, zero, zero, add+asop {x}{c}, zero, add")
        ops.append(f"sop3 {{d}}, {{a}}, {{b}}, {{c}}, zero, s1, add+asop {x}{c}, s1, add")
for o in ["add", "sub"]:
    ops.append(f"sop3 {{d}}, {{a}}, {{b}}, {{c}}, zero.comp, zero.comp, {o}+arsop zero.comp, {o}")
    ops.append(f"sop3 {{d}}, {{a}}, {{b}}, {{c}}, s0, s2a, {o}+asop s0a, s2a, {o}")
for a1 in ["zero", "s1a"]:
    for a2 in ["zero", "s2a"]:
        for c1 in ["", ".comp"]:
            for c2 in ["", ".comp"]:
                ops.append(f"sop3 {{d}}, {{a}}, {{b}}, {{c}}, zero, zero, add+alrp {a1}{c1}, {a2}{c2}")
ops.append("sop3 {d}, {a}, {b}, {c}, zero.comp, zero.comp, add, neg+alrp zero, zero")
for x in ["zero", "s1"]:
    for y in ["zero", "s2"]:
        for z in ["s0", "s0a"]:
            for cc in [("", ""), (".comp", ""), ("", ".comp")]:
                ops.append(f"lrp1 {{d}}, {{a}}, {{b}}, {{c}}, {x}{cc[0]}, {y}{cc[1]}, {z}+asop zero, zero, add")
for x in S3:
    ops.append(f"lrp1 {{d}}, {{a}}, {{b}}, {{c}}, zero, zero, s0+asop {x}, zero, add")
    ops.append(f"lrp1 {{d}}, {{a}}, {{b}}, {{c}}, zero, zero, s0+asop zero, {x}.comp, sub")
for m in ["s1, s2", "s1, s2.comp", "one, s2", "s1.comp, s2"]:
    for x, y in [("zero", "zero"), ("s0", "s0"), ("s0a", "s0a"), ("s0", "zero"), ("zero", "s0"),
                 ("s1", "s2"), ("s2a", "s1a"), ("zero.comp", "zero")]:
        ops.append(f"lrp2 {{d}}, {{a}}, {{b}}, {{c}}, {x}, {y}, {m}+alrp zero, zero")
for a1 in ["zero", "s1a"]:
    for a2 in ["zero", "s2a"]:
        ops.append(f"lrp2 {{d}}, {{a}}, {{b}}, {{c}}, zero, zero, s1, s2+alrp {a1}, {a2}")
        ops.append(f"lrp2 {{d}}, {{a}}, {{b}}, {{c}}, zero, zero, s1, s2+alrp {a1}.comp, {a2}")
xp("x-sop3", "sop3/lrp exploration", [eight3(o) for o in ops], INS3)

# ---- sop2wm -----------------------------------------------------------------------------------
W = ["zero", "asat", "s1", "s1a", "s2", "s2a"]
ops = []
for x in W:
    for c in ["", ".comp"]:
        ops.append(f"sop2wm {{d}}.bytemask1111, {{a}}, {{b}}, {x}{c}, zero, add, add")
        ops.append(f"sop2wm {{d}}.bytemask1111, {{a}}, {{b}}, zero, {x}{c}, add, add")
for o1 in ["add", "sub", "min", "max"]:
    for o2 in ["add", "sub", "min", "max"]:
        ops.append(f"sop2wm {{d}}.bytemask1111, {{a}}, {{b}}, zero.comp, zero.comp, {o1}, {o2}")
for m in range(16):
    ops.append(f"sop2wm {{d}}.bytemask{m:04b}, {{a}}, {{b}}, zero.comp, zero, add, add")
xp("x-sop2wm", "sop2wm exploration", [eight2(o) for o in ops], INS3)

if __name__ == "__main__": open(pathlib.Path(__file__).parent / "x1.toml", "w").write("\n".join(P))
