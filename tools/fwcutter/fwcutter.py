#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
"""sgx545-fwcutter: cut the SGX545 firmware out of the closed Cedarview libraries.

A static carve: the closed code is never executed, so this runs on any host. Each input
must be a known library (facts.KNOWN_INPUTS, by SHA-256); each cut section is checked
against its own hash. The output combines the cut code images with our fact tables
(design/sgx545_fw_format.md). With libsrv_init.so alone the file is format v1; with
libsrv_um.so as well it is v3 and also carries the transfer-queue templates of a fill (§9)
and a copy (§10). --format-version 2 leaves the copy out, for a v2 loader. With
--tq-from-source instead of libsrv_um.so, the templates are our own programs
(tools/usse-asm/progs/, assembled), tagged OURS: format v4 (§11).

    usage: fwcutter.py cut <libsrv_init.so> [<libsrv_um.so> | --tq-from-source]
                           [--format-version 2|3] [-o sgx545_ukernel.fw]
           fwcutter.py info <sgx545_ukernel.fw>
"""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import sys

import facts
import fwformat as F
import ours

LIBDIR = (pathlib.Path(__file__).resolve().parent.parent.parent / "reference" / "preserved" /
          "cedarview-gfx-blob" / "extracted" / "cedarview-userspace-v1.0.3_bee" / "usr" / "lib")
DEFAULT_INPUT = LIBDIR / "libsrv_init.so.1.7.862890"
DEFAULT_UM_INPUT = LIBDIR / "libsrv_um.so.1.7.862890"
LIB_NAMES = {"init": "libsrv_init.so", "um": "libsrv_um.so"}


class UnknownInput(ValueError):
    pass


def default_input() -> pathlib.Path | None:
    """The library to cut in tests: $SGX545_LIBSRV_INIT, else the reference corpus copy."""
    path = pathlib.Path(os.environ.get("SGX545_LIBSRV_INIT", DEFAULT_INPUT))
    return path if path.is_file() else None


def default_um_input() -> pathlib.Path | None:
    """The libsrv_um.so to cut in tests: $SGX545_LIBSRV_UM, else the reference corpus copy."""
    path = pathlib.Path(os.environ.get("SGX545_LIBSRV_UM", DEFAULT_UM_INPUT))
    return path if path.is_file() else None


def identify(blob: bytes, role: str = "init") -> facts.KnownInput:
    digest = hashlib.sha256(blob).hexdigest()
    known = [k for k in facts.KNOWN_INPUTS if k.role == role]
    for k in known:
        if k.sha256 == digest:
            return k
    names = "\n".join(f"  {k.name}  {k.sha256}  ({k.package})" for k in known)
    raise UnknownInput(f"not a known {LIB_NAMES[role]} (sha256 {digest}); supported:\n{names}")


def cut_template(k: facts.KnownInput, t: facts.TQCut, blob: bytes) -> bytes:
    """Assemble one template from its pieces and check it (format §9.2)."""
    out = bytearray(t.size)
    for p in t.pieces:
        out[p.dst:p.dst + p.size] = blob[p.src:p.src + p.size]
    if hashlib.sha256(out).hexdigest() != t.sha256:
        raise UnknownInput(f"{k.name}: template {t.template} does not match its hash")
    return bytes(out)


def cut(blob: bytes, um: bytes | None = None, version: int = F.VERSION_COPY,
        source: bool = False) -> F.Firmware:
    """The firmware file's content: v1 from libsrv_init.so alone; with libsrv_um.so, v3, or
    v2 without the copy templates (format §10.1); with source, v4: our own assembled
    templates instead of libsrv_um.so's (format §11)."""
    if source and um is not None:
        raise ValueError("the templates come from libsrv_um.so or from source, not both")
    if version not in (F.VERSION_TQ, F.VERSION_COPY):
        raise ValueError(f"cannot cut format version {version} (2 or 3)")
    k = identify(blob)
    images = {}
    for s in k.sections:
        data = blob[s.offset:s.offset + s.size]
        if hashlib.sha256(data).hexdigest() != s.sha256:
            raise UnknownInput(f"{k.name}: section for buffer {s.buffer} at {s.offset:#x} "
                               "does not match its hash")
        images[s.buffer] = data
    fw = F.Firmware(ddk=k.ddk, build_options=k.build_options, images=images,
                    buffers=list(facts.BUFFERS), patches=list(facts.PATCHES),
                    values={lid: list(v) for lid, v in facts.VALUES.items()},
                    meta=list(facts.META))
    if source:
        fw.tq_images = ours.templates()
        fw.tq_ours = set(fw.tq_images)
        fw.tq_meta = list(facts.TQ_META)
        fw.tq_patches = list(facts.TQ_PATCHES)
        fw.values[F.V_TQ_CMD] = list(facts.TQ_CMD)
        return fw
    if um is None:
        return fw
    u = identify(um, "um")
    if u.ddk != k.ddk:
        raise UnknownInput(f"{u.name} and {k.name} are from different DDK builds")
    keep = (lambda i: i not in facts.TQ_COPY) if version == F.VERSION_TQ else (lambda i: True)
    fw.tq_images = {t.template: cut_template(u, t, um) for t in u.templates if keep(t.template)}
    fw.tq_meta = [m for m in facts.TQ_META if keep(m.id)]
    fw.tq_patches = [e for e in facts.TQ_PATCHES if keep(e.buffer)]
    fw.values[F.V_TQ_CMD] = list(facts.TQ_CMD)
    return fw


ORIGIN_NAMES = {F.CUT: "CUT ", F.FACT: "FACT", F.OURS: "OURS"}


def info(fw: F.Firmware, raw: bytes) -> str:
    major, minor, build = fw.ddk
    version = int.from_bytes(raw[8:10], "little")
    out = [f"sgx545_ukernel.fw v{version}: DDK {major}.{minor}.{build}, "
           f"build options {fw.build_options:#x}, {len(raw)} bytes"]
    nsec = int.from_bytes(raw[10:12], "little")
    for n in range(nsec):
        typ, origin, off, size, arg = F.SECTION.unpack_from(raw, F.HEADER.size + F.SECTION.size * n)
        what = (facts.BUFFER_NAMES[arg] if typ == F.S_IMAGE else
                facts.TQ_NAMES[arg] if typ == F.S_TQ_TEMPLATE and arg < len(facts.TQ_NAMES)
                else str(arg))
        out.append(f"  {F.SECTION_NAMES[typ]:11s} {ORIGIN_NAMES[origin]} "
                   f"@{off:#07x} {size:#07x}  {what}")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="fwcutter.py", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("cut")
    c.add_argument("input", help="libsrv_init.so")
    c.add_argument("um", nargs="?", help="libsrv_um.so: add the transfer-queue templates (v3)")
    c.add_argument("--format-version", type=int, choices=(2, 3), default=None,
                   help="with libsrv_um.so: 2 leaves out the copy templates (default 3)")
    c.add_argument("--tq-from-source", action="store_true",
                   help="the templates from our own source (tools/usse-asm/progs/) instead "
                        "of libsrv_um.so (v4)")
    c.add_argument("-o", "--output", default="sgx545_ukernel.fw")
    i = sub.add_parser("info")
    i.add_argument("fw")
    args = ap.parse_args(argv)
    if args.cmd == "cut" and args.tq_from_source and (args.um or args.format_version):
        ap.error("--tq-from-source takes neither libsrv_um.so nor --format-version")
    try:
        if args.cmd == "cut":
            um = pathlib.Path(args.um).read_bytes() if args.um else None
            raw = F.pack(cut(pathlib.Path(args.input).read_bytes(), um,
                             args.format_version or F.VERSION_COPY, args.tq_from_source))
            pathlib.Path(args.output).write_bytes(raw)
            print(f"wrote {args.output} ({len(raw)} bytes)")
        else:
            raw = pathlib.Path(args.fw).read_bytes()
            print(info(F.unpack(raw), raw))
    except (UnknownInput, F.FormatError, ours.SourceError) as e:
        print(f"fwcutter: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
