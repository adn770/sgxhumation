#!/bin/bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Josep Torra
# Build the Arch Linux 32 (pentium4) base image for i686 builds on cachix: the userland
# of the Cedarview laptop, so binaries built in it link against the same libraries.
# Runs pacman --root inside Docker's official archlinux image, where we are root, so
# no sudo is needed on the host; the x86-64 kernel runs the i686 userland natively.
#
#   usage (on cachix): tools/arch32/build-rootfs.sh KEYRING.pkg.tar.zst [PACKAGE...]
#
# KEYRING is archlinux32-keyring as the laptop's pacman cache holds it (pacman checked
# it there); it seeds the trust for every package pacman then downloads. The image is
# tagged sgxhumation/arch32:base.
set -euo pipefail
keyring=$(realpath "$1")
shift
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cp "$keyring" "$out/keyring.pkg.tar.zst"
cat > "$out/pacman32.conf" <<'CONF'
[options]
Architecture = pentium4
SigLevel = Required DatabaseOptional
ParallelDownloads = 8
[core]
Include = /out/mirrorlist32
[extra]
Include = /out/mirrorlist32
[community]
Include = /out/mirrorlist32
CONF
cat > "$out/mirrorlist32" <<'CONF'
Server = https://de.mirror.archlinux32.org/$arch/$repo
Server = https://archlinux32.andreasbaumann.cc/$arch/$repo
Server = https://mirror.juniorjpdj.pl/archlinux32/$arch/$repo
CONF
docker run --rm -v "$out:/out" archlinux:latest bash -euc '
	# Trust Arch Linux 32 as its keyring package does on install.
	mkdir /tmp/k && tar -C /tmp/k -xf /out/keyring.pkg.tar.zst
	cp /tmp/k/usr/share/pacman/keyrings/* /usr/share/pacman/keyrings/
	pacman-key --init >/dev/null 2>&1
	pacman-key --populate archlinux32 >/dev/null 2>&1
	# The packager key that signs most packages is certified by two master keys, one
	# short of full trust, so the laptop holds a local signature on it; do the same.
	pacman-key --lsign-key 80EC18799E8BCD375C6E64ABE4D41569196B1160 >/dev/null 2>&1
	# pacman --root rather than pacstrap, which mounts /proc and /dev in the new
	# root and so needs a privileged container.
	mkdir -p /rootfs/var/lib/pacman /rootfs/var/cache/pacman/pkg /rootfs/dev
	# Install scriptlets need /dev/null; /proc cannot be mounted here, so the
	# systemd-tmpfiles hook fails harmlessly (images built on this run it).
	mknod -m 666 /rootfs/dev/null c 1 3
	pacman --root /rootfs --config /out/pacman32.conf --gpgdir /etc/pacman.d/gnupg \
		--cachedir /rootfs/var/cache/pacman/pkg -Sy --noconfirm \
		base archlinux32-keyring "$@" >/rootfs/install.log 2>&1 ||
		{ tail -20 /rootfs/install.log; exit 1; }
	grep -B3 -iE "error|warning" /rootfs/install.log | head -40 || true
	rm -rf /rootfs/var/cache/pacman/pkg/* /rootfs/install.log /rootfs/dev/null
	cp /out/pacman32.conf /rootfs/etc/pacman.conf
	sed -i "s|/out/mirrorlist32|/etc/pacman.d/mirrorlist|" /rootfs/etc/pacman.conf
	cp /out/mirrorlist32 /rootfs/etc/pacman.d/mirrorlist
	cp -a /etc/pacman.d/gnupg /rootfs/etc/pacman.d/
	tar -C /rootfs --numeric-owner --exclude "./etc/pacman.d/gnupg/S.*" -cf /out/rootfs.tar .
' _ "$@"
# setarch: uname -m says i686 inside, so meson and configure scripts see the target.
docker import --change 'ENTRYPOINT ["/usr/bin/setarch", "i686"]' --change 'CMD ["/bin/bash"]' \
	--change 'ENV LANG=C.UTF-8' \
	"$out/rootfs.tar" sgxhumation/arch32:base
docker run --rm sgxhumation/arch32:base bash -c 'uname -m; file -L /usr/bin/bash; pacman -Q | wc -l'
