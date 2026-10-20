/* SPDX-License-Identifier: MIT */
/*
 * An in-process mock of the sgxfive render node, behind struct
 * sgxfive_backend: render-node discovery, DEV_QUERY, GEM BOs with mmap, VM
 * contexts with per-mapping permissions, transfer contexts, DRM syncobjs and
 * SUBMIT_JOBS with the kernel's validation rules (design §4.5). Transfers run
 * synchronously on a tiny interpreter that follows the job's own pointers
 * through its VM (pixel-event PDS -> EOT -> PBE state, background object ->
 * PDS primary -> shader and texture words) and writes the pixels, so a job
 * that placed something wrong produces wrong pixels or a GPU fault. A fault
 * writes nothing and loses the context, as on hardware with recovery.
 */
#ifndef SGXFIVE_MOCK_H
#define SGXFIVE_MOCK_H

#include "sgxfive.h"

enum sgxfive_mock_layout {
	SGXFIVE_MOCK_LAYOUT_REV1 = 0,	/* our silicon: host bridge rev 3 */
	SGXFIVE_MOCK_LAYOUT_HARNESS,	/* the debugfs harness addresses (design §3.5) */
};

struct sgxfive_mock_config {
	enum sgxfive_mock_layout layout;
	const char *other_driver;	/* if set, renderD128 is this driver, sgxfive 129 */
	bool no_sgxfive;		/* no sgxfive node at all */
	const char *sgxfive_name;	/* driver name of the sgxfive node; NULL: "sgxfive" */
};

struct sgxfive_mock_stats {
	uint32_t bos, vms, ctxs, syncobjs;	/* live objects */
	uint32_t mappings;
	uint32_t jobs_run, dummies_run, faults;
	uint64_t last_fault_addr;
};

struct sgxfive_mock;

struct sgxfive_mock *sgxfive_mock_create(const struct sgxfive_mock_config *cfg);
void sgxfive_mock_destroy(struct sgxfive_mock *m);
const struct sgxfive_backend *sgxfive_mock_backend(struct sgxfive_mock *m);
void sgxfive_mock_get_stats(const struct sgxfive_mock *m, struct sgxfive_mock_stats *s);

#endif
