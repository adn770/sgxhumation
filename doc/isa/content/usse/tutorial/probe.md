<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Testing an instruction yourself

What an instruction does is settled on the hardware, not in the text. The probe harness
(`tools/usse-asm/probe.py`) runs a case as the pixel shader of a 64 x 32 fill on the real GPU:
a template sets inputs, runs the instruction under test, and stores registers to a scratch
buffer the host reads back. This is the template most cases use:

<!-- @prog probes/dump.s match='^(phas|mov\S* r(1|10|29),|.OP.|stad .r29,.#[08].)' -->

`{OP}` is the instruction under test, and `#0x5CAF0000` a marker the harness replaces by the
scratch buffer's address; the simulator does the same, so the template runs on it unchanged.
Here it asks how `fmad` rounds a tie: 1 + 2^-12 squared, written to `r10` and stored:

<!-- @sim prog=tutorial/fmad_tie.s -->

The product, 1 + 2^-11 + 2^-24, is a tie between two binary32 values; `fmad` rounds it to even
(*Float rounding and NaN*). On the hardware, the same case is a line of a probe table in
`tools/usse-asm/progs/probes/`; `probe.py run` sends it to the GPU, and
`tools/usse-asm/vectors.py` turns the readback into a vector the simulator is tested against
(`design/sgx545_vectors.md`), so a rule found this way is held to the hardware from then on:

```
$ cargo run --release -p sgx-tools --bin sgx-sim -- --vectors data/vectors/vec1.vec.json
```
