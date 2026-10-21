# sgx_fw: the SGX545 firmware loader in C

A C port of `tools/fwcutter/fwload.py` (and the parsing half of `fwformat.py`): it reads
`sgx545_ukernel.fw` (formats v1 to v4, `design/sgx545_fw_format.md`) and, for the device
addresses a driver chose, produces the patched buffer images, the init scripts, `asInitDevData`
and the EDM metadata. From a v2 file it also builds the seven transfer-queue programs of a solid
fill and its fixed command words (format §9), and from a v3 file those of a copy (§10). The
Python loader is the reference; this code must behave the same.

| file | what |
|------|------|
| `sgx_fw.h`, `sgx_fw.c` | the loader core: freestanding, no allocation, kernel coding style |
| `sgx_fw_types.h` | maps `u8`..`u64` and errno codes to `<linux/*>` or to the C library |
| `sgx_fw_test.c` | KUnit suite: the synthetic cases (expression kinds, header, layout, v2, v3) |
| `fwdump.c`, `Makefile` | userspace harness: `fwdump <file.fw> <layout.txt> <outdir>` |

## Using the core

1. `sgx_fw_parse(&fw, data, len)` validates the whole file (magic, version, size, CRC-32,
   section types and origins, every section and patch within bounds, every expression's kind,
   encoding and target) and keeps pointers into `data`, which must outlive `fw`.
2. Fill a `struct sgx_fw_layout`: an address per buffer id, a base per heap id, the EDM page
   directory's physical address, and a bit in `addr_set` / `heap_set` for each entry given.
   `fw.buffers[]` lists the buffers to allocate (heap, size, alignment).
3. `sgx_fw_build(&fw, &layout, bufs)` checks the layout (alignment, ukernel a whole instruction
   above the KERNEL_CODE base, every code word's pc within 19 bits), then fills each
   `bufs[id]` from its image, zero-padded, and applies the patches.
4. `sgx_fw_script()` evaluates init script part 1, part 2 or deinit into `(reg, val)` pairs;
   `sgx_fw_dev_data()` fills the 24 `asInitDevData` words; `fw.meta[]` holds `EDM_META`.

### A solid fill (format v2)

`fw.version` is 2 or 3 and `fw.tq_present` has a bit per template id (`enum sgx_fw_tq_id`);
`sgx_fw_tq(&fw, id)` returns its record (heap, size, alignment, temps, PDS data size and
attributes, `SGX_FW_TQ_NO_STRADDLE`).

```c
int sgx_fw_tq_fill_params(const struct sgx_fw_tq_fill *f,
                          u32 params[SGX_FW_TQ_NUM_PARAMS]);
int sgx_fw_tq_check(const struct sgx_fw *fw, const struct sgx_fw_layout *l);
int sgx_fw_tq_need(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
                   u32 set);
int sgx_fw_tq_build(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
                    const u32 params[SGX_FW_TQ_NUM_PARAMS],
                    u8 *const progs[SGX_FW_TQ_MAX]);
int sgx_fw_tq_cmd(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
                  const u32 params[SGX_FW_TQ_NUM_PARAMS],
                  struct sgx_fw_write *out, unsigned int max);
```

1. Place the templates the fill runs (`SGX_FW_TQ_FILL_SET`): set `layout.tq_addr[id]` and its
   bit in `tq_set`, and the PIXELSHADER and PDSPIXEL bases in `heap_base`. A template must be
   aligned, at or above its heap base and, with `NO_STRADDLE`, inside one 4 KiB page; USE code
   must lie within 512 KiB of its heap base for the `DOUTU` words (`sgx_fw_tq_check()` checks
   the first three for every placed template, the build the last).
   `sgx_fw_tq_need(&fw, &layout, SGX_FW_TQ_FILL_SET)` checks that each of them the file has is
   placed. Place only templates you will upload: a template without an address is not built.
2. `sgx_fw_tq_fill_params()` turns the fill (`dest`, `width`, `height`, `stride` in bytes,
   `colour`, `x`, `y`) into the parameters: the colour and the five PBE state words (6–8 are 0).
3. `sgx_fw_tq_build()` fills each placed template's `progs[id]` (`size` bytes) from its
   template, zero-padded, and applies its patches; `sgx_fw_tq_cmd()` yields the fixed
   `SGXMKIF_TRANSFERCMD` words as `(byte offset, value)` pairs. The background object and the
   other command fields stay with the driver (format §9.5).

### A copy (format v3)

```c
struct sgx_fw_tq_copy {
        u32 src;          /* first texel of the source rectangle */
        u32 dst;          /* pixel (0, 0) of the destination surface */
        u32 width, height;          /* 1..4096 */
        u32 src_stride, dst_stride; /* bytes per line */
        u32 x, y;                   /* destination rectangle */
};
int sgx_fw_tq_copy_params(const struct sgx_fw_tq_copy *c,
                          u32 params[SGX_FW_TQ_NUM_PARAMS]);
```

1. Check that the file has the copy: `sgx_fw_tq(&fw, SGX_FW_TQ_USE_COPY)` and
   `sgx_fw_tq(&fw, SGX_FW_TQ_PDS_PRIM_1SRC)` are non-NULL in a v3 file.
2. Place `SGX_FW_TQ_COPY_SET` (the copy shader and the `single_source` PDS primary instead of
   the fill's shader and primary; they may take the same slots) and call
   `sgx_fw_tq_need(&fw, &layout, SGX_FW_TQ_COPY_SET)`.
3. `sgx_fw_tq_copy_params()` gives the nine parameters: the destination's PBE words (1–5, as
   for a fill) and the texture words T0 (stride), T1 (type, format, size) and T2 (address).
4. `sgx_fw_tq_build()` and `sgx_fw_tq_cmd()` as for a fill; the command words are the same.
   The driver writes the one-layer background object, whose texture coordinates are (0, 0),
   (1.0, 0) and (0, 1.0) because the texture is the copied rectangle, and sets `TSPDATASIZE`
   in `ISPBgObjTag` (`design/sgx545_transfer.md` §4.4.2, §4.4.4).

`SGX_FW_TQ_MAX` is 16 and `SGX_FW_TQ_NUM_PARAMS` 9; the parser holds a v2 file to ids 0–7 and
parameters 0–5. A driver written for v2 that loops over every present template must now skip
the ids it has no slot for (or loop over `SGX_FW_TQ_FILL_SET`), since a v3 file also has the
copy templates. A loader that predates v3 rejects a v3 file on its version: cut with
`--format-version 2` for it.

A v4 file (format §11) is a v3 file whose templates may be our own assembled programs:
`sgx_fw_tq(&fw, id)->origin` is `SGX_FW_ORIGIN_OURS` for those, `SGX_FW_ORIGIN_CUT` otherwise.
Nothing else changes; the v3 limits apply.

`sgx_fw_emitpix_sb_set()` / `_get()` are the `EMITPIX_SB` encoding, like
`sgx_fw_limm_set()` / `_get()` for `USSE_LIMM`.

Errors are negative errno codes: `-EINVAL` for a malformed file or a fill or copy the hardware
cannot express, `-EBADMSG` for a CRC mismatch, `-EFBIG` for an image larger than its buffer or
template, or too many buffers, `-ENOENT` for a layout missing an address or heap the file
needs (a needed template, or one a patch or command word names; or a fill from a file without
templates), `-ERANGE` for a layout that breaks a constraint, `-ENOSPC` for a too-small script
or command array.

## Stricter than the reference

`fwload.py` passes some malformed records through; the C loader rejects them at parse time,
because a kernel must not write past an allocation or guess at an encoding: an image larger than
its buffer or for an undeclared buffer, an unknown encoding (Python stores it as `WORD32`), a
duplicate buffer id, a `VALUES` list id above 3, a `CODE_BASE` data master above 7 bits.
Everything fwload rejects only while loading (a patch past its buffer, an unknown kind, a
dev-data index past 23) the C loader rejects in `sgx_fw_parse()`. For v2 content there is no
difference: `fwformat.unpack()` already checks every template, patch and command word.

## Tests

`tools/fwcutter/test_cloader.py` builds `fwdump` with the local `cc` and compares it with
`fwload.py`: the synthetic firmware, every damaged file and bad layout of the Python tests, and,
with the user's `libsrv_init.so` (cut at test time, skipped without it), both harness layouts
and 50 random valid layouts. For v2, `layout.txt` also takes `tq <id> <hex>` lines and a
`fill <dest> <width> <height> <stride> <colour>` line (hex); fwdump then writes `tqNN.bin` and
prints the parameters, template records and command words; a `copy <src> <dst> <width>
<height> <src_stride> <dst_stride> [<x> <y>]` line does the same for a copy, and only placed
templates are written. The v2 cases are the synthetic `tiny_v2()`, every damaged v2 file, bad
template layout and bad fill, and, with `libsrv_um.so` as well, the transfer recipe's placement
plus 50 random layouts and fills. The v3 cases are the synthetic `tiny_v3()` (a copy, and a
fill that leaves the copy templates out), every damaged v3 file, bad copy and unplaced
template, 50 random layouts and copies, and, on the real file, the copy recipe's placement, 50
random copy-only layouts and copies, and the real v2 file. The v4 cases are the synthetic
`tiny_v4()` (`OURS` templates; `fwdump` prints each template's origin), every damaged v4 file,
and the real v4 file cut with `--tq-from-source`, at the recipe layouts and with 50 random
fills and copies. Run it with the rest of the
fwcutter suite:

```
.venv/bin/pytest tools/fwcutter
```

`sgx_fw_test.c` needs a kernel tree with `CONFIG_KUNIT`; it is not built here (it builds
cleanly as part of `gma500_gfx` with `W=1`).

## Into gma500

The core files go into `drivers/gpu/drm/gma500/` unchanged: `sgx_fw.c`, `sgx_fw.h`,
`sgx_fw_types.h` (it picks `<linux/types.h>` and `<linux/errno.h>` under `__KERNEL__`).
Add `sgx_fw.o` to `gma500_gfx-y` in the gma500 `Makefile`. The KUnit suite goes in as
`sgx_fw_test.c` behind a `DRM_GMA500_KUNIT_TEST` Kconfig symbol (`depends on DRM_GMA500 &&
KUNIT`, `default KUNIT_ALL_TESTS`). If the suite is built as its own module, the core needs
`EXPORT_SYMBOL_IF_KUNIT()` on the functions it calls; built into `gma500_gfx`, it does not.
The driver side (M2) allocates `fw.buffers[]`, maps them with `psb_mmu_insert_pages()`, gets
the file with `request_firmware("sgx545_ukernel.fw")` and calls the core as above.
