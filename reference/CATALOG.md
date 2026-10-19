# SGXhumation — Reference Corpus Catalog

Third-party open-source material collected for reverse-engineering a hardware/ABI spec of
the PowerVR **SGX545**. This directory is **not ours** — each item keeps its upstream
license. It must be `.gitignore`-d (or converted to git submodules) before the project is
ever pushed. Pinned commits below make the corpus reproducible.

Collected: **2026-10-19**. Total on disk: ~1.3 GB + ~29 MB `preserved/`.

Legend — **RE value** = how directly it feeds `design/sgx545_hw_spec.md`.

---

## 1. `sgx545-ce/` — ⭐ OUR EXACT CORE (primary source)

| | |
|---|---|
| Upstream | https://github.com/The1TrueJoe/sgx545-ce |
| Commit | `1c3ecae090ad7b5801a351cf1341bbcd56e03440` (2026-10-17) |
| License | **GPL-2.0** (Intel DDK, kernel component) |
| Size | 3.5 MB |

Intel's GPL DDK **1.7.862890** for the SGX545 in the Atom CE5300/CE5310 — the *same GPU
core* as Cedarview — ported to modern Linux. `src/` is Intel's DDK imported **verbatim as
the first commit**; the port changes live in later commits, so `git log src/` cleanly
separates "what Intel shipped" from "what was changed to build on new kernels." Builds
clean on modern kernels but **never loaded on real hardware** — that bring-up is our Goal-2
milestone.

**RE value: HIGHEST.** This is the register map + services/bridge command ABI + init/memory
model for our exact core. The spec reconstruction starts here.

## 2. `linux_openpvrsgx/` — multi-version GPL DDK archive

| | |
|---|---|
| Upstream | https://github.com/rggammon/linux_openpvrsgx |
| Commit | `2342ce92fdde66ac5a7ed7548b47a3a0836c9e77` (2026-08-26) |
| License | **GPL-2.0** (Linux kernel drivers) |
| Size | 622 MB (**sparse-checkout**: only `drivers/gpu/drm/pvrsgx/`) |

A Linux kernel fork shaping GPL PVR/SGX5 kernel drivers toward `drivers/gpu/drm` (glue for
OMAP/Sunxi/**jz4780**/testchip; *not* userspace/firmware). We sparse-checked out just
`drivers/gpu/drm/pvrsgx/`, which archives **16 DDK versions**:

```
1.5.15.3226  1.7.862890  1.8.869593  1.9.2188537  1.9.2204701-b8000  1.9.2204701-iq451
1.9.2253347  1.10.2359475  1.12.2701748  1.12.2824438  1.13.3341330  1.14.3699939
1.14.3759903  1.14.3841212  1.15.4564147  1.17.4948957
```

Includes a **second copy of 1.7.862890** (cross-check against `sgx545-ce`; layouts confirmed
identical: `eurasiacon`/`services4`/`include4`/`tools`).

**RE value: HIGH.** (a) Independent copy of our DDK for verification. (b) Version diffing
across 1.5→1.17 exposes how the register defs and bridge ABI evolved — invaluable for
distinguishing core-invariant structure from version churn.

> Note: working tree is sparse; run `git sparse-checkout list` to see the active path. To
> widen, `git sparse-checkout set <path>`. `.git` retains full history (~544 MB).

## 3. `SGX535-reMESA/` — ⭐ sibling RE + REAL HARDWARE CAPTURES

| | |
|---|---|
| Upstream | https://github.com/gabrieelgama/SGX535-reMESA |
| Commit | `bc601ee8ec08fdc593eed970db0db46c3d94ac20` (2026-10-18) |
| License | **MIT** (derived solely from public IMG material + own hardware) |
| Size | 713 MB |

Active (started 2026-09-16) independent reconstruction of the **SGX535** (Poulsbo/GMA500) —
one ISA generation below our 545, same USSE family. Test rig: Dell Inspiron Mini 12, Atom
Z520, SGX535 rev121. **First hardware triangle on 2026-10-06** with verified framebuffer
readback — i.e. controlled TA/3D execution on real silicon.

Layout: `docs/` (reconstructed architecture, register info, MMU/BIF, command submission,
USE/USSE/PDS findings; `evidence-matrix.csv`), `kernel/`, `mesa/`, `include/`, `tools/`,
`experiments/`. The 435 MB `docs/` is mostly empirical: `phase8` (252 MB) and
`hardware-evidence` (149 MB) hold **~1183 JSON, ~1400 stdout/stderr dumps, 50 `.bin`** —
captured command streams and test logs from working hardware.

**RE value: HIGH (empirical anchor).** Our closest living RE effort. Their reconstructed
defs and captured command streams are the hardware-truth check on our DDK reading. Track it;
don't duplicate it. Differences 535→545 must be reconciled (not assumed identical).

## 4. `sgx540-reversing/` — SGX540 RE docs & tooling

| | |
|---|---|
| Upstream | https://codeberg.org/Garnet/sgx540-reversing |
| Commit | `c15884751ad870b3e06abd3d8b5a6ab00e2fb3cd` (2022-10-16) |
| License | No explicit file; README: *"derived exclusively from binaries and code published officially by Imagination Technologies that are publicly available."* Treat as reference-only; confirm before reuse. |
| Size | 2.1 MB |

Documentation and tooling from an SGX540 RE project: `ukernel/` (microkernel notes), `asm/`,
`info/`, Android/postmarket hooks. Useful for **methodology** — how to capture and decode
USSE and the firmware microkernel — and for the `pdump` command-stream approach.

**RE value: MEDIUM (methodology + microkernel insight).**

---

## 5. `preserved/` — ⭐ at-risk artifacts mirrored locally

Orphaned, legally-encumbered, or scrape-blocked material that could vanish from the internet.
Mirrored here with provenance + checksums. Like all of `reference/`, this is **gitignored**;
`SHA256SUMS` files allow re-verification. Collected 2026-10-19.

### 5.1 `cedarview-gfx-blob/` — ⭐⭐ the closed Cedarview driver (CRITICAL PATH)

| | |
|---|---|
| Source | archive.org item `PowerVR-Graphics-Media-Drivers-for-Linux-Intel-GMA3600-3650-GPU` |
| File | `cdv-gfx-drivers-1.0.3_bee.tar.bz2` (3,903,412 bytes) |
| SHA-256 | `2e662b25ffb40f067ea4dabd193d1927b1090b6d4996e159653e4dd695c0317a` |
| Version | DDK **1.7.862890** — **exact match to our GPL kernel DDK** (`sgx545-ce`) |
| License | proprietary Intel/Imagination EULA (was redistributable; study under EU interop, §Legal) |

The missing closed half of DDK 1.7.862890 — pairs perfectly with our GPL kernel source.
Contains (extracted under `extracted/`): `cedarview-userspace` tar, `cedarview-vaapi` tar
(video decode), `cedarview-libwsbm` tar (buffer manager), `cedarview-kernel-v1.0.3_bee.patch`
(GPL kernel), plus README/LICENSE/INSTALL/ReleaseNotes. All binaries are **`ELF 32-bit Intel
80386`** → directly tractable with radare2 / Ghidra / objdump.

**Priority RE targets inside `usr/lib/` (all `…so.1.7.862890`):**
- `libsrv_init.so` — the **`srvinit`**: builds the init scripts + EDM task regs, loads the
  microkernel. This is the spec §7.1 critical-path blocker, in binary form.
- `libsrv_um.so` — user-mode services (bridge/ioctl wrappers to the GPL kernel).
- `libglslcompiler.so` — **GLSL→USSE shader compiler**: the clearest window onto the USSE
  ISA (the eventual-3D blocker).
- `libGLESv2 / libGLES_CM / libIMGegl / libEGL / libpvr2d / libPVROGL_MESA / libOpenVG*` —
  the full client API stack + WSEGL window-system backends.

> Open question (spec §8-Q5): the microkernel image has no standalone `.fw` file — it is
> likely embedded as a data blob inside `libsrv_init`/`libsrv_um`. Locating and extracting it
> is a top v0.4 task.

### 5.2 `web-snapshots/` — fragile info pages

| File | Source | Note |
|---|---|---|
| `imgtec_SGX545_programmer_doc.json` | forums.imgtec.com/t/…/3940 | Discourse thread JSON: the "is there SGX545 programmer documentation?" discussion |
| `imgtec_open_source_older_gpus.json` | forums.imgtec.com/t/…/3425 | Discourse thread JSON: IMG's stance on open-sourcing older GPUs |
| `sunxi_PowerVR.html` | linux-sunxi.org/PowerVR (via Wayback) | sunxi wiki blocks scraping (403); pulled raw from web.archive.org |
| `sunxi_SGX544MP_ReverseEng.html` | linux-sunxi.org/SGX544MP/ReverseEngineering (via Wayback) | SGX544 RE methodology notes |

### 5.3 `windows-drivers/` — Windows & EMGD binaries (RE cross-check)

From archive.org item `intel-gma-drivers-win` (`Graphics Media Accelerator 3600/` subfolder).
Same SGX545 silicon, different OS/build — complementary to the Linux closed userspace. See
spec §11.

| File | SHA-256 | Notes |
|---|---|---|
| `win7_sgx545_8.0.4.1.1096.exe` | `41d5403b…` | Intel GMA 3600 Win7 32-bit driver (17.9 MB). Extracted → `win7_extracted/Graphics/`: `igdkmd32.sys` (KMD), `igdumd32.dll` (D3D UMD), `igdogl32.dll` (GL), `igddxva32.dll` (video) — all **PE32 i386**, with **named PVRSRV/SGX symbols** (`_DevInitSGXPart2KM@12`, `_PVRSRVCommandCompleteKM`, …). |
| `EMGD_CDV_1.15.1_GC_3278.exe` | `b6eab159…` | Intel EMGD, Cedarview, **DDK 1.15** (78 MB). Proprietary Intel **"SEA" self-extractor** (not InstallShield); 77.5 MB `.setup` payload is a custom container, no standard tool unpacks it → needs Wine extract-to-dir or SEA-header RE. Bonus (1.7↔1.15 diff), non-critical. |

**RE value: HIGH.** The Windows KMD/UMD carry symbol names the stripped Linux libs lack → use
them to label `libsrv_um` routines and accelerate ukernel/init RE.

---

## How the corpus feeds the spec (`design/sgx545_hw_spec.md`, TBD)

1. **Registers & defs** — extract from `sgx545-ce` (`sgx545defs.h`-style headers,
   `services4/.../devices/sgx`); verify against `linux_openpvrsgx` 1.7.862890; cross-check
   names/semantics against reMESA's reconstructed defs.
2. **Command / bridge ABI** — `services4/srvkm/bridged/sgx` in the DDK; validate shapes
   against reMESA's **captured** command streams (`docs/hardware-evidence`, `docs/phase8`).
3. **Init / memory / MMU-BIF sequence** — DDK system glue (`sgx_pc_i686_tc3`, etc.) +
   reMESA MMU/BIF investigation + sgx540 microkernel notes.
4. **USSE/PDS notes** — DDK `usedefs`/PDS headers + reMESA USE/USSE/PDS findings +
   sgx540 `asm/`. (Hardest layer; expect long-tail, mark everything provisional.)
5. **Version deltas** — diff across the 16 DDK versions to separate core-invariant from
   churn before trusting any single field.

Architecture context for all of the above: `design/sgx_tbdr_primer.md`.

## Legal note

All items are public and either GPL/MIT or explicitly derived from IMG-published public
material. Reading and clean-room reconstruction are fine. Keep provenance per claim in the
spec. EU jurisdiction (Directive 2009/24/EC Art. 6) is favourable for interoperability work.
Do not vendor GPL/MIT code into our (future, differently-licensed) tree without honoring
their terms — hence `reference/` stays gitignored / submoduled.
