"""Tests for fwformat.py — the sgx545_ukernel.fw container (design/sgx545_fw_format.md §2)."""
import struct

import pytest

import fwformat as F


def sample() -> F.Firmware:
    return F.Firmware(
        ddk=(1, 7, 862890), build_options=0x60218,
        images={0: bytes(range(16)), 3: b"\xaa" * 8},
        buffers=[F.Buffer(0, 2, 0, 0x10, 8), F.Buffer(3, 2, 2, 0x38, 16),
                 F.Buffer(9, 3, 0, 0xec, 32)],
        patches=[F.Expr(0, F.ENC_USSE_LIMM, F.K_ADDR, 0, 8, 9, 0),
                 F.Expr(3, F.ENC_WORD32, F.K_CODE_WORD, 0, 0, 0x1be, 0xF)],
        values={1: [F.Expr(0xFF, F.ENC_CODE_BASE, F.K_HEAP, 3, 0xA48, 2, 0)],
                3: [F.Expr(0xFF, F.ENC_WORD32, F.K_CONST, 0, 8, 0, 2)]},
        meta=list(range(F.META_WORDS)),
    )


def test_round_trip():
    fw = sample()
    assert F.unpack(F.pack(fw)) == fw


def test_header_fields():
    raw = F.pack(sample())
    magic, ver, nsec, major, minor, _, build, opts, size, _crc = struct.unpack_from(
        "<8sHHBBHIIII", raw)
    assert (magic, ver, major, minor, build, opts) == (b"SGX545FW", 1, 1, 7, 862890, 0x60218)
    assert size == len(raw)
    # 2 images + BUFFERS + PATCHES + 2 VALUES lists + EDM_META
    assert nsec == 7


def test_sections_are_tagged_with_their_origin():
    raw = F.pack(sample())
    nsec = struct.unpack_from("<H", raw, 10)[0]
    table = [struct.unpack_from("<HHIII", raw, 32 + 16 * i) for i in range(nsec)]
    origins = {t: o for t, o, *_ in table}
    assert origins[F.S_IMAGE] == F.CUT
    for t in (F.S_BUFFERS, F.S_PATCHES, F.S_VALUES, F.S_EDM_META):
        assert origins[t] == F.FACT
    assert all(off % 8 == 0 for _, _, off, _, _ in table)


@pytest.mark.parametrize("corrupt", [
    lambda b: b"XGX545FW" + b[8:],                            # magic
    lambda b: b[:8] + struct.pack("<H", 2) + b[10:],          # format version
    lambda b: b[:-1] + bytes([b[-1] ^ 1]),                    # crc
    lambda b: b[:-8],                                         # truncated
])
def test_rejects_damaged_files(corrupt):
    with pytest.raises(F.FormatError):
        F.unpack(corrupt(F.pack(sample())))


def test_rejects_unknown_section_type():
    raw = bytearray(F.pack(sample()))
    struct.pack_into("<H", raw, 32, 99)                       # first table entry's type
    struct.pack_into("<I", raw, 28, F.crc(bytes(raw[32:])))
    with pytest.raises(F.FormatError):
        F.unpack(bytes(raw))
