# SGXhumation

> *Exhuming a PowerVR SGX545 from its Cedar Trail grave.*

SGXhumation is a for-fun, learning-driven attempt to make the GPU of an old Intel Atom laptop
do something under a modern mainline Linux kernel.

Intel's Cedar Trail platform (Atom N2600/N2800/D2500/D2700, sold as "GMA 3600/3650") does not
have an Intel GPU. The 3D core is an Imagination **PowerVR SGX545**, and Linux support was
always a closed driver for long-dead kernels. Mainline's `gma500` driver does display only.
This project reverse-engineers enough of the SGX545 to boot its firmware and run GPU jobs from
a small new driver inside `gma500`, using a Cedarview laptop (Atom N2600, 2 GB, i686) as the
test machine.

## Why

- **Fun.** It is a puzzle: an undocumented GPU, a stripped closed driver, and a laptop that
  nobody would otherwise use.
- **Learning.** The real goal is to learn GPU and DRM kernel internals end to end: firmware
  boot, MMUs and address spaces, command rings, `drm_sched`, fences, recovery, UAPI design,
  and the tooling (disassemblers, assemblers, emulation) that reverse engineering needs.
- **Old hardware, new kernel.** Everything targets current mainline and modern DRM
  primitives (shmem GEM, `drm_sched`, syncobjs, devcoredump, KUnit), not a resurrected
  vendor stack.

It is not a promise of 3D acceleration. A full open 3D stack (a Mesa driver and a USSE shader
compiler) is a multi-person, multi-year effort without vendor documentation; this project
contributes towards it without claiming it will get there.

## Status

What works today, on the laptop, with mainline v7.3 plus the patch series in `kernel/patches/`:

- **The firmware boots.** The closed SGX545 microkernel (the EDM) boots from our gma500 code,
  reports init complete about 140 µs after the kick, and answers kernel-CCB commands: misc
  info, idle, power off, and a full stop/boot cycle (`design/sgx545_hw_spec.md` §13.10a).
- **The transfer queue renders.** SGX545 has no 2D core, so fills and copies run as small 3D
  renders without a TA phase. Dummy transfers, sync waits, solid fills and copies of arbitrary
  rectangles work (`design/sgx545_transfer.md`).
- **A render node, `sgxfive`.** GEM buffers, per-client GPU address spaces with their own SGX
  page directories, transfer contexts, `drm_sched` job submission with syncobjs and hardware
  fences (`design/sgx545_render_uapi.md`, `kernel/uapi/sgxfive_drm.h`).
- **Recovery.** Hung jobs and BIF (GPU MMU) faults are detected; the firmware is reset in
  place, innocent queued jobs re-run, the faulting context is lost, and a file that keeps
  faulting is banned. A recovered fault takes about 100 ms and leaves a devcoredump error
  state.
- **An opt-in gate.** Client shaders turned out to be able to read every SGX register and stale
  temporaries (probe Q4 in the UAPI design), so the render node exists only when the module is
  loaded with `gma500_gfx.sgx_render=1`, for trusted users. Without it the primary node answers
  only DEV_QUERY.
- **A first triangle, from userspace.** Render contexts and render jobs (TA + 3D) on the
  render node: a client places our own vertex, pixel and end-of-tile programs and its scene in
  its VM, the kernel builds the firmware side, and every pixel matches a CPU reference, at
  render targets from 32 × 16 up to the device's largest, 4096 × 4096 (the laptop's 1024 × 600
  panel included), with several contexts, transfers and syncobjs interleaved
  (`design/sgx545_render_uapi.md` §11, `design/sgx545_3d.md` §14).
- **Tests on hardware.** 10 KUnit suites in the kernel series, and IGT tests for sgxfive
  (`userspace/igt/`): all subtests pass on the laptop, including fourteen render ones, plus the
  gate's own subtests in both gate states.
- **Our own GPU programs.** All nine transfer-queue programs (fill, copy, end of tile, pixel
  events, ...) are written as our own assembly source (`tools/usse-asm/progs/`) and assemble
  bit for bit to the closed driver's templates, so 2D needs no closed userspace at all.

The tools that got it there:

- **A clean-room USSE and PDS disassembler** (`tools/usse-dis`). On uniformly random words,
  every encoding the reference disassembler considers valid decodes to exactly its text
  (153414 of 153414 on a 200k-word sample, all 32 opcode classes), and all 6637 instructions of
  the SGX545 microkernel decode, with 0 wrong.
- **A clean-room USSE and PDS assembler** (`tools/usse-asm`), built by inverting the same form
  table: every decoded word reassembles, and the closed toolchain's code comes back bit for
  bit.
- **radare2 plugins** for USSE and PDS (`tools/usse-dis/r2`): disassembly, assembly, op types
  and branch targets for graphs, a register profile, and conservative ESIL emulation (71.7% of
  the microkernel's words).
- **sgx545-fwcutter** (`tools/fwcutter`): builds `sgx545_ukernel.fw` from your own copy of the
  closed library, b43-fwcutter style, with a reference loader that reproduces the harness
  captures byte for byte and a C port that the kernel uses. `get-firmware.sh` fetches the
  official package, checks its hashes, asks you to accept Intel's licence and cuts locally.
- **A mock-and-intercept harness** (`harness/ukernel-extract`) that runs the closed i386
  `srvinit` under a fake `libdrm` and captures the microkernel and the boot payload, with no
  GPU involved.
- **libsgxfive and sgxfive-test** (`userspace/libsgxfive`): a small C library and test tool for
  the render node, tested against an in-process mock of the kernel side.
- **Arch Linux 32 build containers** (`tools/arch32`) for building i686 userspace (IGT now)
  against the laptop's distribution.
- **Design documents** (`design/`): an architecture primer from expired patents, the
  reconstructed hardware/ABI spec, the boot sequence, the firmware format, the transfer queue
  and the render-node UAPI.

## Not done

- **More 3D.** Render jobs draw one flat-coloured scene; varyings, textures, depth and
  stencil, multi-kick scenes and partial renders (scenes larger than the parameter buffer) are
  untried. Render clients can write the microkernel's TA/3D control block (one more reason for
  the opt-in gate).
- **Mesa.** No GL driver and no shader compiler; the assembler is the first step.
- **The microkernel is still Imagination's.** The firmware file is cut from the closed library
  by each user; replacing the EDM is a separate, open question.
- **PRIME and scanout** of rendered buffers, runtime PM and suspend/resume.
- **Upstreaming.** The UAPI needs real open userspace (Mesa) and the client-shader leak needs a
  program validator before the render node could go upstream; the driver would also move out
  of gma500 onto the auxiliary bus first.
- Display improvements and video decode (MSVDX) have not been looked at.

## Repository layout

| path | what |
|------|------|
| `design/` | architecture and specs; start with `sgx_tbdr_primer.md` |
| `kernel/` | the gma500 patch series, working copies of the driver, the UAPI header, scripts |
| `tools/usse-dis/` | USSE/PDS disassemblers, the oracle checks, the radare2 plugin |
| `tools/usse-asm/` | USSE/PDS assemblers and our GPU programs (`progs/`) |
| `tools/fwcutter/` | sgx545-fwcutter and the reference firmware loader |
| `tools/arch32/` | i686 build containers |
| `harness/ukernel-extract/` | the mock-and-intercept extraction harness |
| `userspace/libsgxfive/` | the render-node library, test tool and mock |
| `userspace/igt/` | IGT tests for sgxfive |
| `reference/CATALOG.md` | catalog of the third-party reference corpus (not included) |

## Getting started

Most of the tree builds and tests on any Linux or macOS machine with a C compiler and Python
3.10+, with no hardware and no closed files: the disassembler, assembler, fwcutter's own
tests and libsgxfive against its mock. Running the driver needs a Cedar Trail machine, a
kernel built with the patch series, and the firmware cut from your own copy of the closed
library. See **[HACKING.md](HACKING.md)** for the rules, the setup and the build and test
commands.

## Clean room and firmware

- This repository contains **no Imagination code**. The disassemblers and assemblers were
  written from scratch. Their layouts came from bit correlation against public SGX540
  reverse-engineering dumps and from black-box probing of reference disassemblers.
- **Where the reference disassemblers come from, plainly:** they are built from PVR_PSP2, a
  publicly circulating Imagination source tree for another SGX part. It is not open source:
  it is Imagination-confidential, all rights reserved. We ran the disassemblers built from it
  as black-box oracles, and consulted its headers early on for field and encoding names and
  for the transfer-queue structure layouts. No code or text from it is in this repository, its
  macro names are replaced by plain bitfield descriptions, and you must supply it yourself to
  build the oracles (`tools/usse-dis/refdisasm/`).
- Hardware facts come from the GPL-licensed DDK kernel sources, expired patents, the
  extraction harness and measurements on our own laptop. `design/` cites a source for every
  non-obvious claim.
- A few dozen instruction words are quoted in the design documents and test fixtures to
  explain encodings. Our transfer programs (`tools/usse-asm/progs/`) are our own source, but
  they assemble bit for bit to the firmware's templates, because the hardware dictates their
  form; `tools/usse-asm/README.md` explains why.
- The firmware is **not** distributed, and neither is the package it comes from: its licence
  allows redistribution only unmodified. `tools/fwcutter` carries only offsets, sizes, hashes
  and our own reconstructed tables and programs; you supply the closed Cedarview library
  (`cdv-gfx-drivers-1.0.3_bee`, DDK 1.7.862890, or Ubuntu 12.04's identical-firmware
  `cedarview-graphics-drivers`), or let `tools/fwcutter/get-firmware.sh` download it, and the
  tool cuts `sgx545_ukernel.fw` from it, as b43-fwcutter does for Broadcom wireless.
- The third-party corpus the work draws on (GPL DDK trees, other SGX reverse-engineering
  projects, preserved driver packages) is listed in `reference/CATALOG.md` and is not part of
  this repository.

## Licence

The project is under the **MIT licence** (`LICENSE`), with these exceptions, marked by
`SPDX-License-Identifier` headers:

- **The sgxfive driver** (`kernel/gma500/sgxfive/`, `kernel/sgx_fw/`) is
  **GPL-2.0-only OR MIT**, like the upstream driver for later PowerVR GPUs. Its glue into
  gma500 (`kernel/gma500/gma_sgx.*` and the changes to existing gma500 files in
  `kernel/patches/`) is **GPL-2.0-only**, as gma500 is. In the patch series the dual licence
  arrives in the last patch.
- **The UAPI header** `kernel/uapi/sgxfive_drm.h` is
  **(GPL-2.0-only WITH Linux-syscall-note) OR MIT**, like the other DRM UAPI headers.
- **The radare2 patches** in `tools/usse-dis/r2/upstream/` are **LGPL-3.0-only**, radare2's
  licence.
- `userspace/libsgxfive/third_party/` holds Linux's MIT-licensed DRM UAPI headers.

The tree follows the [REUSE](https://reuse.software/) specification: `LICENSES/` holds every
licence text and `reuse lint` passes.

## Related work

- **radare2 fork**: [github.com/adn770/radare2](https://github.com/adn770/radare2), branch
  `arch-usse`: the USSE and PDS plugins in-core, intended for an upstream pull request.
- **Kernel**: [github.com/adn770/linux](https://github.com/adn770/linux), branch `sgx545`, the
  series on mainline v7.3. `kernel/patches/` here is the source of truth.
- **IGT**: [github.com/adn770/igt-gpu-tools](https://github.com/adn770/igt-gpu-tools), branch
  `sgxfive`; `userspace/igt/patches/` here.
- [SGX535-reMESA](https://github.com/gabrieelgama/SGX535-reMESA) reverse-engineers the sibling
  SGX535, and [sgx540-reversing](https://codeberg.org/Garnet/sgx540-reversing) documents the
  SGX540; both were invaluable references. See `reference/CATALOG.md` for the full list.

## How it was made

Much of the code, the tests and the documents were written by an AI coding assistant (Claude
Code) working under the author's direction and review, with the author choosing the design and
running the hardware. Commits here, in IGT and in radare2 carry a `Co-Authored-By:` trailer.
The kernel commits follow the kernel's rules for AI-assisted work instead: they carry
`Assisted-by: LLM` and, for now, no `Signed-off-by:`, which only the author can add after
reviewing each patch.

## Trademarks

PowerVR is a trademark of Imagination Technologies Limited. Intel, Atom and Cedar Trail are
trademarks of Intel Corporation or its subsidiaries. Other names are the property of their
owners and are used here only to identify the hardware. SGXhumation is an independent project,
not affiliated with, endorsed by or sponsored by Imagination Technologies or Intel.
