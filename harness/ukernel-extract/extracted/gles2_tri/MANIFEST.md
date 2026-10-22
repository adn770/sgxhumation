<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->

# gles2_tri capture manifest

The closed Cedarview GLES2 stack (DDK 1.7.862890) drawing one triangle under the harness
(`../../README.md`, "3D capture"). Everything in this directory except this file is
gitignored: it holds bytes written by the closed libraries. Regenerate it with `tri` under
`fakedrm.so` and copy `$SGX_UK_DIR/dump3d/` and `drm.log` here.

| file | bytes | sha256 |
|---|---|---|
| `drm.log` | 183127 | `486230d6135cf8c4...` |
| `dump3d/kick00_in.bin` | 564 | `455fdc55079a4a1a...` |
| `dump3d/kick01_in.bin` | 564 | `01340b42a86e61f0...` |

- `dump3d/kick00/`: 153 objects, 6545408 bytes, `objects.txt` sha256 `26461944c8e21c66...`
- `dump3d/kick01/`: 153 objects, 6545408 bytes, `objects.txt` sha256 `26461944c8e21c66...`
- `dump3d/exit/`: 153 objects, 6545408 bytes, `objects.txt` sha256 `26461944c8e21c66...`

Objects at the second (render) kick, with the role `summary3d.py` gives them (`-` = not
reached by the walk; see the cross references in its output):

| obj | heap | device address | size | sha256 | role |
|---|---|---|---|---|---|
| 0 | KERNEL_DATA | `0xf0001000` | `0x3c` | `147262916009` | parameter buffer descriptor |
| 1 | KERNEL_DATA | `0xf0002000` | `0x10` | `7dff5c9b0088` | parameter buffer block |
| 2 | PERCONTEXT_3DPARAMETERS | `0xc0001000` | `0x422000` | `43f13e0cea41` | - |
| 3 | PERCONTEXT_3DPARAMETERS | `0xc0423000` | `0x40000` | `db05a84f2580` | DPM page table (par... |
| 4 | KERNEL_DATA | `0xf0003000` | `0x10420` | `951c6a0a7de2` | TA client CCB; TA command (SGXM... |
| 5 | KERNEL_DATA | `0xf0014000` | `0x8` | `c1d2cb63958c` | TA client CCB control |
| 6 | SYNCINFO | `0xe0001000` | `0x4` | `ad7facb2586f` | - |
| 7 | SYNCINFO | `0xe0002000` | `0x18` | `ad7facb2586f` | - |
| 8 | PERCONTEXT_3DPARAMETERS | `0xc0463000` | `0x40` | `ad7facb2586f` | scratch primitive block |
| 9 | SYNCINFO | `0xe0003000` | `0x80` | `ad7facb2586f` | - |
| 10 | KERNEL_DATA | `0xf0015000` | `0x44` | `ad7facb2586f` | - |
| 11 | KERNEL_DATA | `0xf0016000` | `0x44` | `6435dc47f36a` | HW render context |
| 12 | GENERAL | `0x08001000` | `0x80004` | `2af25e8ac647` | - |
| 13 | KERNEL_DATA | `0xf0017000` | `0x104c8` | `b5822048397c` | - |
| 14 | KERNEL_DATA | `0xf0028000` | `0x8` | `ad7facb2586f` | - |
| 15 | SYNCINFO | `0xe0004000` | `0x4` | `ad7facb2586f` | - |
| 16 | SYNCINFO | `0xe0005000` | `0x18` | `ad7facb2586f` | - |
| 17 | SYNCINFO | `0xe0006000` | `0x4` | `ad7facb2586f` | - |
| 18 | SYNCINFO | `0xe0007000` | `0x18` | `47ff72e4a75a` | - |
| 19 | PIXELSHADER | `0xf4001000` | `0x800` | `ad7facb2586f` | - |
| 20 | GENERAL | `0x08082000` | `0x4800` | `cc61635da46b` | - |
| 21 | PDSPIXEL_CODEDATA | `0xe4001000` | `0x1800` | `9f1dcbc35c35` | - |
| 22 | PIXELSHADER | `0xf4002000` | `0x18` | `41ded4cb0e04` | - |
| 23 | KERNEL_DATA | `0xf0029000` | `0x4` | `ad7facb2586f` | - |
| 24 | PIXELSHADER | `0xf4003000` | `0x10` | `d3a638a35e3e` | - |
| 25 | PIXELSHADER | `0xf4004000` | `0x58` | `21403ce5debb` | - |
| 26 | PIXELSHADER | `0xf4005000` | `0x48` | `9ddb41788068` | - |
| 27 | PIXELSHADER | `0xf4006000` | `0x18` | `8b2476c03e5d` | - |
| 28 | PIXELSHADER | `0xf4007000` | `0x10` | `5f79e12ea615` | - |
| 29 | PIXELSHADER | `0xf4008000` | `0x10` | `fc663ab344dd` | - |
| 30 | PIXELSHADER | `0xf4009000` | `0x10` | `c6f305bb8239` | - |
| 31 | PIXELSHADER | `0xf400a000` | `0x10` | `dbe18bc87db5` | - |
| 32 | PIXELSHADER | `0xf400b000` | `0x10` | `54be1be458c5` | - |
| 33 | PIXELSHADER | `0xf400c000` | `0x18` | `febe82401445` | - |
| 34 | PIXELSHADER | `0xf400d000` | `0x10` | `d8fedf53a22f` | - |
| 35 | PIXELSHADER | `0xf400e000` | `0x10` | `3a8a15074f4f` | - |
| 36 | PIXELSHADER | `0xf400f000` | `0x10` | `a91361468062` | - |
| 37 | PIXELSHADER | `0xf4010000` | `0x10` | `57289775282d` | - |
| 38 | PIXELSHADER | `0xf4011000` | `0x10` | `d3a638a35e3e` | - |
| 39 | PIXELSHADER | `0xf4012000` | `0x10` | `17a20ffd665b` | - |
| 40 | PIXELSHADER | `0xf4013000` | `0x10` | `a83ecee31e5e` | - |
| 41 | PIXELSHADER | `0xf4014000` | `0x10` | `d56b3f74140c` | - |
| 42 | PIXELSHADER | `0xf4015000` | `0x10` | `1c510722f901` | - |
| 43 | PIXELSHADER | `0xf4016000` | `0x10` | `79f0e2e93aaa` | - |
| 44 | PIXELSHADER | `0xf4017000` | `0x18` | `f2c34874e166` | - |
| 45 | PIXELSHADER | `0xf4018000` | `0x10` | `77f6f198c4bd` | - |
| 46 | PIXELSHADER | `0xf4019000` | `0x10` | `a22a340f50ca` | - |
| 47 | PIXELSHADER | `0xf401a000` | `0x10` | `c68570b49c18` | - |
| 48 | PIXELSHADER | `0xf401b000` | `0x10` | `f8494832e98d` | - |
| 49 | PIXELSHADER | `0xf401c000` | `0x10` | `a40327e31033` | - |
| 50 | PIXELSHADER | `0xf401d000` | `0x18` | `494e0f91ba30` | - |
| 51 | PIXELSHADER | `0xf401e000` | `0x10` | `74595e0eaa6c` | - |
| 52 | PIXELSHADER | `0xf401f000` | `0x10` | `796fc4d75c05` | - |
| 53 | PIXELSHADER | `0xf4020000` | `0x10` | `576ed9c32c8d` | - |
| 54 | PIXELSHADER | `0xf4021000` | `0x10` | `b5e50e3660f7` | - |
| 55 | PIXELSHADER | `0xf4022000` | `0x18` | `0efc7de6c13f` | - |
| 56 | PIXELSHADER | `0xf4023000` | `0x30` | `e7dabe590e1d` | - |
| 57 | PIXELSHADER | `0xf4024000` | `0xa8` | `42d5b1b60132` | - |
| 58 | PIXELSHADER | `0xf4025000` | `0xd0` | `0f4d1338d3c5` | - |
| 59 | PIXELSHADER | `0xf4026000` | `0x28` | `92cad07630d6` | - |
| 60 | PIXELSHADER | `0xf4027000` | `0x28` | `1839a857d19c` | - |
| 61 | PIXELSHADER | `0xf4028000` | `0x38` | `7bdc129d0490` | - |
| 62 | PIXELSHADER | `0xf4029000` | `0x38` | `2088dc86bdf5` | - |
| 63 | PIXELSHADER | `0xf402a000` | `0x30` | `13056539de8f` | - |
| 64 | PIXELSHADER | `0xf402b000` | `0x40` | `41269dac7643` | - |
| 65 | PIXELSHADER | `0xf402c000` | `0x40` | `98c5e481a0b4` | - |
| 66 | PIXELSHADER | `0xf402d000` | `0x38` | `2081de213124` | - |
| 67 | PIXELSHADER | `0xf402e000` | `0x18` | `032b6bde23c4` | - |
| 68 | PIXELSHADER | `0xf402f000` | `0x18` | `b0bb77fb6592` | - |
| 69 | PIXELSHADER | `0xf4030000` | `0x20` | `34bf3d945e3a` | - |
| 70 | PIXELSHADER | `0xf4031000` | `0x1a0` | `560ee8402efb` | - |
| 71 | PIXELSHADER | `0xf4032000` | `0x90` | `aeaa6fddbc0d` | - |
| 72 | PIXELSHADER | `0xf4033000` | `0x10` | `ea50941d2559` | - |
| 73 | PIXELSHADER | `0xf4034000` | `0x28` | `de00313cdfba` | - |
| 74 | PDSPIXEL_CODEDATA | `0xe4003000` | `0x400` | `0c2b16d51614` | - |
| 75 | KERNEL_DATA | `0xf002a000` | `0xb4` | `79bd44f534a4` | - |
| 76 | GENERAL | `0x08087000` | `0x2000` | `9f1dcbc35c35` | - |
| 77 | KERNEL_DATA | `0xf002b000` | `0x1ec` | `1e64a952e4af` | - |
| 78 | SYNCINFO | `0xe0008000` | `0x8` | `ad7facb2586f` | - |
| 79 | KERNEL_DATA | `0xf002c000` | `0x8` | `ad7facb2586f` | - |
| 80 | KERNEL_DATA | `0xf002d000` | `0x4` | `ad7facb2586f` | - |
| 81 | KERNEL_DATA | `0xf002e000` | `0x28` | `4265d6b243e1` | - |
| 82 | SYNCINFO | `0xe0009000` | `0x8` | `ad7facb2586f` | - |
| 83 | KERNEL_DATA | `0xf002f000` | `0x1ec` | `afd50e60a248` | - |
| 84 | SYNCINFO | `0xe000a000` | `0x8` | `ad7facb2586f` | - |
| 85 | KERNEL_DATA | `0xf0030000` | `0x8` | `ad7facb2586f` | - |
| 86 | KERNEL_DATA | `0xf0031000` | `0x4` | `ad7facb2586f` | - |
| 87 | KERNEL_DATA | `0xf0032000` | `0x28` | `5ee58f523dc7` | - |
| 88 | SYNCINFO | `0xe000b000` | `0x8` | `ad7facb2586f` | - |
| 89 | KERNEL_DATA | `0xf0033000` | `0x1ec` | `c2afb02caa64` | render details |
| 90 | SYNCINFO | `0xe000c000` | `0x8` | `f0c500e2401e` | render details access word |
| 91 | KERNEL_DATA | `0xf0034000` | `0x8` | `ad7facb2586f` | pixel event update list |
| 92 | KERNEL_DATA | `0xf0035000` | `0x4` | `a74c3f518fca` | background object PDS update list |
| 93 | KERNEL_DATA | `0xf0036000` | `0x28` | `7af1be9c63a5` | destination sync list |
| 94 | SYNCINFO | `0xe000d000` | `0x8` | `f0c500e2401e` | - |
| 95 | KERNEL_DATA | `0xf0037000` | `0x4` | `2cfe0bfc092f` | render target pending count |
| 96 | KERNEL_DATA | `0xf0038000` | `0x88` | `76cb927fb52f` | render target data set; render ta... |
| 97 | TADATA | `0xd0001000` | `0x100` | `ad7facb2586f` | tail pointers |
| 98 | TADATA | `0xd0002000` | `0x40` | `ad7facb2586f` | DPM context control |
| 99 | TADATA | `0xd0003000` | `0x50` | `ad7facb2586f` | OTPM |
| 100 | PERCONTEXT_3DPARAMETERS | `0xc0464000` | `0x200` | `b86ac3493952` | background object |
| 101 | TADATA | `0xd0004000` | `0x300` | `ad7facb2586f` | ZLS external-Z region headers |
| 102 | TADATA | `0xd0005000` | `0x88` | `ad7facb2586f` | DPM context state |
| 103 | KERNEL_DATA | `0xf0039000` | `0x4` | `ad7facb2586f` | render target status word |
| 104 | TADATA | `0xd0006000` | `0x300` | `ad7facb2586f` | ISP region headers |
| 105 | TADATA | `0xd0007000` | `0x88` | `ad7facb2586f` | DPM context state |
| 106 | KERNEL_DATA | `0xf003a000` | `0x4` | `ad7facb2586f` | render target status word |
| 107 | GENERAL | `0x08089000` | `0x4` | `ad7facb2586f` | - |
| 108 | SYNCINFO | `0xe000e000` | `0x18` | `f0c500e2401e` | - |
| 109 | SYNCINFO | `0xe000f000` | `0x4` | `f0c500e2401e` | 3D status word [2] |
| 110 | PDSPIXEL_CODEDATA | `0xe4004000` | `0xc800` | `9a85e46ee18e` | end-of-render PDS progra... |
| 111 | SYNCINFO | `0xe0010000` | `0x18` | `ad7facb2586f` | - |
| 112 | SYNCINFO | `0xe0011000` | `0x4` | `e80ead4fda4c` | 3D status word [0] |
| 113 | PIXELSHADER | `0xf4035000` | `0x400` | `32d760ad17d3` | - |
| 114 | SYNCINFO | `0xe0012000` | `0x18` | `ad7facb2586f` | - |
| 115 | SYNCINFO | `0xe0013000` | `0x4` | `9049b5c56cc7` | 3D status word [1] |
| 116 | VERTEXSHADER | `0xfc001000` | `0x48` | `dd2cf0837d9b` | - |
| 117 | PDSVERTEX_CODEDATA | `0xec001000` | `0x60` | `e298d0978f36` | PDS state program (VDM wo... |
| 118 | PDSVERTEX_CODEDATA | `0xec002000` | `0x40` | `2f1a943ffb3c` | PDS state program (VDM wo... |
| 119 | GENERAL | `0x0808a000` | `0x4` | `6a60dac9d259` | - |
| 120 | GENERAL | `0x0808b000` | `0x4` | `8895079adb1e` | - |
| 121 | VERTEXSHADER | `0xfc008000` | `0x8000` | `dc2c2cd960cc` | - |
| 122 | SYNCINFO | `0xe0014000` | `0x18` | `ad7facb2586f` | - |
| 123 | PIXELSHADER | `0xf4038000` | `0x8000` | `ea27823ddf41` | - |
| 124 | SYNCINFO | `0xe0015000` | `0x18` | `ad7facb2586f` | - |
| 125 | PDSPIXEL_CODEDATA | `0xe4018000` | `0x8000` | `4085f380e69b` | - |
| 126 | SYNCINFO | `0xe0016000` | `0x18` | `ad7facb2586f` | - |
| 127 | SYNCINFO | `0xe0017000` | `0x4` | `f0c500e2401e` | TA status word [6] |
| 128 | PIXELSHADER | `0xf4040000` | `0x8` | `869fd724f085` | - |
| 129 | VERTEXSHADER | `0xfc010000` | `0x8` | `869fd724f085` | - |
| 130 | PIXELSHADER | `0xf4041000` | `0x30` | `01cc4102dea8` | - |
| 131 | GENERAL | `0x0808c000` | `0x5000` | `61b610bd6acf` | VDM control stream |
| 132 | SYNCINFO | `0xe0018000` | `0x18` | `ad7facb2586f` | - |
| 133 | SYNCINFO | `0xe0019000` | `0x4` | `feb04a0c336f` | TA status word [0] |
| 134 | GENERAL | `0x08091000` | `0x32000` | `abf15d2b4af3` | - |
| 135 | SYNCINFO | `0xe001a000` | `0x18` | `ad7facb2586f` | - |
| 136 | SYNCINFO | `0xe001b000` | `0x4` | `03f76d187d80` | TA status word [1] |
| 137 | GENERAL | `0x080c3000` | `0x32000` | `279dd00e01b6` | - |
| 138 | SYNCINFO | `0xe001c000` | `0x18` | `ad7facb2586f` | - |
| 139 | SYNCINFO | `0xe001d000` | `0x4` | `f865af87cdec` | TA status word [2] |
| 140 | PDSVERTEX_CODEDATA | `0xec003000` | `0xc800` | `78ee3486f2d1` | - |
| 141 | SYNCINFO | `0xe001e000` | `0x18` | `ad7facb2586f` | - |
| 142 | SYNCINFO | `0xe001f000` | `0x4` | `0fd37cc3313f` | TA status word [3] |
| 143 | PDSVERTEX_CODEDATA | `0xec010000` | `0x4` | `ad7facb2586f` | - |
| 144 | SYNCINFO | `0xe0020000` | `0x18` | `ad7facb2586f` | - |
| 145 | SYNCINFO | `0xe0021000` | `0x4` | `ad7facb2586f` | TA status word [4] |
| 146 | PDSVERTEX_CODEDATA | `0xec011000` | `0xc800` | `2326821529d6` | - |
| 147 | SYNCINFO | `0xe0022000` | `0x18` | `ad7facb2586f` | - |
| 148 | SYNCINFO | `0xe0023000` | `0x4` | `9018bbab3927` | TA status word [5] |
| 149 | GENERAL | `0x080f5000` | `0x800` | `2f75107cc5a7` | - |
| 150 | SYNCINFO | `0xe0024000` | `0x18` | `ad7facb2586f` | - |
| 151 | PDSVERTEX_CODEDATA | `0xec01e000` | `0x30` | `d6aae04f095b` | - |
| 152 | SYNCINFO | `0xe0025000` | `0x18` | `ad7facb2586f` | - |

## Differential captures (`var/`)

`capture3d.sh variations3d.txt` runs `tri` once per variation, each changing one piece of GL
state against `base` (`../../variations3d.txt` lists the options), and leaves
`var/<name>/{drm.log,tri.out,dump3d/}` here (2026-10-21, 64 runs, one per line of
`variations3d.txt` and per row of the table below). The copies keep the `exit/` snapshots and
the parameter buffer pages (`obj002_c0001000.bin`, all zero): every run with a snapshot has
`dump3d/{kick00,kick01,exit}/`, so `var/` holds 286 `exit/` directories and 858 `obj002`
files (861 with `dump3d/` above). The srvinit `dump/` of each run stays on the build box,
except for `var/s32x512/dump/`. `diff3d.py --all var` compares every
capture with `base`; the findings are in `design/sgx545_3d.md`, "State words (differential
captures)". The kick IN buffers differ between runs (handles); `base2` repeats `base` and
its extracted state is identical. sha256 prefixes and the size of the second kick's
snapshot:

| variation | `kick00_in.bin` | `kick01_in.bin` | `kick01/objects.txt` | objects, bytes |
|---|---|---|---|---|
| `base` | `4779b72edc1b59c4` | `c940a1ef32c23885` | `26461944c8e21c66` | 153, 6545408 |
| `base2` | `8af29dc4788dbbc1` | `103db66a407284af` | `26461944c8e21c66` | 153, 6545408 |
| `color_red` | `dfa8030ba7a77650` | `306c26d5c86b451b` | `26461944c8e21c66` | 153, 6545408 |
| `color_green` | `db8b3e560ec5bc97` | `ba129e92179e4f0e` | `26461944c8e21c66` | 153, 6545408 |
| `color_white` | `90f6dfe4d76bb549` | `3aa5e0b42e56e597` | `26461944c8e21c66` | 153, 6545408 |
| `color_half` | `9d6a2d5aca7e1b84` | `9277e25e0dc3856e` | `26461944c8e21c66` | 153, 6545408 |
| `clear_red` | `0e1aaf700ba1bbd6` | `19bc65b1e5edbc36` | `26461944c8e21c66` | 153, 6545408 |
| `clear_black` | `69ef8a62e78ad793` | `d3675f85648193e3` | `26461944c8e21c66` | 153, 6545408 |
| `clear_grey` | `30edb392f4617ae0` | `f367c1cd2d435dcd` | `26461944c8e21c66` | 153, 6545408 |
| `noclear` | `b10e1cdd357c772d` | `623d968d7c8b1b9f` | `26461944c8e21c66` | 153, 6545408 |
| `size_32x32` | `2ae8823a42b923ba` | `25e40f708e741bed` | `323b61a852e8177a` | 153, 6541312 |
| `size_128x32` | `d3717e2a3fd9f879` | `10336776d05f840e` | `26461944c8e21c66` | 153, 6545408 |
| `size_100x60` | `c2074fa226812d92` | `1f4e6008c5063b55` | `e9a9d00dacfaf876` | 153, 6553600 |
| `size_256x64` | `4af603ff47a4c8b9` | `6c5ccc6583b3a871` | `d4e0712e4fd20bef` | 153, 6569984 |
| `viewport_q` | `e73d1bc7838aff65` | `ecec5609b8263679` | `26461944c8e21c66` | 153, 6545408 |
| `viewport_half` | `78ebfbfbdf671ce8` | `d41fa14ca07f9444` | `26461944c8e21c66` | 153, 6545408 |
| `scissor` | `4387a55d19fd25e2` | `20d0f7493b972628` | `26461944c8e21c66` | 153, 6545408 |
| `scissor_tl` | `c6b9a19d2a26f120` | `cdebd85eed397bbc` | `26461944c8e21c66` | 153, 6545408 |
| `depth_never` | `03a52957df3ae275` | `0fc8ec9b2397325f` | `ceef854c5300084d` | 154, 6565888 |
| `depth_less` | `ca94abf674340554` | `7d2f3298530aad93` | `ceef854c5300084d` | 154, 6565888 |
| `depth_equal` | `a54bff8c77ed14fb` | `bf155b2116e7c57a` | `ceef854c5300084d` | 154, 6565888 |
| `depth_lequal` | `311467cba9cb80ef` | `1eecdb6ec412447b` | `ceef854c5300084d` | 154, 6565888 |
| `depth_greater` | `7a1fe37143210c91` | `93c21f717263f3aa` | `ceef854c5300084d` | 154, 6565888 |
| `depth_notequal` | `e5a8e59e14179627` | `d9ec64d9dcc3c8c7` | `ceef854c5300084d` | 154, 6565888 |
| `depth_gequal` | `9fd9fc1c32790595` | `42fa00bb16c6fbae` | `ceef854c5300084d` | 154, 6565888 |
| `depth_always` | `2b890d3624287196` | `8006f30164ba8d87` | `ceef854c5300084d` | 154, 6565888 |
| `depth_less_cd05` | `ce098093ab2e9966` | `2bf91578d52f16a0` | `ceef854c5300084d` | 154, 6565888 |
| `depth_less_ro` | `bddad37749a08010` | `311d7250bb8b98e0` | `ceef854c5300084d` | 154, 6565888 |
| `depth_less_z05` | `94485774546e853d` | `1ee6af0b7bc796f7` | `ceef854c5300084d` | 154, 6565888 |
| `stencil_equal` | `bc8e895218a76b77` | `152baef3bce9bf7f` | `ceef854c5300084d` | 154, 6565888 |
| `stencil_always` | `8ba0ab1da732df3e` | `0d87aeaf70a90db4` | `ceef854c5300084d` | 154, 6565888 |
| `stencil_less` | `3cf43b72e34dd84f` | `a0e9231b05b14a09` | `ceef854c5300084d` | 154, 6565888 |
| `stencil_ref85` | `00c42e9bb1be7623` | `786da53609366321` | `ceef854c5300084d` | 154, 6565888 |
| `stencil_masks` | `e60219fe47b39069` | `547009863e643580` | `ceef854c5300084d` | 154, 6565888 |
| `stencil_ops` | `353cbb356dc3fe6d` | `bb8b414a3dd4fe53` | `ceef854c5300084d` | 154, 6565888 |
| `stencil_ops2` | `b0bb84cc5b942981` | `67eab92f3b35b906` | `ceef854c5300084d` | 154, 6565888 |
| `depthrange` | `9e2fc4d88d10c980` | `06989d34529ef299` | `26461944c8e21c66` | 153, 6545408 |
| `scissor_far` | `d8c600608ffc872b` | `974143c5c9425be0` | `26461944c8e21c66` | 153, 6545408 |
| `viewport_far` | `de2ccfa9d9e414df` | `426f78ccc7a7b1a7` | `26461944c8e21c66` | 153, 6545408 |
| `blend_alpha` | `8cc2205dd5063d3f` | `1fd51c6a2d881e9a` | `26461944c8e21c66` | 153, 6545408 |
| `blend_add` | `3302f0ca4de87030` | `e22ca189e0ed0c11` | `26461944c8e21c66` | 153, 6545408 |
| `blend_sub` | `49dc5cc09da3ccfe` | `feea1f07258b0272` | `26461944c8e21c66` | 153, 6545408 |
| `blend_mul` | `27acc25e77572791` | `3f7491145ae5cfc3` | `26461944c8e21c66` | 153, 6545408 |
| `cull_back` | `cd12f2904bb24b2c` | `128152c54c8d1281` | `26461944c8e21c66` | 153, 6545408 |
| `cull_front` | `badd6330888b0074` | `e90e14c0de7505e4` | `26461944c8e21c66` | 153, 6545408 |
| `cull_both` | `76a76e99dd11fb72` | `f601c72f28baf3fb` | `52b8b4ccaa5835ef` | 149, 6529024 |
| `cull_back_cw` | `30694359cf5a860f` | `53ba3139a1434cd4` | `26461944c8e21c66` | 153, 6545408 |
| `frontface_cw` | `9124da34dc11cc33` | `0c4ef3113e29405c` | `26461944c8e21c66` | 153, 6545408 |
| `mask_1110` | `b2cce17c70f4850c` | `0d530b72bcde6489` | `26461944c8e21c66` | 153, 6545408 |
| `mask_1000` | `c1dc725062851ab6` | `734981543a158412` | `26461944c8e21c66` | 153, 6545408 |
| `mask_0000` | `6423ad46b9194ce8` | `8ade212c21a84480` | `26461944c8e21c66` | 153, 6545408 |
| `dither0` | `e5d26f50a7864e05` | `5a8fc90c5a927f24` | `26461944c8e21c66` | 153, 6545408 |
| `prim_two` | `10ed94a659293194` | `8457d68a0c0640b0` | `26461944c8e21c66` | 153, 6545408 |
| `prim_strip` | `a856ccc8131314a8` | `d3c34f68e42795f6` | `26461944c8e21c66` | 153, 6545408 |
| `prim_fan` | `bbc4e67865c9a492` | `ff4351213138a5e3` | `26461944c8e21c66` | 153, 6545408 |
| `prim_points` | `e2134eb9f35ce526` | `a17b621f7f1f299d` | `26461944c8e21c66` | 153, 6545408 |
| `prim_lines` | `6ec3915c4864ef05` | `0771a2101cd34c7e` | `26461944c8e21c66` | 153, 6545408 |
| `prim_linestrip` | `606d320fa3ec33ca` | `dd031996dcd69296` | `26461944c8e21c66` | 153, 6545408 |
| `shift` | `099afbc78270baeb` | `2555dbbd04e02447` | `26461944c8e21c66` | 153, 6545408 |
| `scale` | `776b4ccefa3a28d9` | `5e9a84eb929ec613` | `26461944c8e21c66` | 153, 6545408 |
| `z05` | `612c5614cce1ea8b` | `367dc59e86bd8542` | `26461944c8e21c66` | 153, 6545408 |
| `varying` | `58883f935dbf3f8f` | `21f96c16b9ede945` | `af16bdc9806c9654` | 153, 6545408 |
| `fmt8888` | `2682c3efeebb18b2` | `d7045a8a74449ac8` | `e28b3920c7ae7fde` | 153, 6553600 |
| `fmt8888_128x32` | `3b09092a2012f2ed` | `3621db6fea70bcb9` | `e28b3920c7ae7fde` | 153, 6553600 |

## Size captures (`sizes3d.txt`)

`capture3d.sh sizes3d.txt` (2026-10-21) runs `tri` at 225 render-target sizes, from 1x1 to
4096x4096 (4097 and 8192 pixels fail in the closed EGL), into `var/<name>/` like the
variations above; 222 succeed. The three that fail (`m16x4097`, `m4097x16`, `m8192x16`) keep
`drm.log` and `tri.out` and an empty `dump3d/`. With the 64 variations, `var/` holds 289 runs:
64 state variations and 225 size/tiling runs (`h*`, `w*`, `m*`, `s*`, `t*`). The run named
`s32x512` here (`sizes3d.txt`: `size=32x512`) is the build box's `var/s16x512`: the same
32x512 capture, with identical files, under an older name.
`tiling3d.py var` checks the layout rule of `design/sgx545_3d.md` §14.6 against these and
every other capture here: 286 captures, no mismatch. No hashes are listed: the kick IN
buffers differ between runs (handles) and the words that matter are the ones `tiling3d.py` reads.
