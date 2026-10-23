<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
### `mul` signedness and the `adc`/`sbc` carry

*Not probed yet.* `pds-mul` multiplies low and high halves under `alum unsigned` and `alum
signed` (readings: signed only under `alum signed`; always unsigned; always signed).
`pds-adc-sbc` feeds an `add`'s carry to `sbc` and a `sub`'s borrow to `adc` (readings: a
carry out and a borrow that `adc` adds and `sbc` subtracts; a no-borrow flag whose complement
`sbc` subtracts; no carry at all).
