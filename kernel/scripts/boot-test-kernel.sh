#!/bin/bash
# Reboot the laptop once into the linux-sgx test kernel; the next reboot returns to the
# default (6.19) entry.   usage (on the laptop): sudo ./boot-test-kernel.sh
set -euo pipefail
adv=$(grep -o "submenu 'Advanced options for Arch Linux' \$menuentry_id_option '[^']*'" \
	/boot/grub/grub.cfg | grep -o "gnulinux-advanced-[^']*")
sgx=$(grep -o "gnulinux-linux-sgx-advanced-[^']*" /boot/grub/grub.cfg | head -1)
[ -n "$adv" ] && [ -n "$sgx" ] || { echo "no linux-sgx entry in grub.cfg" >&2; exit 1; }
grub-reboot "$adv>$sgx"
grub-editenv list
systemctl reboot
