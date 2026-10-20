#!/bin/bash
# Build gma500 on cachix, install it on the laptop's linux-sgx kernel and boot that kernel
# once with the crash safety net armed; returns when the new module is up. Runs on the Mac
# (relays files between hosts).
#
#   usage: kernel/scripts/push-gma500.sh [--no-reboot]
#
# The installed kernel is 7.3.0-sgx with LOCALVERSION_AUTO off, so a module built with an
# empty LOCALVERSION keeps matching it. gma500 loads from the initramfs, hence mkinitcpio.
set -euo pipefail
CACHIX=build-host
LAPTOP=user@laptop
SSH_LAPTOP=(ssh -o HostKeyAlias=cedar "$LAPTOP")
MOD=drivers/gpu/drm/gma500/gma500_gfx.ko
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

ssh "$CACHIX" "bash -c 'export PATH=~/.local/bin:\$PATH; cd ~/src/linux-sgx &&
	LOCALVERSION= make ARCH=i386 -j32 M=drivers/gpu/drm/gma500 >/dev/null &&
	strings $MOD | grep -q \"^vermagic=7.3.0-sgx \"'"
scp -q "$CACHIX:src/linux-sgx/$MOD" "$tmp/"
scp -q -o HostKeyAlias=cedar "$tmp/gma500_gfx.ko" "$LAPTOP:/tmp/"
"${SSH_LAPTOP[@]}" "sudo install -m644 /tmp/gma500_gfx.ko \
	/usr/lib/modules/7.3.0-sgx/kernel/$MOD && sudo depmod 7.3.0-sgx &&
	sudo mkinitcpio -p linux-sgx >/dev/null 2>&1"
[ "${1:-}" = --no-reboot ] && exit 0

"${SSH_LAPTOP[@]}" 'sudo ./boot-test-kernel.sh >/dev/null' || true
sleep 15
until "${SSH_LAPTOP[@]}" -o ConnectTimeout=4 -o BatchMode=yes \
	'test "$(uname -r)" = 7.3.0-sgx && test "$(cut -d. -f1 /proc/uptime)" -lt 300 &&
	 sudo test -e /sys/kernel/debug/dri/0/sgx/load' 2>/dev/null; do
	sleep 5
done
# Every test boot runs with the safety net armed: panic and lockup reboots,
# netconsole to cachix (kernel/scripts/test-session.sh).
"${SSH_LAPTOP[@]}" 'sudo ./test-session.sh >/dev/null'
"${SSH_LAPTOP[@]}" 'sudo dmesg | grep -E "gma500_sgx_[a-z]+: pass|not ok"'
