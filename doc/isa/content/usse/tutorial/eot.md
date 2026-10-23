<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Leaving a tile

The pixel programs above only fill output registers. When a tile is finished, the end-of-tile
program hands those registers to the pixel back end (PBE), with the five state words that say
where and how to write them (`design/sgx545_transfer.md` §4.2.5):

<!-- @prog use_eot.s -->

- The four `mov`s load PBE words 0-3 into temporaries. The task is given four temporaries
  (its `temps` in the transfer's meta data); a program that used more would reach other
  instances' registers.
- **`emitpix1`** and **`emitpix2`** send the words to the PBE: the first words 0 and 1, the
  second words 2 and 3 and, as its last immediate, the sideband word 4 (the pack mode and
  `TILERELATIVE`). `.freep` frees the task's resources once the emit is done, and the `.end`
  of the second ends the program.
- The driver patches this program per transfer: the immediates become the PBE words, and the
  destination registers of the last three `mov`s, r0 in the template, become r1-r3. The
  template keeps r0 because the closed generator builds it that way, and ours reproduces its
  words bit for bit (`design/sgx545_fw_format.md` §9.4).

Patched by hand, with made-up PBE words, the simulator shows the two emits and the values each
sends; it models what the USSE hands over, not what the PBE does with it:

<!-- @sim prog=tutorial/eot_patched.s -->
