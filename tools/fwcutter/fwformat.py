#!/usr/bin/env python3
"""The sgx545_ukernel.fw container, format v1 (design/sgx545_fw_format.md).

A 32-byte header, a table of 16-byte section entries, then 8-byte-aligned payloads.
Each section is tagged CUT (bytes from the user's libsrv_init.so) or FACT (our own
reconstructed tables, facts.py), so the two kinds of content never mix.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

MAGIC, VERSION = b"SGX545FW", 1
HEADER = struct.Struct("<8sHHBBHIIII")
SECTION = struct.Struct("<HHIII")
BUFFER = struct.Struct("<BBBBIII")
EXPR = struct.Struct("<BBBBIII")

CUT, FACT = 0, 1
S_IMAGE, S_BUFFERS, S_PATCHES, S_VALUES, S_EDM_META = 1, 2, 3, 4, 5
ORIGIN = {S_IMAGE: CUT, S_BUFFERS: FACT, S_PATCHES: FACT, S_VALUES: FACT, S_EDM_META: FACT}
SECTION_NAMES = {S_IMAGE: "IMAGE", S_BUFFERS: "BUFFERS", S_PATCHES: "PATCHES",
                 S_VALUES: "VALUES", S_EDM_META: "EDM_META"}

ENC_WORD32, ENC_USSE_LIMM, ENC_CODE_BASE = 0, 1, 2
K_CONST, K_ADDR, K_ADDR_SHR4, K_HEAP, K_PD_PHYS, K_CODE_WORD, K_PDS_CTL, K_PDS_ROWS = range(8)

V_PART1, V_PART2, V_DEINIT, V_DEV_DATA = 0, 1, 2, 3
NO_BUFFER = 0xFF
# EDM_META: 11 kick addresses, 13 MKIF struct sizes, 2 EDM task regs, 4 clock-gating
# words, the cache-control word.
META_WORDS = 11 + 13 + 2 + 4 + 1


class FormatError(ValueError):
    pass


@dataclass(frozen=True)
class Buffer:
    id: int
    heap: int
    pds_rows: int
    size: int
    align: int


@dataclass(frozen=True)
class Expr:
    buffer: int
    encoding: int
    kind: int
    param: int
    offset: int
    target: int
    addend: int


@dataclass
class Firmware:
    ddk: tuple[int, int, int]
    build_options: int
    images: dict[int, bytes]
    buffers: list[Buffer]
    patches: list[Expr]
    values: dict[int, list[Expr]]
    meta: list[int] = field(default_factory=lambda: [0] * META_WORDS)


def crc(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _pad8(b: bytes) -> bytes:
    return b + bytes(-len(b) % 8)


def pack(fw: Firmware) -> bytes:
    if len(fw.meta) != META_WORDS:
        raise FormatError(f"EDM_META needs {META_WORDS} words")
    sections: list[tuple[int, int, bytes]] = []
    sections += [(S_IMAGE, i, fw.images[i]) for i in sorted(fw.images)]
    sections.append((S_BUFFERS, 0, b"".join(
        BUFFER.pack(b.id, b.heap, b.pds_rows, 0, b.size, b.align, 0) for b in fw.buffers)))
    sections.append((S_PATCHES, 0, b"".join(EXPR.pack(*vars(e).values()) for e in fw.patches)))
    sections += [(S_VALUES, lid, b"".join(EXPR.pack(*vars(e).values()) for e in fw.values[lid]))
                 for lid in sorted(fw.values)]
    sections.append((S_EDM_META, 0, struct.pack(f"<{META_WORDS}I", *fw.meta)))

    pos = HEADER.size + SECTION.size * len(sections)
    pos += -pos % 8
    table, body = b"", b""
    for typ, arg, payload in sections:
        table += SECTION.pack(typ, ORIGIN[typ], pos + len(body), len(payload), arg)
        body += _pad8(payload)
    rest = _pad8(table) + body
    major, minor, build = fw.ddk
    size = HEADER.size + len(rest)
    return HEADER.pack(MAGIC, VERSION, len(sections), major, minor, 0, build,
                       fw.build_options, size, crc(rest)) + rest


def unpack(raw: bytes) -> Firmware:
    if len(raw) < HEADER.size:
        raise FormatError("truncated header")
    magic, ver, nsec, major, minor, _, build, opts, size, checksum = HEADER.unpack_from(raw)
    if magic != MAGIC:
        raise FormatError("not an sgx545_ukernel.fw file")
    if ver != VERSION:
        raise FormatError(f"format version {ver} not supported (this loader reads {VERSION})")
    if size != len(raw):
        raise FormatError(f"file size {len(raw)} does not match header ({size})")
    if crc(raw[HEADER.size:]) != checksum:
        raise FormatError("CRC mismatch")

    fw = Firmware((major, minor, build), opts, {}, [], [], {})
    for n in range(nsec):
        typ, origin, off, length, arg = SECTION.unpack_from(raw, HEADER.size + SECTION.size * n)
        if typ not in ORIGIN or origin != ORIGIN[typ]:
            raise FormatError(f"section {n}: unknown type {typ} / origin {origin}")
        if off + length > len(raw):
            raise FormatError(f"section {n} runs past the end of the file")
        payload = raw[off:off + length]
        if typ == S_IMAGE:
            fw.images[arg] = payload
        elif typ == S_BUFFERS:
            fw.buffers = [Buffer(i, h, r, s, a) for i, h, r, _, s, a, _ in
                          BUFFER.iter_unpack(payload)]
        elif typ == S_PATCHES:
            fw.patches = [Expr(*e) for e in EXPR.iter_unpack(payload)]
        elif typ == S_VALUES:
            fw.values[arg] = [Expr(*e) for e in EXPR.iter_unpack(payload)]
        elif typ == S_EDM_META:
            if length != 4 * META_WORDS:
                raise FormatError("EDM_META has the wrong size")
            fw.meta = list(struct.unpack(f"<{META_WORDS}I", payload))
    return fw
