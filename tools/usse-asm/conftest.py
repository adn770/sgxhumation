"""Shared fixtures for the usse-asm tests: build the tools once, then run them."""
import pathlib
import struct
import subprocess

import pytest

HERE = pathlib.Path(__file__).resolve().parent
DIS_DIR = HERE.parent / "usse-dis"
REFDIS = DIS_DIR / "refdisasm" / "refdisasm545"
REFPDS = DIS_DIR / "refdisasm" / "refpds545"


@pytest.fixture(scope="session", autouse=True)
def build():
    subprocess.run(["make", "-s", "-C", str(DIS_DIR), "usse-dis", "pds-dis"], check=True)
    subprocess.run(["make", "-s", "-C", str(HERE)], check=True)


def run_asm(tool, source, tmp_path, *opts, check=True):
    """Assemble source text; return the words (or the CompletedProcess if check=False)."""
    src = tmp_path / "in.s"
    out = tmp_path / "out.bin"
    src.write_text(source)
    r = subprocess.run([str(HERE / tool), *opts, "-o", str(out), str(src)],
                       capture_output=True, text=True)
    if not check:
        return r
    assert r.returncode == 0, r.stderr
    data = out.read_bytes()
    if tool == "usse-asm":
        return list(struct.unpack(f"<{len(data) // 8}Q", data))
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def disasm(tool, words, tmp_path, *args, opts=()):
    """Texts of words, as usse-dis / pds-dis (or an oracle) print them. args
    follow the blob (pds-dis: the data dwords), opts precede it (--core)."""
    blob = tmp_path / "words.bin"
    fmt = "Q" if tool.name in ("usse-dis", "refdisasm545") else "I"
    blob.write_bytes(struct.pack(f"<{len(words)}{fmt}", *words))
    out = subprocess.run([str(tool), *opts, str(blob), *args], capture_output=True, text=True,
                         check=True).stdout
    return [line.split("\t", 1)[1].strip() for line in out.splitlines()
            if "\t" in line and ": 0x" in line]


@pytest.fixture
def usse_asm(tmp_path):
    return lambda source, *opts, **kw: run_asm("usse-asm", source, tmp_path, *opts, **kw)


@pytest.fixture
def pds_asm(tmp_path):
    return lambda source, *opts, **kw: run_asm("pds-asm", source, tmp_path, *opts, **kw)


@pytest.fixture
def usse_dis(tmp_path):
    return lambda words: disasm(DIS_DIR / "usse-dis", words, tmp_path)


@pytest.fixture
def pds_dis(tmp_path):
    return lambda words, data=0: disasm(DIS_DIR / "pds-dis", words, tmp_path, str(data))
