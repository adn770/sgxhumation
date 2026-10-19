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

| form | status | encoding (derived) |
|------|--------|--------------------|
| `limm` (`mov r<n>, #imm32`) | ✅ r-bank + unpredicated | `enc[63:54]=0b1111110000`; reg=`[25:21]`; imm=`[20:0]∪([40:36]<<21)∪([49:44]<<26)` |
| `nop` | ✅ | exact `0xF800014000000000` |
| `lapc` | ✅ | exact `0xF800008000000000` |
| `idf drc<n>, bif` | ✅ | `(enc & ~(1<<32)) == 0xF820000000000000`; drc=`[32]` |
| `wdf drc<n>` | ✅ | `(enc & ~(1<<32)) == 0xF920000000000000`; drc=`[32]` |

Regression: **2859/2859 decoded instructions correct, 0 wrong** (of 10752 total — ~26%
coverage).

## Evidence limitation: branch targets

`ba`/`br` are **deferred**. Every branch target in the whole 540 oracle is ≤ 7 (1207 branches,
max target 7), so the target field above bit 2 is never exercised — the oracle cannot pin it.
Decoding `ba` as a 3-bit target would pass this (540-only) regression yet produce *wrong*
disassembly for any real branch ≥ 8 instructions (our 545 ukernel is 6637 instructions). The
DDK carries no `EURASIA_USE*` instruction-encoding macros (those live in the closed `libusc`
compiler), so this needs another source: a corpus with larger targets, the expired patents, or
deriving it from our 545 ukernel's own known control flow once more of the ISA is decoded.

## Next (evidence-driven)

1. **Shared operand framework** — the bank + register-number + immediate + source-operand
   layout used across the ALU/memory classes; unlocks many classes at once. Known starting
   points: predicate `enc[43:41]` (`p0`/`!p0`/`p2` seen), dest bank `enc[52:51]` (`r`=0,`sa`=1),
   indexed dest flagged by `enc[35:26]`.
2. **Integer ALU** (classes 9–13): `and`/`or`/`xor`/`shl`/`shr` with `.test*`/`.chan*` flags.
3. **Memory** (29/30): `ldad`/`stad`/`ldr`/`str` addressing (`[base,+#off]`, `.bpcache`, barrier).
4. Wrap the core as the r2 `RArch` plugin; widen toward the full corpus for upstream.
