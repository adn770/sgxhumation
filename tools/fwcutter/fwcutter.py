#!/usr/bin/env python3
"""sgx545-fwcutter: cut the SGX545 firmware out of the closed Cedarview libsrv_init.so.

A static carve: the closed code is never executed, so this runs on any host. The input
must be a known library (facts.KNOWN_INPUTS, by SHA-256); each cut section is checked
against its own hash. The output combines the cut code images with our fact tables
(design/sgx545_fw_format.md).

    usage: fwcutter.py cut <libsrv_init.so> [-o sgx545_ukernel.fw]
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

DEFAULT_INPUT = (pathlib.Path(__file__).resolve().parent.parent.parent / "reference" /
                 "preserved" / "cedarview-gfx-blob" / "extracted" /
                 "cedarview-userspace-v1.0.3_bee" / "usr" / "lib" / "libsrv_init.so.1.7.862890")


class UnknownInput(ValueError):
    pass


def default_input() -> pathlib.Path | None:
    """The library to cut in tests: $SGX545_LIBSRV_INIT, else the reference corpus copy."""
    path = pathlib.Path(os.environ.get("SGX545_LIBSRV_INIT", DEFAULT_INPUT))
    return path if path.is_file() else None


def identify(blob: bytes) -> facts.KnownInput:
    digest = hashlib.sha256(blob).hexdigest()
    for k in facts.KNOWN_INPUTS:
        if k.sha256 == digest:
            return k
    known = "\n".join(f"  {k.name}  {k.sha256}  ({k.package})" for k in facts.KNOWN_INPUTS)
    raise UnknownInput(f"not a known libsrv_init.so (sha256 {digest}); supported:\n{known}")


def cut(blob: bytes) -> F.Firmware:
    k = identify(blob)
    images = {}
    for s in k.sections:
        data = blob[s.offset:s.offset + s.size]
        if hashlib.sha256(data).hexdigest() != s.sha256:
            raise UnknownInput(f"{k.name}: section for buffer {s.buffer} at {s.offset:#x} "
                               "does not match its hash")
        images[s.buffer] = data
    return F.Firmware(ddk=k.ddk, build_options=k.build_options, images=images,
                      buffers=list(facts.BUFFERS), patches=list(facts.PATCHES),
                      values={lid: list(v) for lid, v in facts.VALUES.items()},
                      meta=list(facts.META))


def info(fw: F.Firmware, raw: bytes) -> str:
    major, minor, build = fw.ddk
    out = [f"sgx545_ukernel.fw v{F.VERSION}: DDK {major}.{minor}.{build}, "
           f"build options {fw.build_options:#x}, {len(raw)} bytes"]
    nsec = int.from_bytes(raw[10:12], "little")
    for n in range(nsec):
        typ, origin, off, size, arg = F.SECTION.unpack_from(raw, F.HEADER.size + F.SECTION.size * n)
        what = facts.BUFFER_NAMES[arg] if typ == F.S_IMAGE else str(arg)
        out.append(f"  {F.SECTION_NAMES[typ]:9s} {'CUT ' if origin == F.CUT else 'FACT'} "
                   f"@{off:#07x} {size:#07x}  {what}")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="fwcutter.py", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("cut")
    c.add_argument("input")
    c.add_argument("-o", "--output", default="sgx545_ukernel.fw")
    i = sub.add_parser("info")
    i.add_argument("fw")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "cut":
            raw = F.pack(cut(pathlib.Path(args.input).read_bytes()))
            pathlib.Path(args.output).write_bytes(raw)
            print(f"wrote {args.output} ({len(raw)} bytes)")
        else:
            raw = pathlib.Path(args.fw).read_bytes()
            print(info(F.unpack(raw), raw))
    except (UnknownInput, F.FormatError) as e:
        print(f"fwcutter: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
