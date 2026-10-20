#!/bin/bash
# Install a linux-sgx test kernel package on the Cedarview laptop, next to the distro
# kernels, without changing what boots by default.
#
#   usage (on the laptop): sudo ./install-test-kernel.sh linux-sgx-*.pkg.tar.zst
#
# GRUB_DEFAULT=saved with saved_entry=0 boots the top "Arch Linux" entry, and grub-mkconfig
# puts the highest kernel version there. GRUB_TOP_LEVEL pins it to the distro kernel, so the
# test kernel only ever boots one-shot via boot-test-kernel.sh.
set -euo pipefail
pkg=$1
grep -q '^GRUB_TOP_LEVEL=' /etc/default/grub ||
	echo 'GRUB_TOP_LEVEL="/boot/vmlinuz-linux"' >> /etc/default/grub
# The kernel names the package arch i386 (UTS_MACHINE); Arch Linux 32 calls it pentium4.
pacman -U --noconfirm --arch i386 "$pkg"
grub-mkconfig -o /boot/grub/grub.cfg
top=$(grep -m1 -A20 "^menuentry 'Arch Linux'" /boot/grub/grub.cfg | grep -m1 -o '/vmlinuz-[^ ]*')
[ "$top" = /vmlinuz-linux ] || { echo "top entry boots $top, not /vmlinuz-linux" >&2; exit 1; }
grub-editenv list
