<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
# data/: the published part of the offline corpus

The hardware instruction vectors (`design/sgx545_vectors.md`) and the raw readbacks they were
converted from, laid out as in the local corpus so that each file's `run.raw` names its readback
here. Everything else in the corpus (firmware, captures, oracle output, closed libraries,
register and memory dumps) stays local; its inventory, `corpus/MANIFEST.md`, is local too.

| path | what |
|---|---|
| `vectors/vec1.vec.json` | the first batch (programs recorded with the run) |
| `vectors/probes2026-10-22/*.vec.json` | the probe rounds 1-4, one file per run |
| `runs/probes/<round>/<run…>/raw.txt`, `hw/vectors/vec1/raw.txt` | each run's readback log |
| `closed-idioms.txt` | program words that also occur in the closed binaries (below) |
| `test_data.py` | the checks below |

**Why these may be published.** The programs are our own assembly, from the probe tables and
templates in `tools/usse-asm/progs/probes/` (the `x-*` exploration probes from the tables in its
`explore/`, `vec1` from `progs/vectors/vec1.toml`); the readbacks are what our hardware returned
for them. No vector carries firmware or closed-library bytes. 28 generic words (the task
preamble, `wdf`, `idf`, `nop`, moves of small immediates, short branches, a register tested
against itself) also occur in the closed binaries, as they would for any program; they are
listed with their disassembly in `closed-idioms.txt`, reviewed 2026-10-23.

**Checks** (`.venv/bin/pytest data`): every vector file is a valid version-1 file, every raw
readback matches the hash its vector file recorded, every program's table is tracked, nothing
else is in this directory, and,
where the local corpus is present, no other program word occurs in the closed binaries.

**Publishing a probe run.** With the local corpus present:

1. Run the probes into the corpus, so that the paths match this directory:
   `tools/usse-asm/probe.py run --out corpus/runs/probes/<round>/<run>` (`probe.py --table`
   for one table). A table that is not in `tools/usse-asm/progs/probes/` goes beside the run
   directory as `<run>.toml`, which is where the converter looks for it first, and is tracked
   like the exploration tables (`progs/probes/explore/`).
2. Convert: `.venv/bin/python tools/usse-asm/vectors.py convert
   corpus/runs/probes/<round>/<run>/raw.txt --root corpus -o data/vectors/<batch>`. The file is
   named after the run's path (`runs_probes_<round>_<run>.vec.json`); programs recorded with
   the run are read from its `bins/` (`vec1` was converted this way and renamed).
3. Copy only the readback: `raw.txt` to `data/runs/probes/<round>/<run>/raw.txt`. The run's
   `bins/`, logs and `dmesg.txt` stay in the corpus.
4. `.venv/bin/pytest data`: raise the file count in `test_data.py`, and review every new word
   the closed-word check reports; a generic idiom goes in `closed-idioms.txt`, anything else
   means the program must change.
5. `cargo test -p sgx-sim --test vectors`: a mismatch is either a simulator rule to fix or a
   rebuilt program that differs from what ran, which goes on `vectors::SKIP` with its reason.
   Then `cargo run --release -p sgx-sim --example coverage` and `python3 doc/isa/build.py`.

**Readers.** `crates/sgx-sim` (`vectors::data()`) runs the vectors in `cargo test` and writes
`doc/isa/sim-coverage.json` from them; `sgx-sim --vectors data/vectors/vec1.vec.json` runs one
file. A new batch is converted with `tools/usse-asm/vectors.py` and added here with its
`raw.txt`, after `pytest data` passes with the local corpus present.
