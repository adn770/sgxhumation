# Hacking on SGXhumation

> *Exhuming a PowerVR SGX545 from its Cedar Trail grave.*

This is the working charter for anyone touching the project: the rules we work under, the
method, how the tree is laid out, and how to build and test each part. What the project *is*
and what it has achieved is in `README.md`. The architecture lives in `design/`.

---

## Ground rules

- **Clean-room discipline.** Architectural understanding comes from *expired patents* and
  *GPL source*; behaviour is captured from *our own hardware* and from *public* reverse-
  engineering work. Closed binaries are run or probed as black boxes (the extraction harness,
  the reference disassemblers), never copied. Record the provenance of every claim. The
  project is developed in the EU, where Directive 2009/24/EC Art. 6 permits decompilation for
  interoperability.
- **Confidential sources stay out.** PVR_PSP2 (`reference/CATALOG.md` §7) is Imagination-
  confidential: build the reference disassemblers from it and use them as oracles, but never
  copy its code, comments or macro names into the tree; describe bitfields in plain words
  instead. New tests use instructions we wrote and assembled ourselves, not firmware words.
- **No Imagination bytes in the tree, ever.** The microkernel, the PDS programs and anything
  cut or captured from the closed Cedarview libraries stay out of git:
  `harness/ukernel-extract/extracted/*.bin`, `tools/fwcutter/*.fw` and `reference/` are
  gitignored. The firmware file is built by each user from their own copy of the closed
  library (`tools/fwcutter`, the b43-fwcutter model). Tables, offsets, hashes and programs we
  wrote ourselves (`tools/fwcutter/facts.py`, `tools/usse-asm/progs/`) are ours and tracked.
- **`reference/` is third-party.** It holds upstream clones and preserved artifacts under
  their own licences. Only our catalog, `reference/CATALOG.md`, is tracked. Do not vendor code
  from it; take field names, offsets and facts, never code.
- **The oracle invariant.** A line the clean-room disassemblers claim to decode must equal the
  reference disassembler's output: `WRONG` stays 0. Emit `; TODO` or `.word` rather than a
  plausible lie. The assembler accepts a word only if the decoder prints it back.
- **Tests first.** No functional change without a test that failed before it. Pure code gets
  unit tests (pytest, TAP C tests, KUnit); hardware behaviour gets a check on the laptop.
- **Clean code.** Remove trailing whitespace before staging (the `kernel/patches/*.patch`
  files are exempt: `.gitattributes` marks them `-whitespace`).
- **Licences.** Every new file gets an `SPDX-License-Identifier` line (MIT for our tools and
  userspace, `GPL-2.0-only OR MIT` for new sgxfive driver files), and `reuse lint` must pass.
- **`debug/`** is for throwaway artifacts (logs, dumps, experiments). `debug/*` is gitignored
  except `.gitkeep`; anything in it can be deleted at any time, so nothing tracked may depend
  on it.

## Method — the evidence triangle

Three complementary, independently sourced layers. No single one is enough:

```
   EXPIRED PATENTS              GPL DDK SOURCE               HARDWARE EVIDENCE
   (architecture, algorithms)   (registers, command ABI,     (captures, probes and
   design/sgx_tbdr_primer.md     services model; our exact    measurements on real
   → the "what & why")           core = DDK 1.7.862890)       SGX5xx silicon)
         │                              │                              │
         └──────────────┬───────────────┘──────────────┬───────────────┘
                        ▼                               ▼
        design/sgx545_hw_spec.md  ◄── reconstruct ──►  cross-checked, cited spec
```

- Patents give the model but **no** registers, encodings or ISA.
- The DDK gives registers, the command/bridge ABI and the init/memory/services model.
- Captures from the closed userspace (run under the harness) and runs on our own laptop are the
  empirical check that the DDK reading is correct.

Every non-obvious statement in `design/` carries a confidence tag: **[patent]**, **[DDK]**
(pristine GPL source), **[RE]** (read from stripped closed-binary disassembly), **[CAP]**
(captured from the closed `srvinit` under the harness), **[HW]** (measured on the laptop),
**[infer]** (inference; verify before relying on it), **[decide]** (needs a decision).

## Repository layout

```
sgxhumation/
├── README.md, HACKING.md, LICENSE, LICENSES/, REUSE.toml
├── Cargo.toml, crates/         ← the Rust tooling workspace (crates/README.md)
├── design/                    ← architecture and specs (start with sgx_tbdr_primer.md)
├── doc/isa/                   ← snippet library + generator of the USSE/PDS ISA references
├── harness/ukernel-extract/   ← mock-and-intercept harness: runs the closed i386 srvinit
│                                under a fake libdrm and captures the microkernel + init info
├── kernel/
│   ├── patches/               ← the gma500 series on mainline v7.3 (source of truth)
│   ├── gma500/                ← working copies: gma_sgx.[ch] and sgxfive/ (the GPU driver)
│   ├── sgx_fw/                ← the firmware-file parser (C), plus the fwdump harness
│   ├── uapi/sgxfive_drm.h     ← the render-node UAPI header
│   └── scripts/               ← build/install/boot loop for the test laptop, netconsole
├── tools/
│   ├── usse-dis/              ← clean-room USSE + PDS disassemblers, radare2 plugin, oracle
│   │   ├── refdisasm/         ← wrappers that build the reference disassemblers (oracle)
│   │   └── r2/                ← radare2 RArch plugin + upstream patch series
│   ├── usse-asm/              ← clean-room USSE + PDS assemblers; progs/ = our GPU programs
│   ├── fwcutter/              ← sgx545-fwcutter: builds sgx545_ukernel.fw from your blob
│   └── arch32/                ← Arch Linux 32 (i686) Docker build images
├── userspace/
│   ├── libsgxfive/            ← C11 library + sgxfive-test over the render node, with a mock
│   └── igt/                   ← IGT tests for sgxfive, as patches + mirrored files
├── reference/CATALOG.md       ← catalog of the third-party corpus (the corpus is gitignored)
└── debug/                     ← throwaway artifacts (.gitkeep only)
```

## Design documents

- `design/sgx_tbdr_primer.md` — PowerVR SGX architecture from expired patents: TBDR, the
  pipeline, the patent shelf. Read it first.
- `design/sgx545_hw_spec.md` — the reconstructed register/ABI spec: identity, MMU/BIF, the kick
  ABI, EDM boot, the captured boot payload, the EDM structure, PDS entry programs and the
  kernel CCB protocol.
- `design/sgx545_boot_sequence.md` — the host side of the firmware boot, register by register,
  for the gma500 port.
- `design/sgx545_fw_format.md` — the `sgx545_ukernel.fw` container, formats v1 to v4.
- `design/sgx545_transfer.md` — the transfer queue: layouts, the EDM handler, fill and copy
  recipes.
- `design/sgx545_render_uapi.md` — the sgxfive render node: memory, contexts, submission,
  recovery, security analysis, the phased plan, and render contexts and jobs (§11).
- `design/sgx545_3d.md` — 3D (draft): what the closed GLES2 stack submits for one triangle,
  the TA command, render details, PB, VDM stream and programs, and the open questions.
- `design/sgx545_usse_isa.md` and `design/sgx545_pds_isa.md` — the USSE and PDS instruction
  set references: machine model, operands, syntax, every opcode group with bit diagrams,
  fields, mnemonics, known semantics and firmware use, the pending probes and every form.
  Generated: edit the snippets in `doc/isa/` and rebuild (`doc/isa/README.md`).

## Getting set up

You need a C11 compiler, Python 3.10+ with pytest and a Rust toolchain (stable, edition
2024) for `crates/`. The Python tools use a project virtualenv:

```sh
python3 -m venv .venv
.venv/bin/pip install pytest reuse
```

Everything below that needs no closed input runs on Linux and macOS. Several tests use
**optional inputs** and skip cleanly without them:

- **Your `libsrv_init.so.1.7.862890`**, via `SGX545_LIBSRV_INIT=/path/...`: cutting the
  firmware and the loader tests.
- **Your `libsrv_um.so.1.7.862890`**, via `SGX545_LIBSRV_UM=/path/...`: the v2/v3 template
  tests.
- **The PVR_PSP2 source tree**, cloned to `reference/PVR_PSP2` (or passed to
  `tools/usse-dis/refdisasm/build.sh` as `PVR=`): the reference disassemblers, i.e. the oracle.
- **The sgx540-reversing corpus**, cloned to `reference/sgx540-reversing`: the SGX540 checks.
- **Harness captures**, by running the harness (`harness/ukernel-extract/README.md`): the
  capture and layout tests.

Both libraries come from Intel's closed Cedarview driver, `cdv-gfx-drivers-1.0.3_bee.tar.bz2`
(DDK 1.7.862890), or from Ubuntu 12.04's `cedarview-graphics-drivers`, which carries the same
firmware; `tools/fwcutter/get-firmware.sh` downloads and verifies them for you, and
`tools/fwcutter/facts.py` lists the SHA-256 of every accepted input. See
`reference/CATALOG.md` for where each corpus item comes from and its licence.

## Build and test

From a fresh clone, with no optional inputs:

```sh
make -C tools/usse-dis && make -C tools/usse-dis pds-dis
make -C tools/usse-asm
cargo test                                       # crates/: the Rust tools, cross-checked with C
.venv/bin/pytest                                 # every suite (pytest.ini lists the trees)
make -C userspace/libsgxfive check              # C tests (TAP) + pytest, against the mock
make -C userspace/libsgxfive check CC='cc -m32'  # the same on i686
.venv/bin/reuse lint                             # licence headers
```

With the oracle built (`tools/usse-dis/refdisasm/build.sh`), also run:

```sh
make -C tools/usse-dis check                     # usse-dis vs refdisasm: WRONG must stay 0
(cd tools/usse-dis && python3 random_oracle_check.py && python3 pds_oracle_check.py)
```

Each tool's README has the details: `tools/usse-dis/README.md`, `tools/usse-asm/README.md`,
`tools/fwcutter/README.md`, `kernel/sgx_fw/README.md`, `userspace/libsgxfive/README.md`.

### Generated and mirrored files

Some files are copies or generated outputs, tracked so that each consumer builds on its own.
The tests fail when `prog_asm_gen.h`, `sgx_tri_progs.h`, `tri_progs_gen.h`, the IGT mirror or
the ISA references are stale; the others must be refreshed by hand:

- `doc/isa/` snippets, the form tables of `tools/usse-dis` and `tools/usse-asm`, or the
  programs the references quote (`tools/usse-asm/progs/`) → `python3 doc/isa/build.py`
  regenerates `design/sgx545_usse_isa.md` and
  `design/sgx545_pds_isa.md`.
- `tools/usse-asm/progs/*.s` → `make -C userspace/libsgxfive gen` regenerates
  `userspace/libsgxfive/src/prog_asm_gen.h` and `src/tri_progs_gen.h` (the first triangle's
  programs for the render engine), and `kernel/gen_tri_progs.py -o
  kernel/gma500/sgxfive/sgx_tri_progs.h` the kernel's copy of the triangle's programs.
- `userspace/libsgxfive/{include,src}` → `userspace/igt/sync-libsgxfive.sh` refreshes
  `userspace/igt/lib/sgxfive/`.
- the USSE form table exists twice, in `tools/usse-dis` (C) and `crates/usse-isa` (Rust): a
  change to one goes into the other, and `cargo test` fails until both list the same forms
  and decode every word alike.
- the `tools/usse-dis` and `tools/usse-asm` cores → `tools/usse-dis/r2/sync-fork.sh <fork>`
  copies them into a radare2 fork checkout.
- the harness decoders or `sgx545_labels.r2` → `harness/ukernel-extract/make_r2.sh` regenerates
  `sgx545_ukernel.r2`.
- `kernel/sgx_fw/sgx_fw.*` → copy them into `kernel/gma500/sgxfive/`.

## Kernel work

The driver lives in gma500 as a patch series on mainline **v7.3**. Keep a kernel tree with a
branch carrying the series (`git am kernel/patches/*.patch` on `v7.3`). Edit the working
copies in `kernel/gma500/`, copy them into the tree, commit there, and re-export the series
after every kernel commit:

```sh
git -C "$SGX_KERNEL_TREE" format-patch -o "$PWD/kernel/patches" v7.3..sgx545
```

The target is an i686 machine with 2 GB of RAM: build kernels on a faster x86 machine
(`make ARCH=i386`), never on the laptop. The pure code (`sgx_fw`, `sgx_seq`, `sgx_xfer`,
`sgx_mmu`, the sgxfive VM, job, coredump and driver-gate suites) has KUnit tests; run them with
`tools/testing/kunit/kunit.py` in the kernel tree.

### The test cycle

`kernel/scripts/` automates build → install → one-shot boot on the target laptop:

- `install-test-kernel.sh` (on the laptop, once per kernel package) installs the test kernel
  next to the distro kernels without changing the default boot entry.
- `boot-test-kernel.sh [0|1]` (on the laptop) reboots once into the test kernel, with
  `gma500_gfx.sgx_render=1` (render node on, the default) or `0` (gate off).
- `test-session.sh` (on the laptop) arms a crash safety net for one boot: reboot on panic,
  panic on oops, soft lockups and hung tasks, kernel log over netconsole.
- `netconsole-listen.py` (on the receiving machine) logs that netconsole stream.
- `push-gma500.sh` (on your workstation) builds the gma500 module on the build machine,
  installs it on the laptop, boots the test kernel and waits for the module to come up.

The scripts take the machines from the environment, or from `kernel/scripts/local.env`
(gitignored; copy `local.env.example` and edit it). `SGX_DRY_RUN=1` prints the commands
instead of running them.

| variable | meaning |
|----------|---------|
| `SGX_BUILD_HOST` | ssh destination of the build machine |
| `SGX_KERNEL_TREE` | the kernel tree on the build machine |
| `SGX_TARGET_HOST` | ssh destination of the Cedarview laptop (`user@host`) |
| `SGX_TARGET_SSH_OPTS` | extra ssh options for the laptop, e.g. a host key alias |
| `SGX_NETCONSOLE_HOST` | IP address that receives netconsole (usually the build machine) |

`local.env.example` lists the rest (release, build jobs, remote PATH, netconsole port).

Every test boot runs with the safety net armed. Do not rely on suspend/resume on the laptop.
Check device buffers against the reference loader with `tools/fwcutter/check_dump.py` on a
copy of debugfs `dri/0/sgx/`.

### Firmware

```sh
tools/fwcutter/get-firmware.sh              # download, verify, accept the licence, cut
sudo install -m644 sgx545_ukernel.fw /usr/lib/firmware/
```

With the libraries already at hand, cut directly:

```sh
python3 tools/fwcutter/fwcutter.py cut path/to/libsrv_init.so.1.7.862890 --tq-from-source \
        -o sgx545_ukernel.fw
```

The driver loads it with `request_firmware()`; it boots lazily on first use, and debugfs
`dri/N/sgx/` exposes the bring-up controls and tests (`load`, `boot`, `status`, `xfer_*`,
`tri`, ...).

## Userspace and IGT

`userspace/libsgxfive` is tested against an in-process mock of the kernel side, so it needs
no hardware. On the laptop the same `sgxfive-test` binary runs against the real render node.

The IGT tests are a patch series on igt-gpu-tools (`userspace/igt/patches/`), plus mirrored
copies of the files they add. Build IGT for i686 in the Arch Linux 32 image
(`tools/arch32/README.md`, then `userspace/igt/README.md`) so it links against the laptop's
libraries, and re-export the patches after each IGT commit.

## Commits

GNOME/freedesktop style:

```
topic: short summary (max 70 chars total)

Description of the changes, wrapped at 80 characters. Explain
what changed and why.
```

- The topic is the component (`kernel`, `sgx_fw`, `uapi`, `usse-dis`, `usse-asm`, `fwcutter`,
  `harness`, `libsgxfive`, `igt`, `tools`, `spec`, `design`) or a type (`docs`, `fix`, `test`,
  `refactor`, `chore`).
- Lowercase after the colon, imperative mood, no period at the end of the first line.
- One logical change per commit, with its tests. Larger efforts may land on a topic branch and
  be merged with `--no-ff` (as the USSE instruction families were).
- Commits inside the kernel and IGT trees follow those projects' rules (`drm/gma500: ...`),
  since the exported patches are meant for upstream review.
- AI-assisted commits say so. Here, in IGT and in radare2 they carry a `Co-Authored-By:`
  trailer; kernel commits carry `Assisted-by: LLM` (`Documentation/process/coding-
  assistants.rst`). An AI agent never adds a `Signed-off-by:`: only the human author signs,
  after reviewing the patch (`git rebase --signoff`).

## Markdown

Wrap lines at 100 characters and break long sentences at natural points.
