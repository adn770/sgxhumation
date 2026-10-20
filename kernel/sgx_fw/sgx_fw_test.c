// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for sgx_fw.c, the synthetic cases of tools/fwcutter/test_fwload.py
 * and test_fwformat.py: every expression kind, header and CRC rejection, the
 * layout constraints. The real firmware is checked against the reference loader
 * in userspace (tools/fwcutter/test_cloader.py).
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

enum { WORD32, LIMM, CODE_BASE };
enum { CONST, ADDR, ADDR_SHR4, HEAP, PD_PHYS, CODE_WORD, PDS_CTL, PDS_ROWS };

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

static const struct sgx_fw_layout tiny_layout = {
	.addr = { [0] = 0xe8001000, [1] = 0xe800e000, [9] = 0xf0005000 },
	.addr_set = 1u << 0 | 1u << 1 | 1u << 9,
	.heap_base = { [2] = 0xe8000000, [4] = 0xf4000000, [6] = 0xe4000000 },
	.heap_set = 1u << 2 | 1u << 4 | 1u << 6,
	.pd_phys = 0x2345000,
};

struct ctx {
	u8 file[512];
	size_t len;
	struct sgx_fw fw;
	u8 img[3][0x40];
	u8 *bufs[SGX_FW_MAX_BUFFERS];
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
	put16(*ent + 2, type != 1);		/* IMAGE is CUT, the rest FACT */
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

/* fwformat.pack(tiny()) */
static void pack_tiny(struct ctx *c)
{
	static const u8 image[16] = {
		[8] = 0xef, 0xbe, 0x0d, 0x00, 0x50, 0x71, 0x23, 0xfc,
	};
	u8 meta[4 * SGX_FW_META_WORDS];
	u8 *ent = c->file + 32, *pos = ent + 6 * 16;
	unsigned int i;

	for (i = 0; i < SGX_FW_META_WORDS; i++)
		put32(meta + 4 * i, i);
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
	c->len = pos - c->file;
	memcpy(c->file, "SGX545FW", 8);
	put16(c->file + 8, 1);
	put16(c->file + 10, 6);
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
	pack_tiny(c);
	c->bufs[0] = c->img[0];
	c->bufs[1] = c->img[1];
	c->bufs[9] = c->img[2];
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
	pack_tiny(c);
	c->file[8] = 2;					/* format version */
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), -EINVAL);
	pack_tiny(c);
	c->file[c->len - 1] ^= 1;			/* CRC */
	KUNIT_EXPECT_EQ(test, sgx_fw_parse(&c->fw, c->file, c->len), -EBADMSG);
	pack_tiny(c);					/* truncated */
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

static struct kunit_case sgx_fw_test_cases[] = {
	KUNIT_CASE(sgx_fw_test_crc32),
	KUNIT_CASE(sgx_fw_test_limm),
	KUNIT_CASE(sgx_fw_test_every_kind),
	KUNIT_CASE(sgx_fw_test_header),
	KUNIT_CASE(sgx_fw_test_patch_bounds),
	KUNIT_CASE(sgx_fw_test_layout),
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
