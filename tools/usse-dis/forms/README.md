# USSE instruction families

`usse.c` holds the shared operand kinds and field macros, the forms that existed before the
families, and includes one file per family twice: with `USSE_PART_DEFS` (the family's own
kinds and macros, before the table) and with `USSE_PART_FORMS` (its entries, inside the
table, after the older forms and before the long tail of exact words). A family owns opcode
classes (`enc[63:59]`):

| file | classes | instructions |
|------|---------|--------------|
| `float.inc` | 0-4, 6, 7, 25, 27 | float ALU, transcendentals, `fdp`, `fminmax`, `efo`, `fpma` |
| `memctl.inc` | 28, 29, 31 | `smp*` texture sampling, `eldd`/`eldq`, the rest of class 31 |
| `intpack.inc` | 8, 14, 16-21, 24, 26 | `pck`/`unpck`, `sop2`/`sop3`, `ima*`, `idiv`, `u8dot` |
| `misc.inc` | 5, 9, 15, 22, 23 | `movc`/`movmsk`, the test group, `firh`/`firv`/`bilin`/`adif` |

## Method

Clean-room, as for every older form: the layouts come from **black-box probing** of the
`refdisasm545` binary (`refdisasm/build.sh`; never its source): take an attested or swept
word, flip one bit or sweep a field, and see what the printed text does; `solve.py` correlates
bits with fields over the SGX540 corpus. Then write the form with fields as general as the
probing supports (an under-determined field is fixed in the mask instead of guessed), and hold
it to the oracle:

```sh
make && make check                                         # corpora + PDS: WRONG must stay 0
python3 random_oracle_check.py --per-form 3000 --uniform 100000
cd ../usse-asm && make && ../../.venv/bin/pytest -q        # assemble every decoded word back
```

`random_oracle_check.py` draws words per form (its fixed bits, random elsewhere, plus one- and
two-bit flips) and uniformly: a form may only decode a word whose text equals the oracle's.
The assembler inverts the same table, so `test_roundtrip.py` must stay at 0 failed.
