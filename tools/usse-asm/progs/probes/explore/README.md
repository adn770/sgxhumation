<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
# Exploration probe tables

The `x-*` probes of the 2026-10-22 rounds, run once each to explore a question before the
settled probes in `../` were written. Their vectors are published in `data/vectors/`
(`data/README.md`); these tables are what those vectors were built from, one directory per
round, as the runs used them:

| round | tables |
|---|---|
| `isa2/` | x1-x5 |
| `isa3/` | c1, c2, d1, g1-g5, k1, k2, l1, p1, q1-q3, r1, s1, s2, u1, v1 |
| `isa4/` | d1, m1, m2, s1-s3 |
| `pds1/` | `pds-c.toml`: tier C of the PDS probes (load/store, the unknown ports), recorded only; written by `../../../pds_tables.py --explore`, run with `probe.py --table` after tiers A and B |

Each table `<name>.toml` has the generator that wrote it, `gen_<name>.py` (with the helpers it
imports), kept for provenance: running one writes its table next to it, and today's scripts
regenerate every table byte for byte. Some generators keep only the cases that assemble, using
`tools/usse-asm/usse-asm`, so build it first. `probe.py` does not load these tables (they are
not in `../`); to rebuild a run's vectors, place the table next to the run's directory as
`<run>.toml` and convert with `vectors.py convert` (`design/sgx545_vectors.md`).
