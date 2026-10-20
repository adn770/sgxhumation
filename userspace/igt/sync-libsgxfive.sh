#!/bin/sh
# Copy the parts of libsgxfive the IGT tests use into lib/sgxfive/, the files the IGT
# patch adds as lib/sgxfive/: the ioctl layer, the transfer builders and engine, and
# our assembled programs. libsgxfive's tests/test_vendored.py checks they are current.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../libsgxfive
mkdir -p "$here/lib/sgxfive"
for f in include/sgxfive.h include/sgxfive_xfer.h src/dev.c src/xfer.c src/xfer_job.c \
	 src/prog_asm.c src/prog_asm_gen.h; do
	cp "$src/$f" "$here/lib/sgxfive/"
done
