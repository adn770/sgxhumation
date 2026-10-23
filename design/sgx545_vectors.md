# SGX545 instruction vectors

A vector is one short program run on the hardware with what went in and what came out, so the
USSE simulator (`crates/sgx-sim`) can be tested offline against the hardware; no PDS vectors
have been recorded yet (the PDS probes, `isa = "pds"` tables, write them in the same format).
Hardware is the source of truth for semantics (rounding, flags, edge cases), and the programs
are our own assembly, so the vectors stay clean-room. The files are tracked in `data/vectors/`,
with each run's raw readback beside them (`data/README.md`); this note and the converter
(`tools/usse-asm/vectors.py`) describe and make them.

## 1. How vectors are recorded

The probe harness (`tools/usse-asm/probe.py`, `sgxfive-test probe`) runs each case as the
pixel shader of a 64 × 32 client fill on the render node: 2048 instances of the same program.
The host reads back `o0` of every pixel (reported as one value when all agree) and the
64 KiB scratch buffer, whose dword *i* starts as `0xD0000000 | i`; `dump.s`-style templates
store registers there with `stad`. With `--coords` each instance gets `pa0 = (y << 16) | x`,
with `--pixels` every pixel is reported. A job that faults or times out is reported as failed
and recovered by the driver (`sgx545_render_uapi.md`, R15). A new batch is a probe table run
with `probe.py run --out DIR`; the converter turns its `raw.txt` into vectors.

## 2. File format, version 1

One JSON file per run, `<run>.vec.json`:

```json
{
  "format": "sgx545-vectors", "version": 1,
  "run": {"raw": "runs/probes/isa1/final/raw.txt", "raw_sha256": "…",
          "converted": "2026-10-22T19:00:00+02:00", "tool": "vectors.py 1"},
  "vectors": [ { … }, … ],
  "orphans": ["old-name__0"]
}
```

Each vector:

| field | meaning |
|---|---|
| `id` | `<probe>__<case>`, the label `sgxfive-test probe` printed |
| `program.isa` | `usse` (64-bit words) or `pds` (32-bit words) |
| `program.words` | the program as run, each word as a hex string (`0x` + 16 or 8 digits) |
| `program.entry` | index of the first instruction (0: the task starts at the program) |
| `program.source` | `recorded` (bytes sent, saved with the run) or `rebuilt` (assembled again) |
| `init.task` | `pixel`: the shader of a fill instance; `pds-primary`: the fill's PDS primary |
| `init.instances` | 2048 (64 × 32), in raster order |
| `init.temps` | temporaries allocated to the task (`--temps`) |
| `init.pa0` | `coords` (`(y << 16) \| x`) or `fill` (the fill's own, not specified) |
| `init.scratch` | `{"base": "0x3e003000", "bytes": 65536, "fill": "0xd0000000\|i"}` |
| `init.regs` | what the program's leading `mov`s preset, `{"r1": "0x…"}`; informative only |
| `program.data` | PDS only: the data segment's dwords at the start of `words` |
| `init.attrs` | PDS only: the primary attributes its `douta` may write (16: `pa0`-`pa15`) |
| `init.use` | PDS only: the USE program the PDS program kicks (`pa_dump.s`, its `words`, `recorded`); it stores `pa0`-`pa15` to scratch dwords 0-15 |
| `readback.failed` | null, or the failure text |
| `readback.o0` | the uniform `o0`, or null |
| `readback.mixed` | `[[value, count], …]` when the pixels differ |
| `readback.scratch` | `{"0x004": "0x…"}`: the dwords that changed, the rest keep the fill |
| `readback.pixels` | every pixel's `o0` in raster order, or null |
| `provenance` | `probe`, `case`, `table`, `template`, `template_sha256`, `inputs`, `params`, |
| | `options` (the `sgxfive-test probe` flags), `question` |

Rules: integers are hex strings so no reader loses bits; a vector never carries firmware or
closed-library bytes; an unknown `version` is rejected. `orphans` lists readback labels whose
probe or case is no longer in the tables (renamed or dropped since the run), so nothing is
silently lost.

**Rebuilt programs.** The probe runs of 2026-10-22 (`runs/probes/`) kept the readbacks but not
the bytes sent. Their vectors are assembled again from the current tables, so a probe whose
template or parameters changed after its run gives the wrong program: `source: rebuilt` marks
them, and a simulator mismatch on such a vector is first checked against the table's history.
Runs from now on save the assembled cases with the readback (`probe.py run --out` keeps the
`.bin` files) and convert as `recorded`.

## 3. What a simulator test does

Load `program.words` at `entry`, set up one instance per `init` (temporaries, `pa0`, the
scratch fill), run to the end, and compare `o0` and the scratch dwords with `readback`, for
every instance when `pixels` is given. Registers the program reads before writing them (the
known stale-register cases, `sgx545_render_uapi.md` threat list) are undefined: the simulator
stops on such a read as unsupported rather than guess, and a vector whose run cannot be
reproduced (a table rebuilt since, other inputs) is on the simulator's skip list with its
reason (`crates/sgx-sim/src/vectors.rs`, `SKIP`).
