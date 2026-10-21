#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
# Regenerate sgx545_ukernel.r2, the radare2 annotation script for the SGX545 ukernel:
#   1. EDM command-handler labels, from the captured init info (initinfo.py);
#   2. hand-written labels and comments (sgx545_labels.r2; see spec §13.7-§13.8);
#   3. a register-name comment on every firmware MMIO access (tools/usse-dis/regtouch.py);
#   4. a flag per touched register in the ESIL register-file window, for emulation with the
#      radare2 fork (tools/usse-dis/r2/README.md, "Emulation").
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$HERE/../.."
PY="$ROOT/.venv/bin/python"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$PY" "$HERE/initinfo.py" "$HERE/extracted/sgx545_initinfo.bin" --r2 "$TMP/handlers.r2" >/dev/null
"$ROOT/tools/usse-dis/usse-dis" "$HERE/extracted/sgx545_ukernel_template.bin" > "$TMP/uk.txt"
(cd "$ROOT" && "$PY" tools/usse-dis/regtouch.py "$TMP/uk.txt" --r2 "$TMP/regs.r2" \
    --r2-flags "$TMP/crflags.r2" >/dev/null)
cat "$TMP/handlers.r2" "$HERE/sgx545_labels.r2" "$TMP/regs.r2" "$TMP/crflags.r2" \
    > "$HERE/sgx545_ukernel.r2"
echo "wrote $HERE/sgx545_ukernel.r2 ($(wc -l < "$HERE/sgx545_ukernel.r2") lines)"
