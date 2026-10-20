/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The SGX545 boot register sequence, from clock gating to the EDM kick.
 * Register access goes through callbacks so the order can be unit tested.
 */
#ifndef _GMA500_SGX_SEQ_H_
#define _GMA500_SGX_SEQ_H_

#include "sgx_fw.h"

/* SGX545 register offsets (sgx545defs.h). */
#define SGX_CLKGATECTL		0x000
#define SGX_CLKGATECTL2		0x004
#define SGX_CLKGATESTATUS	0x008
#define SGX_CORE_ID		0x01c
#define SGX_CORE_REVISION	0x020
#define SGX_SOFT_RESET		0x080
#define SGX_EVENT_HOST_CLEAR2	0x114
#define SGX_EVENT_STATUS2	0x118
#define SGX_EVENT_STATUS	0x12c
#define SGX_EVENT_HOST_ENABLE	0x130
#define SGX_EVENT_HOST_CLEAR	0x134
#define SGX_EVENT_KICK		0xac8
#define SGX_EVENT_KICK2		0xae8
#define SGX_USE0_SERV_EVENT	0xb14
#define SGX_USE1_SERV_EVENT	0xb20
#define SGX_BIF_CTRL		0xc00
#define SGX_BIF_INT_STAT	0xc04
#define SGX_BIF_FAULT		0xc08
#define SGX_BIF_DIR_LIST_BASE1	0xc38	/* 1..15, 4 bytes apart */
#define SGX_BIF_DIR_LIST_BASE15	0xc70
#define SGX_BIF_BANK_SET	0xc74
#define SGX_BIF_BANK0		0xc78
#define SGX_BIF_DIR_LIST_BASE0	0xc84
#define SGX_BIF_MEM_REQ_STAT	0xc8c

#define SGX_SOFT_RESET_ALL	0x3f	/* BIF, DPM, TA, USE, ISP, TSP */
#define SGX_SOFT_RESET_BIF	0x01
#define SGX_BIF_CTRL_INVALDC	0x8
#define SGX_BIF_FAULT_REQ	0xffff	/* BIF_INT_STAT */
#define SGX_BIF_BANK0_EDM15	0xf	/* the EDM requester uses list 15 */

struct sgx_seq {
	u32 (*rd)(void *ctx, u32 reg);
	void (*wr)(void *ctx, u32 reg, u32 val);
	void (*udelay)(void *ctx, unsigned int us);
	void (*pre_kick)(void *ctx);	/* memory writes before the kick */
	void *ctx;

	u32 clkgatectl, clkgatectl2;
	const struct sgx_fw_write *part1, *part2;
	unsigned int npart1, npart2;
	u32 reset_pd;		/* DIR_LIST_BASE0 during and after the reset */
	u32 edm_pd;		/* DIR_LIST_BASE15: the EDM's page directory */
};

int sgx_seq_boot(const struct sgx_seq *s);

#endif
