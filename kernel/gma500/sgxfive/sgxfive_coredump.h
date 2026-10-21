/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sgxfive error state (sgxfive_coredump.c): what a recovery reset captures
 * before it resets, handed to devcoredump and formatted when it is read.
 */
#ifndef _SGXFIVE_COREDUMP_H_
#define _SGXFIVE_COREDUMP_H_

#include <linux/list.h>
#include <linux/sched.h>
#include <linux/time64.h>
#include <linux/types.h>

struct drm_printer;
struct sgxfive;
struct sgxfive_ctx;

/* What sgxfive_dump() shows: registers, HostCtl, TA3DCtl, CCB control. */
#define SGXFIVE_DUMP_NREGS	17
#define SGXFIVE_HC_DWORDS	17
#define SGXFIVE_TA3D_CTL_DWORDS	(0xec / 4)

struct sgxfive_devstate {
	bool loaded, live;
	int sw_events, bif_faults;
	u64 ccb_sent;
	unsigned int ndeferred;
	struct { u32 reg, val; } regs[SGXFIVE_DUMP_NREGS];
	u32 hostctl[SGXFIVE_HC_DWORDS];
	u32 ta3dctl[SGXFIVE_TA3D_CTL_DWORDS];
	u32 ccbctl[2];
};

void sgxfive_devstate_print(struct drm_printer *p, const struct sgxfive_devstate *d);

/* Which detector found the hang. */
enum sgxfive_hang {
	SGXFIVE_HANG_MANUAL,		/* debugfs reset */
	SGXFIVE_HANG_FAULT,		/* the BIF fault watchdog */
	SGXFIVE_HANG_TIMEOUT,		/* the drm_sched job timeout */
};

/*
 * Bounds: the guilty context first, then the others in creation order, and
 * the command words of the first SGXFIVE_CD_MAX_JOBS unfinished jobs. The
 * text stays below SGXFIVE_COREDUMP_MAX.
 */
#define SGXFIVE_CD_MAX_CTXS	8
#define SGXFIVE_CD_MAX_JOBS	8
#define SGXFIVE_CD_CMD_BYTES	0x1c0	/* a transfer's size in the client CCB */
#define SGXFIVE_COREDUMP_MAX	(32 * 1024)

struct sgxfive_cd_job {
	u64 order;			/* device-wide, when the firmware got it */
	u32 seqno, size;
	u8 cmd[SGXFIVE_CD_CMD_BYTES];
};

struct sgxfive_cd_ctx {
	u64 fence_context;
	pid_t pid;			/* of the process that opened the file */
	char comm[TASK_COMM_LEN];
	bool lost;
	u32 done;			/* the fence word: last job finished */
	u32 woff, roff;			/* client CCB offsets */
	u32 npending;			/* unfinished jobs */
	u32 first_job, njobs;		/* the saved ones, in jobs[] */
};

struct sgxfive_coredump {
	struct sgxfive *sgx;
	struct timespec64 time;		/* wall clock at capture */
	u64 uptime_ns;			/* boot time, as in dmesg */
	enum sgxfive_hang hang;
	u64 suspect;			/* the job's order, U64_MAX: none */
	int guilty;			/* index in ctxs, -1: none */
	int result;			/* of the reset */
	u32 bif_int_stat, bif_fault;
	struct sgxfive_devstate dev;
	u32 nctxs, nctxs_seen;
	u32 njobs;
	struct sgxfive_cd_ctx ctxs[SGXFIVE_CD_MAX_CTXS];
	struct sgxfive_cd_job jobs[SGXFIVE_CD_MAX_JOBS];
};

void sgxfive_coredump_ctxs(struct sgxfive_coredump *cd, struct list_head *ctxs,
			   struct sgxfive_ctx *guilty);
void sgxfive_coredump_print(struct drm_printer *p, const struct sgxfive_coredump *cd);
ssize_t sgxfive_coredump_read(char *buf, loff_t offset, size_t count, void *data,
			      size_t datalen);

struct sgxfive_coredump *sgxfive_coredump_alloc(struct sgxfive *sgx,
						enum sgxfive_hang hang, u64 suspect);
void sgxfive_coredump_discard(struct sgxfive_coredump *cd);
void sgxfive_coredump_emit(struct sgxfive_coredump *cd);
void sgxfive_coredump_fini(struct sgxfive *sgx);

#endif
