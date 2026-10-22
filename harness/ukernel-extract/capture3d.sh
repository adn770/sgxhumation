#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
#
# capture3d.sh — run tri under fakedrm.so once per line of a variation list
# (variations3d.txt: "name option..."), each in its own staging directory
# $SGX_UK_DIR/var/<name>/ that links lib/ ($SGX_UK_LIB, default $SGX_UK_DIR/lib),
# devinit.bin and powervr.ini from $SGX_UK_DIR. Words "ini:Key=Value" are not tri options:
# they are appended to that run's own copy of powervr.ini (closed-driver app hints such as
# PBSize, ZLSThreshold, ExternalZBufferMode, FlushBehaviour). Each run leaves drm.log, tri.out
# and dump3d/ there; diff3d.py compares them. Run it where tri and fakedrm.so
# are built (README "3D capture").
#
#   SGX_UK_DIR=~/sgxhumation-uk3d ./capture3d.sh variations3d.txt [name...]
set -eu

list=${1:?usage: capture3d.sh <variations> [name...]}
shift
uk=${SGX_UK_DIR:?set SGX_UK_DIR}
lib=${SGX_UK_LIB:-$uk/lib}       # the closed usr/lib (and libpvrFAKEWSEGL.so)
here=$(cd "$(dirname "$0")" && pwd)

grep -v '^[[:space:]]*\(#\|$\)' "$list" | while read -r name opts; do
    if [ $# -gt 0 ]; then
        case " $* " in *" $name "*) ;; *) continue ;; esac
    fi
    d=$uk/var/$name
    rm -rf "$d"
    mkdir -p "$d"
    ln -s "$lib" "$d/lib"
    ln -s "$uk/devinit.bin" "$d/devinit.bin"
    cp "$uk/powervr.ini" "$d/powervr.ini"
    tri_opts=
    for w in $opts; do
        case $w in
            ini:*) printf '%s\n' "${w#ini:}" >> "$d/powervr.ini" ;;
            *) tri_opts="$tri_opts $w" ;;
        esac
    done
    # shellcheck disable=SC2086  # the options are separate words
    if SGX_UK_DIR=$d LD_LIBRARY_PATH=$lib LD_PRELOAD=$here/fakedrm.so \
        "$here/tri" $tri_opts > "$d/tri.out" 2>&1; then
        status=ok
    else
        status="FAILED ($?)"
    fi
    echo "$name: $status, $(ls "$d/dump3d" 2>/dev/null | grep -c '_in.bin') kicks"
done
