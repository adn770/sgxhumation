# SGX545 USSE microkernel — extraction manifest

Extracted 2026-10-19 by the mock-and-intercept harness (`../fakedrm.c` + `../loader.c`)
running the closed Cedarview usermode (`libsrv_um` + `libsrv_init` 1.7.862890) on an x86-64
Linux host under a stubbed libdrm. `SrvInit` ran to completion (**returned 0**) and uploaded the
microkernel into the KERNEL_CODE device-memory heap; the harness owns that buffer and
dumped it.

Binaries here are **gitignored** (derivatives of a proprietary blob). They are fully
reproducible from `libsrv_init.so.1.7.862890` — see provenance below.

## Source library

All USSE code is a contiguous blob in **`libsrv_init.so.1.7.862890`** (not `libsrv_um`),
in the read-exec segment (mmap offset 0). Confirmed by correlating each `PVRSRVMemCopy`
source address with `/proc/self/maps` (ASLR-independent: offset = src − lib text base).

## Files

| file | libsrv_init offset | size | notes |
|------|--------------------|------|-------|
| `sgx545_ukernel_template.bin` | `0x2080` | 53096 (0xCF68) | the microkernel as stored in the lib (unpatched) |
| `sgx545_ukernel_runtime.bin`  | `0x2080` | 53096 | same, as actually uploaded — init patched 54 bytes (see below) |
| `sgx545_kcode_off1ea0_256.bin` | `0x1ea0` | 256 | auxiliary USSE program uploaded to KERNEL_CODE |
| `sgx545_kcode_off1fa0_104.bin` | `0x1fa0` | 104 | " |
| `sgx545_kcode_off2008_24.bin`  | `0x2008` | 24  | " |
| `sgx545_kcode_off2020_56.bin`  | `0x2020` | 56  | " |
| `sgx545_kcode_off2058_24.bin`  | `0x2058` | 24  | " |

Memory layout in `libsrv_init.so` (contiguous): `0x1ea0`(256) `0x1fa0`(104) `0x2008`(24)
`0x2020`(56) `0x2058`(24) `0x2080`(ukernel, 0xCF68) → ends `0xEFE8`.

Reproduce the template:
```sh
dd if=libsrv_init.so.1.7.862890 bs=1 skip=$((0x2080)) count=53096 of=sgx545_ukernel_template.bin
```

## Template vs runtime (init-time patching)

`template` and `runtime` are identical except **54 bytes across 18 sites** — each a 3-byte
(24-bit) address field inside a USSE instruction that the usermode loader relocates with
device virtual addresses before upload. Patch sites (offsets into the image):

```
0x0d60 0x0d64 0x0d70 0x0d74 0x0d78 0x0d7c 0x0db0 0x0db4 0x0dc0 0x0dc4
0x0e40 0x0e44 0x0e50 0x0e54   0xbaf8 0xbafc 0xbb10 0xbb14   (each 3 bytes)
```

(The `runtime` capture's patched addresses reflect the heaps the harness presented, which
use the real SGX545 `sgxconfig.h` bases — e.g. KERNEL_CODE @ `0xE8000000` — so they are
representative, though synthesised rather than observed on hardware.)

## Validation

The image is unmistakably USSE machine code: dense 64-bit little-endian instruction words
(`…20fc`, `…00f8`, `…0848`, `…00fd` opcodes), ~61% non-zero, terminating cleanly at the
0xCF68 copy boundary. Since disassembled: 99.9% via `tools/usse-dis` (0 wrong against
Imagination's own `ref_usse_disasm`); see `design/sgx545_hw_spec.md` §13.

## Init info and control blocks (added 2026-10-20)

| file | what |
|------|------|
| `sgx545_initinfo.bin` | 2068-byte IN of `SGX_DEVINITPART2`: the full `SGX_BRIDGE_INIT_INFO` |
| `alloc00..08_heapbeef0003_*.bin` | KERNEL_DATA control blocks as srvinit initialised them |
| `sgx545_initinfo_misc_{a,b}.bin` | the same, with `SGX_GETMISCINFO` answered (`MISC_*` env) |

The nine control blocks are, in order: kernel CCB, CCB control, event kicker, TA3D control,
host control, misc, HWPerf circular buffer, TA and 3D signature buffers.

Decoded by `../initinfo.py`; see `design/sgx545_hw_spec.md` §13. Reproduce by running the
harness (`fakedrm.c` dumps both). The two `_misc_` variants come from runs with distinct core
revision/ID, DDK version/build and build-option replies to `SGX_GETMISCINFO`; they are
byte-identical to the zero-mock capture, as are all 15 buffers (`test_initinfo.py`).

## Layout captures (added 2026-10-20)

`layout_base/` and `layout_moved/` hold the raw harness dumps (`alloc00..14_*.bin` and
`devinitpart2_in.bin`) for two address layouts. `layout_base` matches the files above. In
`layout_moved`, every heap is moved up 1 MiB, the first allocation is at heap + `0x5000` and the
PD address is `0x2345000`. Comparing the two shows which fields srvinit derives from which
address. They are the reference for `tools/fwcutter` (`design/sgx545_fw_format.md` §6).
