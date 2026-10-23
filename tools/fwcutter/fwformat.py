#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""The sgx545_ukernel.fw container, formats v1-v4 (design/sgx545_fw_format.md).

A 32-byte header, a table of 16-byte section entries, then 8-byte-aligned payloads.
Each section is tagged CUT (bytes from the user's libsrv_init.so / libsrv_um.so), FACT
(our own reconstructed tables, facts.py) or, for a transfer-queue template, OURS (our own
programs, asm/transfer/), so the kinds of content never mix. Format v2 (§9) adds
the transfer-queue templates; v3 (§10) raises their limits for the copy templates; v4 (§11)
allows OURS templates. The writer picks the lowest version that holds the content.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

MAGIC, VERSION, VERSION_TQ, VERSION_COPY, VERSION_OURS = b"SGX545FW", 1, 2, 3, 4
VERSIONS = (VERSION, VERSION_TQ, VERSION_COPY, VERSION_OURS)
HEADER = struct.Struct("<8sHHBBHIIII")
SECTION = struct.Struct("<HHIII")
BUFFER = struct.Struct("<BBBBIII")
EXPR = struct.Struct("<BBBBIII")
TQ_META = struct.Struct("<BBBBIIHBB")

CUT, FACT, OURS = 0, 1, 2
S_IMAGE, S_BUFFERS, S_PATCHES, S_VALUES, S_EDM_META = 1, 2, 3, 4, 5
S_TQ_TEMPLATE, S_TQ_PATCHES, S_TQ_META = 6, 7, 8
ORIGIN = {S_IMAGE: CUT, S_BUFFERS: FACT, S_PATCHES: FACT, S_VALUES: FACT, S_EDM_META: FACT,
          S_TQ_TEMPLATE: CUT, S_TQ_PATCHES: FACT, S_TQ_META: FACT}
SECTION_NAMES = {S_IMAGE: "IMAGE", S_BUFFERS: "BUFFERS", S_PATCHES: "PATCHES",
                 S_VALUES: "VALUES", S_EDM_META: "EDM_META", S_TQ_TEMPLATE: "TQ_TEMPLATE",
                 S_TQ_PATCHES: "TQ_PATCHES", S_TQ_META: "TQ_META"}

ENC_WORD32, ENC_USSE_LIMM, ENC_CODE_BASE, ENC_EMITPIX_SB = 0, 1, 2, 3
K_CONST, K_ADDR, K_ADDR_SHR4, K_HEAP, K_PD_PHYS, K_CODE_WORD, K_PDS_CTL, K_PDS_ROWS = range(8)
K_PARAM, K_DOUTU = 8, 9
# What each kind of record may use (format §4, §9.4).
V1_KINDS, V1_ENCODINGS = frozenset(range(8)), frozenset({ENC_WORD32, ENC_USSE_LIMM,
                                                         ENC_CODE_BASE})
TQ_KINDS = frozenset({K_CONST, K_ADDR, K_HEAP, K_PARAM, K_DOUTU})
TQ_ENCODINGS = frozenset({ENC_WORD32, ENC_USSE_LIMM, ENC_EMITPIX_SB})

V_PART1, V_PART2, V_DEINIT, V_DEV_DATA, V_TQ_CMD = 0, 1, 2, 3, 4
NO_BUFFER = 0xFF
# EDM_META: 11 kick addresses, 13 MKIF struct sizes, 2 EDM task regs, 4 clock-gating
# words, the cache-control word.
META_WORDS = 11 + 13 + 2 + 4 + 1
META_TRANSFERCMD_SIZE = 11 + 2       # the SGXMKIF_TRANSFERCMD size, bounds TQ_CMD offsets

# Transfer-queue templates (format §9, §10): (number of template ids, number of per-transfer
# parameters) by format version; TQ_MAX and TQ_NUM_PARAMS are the latest version's.
TQ_LIMITS = {VERSION_TQ: (8, 6), VERSION_COPY: (16, 9), VERSION_OURS: (16, 9)}
TQ_MAX, TQ_NUM_PARAMS = TQ_LIMITS[VERSION_COPY]
TQ_NO_STRADDLE = 1
MAX_HEAPS = 16


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


@dataclass(frozen=True)
class TQMeta:
    id: int
    heap: int
    temps: int
    pds_attrs: int
    size: int
    align: int
    pds_data: int
    flags: int


@dataclass
class Firmware:
    ddk: tuple[int, int, int]
    build_options: int
    images: dict[int, bytes]
    buffers: list[Buffer]
    patches: list[Expr]
    values: dict[int, list[Expr]]
    meta: list[int] = field(default_factory=lambda: [0] * META_WORDS)
    tq_images: dict[int, bytes] = field(default_factory=dict)
    tq_meta: list[TQMeta] = field(default_factory=list)
    tq_patches: list[Expr] = field(default_factory=list)
    tq_ours: set[int] = field(default_factory=set)   # templates of origin OURS (§11)

    @property
    def has_tq(self) -> bool:
        return bool(self.tq_images or self.tq_meta or self.tq_patches or
                    V_TQ_CMD in self.values)

    @property
    def version(self) -> int:
        """The lowest format version that holds this content (format §10.1)."""
        if not self.has_tq:
            return VERSION
        if self.tq_ours:
            return VERSION_OURS
        ids = {m.id for m in self.tq_meta} | set(self.tq_images) | \
            {e.buffer for e in self.tq_patches}
        params = {e.target for e in self.tq_patches + self.values.get(V_TQ_CMD, [])
                  if e.kind == K_PARAM}
        nids, nparams = TQ_LIMITS[VERSION_TQ]
        if max(ids, default=0) < nids and max(params, default=0) < nparams:
            return VERSION_TQ
        return VERSION_COPY


def crc(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _pad8(b: bytes) -> bytes:
    return b + bytes(-len(b) % 8)


def pack(fw: Firmware) -> bytes:
    if len(fw.meta) != META_WORDS:
        raise FormatError(f"EDM_META needs {META_WORDS} words")
    if not fw.tq_ours <= set(fw.tq_images):
        raise FormatError("an OURS template needs an image")
    sections: list[tuple[int, int, bytes]] = []
    sections += [(S_IMAGE, i, fw.images[i]) for i in sorted(fw.images)]
    sections.append((S_BUFFERS, 0, b"".join(
        BUFFER.pack(b.id, b.heap, b.pds_rows, 0, b.size, b.align, 0) for b in fw.buffers)))
    sections.append((S_PATCHES, 0, b"".join(EXPR.pack(*vars(e).values()) for e in fw.patches)))
    sections += [(S_VALUES, lid, b"".join(EXPR.pack(*vars(e).values()) for e in fw.values[lid]))
                 for lid in sorted(fw.values)]
    sections.append((S_EDM_META, 0, struct.pack(f"<{META_WORDS}I", *fw.meta)))
    if fw.tq_meta:
        sections.append((S_TQ_META, 0, b"".join(TQ_META.pack(*vars(m).values(), 0)
                                                for m in fw.tq_meta)))
    sections += [(S_TQ_TEMPLATE, i, fw.tq_images[i]) for i in sorted(fw.tq_images)]
    if fw.tq_patches:
        sections.append((S_TQ_PATCHES, 0, b"".join(EXPR.pack(*vars(e).values())
                                                   for e in fw.tq_patches)))

    pos = HEADER.size + SECTION.size * len(sections)
    pos += -pos % 8
    table, body = b"", b""
    for typ, arg, payload in sections:
        origin = OURS if typ == S_TQ_TEMPLATE and arg in fw.tq_ours else ORIGIN[typ]
        table += SECTION.pack(typ, origin, pos + len(body), len(payload), arg)
        body += _pad8(payload)
    rest = _pad8(table) + body
    major, minor, build = fw.ddk
    size = HEADER.size + len(rest)
    return HEADER.pack(MAGIC, fw.version, len(sections), major, minor, 0, build,
                       fw.build_options, size, crc(rest)) + rest


def unpack(raw: bytes) -> Firmware:
    if len(raw) < HEADER.size:
        raise FormatError("truncated header")
    magic, ver, nsec, major, minor, _, build, opts, size, checksum = HEADER.unpack_from(raw)
    if magic != MAGIC:
        raise FormatError("not an sgx545_ukernel.fw file")
    if ver not in VERSIONS:
        raise FormatError(f"format version {ver} not supported (this loader reads "
                          f"{', '.join(map(str, VERSIONS))})")
    if size != len(raw):
        raise FormatError(f"file size {len(raw)} does not match header ({size})")
    if crc(raw[HEADER.size:]) != checksum:
        raise FormatError("CRC mismatch")

    fw = Firmware((major, minor, build), opts, {}, [], [], {})
    for n in range(nsec):
        typ, origin, off, length, arg = SECTION.unpack_from(raw, HEADER.size + SECTION.size * n)
        ours = typ == S_TQ_TEMPLATE and origin == OURS
        if typ not in ORIGIN or (origin != ORIGIN[typ] and not ours):
            raise FormatError(f"section {n}: unknown type {typ} / origin {origin}")
        if ours and ver < VERSION_OURS:
            raise FormatError(f"section {n}: an OURS template needs format v{VERSION_OURS}")
        if ver == VERSION and (typ >= S_TQ_TEMPLATE or (typ == S_VALUES and arg == V_TQ_CMD)):
            raise FormatError(f"section {n}: transfer-queue content in a v1 file")
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
        elif typ == S_TQ_TEMPLATE:
            fw.tq_images[arg] = payload
            fw.tq_ours.discard(arg)
            if ours:
                fw.tq_ours.add(arg)
        elif typ == S_TQ_PATCHES:
            fw.tq_patches = [Expr(*e) for e in EXPR.iter_unpack(payload)]
        elif typ == S_TQ_META:
            fw.tq_meta = [TQMeta(*m[:8]) for m in TQ_META.iter_unpack(payload)]
    check_kinds(fw)
    check_tq(fw, ver)
    return fw


def check_kinds(fw: Firmware) -> None:
    """v1 records use v1 kinds and encodings only (format §9.4)."""
    for lid, es in [(None, fw.patches)] + [(k, v) for k, v in fw.values.items()]:
        if lid == V_TQ_CMD:
            continue
        for e in es:
            if e.kind in TQ_KINDS - V1_KINDS or e.encoding == ENC_EMITPIX_SB:
                raise FormatError(f"transfer-queue kind or encoding in a v1 record: {e}")


def _check_tq_expr(fw: Firmware, e: Expr, meta: dict[int, TQMeta], nparams: int) -> None:
    if e.kind not in TQ_KINDS or e.param:
        raise FormatError(f"bad transfer-queue record: {e}")
    if e.kind == K_PARAM and e.target >= nparams:
        raise FormatError(f"parameter {e.target} out of range")
    if e.kind in (K_ADDR, K_DOUTU) and e.target not in meta:
        raise FormatError(f"record names template {e.target}, which has no TQ_META")
    if e.kind == K_HEAP and e.target >= MAX_HEAPS:
        raise FormatError(f"heap {e.target} out of range")


def check_tq(fw: Firmware, version: int = VERSION_COPY) -> None:
    """The structural rules of format §9 and §10: every template, patch and command word
    valid, within the limits of the file's version (a v1 file has no such content)."""
    nids, nparams = TQ_LIMITS.get(version, TQ_LIMITS[VERSION_TQ])
    meta: dict[int, TQMeta] = {}
    for m in fw.tq_meta:
        if m.id >= nids or m.id in meta:
            raise FormatError(f"bad or duplicate TQ_META id {m.id}")
        if not m.align or m.heap >= MAX_HEAPS or m.pds_data > m.size:
            raise FormatError(f"bad TQ_META record {m}")
        meta[m.id] = m
    for i, img in fw.tq_images.items():
        if i not in meta or len(img) > meta[i].size:
            raise FormatError(f"template {i} has no TQ_META record or is too large")
    for e in fw.tq_patches:
        if e.encoding not in TQ_ENCODINGS or e.buffer not in meta:
            raise FormatError(f"bad transfer-queue patch: {e}")
        if e.offset + (4 if e.encoding == ENC_WORD32 else 8) > meta[e.buffer].size:
            raise FormatError(f"patch past the end of template {e.buffer}: {e}")
        _check_tq_expr(fw, e, meta, nparams)
    for e in fw.values.get(V_TQ_CMD, []):
        if e.encoding != ENC_WORD32 or e.offset % 4 or \
                e.offset + 4 > fw.meta[META_TRANSFERCMD_SIZE]:
            raise FormatError(f"bad command word: {e}")
        _check_tq_expr(fw, e, meta, nparams)
