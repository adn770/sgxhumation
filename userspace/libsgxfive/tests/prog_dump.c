// SPDX-License-Identifier: MIT
/*
 * prog_dump: build every placed program of a program source and print it, for
 * tests/test_prog_asm.py, which compares the output with fwload.build_tq().
 *
 *   prog_dump asm|fake < layout
 *
 * layout lines (numbers in C notation):
 *   heap <DRM heap id> <base>
 *   addr <program id> <address>
 *   param <index> <value>
 *
 * Output, by program id, for each placed program:
 *   meta <id> heap <h> temps <t> attrs <a> size <s> align <al> data <d> flags <f>
 *   prog <id> <hex bytes>          or          prog <id> error <errno>
 * The DOUTU words of the env are those the engine computes (sgxfive_doutu()).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgxfive_xfer.h"

int main(int argc, char **argv)
{
	struct sgxfive_prog_source *src;
	struct sgxfive_prog_env env = { 0 };
	char key[16];
	unsigned long long a, b;

	if (argc != 2 || (strcmp(argv[1], "asm") && strcmp(argv[1], "fake"))) {
		fprintf(stderr, "usage: prog_dump asm|fake < layout\n");
		return 2;
	}
	src = !strcmp(argv[1], "asm") ? sgxfive_prog_asm_create() : sgxfive_prog_fake_create();
	if (!src)
		return 1;
	while (scanf("%15s %lli %lli", key, &a, &b) == 3) {
		if (!strcmp(key, "heap") && a < DRM_SGXFIVE_HEAP_COUNT)
			env.heap_base[a] = b;
		else if (!strcmp(key, "addr") && a < SGXFIVE_PROG_COUNT)
			env.addr[a] = b;
		else if (!strcmp(key, "param") && a < SGXFIVE_PARAM_COUNT)
			env.params[a] = (uint32_t)b;
		else
			return 2;
	}
	for (uint32_t id = 0; id < SGXFIVE_PROG_COUNT; id++) {
		struct sgxfive_prog_meta m;

		if (!env.addr[id] || src->ops->meta(src, id, &m))
			continue;
		if (m.heap == DRM_SGXFIVE_HEAP_USSE_PIXEL)
			sgxfive_doutu(env.addr[id], env.heap_base[m.heap], m.temps, &env.doutu[id]);
	}
	for (uint32_t id = 0; id < SGXFIVE_PROG_COUNT; id++) {
		struct sgxfive_prog_meta m;
		uint8_t *out;
		int ret;

		if (!env.addr[id] || src->ops->meta(src, id, &m))
			continue;
		printf("meta %u heap %u temps %u attrs %u size %u align %u data %u flags %u\n", id,
		       m.heap, m.temps, m.attrs, m.size, m.align, m.data_size, m.flags);
		out = calloc(1, m.size);
		ret = out ? src->ops->build(src, id, &env, out) : -ENOMEM;
		printf("prog %u ", id);
		if (ret) {
			printf("error %d\n", -ret);
		} else {
			for (uint32_t i = 0; i < m.size; i++)
				printf("%02x", out[i]);
			printf("\n");
		}
		free(out);
	}
	sgxfive_prog_source_destroy(src);
	return 0;
}
