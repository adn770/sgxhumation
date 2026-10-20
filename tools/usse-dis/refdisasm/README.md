# refdisasm — authoritative USSE disassembler (reference oracle)

`ref_usse_disasm` is Imagination's own USSE disassembler, shipped as source in the PVR_PSP2 driver
(`reference/PVR_PSP2/tools/intern/useasm/`). `main.c` here wraps it to disassemble a raw
little-endian USSE blob; `build.sh` compiles it for SGX545 and SGX540.

This is **ground truth**: it fully disassembles our extracted SGX545 microkernel (6637/6637
instructions, 0 failures) and serves as the **regression oracle** for the clean-room `usse-dis`
decoder one directory up (`oracle_check.py` diffs usse-dis against refdisasm output).

Only `main.c`, `pdsmain.c`, `build.sh` and this README are tracked. The PVR sources stay in
`reference/` (Imagination copyright — reference only, never vendored); the built binaries and
any generated disassembly are gitignored and regenerated via `build.sh`.

```sh
./build.sh                                   # -> refdisasm545, refdisasm540, refpds545
./refdisasm545 ../../../harness/ukernel-extract/extracted/sgx545_ukernel_template.bin
```

Notes (from the build RE): configure the core at compile time — `-DSGX545 -DSUPPORT_SGX545
-DSGX_CORE_REV=1014 -DSGX_BUILD_CORE_ID=9` (545) vs `-DSGX540 … 130 … 5` (540); these are
mutually exclusive. `use_disassemble_instruction()`'s return is the END flag, *not* decode
success — a failed decode writes an `invalid_*`/`unknown_error` string instead. Branch targets
are printed as the raw 20-bit (`feature_use_number_pc_bits`) PC field — unlike the
sgx540-reversing text dumps, which truncated/rebased them.

`refpds545` (also from `build.sh`) wraps PVR_PSP2's PDS disassembler
(`codegen/pdsasm/pdsdisasm.c`): `./refpds545 <program.bin> <data_dwords>` prints the data
segment as constants and disassembles the 32-bit PDS instructions after it. The EDM is
entered through such programs; `harness/ukernel-extract/pdsprog.py` decodes their data banks
and USSE entry words (spec §13.8). The clean-room `../pds-dis` is checked against
it by `../pds_oracle_check.py`.
