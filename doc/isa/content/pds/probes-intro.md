<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Hardware probes

The questions of *Open questions* are put to the GPU by PDS probes: our own PDS program runs
as the PDS primary program of a client fill, does the operation under test on preset data
words and passes the results to the USE program `pa_dump.s` with `douta`, which stores them
where the host reads them (`tools/usse-asm/probe.py`, the table
`tools/usse-asm/progs/probes/pds.toml` written by `tools/usse-asm/pds_tables.py`; the readings
below are its hypotheses). The first, `pds-readback`, checks that path itself. Each fact
is one snippet in `doc/isa/content/pds/probe/`, marked *Not probed yet* until the run:

<!-- @probes -->
