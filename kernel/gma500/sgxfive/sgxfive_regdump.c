// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sgxfive register snapshot, for debugfs dri/N/sgx/regs: the 191 registers
 * the EDM firmware reaches with ldr/str (tools/usse-dis/regtouch.py over the
 * microkernel), the host's own (event enable, the BIF fault and memory
 * request status, the 16 directory lists), and the firmware control blocks
 * sgxfive_dump() shows (HostCtl, TA3DCtl, the kernel CCB control words).
 *
 * The text is one value a line ("reg 0x0408 0x...") so two snapshots diff
 * line by line (tools/regsnap.py). Registers whose write starts work (the
 * VDM, ISP and render starts, the event and PDS cache clears, the PDS
 * invalidates, the DPM allocate, free, store/load and out-of-memory commands)
 * are listed as "skip" and never read: nothing shows a read of them is
 * harmless, and their read value says nothing about state. Everything read is
 * a plain MMIO read, as SGXDumpDebugInfo does for its smaller set.
 */
#include <drm/drm_print.h>

#include "sgxfive_coredump.h"
#include "sgxfive_regdump.h"

#define F(o)	{ (o), SGXFIVE_REGDUMP_FW }
#define T(o)	{ (o), SGXFIVE_REGDUMP_FW | SGXFIVE_REGDUMP_SKIP }
#define H(o)	{ (o), SGXFIVE_REGDUMP_HOST }

/* By offset; F: the firmware's, T: the firmware's but skipped, H: the host's. */
const struct sgxfive_regdump_reg sgxfive_regdump_regs[SGXFIVE_REGDUMP_NREGS] = {
	F(0x0000), F(0x0008), F(0x001c), F(0x0020), F(0x0040), F(0x0080), T(0x0114),
	F(0x0118), F(0x012c), H(0x0130), T(0x0134), F(0x0138), T(0x0140), F(0x0144),
	F(0x0148), T(0x0200), F(0x0204), F(0x0208), F(0x020c), F(0x0210), F(0x0214),
	F(0x0218), F(0x021c), F(0x0220), F(0x0224), F(0x0230), F(0x0234), F(0x0238),
	F(0x023c), F(0x0240), F(0x0244), F(0x0248), T(0x024c), F(0x026c), F(0x0274),
	F(0x0284), F(0x0290), F(0x02a8), F(0x02b0), F(0x02b4), F(0x02d0), F(0x02d4),
	F(0x0308), F(0x030c), F(0x0324), F(0x0364), F(0x0368), F(0x0370), F(0x0374),
	F(0x0400), F(0x0404), F(0x0408), F(0x040c), F(0x0410), F(0x0414), F(0x0418),
	F(0x041c), F(0x0420), F(0x0424), T(0x0428), F(0x042c), T(0x0430), F(0x043c),
	F(0x0444), F(0x0448), F(0x044c), T(0x0450), F(0x0480), F(0x0484), F(0x0488),
	F(0x048c), F(0x0490), F(0x0494), F(0x0498), F(0x049c), F(0x04a0), F(0x04a4),
	F(0x04a8), F(0x04ac), F(0x04b0), F(0x04b4), F(0x04b8), F(0x04bc), F(0x04c4),
	F(0x04c8), F(0x04d4), F(0x04d8), F(0x04dc), F(0x04e8), F(0x050c), F(0x0518),
	F(0x051c), F(0x0520), F(0x0600), F(0x0604), F(0x0618), F(0x061c), F(0x0620),
	F(0x0624), F(0x0628), F(0x062c), F(0x0630), F(0x0634), F(0x0638), T(0x063c),
	T(0x0648), T(0x064c), T(0x0658), F(0x065c), F(0x0660), F(0x0664), F(0x0668),
	F(0x0674), T(0x0680), T(0x0684), T(0x0694), T(0x0698), T(0x069c), T(0x06a0),
	F(0x0700), F(0x070c), F(0x0710), F(0x071c), F(0x0720), F(0x0724), F(0x0728),
	F(0x072c), F(0x0730), F(0x0734), F(0x073c), F(0x0740), F(0x074c), F(0x0800),
	F(0x0804), F(0x0820), F(0x0824), F(0x0828), F(0x082c), F(0x0830), F(0x0834),
	F(0x0838), F(0x083c), F(0x0840), F(0x0844), F(0x0848), F(0x0860), F(0x0864),
	F(0x0868), F(0x0a04), F(0x0a08), F(0x0a5c), F(0x0a60), F(0x0a64), F(0x0a68),
	F(0x0a6c), F(0x0a74), F(0x0a84), F(0x0a88), F(0x0a8c), F(0x0a90), F(0x0a94),
	F(0x0a98), F(0x0a9c), F(0x0aa0), F(0x0aa4), F(0x0aa8), F(0x0ab8), F(0x0acc),
	T(0x0ad0), T(0x0ad4), T(0x0ae0), F(0x0b0c), F(0x0b10), F(0x0b14), F(0x0b18),
	F(0x0b1c), H(0x0b20), F(0x0b24), F(0x0b28), F(0x0b2c), F(0x0b80), F(0x0b84),
	F(0x0b88), F(0x0b8c), F(0x0b94), F(0x0b98), F(0x0c00), H(0x0c04), H(0x0c08),
	H(0x0c38), H(0x0c3c), H(0x0c40), H(0x0c44), H(0x0c48), H(0x0c4c), H(0x0c50),
	H(0x0c54), H(0x0c58), H(0x0c5c), H(0x0c60), H(0x0c64), H(0x0c68), H(0x0c6c),
	H(0x0c70), F(0x0c74), F(0x0c78), H(0x0c84), H(0x0c8c), F(0x0c90), F(0x0cac),
	F(0x0cb0), F(0x0e80),
};

void sgxfive_regdump_capture(struct sgxfive_regdump *rd,
			     u32 (*read)(void *ctx, u32 reg), void *ctx)
{
	unsigned int i;

	for (i = 0; i < SGXFIVE_REGDUMP_NREGS; i++) {
		const struct sgxfive_regdump_reg *r = &sgxfive_regdump_regs[i];

		rd->val[i] = r->flags & SGXFIVE_REGDUMP_SKIP ? 0 : read(ctx, r->off);
	}
}

static void sgxfive_regdump_words(struct drm_printer *p, const char *name,
				  const u32 *w, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		drm_printf(p, "%s 0x%02x 0x%08x\n", name, i * 4, w[i]);
}

void sgxfive_regdump_print(struct drm_printer *p, const struct sgxfive_regdump *rd,
			   const struct sgxfive_devstate *d)
{
	unsigned int i;

	drm_printf(p, "sgxfive regs %d\n", SGXFIVE_REGDUMP_VERSION);
	drm_printf(p, "state loaded %d live %d sw_events %d bif_faults %d ccb_sent %llu deferred %u\n",
		   d->loaded, d->live, d->sw_events, d->bif_faults, d->ccb_sent,
		   d->ndeferred);
	for (i = 0; i < SGXFIVE_REGDUMP_NREGS; i++) {
		const struct sgxfive_regdump_reg *r = &sgxfive_regdump_regs[i];

		if (r->flags & SGXFIVE_REGDUMP_SKIP)
			drm_printf(p, "reg 0x%04x skip\n", r->off);
		else
			drm_printf(p, "reg 0x%04x 0x%08x\n", r->off, rd->val[i]);
	}
	if (d->loaded) {
		sgxfive_regdump_words(p, "hostctl", d->hostctl, SGXFIVE_HC_DWORDS);
		sgxfive_regdump_words(p, "ta3dctl", d->ta3dctl, SGXFIVE_TA3D_CTL_DWORDS);
		sgxfive_regdump_words(p, "ccbctl", d->ccbctl, 2);
	}
	drm_printf(p, "end\n");
}
