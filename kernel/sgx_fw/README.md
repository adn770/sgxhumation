# sgx_fw: the SGX545 firmware loader in C

A C port of `tools/fwcutter/fwload.py` (and the parsing half of `fwformat.py`): it reads
`sgx545_ukernel.fw` (format v1, `design/sgx545_fw_format.md`) and, for the device addresses a
driver chose, produces the patched buffer images, the init scripts, `asInitDevData` and the EDM
metadata. The Python loader is the reference; this code must behave the same.

| file | what |
|------|------|
| `sgx_fw.h`, `sgx_fw.c` | the loader core: freestanding, no allocation, kernel coding style |
| `sgx_fw_types.h` | maps `u8`..`u64` and errno codes to `<linux/*>` or to the C library |
| `sgx_fw_test.c` | KUnit suite: the synthetic cases (expression kinds, header, layout) |
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

Errors are negative errno codes: `-EINVAL` for a malformed file, `-EBADMSG` for a CRC mismatch,
`-EFBIG` for an image larger than its buffer or too many buffers, `-ENOENT` for a layout
missing an address or heap the file needs, `-ERANGE` for a layout that breaks a constraint,
`-ENOSPC` for a too-small script array.

## Stricter than the reference

`fwload.py` passes some malformed records through; the C loader rejects them at parse time,
because a kernel must not write past an allocation or guess at an encoding: an image larger than
its buffer or for an undeclared buffer, an unknown encoding (Python stores it as `WORD32`), a
duplicate buffer id, a `VALUES` list id above 3, a `CODE_BASE` data master above 7 bits.
Everything fwload rejects only while loading (a patch past its buffer, an unknown kind, a
dev-data index past 23) the C loader rejects in `sgx_fw_parse()`.

## Tests

`tools/fwcutter/test_cloader.py` builds `fwdump` with the local `cc` and compares it with
`fwload.py`: the synthetic firmware, every damaged file and bad layout of the Python tests, and,
with the user's `libsrv_init.so` (cut at test time, skipped without it), both harness layouts
and 50 random valid layouts. Run it with the rest of the fwcutter suite:

```
.venv/bin/pytest tools/fwcutter
```

`sgx_fw_test.c` needs a kernel tree with `CONFIG_KUNIT`; it is not built here.

## Into gma500 (milestone M1 → M2)

The core files go into `drivers/gpu/drm/gma500/` unchanged: `sgx_fw.c`, `sgx_fw.h`,
`sgx_fw_types.h` (it picks `<linux/types.h>` and `<linux/errno.h>` under `__KERNEL__`).
Add `sgx_fw.o` to `gma500_gfx-y` in the gma500 `Makefile`. The KUnit suite goes in as
`sgx_fw_test.c` behind a `DRM_GMA500_KUNIT_TEST` Kconfig symbol (`depends on DRM_GMA500 &&
KUNIT`, `default KUNIT_ALL_TESTS`). If the suite is built as its own module, the core needs
`EXPORT_SYMBOL_IF_KUNIT()` on the functions it calls; built into `gma500_gfx`, it does not.
The driver side (M2) allocates `fw.buffers[]`, maps them with `psb_mmu_insert_pages()`, gets
the file with `request_firmware("sgx545_ukernel.fw")` and calls the core as above.
