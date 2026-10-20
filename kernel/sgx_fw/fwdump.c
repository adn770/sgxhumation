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
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sgx_fw.h"

#define MAX_WRITES	256

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

static int read_layout(const char *path, struct sgx_fw_layout *l)
{
	char line[256], key[16];
	unsigned long id, val;
	FILE *f = fopen(path, "r");

	if (!f)
		return -errno;
	memset(l, 0, sizeof(*l));
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%15s %lx", key, &val) == 2 &&
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

int main(int argc, char **argv)
{
	static struct sgx_fw_write writes[3][MAX_WRITES];
	u8 *bufs[SGX_FW_MAX_BUFFERS] = { 0 };
	u32 dev[SGX_FW_DEV_DATA_WORDS];
	const struct sgx_fw_buffer *b;
	struct sgx_fw_layout l;
	int nw[3], ret, i, k;
	struct sgx_fw fw;
	char path[4096];
	size_t len;
	u8 *data;
	FILE *f;

	if (argc != 4) {
		fprintf(stderr, "usage: fwdump <file.fw> <layout.txt> <outdir>\n");
		return 2;
	}
	data = read_file(argv[1], &len);
	if (!data)
		return die(argv[1], errno ? errno : EIO);
	ret = read_layout(argv[2], &l);
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
		f = fopen(path, "wb");
		if (!f || fwrite(bufs[b->id], 1, b->size, f) != b->size ||
		    fclose(f))
			return die(path, errno);
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
	return 0;
}
