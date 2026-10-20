# Kernel side

The SGX545 bring-up lives in gma500, as a patch series on mainline. The kernel tree is on
cachix (`~/src/linux-sgx`, branch `sgx545` on `v7.3`); all kernel builds happen there.

| path | what |
|------|------|
| `patches/` | `git format-patch v7.3..sgx545`, the source of truth, re-exported after each commit |
| `sgx_fw/` | the firmware parser (also in the series) plus its userspace harness, `fwdump` |
| `gma500/` | working copies of the new gma500 files, edited here and copied into the tree |
| `scripts/` | installing and booting test kernels on the laptop, the netconsole listener |

Test cycle: build on cachix with `LOCALVERSION=` (the installed kernel is `7.3.0-sgx`, and
`LOCALVERSION_AUTO` is off, so modules keep matching it); copy `gma500_gfx.ko` to the laptop,
`depmod`, `mkinitcpio -p linux-sgx`; then `boot-test-kernel.sh`. Check the device buffers with
`tools/fwcutter/check_dump.py` on a copy of debugfs `dri/0/sgx/`.
