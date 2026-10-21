# SGX545 firmware container (`sgx545_ukernel.fw`), formats v1 to v4

`tools/fwcutter/` builds this file from the user's own copy of the closed Cedarview
`libsrv_init.so`. A future kernel driver loads it with `request_firmware()`. This is the
b43-fwcutter model: the tool ships no Imagination bytes, only offsets, sizes, hashes and our
own reconstructed tables. The user supplies the blob.

The file carries everything srvinit contributes to an SGX545 boot: the code images, how to
patch them for the device addresses the driver picks, the register init scripts, and the
initial contents of the control blocks. The reference loader (`tools/fwcutter/fwload.py`)
reproduces the harness captures byte for byte from it, at two different address layouts
(§6). That is the evidence that the format is complete.

Format v2 (§9) adds the transfer-queue program templates a solid fill needs, cut from the
user's `libsrv_um.so` in the same way. §1–§8 describe v1 and stay valid for v2: a v2 file is a
v1 file plus three section types and one `VALUES` list. Format v3 (§10) adds the two templates
of a copy and raises the template and parameter limits; it has no new section types. Format
v4 (§11) lets the templates be our own programs (`tools/usse-asm/progs/`, assembled) under a
third origin, `OURS`, so that 2D needs no `libsrv_um.so`.

## 1. Two kinds of content

Every section is tagged with its origin, so the two never mix:

| origin | what | where it comes from |
|--------|------|---------------------|
| `CUT` | the ukernel and the five PDS programs, as stored in `libsrv_init.so` | the user's blob |
| `FACT` | everything else | `tools/fwcutter/facts.py`, our reconstruction (spec §13) |
| `OURS` | transfer-queue templates written by us (v4, §11) | `tools/usse-asm/progs/`, assembled |

Item by item:

| item | origin | notes |
|------|--------|-------|
| ukernel template (`0x2080`, `0xCF68` bytes) | CUT | the 9 placeholder `mov`s stay unpatched |
| PDS programs #10–#14 (`0x1ea0`–`0x2070`) | CUT | code and data; the data is mostly zero |
| patch table for the ukernel and PDS data | FACT | which field, which value (§4) |
| PDS data-segment constants | FACT | srvinit generates them at runtime; they are not in the blob |
| buffer sizes, alignments, heaps | FACT | the allocations srvinit requests |
| init scripts part 1, part 2, deinit | FACT | spec §13.5 |
| `asInitDevData` | FACT | spec §13.5 |
| TA3DCtl initial image (the `sa` binding) | FACT | spec §13.9 |
| TA / 3D signature register lists | FACT | register offsets the EDM samples |
| kick table, MKIF sizes, EDM task regs, clock gating, cache control | FACT | §13.3–13.5 |
| HostCtl layout, CCB protocol | — | not in the file: driver source (`ccb.py` is the reference) |

The fact sections are what srvinit's *code* computes. They are build-specific, because the
PDS programs index specific data slots and the ukernel expects specific `sa` values. That is
why they travel with the cut code in one file, instead of being compiled into the driver: a
driver that reads them from the file supports any build the cutter knows, without changes.

## 2. Container

Little-endian throughout. The file is a 32-byte header, a section table, then the section
payloads, each 8-byte aligned.

```
header (32 bytes)
  +0   char[8] magic           "SGX545FW"
  +8   u16     format_version  1 to 4 (§9.1, §10.1, §11.1)
  +10  u16     section_count
  +12  u8      ddk_major       1
  +13  u8      ddk_minor       7
  +14  u16     reserved        0
  +16  u32     ddk_build       862890
  +20  u32     build_options   0x60218 (the client build options srvinit reports)
  +24  u32     file_size
  +28  u32     crc32           CRC-32 (zlib) of bytes [32, file_size)

section table entry (16 bytes)
  +0   u16     type
  +2   u16     origin          0 = CUT, 1 = FACT, 2 = OURS (v4 templates, §11)
  +4   u32     offset          from the start of the file
  +8   u32     size
  +12  u32     arg             type-specific
```

A loader must reject a wrong magic, an unknown `format_version`, a size or CRC mismatch, and
an unknown section type.

| type | name | origin | arg | payload |
|------|------|--------|-----|---------|
| 1 | `IMAGE` | CUT | buffer id | the buffer's template bytes |
| 2 | `BUFFERS` | FACT | 0 | buffer records (§3) |
| 3 | `PATCHES` | FACT | 0 | expression records, applied to buffer images (§4) |
| 4 | `VALUES` | FACT | list id | expression records evaluated to a list of values (§5) |
| 5 | `EDM_META` | FACT | 0 | 31 × u32 (§5) |
| 6 | `TQ_TEMPLATE` | CUT, OURS | template id | v2+: a transfer-queue program template (§9) |
| 7 | `TQ_PATCHES` | FACT | 0 | v2 only: expression records, applied to templates (§9.4) |
| 8 | `TQ_META` | FACT | 0 | v2 only: template records (§9.3) |

## 3. Buffers

The driver allocates one device buffer per record and places it where it likes, within the
constraints below. Buffer ids:

| id | buffer | heap | size | align |
|----|--------|------|------|-------|
| 0 | ukernel | KERNEL_CODE | `0xcf68` | 8 |
| 1–5 | PDS #10–#14 | KERNEL_CODE | `0x18 0x38 0x18 0x100 0x68` | 16 |
| 6 | kernel CCB | KERNEL_DATA | `0x2000` | `0x1000` |
| 7 | CCB control | KERNEL_DATA | `0x8` | `0x1000` |
| 8 | event kicker | KERNEL_DATA | `0x4` | 16 |
| 9 | TA3DCtl | KERNEL_DATA | `0xec` | 32 |
| 10 | HostCtl | KERNEL_DATA | `0x44` | 32 |
| 11 | misc | KERNEL_DATA | `0x54` | 32 |
| 12 | HWPerf circular buffer | KERNEL_DATA | `0x640c` | 32 |
| 13 | TA signature buffer | KERNEL_DATA | `0x620` | 32 |
| 14 | 3D signature buffer | KERNEL_DATA | `0x2088` | 32 |

Heap ids are `sgxapi_km.h`'s: 2 KERNEL_CODE, 3 KERNEL_DATA, 4 PIXELSHADER, 5 VERTEXSHADER,
6 PDSPIXEL_CODEDATA.

```
buffer record (16 bytes)
  +0   u8      id
  +1   u8      heap
  +2   u8      pds_rows        PDS data-segment size in rows of 4 dwords (0 if not PDS)
  +3   u8      reserved
  +4   u32     size            allocation size
  +8   u32     align
  +12  u32     reserved
```

A buffer starts as its `IMAGE` section (zero-padded to `size`) or, without one, as zeros.
Then the patches are applied.

**Placement constraint.** The EDM code base (`USE_CODE_BASE_15`) is the KERNEL_CODE heap base.
PDS code words name the ukernel by instruction index from that base, so the ukernel's offset
`uk_off = (addr[0] − heap_base[2]) / 8` enters every code word. **For the firmware to run,
`uk_off` must be 0**: the ukernel's own branch targets and the CCB service addresses (the kick
table) are also instruction indices from the code base, and nothing relocates them. srvinit's
real allocator puts the ukernel at the heap base, so it never notices. The harness captures
place it at `+0x1000` (`uk_off = 0x200`) and `+0x5000`: srvinit still patches the PDS words
consistently, which is why the loader accepts any `uk_off`, but such an image faults on its
first absolute branch. Measured on the laptop (2026-10-20): `uk_off = 0x200` faulted in
context 15 on page `0xE8000000`; `uk_off = 0` booted.

## 4. Expressions and patches

A 16-byte expression record computes one 32-bit value and says how to store it:

```
expression record (16 bytes)
  +0   u8      buffer          target buffer id (PATCHES); 0xff in VALUES
  +1   u8      encoding
  +2   u8      kind
  +3   u8      param           encoding parameter
  +4   u32     offset          byte offset in the buffer (PATCHES); register or index (VALUES)
  +8   u32     target
  +12  u32     addend
```

| kind | value |
|------|-------|
| 0 `CONST` | `addend` |
| 1 `ADDR` | `addr[target] + addend` |
| 2 `ADDR_SHR4` | `(addr[target] + addend) >> 4` (the `emitpds` address operand) |
| 3 `HEAP` | `heap_base[target] + addend` |
| 4 `PD_PHYS` | `pd_phys + addend` (the EDM page directory's physical address) |
| 5 `CODE_WORD` | `addend \| ((target + uk_off) << 4)`, with `target` a ukernel instruction index |
| 6 `PDS_CTL` | `(pds_rows[target] << 23) \| addend` (an `emitpds` control word) |
| 7 `PDS_ROWS` | `pds_rows[target]` |

| encoding | store |
|----------|-------|
| 0 `WORD32` | the value as a dword at `offset` |
| 1 `USSE_LIMM` | `imm32` of the `mov rN, #imm32` at `offset`: `e[20:0]`, `e[40:36]`, `e[49:44]` |
| 2 `CODE_BASE` | `USE_CODE_BASE_n` form: `(value >> 7) & 0x1ffffff \| param << 25`; `param` = DM |

`USSE_LIMM` patches only the immediate bits. All other bits of the instruction (opcode,
destination) come from the template, which holds `mov rN, #0xDEADBEEF` at each patch site.

The `PATCHES` table for 1.7.862890 has 81 records:

- **ukernel, 9 `USSE_LIMM`** (the 18 three-byte patch sites of spec §13.9 are the two halves of
  these 9 instructions):

  | insn | byte | value | used as |
  |------|------|-------|---------|
  | 428 | `0xd60` | `PDS_CTL` #11, `0x1000` | `emitpds` control, PDS #11 |
  | 430 | `0xd70` | `ADDR_SHR4` PDS #11 | `emitpds` address, PDS #11 |
  | 431 | `0xd78` | `ADDR` TA3DCtl | boot task reads TA3DCtl |
  | 438 | `0xdb0` | `PDS_CTL` #12, `0x100` | `emitpds` control, PDS #12 |
  | 440 | `0xdc0` | `ADDR_SHR4` PDS #12 | `emitpds` address, PDS #12 |
  | 456 | `0xe40` | `ADDR` PDS #13 | → `EVENT_OTHER_PDS_EXEC` |
  | 458 | `0xe50` | `PDS_ROWS` #13 | → `EVENT_OTHER_PDS_DATA` |
  | 5983 | `0xbaf8` | `PDS_CTL` #14, `0x100` | `emitpds` control, PDS #14 |
  | 5986 | `0xbb10` | `ADDR_SHR4` PDS #14 | `emitpds` address, PDS #14 |

- **PDS data segments, 24 `WORD32`**: the code words of every USSE entry (spec §13.8;
  addend `0x0800000f`, except PDS #11's `0xf`), PDS #11's TA3DCtl DMA source address and DMA
  control word `0x03c0003f`, and constants (`2`, `0x100`, `0x200`, the PDS #14 request masks
  `0x4000f` and `0x120080`).
- **TA3DCtl, 8 `WORD32`**: `sa0` TA3DCtl, `sa1` HostCtl, `sa2` CCB control, `sa22` CCB, `sa23`
  `PD_PHYS`, `sa43`–`sa45` HWPerf / TA signature / 3D signature buffers.
- **Signature buffers, 40 `WORD32` `CONST`**: a register count, a zero word, then the register
  offsets the EDM samples: 6 for TA, 32 for 3D.

## 5. Values and EDM metadata

`VALUES` sections, by list id: 0 init script part 1, 1 part 2, 2 deinit (record `offset` = the
register, in write order), 3 `asInitDevData` (record `offset` = the index; absent entries are
0). Address-valued entries are expressions: `USE_CODE_BASE_0–9` = `HEAP` PIXELSHADER
`+ n·0x80000`, data master 1; `10–13` = `HEAP` VERTEXSHADER, data master 0; `15` = `HEAP`
KERNEL_CODE, data master 3; `PDS_EXEC_BASE` = `HEAP` PDSPIXEL_CODEDATA; `EVENT_KICKER` =
`ADDR` kicker; `EVENT_OTHER_PDS_EXEC` = `ADDR` PDS #10; `EVENT_OTHER_PDS_DATA` = `PDS_ROWS`
#10; dev data 12 and 13 = `HEAP` PDSPIXEL_CODEDATA and PIXELSHADER.

`EDM_META` is 31 dwords: the 11 host kick addresses (`SGXMKIF_CMD_*` order, 0 = no handler),
the 13 MKIF struct sizes (spec §13.3 order), the two EDM task registers, clock gating (`ctl`,
`ctl2`, status register, status mask) and the cache-control word.

The kick addresses are **not relocated**. They stay the same when the ukernel moves within
the heap (§6), unlike the PDS code words.

## 6. Verification

`tools/fwcutter/test_fwload.py` runs the loader with the addresses of two harness captures,
both kept in `harness/ukernel-extract/extracted/` (gitignored):

| capture | heap bases | first allocation | PD address |
|---------|-----------|------------------|------------|
| `layout_base` | `sgxconfig.h` | heap + `0x1000` | `0x1000` |
| `layout_moved` | each heap + 1 MiB | heap + `0x5000` | `0x2345000` |

For both, the loader output equals the capture: all 15 buffers (the rest of each captured
page is zero), the three init scripts, dev data, kick table, struct sizes and EDM values.
`layout_moved` exercises every expression kind with values that differ from `layout_base`.

`test_fwcutter.py` checks that the static carve equals the harness's extracted template, and
that the cutter refuses an unknown input.

## 7. Known inputs

| file | SHA-256 | size | DDK |
|------|---------|------|-----|
| `libsrv_init.so.1.7.862890` | `8c5a59c4…e327869a` | 63524 | 1.7.862890 |
| `libsrv_um.so.1.7.862890` | `2b2c0986…11988a44` | 196152 | 1.7.862890 (v2, §9) |
| `libsrv_init.so.1.7.862890` | `4d3df850…effe53b6d` | 66976 | 1.7.862890 (Ubuntu) |
| `libsrv_um.so.1.7.862890` | `e19402fc…19ad0dce` | 198204 | 1.7.862890 (Ubuntu) |

The first two come from `cdv-gfx-drivers-1.0.3_bee.tar.bz2` (catalog §5.1): the
`cedarview-userspace-v1.0.3_bee` tarball inside it, `usr/lib/`. The Ubuntu pair comes from
`cedarview-graphics-drivers_20120717.orig.tar.gz` (Ubuntu 12.04 multiverse), `usr/lib/`. It
is a different link of the same code: every `libsrv_init.so` section sits 0x80 bytes lower
(the ukernel at 0x2000), and in `libsrv_um.so` the code immediates sit 0x1d0 and the whole
programs 0x200 bytes higher. The cut sections and templates hash the same, so both pairs give
the same file, byte for byte.

Each cut section is also checked against its own SHA-256, so a future table entry with the
same file hash but different offsets would be caught. Accepting the `.deb` or the tarball
directly is a later addition.

## 8. Out of scope

Kernel-side loading, a C header generated from `facts.py`, and anything on hardware. (The C
loader in `kernel/sgx_fw/` has since ported §2–§5, §9 and §10.)


## 9. Format v2: transfer-queue templates

A solid fill (`design/sgx545_transfer.md` §4.2) runs seven small programs on the GPU. Their
bytes are Imagination's, stored in or generated by `libsrv_um.so`, so they travel in this file
the way the ukernel does: `CUT` sections carved at run time from the user's library, plus
`FACT` tables saying how to place and patch them. This settles the proposal of transfer §4.3;
§9.8 lists where it differs.

### 9.1 Versions and the cutter

- `fwcutter.py cut <libsrv_init.so>` still writes a **v1** file, byte for byte the same as
  before. `fwcutter.py cut <libsrv_init.so> <libsrv_um.so>` writes a **v3** file since §10;
  with `--format-version 2` it writes the v2 file described here. Both libraries must be
  known inputs (§7) of the same DDK build, or the cutter refuses.
- The writer sets `format_version` to 1 when the file has no transfer-queue content
  (sections 6–8 or `VALUES` list 4), and otherwise to the lowest version whose limits hold
  it (§10.1): 2 for the seven fill templates. So a v1 loader keeps loading every file cut
  from `libsrv_init.so` alone, and rejects a v2 file on its version.
- A v2 loader reads both versions. It rejects sections 6–8 and `VALUES` list 4 in a v1 file.
  A v2 file without them is valid (the cutter never writes one).
- In a v2 file, every `TQ_TEMPLATE` and every `TQ_PATCHES` record names a template that has a
  `TQ_META` record, and a template's image is at most its size.

### 9.2 Templates (`TQ_TEMPLATE`, CUT)

Template ids are a namespace of their own, 0–7 in v2 (0–15 in v3), separate from buffer
ids. Each template is assembled from pieces of `libsrv_um`, `(source offset, length,
destination offset)`, into a zeroed array, and the result is checked against its SHA-256. A
template taken from the immediates of a code generator is thus no different from one copied
out of `.rodata`.

| id | name | pieces | size | SHA-256 |
|----|------|--------|------|---------|
| 0 | `USE_FILL` | `0x22218`+8 → 0; `0x1d1cc`+4 → 12 | `0x10` | `7727cde6…a7a400b0` |
| 1 | `USE_SEC` | `0x223d8`+16 → 0 | `0x10` | `74c3e748…1847cece` |
| 2 | `USE_EOT` | 9 imm32, below | `0x38` | `d03fa3dc…5a6667be` |
| 3 | `USE_EOR` | 4 imm32, below | `0x18` | `dfbb3b5d…a4ccddaa` |
| 4 | `PDS_PRIM` | `0x227e0`+24 → 0 (`KICKONLY`) | `0x18` | `0963b59a…55005092` |
| 5 | `PDS_SEC` | `0x229a0`+24 → 0 (`BASIC`) | `0x18` | `0963b59a…55005092` |
| 6 | `PDS_PIXEVENT` | `0x23080`+196 → 0 | `0xc4` | `65e78fa8…19881671` |

`USE_EOT` takes the 4-byte immediates at `0x2001e` → byte 4, `0x20062` → 12, 20, 28 and 36,
`0x2007c` → 40, `0x20083` → 44, `0x20092` → 48 and `0x200b3` → 52. `USE_EOR` takes `0x1fd9d` →
4, `0x1fdab` → 12, `0x1fdb2` → 16 and `0x1fdb9` → 20. `USE_FILL`'s piece at `0x1d1cc` is the
`or eax, imm32` of the colour patcher, so the template already reads `mov.skipinv.end o0, #0`.
Templates 4 and 5 are byte-identical in this build (and equal to PDS #10), as are the two
pixel-event copies at `0x23080` and `0x23160`; they keep separate ids because other builds may
differ. Disassembled (`refdisasm545`, `pds-dis`), the cut templates read:

```
USE_FILL      phas #0, #0, pixel, end, parallel; mov.skipinv.end o0, #0x00000000
USE_SEC       phas …; nop.end
USE_EOT       phas …; mov.skipinv r0, #0 (× 4); emitpix1.freep #0, r0, r1;
              emitpix2.end.freep #0, r2, r3, r0, #0x00000000
USE_EOR       phas …; mov.skipinv r0, #0; emitpix2.end.freep #0, r0, #0, r0, #0x00000200
PDS_PRIM/SEC  4 data dwords (0); movs doutu, ds0[0], ds0[1], ds1[0]; halt
PDS_PIXEVENT  20 data dwords; 29 instructions branching on ir1 bits 3, 1, 8 and 2
```

The four `mov`s of `USE_EOT` all name `r0` until patched: the generator adds the register
number in a loop (`i << 21`), which is code, not a byte range, so it is a FACT patch (§9.4).

### 9.3 Template records (`TQ_META`, FACT)

```
template record (16 bytes)
  +0   u8      id
  +1   u8      heap            4 PIXELSHADER (USE code), 6 PDSPIXEL (PDS programs)
  +2   u8      temps           USE temporaries; enters the DOUTU word
  +3   u8      pds_attrs       attributes the PDS program's task receives
  +4   u32     size            allocation size
  +8   u32     align
  +12  u16     pds_data        PDS data segment in bytes (0 for USE code)
  +14  u8      flags           bit 0 NO_STRADDLE: must not cross a 4 KiB page
  +15  u8      reserved        0
```

| id | heap | temps | attrs | size | align | data | flags |
|----|------|-------|-------|------|-------|------|-------|
| 0 `USE_FILL` | 4 | 0 | 0 | `0x10` | 8 | 0 | 1 |
| 1 `USE_SEC` | 4 | 0 | 0 | `0x10` | 8 | 0 | 1 |
| 2 `USE_EOT` | 4 | 4 | 0 | `0x38` | 8 | 0 | 1 |
| 3 `USE_EOR` | 4 | 1 | 0 | `0x18` | 8 | 0 | 1 |
| 4 `PDS_PRIM` | 6 | 0 | 0 | `0x18` | 16 | `0x10` | 1 |
| 5 `PDS_SEC` | 6 | 0 | 0 | `0x18` | 16 | `0x10` | 1 |
| 6 `PDS_PIXEVENT` | 6 | 0 | 1 | `0xc4` | 16 | `0x50` | 1 |

These are the resource records `SGXCreateTransferContext` fills (transfer §4.2.4); the
no-straddle flag is the closed allocator's page rule. The driver places each template where it
likes, with its address **aligned**, **at or above its heap's base** and, with `NO_STRADDLE`,
inside one page. `DOUTU` (§9.4) adds one more constraint for USE code.

The background object needs `pds_data` and `pds_attrs` of `PDS_PRIM` and `PDS_SEC`; it stays
in driver code (transfer §4.2.3), like the PBE words (§9.6), because both are SGX545 data
formats, not build facts.

### 9.4 Patches (`TQ_PATCHES`, FACT)

The 16-byte expression record of §4, with `buffer` = the template id. The driver copies each
template into its placed copy (zero-padded to `size`) and applies the records **in file order**;
order matters where a `WORD32` and a `USSE_LIMM` share a dword. In transfer-queue records,
`param` is 0 and these kinds are allowed:

| kind | value |
|------|-------|
| 0 `CONST` | `addend` |
| 1 `ADDR` | `tq_addr[target] + addend`: `target` is a **template** id here |
| 3 `HEAP` | `heap_base[target] + addend` |
| 8 `PARAM` | `param[target] + addend`, a per-transfer parameter (§9.6) |
| 9 `DOUTU` | the `DOUTU` word 0 of template `target`, below |

`DOUTU` takes `off = tq_addr[target] + addend − heap_base[heap of target]`, which must be a
multiple of 8 in `[0, 0x80000)`, and yields

```
((off << 1) & 0xffff0) | ((off >> 19) & 0xf) | ((temps[target] + 3) >> 2) << 24
```

(`EXE` [19:4], `CBASE` [3:0], `TRC` [31:24], transfer §4.2.4). Keeping USE code in the first
512 KiB of the heap makes `CBASE` 0, which sidesteps the 18-bit-PC reading of the `DOUTU`
layout (transfer §4.2.8 item 6). Encodings are `WORD32`, `USSE_LIMM` and one new one:

| encoding | store |
|----------|-------|
| 3 `EMITPIX_SB` | the 14-bit sideband of the `emitpix2` at `offset`, other bits kept |

`EMITPIX_SB` scatters value bits [5:0] to `e[27:22]`, [11:6] to `e[40:35]` and [13:12] to
`e[55:54]`, as the generator at `0x20087`–`0x200b7` does.


v1 sections (`PATCHES`, `VALUES` 0–3) may not use kinds 8 and 9 or encoding 3; v2 records
may not use kinds 2 and 4–7 or encoding 2. For 1.7.862890 there are 22 records:

| template | byte | encoding | value | what |
|----------|------|----------|-------|------|
| `USE_FILL` | 8 | `USSE_LIMM` | `PARAM` 0 | the colour |
| `USE_EOT` | 16, 24, 32 | `WORD32` | `CONST` `1 << 21`, `2 << 21`, `3 << 21` | dest `r1`–`r3` |
| `USE_EOT` | 8, 16, 24, 32 | `USSE_LIMM` | `PARAM` 1–4 | PBE words 0–3 into `r0`–`r3` |
| `USE_EOT` | 48 | `EMITPIX_SB` | `PARAM` 5 | the PBE sideband |
| `PDS_PRIM` | 0, 4, 8 | `WORD32` | `DOUTU` `USE_FILL`, `CONST` 4, `CONST` 0 | `U0`, `U1`, `U2` |
| `PDS_SEC` | 0, 4, 8 | `WORD32` | `DOUTU` `USE_SEC`, `CONST` 4, `CONST` 0 | `U0`, `U1`, `U2` |
| `PDS_PIXEVENT` | 0, 4 | `WORD32` | `DOUTU` `USE_EOT`, `CONST` `0x00800000` | PTOFF task |
| `PDS_PIXEVENT` | 8, 12 | `WORD32` | `CONST` 0, `DOUTU` `USE_EOT` | `douta` source, EOT `U0` |
| `PDS_PIXEVENT` | 16, 20 | `WORD32` | `DOUTU` `USE_EOR`, `CONST` `0x00c00000` | EOR task |
| `PDS_PIXEVENT` | 48 | `WORD32` | `CONST` `0x00800000` | EOT `U1` base (`ds0[6]`) |

The pixel-event dwords are the seven `0x1e24f`–`0x1e27d` writes (transfer §4.2.4); in banks
(hardware spec §13.8) they are `ds0[0]`, `ds0[1]`, `ds1[0]`, `ds1[1]`, `ds0[2]`, `ds0[3]` and
`ds0[6]`. `LINESTRIDE` (§9.6) counts pixel pairs.

### 9.5 Command words (`VALUES` list 4, `TQ_CMD`, FACT)

The fixed `SGXMKIF_TRANSFERCMD` words of a fill (transfer §4.2.6), as `WORD32` expression
records evaluated like §9.4 (template ids, parameters). `offset` is the byte offset in the
command: a multiple of 4, with `offset + 4` at most the `TRANSFERCMD` size in `EDM_META`.

| offset | field | value |
|--------|-------|-------|
| `0x004` | `ISPBgObj` | `0x200` |
| `0x008` | `ISPBgObjDepth` | `0x3f800000` |
| `0x00c` | `ISPRender` | 3 |
| `0x014` | `ISPIPFMisc` | `0x100` |
| `0x01c` | `3DAAMode` | `0x80000000` |
| `0x020` | `ISPMultiSampCtl` | `0x88` |
| `0x024` | `EDMPixelPDSExec` | `ADDR` `PDS_PIXEVENT` |
| `0x028` | `EDMPixelPDSData` | 5 (`pds_data >> 4`) |
| `0x02c` | `EDMPixelPDSInfo` | 1 (`(4 × pds_attrs + 15) >> 4`) |
| `0x038` | `PixelBE` | `0x80` |
| `0x03c` | `PDSExecBase` | `HEAP` PDSPIXEL |
| `0x194` | `num_pixel_partitions` | `0x07c00000` |

The driver zeroes the command, writes these, then the fields that depend on its own memory
and queue: `ISPBgObjTag`, `Bif3DReqBase`, `ISPRenderBox1/2`, `ui32Size`, `ui32Flags`, the
status values and `fence_id`.

### 9.6 Parameters of a fill

The `PARAM` values come from the fill, in driver code (`fwload.fill_params()`,
`sgx_fw_tq_fill_params()`). An ARGB8888 fill of the `width` × `height` rectangle at (`x`, `y`)
of a linear surface whose pixel (0, 0) is at `dest`, `stride` bytes per line, with colour
`colour` (`x` and `y` default to 0, a whole-surface fill):

| param | value |
|-------|-------|
| 0 | `colour` (pack mode 0 stores the `o0` dword unchanged) |
| 1 | PBE word 0: `x \| y << 16` (`XMIN`, `YMIN`) |
| 2 | PBE word 1: `(x + width − 1) \| (y + height − 1) << 16` (`XMAX`, `YMAX`) |
| 3 | PBE word 2: `0x8000 \| (stride / 8 − 1) << 16` (`COUNT` `0x80`, `LINESTRIDE`) |
| 4 | PBE word 3: `dest` |
| 5 | sideband: `0x200` (`TILERELATIVE`, pack mode 0) |

Accepted: `width, height ≥ 1` with `x + width` and `y + height` at most 8192 (13-bit
`XMAX`/`YMAX`), `stride` a multiple of 8 (an even pixel stride) with `4 × (x + width) ≤ stride
≤ 0x8000` (12-bit `LINESTRIDE`), `dest` a multiple of 4, and the surface below 4 GiB. The
driver's render box and background object must cover the same rectangle.

### 9.7 Verification

`tools/fwcutter/test_fwload.py` cuts both libraries, places the templates at the addresses of
transfer §4.2.7 and checks every word given there; with the same layout it disassembles the
patched programs (`refdisasm545`, `pds-dis`) and compares the text, e.g. the fill shader for
`0xff336699`:

```
phas #0, #0, pixel, end, parallel
mov.skipinv.end o0, #0xFF336699
```

`test_fwcutter.py` checks the seven hashes and that a `libsrv_um` with another SHA-256 is
refused. `test_cloader.py` runs the C port on the real v2 file with random layouts and fill
parameters and requires the same images and command words as `fwload.py`.

### 9.8 Differences from the transfer §4.3 proposal

- One `TQ_PATCHES` section (`arg` 0) with the template id in each record's `buffer`, like v1
  `PATCHES`, instead of one section per template.
- No `DEST_REG` encoding: the destination registers are `CONST` `WORD32` patches written
  before the immediates, since `USSE_LIMM` keeps every non-immediate bit.
- No `PDS_PTR` kind: no template holds a PDS state pointer; the background object, which
  does, is driver code. `DOUTU` takes the temporaries from `TQ_META`, not from `param`.
- The fixed command words are a `VALUES` list rather than part of `TQ_META`; `ADDR` names
  templates in transfer-queue records. Tile size, PBE `COUNT` and the state-size word stay in
  driver code with the formats they belong to (§9.6).


## 10. Format v3: the copy templates

A linear ARGB8888 copy (`design/sgx545_transfer.md` §4.4) runs seven programs: five of the
fill's (`USE_SEC`, `USE_EOT`, `USE_EOR`, `PDS_SEC`, `PDS_PIXEVENT`) and two of its own, the
copy shader and a PDS primary that fetches the texel. v3 carries those two the §9 way. It adds
no section type, kind or encoding; it raises two limits that v2 loaders enforce, so it needs
its own version. This settles the proposal of transfer §4.4.6; §10.7 lists where it differs.

### 10.1 Versions and limits

| version | content | template ids | parameters |
|---------|---------|--------------|------------|
| 1 | §2–§5 | — | — |
| 2 | + sections 6–8, `VALUES` list 4 (§9) | 0–7 | 0–5 |
| 3 | the same sections | 0–15 | 0–8 |

- The writer picks the **lowest version that holds the content**: 1 without transfer-queue
  content; 2 when every template id (in `TQ_META`, `TQ_TEMPLATE` and `TQ_PATCHES`) is below 8
  and every `PARAM` target (in `TQ_PATCHES` and `TQ_CMD`) below 6; 3 otherwise. A cut from
  both libraries holds template 8 and parameters 6–8, so it is v3.
- `fwcutter.py cut <libsrv_init.so> <libsrv_um.so> --format-version 2` leaves out templates
  7 and 8 and their patches, which gives the v2 file of §9 byte for byte: for a loader that
  predates v3, which rejects a v3 file on its version.
- A v3 loader reads all three versions and applies the limits of the file's version: a v2 file
  with template 8 or parameter 6 is rejected, like a v1 file with transfer-queue content. A v3
  file without transfer-queue content is valid (the cutter never writes one).

### 10.2 Templates

| id | name | pieces | size | SHA-256 |
|----|------|--------|------|---------|
| 7 | `USE_COPY` | `0x21d60`+16 → 0 | `0x10` | `9d0dfd83…5a78b297` |
| 8 | `PDS_PRIM_1SRC` | `0x22960`+64 → 0 | `0x40` | `e3f3a906…13da3758` |

`USE_COPY` is USE resource 0 of `SGXCreateTransferContext`, the default case of its switch
(table base `0x21d60`, offset 0; the same 16 bytes also sit at `0x223a8`). `PDS_PRIM_1SRC` is
the `single_source` PDS primary, copied 16 times into the context's `"PDS - 1src"` ring at
`0x11bdc`. Disassembled (`refdisasm545`; `pds-dis` and `refpds545` agree), the cut templates
read:

```
USE_COPY       phas #0, #0, pixel, end, parallel; mov.skipinv.end o0, pa0
PDS_PRIM_1SRC  12 data dwords (0); movs doutu, ds0[0], ds0[1], ds1[0];
               movs douti, ds0[2], ds0[3]; movs doutt, ds0[4], ds0[5], ds1[2], ds1[3]; halt
```

### 10.3 Placing a subset of the templates

The copy's two templates take the slots of the fill's `USE_FILL` and `PDS_PRIM` in the
transfer recipes (transfer §4.2.7 and §4.4.5 both put a USE program at the PIXELSHADER base
and a PDS primary at the PDSPIXEL base). So a v3 loader does not require every template to be
placed. This applies to v2 files too:

- The driver places the templates it will run. The loader checks each **placed** template
  (aligned, in its heap, within a page with `NO_STRADDLE`), copies it and applies the
  `TQ_PATCHES` records that target it. Templates without an address are left out, with their
  patches.
- A patch into a placed template, or a command word, that names (`ADDR`, `DOUTU`) a template
  without an address is an error.
- Each operation names the templates it runs, and they must be placed if the file has them:
  a fill runs ids 0–6, a copy runs ids 1, 2, 3, 5, 6, 7 and 8 (`fwload.FILL_TEMPLATES`,
  `COPY_TEMPLATES`; `SGX_FW_TQ_FILL_SET`, `SGX_FW_TQ_COPY_SET` with `sgx_fw_tq_need()` in C).
  A copy also needs templates 7 and 8 in the file, i.e. a v3 file.

A v2 driver that places all seven fill templates is unaffected. The one-slot rule of transfer
§4.4.7 item 6 holds for the shared slots as well: a fill and a copy that reuse the same memory
must not be in flight together, which the EDM's one-transfer-at-a-time queue guarantees once
the earlier one has completed.

### 10.4 Records, patches and the parameters of a copy

| id | heap | temps | attrs | size | align | data | flags |
|----|------|-------|-------|------|-------|------|-------|
| 7 `USE_COPY` | 4 | 0 | 0 | `0x10` | 8 | 0 | 1 |
| 8 `PDS_PRIM_1SRC` | 6 | 0 | 1 | `0x40` | 16 | `0x30` | 1 |

(`0x1194a`–`0x11994`: size `0x10`, alignment 8; `0x11b50`–`0x11ba9`: data size `0x30`, one
attribute, alignment 16.) The copy shader is never patched. The PDS primary takes nine
`WORD32` records, all of existing kinds; with the §13.8 interleave its data dwords 0–11 are
`ds0[0]`, `ds0[1]`, `ds1[0]`, `ds1[1]`, `ds0[2]`, `ds0[3]`, `ds1[2]`, `ds1[3]`, `ds0[4]`,
`ds0[5]`, `ds1[4]`, `ds1[5]` (written by `0x1cb14`–`0x1cb47`):

| byte | value | what |
|------|-------|------|
| 0, 4, 8 | `DOUTU` `USE_COPY`, `CONST` `0x02000004`, `CONST` 0 | `U0`–`U2` |
| 16, 20 | `CONST` `0x40012001`, `CONST` 0 | `DOUTI` words 0, 1: TC0, no USE issue, last |
| 24, 28 | `PARAM` 8, `CONST` `0xa0800000` | T2 (`TEXADDR`), T3 |
| 32, 36 | `PARAM` 6, `PARAM` 7 | T0, T1 |

So `doutt` reads T0, T1, T2, T3 from dwords 8, 9, 6, 7. The command words are the fill's
(`VALUES` list 4, §9.5) unchanged; only `ISPBgObjTag` differs (`TSPDATASIZE`, transfer §4.4.4),
and the driver writes that field itself.

The parameters come from the copy in driver code (`fwload.copy_params()`,
`sgx_fw_tq_copy_params()`). The **source** is the linear `width` × `height` texture whose first
texel is at `src`, `src_stride` bytes per line: to copy a sub-rectangle of a larger surface,
`src` is that rectangle's top-left pixel. It lands 1:1 on the `width` × `height` rectangle at
(`x`, `y`) of the **destination**, a linear surface whose pixel (0, 0) is at `dst`,
`dst_stride` bytes per line:

| param | value |
|-------|-------|
| 0 | 0 (unused) |
| 1–5 | the PBE words and sideband of the destination rectangle, as `fill_params()` (§9.6) |
| 6 | T0: `s = src_stride / 4 − 1`; `(s & 7) << 9 \| (s >> 3) << 17` (`STRIDELO`, `STRIDEHI`) |
| 7 | T1: `0xcc000000 \| (width − 1) << 12 \| (height − 1)` (`NP2_STRIDE_EXT`, `B8G8R8A8`) |
| 8 | T2: `src` |

Accepted: `1 ≤ width, height ≤ 4096` (12-bit `WIDTH`/`HEIGHT`, no extension bits), `src_stride`
a multiple of 4 with `4 × width ≤ src_stride ≤ 0x8000` (13-bit stride in dwords), `src` a
multiple of 4 and the source below 4 GiB, and the destination as for a fill (§9.6). Source and
destination must not overlap: the command's scan order is fixed top-left to bottom-right.
Because the texture is exactly the copied rectangle, the background object's texture
coordinates are always (0, 0), (1.0, 0), (0, 1.0), exact for every size (the closed driver
instead normalises a sub-rectangle to the whole source surface, truncated to 20 fraction
bits, transfer §4.4.2). The background object stays driver code, as for the fill.

### 10.5 The reference API

```python
fwload.copy_params(src, dst, width, height, src_stride, dst_stride, x=0, y=0) -> list[int]
fwload.copy(fw, layout, src, dst, width, height, src_stride, dst_stride, x=0, y=0) -> TQBuild
```

`copy()` checks that the file has the copy templates (`FormatError` otherwise) and that the
`COPY_TEMPLATES` are placed, then returns the nine parameters, the patched programs of every
placed template by id, and the command words. `fill()` is unchanged, but its parameter list
now has nine entries (6–8 are 0). `fwload.py --copy src,dst,w,h,src_stride,dst_stride[,x,y]`
is the command-line form. The C equivalents are `struct sgx_fw_tq_copy`,
`sgx_fw_tq_copy_params()` and `sgx_fw_tq_need()`, with `sgx_fw_tq_build()` and
`sgx_fw_tq_cmd()` unchanged in signature (`kernel/sgx_fw/README.md`).

### 10.6 Verification

`test_fwcutter.py` checks the two new hashes and pieces, that the v3 cut is v3 and that the v2
cut is the v3 cut minus templates 7 and 8. `test_fwload.py` places the templates at the
addresses of transfer §4.4.5 and checks every word given there (the PDS primary, secondary and
pixel-event program, the copy shader, the `DOUTU` words, the EOT, the EOR and the command
words), the T0 cross-checks of §4.4.3, and the disassembly of the cut and the patched copy
programs with `refdisasm545`, `pds-dis` and `refpds545`, including that each `doutu`, `douti`
and `doutt` operand reads the dword its patch wrote. `test_fwformat.py` checks the version
rule and rejects v3 content under v1 or v2 and records past the v3 limits.
`test_cloader.py` runs the C port on the synthetic `tiny_v3()` and on the real v3 file with
random copy-only layouts and random copies, and on the real v2 file, and requires the same
output as `fwload.py`.

### 10.7 Differences from the transfer §4.4.6 proposal

- The writer rule is "the lowest version that holds the content" rather than "v3 when
  templates 7–8 are present": template 7 alone fits v2, parameter 6 alone does not.
- A build places and patches a subset of the templates (§10.3); the proposal did not say how a
  copy's programs could share the fill's slots.
- The source is the copied rectangle itself (`src` = its first texel), so T1 holds the copy's
  size and the coordinates are 0 and 1.0, rather than a sub-rectangle of a surface.
- The cutter keeps a `--format-version 2` cut for loaders that predate v3.


## 11. Format v4: templates of our own

`tools/usse-asm/progs/` holds the nine templates as our own assembly source; assembled, each
equals the cut template bit for bit (same SHA-256, §9.2, §10.2). They are our code, not
Imagination's, so a file that carries them needs no `libsrv_um.so` and must not call them
`CUT`. v4 adds one origin and nothing else.

### 11.1 Versions and the cutter

| version | content | template ids | parameters |
|---------|---------|--------------|------------|
| 4 | v3, and `TQ_TEMPLATE` sections may have origin 2 `OURS` | 0–15 | 0–8 |

- `fwcutter.py cut <libsrv_init.so> --tq-from-source` assembles the nine programs
  (`tools/fwcutter/ours.py`, which runs `usse-asm`/`pds-asm`) and writes them as `OURS`
  templates, with the same `TQ_META`, `TQ_PATCHES` and `VALUES` list 4 as a cut: the sources
  keep each patched field where the template has it. It takes neither `libsrv_um.so` nor
  `--format-version`.
- `OURS` is valid only on `TQ_TEMPLATE` and only from v4. The writer's rule of §10.1 gains a
  first case: a file with an `OURS` template is v4; otherwise v1–v3 as before. A v4 file
  without one is valid (the cutter never writes one).
- A v4 loader reads all four versions; v4 has the v3 limits. A v3 loader rejects a v4 file on
  its version, which says more than an unknown origin would.
- The origin changes nothing in loading: an `OURS` template is placed, copied and patched
  like a `CUT` one. Loaders report it (`fwformat.Firmware.tq_ours`, `sgx_fw_tq.origin`).

### 11.2 Why an origin and not a new section type

The bytes, records and rules are those of §9–§10; only where the bytes come from differs, and
the origin field exists to say exactly that. Keeping the type lets every loader path stay
one path. A template's origin is per section, so a file could mix cut and assembled
templates; the cutter writes all of one kind.

### 11.3 Verification

`test_ours.py` assembles the nine programs and checks their SHA-256 against §9.2 and §10.2
(always: no closed input is needed) and, with `libsrv_um.so`, against the cut bytes.
`test_fwformat.py` checks the version rule and rejects `OURS` before v4 or on another section.
`test_fwcutter.py` checks that the source cut is v4, all `OURS`, with the fact tables of a
cut, and that its fill and copy builds equal the cut file's. `test_cloader.py` runs the C
port on a synthetic `tiny_v4()` and on the real v4 file (cut from `libsrv_init.so` alone)
with the recipe layouts and random fills and copies, and requires `fwload.py`'s output,
origins included.
