// SPDX-License-Identifier: MIT
/*
 * The assembled program source: our own transfer programs
 * (tools/usse-asm/progs/), with the TQ_META records and TQ_PATCHES of the
 * firmware file's transfer-queue templates (design/sgx545_fw_format.md §9-§11).
 * The tables are generated (src/prog_asm_gen.h, make gen); build() is
 * fwload.build_tq() for one program: copy the template, apply its patches in
 * order. tests/test_prog_asm.py checks it against fwload.py byte for byte.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sgxfive_xfer.h"

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

enum { PA_WORD32, PA_USSE_LIMM, PA_EMITPIX_SB };		/* encodings */
enum { PA_CONST, PA_ADDR, PA_PARAM, PA_DOUTU };			/* kinds */

struct prog_asm {
	struct sgxfive_prog_meta meta;
	const uint64_t *usse;		/* USE code, or */
	const uint32_t *pds;		/* PDS data segment and code */
	uint32_t count;			/* words */
};

struct prog_asm_patch {
	uint8_t prog, enc, kind, target;
	uint16_t offset;		/* bytes */
	uint32_t addend;
};

#include "prog_asm_gen.h"

static int asm_meta(struct sgxfive_prog_source *src, uint32_t id, struct sgxfive_prog_meta *m)
{
	if (id >= SGXFIVE_PROG_COUNT)
		return -ENOENT;
	*m = progs[id].meta;
	return 0;
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint64_t get64(const uint8_t *p)
{
	return get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

/* EMITPIX_SB (format §9.4): sideband [5:0] to e[27:22], [11:6] to e[40:35], [13:12] to e[55:54]. */
uint64_t sgxfive_emitpix_sb_set(uint64_t insn, uint32_t sb)
{
	static const uint8_t f[3][3] = { { 22, 6, 0 }, { 35, 6, 6 }, { 54, 2, 12 } };

	for (int i = 0; i < 3; i++) {
		const uint64_t mask = (1ull << f[i][1]) - 1;

		insn = (insn & ~(mask << f[i][0])) | ((sb >> f[i][2]) & mask) << f[i][0];
	}
	return insn;
}

static int value(const struct prog_asm_patch *p, const struct sgxfive_prog_env *env,
		 uint32_t *v)
{
	const struct sgxfive_prog_meta *t;
	int ret;

	switch (p->kind) {
	case PA_CONST:
		*v = p->addend;
		return 0;
	case PA_PARAM:
		*v = env->params[p->target] + p->addend;
		return 0;
	case PA_ADDR:
		if (!env->addr[p->target])
			return -ENOENT;
		*v = (uint32_t)(env->addr[p->target] + p->addend);
		return 0;
	case PA_DOUTU:
		/*
		 * Check the placement, then take the engine's word: it carries the
		 * temporaries of the metadata the engine used, which a source
		 * wrapping this one may have changed.
		 */
		t = &progs[p->target].meta;
		if (!env->addr[p->target])
			return -ENOENT;
		ret = sgxfive_doutu(env->addr[p->target] + p->addend, env->heap_base[t->heap],
				    t->temps, v);
		if (!ret && !p->addend)
			*v = env->doutu[p->target];
		return ret;
	}
	return -EINVAL;
}

static int asm_build(struct sgxfive_prog_source *src, uint32_t id,
		     const struct sgxfive_prog_env *env, void *out)
{
	const struct prog_asm *pr;
	uint8_t *o = out;
	uint32_t v;
	int ret;

	if (id >= SGXFIVE_PROG_COUNT)
		return -ENOENT;
	pr = &progs[id];
	memset(o, 0, pr->meta.size);
	for (uint32_t i = 0; i < pr->count; i++) {
		if (pr->usse)
			put64(o + 8 * i, pr->usse[i]);
		else
			put32(o + 4 * i, pr->pds[i]);
	}
	for (size_t k = 0; k < ARRAY_SIZE(patches); k++) {
		const struct prog_asm_patch *p = &patches[k];

		if (p->prog != id)
			continue;
		ret = value(p, env, &v);
		if (ret)
			return ret;
		if (p->enc == PA_WORD32)
			put32(o + p->offset, v);
		else if (p->enc == PA_USSE_LIMM)
			put64(o + p->offset, sgxfive_limm_set(get64(o + p->offset), v));
		else
			put64(o + p->offset, sgxfive_emitpix_sb_set(get64(o + p->offset), v));
	}
	return 0;
}

static void asm_destroy(struct sgxfive_prog_source *src)
{
	free(src);
}

static const struct sgxfive_prog_source_ops asm_ops = {
	.meta = asm_meta, .build = asm_build, .destroy = asm_destroy,
};

struct sgxfive_prog_source *sgxfive_prog_asm_create(void)
{
	struct sgxfive_prog_source *src = calloc(1, sizeof(*src));

	if (src) {
		src->ops = &asm_ops;
		src->name = "asm";
	}
	return src;
}
