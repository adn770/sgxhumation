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

The SGX540 RE project (`reference/sgx540-reversing/ukernel/`) shipped its ukernel/TA/3D/SPM
**binaries plus their disassembly** (`*-disasm`), produced by a hijacked official Imagination
tool. Each line pairs the raw 64-bit encoding with its mnemonic — ~3600 unique instructions.
Our SGX545 ukernel shares the same DDK source (opening instructions are byte-identical), so the
540 corpus is both an ISA reference *and* a direct validation set.

**Every field layout in `usse.c` is derived from this oracle by bit-correlation, not guessed.**
`solve.py` is the solver: for a chosen instruction form it finds, per value-bit,
the encoding bit(s) that match across all samples. `oracle_check.py` is the regression: it runs
`usse-dis` over each 540 binary and compares to the oracle. **Invariant: a line we claim to
decode must equal the oracle — `WRONG` must stay 0.** We'd rather emit `; TODO` than a plausible
lie.

## Build & test

```sh
make            # builds ./usse-dis
make check      # runs oracle_check.py (regression vs the SGX540 disassembly)
./usse-dis ../../harness/ukernel-extract/extracted/sgx545_ukernel_template.bin | less
```

## Status

Decoded groups (each a `decode_*` in the `DECODERS[]` table; many derived by a parallel
multi-agent RE fan-out, each self-verified against the oracle):

- `mov`/`limm` (r-bank), `nop`, `lapc`, `idf`, `wdf`
- integer ALU: `and`/`or`/`xor`/`isub16`/`isubu16`/`iaddu32` (incl. `.test*.chan0` predicate-set)
- shifts: `shl`/`shr`/`rol`
- integer mul: `imae`/`ima16`
- class-5 `mov`/`mov.end`
- memory: `ldad`/`ldaw`, `stad`/`staw` (540 addressing forms)
- control: `str`/`ldr`/`smlsi`/`emitpds`
- `fmad` (+ `invalid_mutex_number`)

Regression vs the 540 oracle: **8625/8625 decoded correct, 0 wrong (~80%).**
Our 545 ukernel: **63% decoded** (4235/6637).

## Evidence limitation: branch targets

`ba`/`br` are **deferred**. Every branch target in the whole 540 oracle is ≤ 7 (1207 branches,
max target 7), so the target field above bit 2 is never exercised — the oracle cannot pin it.
Decoding `ba` as a 3-bit target would pass this (540-only) regression yet produce *wrong*
disassembly for any real branch ≥ 8 instructions (our 545 ukernel is 6637 instructions). The
DDK carries no `EURASIA_USE*` instruction-encoding macros (those live in the closed `libusc`
compiler), so this needs another source: a corpus with larger targets, the expired patents, or
deriving it from our 545 ukernel's own known control flow once more of the ISA is decoded.

## Next (evidence-driven)

Remaining 545-ukernel gaps and their blockers:

1. **Oracle-backed extensions** (verifiable now): predicated/`sa`/`o`/`pa`-bank/indexed `limm`;
   deferred ALU predicate/bank forms; class-31 `mov`-special (`pclink`), `lock`/`release`.
2. **SGX545 `ldad`/`stad` bit-33 flag** — the ukernel's memory ops set `enc[33]`, which is 0 in
   all 540 samples (and not the offset field, `enc[13:7]`). Needs evidence beyond the 540 oracle
   (DDK load/store flag macros, patents) to know its mnemonic effect — else it stays deferred.
3. **`ba`/`br` target field** — under-determined by the oracle (all targets ≤ 7); needs patents /
   gma500 / DDK or an airtight self-consistency proof over the 545 ukernel's control flow.
4. **class 26** — an opcode with zero oracle occurrences; no evidence yet.
5. Wrap the core as the r2 `RArch` plugin; widen toward the full corpus for upstream.
