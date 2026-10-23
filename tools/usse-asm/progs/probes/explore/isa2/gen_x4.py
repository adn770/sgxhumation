import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
from gen_x1 import xp, P, f
del P[1:]
S0V = [0, 1, 2, 3, 0x80, 0xff, 0x100, 0x8000, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff, 0x3f800000, 0xbf800000, 0x3f000000, 0x00000005]
ops = []
for mn, sel, vals in [("movmsk.mask.smsk", ".1", [0x00000100, 0x00008000, 0x0000fe00]), ("movmsk.smsk.u8", ".1", [0x00000100, 0x00008000, 0x00007f00]),
                      ("movmsk.flt.smsk", "", [f(1.0), f(0.25), f(-1.0)]), ("movmsk.flt16.smsk", ".1", [0x003c0000, 0x00340000, 0x00bc0000])]:
    for v in vals:
        ops.append(f"mov r9, #{v:#x}\nmov r18, #0x11223344\n" + "\n".join(f"{mn} r{10+i}, r{1+i}, r18, r9{sel}" for i in range(8)))
xp("x-smsk2", "movmsk smsk s0", ops, [S0V[:8], S0V[8:]])
open(pathlib.Path(__file__).parent / 'x4.toml', 'w').write("\n".join(P))
