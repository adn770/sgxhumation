"""The radare2 plugin round-trips: rasm2 disassembles what usse-dis decodes to the same text,
and assembles that text back to a word that decodes the same.

Needs the plugin built and installed (make install) and rasm2 from the workspace r2 build
(../../radare2-prefix) or R2_PREFIX; skipped otherwise.
"""
import os
import pathlib
import random
import shutil
import struct
import subprocess

import pytest

HERE = pathlib.Path(__file__).resolve().parent
PREFIX = pathlib.Path(os.environ.get("R2_PREFIX", HERE.parent.parent / "radare2-prefix"))
RASM2 = PREFIX / "bin" / "rasm2"
DIS = HERE.parent / "usse-dis"


def rasm2(*args):
    r = subprocess.run([str(RASM2), "-a", "usse", "-b", "32", *args], capture_output=True,
                       text=True)
    return r.stdout.strip()


def decoded_words(n, seed=1):
    rng = random.Random(seed)
    words = [rng.getrandbits(64) for _ in range(n * 6)]
    blob = HERE / "test_r2.bin"
    blob.write_bytes(struct.pack(f"<{len(words)}Q", *words))
    out = subprocess.run([str(DIS), str(blob)], capture_output=True, text=True).stdout
    blob.unlink()
    pairs = []
    for line in out.splitlines():
        if "\t" not in line or ": 0x" not in line:
            continue
        head, text = line.split("\t", 1)
        if text.startswith(".long") or "TODO" in text:
            continue
        pairs.append((int(head.split("0x")[1], 16), text.strip()))
    return pairs[:n]


@pytest.mark.skipif(not RASM2.exists(), reason="no workspace rasm2")
def test_round_trip():
    if not shutil.which(str(RASM2)) or "usse" not in rasm2("-L"):
        pytest.skip("usse plugin not installed (make install)")
    pairs = decoded_words(300)
    assert len(pairs) > 100
    for word, text in pairs:
        hexbytes = struct.pack("<Q", word).hex()
        assert rasm2("-d", hexbytes) == text, hex(word)
        back = rasm2(text)
        assert len(back) == 16, (text, back)
        assert rasm2("-d", back) == text, (text, back)
