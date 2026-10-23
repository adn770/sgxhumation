<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Assembling, disassembling and running

Three tools cover a program's life: the assembler and the disassembler exist in C (`tools/`) and
in Rust (`crates/sgx-tools`), the two held byte for byte to each other, and the simulator in Rust
only:

- **`usse-asm`** assembles source in the syntax above into raw little-endian 64-bit words;
  `-l` lists each word with its text. Labels name instruction indices, so branches and
  `lapc` targets can be written by name.
- **`usse-dis`** prints a blob of words, one instruction per line, as the assembler reads it
  back; any decodable word round-trips through the two.
- **`sgx-sim`** runs a program on the simulator, as one instance or as the 2x2 quad of a pixel
  (`--quad X,Y`), and prints every register it wrote and every scratch dword it changed;
  `--trace` shows each instruction with its writes, `--set REG=VALUE` presets registers,
  `--near` lets the float functions give their near values and `--vectors FILE` runs hardware
  vectors (`data/vectors`). It stops, with the reason, at the first instruction whose behaviour
  is not established.

```
$ printf 'mov r1, #0x5caf0010\nmov r2, #7\nstad [r1,+#0], r2\nmov.end o0, r2\n' |
    cargo run -q -p sgx-tools --bin sgx-sim -- --trace -
0: mov r1, #0x3E003010
    r1 = 0x3e003010
1: mov r2, #0x00000007
    r2 = 0x00000007
2: stad [r1,+#0], r2
    [0x3e003010] = 0x00000007
3: mov.end o0, r2
    o0 = 0x00000007
ended after 4 steps
```

The simulator's memory is the probe harness's scratch buffer (64 KiB at `0x3e003000`, and a
`limm` of `0x5cafnnnn` loads its address + `nnnn`), so a probe program runs unchanged; our own
pixel, vertex and state programs are in `tools/usse-asm/progs/`.
