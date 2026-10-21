#!/bin/bash
# Build gma500 on the build host, install it on the laptop's linux-sgx kernel and boot that
# kernel once with the crash safety net armed; returns when the new module is up. Runs on
# the workstation, which relays the files between the two hosts over ssh.
#
#   usage: [SGX_RENDER=0] [SGX_DRY_RUN=1] kernel/scripts/push-gma500.sh [--no-reboot]
#
# The test kernel boots with gma500_gfx.sgx_render=1 (the render node the tests use) unless
# SGX_RENDER=0 asks for the gate-off boot (no render node, card0 answers only DEV_QUERY).
# The installed kernel is 7.3.0-sgx with LOCALVERSION_AUTO off, so a module built with an
# empty LOCALVERSION keeps matching it. gma500 loads from the initramfs, hence mkinitcpio.
# SGX_DRY_RUN=1 prints the commands instead of running them.
#
# Settings (environment, or kernel/scripts/local.env; see local.env.example):
#   SGX_BUILD_HOST       ssh destination of the build host            (sgx-build)
#   SGX_TARGET_HOST      ssh destination of the test laptop           (sgx-target)
#   SGX_TARGET_SSH_OPTS  extra ssh/scp options for the laptop         (none)
#   SGX_KERNEL_TREE      kernel tree on the build host, from $HOME    (src/linux-sgx)
#   SGX_KERNEL_RELEASE   release of the installed test kernel         (7.3.0-sgx)
#   SGX_BUILD_JOBS       make -j on the build host                    ($(nproc) there)
#   SGX_BUILD_PATH       directories prepended to PATH on the build host (none)
#   SGX_NETCONSOLE_HOST  where the laptop sends its kernel log        (the build host)
#   SGX_NETCONSOLE_PORT  UDP port of netconsole-listen.py             (6666)
# The remote build command is wrapped in bash -c, so any login shell works on the build host.
set -euo pipefail
SCRIPTS=$(dirname "$0")
LOCAL_ENV=${SGX_LOCAL_ENV:-$SCRIPTS/local.env}
# shellcheck source=/dev/null
[ -f "$LOCAL_ENV" ] && . "$LOCAL_ENV"
BUILD=${SGX_BUILD_HOST:-sgx-build}
TARGET=${SGX_TARGET_HOST:-sgx-target}
read -ra TOPTS <<< "${SGX_TARGET_SSH_OPTS:-}"
KTREE=${SGX_KERNEL_TREE:-src/linux-sgx}
KREL=${SGX_KERNEL_RELEASE:-7.3.0-sgx}
JOBS=${SGX_BUILD_JOBS:-'$(nproc)'}
BPATH=${SGX_BUILD_PATH:+export PATH=$SGX_BUILD_PATH:\$PATH; }
NC_HOST=${SGX_NETCONSOLE_HOST:-$(ssh -G "$BUILD" 2>/dev/null | awk '$1 == "hostname" { print $2; exit }')}
NC_PORT=${SGX_NETCONSOLE_PORT:-6666}
MOD=drivers/gpu/drm/gma500/gma500_gfx.ko
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

run() {
	if [ -n "${SGX_DRY_RUN:-}" ]; then
		printf '%s\n' "$*"
	else
		"$@"
	fi
}
on_target() { run ssh ${TOPTS[@]+"${TOPTS[@]}"} "$TARGET" "$@"; }

run ssh "$BUILD" "bash -c '${BPATH}cd $KTREE &&
	LOCALVERSION= make ARCH=i386 -j$JOBS M=drivers/gpu/drm/gma500 >/dev/null &&
	strings $MOD | grep -q \"^vermagic=$KREL \"'"
run scp -q "$BUILD:$KTREE/$MOD" "$tmp/"
run scp -q ${TOPTS[@]+"${TOPTS[@]}"} "$tmp/gma500_gfx.ko" "$TARGET:/tmp/"
run scp -q ${TOPTS[@]+"${TOPTS[@]}"} "$SCRIPTS/boot-test-kernel.sh" "$SCRIPTS/test-session.sh" \
	"$TARGET:"
on_target "sudo install -m644 /tmp/gma500_gfx.ko \
	/usr/lib/modules/$KREL/kernel/$MOD && sudo depmod $KREL &&
	sudo mkinitcpio -p linux-sgx >/dev/null 2>&1"
[ "${1:-}" = --no-reboot ] && exit 0

on_target "sudo ./boot-test-kernel.sh ${SGX_RENDER:-1} >/dev/null" || true
run sleep 15
until on_target -o ConnectTimeout=4 -o BatchMode=yes \
	"test \"\$(uname -r)\" = $KREL && test \"\$(cut -d. -f1 /proc/uptime)\" -lt 300 &&
	 sudo test -e /sys/kernel/debug/dri/0/sgx/load" 2>/dev/null; do
	sleep 5
done
# Every test boot runs with the safety net armed: panic and lockup reboots,
# netconsole to the listener (kernel/scripts/test-session.sh, netconsole-listen.py).
on_target "sudo ./test-session.sh $NC_HOST $NC_PORT >/dev/null"
on_target 'sudo dmesg | grep -E "# gma500_[a-z_]+: pass|^\[[ 0-9.]+\] (not )?ok [0-9]+ gma500"'
