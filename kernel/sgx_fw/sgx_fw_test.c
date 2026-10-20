// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgx_fw.c, the synthetic cases of tools/fwcutter/test_fwload.py
 * and test_fwformat.py: every expression kind, header and CRC rejection, the
 * layout constraints, a format v2 file with transfer-queue templates
 * (test_fwload.tiny_v2()) and a v3 file with copy templates (tiny_v3()). The
 * real firmware is checked against the reference loader in userspace
 * (tools/fwcutter/test_cloader.py).
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "sgx_fw.h"

/* A buffer or expression record, as fwformat.BUFFER / fwformat.EXPR. */
struct rec {
	u8 b[4];
	u32 w[3];
};

#define BUF(id, heap, rows, size, align) { { id, heap, rows, 0 }, { size, align, 0 } }
#define EXPR(buf, enc, kind, param, off, target, addend) \
	{ { buf, enc, kind, param }, { off, target, addend } }

enum { WORD32, LIMM, CODE_BASE, EMITPIX_SB };
enum {
	CONST, ADDR, ADDR_SHR4, HEAP, PD_PHYS, CODE_WORD, PDS_CTL, PDS_ROWS,
	PARAM, DOUTU
};

/* test_fwload.tiny(): a 16-byte "ukernel", one PDS program, one data buffer. */
static const struct rec tiny_buffers[] = {
	BUF(0, 2, 0, 16, 8), BUF(1, 2, 3, 0x40, 16), BUF(9, 3, 0, 0x20, 32),
};

static const struct rec tiny_patches[] = {
	EXPR(0, LIMM, ADDR_SHR4, 0, 8, 1, 0),
	EXPR(1, WORD32, CODE_WORD, 0, 0, 0x1ac, 0x0800000f),
	EXPR(1, WORD32, PDS_CTL, 0, 4, 1, 0x100),
	EXPR(1, WORD32, PDS_ROWS, 0, 8, 1, 0),
	EXPR(9, WORD32, PD_PHYS, 0, 0, 0, 0),
	EXPR(9, WORD32, ADDR, 0, 4, 9, 0x10),
	EXPR(9, WORD32, CONST, 0, 8, 0, 0x3c0003f),
};

static const struct rec tiny_part2[] = {
	EXPR(0xff, CODE_BASE, HEAP, 3, 0xa48, 2, 0),
	EXPR(0xff, CODE_BASE, HEAP, 1, 0xa10, 4, 0x80000),
};

static const struct rec tiny_dev_data[] = {
	EXPR(0xff, WORD32, HEAP, 0, 12, 6, 0),
};

/*
 * test_fwload.tiny_v2(): template records, patches and command words; the
 * last two records and five patches are the copy templates of tiny_v3().
 */
#define TQ_META(id, heap, temps, attrs, size, align, data, flags) \
	{ { id, heap, temps, attrs }, { size, align, (data) | (flags) << 16 } }

static const struct rec tq_meta[] = {
	TQ_META(0, 4, 0, 0, 0x10, 8, 0, 1),
	TQ_META(2, 4, 4, 0, 0x38, 8, 0, 1),
	TQ_META(4, 6, 0, 0, 0x18, 16, 0x10, 0),
	TQ_META(7, 4, 0, 0, 0x10, 8, 0, 1),
	TQ_META(8, 6, 0, 1, 0x40, 16, 0x30, 1),
};

#define V2_META		3	/* tq_meta[] entries of tiny_v2() */
#define V2_PATCHES	9	/* tq_patches[] entries of tiny_v2() */

static const struct rec tq_patches[] = {
	EXPR(0, LIMM, PARAM, 0, 8, 0, 0),
	EXPR(2, WORD32, CONST, 0, 16, 0, 1u << 21),
	EXPR(2, LIMM, PARAM, 0, 16, 2, 0),
	EXPR(2, EMITPIX_SB, PARAM, 0, 48, 5, 0),
	EXPR(4, WORD32, DOUTU, 0, 0, 0, 0),
	EXPR(4, WORD32, DOUTU, 0, 4, 2, 0),
	EXPR(4, WORD32, HEAP, 0, 8, 6, 0),
	EXPR(4, WORD32, ADDR, 0, 12, 2, 4),
	EXPR(4, WORD32, PARAM, 0, 16, 4, 1),
	EXPR(8, WORD32, DOUTU, 0, 0, 7, 0),
	EXPR(8, WORD32, CONST, 0, 4, 0, 0x02000004),
	EXPR(8, WORD32, PARAM, 0, 24, 8, 0),
	EXPR(8, WORD32, PARAM, 0, 32, 6, 0),
	EXPR(8, WORD32, PARAM, 0, 36, 7, 0),
};

static const struct rec tq_cmd[] = {
	EXPR(0xff, WORD32, CONST, 0, 0x4, 0, 0x200),
	EXPR(0xff, WORD32, ADDR, 0, 0x24, 4, 0),
	EXPR(0xff, WORD32, HEAP, 0, 0x3c, 6, 0),
	EXPR(0xff, WORD32, PARAM, 0, 0x40, 4, 0),
};

static const struct sgx_fw_layout tiny_layout = {
	.addr = { [0] = 0xe8001000, [1] = 0xe800e000, [9] = 0xf0005000 },
	.addr_set = 1u << 0 | 1u << 1 | 1u << 9,
	.heap_base = { [2] = 0xe8000000, [4] = 0xf4000000, [6] = 0xe4000000 },
	.heap_set = 1u << 2 | 1u << 4 | 1u << 6,
	.pd_phys = 0x2345000,
	.tq_addr = { [0] = 0xf4000000, [2] = 0xf4000040, [4] = 0xe4000040 },
	.tq_set = 1u << 0 | 1u << 2 | 1u << 4,
};

/* test_fwload.TINY_V3_LAYOUT: the copy templates placed as well */
static const struct sgx_fw_layout tiny_v3_layout = {
	.addr = { [0] = 0xe8001000, [1] = 0xe800e000, [9] = 0xf0005000 },
	.addr_set = 1u << 0 | 1u << 1 | 1u << 9,
	.heap_base = { [2] = 0xe8000000, [4] = 0xf4000000, [6] = 0xe4000000 },
	.heap_set = 1u << 2 | 1u << 4 | 1u << 6,
	.pd_phys = 0x2345000,
	.tq_addr = { [0] = 0xf4000000, [2] = 0xf4000040, [4] = 0xe4000040,
		     [7] = 0xf4000080, [8] = 0xe4000080 },
	.tq_set = 1u << 0 | 1u << 2 | 1u << 4 | 1u << 7 | 1u << 8,
};

/* transfer §4.2.5: 64 x 32 ARGB8888 at 0xf0200000, 256-byte stride */
static const struct sgx_fw_tq_fill tiny_fill = {
	.dest = 0xf0200000, .width = 64, .height = 32, .stride = 256,
	.colour = 0xff336699,
};

/* transfer §4.4.5: the same surface copied from 0xf0300000 */
static const struct sgx_fw_tq_copy tiny_copy = {
	.src = 0xf0300000, .dst = 0xf0200000, .width = 64, .height = 32,
	.src_stride = 256, .dst_stride = 256,
};

struct ctx {
	u8 file[2048];
	size_t len;
	struct sgx_fw fw;
	u8 img[3][0x40];
	u8 *bufs[SGX_FW_MAX_BUFFERS];
	u8 prog[5][0x40];
	u8 *progs[SGX_FW_TQ_MAX];
	struct sgx_fw_write cmd[4];
};

static void put16(u8 *p, u16 v)
{
	p[0] = v;
	p[1] = v >> 8;
}

static void put32(u8 *p, u32 v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

/* Emit one section at *pos (8-aligned) and its table entry at *ent. */
static void section(u8 *file, u8 **ent, u8 **pos, u16 type, u32 arg,
		    const struct rec *r, unsigned int n, const u8 *raw, u32 len)
{
	unsigned int i;

	put16(*ent, type);
	put16(*ent + 2, type != 1 && type != 6);	/* IMAGE, TQ_TEMPLATE: CUT */
	put32(*ent + 4, *pos - file);
	put32(*ent + 8, r ? n * 16 : len);
	put32(*ent + 12, arg);
	*ent += 16;
	for (i = 0; r && i < n; i++, *pos += 16) {
		memcpy(*pos, r[i].b, 4);
		put32(*pos + 4, r[i].w[0]);
		put32(*pos + 8, r[i].w[1]);
		put32(*pos + 12, r[i].w[2]);
	}
	if (!r) {
		memcpy(*pos, raw, len);
		*pos += (len + 7) & ~7u;
	}
}

static void set_crc(struct ctx *c)
{
	put32(c->file + 28, sgx_fw_crc32(c->file + 32, c->len - 32));
}

/* fwformat.pack() of tiny(), tiny_v2() or tiny_v3(), by format version */
static void pack(struct ctx *c, int version)
{
	static const u8 image[16] = {
		[8] = 0xef, 0xbe, 0x0d, 0x00, 0x50, 0x71, 0x23, 0xfc,
	};
	/* "mov.skipinv.end o0, #0" after a dummy word; an emitpix2 at 48 */
	static const u8 fill[16] = {
		0x11, 0x11, [12] = 0x01, 0x00, 0xa4, 0xfc,
	};
	static const u8 eot[56] = {
		[48] = 0x80, 0x81, 0x20, 0x00, 0x00, 0x00, 0x24, 0xfb,
	};
	/* "mov.skipinv.end o0, pa0" after a dummy word */
	static const u8 copy[16] = {
		0x22, 0x22, [11] = 0xa0, 0x01, 0x10, 0x85, 0x28,
	};
	/* 12 data dwords; movs doutu, douti, doutt; halt */
	static const u8 prim[64] = {
		[48] = 0x05, 0x03, 0x00, 0x07, 0x02, 0x02, 0x04, 0x07,
		0x64, 0x23, 0x08, 0x07, 0x00, 0x00, 0x00, 0xaf,
	};
	bool v2 = version >= 2, v3 = version >= 3;
	unsigned int i, nsec = v3 ? 13 : v2 ? 11 : 6;
	u8 meta[4 * SGX_FW_META_WORDS];
	u8 *ent = c->file + 32, *pos = ent + nsec * 16;

	for (i = 0; i < SGX_FW_META_WORDS; i++)
		put32(meta + 4 * i, i);
	if (v2)
		put32(meta + 4 * (SGX_FW_META_STRUCT_SIZES + 2), 0x4c8);
	memset(c->file, 0, sizeof(c->file));
	section(c->file, &ent, &pos, 1, 0, NULL, 0, image, sizeof(image));
	section(c->file, &ent, &pos, 2, 0, tiny_buffers,
		ARRAY_SIZE(tiny_buffers), NULL, 0);
	section(c->file, &ent, &pos, 3, 0, tiny_patches,
		ARRAY_SIZE(tiny_patches), NULL, 0);
	section(c->file, &ent, &pos, 4, 1, tiny_part2,
		ARRAY_SIZE(tiny_part2), NULL, 0);
	section(c->file, &ent, &pos, 4, 3, tiny_dev_data,
		ARRAY_SIZE(tiny_dev_data), NULL, 0);
	section(c->file, &ent, &pos, 5, 0, NULL, 0, meta, sizeof(meta));
	if (v2) {
		section(c->file, &ent, &pos, 8, 0, tq_meta,
			v3 ? ARRAY_SIZE(tq_meta) : V2_META, NULL, 0);
		section(c->file, &ent, &pos, 6, 0, NULL, 0, fill, sizeof(fill));
		section(c->file, &ent, &pos, 6, 2, NULL, 0, eot, sizeof(eot));
		if (v3) {
			section(c->file, &ent, &pos, 6, 7, NULL, 0, copy,
				sizeof(copy));
			section(c->file, &ent, &pos, 6, 8, NULL, 0, prim,
				sizeof(prim));
		}
		section(c->file, &ent, &pos, 7, 0, tq_patches,
			v3 ? ARRAY_SIZE(tq_patches) : V2_PATCHES, NULL, 0);
		section(c->file, &ent, &pos, 4, 4, tq_cmd,
			ARRAY_SIZE(tq_cmd), NULL, 0);
	}
	c->len = pos - c->file;
	memcpy(c->file, "SGX545FW", 8);
	put16(c->file + 8, version);
	put16(c->file + 10, nsec);
	c->file[12] = 1;
	c->file[13] = 7;
	put32(c->file + 16, 862890);
	put32(c->file + 20, 0x60218);
	put32(c->file + 24, c->len);
	set_crc(c);
}

static int sgx_fw_test_init(struct kunit *test)
{
	struct ctx *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, c);
	pack(c, 1);
	c->bufs[0] = c->img[0];
	c->bufs[1] = c->img[1];
	c->bufs[9] = c->img[2];
	c->progs[0] = c->prog[0];
	c->progs[2] = c->prog[1];
	c->progs[4] = c->prog[2];
	c->progs[7] = c->prog[3];
	c->progs[8] = c->prog[4];
	test->priv = c;
	return 0;
}

static u32 word(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static void sgx_fw_test_crc32(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sgx_fw_crc32((const u8 *)"123456789", 9),
			0xcbf43926u);
}

static void sgx_fw_test_limm(struct kunit *test)
{
	const u64 tmpl = 0xfc237150000dbeefULL;
	int bit;

	KUNIT_EXPECT_EQ(test, sgx_fw_limm_get(tmpl), 0xdeadbeefu);
	/* a runtime word from the layout_base capture */
	KUNIT_EXPECT_EQ(test,
			sgx_fw_limm_set(tmpl,
					sgx_fw_limm_get(0xfc20008000001000ULL)),
			0xfc20008000001000ULL);
	for (bit = 0; bit < 32; bit++)
		KUNIT_EXPECT_EQ(test,
				sgx_fw_limm_get(sgx_fw_limm_set(tmpl, 1u << bit)),
				1u << bit);
}

static void sgx_fw_test_every_kind(struct kunit *test)
{
	struct ctx *c = test->priv;
	struct sgx_fw_write w[4];
	u32 dev[SGX_FW_DEV_DATA_WORDS];

	KUNIT_ASSERT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), 0);
	KUNIT_EXPECT_EQ(test, c->fw.ddk_build, 862890u);
	KUNIT_EXPECT_EQ(test, c->fw.meta[SGX_FW_META_CACHE_CONTROL], 30u);
	KUNIT_ASSERT_EQ(test, sgx_fw_build(&c->fw, &tiny_layout, c->bufs), 0);

	/* E800E000 >> 4 into the immediate of "mov r0, #0xdeadbeef" */
	KUNIT_EXPECT_EQ(test, word(c->img[0] + 8) |
			(u64)word(c->img[0] + 12) << 32,
			0xfc20314000000e00ULL);
	/* code word: (0x1ac + uk_off 0x200) << 4 | tag; emitpds control; rows */
	KUNIT_EXPECT_EQ(test, word(c->img[1]), 0x08003acfu);
	KUNIT_EXPECT_EQ(test, word(c->img[1] + 4), 3u << 23 | 0x100);
	KUNIT_EXPECT_EQ(test, word(c->img[1] + 8), 3u);
	KUNIT_EXPECT_EQ(test, word(c->img[2]), 0x2345000u);
	KUNIT_EXPECT_EQ(test, word(c->img[2] + 4), 0xf0005010u);
	KUNIT_EXPECT_EQ(test, word(c->img[2] + 8), 0x3c0003fu);

	KUNIT_ASSERT_EQ(test, sgx_fw_script(&c->fw, &tiny_layout, SGX_FW_PART2,
					    w, ARRAY_SIZE(w)), 2);
	KUNIT_EXPECT_EQ(test, w[0].reg, 0xa48u);
	KUNIT_EXPECT_EQ(test, w[0].val, 0x07d00000u);
	KUNIT_EXPECT_EQ(test, w[1].val, 0x03e81000u);
	KUNIT_EXPECT_EQ(test, sgx_fw_script(&c->fw, &tiny_layout, SGX_FW_PART1,
					    w, ARRAY_SIZE(w)), 0);
	KUNIT_ASSERT_EQ(test, sgx_fw_dev_data(&c->fw, &tiny_layout, dev), 0);
	KUNIT_EXPECT_EQ(test, dev[12], 0xe4000000u);
	KUNIT_EXPECT_EQ(test, dev[11], 0u);
}

static void sgx_fw_test_header(struct kunit *test)
{
	struct ctx *c = test->priv;

	c->file[0] = 'X';
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), -EINVAL);
	pack(c, 1);
	c->file[8] = 4;					/* format version */
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), -EINVAL);
	pack(c, 1);
	c->file[c->len - 1] ^= 1;			/* CRC */
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), -EBADMSG);
	pack(c, 1);					/* truncated */
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len - 8),
			-EINVAL);
	c->file[32] = 99;				/* section type */
	set_crc(c);
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), -EINVAL);
}

static void sgx_fw_test_patch_bounds(struct kunit *test)
{
	struct ctx *c = test->priv;
	u8 *ent = c->file + 32 + 2 * 16;		/* the PATCHES entry */

	/* the first patch (a LIMM at 8) moved to 12 reaches past the ukernel */
	c->file[word(ent + 4) + 4] = 12;
	set_crc(c);
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), -EINVAL);
}

static void sgx_fw_test_layout(struct kunit *test)
{
	struct ctx *c = test->priv;
	struct sgx_fw_write w[2];
	struct sgx_fw_layout l;

	KUNIT_ASSERT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_check(&c->fw, &tiny_layout), 0);

	l = tiny_layout;
	l.addr[0] += 4;				/* ukernel not 8-aligned */
	KUNIT_EXPECT_EQ(test, sgx_fw_check(&c->fw, &l), -ERANGE);
	l = tiny_layout;
	l.addr[1] += 8;				/* PDS not 16-aligned */
	KUNIT_EXPECT_EQ(test, sgx_fw_check(&c->fw, &l), -ERANGE);
	l = tiny_layout;
	l.addr_set &= ~(1u << 1);		/* a buffer has no address */
	KUNIT_EXPECT_EQ(test, sgx_fw_check(&c->fw, &l), -ENOENT);
	l = tiny_layout;
	l.heap_base[2] = 0xe9000000;		/* ukernel below code base */
	KUNIT_EXPECT_EQ(test, sgx_fw_check(&c->fw, &l), -ERANGE);
	l = tiny_layout;
	l.heap_base[2] = 0xe8001000 - 8 * 0x7ff00;	/* pc > 19 bits */
	KUNIT_EXPECT_EQ(test, sgx_fw_build(&c->fw, &l, c->bufs), -ERANGE);
	l = tiny_layout;
	l.heap_set &= ~(1u << 4);		/* part 2 needs PIXELSHADER */
	KUNIT_EXPECT_EQ(test, sgx_fw_script(&c->fw, &l, SGX_FW_PART2, NULL, 0),
			2);
	KUNIT_EXPECT_EQ(test, sgx_fw_script(&c->fw, &l, SGX_FW_PART2, w, 2),
			-ENOENT);
}

/*
 * sgx_fw_place() reproduces the harness allocator (fakedrm.c): per heap, the
 * first buffer at base + start, each one page-aligned at least, in srvinit's
 * allocation order. With the sgxconfig.h bases these are the addresses of the
 * layout_base capture, which the reference loader reproduces byte for byte.
 */
static void sgx_fw_test_place(struct kunit *test)
{
	static const struct sgx_fw_buffer bufs[] = {
		{ .id = 0, .heap = 2, .size = 0xcf68, .align = 8 },
		{ .id = 1, .heap = 2, .size = 0x18, .align = 16 },
		{ .id = 2, .heap = 2, .size = 0x38, .align = 16 },
		{ .id = 3, .heap = 2, .size = 0x18, .align = 16 },
		{ .id = 4, .heap = 2, .size = 0x100, .align = 16 },
		{ .id = 5, .heap = 2, .size = 0x68, .align = 16 },
		{ .id = 6, .heap = 3, .size = 0x2000, .align = 0x1000 },
		{ .id = 7, .heap = 3, .size = 0x8, .align = 0x1000 },
		{ .id = 8, .heap = 3, .size = 0x4, .align = 16 },
		{ .id = 9, .heap = 3, .size = 0xec, .align = 32 },
		{ .id = 10, .heap = 3, .size = 0x44, .align = 32 },
		{ .id = 11, .heap = 3, .size = 0x54, .align = 32 },
		{ .id = 12, .heap = 3, .size = 0x640c, .align = 32 },
		{ .id = 13, .heap = 3, .size = 0x620, .align = 32 },
		{ .id = 14, .heap = 3, .size = 0x2088, .align = 32 },
	};
	static const u8 order[] = { 6, 7, 8, 9, 10, 11, 12, 13, 14, 0, 1, 2, 3, 4, 5 };
	static const u32 want[] = {
		0xe8001000, 0xe800e000, 0xe800f000, 0xe8010000, 0xe8011000,
		0xe8012000, 0xf0001000, 0xf0003000, 0xf0004000, 0xf0005000,
		0xf0006000, 0xf0007000, 0xf0008000, 0xf000f000, 0xf0010000,
	};
	struct sgx_fw_layout l = {
		.heap_base = { [2] = 0xe8000000, [3] = 0xf0000000 },
		.heap_set = 1u << 2 | 1u << 3,
	};
	unsigned int i;

	KUNIT_ASSERT_EQ(test, sgx_fw_place(bufs, ARRAY_SIZE(bufs), order,
					   ARRAY_SIZE(order), 0x1000, 0x1000, &l), 0);
	KUNIT_EXPECT_EQ(test, l.addr_set, 0x7fffu);
	for (i = 0; i < ARRAY_SIZE(want); i++)
		KUNIT_EXPECT_EQ_MSG(test, l.addr[i], want[i], "buffer %u", i);

	l.heap_set = 1u << 2;			/* KERNEL_DATA heap missing */
	KUNIT_EXPECT_EQ(test, sgx_fw_place(bufs, ARRAY_SIZE(bufs), order,
					   ARRAY_SIZE(order), 0x1000, 0x1000, &l),
			-ENOENT);
	l.heap_set = 1u << 2 | 1u << 3;
	l.heap_base[3] = 0xffffe000;		/* runs past 4 GiB */
	KUNIT_EXPECT_EQ(test, sgx_fw_place(bufs, ARRAY_SIZE(bufs), order,
					   ARRAY_SIZE(order), 0x1000, 0x1000, &l),
			-ERANGE);
	l.heap_base[3] = 0xf0000000;
	KUNIT_EXPECT_EQ(test, sgx_fw_place(bufs, ARRAY_SIZE(bufs), order, 1,
					   0x1000, 0x1000, &l), -EINVAL);
}

static u64 qword(const u8 *p)
{
	return word(p) | (u64)word(p + 4) << 32;
}

static void sgx_fw_test_tq_fill_params(struct kunit *test)
{
	static const u32 want[SGX_FW_TQ_NUM_PARAMS] = {
		0xff336699, 0, 0x001f003f, 0x001f8000, 0xf0200000, 0x200, 0, 0, 0,
	};
	struct sgx_fw_tq_fill f = tiny_fill;
	u32 p[SGX_FW_TQ_NUM_PARAMS];
	unsigned int i;

	KUNIT_ASSERT_EQ(test, sgx_fw_tq_fill_params(&f, p), 0);
	for (i = 0; i < SGX_FW_TQ_NUM_PARAMS; i++)
		KUNIT_EXPECT_EQ_MSG(test, p[i], want[i], "param %u", i);
	f.stride = 252;				/* odd pixel stride */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_fill_params(&f, p), -EINVAL);
	f = tiny_fill;
	f.width = 8193;
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_fill_params(&f, p), -EINVAL);
	f = tiny_fill;
	f.dest = 0xfffff000;			/* surface past 4 GiB */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_fill_params(&f, p), -EINVAL);

	/* A 32 x 16 rectangle at (8, 4) of a 320-byte-stride surface. */
	f = tiny_fill;
	f.width = 32;
	f.height = 16;
	f.stride = 320;
	f.x = 8;
	f.y = 4;
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_fill_params(&f, p), 0);
	KUNIT_EXPECT_EQ(test, p[1], 0x00040008u);
	KUNIT_EXPECT_EQ(test, p[2], 0x00130027u);
	KUNIT_EXPECT_EQ(test, p[3], 0x00278000u);
	KUNIT_EXPECT_EQ(test, p[4], 0xf0200000u);
	f.x = 72;				/* x + width past the stride */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_fill_params(&f, p), -EINVAL);
}

static void sgx_fw_test_emitpix_sb(struct kunit *test)
{
	const u64 tmpl = 0xfb24000000208180ULL;
	int bit;

	KUNIT_EXPECT_EQ(test, sgx_fw_emitpix_sb_set(tmpl, 0x200),
			0xfb24004000208180ULL);
	for (bit = 0; bit < 14; bit++)
		KUNIT_EXPECT_EQ(test,
				sgx_fw_emitpix_sb_get(sgx_fw_emitpix_sb_set(tmpl,
									    1u << bit)),
				1u << bit);
}

/* test_fwload.test_tq_every_kind() */
static void sgx_fw_test_tq_every_kind(struct kunit *test)
{
	struct ctx *c = test->priv;
	u32 p[SGX_FW_TQ_NUM_PARAMS];
	struct sgx_fw_write w[4];
	const struct sgx_fw_tq *t;

	pack(c, 2);
	KUNIT_ASSERT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), 0);
	KUNIT_EXPECT_EQ(test, c->fw.tq_present, 1u << 0 | 1u << 2 | 1u << 4);
	t = sgx_fw_tq(&c->fw, 4);
	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_EXPECT_EQ(test, t->heap, 6);
	KUNIT_EXPECT_EQ(test, t->pds_data, 0x10);
	KUNIT_EXPECT_EQ(test, t->size, 0x18u);
	KUNIT_EXPECT_NULL(test, sgx_fw_tq(&c->fw, 1));
	/* the boot part is unchanged */
	KUNIT_EXPECT_EQ(test, sgx_fw_build(&c->fw, &tiny_layout, c->bufs), 0);

	KUNIT_ASSERT_EQ(test, sgx_fw_tq_fill_params(&tiny_fill, p), 0);
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_build(&c->fw, &tiny_layout, p,
					      c->progs), 0);
	KUNIT_EXPECT_EQ(test, qword(c->prog[0]), 0x1111ULL);
	KUNIT_EXPECT_EQ(test, qword(c->prog[0] + 8),
			sgx_fw_limm_set(0xfca4000100000000ULL, 0xff336699));
	KUNIT_EXPECT_EQ(test, qword(c->prog[1] + 16),
			sgx_fw_limm_set(1u << 21, 0x001f003f));
	KUNIT_EXPECT_EQ(test, qword(c->prog[1] + 48), 0xfb24004000208180ULL);
	KUNIT_EXPECT_EQ(test, word(c->prog[2]), 0u);
	KUNIT_EXPECT_EQ(test, word(c->prog[2] + 4), 0x01000080u);
	KUNIT_EXPECT_EQ(test, word(c->prog[2] + 8), 0xe4000000u);
	KUNIT_EXPECT_EQ(test, word(c->prog[2] + 12), 0xf4000044u);
	KUNIT_EXPECT_EQ(test, word(c->prog[2] + 16), 0xf0200001u);
	KUNIT_EXPECT_EQ(test, word(c->prog[2] + 20), 0u);

	KUNIT_EXPECT_EQ(test, sgx_fw_tq_cmd(&c->fw, &tiny_layout, p, NULL, 0), 4);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_cmd(&c->fw, &tiny_layout, p, w, 3),
			-ENOSPC);
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_cmd(&c->fw, &tiny_layout, p, w, 4), 4);
	KUNIT_EXPECT_EQ(test, w[0].reg, 0x4u);
	KUNIT_EXPECT_EQ(test, w[0].val, 0x200u);
	KUNIT_EXPECT_EQ(test, w[1].val, 0xe4000040u);
	KUNIT_EXPECT_EQ(test, w[2].val, 0xe4000000u);
	KUNIT_EXPECT_EQ(test, w[3].reg, 0x40u);
	KUNIT_EXPECT_EQ(test, w[3].val, 0xf0200000u);
}

static void sgx_fw_test_tq_reject(struct kunit *test)
{
	struct ctx *c = test->priv;

	/* transfer-queue sections in a v1 file */
	pack(c, 2);
	c->file[8] = 1;
	set_crc(c);
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), -EINVAL);
	/* a v1 file has no templates to build */
	pack(c, 1);
	KUNIT_ASSERT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_check(&c->fw, &tiny_layout), -ENOENT);
}

static void sgx_fw_test_tq_layout(struct kunit *test)
{
	struct ctx *c = test->priv;
	u32 p[SGX_FW_TQ_NUM_PARAMS];
	struct sgx_fw_layout l;

	pack(c, 2);
	KUNIT_ASSERT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), 0);
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_fill_params(&tiny_fill, p), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_check(&c->fw, &tiny_layout), 0);

	l = tiny_layout;
	l.tq_set &= ~(1u << 4);			/* a template has no address: */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_check(&c->fw, &l), 0);	/* not built */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_need(&c->fw, &l, 1u << 4), -ENOENT);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_need(&c->fw, &l, 1u << 0 | 1u << 1), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_build(&c->fw, &l, p, c->progs), 0);
	/* but the command names it */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_cmd(&c->fw, &l, p, NULL, 0), 4);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_cmd(&c->fw, &l, p, c->cmd, 4), -ENOENT);
	l = tiny_layout;
	l.tq_addr[0] += 4;			/* misaligned */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_check(&c->fw, &l), -ERANGE);
	l = tiny_layout;
	l.tq_addr[4] = 0xe3fffff0;		/* below its heap */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_check(&c->fw, &l), -ERANGE);
	l = tiny_layout;
	l.tq_addr[0] = 0xf4000ff8;		/* straddles a page */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_check(&c->fw, &l), -ERANGE);
	l = tiny_layout;
	l.tq_addr[4] = 0xe4000ff0;		/* no NO_STRADDLE flag: fine */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_check(&c->fw, &l), 0);
	l = tiny_layout;
	l.tq_addr[0] = 0xf4080000;		/* beyond a DOUTU word's reach */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_check(&c->fw, &l), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_build(&c->fw, &l, p, c->progs),
			-ERANGE);
}

/* test_fwload.test_copy_params() and the T0 cross-checks of transfer §4.4.3 */
static void sgx_fw_test_tq_copy_params(struct kunit *test)
{
	static const u32 want[SGX_FW_TQ_NUM_PARAMS] = {
		0, 0, 0x001f003f, 0x001f8000, 0xf0200000, 0x200,
		0x000e0e00, 0xcc03f01f, 0xf0300000,
	};
	static const u32 t0[][2] = {
		{ 260, 0x00100000 }, { 264, 0x00100200 }, { 288, 0x00100e00 },
		{ 320, 0x00120e00 }, { 512, 0x001e0e00 },
	};
	struct sgx_fw_tq_copy cp = tiny_copy;
	u32 p[SGX_FW_TQ_NUM_PARAMS];
	unsigned int i;

	KUNIT_ASSERT_EQ(test, sgx_fw_tq_copy_params(&cp, p), 0);
	for (i = 0; i < SGX_FW_TQ_NUM_PARAMS; i++)
		KUNIT_EXPECT_EQ_MSG(test, p[i], want[i], "param %u", i);
	for (i = 0; i < ARRAY_SIZE(t0); i++) {
		cp.src_stride = t0[i][0];
		KUNIT_ASSERT_EQ(test, sgx_fw_tq_copy_params(&cp, p), 0);
		KUNIT_EXPECT_EQ(test, p[6], t0[i][1]);
	}

	/* the limits: 4096 x 4096 texels, a 0x8000-byte source stride */
	cp = (struct sgx_fw_tq_copy){
		.width = 4096, .height = 4096, .src_stride = 0x8000,
		.dst_stride = 0x4000,
	};
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_copy_params(&cp, p), 0);
	KUNIT_EXPECT_EQ(test, p[6], 0x07fe0e00u);
	KUNIT_EXPECT_EQ(test, p[7], 0xccffffffu);
	cp.width = 4097;			/* T1 WIDTH is 12 bits */
	cp.src_stride = 0x8000;
	cp.dst_stride = 0x8000;
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_copy_params(&cp, p), -EINVAL);

	cp = tiny_copy;
	cp.src += 2;				/* source not 4-byte aligned */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_copy_params(&cp, p), -EINVAL);
	cp = tiny_copy;
	cp.src_stride = 258;			/* not whole texels */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_copy_params(&cp, p), -EINVAL);
	cp.src_stride = 252;			/* below the width */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_copy_params(&cp, p), -EINVAL);
	cp.src_stride = 0x8004;			/* STRIDE is 13 bits */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_copy_params(&cp, p), -EINVAL);
	cp = tiny_copy;
	cp.src = 0xfffff000;			/* source past 4 GiB */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_copy_params(&cp, p), -EINVAL);
	cp = tiny_copy;
	cp.dst_stride = 260;			/* odd destination pixel stride */
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_copy_params(&cp, p), -EINVAL);

	/* a 32 x 16 rectangle to (8, 4): the fill's PBE words, its own T1 */
	cp = tiny_copy;
	cp.src = 0xf0300104;
	cp.width = 32;
	cp.height = 16;
	cp.src_stride = 320;
	cp.dst_stride = 320;
	cp.x = 8;
	cp.y = 4;
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_copy_params(&cp, p), 0);
	KUNIT_EXPECT_EQ(test, p[1], 0x00040008u);
	KUNIT_EXPECT_EQ(test, p[2], 0x00130027u);
	KUNIT_EXPECT_EQ(test, p[3], 0x00278000u);
	KUNIT_EXPECT_EQ(test, p[7], 0xcc01f00fu);
	KUNIT_EXPECT_EQ(test, p[8], 0xf0300104u);
}

/* test_fwload.test_tq_v3_parameters() and the placed-only build */
static void sgx_fw_test_tq_copy(struct kunit *test)
{
	static const u32 want[16] = {
		0x00000100, 0x02000004, 0, 0, 0, 0, 0xf0300000, 0,
		0x000e0e00, 0xcc03f01f, 0, 0,
		0x07000305, 0x07040202, 0x07082364, 0xaf000000,
	};
	struct ctx *c = test->priv;
	u32 p[SGX_FW_TQ_NUM_PARAMS];
	const struct sgx_fw_tq *t;
	unsigned int i;

	pack(c, 3);
	KUNIT_ASSERT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), 0);
	KUNIT_EXPECT_EQ(test, c->fw.version, 3);
	KUNIT_EXPECT_EQ(test, c->fw.tq_present,
			1u << 0 | 1u << 2 | 1u << 4 | 1u << 7 | 1u << 8);
	t = sgx_fw_tq(&c->fw, SGX_FW_TQ_PDS_PRIM_1SRC);
	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_EXPECT_EQ(test, t->pds_data, 0x30);
	KUNIT_EXPECT_EQ(test, t->pds_attrs, 1);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_need(&c->fw, &tiny_v3_layout,
					     SGX_FW_TQ_COPY_SET), 0);

	KUNIT_ASSERT_EQ(test, sgx_fw_tq_copy_params(&tiny_copy, p), 0);
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_build(&c->fw, &tiny_v3_layout, p,
					      c->progs), 0);
	for (i = 0; i < ARRAY_SIZE(want); i++)
		KUNIT_EXPECT_EQ_MSG(test, word(c->prog[4] + 4 * i), want[i],
				    "dword %u", i);
	KUNIT_EXPECT_EQ(test, qword(c->prog[3] + 8), 0x28851001a0000000ULL);
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_cmd(&c->fw, &tiny_v3_layout, p, c->cmd,
					    4), 4);
	KUNIT_EXPECT_EQ(test, c->cmd[3].val, 0xf0200000u);

	/* the fill's layout: the copy templates are left alone */
	memset(c->prog[4], 0x5a, sizeof(c->prog[4]));
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_fill_params(&tiny_fill, p), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_need(&c->fw, &tiny_layout,
					     SGX_FW_TQ_FILL_SET), 0);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_need(&c->fw, &tiny_layout,
					     SGX_FW_TQ_COPY_SET), -ENOENT);
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_build(&c->fw, &tiny_layout, p,
					      c->progs), 0);
	KUNIT_EXPECT_EQ(test, word(c->prog[4]), 0x5a5a5a5au);
}

static void sgx_fw_test_tq_v3_reject(struct kunit *test)
{
	struct ctx *c = test->priv;
	struct sgx_fw_layout l = tiny_v3_layout;
	u32 p[SGX_FW_TQ_NUM_PARAMS];

	/* templates 7 and 8, parameters 6 to 8 in a v2 file */
	pack(c, 3);
	c->file[8] = 2;
	set_crc(c);
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), -EINVAL);
	/* a patch naming an unplaced template */
	pack(c, 3);
	KUNIT_ASSERT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), 0);
	KUNIT_ASSERT_EQ(test, sgx_fw_tq_copy_params(&tiny_copy, p), 0);
	l.tq_set &= ~(1u << 7);
	KUNIT_EXPECT_EQ(test, sgx_fw_tq_build(&c->fw, &l, p, c->progs),
			-ENOENT);
	/* a v2 file still parses */
	pack(c, 2);
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), 0);
	KUNIT_EXPECT_NULL(test, sgx_fw_tq(&c->fw, SGX_FW_TQ_USE_COPY));
}

static struct kunit_case sgx_fw_test_cases[] = {
	KUNIT_CASE(sgx_fw_test_crc32),
	KUNIT_CASE(sgx_fw_test_limm),
	KUNIT_CASE(sgx_fw_test_every_kind),
	KUNIT_CASE(sgx_fw_test_header),
	KUNIT_CASE(sgx_fw_test_patch_bounds),
	KUNIT_CASE(sgx_fw_test_layout),
	KUNIT_CASE(sgx_fw_test_place),
	KUNIT_CASE(sgx_fw_test_tq_fill_params),
	KUNIT_CASE(sgx_fw_test_emitpix_sb),
	KUNIT_CASE(sgx_fw_test_tq_every_kind),
	KUNIT_CASE(sgx_fw_test_tq_reject),
	KUNIT_CASE(sgx_fw_test_tq_layout),
	KUNIT_CASE(sgx_fw_test_tq_copy_params),
	KUNIT_CASE(sgx_fw_test_tq_copy),
	KUNIT_CASE(sgx_fw_test_tq_v3_reject),
	{}
};

static struct kunit_suite sgx_fw_test_suite = {
	.name = "gma500_sgx_fw",
	.init = sgx_fw_test_init,
	.test_cases = sgx_fw_test_cases,
};

kunit_test_suite(sgx_fw_test_suite);

MODULE_DESCRIPTION("KUnit tests for the SGX545 firmware loader");
MODULE_LICENSE("Dual MIT/GPL");
