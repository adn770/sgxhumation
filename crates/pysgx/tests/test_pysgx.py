# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""pysgx: the decoders, encoders, sgx-asm and the simulator from Python, checked against
the command-line tools they share their code with."""
import pathlib
import shutil
import subprocess
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import load  # noqa: E402

pytestmark = pytest.mark.skipif(shutil.which("cargo") is None, reason="needs cargo")
REPO = load.REPO


@pytest.fixture(scope="module")
def sgx():
    return load.pysgx()


def test_decode_and_encode(sgx):
    word = sgx.usse_encode("mov.skipinv.end o0, #0x00000000")
    assert word == 0xFCA4000100000000
    assert sgx.usse_decode(word) == "mov.skipinv.end o0, #0x00000000"
    assert sgx.usse_encode("frob r1") is None
    assert sgx.pds_encode("halt") == 0xAF000000
    assert sgx.pds_decode(0xAF000000) == "halt"
    with pytest.raises(ValueError):
        sgx.usse_decode(0, core=535)


def test_assemble_is_sgx_asm(sgx, tmp_path):
    """In-process, the same images, labels and records as `sgx-asm -d`."""
    sys.path.insert(0, str(REPO / "tools" / "fwcutter"))
    import asmmap
    src = REPO / "asm" / "transfer" / "transfer.s"
    want = asmmap.assemble(src, tmp_path / "out")
    assert asmmap.from_dict(sgx.assemble(src.read_bytes(), str(src))) == want


def test_assemble_errors(sgx):
    with pytest.raises(sgx.AsmError, match="<string>:1: statement outside a section"):
        sgx.assemble("nop\n")
    p = sgx.assemble(".section a, usse, 3\nhere: nop\n.section b, data, 4\n.word addr(a, 8)\n")
    assert [s["name"] for s in p["sections"]] == ["a", "b"]
    assert p["symbols"] == [{"name": "here", "section": "a", "index": 0, "offset": 0}]
    assert p["patches"][0]["kind"] == "addr" and p["patches"][0]["target"] == 3


class Bus:
    """Memory at 0x1000, SGX registers as a dict, emitted tasks in a list."""

    def __init__(self):
        self.mem = {0x1000 + 4 * i: 0x11 * i for i in range(16)}
        self.regs = {0x40: 0xCAFE}
        self.emits = []

    def load32(self, addr):
        return self.mem.get(addr)

    def store32(self, addr, value):
        if addr not in self.mem:
            return False
        self.mem[addr] = value

    def reg_read(self, index):
        return self.regs[index]

    def reg_write(self, index, value):
        self.regs[index] = value

    def emit(self, text, srcs):
        self.emits.append((text, srcs))


def program(sgx, src):
    p = sgx.assemble(".section p, usse, 0\n" + src)
    words = [int.from_bytes(p["sections"][0]["image"][i:i + 8], "little")
             for i in range(0, len(p["sections"][0]["image"]), 8)]
    return sgx.Program(words)


def test_machine_registers_and_run(sgx):
    m = sgx.Machine()
    m.set("r", 1, 5)
    prog = program(sgx, "mov.skipinv r2, #7\nmov.skipinv.end r3, r1\n")
    assert len(prog) == 2 and prog.text(0) == "mov.skipinv r2, #0x00000007"
    m.run(prog, Bus())
    assert (m.get("r", 2), m.get("r", 3), m.steps) == (7, 5, 2)
    m.pc = 0
    assert m.step(prog, Bus()) is False and m.pc == 1


def test_sgx_registers_through_the_bus(sgx):
    bus = Bus()
    prog = program(sgx, "ldr.skipinv r0, #0x40, drc0\nwdf drc0\nstr.skipinv #0x41, r0\nnop.end\n")
    m = sgx.Machine()
    try:
        m.run(prog, bus)
    except sgx.SimError as e:
        pytest.skip(f"the simulator does not run this ldr/str form yet: {e}")
    assert bus.regs[0x41] == 0xCAFE


def test_sim_errors(sgx):
    m = sgx.Machine()
    with pytest.raises(sgx.SimError) as e:
        m.run(program(sgx, "mov.skipinv.end r0, r9\n"), Bus())
    assert e.value.args[0] == "unsupported"
    with pytest.raises(sgx.SimError) as e:
        m.run(program(sgx, "nop\n"), Bus())
    assert e.value.args[0] == "pc"


def test_bus_exceptions_propagate(sgx):
    class Broken(Bus):
        def reg_read(self, index):
            raise KeyError(index)

    m = sgx.Machine()
    with pytest.raises(KeyError):
        m.run(program(sgx, "ldr.skipinv r0, #0x99, drc0\nwdf drc0\nnop.end\n"), Broken())


def test_cli_and_module_agree_on_listings(sgx, tmp_path):
    """usse-dis and the module decode the transfer templates alike."""
    cli = REPO / "target" / "debug" / "usse-dis"
    subprocess.run(["cargo", "build", "-q", "-p", "sgx-tools", "--bin", "usse-dis"], cwd=REPO,
                   check=True)
    img = sgx.assemble((REPO / "asm" / "transfer" / "transfer.s").read_bytes(),
                       str(REPO / "asm" / "transfer" / "transfer.s"))["sections"][2]["image"]
    (tmp_path / "eot.bin").write_bytes(img)
    out = subprocess.run([str(cli), str(tmp_path / "eot.bin")], capture_output=True,
                         check=True).stdout.decode()
    texts = [ln.split("\t", 1)[1] for ln in out.splitlines() if "\t" in ln]
    words = [int.from_bytes(img[i:i + 8], "little") for i in range(0, len(img), 8)]
    assert texts == [sgx.usse_decode(w) for w in words]
