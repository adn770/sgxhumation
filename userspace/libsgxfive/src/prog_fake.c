// SPDX-License-Identifier: MIT
/*
 * The fake program source: every transfer program rebuilt from the words the
 * design documents print (design/sgx545_transfer.md §4.2.4, §4.2.7, §4.4.3,
 * §4.4.5) and the metadata of the firmware file's TQ_META records
 * (design/sgx545_fw_format.md §9, §10). The bodies the documents do not give
 * (the USE secondary's nop and the pixel-event PDS code) are zero, so these
 * images are for the mock only.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sgxfive_xfer.h"

#define PHAS_LO		0x00000000u	/* phas #0, #0, pixel, end, parallel */
#define PHAS_HI		0xfa440700u
#define MOVS_DOUTU	0x07000305u	/* movs doutu, ds0[0], ds0[1], ds1[0] */
#define MOVS_DOUTI	0x07040202u	/* movs douti, ds0[2], ds0[3] */
#define MOVS_DOUTT	0x07082364u	/* movs doutt, ds0[4], ds0[5], ds1[2], ds1[3] */
#define PDS_HALT	0xaf000000u
#define DOUTU1_PIXEL	0x00000004u	/* SDSOFT */
#define DOUTU1_TEXTURE	0x02000004u	/* SDSOFT | TEXTUREDEPENDENCY */
#define DOUTU1_EOT	0x00800000u	/* PBENABLE */
#define DOUTU1_EOR	0x00c00000u	/* PBENABLE | ENDOFRENDER */

#define U DRM_SGXFIVE_HEAP_USSE_PIXEL
#define P DRM_SGXFIVE_HEAP_PDS_PIXEL
#define NS SGXFIVE_PROG_NO_STRADDLE

static const struct sgxfive_prog_meta meta[SGXFIVE_PROG_COUNT] = {
	[SGXFIVE_PROG_USE_FILL]		= { U, 0, 0, 0x10, 8, 0, NS },
	[SGXFIVE_PROG_USE_SEC]		= { U, 0, 0, 0x10, 8, 0, NS },
	[SGXFIVE_PROG_USE_EOT]		= { U, 4, 0, 0x38, 8, 0, NS },
	[SGXFIVE_PROG_USE_EOR]		= { U, 1, 0, 0x18, 8, 0, NS },
	[SGXFIVE_PROG_PDS_PRIM]		= { P, 0, 0, 0x18, 16, 0x10, NS },
	[SGXFIVE_PROG_PDS_SEC]		= { P, 0, 0, 0x18, 16, 0x10, NS },
	[SGXFIVE_PROG_PDS_PIXEVENT]	= { P, 0, 1, 0xc4, 16, 0x50, NS },
	[SGXFIVE_PROG_USE_COPY]		= { U, 0, 0, 0x10, 8, 0, NS },
	[SGXFIVE_PROG_PDS_PRIM_1SRC]	= { P, 0, 1, 0x40, 16, 0x30, NS },
};

static int fake_meta(struct sgxfive_prog_source *src, uint32_t id, struct sgxfive_prog_meta *m)
{
	if (id >= SGXFIVE_PROG_COUNT)
		return -ENOENT;
	*m = meta[id];
	return 0;
}

static void put_insn(uint32_t *w, unsigned int i, uint64_t insn)
{
	w[2 * i] = (uint32_t)insn;
	w[2 * i + 1] = (uint32_t)(insn >> 32);
}

/* mov.skipinv r<reg>, #imm */
static uint64_t mov_imm(unsigned int reg, uint32_t imm)
{
	return sgxfive_limm_set((uint64_t)0xfca00000 << 32 | reg << 21, imm);
}

/* emitpix2.end.freep #0, r2, r3, r0, #sb: the sideband split (fw_format §9). */
static uint64_t emitpix2(uint32_t sb)
{
	uint32_t lo = ((sb << 22) & 0x0fc00000) | 0x00208180;
	uint32_t hi = 0xfb240000 | ((sb >> 3) & 0x1f8) | ((sb << 10) & 0xc00000);

	return (uint64_t)hi << 32 | lo;
}

static void pds_kick(uint32_t *w, uint32_t u0, uint32_t u1)
{
	w[0] = u0;
	w[1] = u1;
	w[2] = 0;
	w[3] = 0;
	w[4] = MOVS_DOUTU;
	w[5] = PDS_HALT;
}

static int fake_build(struct sgxfive_prog_source *src, uint32_t id,
		      const struct sgxfive_prog_env *env, void *out)
{
	const uint32_t *pa = env->params;
	uint32_t w[0xc4 / 4];

	if (id >= SGXFIVE_PROG_COUNT)
		return -ENOENT;
	memset(w, 0, sizeof(w));
	switch (id) {
	case SGXFIVE_PROG_USE_FILL:
		put_insn(w, 0, (uint64_t)PHAS_HI << 32 | PHAS_LO);
		put_insn(w, 1, sgxfive_limm_set((uint64_t)0xfca40001 << 32,
						pa[SGXFIVE_PARAM_COLOUR]));
		break;
	case SGXFIVE_PROG_USE_SEC:
		put_insn(w, 0, (uint64_t)PHAS_HI << 32 | PHAS_LO);
		break;
	case SGXFIVE_PROG_USE_EOT:
		put_insn(w, 0, (uint64_t)PHAS_HI << 32 | PHAS_LO);
		for (unsigned int i = 0; i < 4; i++)
			put_insn(w, 1 + i, mov_imm(i, pa[SGXFIVE_PARAM_PBE0 + i]));
		put_insn(w, 5, (uint64_t)0xfb200100 << 32 | 0x00200080);  /* emitpix1.freep */
		put_insn(w, 6, emitpix2(pa[SGXFIVE_PARAM_SIDEBAND]));
		break;
	case SGXFIVE_PROG_USE_EOR:
		put_insn(w, 0, (uint64_t)PHAS_HI << 32 | PHAS_LO);
		put_insn(w, 1, mov_imm(0, 0));
		put_insn(w, 2, (uint64_t)0xfb260040 << 32 | 0x80200000);
		break;
	case SGXFIVE_PROG_PDS_PRIM:
		pds_kick(w, env->doutu[SGXFIVE_PROG_USE_FILL], DOUTU1_PIXEL);
		break;
	case SGXFIVE_PROG_PDS_SEC:
		pds_kick(w, env->doutu[SGXFIVE_PROG_USE_SEC], DOUTU1_PIXEL);
		break;
	case SGXFIVE_PROG_PDS_PIXEVENT:
		w[0] = env->doutu[SGXFIVE_PROG_USE_EOT];
		w[1] = DOUTU1_EOT;
		w[3] = env->doutu[SGXFIVE_PROG_USE_EOT];
		w[4] = env->doutu[SGXFIVE_PROG_USE_EOR];
		w[5] = DOUTU1_EOR;
		w[12] = DOUTU1_EOT;
		break;
	case SGXFIVE_PROG_USE_COPY:
		put_insn(w, 0, (uint64_t)PHAS_HI << 32 | PHAS_LO);
		put_insn(w, 1, (uint64_t)0x28851001 << 32 | 0xa0000000);  /* mov.end o0, pa0 */
		break;
	case SGXFIVE_PROG_PDS_PRIM_1SRC:
		w[0] = env->doutu[SGXFIVE_PROG_USE_COPY];
		w[1] = DOUTU1_TEXTURE;
		w[4] = 0x40012001;			/* DOUTI: TEXISSUE 1, USEISSUE NONE */
		w[6] = pa[SGXFIVE_PARAM_SRC] & ~3u;	/* T2 */
		w[7] = 0xa0800000;			/* T3 */
		w[8] = pa[SGXFIVE_PARAM_T0];
		w[9] = pa[SGXFIVE_PARAM_T1];
		w[12] = MOVS_DOUTU;
		w[13] = MOVS_DOUTI;
		w[14] = MOVS_DOUTT;
		w[15] = PDS_HALT;
		break;
	}
	memcpy(out, w, meta[id].size);
	return 0;
}

static void fake_destroy(struct sgxfive_prog_source *src)
{
	free(src);
}

static const struct sgxfive_prog_source_ops fake_ops = {
	.meta = fake_meta, .build = fake_build, .destroy = fake_destroy,
};

struct sgxfive_prog_source *sgxfive_prog_fake_create(void)
{
	struct sgxfive_prog_source *src = calloc(1, sizeof(*src));

	if (src) {
		src->ops = &fake_ops;
		src->name = "fake";
	}
	return src;
}
