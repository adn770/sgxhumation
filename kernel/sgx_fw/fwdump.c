// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * fwdump: userspace harness for sgx_fw.c, the counterpart of fwload.py.
 *
 *   fwdump <file.fw> <layout.txt> <outdir>
 *
 * layout.txt holds lines "addr <buffer id> <hex>", "heap <heap id> <hex>" and
 * "pd_phys <hex>". fwdump writes each patched buffer to <outdir>/NN.bin (NN =
 * buffer id) and prints the buffers, scripts, dev data and EDM metadata in
 * the text form tools/fwcutter/test_cloader.py compares with fwload.py.
 *
 * With "tq <template id> <hex>" lines and a line "fill <dest> <width> <height>
 * <stride> <colour> [<x> <y>]" (all hex), it also builds the transfer-queue
 * programs of that fill (format v2), writes each placed one to
 * <outdir>/tqNN.bin and prints the parameters, the template records and the
 * command words. A line "copy <src> <dst> <width> <height> <src_stride>
 * <dst_stride> [<x> <y>]" does the same for a copy (format v3).
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sgx_fw.h"

#define MAX_WRITES	256

enum { OP_NONE, OP_FILL, OP_COPY };

struct tq_op {
	int op;
	struct sgx_fw_tq_fill f;
	struct sgx_fw_tq_copy c;
};

static const char *const list_names[] = { "part1", "part2", "deinit" };

static int die(const char *what, int err)
{
	fprintf(stderr, "fwdump: %s: %s\n", what, strerror(err < 0 ? -err : err));
	return 1;
}

static u8 *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	u8 *data = NULL;
	long n;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
	    fseek(f, 0, SEEK_SET) == 0 && (data = malloc(n ? n : 1)) &&
	    fread(data, 1, n, f) == (size_t)n) {
		*len = n;
	} else {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

static int read_layout(const char *path, struct sgx_fw_layout *l,
		       struct tq_op *op)
{
	unsigned long id, val, v[8];
	char line[256], key[16];
	int n;
	FILE *f = fopen(path, "r");

	if (!f)
		return -errno;
	memset(l, 0, sizeof(*l));
	memset(op, 0, sizeof(*op));
	while (fgets(line, sizeof(line), f)) {
		memset(v, 0, sizeof(v));	/* x, y are optional */
		n = sscanf(line, "%15s %lx %lx %lx %lx %lx %lx %lx %lx", key,
			   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
			   &v[7]);
		if ((v[0] | v[1] | v[2] | v[3] | v[4] | v[5] | v[6] | v[7]) >
		    0xffffffff) {
			fclose(f);
			return -EINVAL;
		}
		if ((n == 6 || n == 8) && !strcmp(key, "fill")) {
			op->op = OP_FILL;
			op->f = (struct sgx_fw_tq_fill){
				.dest = v[0], .width = v[1], .height = v[2],
				.stride = v[3], .colour = v[4], .x = v[5], .y = v[6],
			};
		} else if ((n == 7 || n == 9) && !strcmp(key, "copy")) {
			op->op = OP_COPY;
			op->c = (struct sgx_fw_tq_copy){
				.src = v[0], .dst = v[1], .width = v[2],
				.height = v[3], .src_stride = v[4],
				.dst_stride = v[5], .x = v[6], .y = v[7],
			};
		} else if (sscanf(line, "%15s %lu %lx", key, &id, &val) == 3 &&
			   id < SGX_FW_TQ_MAX && val <= 0xffffffff &&
			   !strcmp(key, "tq")) {
			l->tq_addr[id] = val;
			l->tq_set |= 1u << id;
		} else if (sscanf(line, "%15s %lx", key, &val) == 2 &&
		    !strcmp(key, "pd_phys") && val <= 0xffffffff) {
			l->pd_phys = val;
		} else if (sscanf(line, "%15s %lu %lx", key, &id, &val) == 3 &&
			   id < SGX_FW_MAX_BUFFERS && val <= 0xffffffff &&
			   !strcmp(key, "addr")) {
			l->addr[id] = val;
			l->addr_set |= 1u << id;
		} else if (sscanf(line, "%15s %lu %lx", key, &id, &val) == 3 &&
			   id < SGX_FW_MAX_HEAPS && val <= 0xffffffff &&
			   !strcmp(key, "heap")) {
			l->heap_base[id] = val;
			l->heap_set |= 1u << id;
		} else if (line[0] != '\n') {
			fclose(f);
			return -EINVAL;
		}
	}
	fclose(f);
	return 0;
}

static int write_file(const char *path, const u8 *p, u32 size)
{
	FILE *f = fopen(path, "wb");

	if (!f || fwrite(p, 1, size, f) != size || fclose(f))
		return die(path, errno);
	return 0;
}

/*
 * The transfer-queue part: build the fill or copy, write the placed programs
 * to tqNN.bin, print it.
 */
static int dump_tq(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		   const struct tq_op *op, const char *outdir)
{
	static struct sgx_fw_write cmd[MAX_WRITES];
	u8 *progs[SGX_FW_TQ_MAX] = { 0 };
	u32 params[SGX_FW_TQ_NUM_PARAMS];
	const struct sgx_fw_tq *t;
	char path[4096];
	int ret, ncmd;
	unsigned int i;
	u32 set;

	if (op->op == OP_FILL) {
		ret = sgx_fw_tq_fill_params(&op->f, params);
		set = SGX_FW_TQ_FILL_SET;
	} else if (!sgx_fw_tq(fw, SGX_FW_TQ_USE_COPY) ||
		   !sgx_fw_tq(fw, SGX_FW_TQ_PDS_PRIM_1SRC)) {
		return die("copy", ENOENT);	/* not a v3 file */
	} else {
		ret = sgx_fw_tq_copy_params(&op->c, params);
		set = SGX_FW_TQ_COPY_SET;
	}
	if (ret)
		return die(op->op == OP_FILL ? "fill" : "copy", ret);
	ret = sgx_fw_tq_need(fw, l, set);
	if (ret)
		return die("tq need", ret);
	for (i = 0; i < SGX_FW_TQ_MAX; i++) {
		t = sgx_fw_tq(fw, i);
		if (t && (l->tq_set & 1u << i) &&
		    !(progs[i] = malloc(t->size ? t->size : 1)))
			return die("malloc", ENOMEM);
	}
	ret = sgx_fw_tq_build(fw, l, params, progs);
	if (ret)
		return die("tq build", ret);
	ncmd = sgx_fw_tq_cmd(fw, l, params, cmd, MAX_WRITES);
	if (ncmd < 0)
		return die("tq cmd", ncmd);

	for (i = 0; i < SGX_FW_TQ_MAX; i++) {
		t = sgx_fw_tq(fw, i);
		if (!t || !(l->tq_set & 1u << i))
			continue;
		snprintf(path, sizeof(path), "%s/tq%02u.bin", outdir, i);
		if (write_file(path, progs[i], t->size))
			return 1;
	}
	printf("tq_params");
	for (i = 0; i < SGX_FW_TQ_NUM_PARAMS; i++)
		printf(" 0x%08x", params[i]);
	printf("\n");
	for (i = 0; i < SGX_FW_TQ_MAX; i++) {
		t = sgx_fw_tq(fw, i);
		if (t)
			printf("tq %u heap %u temps %u attrs %u size 0x%08x align 0x%08x data 0x%08x flags %u\n",
			       i, t->heap, t->temps, t->pds_attrs, t->size,
			       t->align, t->pds_data, t->flags);
	}
	printf("tq_cmd %d\n", ncmd);
	for (i = 0; i < (unsigned int)ncmd; i++)
		printf("  0x%08x 0x%08x\n", cmd[i].reg, cmd[i].val);
	return 0;
}

int main(int argc, char **argv)
{
	static struct sgx_fw_write writes[3][MAX_WRITES];
	u8 *bufs[SGX_FW_MAX_BUFFERS] = { 0 };
	struct tq_op op;
	u32 dev[SGX_FW_DEV_DATA_WORDS];
	const struct sgx_fw_buffer *b;
	struct sgx_fw_layout l;
	int nw[3], ret, i, k;
	struct sgx_fw fw;
	char path[4096];
	size_t len;
	u8 *data;

	if (argc != 4) {
		fprintf(stderr, "usage: fwdump <file.fw> <layout.txt> <outdir>\n");
		return 2;
	}
	data = read_file(argv[1], &len);
	if (!data)
		return die(argv[1], errno ? errno : EIO);
	ret = read_layout(argv[2], &l, &op);
	if (ret)
		return die(argv[2], ret);
	ret = sgx_fw_parse(&fw, data, len);
	if (ret)
		return die("parse", ret);

	for (i = 0; i < (int)fw.nbuffers; i++) {
		b = &fw.buffers[i];
		bufs[b->id] = malloc(b->size ? b->size : 1);
		if (!bufs[b->id])
			return die("malloc", ENOMEM);
	}
	ret = sgx_fw_build(&fw, &l, bufs);
	if (ret)
		return die("build", ret);
	for (i = 0; i < 3; i++) {
		nw[i] = sgx_fw_script(&fw, &l, i, writes[i], MAX_WRITES);
		if (nw[i] < 0)
			return die(list_names[i], nw[i]);
	}
	ret = sgx_fw_dev_data(&fw, &l, dev);
	if (ret)
		return die("dev data", ret);

	if (mkdir(argv[3], 0777) && errno != EEXIST)
		return die(argv[3], errno);
	for (i = 0; i < (int)fw.nbuffers; i++) {
		b = &fw.buffers[i];
		snprintf(path, sizeof(path), "%s/%02u.bin", argv[3], b->id);
		if (write_file(path, bufs[b->id], b->size))
			return 1;
	}

	printf("ddk %u.%u.%u options 0x%08x\n", fw.ddk_major, fw.ddk_minor,
	       fw.ddk_build, fw.build_options);
	for (i = 0; i < (int)fw.nbuffers; i++) {
		b = &fw.buffers[i];
		printf("buffer %u heap %u rows %u size 0x%08x align 0x%08x\n",
		       b->id, b->heap, b->pds_rows, b->size, b->align);
	}
	for (i = 0; i < 3; i++) {
		printf("%s %d\n", list_names[i], nw[i]);
		for (k = 0; k < nw[i]; k++)
			printf("  0x%08x 0x%08x\n", writes[i][k].reg,
			       writes[i][k].val);
	}
	printf("dev_data");
	for (i = 0; i < SGX_FW_DEV_DATA_WORDS; i++)
		printf(" 0x%08x", dev[i]);
	printf("\nmeta");
	for (i = 0; i < SGX_FW_META_WORDS; i++)
		printf(" 0x%08x", fw.meta[i]);
	printf("\n");
	return op.op != OP_NONE ? dump_tq(&fw, &l, &op, argv[3]) : 0;
}
