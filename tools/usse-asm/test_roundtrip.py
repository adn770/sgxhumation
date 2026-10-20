"""Round trips over every encoding the disassemblers decode.

For random words, whenever usse-dis (pds-dis) decodes a word to text, the
assembler must give back a word that decodes to the same text. Several
encodings print identically (bits the instruction ignores, operand fields an
operation does not read), so the words are compared through their decodes;
the share of words that come back bit-identical is reported. Where the
oracles are built, refdisasm545 / refpds545 must also print the original and
the reassembled word identically.

USSE words are drawn per form of the decoder's table (its fixed bits, random
elsewhere, plus one- or two-bit flips of the fixed bits) and uniformly; PDS
words uniformly per opcode, as pds_oracle_check.py does.
"""
import random
import subprocess

import pytest

from conftest import DIS_DIR, REFDIS, REFPDS, disasm, run_asm

SEED = 20261020
PER_FORM = 1500
UNIFORM = 50000
PDS_PER_OPCODE = 4000


def usse_corpus(rng):
    out = subprocess.run([str(DIS_DIR / "usse-dis"), "--forms"], capture_output=True,
                         text=True, check=True).stdout
    words = []
    for line in out.splitlines():
        _, mask, val, _, pat = line.split("\t")
        if pat == "(reject)":
            continue
        mask, val = int(mask, 16), int(val, 16)
        fixed = [b for b in range(64) if mask >> b & 1]
        for i in range(PER_FORM):
            w = val | (rng.getrandbits(64) & ~mask & (2**64 - 1))
            if i % 3 == 2:
                for b in rng.sample(fixed, min(len(fixed), rng.choice((1, 2)))):
                    w ^= 1 << b
            words.append(w)
    return words + [rng.getrandbits(64) for _ in range(UNIFORM)]


def decoded(text):
    return not (text.startswith((".long", ".word")) or "TODO" in text)


def round_trip(tool, dis, words, tmp_path, oracle, *dis_args):
    texts = disasm(dis, words, tmp_path, *dis_args)
    keep = [(w, t) for w, t in zip(words, texts) if decoded(t)]
    again = run_asm(tool, "\n".join(t for _, t in keep) + "\n", tmp_path)
    assert len(again) == len(keep)
    texts2 = disasm(dis, again, tmp_path, *dis_args)
    bad = [(w, t, w2, t2) for (w, t), w2, t2 in zip(keep, again, texts2) if t != t2]
    same = sum(w == w2 for (w, _), w2 in zip(keep, again))
    print(f"\n{tool}: {len(words)} words, {len(keep)} decoded, {len(bad)} failed, "
          f"{same} bit-identical, {len(keep) - same} canonicalised "
          f"({100 * (len(keep) - same) / max(len(keep), 1):.1f}%)")
    assert not bad, bad[:5]
    if oracle.exists():
        ref1 = disasm(oracle, [w for w, _ in keep], tmp_path, *dis_args)
        ref2 = disasm(oracle, again, tmp_path, *dis_args)
        diff = [(w, a, b) for (w, _), a, b in zip(keep, ref1, ref2) if a != b]
        print(f"  oracle {oracle.name}: {len(keep) - len(diff)}/{len(keep)} identical")
        assert not diff, diff[:5]
    return len(keep), same


def test_usse_round_trip(tmp_path):
    words = usse_corpus(random.Random(SEED))
    n, _ = round_trip("usse-asm", DIS_DIR / "usse-dis", words, tmp_path, REFDIS)
    assert n > 50000


def test_pds_round_trip(tmp_path):
    rng = random.Random(SEED)
    words = [(op << 27) | rng.getrandbits(27) for op in range(32) for _ in range(PDS_PER_OPCODE)]
    n, _ = round_trip("pds-asm", DIS_DIR / "pds-dis", words, tmp_path, REFPDS, "0")
    assert n > 80000


ROOT = DIS_DIR.parent.parent
EXTRACTED = ROOT / "harness/ukernel-extract/extracted"
SGX540 = ROOT / "reference/sgx540-reversing/ukernel"


def words_of(blob, width):
    data = blob.read_bytes()
    return [int.from_bytes(data[i:i + width], "little") for i in range(0, len(data), width)]


def test_usse_corpus_round_trip(tmp_path):
    """The SGX545 microkernel reassembles bit for bit: the assembler's choices
    for ignored bits follow the closed toolchain's (usse_form.h, dflt)."""
    blob = EXTRACTED / "sgx545_ukernel_template.bin"
    if not blob.exists():
        pytest.skip("ukernel capture not present")
    n, same = round_trip("usse-asm", DIS_DIR / "usse-dis", words_of(blob, 8), tmp_path, REFDIS)
    assert n >= 6631 and same == n


@pytest.mark.parametrize("name", ["ukernel", "TA", "3D", "SPM"])
def test_sgx540_corpus_round_trip(tmp_path, name):
    blob = SGX540 / name
    if not blob.exists():
        pytest.skip("SGX540 corpus not present")
    words = words_of(blob, 8)
    texts = disasm(DIS_DIR / "usse-dis", words, tmp_path, opts=("--core", "540"))
    assert len(texts) == len(words)
    keep = [(w, t) for w, t in zip(words, texts) if decoded(t)]
    assert len(keep) > 2000
    again = run_asm("usse-asm", "\n".join(t for _, t in keep) + "\n", tmp_path, "--core", "540")
    assert again == [w for w, _ in keep]


@pytest.mark.parametrize("prog,data", [(10, 4), (11, 8), (12, 4), (13, 24), (14, 12)])
def test_edm_pds_programs_round_trip(tmp_path, prog, data):
    """The captured EDM PDS programs reassemble bit for bit from pds-dis output."""
    blob = EXTRACTED / f"sgx545_pds{prog}_runtime.bin"
    if not blob.exists():
        pytest.skip("PDS capture not present")
    listing = subprocess.run([str(DIS_DIR / "pds-dis"), str(blob), str(data)],
                             capture_output=True, text=True, check=True).stdout
    assert run_asm("pds-asm", listing, tmp_path) == words_of(blob, 4)
