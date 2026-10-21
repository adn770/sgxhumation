# Kernel side

The SGX545 bring-up lives in gma500, as a patch series on mainline. The kernel tree is on
cachix (`~/src/linux-sgx`, branch `sgx545` on `v7.3`); all kernel builds happen there.

| path | what |
|------|------|
| `patches/` | `git format-patch v7.3..sgx545`, the source of truth, re-exported after each commit |
| `sgx_fw/` | the firmware parser (also in the series) plus its userspace harness, `fwdump` |
| `gma500/` | working copies: `gma_sgx.[ch]` (gma500's side) and `sgxfive/` (the GPU driver; |
| | its `sgx_fw.*` are copied from `sgx_fw/`), edited here and copied into the tree |
| `scripts/` | installing and booting test kernels on the laptop, the netconsole listener |

Test cycle: `scripts/push-gma500.sh` builds gma500 on cachix with `LOCALVERSION=` (the installed
kernel is `7.3.0-sgx` and `LOCALVERSION_AUTO` is off, so the module keeps matching it), installs
it on the laptop and boots the test kernel once. Check the device buffers with
`tools/fwcutter/check_dump.py` on a copy of debugfs `dri/0/sgx/`.

The sgxfive render node is opt-in (`design/sgx545_render_uapi.md` §6.3, decision 8): gma500
creates it only with `gma500_gfx.sgx_render=1`, a read-only parameter for trusted users only.
Without it there is no `/dev/dri/renderD*` and card0 answers only DEV_QUERY (`-EPERM` for the
rest), so the render-node tests skip. `scripts/boot-test-kernel.sh` boots the test kernel with
`gma500_gfx.sgx_render=1`, read by the linux-sgx GRUB entries from grubenv (`sgx_cmdline`);
`boot-test-kernel.sh 0`, or `SGX_RENDER=0 scripts/push-gma500.sh`, boots it with the gate off.
`push-gma500.sh` copies both laptop scripts before booting.
