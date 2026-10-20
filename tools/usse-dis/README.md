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

## radare2 plugin

`r2/` wraps the core as a radare2 `RArch` plugin (`arch usse`, `bits 32`): `make -C r2 install`,
then `radare2 -a usse -b 32 <blob>`. Verified against refdisasm in r2 (6631/6637 of the ukernel;
op types + branch targets wired so jump arrows / function graphs work). See `r2/README.md` for
the upstream path to radareorg/radare2.

## Next

1. Long tail (~0.4%) is deferred by design — the remaining undecoded words are zero-padding
   (shown `invalid`) and the `efo`/`"SrvInit:"` string (data). Generalising `ci`/`cfi`/`phas`
   operand fields (now exact-matched) is optional polish.
2. Open the radare2 PR (plugin is structured for it; `r2/README.md`).
3. Widen toward the full 540 corpus / shader code for broader coverage.

## Load/store addressing — what the immediate means

usse-dis prints the encoded offset field of `ldad`/`ldaw`/`stad`/`staw`, exactly as refdisasm545
does (`test_ldst.py` pins this; `oracle_check.py` finds 0 differences on every fetch form). The
text is right. What it means depends on the form. Offsets count elements of the data size (4
bytes for `d`, 2 for `w`):

| form | elements accessed | base writeback |
|------|-------------------|----------------|
| `[b,+#k]` / `[b,-#k]` | `b+k` / `b-k` | none |
| `[b,++#k]` (pre) | `b+k` | `b += k` |
| `[b,#k++]` (post) | `b` | `b += k` |
| `.fetchN [b,+#k]` | **`b+k+1` … `b+k+N`** | none |
| `.fetchN [b,#k++]` | `b+k` … `b+k+N-1` | `b += N` |

So a fetch with a base+offset address starts **one element after** the printed offset:
`ldad.fetch4 r3, [r2,+#1]` loads dwords 2–5 into `r3`–`r6`. Stores behave the same way.
`.fetchN` is printed when the encoding's MOE-expand bit is clear, `.fetch1` included. Plain
forms (no `.fetch`) never add the extra element.

Evidence:
- **DDK, the base+offset fetch.** The PSP2 shader compilers compensate for the extra element.
  `usc2/finalise.c`: "An LDAD instruction with the FETCH flag adds an extra 4 byte onto the
  memory address", so it lowers the static offset by one data size. `ffgen/reg.c` emits fetch
  loads and stores with offset − 1 dword "to compensate for preincrement" and calls the
  non-incrementing form a "pre increment without the writeback".
- **Hardware, the plain forms and the post-increment fetch.** The CCB fetch at `0x4c0` is
  `ldad r4, [r0,#1++]` then `ldad.fetch7 r5, [r0,#0++]`. The handlers get `ui32Data[1]` in
  `r7`, so `r5`–`r11` are dwords 1–7 (no extra element). GETMISCINFO then stores the reply
  with plain `stad [r2,+#1]` … `[r2,+#7]` into dwords 1–7. Both were checked on the laptop
  (spec §13.10a).
- **ukernel, the post-increment writeback.** Loops such as `0x3f28` (`ldad.fetch2 [r3,#0++]`
  until `r3` reaches `start + 8 × count`) need `b += N`. `stad.fetch2 [r1,#82++]` in a loop
  after `stad [r1,+#81]` (the count) fills pairs from dword 82, so `k` is a displacement
  there, not part of the increment. The SGX540 TA has the same pattern at `#64++`.
- **ukernel, the base+offset fetch.** With the extra element the TRANSFER handler's sync
  blocks land on the documented `SGXMKIF_TRANSFERCMD` fields (`design/sgx545_transfer.md` §3).

Not seen in any corpus and left unmodelled: `.fetchN` with pre-increment or a negative offset,
and register offsets (`[b,+rN]`). The radare2 plugin follows this table. Its ESIL loads and
stores every fetched register and updates the base. `asm.pseudo` names the elements actually
accessed (`r3..r6 = mem[r2 + 2..5]`, `r5..r11 = mem[r0 + 0..6]; r0 += 7`).

## regtouch — the firmware's register map

`regtouch.py` lists every SGX register a USSE program touches directly: the EDM uses
`ldr rD, #idx` / `str #idx, src` with a dword register index (offset = idx × 4). It runs on
usse-dis (or refdisasm) output and can emit radare2 comments for each access site:

```sh
./usse-dis ukernel.bin > uk.txt
../../.venv/bin/python regtouch.py uk.txt --r2 regs.r2     # 191 registers on the SGX545 ukernel
../../.venv/bin/pytest -q test_regtouch.py
```

## pds-dis — the PDS (data sequencer) decoder

The EDM is entered through small **PDS programs** (spec §13.8). `pds.c` / `pds.h` decode the
32-bit PDS instruction word: opcode `w[31:27]`, predicate `w[26:24]` (`p0`–`p2`, `if0`/`if1`,
`aluz`/`alun`, 7 = always), and a 24-bit operand body per group (`movs`/`movsa` to the
`dout*` issue ports, `mov16`–`mov128`, `load`/`store`, integer ALU, `tstz`/`tstn`, `bra`/`call`,
fences). `pds-dis <program.bin> <data_dwords>` prints the data segment as `data` and decodes
the rest, in `refpds545`'s format.

The layouts are clean-room: they were derived **black-box**, by probing `refpds545` with
single-bit flips and value sweeps over each field. `pds_oracle_check.py` then diffs the result
against the oracle over the five captured EDM programs and a fixed-seed random corpus of
128k words. **0 wrong, and nothing missed**: `.word` appears only where the oracle itself
reports a reserved encoding.

```sh
make pds-dis
./pds-dis ../../harness/ukernel-extract/extracted/sgx545_pds13_runtime.bin 24
python3 pds_oracle_check.py              # needs refdisasm/refpds545
../../.venv/bin/pytest -q test_pds.py
```
