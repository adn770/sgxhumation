# isa — the instruction-form tables, single source

The forms of the USSE and PDS instruction sets are described once, here, and generated into
the C and Rust decoders and the assemblers:

| description | output | used by |
|-------------|--------|---------|
| `usse/` | `tools/usse-dis/usse_table.inc` | usse-dis, usse-asm, r2 plugin, `doc/isa/formdump.c` |
| | `crates/usse-isa/src/table.rs` | the Rust USSE decoder and its `usse-dis` |
| `pds/` | `tools/usse-dis/pds_table.inc` | pds-dis, pds-asm, r2 plugin, `doc/isa/formdump.c` |
| | `crates/pds-isa/src/table.rs` | the Rust PDS decoder and its `pds-dis` |

`isa/gen.py` (Python stdlib) writes them; both are committed, so neither C nor Rust needs
the generator to build. `python3 isa/gen.py --check`, which pytest runs, fails when one is
stale. After editing a description:

```sh
python3 isa/gen.py                                   # rewrite the tables
make -C tools/usse-dis && make -C tools/usse-asm
cargo test && .venv/bin/pytest                       # C/Rust cross-check, round trips, docs
python3 doc/isa/build.py                             # if a form changed: the ISA references
```

## What a description holds

`isa/usse/{main,float,memctl,intpack,misc}.toml`, read in that order (the order of the
table: the first matching form decodes a word), and `isa/pds/pds.toml`. Names are global
across the files of one instruction set. Both sets run on one form engine (`usse_form.[ch]`,
`usse_isa::form`); PDS words are 32 bits.

- **`[[form]]`**: `pat`, the text with `{i}` for field i; `mask` and `val`, the bits that
  identify it (`word & mask == val`); `fields`, at most ten; optional `dflt`, how the closed
  toolchain fills the bits the form ignores (the assembler's preference).
- **Fields** are written as a bit-range list with a kind (`"PRED_ALL 58:56"`), a layout with
  a kind (`"SRC1(K_SRCD)"`) or the name of a `[field]` entry (`"F_DST"`). Bit ranges are
  `hi:lo` or one bit, listed LSB first: `"K_DST 27:21 33:32 51"` is enc[27:21] in bits 6:0
  of the value, enc[33:32] in 8:7 and enc[51] in 9.
- **Expressions** (`mask`, `val`, `dflt`, `[const]`): `b(n)`, `m(hi, lo)`, `cls(c)` (the
  major class, enc[63:59]), `CLSM`, the consts, `|`, `&`, `~`, `<<`, numbers.
- **Data kinds**, `[kind.NAME]`, one of:
  - `enum = [...]`, the text of each value, `false` for an invalid one (values past the end
    are invalid), or sparse, `enum = { 4 = "frcp", 5 = "frsq" }`;
  - `flags = [...]`, one-bit flags printed in field order;
  - `num = "dec"` or `"hex"`, with optional `mul` and `add` (printed `v * mul + add`);
  - `seq = [[...], ...]`, enumerations printed one after another, each over 2^w names;
  - `sel = [{ t = [...], comp = bool }, ...]` with optional `pre`/`neg`: a blend selection.
- **Code kinds** (`code = [...]`): register operands, the float, c10 and dual-issue readings,
  and every other kind with logic. They are written by hand in both languages under the same
  name: C in `tools/usse-dis/usse.c`, `forms/*.inc` and `pds.c`, Rust in
  `crates/usse-isa/src/kinds.rs`, `forms/*.rs` and `crates/pds-isa/src/kinds.rs`. `cargo test`
  cross-checks them (`crates/README.md`).

The comments in the description carry the evidence for each layout. As before, the layouts
are clean-room: black-box probing of refdisasm545, held to 0 wrong by
`tools/usse-dis/random_oracle_check.py` (`tools/usse-dis/forms/README.md`).
