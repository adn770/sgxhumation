"""Exploration k1: cache control around a store and a load to the scratch buffer."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from tomlgen import *
cases, labels = [], []
CO = ["", "ci r20, r21, #1", "ci r20, r21, #2", "ci r20, r21, #3", "cf r20, r21, #1", "cf r20, r21, #2",
      "cf r20, r21, #3", "cfi r20, r21, #1", "cfi r20, r21, #2", "cfi r20, r21, #3", "ci.global r20, r21, #3",
      "cfi.global r20, r21, #3", "cf.global r20, r21, #3", "ci.dm_nomatch r20, r21, #3"]
for co in CO:
    for variant in ["store", "load-store"]:
        pre = "mov r20, #0x5CAF0100\nmov r21, #0x5CAF0200\nmov r9, #0x5CAF0100\nmov r3, #0x11111111\n"
        if variant == "load-store":
            pre += "ldad r11, [r9,+#0], drc0\nwdf drc0\n"
        op = pre + "stad [r9,+#0], r3\n" + co + "\nldad r10, [r9,+#0], drc0\nwdf drc0"
        cases.append([op, 0, 0, 0, 0, 0, 0, 0, 0]); labels.append(f"{variant}: {co or 'nothing'}")
P = [HDR, probe("x-cache", "cache", "dump.s", dparams(OUT="r10"), ["OP"] + list("ABCDEFGH"), cases, {}, labels=labels)]
(pathlib.Path(__file__).parent / 'k1.toml').write_text("\n".join(P))
