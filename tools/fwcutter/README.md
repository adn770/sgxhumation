# sgx545-fwcutter

Cuts the SGX545 firmware out of your own copy of the closed Cedarview `libsrv_init.so` and
writes `sgx545_ukernel.fw`, the single file a future kernel driver loads with
`request_firmware()`. Given `libsrv_um.so` as well, it also carries the transfer-queue
programs of a solid fill (format v2). It works the way b43-fwcutter does: the tool contains
offsets, sizes, hashes and our own reconstructed tables, and no Imagination bytes. You supply
the libraries.

The format is specified in `design/sgx545_fw_format.md`.

```sh
python3 fwcutter.py cut path/to/libsrv_init.so.1.7.862890 -o sgx545_ukernel.fw   # v1
python3 fwcutter.py cut path/to/libsrv_init.so.1.7.862890 path/to/libsrv_um.so.1.7.862890 \
        -o sgx545_ukernel.fw                                                      # v2
python3 fwcutter.py info sgx545_ukernel.fw
python3 fwload.py sgx545_ukernel.fw layout.json -o images/   # reference loader
python3 fwload.py sgx545_ukernel.fw layout.json --fill 0xf0200000,64,32,256,0xff336699 -o tq/
```

The cut is static, so no closed code runs. Python 3.10+, standard library only. Unknown
inputs are refused by SHA-256; the supported ones are listed in `facts.KNOWN_INPUTS`. With
one input the file is format v1, byte for byte as before; with both it is v2.

The fill API is `fwload.fill(fw, layout, dest, width, height, stride, colour)`: with the
templates placed in `layout.tq_addr` (and `layout.heap_base` for PIXELSHADER and PDSPIXEL),
it returns the six parameters, the seven patched programs by template id and the fixed
`SGXMKIF_TRANSFERCMD` words. `fill_params()` and `build_tq()` are its two halves.

| file | role |
|------|------|
| `facts.py` | known inputs and every reconstructed table (the FACT side) |
| `fwformat.py` | the container: pack / unpack, origin tags, CRC |
| `fwcutter.py` | identify the input, carve and hash-check the code images, write the file |
| `fwload.py` | reference loader: addresses in, patched images and init scripts out; the fill |
| `check_dump.py` | compare a device dump (debugfs) with the reference loader |

## Tests

```sh
../../.venv/bin/pytest -q
```

The cut and loader tests need the library. They look for `$SGX545_LIBSRV_INIT`, then for the
copy in the reference corpus, and skip if neither is present. The loader is checked against
two harness captures in `harness/ukernel-extract/extracted/layout_{base,moved}/` (gitignored;
`harness/ukernel-extract/README.md` describes how to take them). With each capture's
addresses, it must reproduce all 15 buffers and the init info exactly.

The v2 tests also need `libsrv_um.so` (`$SGX545_LIBSRV_UM`, then the corpus copy). They check
the seven template hashes, every word of the transfer recipe (`design/sgx545_transfer.md`
§4.2.7) for its layout, and the disassembly of the patched programs: USE code with
`tools/usse-dis/refdisasm/refdisasm545` (skipped unless built), PDS code with
`tools/usse-dis/pds-dis` (built by the test). `test_cloader.py` checks the C port in
`kernel/sgx_fw/` against all of this.

`*.fw` output is gitignored: like `extracted/`, it contains Imagination code.
