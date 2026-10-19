# usse-dis — SGX Series-5 USSE disassembler

A from-scratch disassembler for the PowerVR SGX **USSE** instruction set (Series-5 / SGX5xx),
built to read the extracted SGX545 microkernel (`harness/ukernel-extract/extracted/`) and,
longer term, to ship as a **radare2 architecture plugin** (no PowerVR/USSE arch exists in r2
today — this is intended to be upstreamable).

## Design

- **C decoder core** (`usse.c` / `usse.h`): pure `decode(enc) -> {text}`, no radare2 dependency,
  so it can be regression-tested standalone and later wrapped as an r2 `RArch` plugin from the
  *same* source (one source of truth).
- **CLI** (`main.c` → `usse-dis`): decodes a raw little-endian USSE blob, one 64-bit word per
  line, in a format that lines up with the SGX540 oracle for diffing.
- **Scope**: *narrow* — implement the opcode subset the microkernel actually uses first, grown
  opcode-by-opcode. The opcode space (major class = `enc[63:59]`) is fully enumerated from the
  oracle; see the table in `usse.c`.

## The oracle (ground truth)

Two tiers, in the order we found them:

1. **Bootstrap** — the SGX540 RE project (`reference/sgx540-reversing/`) shipped ukernel/TA/3D/SPM
   binaries + disassembly text. We bootstrapped the decoder by bit-correlating field layouts
   against it (`solve.py`). It has gaps, though: it duplicates each dump and **truncates branch
   targets** (every branch printed ≤ 7), which made branches look undecidable.
2. **Authoritative** — `refdisasm/` builds **Imagination's own `ref_usse_disasm`** (from the PVR_PSP2
   source, SGX545 config) into a reference disassembler. It fully disassembles our extracted 545
   microkernel (6637/6637, 0 failures) with *correct* 20-bit branch targets. This is now the
   regression oracle.

**`oracle_check.py`** disassembles each binary with both `refdisasm` (truth) and our `usse-dis`,
and compares per instruction. **Invariant: a line we claim to decode must equal refdisasm —
`WRONG` must stay 0.** We'd rather emit `; TODO` than a plausible lie. The clean-room decoder is
thus validated against authoritative output without copying Imagination's source (keeping the
eventual r2 plugin clean-room).

## Build & test

```sh
refdisasm/build.sh   # build the authoritative reference disassembler (needs reference/PVR_PSP2)
make                 # builds ./usse-dis
make check           # oracle_check.py: usse-dis vs refdisasm (545 ukernel + 540 corpus)
./usse-dis ../../harness/ukernel-extract/extracted/sgx545_ukernel_template.bin | less
```

## Status

`DECODERS[]` groups (bootstrapped via a parallel multi-agent RE fan-out, then completed against
the authoritative refdisasm): `mov`/`limm` (all banks/predicated/indexed/`pclink`), `nop`,
`lapc`, `idf`, `wdf`, `ba`/`br` (+`.savelink`, 20-bit targets), integer ALU
(`and`/`or`/`xor`/`isub16`/`isubu16`/`iaddu32`, incl. `.test*.chan0`), shifts (`shl`/`shr`/`rol`),
mul (`imae`/`ima16`), class-5 `mov`/`mov.end`, memory (`ldad`/`ldaw`/`stad`/`staw` incl. `.fcfill`
and `#n++`), control (`str`/`ldr`/`smlsi`/`emitpds`), class-26 `ima32`, `lock`.

Validated vs authoritative refdisasm — **0 wrong**:
- **545 microkernel: 6613/6637 decoded (99.6%).**
- 540 corpus (ukernel+TA+3D+SPM): 17353/17389 (99%).

Remaining ~0.4% deferred (long tail): `fmad` (+ zero-padding), `phase`, cache `ci`/`cfi`, and a
few operand corners.

## Next

1. Close the ~0.4% long tail (`fmad` operand syntax, `phase`, `ci`/`cfi`) against refdisasm.
2. Wrap the core as the radare2 `RArch` plugin (the clean-room decoder is the upstreamable
   artifact; refdisasm stays a local reference-only oracle).
3. Widen toward the full 540 corpus / shader code for broader upstream coverage.
