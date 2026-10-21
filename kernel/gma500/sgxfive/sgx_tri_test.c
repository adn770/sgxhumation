// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgx_tri.c: the first triangle's TA + 3D job. Field
 * placement is checked against the dword offsets design/sgx545_3d.md §4-§12
 * cites (CMDTA dwN, rd dwN, rtdata dwN, ctx dwN, pb dwN), the values against
 * §12.6 and §13 and the REV1 layout the debugfs test uses.
 */
#include <kunit/test.h>
#include <linux/module.h>

#include "sgx_fw.h"
#include "sgx_tri.h"

static const struct sgx_tri_addrs addrs = {
	.pd_phys = 0x039ea000, .param_base = 0x10000000, .pb = 0x10001000,
	.pt = 0x10423000, .prim = 0x10463000, .bg = 0x10464000,
	.tadata = 0x20001000, .ccb = 0x32140000, .kd = 0x32152000,
	.pdsv = 0x30401000, .usev = 0x3c001000, .pdsp = 0x2e001000,
	.usep = 0x37001000, .gen = 0x3e001000, .fb = 0x3e010000,
	.pds_exec_base = 0x2e000000, .usep_base = 0x37000000,
	.usev_base = 0x3c000000,
};

#define KD(off)		(addrs.kd + (off))
#define WORD(i)		KD(SGX_TRI_KD_WORDS + 4 * (i))

static u32 dw(const void *p, unsigned int off)
{
	const u8 *b = p;

	return b[off] | b[off + 1] << 8 | b[off + 2] << 16 | (u32)b[off + 3] << 24;
}

/* Every offset the EDM trace names (§12.2-§12.4) lands on our field. */
static void sgx_tri_test_offsets(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, SGX_TRI_CMD_VPART, 4 * 5);	/* CMDTA dw5 */
	KUNIT_EXPECT_EQ(test, SGX_TRI_CMD_RD, 4 * 14);
	KUNIT_EXPECT_EQ(test, SGX_TRI_CMD_DSTSYNC, 4 * 15);
	KUNIT_EXPECT_EQ(test, SGX_TRI_CMD_RTDATA, 4 * 17);
	KUNIT_EXPECT_EQ(test, SGX_TRI_SH_TATQ, 4 * 25);
	KUNIT_EXPECT_EQ(test, SGX_TRI_CMD_TAREGS, 4 * 142);
	KUNIT_EXPECT_EQ(test, SGX_TRI_CMD_3DREGUPD, 4 * 170);
	KUNIT_EXPECT_EQ(test, SGX_TRI_CMD_SIZE, SGX_TRI_CMD_3DREGUPD + 8);
	KUNIT_EXPECT_EQ(test, SGX_TRI_CMD_STRIDE, ALIGN(SGX_TRI_CMD_SIZE, 64));
	KUNIT_EXPECT_EQ(test, SGX_TRI_RD_TQSYNC, 4 * 57);	/* rd dw57 */
	KUNIT_EXPECT_EQ(test, SGX_TRI_RD_NEXTRT, 4 * 66);
	KUNIT_EXPECT_EQ(test, SGX_TRI_RD_BGPRI_LIST, 4 * 69);
	KUNIT_EXPECT_EQ(test, SGX_TRI_RD_TESTATE, 4 * 73);
	KUNIT_EXPECT_EQ(test, SGX_TRI_RD_ACCESS, 4 * 52);	/* after 52 registers */
	KUNIT_EXPECT_EQ(test, SGX_TRI_RD_DEPTHSORT, 4 * 51);
	KUNIT_EXPECT_EQ(test, SGX_TRI_RT_RD, 4 * 1);		/* rtdata dw1 */
	KUNIT_EXPECT_EQ(test, SGX_TRI_RT_STATE, 4 * 5);
	KUNIT_EXPECT_EQ(test, SGX_TRI_RT_CONTROL, 4 * 6);
	KUNIT_EXPECT_EQ(test, SGX_TRI_RT_OTPM, 4 * 7);
	KUNIT_EXPECT_EQ(test, SGX_TRI_RT_COMMON, 4 * 13);
	KUNIT_EXPECT_EQ(test, SGX_TRI_RTDS_RT, 0x18);
	KUNIT_EXPECT_EQ(test, SGX_TRI_RTDS_SIZE, 0x18 + 0x38);
	KUNIT_EXPECT_EQ(test, SGX_TRI_CTX_TACOUNT, 4 * 5);	/* ctx dw5 */
	KUNIT_EXPECT_EQ(test, SGX_TRI_CTX_PBDESC, 4 * 6);
	KUNIT_EXPECT_EQ(test, SGX_TRI_CTX_PARTIAL, 4 * 8);
	KUNIT_EXPECT_EQ(test, SGX_TRI_CTX_COMPLETE, 4 * 10);
	KUNIT_EXPECT_EQ(test, SGX_TRI_PB_ALLOC, 4 * 5);		/* pb dw5 */
	KUNIT_EXPECT_EQ(test, SGX_TRI_PB_TA_THRESH, 4 * 10);
	/* The KD items do not overlap. */
	KUNIT_EXPECT_LE(test, SGX_TRI_KD_CTX + SGX_TRI_CTX_SIZE, SGX_TRI_KD_RTDS);
	KUNIT_EXPECT_LE(test, SGX_TRI_KD_RTDS + SGX_TRI_RTDS_SIZE, SGX_TRI_KD_PBDESC);
	KUNIT_EXPECT_LE(test, SGX_TRI_KD_PBDESC + SGX_TRI_PBDESC_SIZE,
			SGX_TRI_KD_PBBLOCK);
	KUNIT_EXPECT_LE(test, SGX_TRI_KD_WORDS + 4 * SGX_TRI_W_COUNT, SGX_TRI_KD_RD);
	KUNIT_EXPECT_LE(test, SGX_TRI_KD_RD + SGX_TRI_RD_SIZE, SGX_TRI_KD_SIZE);
}

static void sgx_tri_test_f32(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(1, 0), 0x3f800000u);
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(1, 1), 0x3f000000u);
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(-1, 1), 0xbf000000u);
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(32, 0), 0x42000000u);
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(-384, 9), 0xbf400000u);	/* -0.75 */
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(-192, 9), 0xbec00000u);	/* -0.375 */
	KUNIT_EXPECT_EQ(test, sgx_tri_f32(0, 4), 0u);
}

/* DOUTU U0 (design/sgx545_transfer.md §4.2.4) and the pointer words (S.5). */
static void sgx_tri_test_words(struct kunit *test)
{
	/* The capture's vertex program: base 10, instruction 0x11b0. */
	KUNIT_EXPECT_EQ(test, sgx_tri_doutu(0xfc008d80, 0xfc000000, 10, 0), 0x11b0au);
	/* The transfer EOT task: 4 temporaries. */
	KUNIT_EXPECT_EQ(test, sgx_tri_doutu(0xf4000020, 0xf4000000, 0, 4), 0x01000040u);
	/* 16 data bytes = 1 row, offset 0x1040 from PDS_EXEC_BASE. */
	KUNIT_EXPECT_EQ(test, sgx_tri_pix_ptr(&addrs, 0x2e001040, 4), 0x04000104u);
	/* The capture's state copies: 12 dwords in one burst, 18 in 2 x 9. */
	KUNIT_EXPECT_EQ(test, sgx_tri_doutd(12), 0x02c0000bu);
	KUNIT_EXPECT_EQ(test, sgx_tri_doutd(18), 0x02000018u);
	KUNIT_EXPECT_EQ(test, sgx_tri_doutd(2), 0x00400001u);
	KUNIT_EXPECT_EQ(test, sgx_tri_doutd(4), 0x00c00003u);
}

static void sgx_tri_test_ctx(struct kunit *test)
{
	u32 *ctx = kunit_kzalloc(test, SGX_TRI_CTX_SIZE, GFP_KERNEL);
	unsigned int off;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	memset(ctx, 0xa5, SGX_TRI_CTX_SIZE);
	sgx_tri_ctx(ctx, &addrs);
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x00), 1u);		/* new context */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x04), 0u);		/* priority 0 */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x08), 0x039ea000u);
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x0c), 0x32140000u);	/* TA CCB */
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x10), KD(SGX_TRI_KD_CCB_CTL));
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x18), KD(SGX_TRI_KD_PBDESC));
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x1c), 0x10463000u);
	KUNIT_EXPECT_EQ(test, dw(ctx, 0x14), 0u);		/* the EDM's */
	for (off = 0x20; off < SGX_TRI_CTX_SIZE; off += 4)
		KUNIT_EXPECT_EQ_MSG(test, dw(ctx, off), 0u, "offset %#x", off);
}

/* §12.4: the capture's descriptor, block and page table for 0x422 pages. */
static void sgx_tri_test_pb(struct kunit *test)
{
	u32 *desc = kunit_kzalloc(test, SGX_TRI_PBDESC_SIZE, GFP_KERNEL);
	u32 *block = kunit_kzalloc(test, SGX_TRI_PBBLOCK_SIZE, GFP_KERNEL);
	u32 *pt = kunit_kzalloc(test, SGX_TRI_PT_BYTES, GFP_KERNEL);
	static const u32 want[] = {
		0, 0x422, 0x04220001, 0x04210000, 0x10423000, 0, 0x04220001,
		0x04210000, 0, 0, 0x432, 0x400, 0x400, 0x300,
	};
	unsigned int i, nonzero = 0;

	KUNIT_ASSERT_NOT_NULL(test, pt);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_ASSERT_NOT_NULL(test, block);
	memset(pt, 0xa5, SGX_TRI_PT_BYTES);
	sgx_tri_pb(desc, block, pt, &addrs);
	for (i = 0; i < ARRAY_SIZE(want); i++)
		KUNIT_EXPECT_EQ_MSG(test, desc[i], want[i], "dword %u", i);
	KUNIT_EXPECT_EQ(test, desc[14], KD(SGX_TRI_KD_PBBLOCK));
	KUNIT_EXPECT_EQ(test, block[0], 0x422u);
	KUNIT_EXPECT_EQ(test, block[1], 0x04220001u);
	KUNIT_EXPECT_EQ(test, block[2], KD(SGX_TRI_KD_PBDESC));
	KUNIT_EXPECT_EQ(test, block[3], 0u);
	KUNIT_EXPECT_EQ(test, pt[0], 0u);
	KUNIT_EXPECT_EQ(test, pt[1], 0xffff0002u);
	KUNIT_EXPECT_EQ(test, pt[2], 0x00010003u);
	KUNIT_EXPECT_EQ(test, pt[0x421], 0x04200422u);
	KUNIT_EXPECT_EQ(test, pt[0x422], 0x0421ffffu);
	for (i = 0; i < SGX_TRI_PT_BYTES / 4; i++)
		nonzero += pt[i] != 0;
	KUNIT_EXPECT_EQ(test, nonzero, 0x422u);
}

static void sgx_tri_test_rtds(struct kunit *test)
{
	u32 *r = kunit_kzalloc(test, SGX_TRI_RTDS_SIZE, GFP_KERNEL);
	const u8 *rt;

	KUNIT_ASSERT_NOT_NULL(test, r);
	memset(r, 0xa5, SGX_TRI_RTDS_SIZE);
	sgx_tri_rtds(r, &addrs);
	rt = (const u8 *)r + SGX_TRI_RTDS_RT;
	KUNIT_EXPECT_EQ(test, dw(r, 0x00), WORD(SGX_TRI_W_PENDING));
	KUNIT_EXPECT_EQ(test, dw(r, 0x04), 0u);
	KUNIT_EXPECT_EQ(test, dw(r, 0x08), 8u);			/* MTileWidth */
	KUNIT_EXPECT_EQ(test, dw(r, 0x0c), 0u);
	KUNIT_EXPECT_EQ(test, dw(r, 0x10), 0u);
	KUNIT_EXPECT_EQ(test, dw(r, 0x14), 1u);			/* one RT data */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x00), KD(SGX_TRI_KD_CTX));
	KUNIT_EXPECT_EQ(test, dw(rt, 0x04), 0u);		/* the EDM's */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x08), 1u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x0c), 0xc0u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x10), 0x10464000u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x14), 0x20004000u);	/* DPM state */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x18), 0x20002000u);	/* DPM control */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x1c), 0x20003000u);	/* OTPM */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x20), 0x20001000u);	/* tail pointers */
	KUNIT_EXPECT_EQ(test, dw(rt, 0x24), 0x100u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x28), 1u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x2c), 0u);
	KUNIT_EXPECT_EQ(test, dw(rt, 0x30), WORD(SGX_TRI_W_RTSTATUS));
	KUNIT_EXPECT_EQ(test, dw(rt, 0x34), 0x10u);		/* free */
}

/* §12.6 "render details": the captured 3D registers, our addresses. */
static void sgx_tri_test_rd(struct kunit *test)
{
	u32 *rd = kunit_kzalloc(test, SGX_TRI_RD_SIZE, GFP_KERNEL);
	static const struct { u16 off; u32 val; } want[] = {
		{ 0x000, 0x10000000 }, { 0x008, 0x80 }, { 0x00c, 0x000f0000 },
		{ 0x010, 0x80000000 }, { 0x020, 0x20005000 }, { 0x028, 0x20006000 },
		{ 0x038, 0x80000000 }, { 0x03c, 0x80000000 }, { 0x040, 0x3f800000 },
		{ 0x044, 0x300 }, { 0x048, 0x46400 }, { 0x050, 0x88 }, { 0x058, 0x30 },
		{ 0x05c, 0x2e001080 }, { 0x060, 5 }, { 0x064, 1 }, { 0x068, 1 },
		{ 0x0cc, 0x100 }, { 0x0d0, 0x32152408 }, { 0x0d8, 0x32152200 },
		{ 0x0dc, 0x32152218 }, { 0x104, 1 }, { 0x114, 0x32152414 },
		{ 0x13c, 0x07c00000 },
	};
	unsigned int i, j;

	KUNIT_ASSERT_NOT_NULL(test, rd);
	memset(rd, 0xa5, SGX_TRI_RD_SIZE);
	sgx_tri_rd(rd, &addrs);
	for (i = 0; i < SGX_TRI_RD_SIZE; i += 4) {
		u32 v = 0;

		for (j = 0; j < ARRAY_SIZE(want); j++)
			if (want[j].off == i)
				v = want[j].val;
		KUNIT_EXPECT_EQ_MSG(test, dw(rd, i), v, "offset %#x", i);
	}
}

static void sgx_tri_test_words_area(struct kunit *test)
{
	u32 w[SGX_TRI_W_COUNT];

	memset(w, 0xa5, sizeof(w));
	sgx_tri_words(w, &addrs);
	KUNIT_EXPECT_EQ(test, w[SGX_TRI_W_TA], 0u);
	KUNIT_EXPECT_EQ(test, w[SGX_TRI_W_3D], 0u);
	KUNIT_EXPECT_EQ(test, w[SGX_TRI_W_RTSTATUS], 0u);
	/* The background's primary PDS pointer, for background dword 2. */
	KUNIT_EXPECT_EQ(test, w[SGX_TRI_W_BGPRI], 0x04000102u);
}

/* §12.6 "SGXMKIF_CMDTA": the single-kick form. */
static void sgx_tri_test_cmd(struct kunit *test)
{
	static const struct sgx_tri_kick k = {
		.flags = SGX_TRI_TA_FIRST | SGX_TRI_TA_LAST | SGX_TRI_TA_INVAL,
		.render_flags = SGX_TRI_RENDER_SCENE, .vdm = 0x3e001000,
		.te_psg = SGX_TRI_TEPSG_END, .frame = 7, .ta_val = 0x7a01,
		.td_val = 0x3d01,
	};
	static const struct { u16 off; u32 val; } want[] = {
		{ 0x000, 0x2c0 }, { 0x004, 0x73 }, { 0x008, 0x04000104 },
		{ 0x00c, 0x00030000 }, { 0x010, 0x04000102 }, { 0x014, 0x50000000 },
		{ 0x018, 0x50000000 }, { 0x01c, 0x400 }, { 0x020, 7 },
		{ 0x024, 0x32152300 }, { 0x038, 0x32152800 }, { 0x040, 0x32152200 },
		{ 0x044, 0x32152218 },
		{ 0x058, 1 }, { 0x05c, 1 }, { 0x060, 1 },		/* READY, 1 + 1 */
		{ 0x118, 0x32152400 }, { 0x11c, 0x7a01 },		/* TA status */
		{ 0x218, 0x32152404 }, { 0x21c, 0x3d01 },		/* 3D status */
		{ 0x238, 0x20000000 }, { 0x23c, 0x01004004 }, { 0x240, 0x01004004 },
		{ 0x244, 0x3001 }, { 0x248, 0x10 }, { 0x24c, 0x400000 },
		{ 0x250, 0x3e001000 }, { 0x254, 0xa2 }, { 0x258, 0x20005000 },
		{ 0x25c, 0x20001000 }, { 0x260, 0x358637bd }, { 0x264, 0x358637bd },
		{ 0x268, 0x003f003f }, { 0x274, 0x10000000 }, { 0x2a4, 0x88 },
	};
	u8 *cmd = kunit_kzalloc(test, SGX_TRI_CMD_STRIDE, GFP_KERNEL);
	unsigned int i, j;

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	memset(cmd, 0xa5, SGX_TRI_CMD_STRIDE);
	KUNIT_EXPECT_EQ(test, sgx_tri_cmd(cmd, &addrs, &k), (u32)SGX_TRI_CMD_STRIDE);
	for (i = 0; i < SGX_TRI_CMD_SIZE; i += 4) {
		u32 v = 0;

		for (j = 0; j < ARRAY_SIZE(want); j++)
			if (want[j].off == i)
				v = want[j].val;
		KUNIT_EXPECT_EQ_MSG(test, dw(cmd, i), v, "offset %#x", i);
	}
}

/* S.2, S.9: the triangle's 18-word state block and the end of scene. */
static void sgx_tri_test_blocks(struct kunit *test)
{
	static const u32 want[SGX_TRI_BLOCK_WORDS] = {
		0xdf43, 0x01d00200, 0x10f0, 0x04000104, 0x00030001, 0x04000100,
		0x42000000, 0x42000000, 0x42000000, 0x42000000, 0x3f000000,
		0x3f000000, 0, 0x43000, 0x3727c5ac, 0x01555620, 0, 0,
	};
	u32 blk[SGX_TRI_BLOCK_WORDS], eos[SGX_TRI_EOS_WORDS];
	unsigned int i, n = 0;

	sgx_tri_block(blk, &addrs);
	for (i = 0; i < SGX_TRI_BLOCK_WORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, blk[i], want[i], "word %u", i);
	/* The presence word announces exactly the words that follow. */
	for (i = 0; i < 16; i++)
		if (blk[0] & BIT(i))
			n += i == 6 ? 3 : i == 7 ? 2 : i == 8 ? 6 : 1;
	KUNIT_EXPECT_EQ(test, n + 1, (unsigned int)SGX_TRI_BLOCK_WORDS);
	sgx_tri_eos(eos);
	KUNIT_EXPECT_EQ(test, eos[0], 0x2000u);
	KUNIT_EXPECT_EQ(test, eos[1], 0x00010001u);	/* 64 x 64: blocks 0-1 */
}

/* S.1: state, index list with its vertex task, end of scene, terminate. */
static void sgx_tri_test_vdm(struct kunit *test)
{
	static const u32 want[SGX_TRI_VDM_WORDS] = {
		0x43040104, 0x0a022405,			/* state: pds_mte, 5 rows */
		0x80c00003, 0x3e001200, 0xf0000000, 0x003fffff,
		0xf3040100, 0x01c00082,			/* vertex task: pds_vtx */
		0x63040108, 0x0a022401,			/* last task: end of scene */
		0xc0000000,
	};
	static const u32 want1[SGX_TRI_VDM1_WORDS] = {
		0x63040108, 0x0a022401, 0xc0000000,
	};
	u32 vdm[SGX_TRI_VDM_WORDS], vdm1[SGX_TRI_VDM1_WORDS];
	unsigned int i;

	sgx_tri_vdm(vdm, &addrs);
	for (i = 0; i < SGX_TRI_VDM_WORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, vdm[i], want[i], "word %u", i);
	sgx_tri_vdm1(vdm1, &addrs);
	for (i = 0; i < SGX_TRI_VDM1_WORDS; i++)
		KUNIT_EXPECT_EQ_MSG(test, vdm1[i], want1[i], "word %u", i);
}

static u64 qw(const u32 *p, unsigned int insn)
{
	return p[2 * insn] | (u64)p[2 * insn + 1] << 32;
}

/* The programs in place, with their patched fields. */
static void sgx_tri_test_scene(struct kunit *test)
{
	struct sgx_tri_cpu c = {
		.pdsv = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
		.usev = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
		.pdsp = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
		.usep = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
		.gen = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
		.bg = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL),
	};
	const u32 *pv, *pp, *g;
	u64 eot;

	KUNIT_ASSERT_NOT_NULL(test, c.pdsv);
	KUNIT_ASSERT_NOT_NULL(test, c.usev);
	KUNIT_ASSERT_NOT_NULL(test, c.pdsp);
	KUNIT_ASSERT_NOT_NULL(test, c.usep);
	KUNIT_ASSERT_NOT_NULL(test, c.gen);
	KUNIT_ASSERT_NOT_NULL(test, c.bg);
	sgx_tri_scene(&c, &addrs, 0xff00ff00, 0xff203040);

	/* pds_vtx: vertex buffer, doutd of 4 dwords, stride 16, U0. */
	pv = c.pdsv + SGX_TRI_PV_VTX / 4;
	KUNIT_EXPECT_EQ(test, pv[0], 0x3e001100u);
	KUNIT_EXPECT_EQ(test, pv[1], 3u);
	KUNIT_EXPECT_EQ(test, pv[2], 16u);
	KUNIT_EXPECT_EQ(test, pv[4], 0x200au);
	KUNIT_EXPECT_EQ(test, pv[8], 0x67800072u);	/* code follows */
	/* pds_mte for the block and for the end of scene. */
	pv = c.pdsv + SGX_TRI_PV_MTE / 4;
	KUNIT_EXPECT_EQ(test, pv[0], 0x30401100u);
	KUNIT_EXPECT_EQ(test, pv[1], 0x02000018u);
	KUNIT_EXPECT_EQ(test, pv[4], 0x210au);
	pv = c.pdsv + SGX_TRI_PV_EOS / 4;
	KUNIT_EXPECT_EQ(test, pv[0], 0x30401180u);
	KUNIT_EXPECT_EQ(test, pv[1], 0x00400001u);
	KUNIT_EXPECT_EQ(test, pv[4], 0x220au);
	KUNIT_EXPECT_EQ(test, c.pdsv[SGX_TRI_PV_BLOCK / 4], 0xdf43u);
	KUNIT_EXPECT_EQ(test, c.pdsv[SGX_TRI_PV_EOSBLOCK / 4], 0x2000u);
	/* use_vtx: the next phase is instruction 7 of the program at 0x1000. */
	KUNIT_EXPECT_EQ(test, qw(c.usev, 0), 0xfa44020000000207ull);
	KUNIT_EXPECT_EQ(test, qw(c.usev + SGX_TRI_UV_MTE18 / 4, 9),
			0xfb2f4000a0200000ull);		/* emitmtest.end */

	/* Pixel PDS: U0 words of the shaders, the pixel-event dispatch. */
	pp = c.pdsp;
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_PRIM / 4], 0x2000u);
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_PRIM / 4 + 1], 4u);
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_BGPRIM / 4], 0x2020u);
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_SEC / 4], 0x2040u);
	KUNIT_EXPECT_EQ(test, pp[SGX_TRI_PP_SEC / 4 + 1], 4u);
	pp = c.pdsp + SGX_TRI_PP_PIXEV / 4;
	KUNIT_EXPECT_EQ(test, pp[0], 0x2060u);		/* PTOFF */
	KUNIT_EXPECT_EQ(test, pp[1], 0u);
	KUNIT_EXPECT_EQ(test, pp[3], 0x01002080u);	/* EOT, 4 temps */
	KUNIT_EXPECT_EQ(test, pp[4], 0x01002100u);	/* EOR, 1 temp */
	KUNIT_EXPECT_EQ(test, pp[5], 0x00c00000u);
	KUNIT_EXPECT_EQ(test, pp[12], 0x00800000u);
	KUNIT_EXPECT_EQ(test, pp[6], 8u);		/* template constants kept */

	/* The colours and the PBE words. */
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_FILL / 4, 1)),
			0xff00ff00u);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_BGFILL / 4, 1)),
			0xff203040u);
	KUNIT_EXPECT_EQ(test, qw(c.usep + SGX_TRI_UP_EOT / 4, 1), 0xfca0000000000000ull);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_EOT / 4, 2)),
			0x003f003fu);
	KUNIT_EXPECT_EQ(test, (qw(c.usep + SGX_TRI_UP_EOT / 4, 2) >> 21) & 0x7f, 1ull);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_EOT / 4, 3)),
			0x001f8000u);
	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(qw(c.usep + SGX_TRI_UP_EOT / 4, 4)),
			0x3e010000u);
	KUNIT_EXPECT_EQ(test, (qw(c.usep + SGX_TRI_UP_EOT / 4, 4) >> 21) & 0x7f, 3ull);
	eot = qw(c.usep + SGX_TRI_UP_EOT / 4, 6);
	KUNIT_EXPECT_EQ(test, eot, sgx_fw_emitpix_sb_set(0xfb24000000208180ull, 0x200));

	/* Geometry: three clip-space vertices with w = 1, indices 0 1 2. */
	g = c.gen;
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 0], 0xbf400000u);	/* -0.75 */
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 1], 0xbf400000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 2], 0u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 3], 0x3f800000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 4], 0x3f400000u);	/* 0.75 */
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 5], 0xbec00000u);	/* -0.375 */
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 8], 0xbec00000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VTX / 4 + 9], 0x3f400000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_IDX / 4], 0x00010000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_IDX / 4 + 1] & 0xffff, 2u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VDM / 4 + 10], 0xc0000000u);
	KUNIT_EXPECT_EQ(test, g[SGX_TRI_G_VDM1 / 4 + 2], 0xc0000000u);

	/* The background object: the full surface, zero layers. */
	KUNIT_EXPECT_EQ(test, c.bg[0], 0x04000104u);
	KUNIT_EXPECT_EQ(test, c.bg[1], 0x00030000u);
	KUNIT_EXPECT_EQ(test, c.bg[2], 0x04000102u);
	KUNIT_EXPECT_EQ(test, c.bg[9], 0x00100000u);	/* (0, 0) */
}

/* The CPU reference: (8, 8), (56, 20), (20, 56) in pixels, edges excluded. */
static void sgx_tri_test_expect(struct kunit *test)
{
	unsigned int x, y, in = 0, edge = 0;

	KUNIT_EXPECT_EQ(test, sgx_tri_expect(28, 28), 1);
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(2, 2), 0);
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(60, 60), 0);
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(50, 50), 0);
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(37, 38), -1);	/* on x + y = 76 */
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(30, 13), -1);	/* on v0-v1 */
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(30, 10), 0);	/* 3 px off it */
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(50, 22), 1);	/* not mirrored: */
	KUNIT_EXPECT_EQ(test, sgx_tri_expect(50, 41), 0);	/* y = 63 - y */
	for (y = 0; y < SGX_TRI_H; y++) {
		for (x = 0; x < SGX_TRI_W; x++) {
			int e = sgx_tri_expect(x, y);

			in += e == 1;
			edge += e == -1;
		}
	}
	/* The area is 1080 pixels; the edges hold about 2 px per unit length. */
	KUNIT_EXPECT_LT(test, in, 1080u);
	KUNIT_EXPECT_GT(test, in + edge, 1080u);
	KUNIT_EXPECT_GT(test, in, 800u);
}

static struct kunit_case sgx_tri_test_cases[] = {
	KUNIT_CASE(sgx_tri_test_offsets),
	KUNIT_CASE(sgx_tri_test_f32),
	KUNIT_CASE(sgx_tri_test_words),
	KUNIT_CASE(sgx_tri_test_ctx),
	KUNIT_CASE(sgx_tri_test_pb),
	KUNIT_CASE(sgx_tri_test_rtds),
	KUNIT_CASE(sgx_tri_test_rd),
	KUNIT_CASE(sgx_tri_test_words_area),
	KUNIT_CASE(sgx_tri_test_cmd),
	KUNIT_CASE(sgx_tri_test_blocks),
	KUNIT_CASE(sgx_tri_test_vdm),
	KUNIT_CASE(sgx_tri_test_scene),
	KUNIT_CASE(sgx_tri_test_expect),
	{}
};

static struct kunit_suite sgx_tri_test_suite = {
	.name = "gma500_sgx_tri",
	.test_cases = sgx_tri_test_cases,
};

kunit_test_suite(sgx_tri_test_suite);
