#!/bin/bash
# Arm the crash safety net for one SGX test session on the laptop (nothing persists across
# reboots): reboot 10 s after a panic, panic on oops, soft lockup and hung tasks, and send
# the kernel log with netconsole to HOST:PORT (listener there: netconsole-listen.py).
#   usage (on the laptop): sudo ./test-session.sh HOST [PORT]
# HOST is an IPv4 address or a name the laptop resolves; PORT defaults to 6666.
# push-gma500.sh passes both (SGX_NETCONSOLE_HOST, SGX_NETCONSOLE_PORT).
set -euo pipefail
host=${1:-}
port=${2:-6666}
[ -n "$host" ] || { echo "usage: test-session.sh HOST [PORT]" >&2; exit 2; }
if [[ $host =~ ^[0-9]+(\.[0-9]+){3}$ ]]; then
	ip=$host
else
	ip=$(getent ahostsv4 "$host" | awk 'NR == 1 { print $1 }')
	[ -n "$ip" ] || { echo "cannot resolve $host" >&2; exit 1; }
fi
sysctl -w kernel.panic=10 kernel.panic_on_oops=1 kernel.softlockup_panic=1 \
	kernel.hung_task_panic=1
dev=$(ip -o route get "$ip" | grep -o 'dev [^ ]*' | cut -d' ' -f2)
src=$(ip -o route get "$ip" | grep -o 'src [^ ]*' | cut -d' ' -f2)
modprobe netconsole netconsole="6665@$src/$dev,$port@$ip/"
dmesg -n 8
echo "netconsole armed on $dev ($src) -> $ip:$port"
