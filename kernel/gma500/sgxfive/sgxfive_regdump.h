/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sgxfive register snapshot (sgxfive_regdump.c): the registers the EDM
 * firmware reaches with ldr/str (tools/usse-dis/regtouch.py, 191 of them) and
 * the host's own (BIF fault and directory lists), read for debugfs
 * dri/N/sgx/regs. Registers whose write starts work are listed but never read.
 */
#ifndef _SGXFIVE_REGDUMP_H_
#define _SGXFIVE_REGDUMP_H_

#include <linux/types.h>

struct drm_printer;
struct sgxfive_devstate;

#define SGXFIVE_REGDUMP_FW	0x1	/* the firmware reads or writes it */
#define SGXFIVE_REGDUMP_HOST	0x2	/* only the host driver uses it */
#define SGXFIVE_REGDUMP_SKIP	0x4	/* a write starts work: never read */

#define SGXFIVE_REGDUMP_NREGS	212
#define SGXFIVE_REGDUMP_LIMIT	0x1000	/* every offset is below this */
#define SGXFIVE_REGDUMP_VERSION	1

struct sgxfive_regdump_reg {
	u16 off;
	u8 flags;
};

extern const struct sgxfive_regdump_reg sgxfive_regdump_regs[SGXFIVE_REGDUMP_NREGS];

struct sgxfive_regdump {
	u32 val[SGXFIVE_REGDUMP_NREGS];
};

void sgxfive_regdump_capture(struct sgxfive_regdump *rd,
			     u32 (*read)(void *ctx, u32 reg), void *ctx);
void sgxfive_regdump_print(struct drm_printer *p, const struct sgxfive_regdump *rd,
			   const struct sgxfive_devstate *d);

#endif
