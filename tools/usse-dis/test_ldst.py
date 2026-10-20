"""Pin the load/store text of usse-dis to refdisasm545 for fetch and increment forms.

The printed immediate is the encoded offset field, exactly as Imagination's
disassembler prints it. It is NOT always the first element accessed: a fetch with
a base+offset address starts one element later (README, "Load/store addressing").
These tests stop anyone from "fixing" that into the text, which would break
oracle compatibility. Expected texts are refdisasm545 output for the same words;
the last test re-checks them against the oracle when it has been built.
"""
import pathlib
import struct
import subprocess

import pytest

HERE = pathlib.Path(__file__).resolve().parent
REFDIS = HERE / "refdisasm" / "refdisasm545"

# word -> refdisasm545 text (all from the SGX545 ukernel)
FORMS = {
    0xE8033002A0608080: "ldad.bpcache.fetch4 r3, [r2,+#1], drc0",
    0xE8031002A0400D00: "ldad.bpcache.fetch2 r2, [r0,+#26], drc0",
    0xF002300280008483: "stad.bpcache.fetch4 [r2,+#9], r3",
    0xE82B0200A0800080: "ldad.fcfill r4, [r0,#1++], drc0",
    0xE8036202A0A00000: "ldad.bpcache.fetch7 r5, [r0,#0++], drc0",
    0xF002120280006905: "stad.bpcache.fetch2 [r1,#82++], r5",
    0xF022000280008081: "stad.bpcache [r2,+#1], r1",
    0xE823000AA0000080: "ldad.bpcache r0, [r0,-#1], drc0",
    0xF020000200044994: "stad.bpcache [r17,+r19], r20",
}


def _disasm(tool, tmp_path, words):
    blob = tmp_path / "words.bin"
    blob.write_bytes(struct.pack(f"<{len(words)}Q", *words))
    out = subprocess.run([str(tool), str(blob)], capture_output=True, text=True,
                         check=True).stdout
    return [line.split("\t", 1)[1].strip() for line in out.splitlines() if "\t" in line]


@pytest.fixture(scope="module")
def ours():
    subprocess.run(["make", "-s", "-C", str(HERE), "usse-dis"], check=True)
    return HERE / "usse-dis"


def test_fetch_and_increment_text_matches_oracle(ours, tmp_path):
    assert _disasm(ours, tmp_path, list(FORMS)) == list(FORMS.values())


@pytest.mark.skipif(not REFDIS.exists(), reason="refdisasm545 not built")
def test_expected_texts_are_the_oracle_output(tmp_path):
    assert _disasm(REFDIS, tmp_path, list(FORMS)) == list(FORMS.values())
