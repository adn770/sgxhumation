<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Hardware probe results

What the instruction text leaves open was asked of the GPU on 2026-10-22: our own pixel
programs ran each instruction under test on preset inputs and wrote the result to the pixel
(`tools/usse-asm/probe.py`, the probe table in `tools/usse-asm/progs/probes/`). The results are
summarised in `design/sgx545_hw_spec.md` §14 and given with their evidence in
`tools/usse-dis/r2/README.md` ("Hardware probes"), whose radare2 ESIL models them. Each fact is
one snippet in `doc/isa/content/usse/probe/`, marked *Measured on the hardware (2026-10-22)*,
with what stays undecided and what would settle it, or *Not probed yet*:

<!-- @probes -->
