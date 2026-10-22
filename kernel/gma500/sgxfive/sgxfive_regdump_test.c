// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the sgxfive register snapshot (sgxfive_regdump.c): the
 * register table, that a capture never reads a register whose write starts
 * work, and the one-value-a-line text debugfs regs shows.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include <drm/drm_print.h>

#include "sgxfive_coredump.h"
#include "sgxfive_regdump.h"

#define TEXT_MAX	16384

/* The table is sorted, without duplicates, dword aligned and in range. */
static void sgxfive_regdump_test_table(struct kunit *test)
{
	unsigned int i, fw = 0, host = 0, skip = 0;

	for (i = 0; i < SGXFIVE_REGDUMP_NREGS; i++) {
		const struct sgxfive_regdump_reg *r = &sgxfive_regdump_regs[i];

		KUNIT_EXPECT_EQ(test, r->off % 4, 0);
		KUNIT_EXPECT_LT(test, r->off, SGXFIVE_REGDUMP_LIMIT);
		if (i)
			KUNIT_EXPECT_GT(test, r->off, sgxfive_regdump_regs[i - 1].off);
		/* Exactly one of firmware or host; only firmware ones are skipped. */
		KUNIT_EXPECT_NE(test, !(r->flags & SGXFIVE_REGDUMP_FW),
				!(r->flags & SGXFIVE_REGDUMP_HOST));
		if (r->flags & SGXFIVE_REGDUMP_SKIP)
			KUNIT_EXPECT_TRUE(test, r->flags & SGXFIVE_REGDUMP_FW);
		fw += !!(r->flags & SGXFIVE_REGDUMP_FW);
		host += !!(r->flags & SGXFIVE_REGDUMP_HOST);
		skip += !!(r->flags & SGXFIVE_REGDUMP_SKIP);
	}
	KUNIT_EXPECT_EQ(test, fw, 191);	/* regtouch.py's list */
	KUNIT_EXPECT_EQ(test, host, 21);
	KUNIT_EXPECT_EQ(test, skip, 21);
}

static int find(u32 off)
{
	unsigned int i;

	for (i = 0; i < SGXFIVE_REGDUMP_NREGS; i++)
		if (sgxfive_regdump_regs[i].off == off)
			return i;
	return -1;
}

/* The kicks, clears and DPM commands are skipped; status and state are read. */
static void sgxfive_regdump_test_classes(struct kunit *test)
{
	static const u32 skipped[] = {
		0x0114, 0x0134, 0x0140, 0x0200, 0x024c, 0x0428, 0x0430, 0x0450,
		0x063c, 0x0648, 0x064c, 0x0658, 0x0680, 0x0684, 0x0694, 0x0698,
		0x069c, 0x06a0, 0x0ad0, 0x0ad4, 0x0ae0,
	};
	static const u32 read_fw[] = {
		0x0000, 0x001c, 0x0020, 0x012c, 0x0208, 0x0408, 0x0414, 0x0480,
		0x0604, 0x0630, 0x0724, 0x0a74, 0x0b14, 0x0c00, 0x0c90, 0x0e80,
	};
	static const u32 read_host[] = {
		0x0130, 0x0b20, 0x0c04, 0x0c08, 0x0c38, 0x0c70, 0x0c84, 0x0c8c,
	};
	unsigned int i;
	int k;

	for (i = 0; i < ARRAY_SIZE(skipped); i++) {
		k = find(skipped[i]);
		KUNIT_ASSERT_GE(test, k, 0);
		KUNIT_EXPECT_EQ(test, sgxfive_regdump_regs[k].flags,
				SGXFIVE_REGDUMP_FW | SGXFIVE_REGDUMP_SKIP);
	}
	for (i = 0; i < ARRAY_SIZE(read_fw); i++) {
		k = find(read_fw[i]);
		KUNIT_ASSERT_GE(test, k, 0);
		KUNIT_EXPECT_EQ(test, sgxfive_regdump_regs[k].flags, SGXFIVE_REGDUMP_FW);
	}
	for (i = 0; i < ARRAY_SIZE(read_host); i++) {
		k = find(read_host[i]);
		KUNIT_ASSERT_GE(test, k, 0);
		KUNIT_EXPECT_EQ(test, sgxfive_regdump_regs[k].flags, SGXFIVE_REGDUMP_HOST);
	}
	/* The kick doorbells are not even listed. */
	KUNIT_EXPECT_LT(test, find(0x0ac8), 0);
	KUNIT_EXPECT_LT(test, find(0x0ae8), 0);
}

struct fake_regs {
	u8 reads[SGXFIVE_REGDUMP_LIMIT / 4];
};

static u32 fake_read(void *ctx, u32 reg)
{
	struct fake_regs *f = ctx;

	if (reg < SGXFIVE_REGDUMP_LIMIT)
		f->reads[reg / 4]++;
	return 0xa5000000 | reg;
}

/* Each register that is not skipped is read once; a skipped one never. */
static void sgxfive_regdump_test_capture(struct kunit *test)
{
	struct fake_regs *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	struct sgxfive_regdump *rd = kunit_kzalloc(test, sizeof(*rd), GFP_KERNEL);
	unsigned int i, total = 0;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, rd);
	sgxfive_regdump_capture(rd, fake_read, f);
	for (i = 0; i < SGXFIVE_REGDUMP_NREGS; i++) {
		const struct sgxfive_regdump_reg *r = &sgxfive_regdump_regs[i];

		if (r->flags & SGXFIVE_REGDUMP_SKIP) {
			KUNIT_EXPECT_EQ(test, f->reads[r->off / 4], 0);
			KUNIT_EXPECT_EQ(test, rd->val[i], 0);
		} else {
			KUNIT_EXPECT_EQ(test, f->reads[r->off / 4], 1);
			KUNIT_EXPECT_EQ(test, rd->val[i], 0xa5000000 | r->off);
		}
	}
	for (i = 0; i < ARRAY_SIZE(f->reads); i++)
		total += f->reads[i];
	KUNIT_EXPECT_EQ(test, total, 191);
}

static char *print_text(struct kunit *test, const struct sgxfive_regdump *rd,
			const struct sgxfive_devstate *d)
{
	char *buf = kunit_kzalloc(test, TEXT_MAX + 1, GFP_KERNEL);
	struct drm_print_iterator iter = { .data = buf, .remain = TEXT_MAX };
	struct drm_printer p = drm_coredump_printer(&iter);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	sgxfive_regdump_print(&p, rd, d);
	KUNIT_ASSERT_GT(test, iter.remain, 0);	/* it all fits */
	return buf;
}

static unsigned int count_lines(const char *s, const char *prefix)
{
	unsigned int n = 0;
	size_t len = strlen(prefix);

	while (s && *s) {
		if (!strncmp(s, prefix, len))
			n++;
		s = strchr(s, '\n');
		if (s)
			s++;
	}
	return n;
}

/* A versioned header, the state line, one line a register, then the words. */
static void sgxfive_regdump_test_print(struct kunit *test)
{
	struct fake_regs *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	struct sgxfive_regdump *rd = kunit_kzalloc(test, sizeof(*rd), GFP_KERNEL);
	struct sgxfive_devstate *d = kunit_kzalloc(test, sizeof(*d), GFP_KERNEL);
	char *text;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, rd);
	KUNIT_ASSERT_NOT_NULL(test, d);
	sgxfive_regdump_capture(rd, fake_read, f);
	d->loaded = true;
	d->live = true;
	d->sw_events = 7;
	d->ccb_sent = 9;
	d->hostctl[0] = 0x1;
	d->ta3dctl[46] = 0x10001000;
	d->ccbctl[0] = 0x40;
	d->ccbctl[1] = 0x20;

	text = print_text(test, rd, d);
	KUNIT_EXPECT_EQ(test, strncmp(text, "sgxfive regs 1\n", 15), 0);
	KUNIT_EXPECT_NOT_NULL(test, strstr(text,
		"\nstate loaded 1 live 1 sw_events 7 bif_faults 0 ccb_sent 9 deferred 0\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nreg 0x0408 0xa5000408\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nreg 0x0c08 0xa5000c08\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nreg 0x0200 skip\n"));
	KUNIT_EXPECT_EQ(test, count_lines(text, "reg "), SGXFIVE_REGDUMP_NREGS);
	KUNIT_EXPECT_EQ(test, count_lines(text, "hostctl "), SGXFIVE_HC_DWORDS);
	KUNIT_EXPECT_EQ(test, count_lines(text, "ta3dctl "), SGXFIVE_TA3D_CTL_DWORDS);
	KUNIT_EXPECT_EQ(test, count_lines(text, "ccbctl "), 2);
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nhostctl 0x00 0x00000001\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nta3dctl 0xb8 0x10001000\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nccbctl 0x04 0x00000020\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "\nend\n"));

	/* Not loaded: no firmware blocks to show. */
	d->loaded = false;
	text = print_text(test, rd, d);
	KUNIT_EXPECT_EQ(test, count_lines(text, "reg "), SGXFIVE_REGDUMP_NREGS);
	KUNIT_EXPECT_EQ(test, count_lines(text, "hostctl "), 0);
	KUNIT_EXPECT_EQ(test, count_lines(text, "ta3dctl "), 0);
	KUNIT_EXPECT_EQ(test, count_lines(text, "ccbctl "), 0);
}

static struct kunit_case sgxfive_regdump_test_cases[] = {
	KUNIT_CASE(sgxfive_regdump_test_table),
	KUNIT_CASE(sgxfive_regdump_test_classes),
	KUNIT_CASE(sgxfive_regdump_test_capture),
	KUNIT_CASE(sgxfive_regdump_test_print),
	{}
};

static struct kunit_suite sgxfive_regdump_test_suite = {
	.name = "gma500_sgxfive_regdump",
	.test_cases = sgxfive_regdump_test_cases,
};

kunit_test_suite(sgxfive_regdump_test_suite);
