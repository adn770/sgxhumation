# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
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


# Damaged files every loader must reject (test_cloader.py runs them through the C port too).
CORRUPTIONS = [
    lambda b: b"XGX545FW" + b[8:],                            # magic
    lambda b: b[:8] + struct.pack("<H", 5) + b[10:],          # format version
    lambda b: b[:-1] + bytes([b[-1] ^ 1]),                    # crc
    lambda b: b[:-8],                                         # truncated
]


def unknown_section_type(b: bytes) -> bytes:
    raw = bytearray(b)
    struct.pack_into("<H", raw, 32, 99)                       # first table entry's type
    struct.pack_into("<I", raw, 28, F.crc(bytes(raw[32:])))
    return bytes(raw)


@pytest.mark.parametrize("corrupt", CORRUPTIONS)
def test_rejects_damaged_files(corrupt):
    with pytest.raises(F.FormatError):
        F.unpack(corrupt(F.pack(sample())))


def test_rejects_unknown_section_type():
    with pytest.raises(F.FormatError):
        F.unpack(unknown_section_type(F.pack(sample())))


# --- format v2: transfer-queue sections (format §9) --------------------------------------

def sample_v2() -> F.Firmware:
    fw = sample()
    fw.meta = [0] * F.META_WORDS
    fw.meta[F.META_TRANSFERCMD_SIZE] = 0x4C8
    fw.tq_images = {0: b"\x11" * 16, 6: b"\x22" * 12}
    fw.tq_meta = [F.TQMeta(0, 4, 4, 0, 0x10, 8, 0, F.TQ_NO_STRADDLE),
                  F.TQMeta(6, 6, 0, 1, 0x20, 16, 0x10, F.TQ_NO_STRADDLE)]
    fw.tq_patches = [F.Expr(0, F.ENC_USSE_LIMM, F.K_PARAM, 0, 8, 0, 0),
                     F.Expr(6, F.ENC_WORD32, F.K_DOUTU, 0, 0, 0, 0),
                     F.Expr(0, F.ENC_EMITPIX_SB, F.K_PARAM, 0, 0, 5, 0)]
    fw.values[F.V_TQ_CMD] = [F.Expr(0xFF, F.ENC_WORD32, F.K_ADDR, 0, 0x24, 6, 0)]
    return fw


def test_v2_round_trip():
    fw = sample_v2()
    assert F.unpack(F.pack(fw)) == fw


def test_version_follows_content():
    assert struct.unpack_from("<H", F.pack(sample()), 8)[0] == 1
    assert struct.unpack_from("<H", F.pack(sample_v2()), 8)[0] == 2


def test_tq_sections_are_tagged_with_their_origin():
    raw = F.pack(sample_v2())
    nsec = struct.unpack_from("<H", raw, 10)[0]
    table = [struct.unpack_from("<HHIII", raw, 32 + 16 * i) for i in range(nsec)]
    origins = {t: o for t, o, *_ in table}
    assert origins[F.S_TQ_TEMPLATE] == F.CUT
    assert origins[F.S_TQ_PATCHES] == F.FACT and origins[F.S_TQ_META] == F.FACT
    assert sorted(a for t, _, _, _, a in table if t == F.S_TQ_TEMPLATE) == [0, 6]


def _recrc(raw: bytearray) -> bytes:
    struct.pack_into("<I", raw, 28, F.crc(bytes(raw[32:])))
    return bytes(raw)


def as_version(b: bytes, version: int) -> bytes:
    raw = bytearray(b)
    struct.pack_into("<H", raw, 8, version)
    return _recrc(raw)


def as_version_1(b: bytes) -> bytes:
    return as_version(b, 1)


def _v2(**changes) -> bytes:
    fw = sample_v2()
    for k, v in changes.items():
        setattr(fw, k, v)
    return F.pack(fw)


def _tq_patch(i: int, **changes) -> bytes:
    import dataclasses
    patches = sample_v2().tq_patches
    patches[i] = dataclasses.replace(patches[i], **changes)
    return _v2(tq_patches=patches)


def _tq_cmd(**changes) -> bytes:
    import dataclasses
    fw = sample_v2()
    fw.values[F.V_TQ_CMD] = [dataclasses.replace(fw.values[F.V_TQ_CMD][0], **changes)]
    return F.pack(fw)


# Malformed v2 files every loader must reject (test_cloader.py runs them through C too).
TQ_DAMAGED = {
    "tq sections in a v1 file": lambda: as_version_1(F.pack(sample_v2())),
    "tq cmd list in a v1 file": lambda: as_version_1(F.pack(
        F.Firmware(**{**vars(sample()), "values": {4: sample_v2().values[4]}}))),
    "template without meta": lambda: _v2(tq_images={0: bytes(16), 3: bytes(8)}),
    "template larger than meta": lambda: _v2(tq_images={0: bytes(24)}),
    "duplicate meta": lambda: _v2(tq_meta=sample_v2().tq_meta * 2),
    "meta id out of range": lambda: as_version(_v2(tq_meta=[F.TQMeta(8, 4, 0, 0, 0x10, 8, 0,
                                                                   0)],
                                                   tq_images={}, tq_patches=[], values={}), 2),
    "meta zero align": lambda: _v2(tq_meta=[F.TQMeta(0, 4, 0, 0, 0x10, 0, 0, 0),
                                            sample_v2().tq_meta[1]]),
    "meta heap out of range": lambda: _v2(tq_meta=[F.TQMeta(0, 16, 0, 0, 0x10, 8, 0, 0),
                                                   sample_v2().tq_meta[1]]),
    "pds data past size": lambda: _v2(tq_meta=[sample_v2().tq_meta[0],
                                               F.TQMeta(6, 6, 0, 1, 0x20, 16, 0x30, 1)]),
    "patch on unknown template": lambda: _tq_patch(0, buffer=3),
    "patch past template": lambda: _tq_patch(0, offset=12),
    "word past template": lambda: _tq_patch(1, offset=0x1E),
    "v1 kind in a tq patch": lambda: _tq_patch(1, kind=F.K_CODE_WORD),
    "v1 encoding in a tq patch": lambda: _tq_patch(1, encoding=F.ENC_CODE_BASE),
    "unknown encoding": lambda: _tq_patch(1, encoding=4),
    "nonzero param": lambda: _tq_patch(1, param=1),
    "param out of range": lambda: _tq_patch(0, target=F.TQ_NUM_PARAMS),
    "doutu of unknown template": lambda: _tq_patch(1, target=2),
    "heap out of range": lambda: _tq_patch(1, kind=F.K_HEAP, target=16),
    "tq kind in a v1 patch": lambda: _v2(patches=[F.Expr(0, F.ENC_WORD32, F.K_PARAM, 0, 0, 0,
                                                         0)]),
    "tq encoding in a v1 list": lambda: _v2(values={**sample_v2().values, 3: [
        F.Expr(0xFF, F.ENC_EMITPIX_SB, F.K_CONST, 0, 8, 0, 2)]}),
    "cmd offset unaligned": lambda: _tq_cmd(offset=0x26),
    "cmd offset past command": lambda: _tq_cmd(offset=0x4C8),
    "cmd not word32": lambda: _tq_cmd(encoding=F.ENC_USSE_LIMM),
    "cmd address of unknown template": lambda: _tq_cmd(target=1),
}


@pytest.mark.parametrize("name", TQ_DAMAGED)
def test_rejects_damaged_v2(name):
    with pytest.raises(F.FormatError):
        F.unpack(TQ_DAMAGED[name]())


def test_v2_without_tq_sections_is_valid():
    raw = bytearray(F.pack(sample()))
    struct.pack_into("<H", raw, 8, 2)
    assert F.unpack(_recrc(raw)) == sample()


# --- format v3: the copy templates (format §10) ------------------------------------------

def sample_v3() -> F.Firmware:
    """sample_v2() plus a template with id 8, patched with the v3 parameters 6-8."""
    fw = sample_v2()
    fw.tq_images[8] = b"\x33" * 48
    fw.tq_meta.append(F.TQMeta(8, 6, 0, 1, 0x40, 16, 0x30, F.TQ_NO_STRADDLE))
    fw.tq_patches += [F.Expr(8, F.ENC_WORD32, F.K_DOUTU, 0, 0, 0, 0),
                      F.Expr(8, F.ENC_WORD32, F.K_PARAM, 0, 24, 8, 0),
                      F.Expr(8, F.ENC_WORD32, F.K_PARAM, 0, 32, 6, 0)]
    return fw


def test_v3_limits():
    assert (F.TQ_MAX, F.TQ_NUM_PARAMS) == (16, 9)
    assert {v: F.TQ_LIMITS[v] for v in (2, 3)} == {2: (8, 6), 3: (16, 9)}


def test_v3_round_trip():
    fw = sample_v3()
    assert F.unpack(F.pack(fw)) == fw


def _version(fw: F.Firmware) -> int:
    return struct.unpack_from("<H", F.pack(fw), 8)[0]


def test_version_is_the_lowest_that_holds_the_content():
    import dataclasses
    assert [_version(f()) for f in (sample, sample_v2, sample_v3)] == [1, 2, 3]
    # template 7 fits v2 (ids 0-7); parameter 6 does not (0-5), in a patch or a command
    fw = sample_v2()
    fw.tq_meta.append(F.TQMeta(7, 4, 0, 0, 0x10, 8, 0, 0))
    assert _version(fw) == 2
    fw.tq_patches.append(F.Expr(7, F.ENC_WORD32, F.K_PARAM, 0, 0, 6, 0))
    assert _version(fw) == 3
    fw = sample_v2()
    fw.values[F.V_TQ_CMD] = [dataclasses.replace(fw.values[F.V_TQ_CMD][0], kind=F.K_PARAM,
                                                 target=8)]
    assert _version(fw) == 3
    fw = sample_v2()
    fw.tq_meta.append(F.TQMeta(15, 4, 0, 0, 0x10, 8, 0, 0))
    assert _version(fw) == 3


def test_v3_without_tq_sections_is_valid():
    assert F.unpack(as_version(F.pack(sample()), 3)) == sample()


def test_v3_reads_v2_content():
    fw = F.unpack(as_version(F.pack(sample_v2()), 3))
    assert fw == sample_v2()


def _v3_param(target: int) -> F.Firmware:
    fw = sample_v2()
    fw.tq_patches = [*fw.tq_patches, F.Expr(6, F.ENC_WORD32, F.K_PARAM, 0, 4, target, 0)]
    return fw


def _v3_cmd_param(target: int) -> F.Firmware:
    fw = sample_v2()
    fw.values[F.V_TQ_CMD] = [*fw.values[F.V_TQ_CMD],
                             F.Expr(0xFF, F.ENC_WORD32, F.K_PARAM, 0, 0x28, target, 0)]
    return fw


# Files a v3 loader must reject: v3 content under an older version, or past the v3 limits.
TQ_V3_DAMAGED = {
    "template 8 in a v2 file": lambda: as_version(F.pack(sample_v3()), 2),
    "parameter 6 in a v2 patch": lambda: as_version(F.pack(_v3_param(6)), 2),
    "parameter 8 in a v2 command word": lambda: as_version(F.pack(_v3_cmd_param(8)), 2),
    "v3 content in a v1 file": lambda: as_version_1(F.pack(sample_v3())),
    "meta id out of range (v3)": lambda: _v2(tq_meta=[*sample_v2().tq_meta,
                                                      F.TQMeta(16, 4, 0, 0, 0x10, 8, 0, 0)]),
    "param out of range (v3)": lambda: F.pack(_v3_param(9)),
    "cmd param out of range (v3)": lambda: F.pack(_v3_cmd_param(9)),
}


@pytest.mark.parametrize("name", TQ_V3_DAMAGED)
def test_rejects_damaged_v3(name):
    with pytest.raises(F.FormatError):
        F.unpack(TQ_V3_DAMAGED[name]())


# --- format v4: templates of our own (format §11) ----------------------------------------

def sample_v4() -> F.Firmware:
    """sample_v3() whose templates 0 and 8 are our own code (origin OURS)."""
    fw = sample_v3()
    fw.tq_ours = {0, 8}
    return fw


def _origins(raw: bytes) -> dict[int, int]:
    """Template id -> origin of its TQ_TEMPLATE section."""
    nsec = struct.unpack_from("<H", raw, 10)[0]
    table = [struct.unpack_from("<HHIII", raw, 32 + 16 * i) for i in range(nsec)]
    return {a: o for t, o, _, _, a in table if t == F.S_TQ_TEMPLATE}


def test_v4_limits_are_v3_limits():
    assert (F.OURS, F.VERSION_OURS) == (2, 4)
    assert F.TQ_LIMITS[4] == F.TQ_LIMITS[3]
    assert F.VERSIONS == (1, 2, 3, 4)


def test_v4_round_trip():
    fw = sample_v4()
    assert F.unpack(F.pack(fw)) == fw


def test_our_templates_are_tagged_ours():
    raw = F.pack(sample_v4())
    assert _origins(raw) == {0: F.OURS, 6: F.CUT, 8: F.OURS}
    assert struct.unpack_from("<H", raw, 8)[0] == 4


def test_version_4_only_with_our_templates():
    # a template of our own needs v4 whatever its id; without one, the v1-v3 rule holds
    assert [_version(f()) for f in (sample_v2, sample_v3, sample_v4)] == [2, 3, 4]
    fw = sample_v2()
    fw.tq_ours = {6}
    assert _version(fw) == 4


def test_v4_without_our_templates_is_valid():
    fw = F.unpack(as_version(F.pack(sample_v3()), 4))
    assert fw == sample_v3() and not fw.tq_ours


def test_v4_reads_v1_content():
    assert F.unpack(as_version(F.pack(sample()), 4)) == sample()


def test_ours_needs_an_image():
    fw = sample_v3()
    fw.tq_ours = {2}                              # template 2 has no image here
    with pytest.raises(F.FormatError):
        F.pack(fw)


def _origin_of(raw: bytes, typ: int, origin: int, arg: int | None = None) -> bytes:
    """raw with the first section of type typ (and argument arg) retagged origin."""
    buf = bytearray(raw)
    nsec = struct.unpack_from("<H", buf, 10)[0]
    for i in range(nsec):
        t, _, _, _, a = struct.unpack_from("<HHIII", buf, 32 + 16 * i)
        if t == typ and arg in (None, a):
            struct.pack_into("<H", buf, 32 + 16 * i + 2, origin)
            return _recrc(buf)
    raise AssertionError("no such section")


# Files a v4 loader must reject: OURS in an older file, on a section that is not a
# template, or an origin no version defines.
TQ_V4_DAMAGED = {
    "ours in a v3 file": lambda: as_version(F.pack(sample_v4()), 3),
    "ours in a v2 file": lambda: as_version(_origin_of(F.pack(sample_v2()), F.S_TQ_TEMPLATE,
                                                       F.OURS), 2),
    "ours image": lambda: _origin_of(F.pack(sample_v4()), F.S_IMAGE, F.OURS),
    "ours patches": lambda: _origin_of(F.pack(sample_v4()), F.S_TQ_PATCHES, F.OURS),
    "ours meta": lambda: _origin_of(F.pack(sample_v4()), F.S_TQ_META, F.OURS),
    "fact template": lambda: _origin_of(F.pack(sample_v4()), F.S_TQ_TEMPLATE, F.FACT),
    "origin 3": lambda: _origin_of(F.pack(sample_v4()), F.S_TQ_TEMPLATE, 3),
}


@pytest.mark.parametrize("name", TQ_V4_DAMAGED)
def test_rejects_damaged_v4(name):
    with pytest.raises(F.FormatError):
        F.unpack(TQ_V4_DAMAGED[name]())
