# libsgxfive and sgxfive-test

A small C11 library over the **sgxfive** render-node ioctls (`kernel/uapi/sgxfive_drm.h`,
designed in `design/sgx545_render_uapi.md`), a test tool that redoes the debugfs checks of the
bring-up from userspace, and an in-process mock of the kernel side so all of it is tested
before the kernel driver exists.

This is a development tool. It is not the open-source userspace that upstreaming the UAPI
needs (that is a Mesa driver, design §1.3–§1.4).

## Layout

| path | what |
|------|------|
| `include/sgxfive.h` | device, DEV_QUERY, BOs, VM contexts, contexts, syncobjs, SUBMIT_JOBS |
| `include/sgxfive_xfer.h` | transfer builders, the program-source interface, the transfer engine |
| `include/sgxfive_mock.h` | the mock render node |
| `src/dev.c` | ioctl wrappers and the OS backend (`open`/`ioctl`/`mmap`) |
| `src/xfer.c` | pure builders: DOUTU, PBE state, background objects, texture words, registers |
| `src/xfer_job.c` | the engine: places programs in the client VM, fills and copies |
| `src/prog_fake.c` | test-double program source (mock only) |
| `src/mock.c` | the mock: kernel rules plus a tiny transfer interpreter |
| `tools/sgxfive-test.c` | the CLI |
| `tests/` | C tests (TAP) and `test_cli.py` (pytest) |
| `third_party/` | Linux's `drm.h`/`drm_mode.h` (MIT) and non-Linux shims; see its README |

## Build and test

```sh
make            # build/libsgxfive.a, build/sgxfive-test
make check      # C unit tests, then pytest on the CLI, all against the mock
make check CC='cc -m32'   # the same on i686 (layout test included)
```

No dependencies beyond a C11 compiler and, for the CLI tests, pytest (the repo's
`.venv/bin/pytest` if present, else `pytest`; override with `PYTEST=`). libdrm is not used: the
library calls the ioctls directly with the UAPI header. It builds and passes on macOS, Linux
x86-64 and Linux i686.

Tests are written first. The C tests are plain programs printing TAP (`tests/tap.h`):

- `test_uapi` — every UAPI struct is the sum of its members (no implicit padding, so i686 and
  x86-64 agree), sizes and offsets, the transfer command at the `sHWRegs` offsets, flag masks;
- `test_xfer` — the pure builders against the worked examples of `design/sgx545_transfer.md`
  (§4.2.3–§4.2.7, §4.4.2–§4.4.5): DOUTU words, PBE state, background objects, the truncated
  texture coordinates, T0–T3, the register block, the `USSE_LIMM` split;
- `test_dev` — the wrappers against the mock: discovery by driver name, DEV_QUERY size rules,
  BO flags, VM_MAP heap and overlap rules, contexts, syncobjs (binary and timeline), and the
  SUBMIT_JOBS validation rules including the reserved update list;
- `test_job` — fills and copies through the engine: with the harness layout the programs
  equal the debugfs recipes word for word; every destination dword is checked (the rectangle
  matrix of the hardware runs); isolation between VMs, faults, lost contexts, read-only maps.

`tests/test_cli.py` drives `sgxfive-test --mock` through every subcommand.

## The mock

`sgxfive_mock_create()` returns an object whose `struct sgxfive_backend` replaces `open`,
`close`, `ioctl`, `mmap` and `munmap`; every library function takes a backend
(`sgxfive_dev_open(be, ...)`, NULL meaning the real one), so the code under test is the code
that will run on the laptop. The mock implements what the kernel side must:

- render nodes: optionally another driver on `renderD128` (discovery must skip it);
- DEV_QUERY with the `pvr` size rules, and the heap table of the `REV1` layout (default; quirk
  `SAMPLE_CACHE`, bridge revision 3) or of the debugfs harness layout (`--mock=harness`);
- GEM BOs with mmap only when `ALLOW_CPU_ACCESS`; zeroed padding and flag masks everywhere;
- VM contexts with per-mapping read-only flags, mappings only inside one user heap, `-EEXIST`
  on overlap, whole-mapping unmap; BOs live while mapped; closing the file frees everything;
- transfer contexts, DRM syncobjs (`CREATE`, `DESTROY`, `WAIT`, `TIMELINE_WAIT`);
- SUBMIT_JOBS: all jobs validated before any runs, the failing index in `jobs.count`, the
  §4.5 address checks, the update list rejected while `max_updates` is 0, waits needing a
  fence (as `drm_syncobj_find_fence`) unless an earlier job of the same call signals it.

Jobs run synchronously. A transfer is executed by an interpreter that follows the job's own
pointers through its VM: pixel-event PDS → EOT program → PBE state; background object → PDS
primary → shader (fill colour) or texture words (copy); the vertices and the render box clip
the rectangle. So a wrong address, encoding or placement shows as wrong pixels or a fault (a
mutation of any builder fails `test_job`). A fault writes nothing, counts in the stats and
loses the context: later submissions get `-ECANCELED`, as with the planned recovery.

What the mock cannot tell: whether the real firmware and hardware accept the programs and
registers. That is what the laptop runs are for.

## Library API in brief

```c
sgxfive_dev_open(be, path_or_NULL, &dev);       /* scans renderD128..191 for "sgxfive" */
sgxfive_dev_info(dev);                          /* GPU, FW, job limits, runtime, heaps */
sgxfive_bo_create(dev, size, flags, &bo); sgxfive_bo_mmap(bo, &p); sgxfive_bo_destroy(bo);
sgxfive_vm_create(dev, &vm); sgxfive_vm_alloc(vm, heap, size, align, &addr);
sgxfive_vm_map(vm, addr, bo, offset, size, flags); sgxfive_vm_unmap(vm, addr, size);
sgxfive_ctx_create(vm, &ctx); sgxfive_ctx_destroy(dev, ctx);
sgxfive_syncobj_create(dev, signaled, &h); sgxfive_syncobj_wait(dev, h, point, timeout_ns);
sgxfive_submit(dev, jobs, n, &failed); sgxfive_submit_transfer(dev, ctx, flags, cmd, w, s);

sgxfive_xfer_create(vm, ctx, program_source, &x);
sgxfive_xfer_fill(x, &dst, &rect, colour, signal);
sgxfive_xfer_copy(x, &src, sx, sy, &dst, &rect, signal);
sgxfive_xfer_wait(x, timeout_ns);
```

Functions return 0 or a negative errno.

### Program sources

The kernel builds no GPU programs: userspace places them in its own VM (design §4.4). The
engine takes them from a `struct sgxfive_prog_source`, two callbacks over the ids, metadata
and parameters of the firmware file's transfer-queue templates (`design/sgx545_fw_format.md`
§9, §10):

- `meta(id)` → heap, temporaries, attributes, size, alignment, PDS data size;
- `build(id, env)` → the finished image, given the parameters (colour, PBE words, texture
  words) and where everything was placed (addresses and DOUTU words of the USE programs).

The engine places the seven programs of a fill or copy in a private three-page BO mapped into
the context's VM (PDS_PIXEL, USSE_PIXEL and GENERAL pages, 16-byte slots, which reproduces the
debugfs recipes), computes the DOUTU words, builds the background object and the register
block, and submits. One job is in flight per engine.

Today there is one source, `sgxfive_prog_fake_create()`: a test double that writes the words
the design documents print and leaves the closed bodies it does not know (USE secondary,
pixel-event PDS code) zero. Its images must never reach a GPU; the CLI refuses `fill`, `copy`
and `isolation` without `--mock`. Two real sources plug into the same interface later:

1. a `.fw` source reading the user's `sgx545_ukernel.fw` (format v3): `meta` from `TQ_META`,
   `build` by applying `TQ_PATCHES`, as `tools/fwcutter/fwload.py` and the kernel's
   `sgx_fw_tq_build()` already do;
2. an assembler source built on `tools/usse-asm/` once it reproduces the nine templates.

## sgxfive-test

```
sgxfive-test [--mock[=rev1|harness|none]] [--device PATH] <command> [options]

query                          GPU, firmware, job limits and heaps
dummy [-n N]                   N dummy transfers chained through one syncobj
fill  [--width W --height H --x X --y Y --stride S --colour C]
copy  [--width W --height H --sx X --sy Y --src-stride S --dx X --dy Y --dst-stride S]
isolation                      kernel-heap-unmappable, same-address-separate-vms,
                               other-vm-address-faults, lost-context-cancelled,
                               other-context-survives, read-only-faults
```

Exit status 0 when every check passes, 1 on a failed check or call, 2 on usage errors. Each
subcommand mirrors a debugfs file of the bring-up: `dummy` ↔ `xfer_dummy`/`xfer_sync`, `fill` ↔
`xfer_fill` (sentinel-filled destination plus a guard page, every dword checked), `copy` ↔
`xfer_copy` (a source in which every byte varies), `isolation` ↔ `xfer_iso`, `alias_ro`,
`xfer_ro`.

## Against the real kernel

Once the render node lands (plan track T3, design §8 P3–P6) the same binary runs on the laptop
without `--mock`: `query` from P3, `dummy` from P5, `fill`/`copy`/`isolation` from P6 when a
real program source exists. The intent is that each mock rule becomes a kernel KUnit test and
each test here an IGT-style test: the program-free ones (queries, BOs, VM rules, dummy jobs,
syncobjs, every `-EINVAL` path) port directly to `igt-gpu-tools/tests/sgxfive/` with raw
ioctls, as `tests/panfrost` and `tests/v3d` do; the fill, copy and isolation tests need
programs and will use the assembler source there, since IGT cannot ship closed bytes.
