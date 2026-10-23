# sgx545-fwcutter

Cuts the SGX545 firmware out of your own copy of the closed Cedarview `libsrv_init.so` and
writes `sgx545_ukernel.fw`, the single file a future kernel driver loads with
`request_firmware()`. Given `libsrv_um.so` as well, it also carries the transfer-queue
programs of a solid fill and of a copy (format v3). With `--tq-from-source` instead, those
programs are our own (`asm/transfer/`, assembled), so 2D needs only `libsrv_init.so`
(format v4). It works the way b43-fwcutter does: the tool contains offsets, sizes, hashes
and our own reconstructed tables and programs, and no Imagination bytes. You supply the
libraries, or `get-firmware.sh` fetches them for you (below).

The format is specified in `design/sgx545_fw_format.md`.

## The input

The libraries are part of Intel's "Linux PowerVR Graphics/Media Drivers" for Cedarview
(DDK 1.7.862890). Two builds of them were published, and fwcutter knows both:

| package | SHA-256 | published by |
|---------|---------|--------------|
| `cedarview-graphics-drivers_20120717.orig.tar.gz` | `c23e7261…0da8fd83` | Ubuntu 12.04 |
| `cdv-gfx-drivers-1.0.3_bee.tar.bz2` | `2e662b25…95c0317a` | Intel (Download Center #21938) |

Ubuntu shipped its package in multiverse, and old-releases.ubuntu.com still serves it from
`http://old-releases.ubuntu.com/ubuntu/pool/multiverse/c/cedarview-graphics-drivers/`. Intel
withdrew its download in 2019, but the file survives in the archive.org item
`PowerVR-Graphics-Media-Drivers-for-Linux-Intel-GMA3600-3650-GPU` and in the Wayback Machine
(snapshot 20190902150937 of `https://downloadmirror.intel.com/21938/eng/`).

The libraries are `usr/lib/libsrv_{init,um}.so.1.7.862890`: at the top of the Ubuntu
tarball, and inside `bin/cedarview-userspace-v1.0.3_bee.tar` in Intel's. The two builds are
different links of the same code, so they cut the same file, byte for byte (format §7).

Both packages carry the same licence, the "User Space Graphics Driver Binary License"
(`usr/share/doc/powervr/license.txt`). It allows redistribution only of the unmodified
software, with the licence. A cut `sgx545_ukernel.fw` is a modification, so it may be used
but not redistributed. That is why this project ships neither the package nor the cut file
(`*.fw` is gitignored), and why you cut your own, the way Debian's `firmware-b43-installer`
does for b43.

### get-firmware.sh

```sh
./get-firmware.sh                     # asks you to accept the licence; writes ./sgx545_ukernel.fw
./get-firmware.sh --accept-license -o /lib/firmware   # non-interactive, another directory
./get-firmware.sh --cache-dir ~/.cache/sgx545 ...     # keep the package for next time
./get-firmware.sh --init-only ...     # libsrv_init.so alone: format v1
./get-firmware.sh --list-sources      # the URLs and SHA-256s, for fetching by hand
```

The script tries old-releases.ubuntu.com, then archive.org, then the Wayback Machine, with
`curl` or `wget`. It checks the SHA-256 of the package and of each library it extracts, shows
the licence, and asks you to accept it (`--accept-license` accepts it without asking). Then
it runs `fwcutter.py cut` with both libraries (format v3) and prints the result's SHA-256.
The download goes to a temporary directory that is removed on exit, or with `--cache-dir` to
a directory of your choosing, which must be outside this repository. It needs a POSIX shell,
`tar` with gzip and bzip2 support, `sha256sum` or `shasum`, and Python 3.10+.

At this revision, `sgx545_ukernel.fw` (format v3) has SHA-256
`6a00268e9d4ca552c02e52651a42a2b4db3887d7fe626f209888369985162b0a`, from either package. With
`--init-only` (format v1) it has SHA-256
`da4507d56e90fb03cdb150a9b24436844a271748ecac36d6ed361ce8513c8524`. A later change to the
fact tables or the format changes these hashes.

### By hand

1. Download one of the packages from the URLs above (`./get-firmware.sh --list-sources`
   prints them with their SHA-256s), and check its SHA-256.
2. Extract the libraries and the licence, and read the licence:

   ```sh
   # Ubuntu
   tar -xzf cedarview-graphics-drivers_20120717.orig.tar.gz ./usr/lib/libsrv_init.so.1.7.862890 \
       ./usr/lib/libsrv_um.so.1.7.862890 ./usr/share/doc/powervr/license.txt
   # Intel
   tar -xjf cdv-gfx-drivers-1.0.3_bee.tar.bz2 \
       cdv-gfx-drivers-1.0.3_bee/bin/cedarview-userspace-v1.0.3_bee.tar
   tar -xf cdv-gfx-drivers-1.0.3_bee/bin/cedarview-userspace-v1.0.3_bee.tar \
       ./usr/lib/libsrv_init.so.1.7.862890 ./usr/lib/libsrv_um.so.1.7.862890 \
       ./usr/share/doc/powervr/license.txt
   ```

3. Run `python3 fwcutter.py cut usr/lib/libsrv_init.so.1.7.862890
   usr/lib/libsrv_um.so.1.7.862890 -o sgx545_ukernel.fw`. fwcutter refuses a library whose
   SHA-256 it does not know.

## Using fwcutter

```sh
python3 fwcutter.py cut path/to/libsrv_init.so.1.7.862890 -o sgx545_ukernel.fw   # v1
python3 fwcutter.py cut path/to/libsrv_init.so.1.7.862890 path/to/libsrv_um.so.1.7.862890 \
        -o sgx545_ukernel.fw                                                      # v3
python3 fwcutter.py cut ... --format-version 2 -o sgx545_ukernel.fw   # v2: no copy templates
python3 fwcutter.py cut path/to/libsrv_init.so.1.7.862890 --tq-from-source \
        -o sgx545_ukernel.fw                                       # v4: our own templates
python3 fwcutter.py info sgx545_ukernel.fw
python3 fwload.py sgx545_ukernel.fw layout.json -o images/   # reference loader
python3 fwload.py sgx545_ukernel.fw layout.json --fill 0xf0200000,64,32,256,0xff336699 -o tq/
python3 fwload.py sgx545_ukernel.fw layout.json --copy 0xf0300000,0xf0200000,64,32,256,256 -o tq/
```

The cut is static, so no closed code runs. Python 3.10+, standard library only. Unknown
inputs are refused by SHA-256; the supported ones, the Intel and the Ubuntu build of each
library, are listed in `facts.KNOWN_INPUTS`. With one input the file is format v1, byte for
byte as before; with both it is v3, or with
`--format-version 2` the v2 file of a fill alone, byte for byte as before (for a loader that
predates v3). With `--tq-from-source` it is v4: the nine templates are assembled from our
source (`ours.py`; needs a C compiler for `tools/usse-asm`) and tagged `OURS`, with the same
fact tables, so every fill and copy builds the same programs as from a cut (format §11). The
writer always picks the lowest version that holds the content (format §10.1, §11.1).

The fill API is `fwload.fill(fw, layout, dest, width, height, stride, colour, x=0, y=0)`: with
the fill's templates (`fwload.FILL_TEMPLATES`) placed in `layout.tq_addr` (and
`layout.heap_base` for PIXELSHADER and PDSPIXEL), it returns the nine parameters, the patched
programs by template id and the fixed `SGXMKIF_TRANSFERCMD` words. `fill_params()` and
`build_tq()` are its two halves.

The copy API (format v3, §10) is

```python
fwload.copy(fw, layout, src, dst, width, height, src_stride, dst_stride, x=0, y=0)
```

It copies the linear ARGB8888 `width` × `height` texture whose first texel is at `src`
(`src_stride` bytes per line; for a sub-rectangle of a surface, its top-left pixel) to the
rectangle at (`x`, `y`) of the destination surface whose pixel (0, 0) is at `dst`
(`dst_stride` bytes per line), at most 4096 pixels a side. With `fwload.COPY_TEMPLATES`
placed, it returns `copy_params()` (the PBE words of the destination and the texture words
T0, T1, T2), the patched programs and the same command words as a fill. Only placed templates
are built, so a copy's `USE_COPY` and `PDS_PRIM_1SRC` may take the fill's slots. The
background object (its texture coordinates are (0, 0), (1, 0), (0, 1)) and `ISPBgObjTag`
stay driver code.

| file | role |
|------|------|
| `facts.py` | known inputs and every reconstructed table (the FACT side) |
| `fwformat.py` | the container: pack / unpack, origin tags, CRC |
| `fwcutter.py` | identify the input, carve and hash-check the code images, write the file |
| `ours.py` | assemble the templates from `asm/transfer/` (the `OURS` origin) |
| `asmmap.py` | read `sgx-asm` output (images + `map.json`) as images and expression records |
| `fwload.py` | reference loader: addresses in, patched images and init scripts out; the fill |
| `check_dump.py` | compare a device dump (debugfs) with the reference loader |
| `get-firmware.sh` | fetch and check a package, ask for the licence, run the cut |

## Tests

```sh
../../.venv/bin/pytest -q
```

The cut and loader tests need the library. They look for `$SGX545_LIBSRV_INIT`, then for the
copy in the reference corpus, and skip if neither is present. The loader is checked against
two harness captures in `harness/ukernel-extract/extracted/layout_{base,moved}/` (gitignored;
`harness/ukernel-extract/README.md` describes how to take them). With each capture's
addresses, it must reproduce all 15 buffers and the init info exactly.

The v2 and v3 tests also need `libsrv_um.so` (`$SGX545_LIBSRV_UM`, then the corpus copy).
They check the nine template hashes, every word of the fill and copy recipes
(`design/sgx545_transfer.md` §4.2.7 and §4.4.5) for their layouts, and the disassembly of the
cut and patched programs: USE code with `tools/usse-dis/refdisasm/refdisasm545` and PDS code
with `refpds545` (skipped unless built) and `tools/usse-dis/pds-dis` (built by the test).
`test_cloader.py` checks the C port in `kernel/sgx_fw/` against all of this, with random
layouts, fills and copies.

`test_ours.py` needs no closed input: the assembled templates must hash like the cut ones
(and, with `libsrv_um.so`, equal them). The v4 cut and its C-loader checks need only
`libsrv_init.so`.

`test_asmmap.py` (needs cargo) assembles `asm/transfer/transfer.s` with `sgx-asm`: its
sections must equal `ours.py`'s templates and its patch records `facts.TQ_PATCHES`, in order.

The Ubuntu build is checked against the Intel one (same cut, byte for byte) when
`$SGX545_UBUNTU_LIBSRV_INIT` and `$SGX545_UBUNTU_LIBSRV_UM` name its libraries; otherwise only
its offsets and hashes are checked.

`test_get_firmware.py` runs `get-firmware.sh` against a local HTTP server that serves
tarballs of dummy text, with the known hashes overridden through the environment and a fake
cutter. It checks the fallback from one source to the next, package and library hash
mismatches, the licence prompt and `--accept-license`, the cache and the cutter call. With
`SGX_NET_TESTS=1` it also downloads the real package from each of the three sources and
checks that the cut has the SHA-256 given above.

`*.fw` output is gitignored: like `extracted/`, it contains Imagination code.
