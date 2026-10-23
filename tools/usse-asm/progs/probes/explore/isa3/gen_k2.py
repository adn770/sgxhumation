"""Exploration k2 (coords): per-pixel address; store, cache op, load -> o0."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
CO = ["", "ci r20, r21, #1", "ci r20, r21, #2", "ci r20, r21, #3", "cf r20, r21, #3", "cfi r20, r21, #3",
      "ci.global r20, r21, #3", "cfi.global r20, r21, #3", "ci.global r20, r21, #1", "ci.global r20, r21, #2"]
cases, labels = [], []
for co in CO:
    for variant in ["store", "load-store"]:
        pre = ("shr.skipinv r21, r1, #12\nmov r20, #0x5CAF4000\nimae.u r9, r21.low, #4.low, r20, u32\nmov r21, r9\n")
        if variant == "load-store":
            pre += "ldad r11, [r9,+#0], drc0\nwdf drc0\n"
        op = pre + "stad [r9,+#0], r6\n" + co + "\nldad r10, [r9,+#0], drc0\nwdf drc0"
        cases.append([pre, op[len(pre):], "r10"]); labels.append(f"{variant}: {co or 'nothing'}")
P = [HDR, probe("x-cache2", "cache2", "coords.s", {}, ["PRE", "OP", "OUT"], cases, {}, labels=labels, coords=True, pixels=True)]
(pathlib.Path(__file__).parent / 'k2.toml').write_text("\n".join(P))
