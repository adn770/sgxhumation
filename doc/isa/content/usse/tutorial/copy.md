<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Josep Torra -->
## Reading what the PDS fetched

A copy reads a source surface. The task's data comes from the PDS, the data sequencer that
starts every USSE task: for a copy its program (`pds_prim_1src.s`) has the texture unit fetch the
source texel of each pixel into primary attribute `pa0` and makes the task wait for it. The
shader only passes the texel on (`design/sgx545_transfer.md` §4.4.3):

<!-- @prog use_copy.s -->

The primary attributes `pa0`-`pa127` are per instance and filled before the task runs; here the
simulator takes them from `--set`. A register no one has written is not defined, and the
simulator stops on a read of one rather than guess its value:

<!-- @sim --set pa0=0x80402010 prog=use_copy.s -->
<!-- @sim -- mov.skipinv.end o0, pa1 -->
