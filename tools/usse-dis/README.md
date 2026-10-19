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
`debug/usse/solve_bits.py` is the solver: for a chosen instruction form it finds, per value-bit,
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
| `limm` (`mov r<n>, #imm32`) | ✅ decoded, r-bank + unpredicated | `enc[63:54]=0b1111110000`; reg=`[25:21]`; imm=`[20:0]∪([40:36]<<21)∪([49:44]<<26)` |

Regression: **931/931 decoded instructions correct, 0 wrong** (of 10752 total — ~8% coverage,
by design: only `limm` so far).

## Next (evidence-driven, surfaced by the regression)

1. **Shared operand fields** — solving these unlocks many classes at once:
   - predicate prefix `enc[43:41]` (confirmed: `p0`/`!p0`/`p2`; enumerate the rest)
   - destination bank `enc[52:51]` (confirmed: `r`=0, `sa`=1; find `o`/`pa`/…)
   - indexed-dest addressing flagged by `enc[35:26]` (`sa[il + #n]`)
2. **Class 0x1F (control/special)** sub-opcodes — dominates the ukernel: `wdf`, `idf`, `ba`/`br`
   (+ predicated), `nop`, `smlsi`, `str`/`ldr`, `lapc`.
3. **Integer ALU** (classes 9–13): `and`/`or`/`xor`/`shl`/`shr` with the `.test*` / `.chan*`
   flags and the source-operand bank/number/immediate encoding.
4. **Memory** (29/30): `ldad`/`stad` addressing (`[base,+#off]`, `.bpcache`, barrier reg).
5. Then wrap the core as the r2 `RArch` plugin and widen coverage toward the full corpus for
   upstream.
