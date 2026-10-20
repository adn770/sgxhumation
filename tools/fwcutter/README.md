# sgx545-fwcutter

Cuts the SGX545 firmware out of your own copy of the closed Cedarview `libsrv_init.so` and
writes `sgx545_ukernel.fw`, the single file a future kernel driver loads with
`request_firmware()`. It works the way b43-fwcutter does: the tool contains offsets, sizes,
hashes and our own reconstructed tables, and no Imagination bytes. You supply the library.

The format is specified in `design/sgx545_fw_format.md`.

```sh
python3 fwcutter.py cut path/to/libsrv_init.so.1.7.862890 -o sgx545_ukernel.fw
python3 fwcutter.py info sgx545_ukernel.fw
python3 fwload.py sgx545_ukernel.fw layout.json -o images/   # reference loader
```

The cut is static, so no closed code runs. Python 3.10+, standard library only. Unknown
inputs are refused by SHA-256. The supported input is listed in `facts.KNOWN_INPUTS`.

| file | role |
|------|------|
| `facts.py` | known inputs and every reconstructed table (the FACT side) |
| `fwformat.py` | the container: pack / unpack, origin tags, CRC |
| `fwcutter.py` | identify the input, carve and hash-check the code images, write the file |
| `fwload.py` | reference loader: device addresses in, patched images and init scripts out |

## Tests

```sh
../../.venv/bin/pytest -q
```

The cut and loader tests need the library. They look for `$SGX545_LIBSRV_INIT`, then for the
copy in the reference corpus, and skip if neither is present. The loader is checked against
two harness captures in `harness/ukernel-extract/extracted/layout_{base,moved}/` (gitignored;
`harness/ukernel-extract/README.md` describes how to take them). With each capture's
addresses, it must reproduce all 15 buffers and the init info exactly.

`*.fw` output is gitignored: like `extracted/`, it contains Imagination code.
