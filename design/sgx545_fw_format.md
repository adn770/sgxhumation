# SGX545 firmware container (`sgx545_ukernel.fw`), format v1

`tools/fwcutter/` builds this file from the user's own copy of the closed Cedarview
`libsrv_init.so`. A future kernel driver loads it with `request_firmware()`. This is the
b43-fwcutter model: the tool ships no Imagination bytes, only offsets, sizes, hashes and our
own reconstructed tables. The user supplies the blob.

The file carries everything srvinit contributes to an SGX545 boot: the code images, how to
patch them for the device addresses the driver picks, the register init scripts, and the
initial contents of the control blocks. The reference loader (`tools/fwcutter/fwload.py`)
reproduces the harness captures byte for byte from it, at two different address layouts
(§6). That is the evidence that the format is complete.

## 1. Two kinds of content

Every section is tagged with its origin, so the two never mix:

| origin | what | where it comes from |
|--------|------|---------------------|
| `CUT` | the ukernel and the five PDS programs, as stored in `libsrv_init.so` | the user's blob |
| `FACT` | everything else | `tools/fwcutter/facts.py`, our reconstruction (spec §13) |

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
  +8   u16     format_version  1
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
  +2   u16     origin          0 = CUT, 1 = FACT
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

It comes from `cdv-gfx-drivers-1.0.3_bee.tar.bz2` (catalog §5.1): the
`cedarview-userspace-v1.0.3_bee` tarball inside it, `usr/lib/`.

Each cut section is also checked against its own SHA-256, so a future table entry with the
same file hash but different offsets would be caught. Accepting the `.deb` or the tarball
directly is a later addition.

## 8. Out of scope

Kernel-side loading, a C header generated from `facts.py`, and anything on hardware.
