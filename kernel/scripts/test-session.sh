#!/bin/bash
# Arm the crash safety net for one SGX test session on the laptop (nothing persists across
# reboots): reboot 10 s after a panic, panic on oops, soft lockup and hung tasks, and send
# the kernel log to cachix with netconsole (listener there: netconsole-listen.py).
#   usage (on the laptop): sudo ./test-session.sh
set -euo pipefail
sysctl -w kernel.panic=10 kernel.panic_on_oops=1 kernel.softlockup_panic=1 \
	kernel.hung_task_panic=1
dev=$(ip -o route get build-host | grep -o 'dev [^ ]*' | cut -d' ' -f2)
src=$(ip -o route get build-host | grep -o 'src [^ ]*' | cut -d' ' -f2)
modprobe netconsole netconsole="6665@$src/$dev,6666@build-host/"
dmesg -n 8
echo "netconsole armed on $dev ($src) -> build-host:6666"
