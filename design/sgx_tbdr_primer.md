# SGXhumation — PowerVR SGX545 Architecture Primer

> *Exhuming a PowerVR SGX545 from its Cedar Trail grave.*

This is the **textbook** for the project: the conceptual machine behind the GPU in the
Cedarview laptop, extracted from now-expired Imagination/VideoLogic patents and public
architecture material. It deliberately stops at the *architecture* level — the "what and
why." The register map, command-stream byte layout, and USSE shader ISA are **not** here,
because patents don't disclose them; those come from the GPL DDK and live tracing (see
§6). Read this before touching any code so the DDK reads as a reference implementation of
ideas you already understand rather than an opaque blob.

**Status:** living design doc. Sourced claims are marked `[patent]`; architectural
extrapolation from general PowerVR knowledge is marked `[infer]`. Verify `[infer]` items
against the DDK before relying on them.

---

## 1. What this project is

A learning-driven effort to extend Linux support for the PowerVR SGX545 GPU found in Intel
Cedar Trail (Atom N2600/N2800/D2500/D2700, marketed "GMA 3600/3650"). Two primary goals,
in priority order:

1. **Make the laptop genuinely more usable** — better display/output behaviour and, if
   reachable, video decode and/or basic acceleration.
2. **Deeply learn GPU + DRM kernel-driver internals** — the concrete milestone is bringing
   the GPL CE5310 SGX545 DDK (`pvrsrvkm`) up on real hardware, end to end.

Full open 3D (a Mesa driver with an open USSE shader compiler) is a **non-goal as a
completion target** — it is a multi-person, multi-year reverse-engineering effort with no
vendor docs. We may contribute to it; we do not promise to finish it.

---

## 2. The hardware reality

Cedarview's GPU is **not** an Intel design. It is a PowerVR **SGX545** core licensed from
Imagination Technologies. Consequences:

- Intel never owned the 3D IP, so no open 3D driver ever shipped from them.
- Imagination never released SGX programmer documentation (ISA, registers).
- Mesa has no SGX driver to extend (unlike Intel's own i915/iris).

The SGX545 belongs to **Series 5XT (USSE ISA)**. The open-source PowerVR driver that
exists today (mainline `powervr` DRM + Mesa `pvr` Vulkan, merged ~Linux 6.8) targets
**Series 6+ "Rogue" (USC ISA)** — an architectural clean break. Rogue teaches PowerVR
*philosophy* and modern driver *structure*, but will not decode our ISA or command stream.

---

## 3. Source map (the three complementary layers of evidence)

| Source | Gives us | Legal footing |
|---|---|---|
| **Expired patents** (1995–2024, see §7) | Architecture, algorithms, data flow — the mental model | Free to read *and* implement |
| **GPL CE5310 SGX545 DDK** (`The1TrueJoe/sgx545-ce`, Intel DDK 1.7.862890) | Register/command layer, kernel services model; *same core as ours* | GPL; builds on modern kernels, never loaded on HW yet |
| **Live command-stream tracing** (valgrind-mmt style MMIO/cmdbuf capture) | The actual byte-level encoding + USSE opcodes | Our own hardware |
| SGX530/535/540 RE projects (SGX535-reMESA, CI20 SGX540 userspace RE) | Methodology for capturing + decoding USSE on *older* family members | FOSS |

The textbook (patents) + the reference implementation (DDK) + the lab (tracing) form a
clean-room triangle. Each covers the others' blind spots.

---

## 4. The TBDR mental model

PowerVR is a **Tile-Based Deferred Renderer (TBDR)**. Two words, two big ideas:

### 4.1 Tile-based — divide the screen, not the work

The screen is subdivided into rectangular **tiles**; for each tile, only geometry that
falls in that tile is processed `[patent]`. The foundational patent's example: splitting a
20×20 region into four 10×10 tiles cut processing from 16,000 to 4,000 cycles — a 75% drop
`[patent, EP0725367]`. Geometry isn't duplicated per tile; instead each tile holds a **list
of object pointers** into shared surface data, with position/dimension metadata at the head
of each tile list `[patent]`. This per-tile geometry store is the **Parameter Buffer (PB)**
in SGX terminology `[infer]` — the intermediate store holding transformed geometry + tile
lists between the geometry phase and the pixel phase.

### 4.2 Deferred — know visibility *before* you shade

Classic GPUs shade a pixel, then maybe overwrite it when a closer surface arrives (overdraw
→ wasted bandwidth). PowerVR **defers all texturing/shading until per-pixel visibility is
already resolved for the tile.** The original scheme casts a ray from the viewpoint through
each pixel's elementary area and finds the nearest intersecting surface `[patent]` — a
ray-cast / "inverse-Z" HSR rather than a read-modify-write depth buffer. Surfaces are
described by plane coefficients (the patent cites **96-bit surface parameters with
coefficients A, B, C**) `[patent]`. In SGX this HSR happens on-chip per tile, so only the
winning fragments ever reach texturing/shading `[infer]`.

### 4.3 The pipeline stages (SGX naming) `[infer]`

```
            GEOMETRY PHASE                         PIXEL PHASE (per tile)
  vertices → [TA: Tile Accelerator]          [ISP] → [TSP] → [USSE] → tile buffer → framebuffer
             transform + bin into tiles        │        │        │
             write Parameter Buffer            │        │        └ USSE: Universal Scalable
                                               │        │           Shading Engine — unified
                                               │        │           shader ALUs (USSE ISA)
                                               │        └ TSP: Texture & Shading Processor
                                               │           — texture address/fetch/iterate
                                               └ ISP: Image Synthesis Processor
                                                  — on-chip HSR / depth resolve per tile
```

- **TA (Tile Accelerator):** transforms geometry and bins primitives into per-tile lists in
  the Parameter Buffer. This is the "tile-based" write phase.
- **ISP (Image Synthesis Processor):** the deferred-HSR engine. Resolves which surface wins
  at each sample in the tile *before* any shading. Historically a master/slave daisy-chain
  of ISP cells sharing a 96-bit instruction word, each working a different output line of
  the same tile `[patent]`.
- **TSP (Texture & Shading Processor):** texture addressing/fetch and parameter iteration
  for the surviving fragments.
- **USSE (Universal Scalable Shading Engine):** the unified programmable ALU array running
  vertex *and* fragment shaders — the part whose **ISA is the hardest RE wall.**
- **Tile buffer → framebuffer:** a tile is fully resolved on-chip, then flushed out, so no
  large external depth buffer round-trips are needed `[patent]`.

### 4.4 Why it stays fed (the interleave trick) `[patent]`

Tiles vary wildly in complexity. To keep the pixel pipeline from stalling on a heavy tile,
rows from tiles of differing complexity are **interleaved** so the post-processing units see
a steady data rate without an expensive full framebuffer. Useful to remember when reasoning
about scheduling/timing in the DDK.

---

## 5. How the architecture maps onto the Linux stack

| TBDR concept | Linux component | State today |
|---|---|---|
| Display controller / scanout (Intel's own, *not* PowerVR) | `gma500` DRM/KMS driver | Mainline; modesetting + limited 2D; ~8 MB stolen-mem cap; no accel |
| GPU command submission, memory contexts, sync ("Services") | `pvrsrvkm` (the GPL CE DDK) | Ported, builds, **never loaded on HW** ← Goal-2 milestone lives here |
| TA/ISP/TSP/USSE programming, command stream, shaders | Mesa userspace (does not exist for SGX) | Open 3D = the brutal, deferred goal |

Note the clean split: **display is Intel silicon** (so gma500 wins are independent of the
PowerVR curse — this is where Goal 1's easy, shippable improvements live), while
**everything GPU is PowerVR** (Goal 2's DDK bring-up, then the long RE road).

---

## 6. What patents do *not* give us (and where it comes from)

Patents describe methods, not the programming interface. Specifically missing:

- **Register map / MMIO offsets** → read from the GPL CE DDK.
- **Command-stream byte encoding** → DDK + live MMIO/cmdbuf tracing on our hardware.
- **USSE ISA opcodes** → live tracing + the SGX530/540 RE methodology (reMESA, CI20).

Clean-room discipline: build understanding from expired patents and GPL source; capture
behaviour from *our own* hardware. If the Windows/EMGD binary is ever decompiled, note that
we are in the **EU** — Directive 2009/24/EC Art. 6 permits decompilation for
interoperability, and pure-software patents are largely unenforceable here. (Not legal
advice; just a friendlier baseline than the US.)

---

## 7. Patent reference shelf

**Expired — free to read and implement:**

- `EP0725367B1` / `GB2298111A` / `US5856829` — *Computer 3D rendering; inverse-Z,
  list-based control* (priority **1995**, expired ~2015–16). **The foundational TBDR text**
  and the primary source for §4.
- `WO2001095257A1` — *Tiling and compression for rendering 3D images* (priority 2000,
  expired ~2020).
- `EP1351195A2` / `GB2378108A` — *3-D computer graphics rendering system* (priority ~2002,
  expired ~2022).
- `US7834872B2` — *Tiling system for 3D rendered graphics* (priority 2003-03-27, expired
  **2024-03-26**): tile-binning math, conservative reduced-precision edge tests,
  small-primitive culling against sample points.
- `GB2404316A` — *Visibility determination in 3D graphics rendering* (priority ~2003–04,
  expired ~2023–24).

**Still live — public to read, caution to *practice* (SGX-era, closest to our 545):**

- `US7978199B2` — *Parameter compaction in tile based rendering device* (priority 2006,
  expires ~2030). Directly about Parameter Buffer management.
- `US8368691B2` — *Untransformed display lists in a tile based rendering system* (~2009,
  ~2029).
- `US8502829B2` — *Multi-core geometry processing in a tile based rendering system* (~2010,
  ~2030).
- `US9633405B2` — *Tile based computer graphics* (later; 2030s).

---

## 8. Open questions / next step

Nothing here is confirmed against *our* silicon yet. The immediate next step is a
**hardware inventory** of the laptop once it boots:

- exact Cedarview SKU + PCI IDs (`lspci -nnk`),
- kernel version and whether `gma500` probes/binds (`dmesg | grep -i gma500`, `modinfo`),
- stolen/GTT memory size and current display capabilities,
- whether the SGX block is even visible to the OS.

That snapshot turns this textbook into a concrete milestone ladder (gma500 display wins →
DDK build → DDK load/init on HW → first command submitted).
