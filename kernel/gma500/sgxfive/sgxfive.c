// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * SGX545 microkernel bring-up for Cedarview (GMA3600/3650).
 *
 * Loads sgx545_ukernel.fw (see sgx_fw.c), places its buffers in the SGX
 * address space with the DDK's heap layout and maps them through a private
 * page directory, the one the EDM will use. Nothing runs at probe: writing to
 * debugfs dri/N/sgx/load does the work, and the buffers can then be read back
 * from dri/N/sgx/bufNN to check them against the reference loader.
 *
 * dri/N/sgx/boot then runs the DDK's first power-on sequence (sgx_seq.c) and
 * waits for the EDM to report init complete in HostCtl; dri/N/sgx/status shows
 * the state and dri/N/sgx/stop puts the core back into reset.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <asm/set_memory.h>

#include <drm/drm_file.h>
#include <drm/drm_mm.h>
#include <drm/drm_print.h>

#include "sgxfive.h"
#include "sgxfive_coredump.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"
#include "sgxfive_regdump.h"
#include "sgx_fw.h"
#include "sgx_mmu.h"
#include "sgx_seq.h"
#include "sgx_tri.h"
#include "sgx_xfer.h"

#define SGX_FW_NAME	"sgx545_ukernel.fw"

/*
 * The GPU address layout, by sgxapi_km.h heap id: the DDK's SGX545 REV1 table
 * (sgxconfig.h), which Intel's Cedarview kernel uses when the host bridge is
 * below revision 4 (FIX_HW_BRN_SAMPLE_CACHE): every heap below 0x3e000000 and
 * textures in a heap of their own (design/sgx545_boot_sequence.md (d)).
 */
enum {
	HEAP_KERNEL_CODE = 2,
	HEAP_KERNEL_DATA = 3,
	HEAP_PIXELSHADER = 4,
	HEAP_VERTEXSHADER = 5,
	HEAP_PDSPIXEL = 6,
	HEAP_TEXTURE = 14,	/* SGX_TEXTURE_HEAP_ID */
};

static const u32 sgx_heap_base[] = {
	[HEAP_KERNEL_CODE] = 0x30000000,
	[HEAP_KERNEL_DATA] = 0x32000000,
	[HEAP_PIXELSHADER] = 0x37000000,
	[HEAP_VERTEXSHADER] = 0x3c000000,
	[HEAP_PDSPIXEL] = 0x2e000000,
	[HEAP_TEXTURE] = 0xf0000000,
};

/*
 * srvinit's allocation order: the control blocks, then the ukernel and the
 * five PDS programs. The ukernel must start exactly at the EDM code base: its
 * branch targets and the CCB service addresses are instruction indices from
 * that base, and srvinit relocates only the PDS code words. So the first
 * buffer of each heap goes at the heap base (debugfs sgx/heap_start moves it).
 */
static const u8 sgx_alloc_order[] = {
	6, 7, 8, 9, 10, 11, 12, 13, 14, 0, 1, 2, 3, 4, 5,
};

static u32 sgx_rd(struct sgxfive *sgx, u32 reg)
{
	return ioread32(sgx->regs + reg);
}

static void sgx_wr(struct sgxfive *sgx, u32 reg, u32 val)
{
	iowrite32(val, sgx->regs + reg);
}

/* Buffer ids and HostCtl dwords (design/sgx545_fw_format.md, spec §13.10). */
enum {
	BUF_CCB = 6, BUF_CCB_CTL = 7, BUF_KICKER = 8, BUF_TA3D_CTL = 9,
	BUF_HOST_CTL = 10, BUF_MISC = 11,
};

enum {
	HC_INIT_STATUS = 0, HC_POWER_STATUS = 1, HC_CLEANUP_STATUS = 2,
	HC_HWR_SAMPLE_RATE = 5,
	HC_UKERNEL_TIMER_CLOCK = 6, HC_APM_SAMPLE_RATE = 7, HC_HOST_CLOCK = 13,
	HC_ASSERT_FAIL = 14, HC_DWORDS = 17,
};

#define TA3D_CTL_DWORDS		(0xec / 4)
static_assert(HC_DWORDS == SGXFIVE_HC_DWORDS);
static_assert(TA3D_CTL_DWORDS == SGXFIVE_TA3D_CTL_DWORDS);
#define EDM_INIT_COMPLETE	0x1
#define SGX_CLOCK_HZ		400000000	/* SYS_SGX_CLOCK_SPEED */
#define SGX_EVENT_SW		0x00004000
#define SGX_EVENT_MASTER	0x80000000

/* SGXMKIF_CMD_* index into the kick table, and the misc-info protocol. */
#define SGX_CMD_TA		0
#define SGX_CMD_TRANSFER	1
#define SGX_CMD_POWER		3
#define SGX_CMD_CLEANUP		5
#define CLEANUP_RT		1	/* PVRSRV_CLEANUPCMD_*: an RT data set */
#define CLEANUP_RC		2	/* a HW render context */
#define CLEANUP_PB		5	/* a PB descriptor */
#define CLEANUP_COMPLETE	BIT(0)	/* in HostCtl.ui32CleanupStatus */
#define SGX_CMD_PROCESS_QUEUES	7
#define SGX_CMD_GETMISCINFO	6
#define POWERCMD_POWEROFF	1
#define POWERCMD_IDLE		2
#define POWMAN_IDLE_COMPLETE	BIT(2)
#define POWMAN_POWEROFF_COMPLETE BIT(3)
#define MISCINFO_READY		0x1
#define MISCINFO_GET_STRUCT_SIZES 0x2
#define MISCINFO_DWORDS		(1 + 7 + 13)	/* flags, features, sizes */
#define SGX_CC_INVAL_DATA	0x8		/* SGXMKIF_CC_INVAL_DATA */
#define SGX_CC_INVAL_BIF	0x3		/* SGXMKIF_CC_INVAL_BIF_PT | _PD */

/*
 * The transfer-queue test area, in KERNEL_DATA above the firmware's buffers so
 * the EDM's own page directory maps it: a 64 KiB client CCB (plus room for a
 * command at its end), its control block, one HW transfer context and a
 * scratch area: U (update target), F (status target) and, from SCR_SYNC, the
 * read- and write-complete counters of a destination sync. For a solid fill
 * (design/sgx545_transfer.md §4.2.7) there are also a page of PDS code at the
 * PDS heap base, a page of USE code at the pixel-shader code base, the
 * background object, and a destination of two pages plus a guard page.
 */
enum { SCR_U = 0, SCR_F = 1, SCR_SYNC_READ = 8, SCR_SYNC_WRITE = 9 };
enum {
	XB_CCB, XB_CCB_CTL, XB_CTX, XB_SCRATCH, XB_PDS, XB_USE, XB_BG, XB_DST,
	XB_SRC, XB_COUNT
};

/* By heap and offset, so the area follows the layout. */
static const struct {
	u8 heap;
	u32 off, size;
} sgxfive_xbuf[XB_COUNT] = {
	[XB_CCB] = { HEAP_KERNEL_DATA, 0x100000,
		     SGX_XFER_CCB_SIZE + SGX_XFER_CMD_SIZE },
	[XB_CCB_CTL] = { HEAP_KERNEL_DATA, 0x120000, 8 },
	[XB_CTX] = { HEAP_KERNEL_DATA, 0x121000, SGX_XFER_CTX_SIZE },
	[XB_SCRATCH] = { HEAP_KERNEL_DATA, 0x122000, 0x40 },
	[XB_PDS] = { HEAP_PDSPIXEL, 0, 0x1000 },	/* PDS_EXEC_BASE */
	[XB_USE] = { HEAP_PIXELSHADER, 0, 0x1000 },	/* USE_CODE_BASE_0 */
	[XB_BG] = { HEAP_KERNEL_DATA, 0x130000, 4 * SGX_XFER_BGOBJ1_DWORDS },
	[XB_DST] = { HEAP_KERNEL_DATA, 0x200000, 0x3000 },
	[XB_SRC] = { HEAP_TEXTURE, 0x300000, 0x2000 },	/* copy source */
};

static_assert(ARRAY_SIZE(((struct sgxfive *)NULL)->xbuf) >= XB_COUNT);

static void sgxfive_unmap(struct sgxfive *sgx, struct sgxfive_buf *b)
{
	unsigned int i;

	if (!b->pages)
		return;
	if (b->pd)
		sgx_mmu_unmap(b->pd, b->addr, b->npages);
	if (b->va)
		vunmap(b->va);
	if (b->wc)
		set_pages_array_wb(b->pages, b->npages);
	for (i = 0; i < b->npages; i++)
		if (b->pages[i])
			__free_page(b->pages[i]);
	kfree(b->pages);
	memset(b, 0, sizeof(*b));
}

static void sgxfive_stop(struct sgxfive *sgx);

static void sgxfive_unload(struct sgxfive *sgx)
{
	unsigned int id;

	sgxfive_stop(sgx);

	for (id = 0; id < SGX_FW_MAX_BUFFERS; id++)
		sgxfive_unmap(sgx, &sgx->buf[id]);
	for (id = 0; id < ARRAY_SIZE(sgx->cbuf); id++)
		sgxfive_unmap(sgx, &sgx->cbuf[id]);
	if (sgx->cpd)
		sgx_mmu_pd_destroy(sgx->cpd);	/* also drops its aliases */
	sgx->cpd = NULL;
	for (id = 0; id < XB_COUNT; id++)
		sgxfive_unmap(sgx, &sgx->xbuf[id]);
	if (sgx->tpd)
		sgx_mmu_pd_destroy(sgx->tpd);	/* aliases only */
	sgx->tpd = NULL;
	for (id = 0; id < ARRAY_SIZE(sgx->tbuf); id++)
		sgxfive_unmap(sgx, &sgx->tbuf[id]);
	sgx->xfer_woff = 0;
	sgx->xfer_runs = 0;	/* the next transfer re-initialises the context */
	if (sgx->pd)
		sgx_mmu_pd_destroy(sgx->pd);
	sgx->pd = NULL;
	release_firmware(sgx->blob);
	sgx->blob = NULL;
	memset(&sgx->layout, 0, sizeof(sgx->layout));
	sgx->loaded = false;
}

/* Allocate a zeroed buffer write-combined, map it for the CPU and in pd. */
static int sgxfive_map_pd(struct sgxfive *sgx, struct sgx_mmu_pd *pd,
			  struct sgxfive_buf *b, u32 addr, u32 size)
{
	unsigned int i;
	int ret;

	b->npages = DIV_ROUND_UP(size, PAGE_SIZE);
	b->pages = kcalloc(b->npages, sizeof(*b->pages), GFP_KERNEL);
	if (!b->pages)
		return -ENOMEM;
	for (i = 0; i < b->npages; i++) {
		b->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!b->pages[i])
			return -ENOMEM;
	}
	set_pages_array_wc(b->pages, b->npages);
	b->wc = true;
	b->va = vmap(b->pages, b->npages, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!b->va)
		return -ENOMEM;
	ret = sgx_mmu_map(pd, addr, b->pages, b->npages, 0);
	if (ret)
		return ret;
	b->addr = addr;
	b->pd = pd;
	b->blob.data = b->va;
	b->blob.size = size;
	return 0;
}

/* ...in the EDM's own (kernel) page directory. */
static int sgxfive_map(struct sgxfive *sgx, struct sgxfive_buf *b, u32 addr,
		       u32 size)
{
	return sgxfive_map_pd(sgx, sgx->pd, b, addr, size);
}

static int sgxfive_load(struct sgxfive *sgx)
{
	u8 *bufs[SGX_FW_MAX_BUFFERS] = { NULL };
	struct sgx_fw_layout *l = &sgx->layout;
	unsigned int i;
	int ret;

	ret = request_firmware(&sgx->blob, SGX_FW_NAME, sgx->dev->dev);
	if (ret)
		return ret;
	ret = sgx_fw_parse(&sgx->fw, sgx->blob->data, sgx->blob->size);
	if (ret)
		goto err;

	for (i = 0; i < ARRAY_SIZE(sgx_heap_base); i++) {
		if (sgx_heap_base[i]) {
			l->heap_base[i] = sgx_heap_base[i];
			l->heap_set |= 1u << i;
		}
	}
	ret = sgx_fw_place(sgx->fw.buffers, sgx->fw.nbuffers, sgx_alloc_order,
			   ARRAY_SIZE(sgx_alloc_order), sgx->heap_start, PAGE_SIZE,
			   l);
	if (ret)
		goto err;

	/* Faults trap rather than hit a dummy page, so a bad access shows. */
	sgx->pd = sgx_mmu_pd_create();
	if (!sgx->pd) {
		ret = -ENOMEM;
		goto err;
	}
	l->pd_phys = sgx_mmu_pd_phys(sgx->pd);

	for (i = 0; i < sgx->fw.nbuffers; i++) {
		const struct sgx_fw_buffer *fb = &sgx->fw.buffers[i];

		ret = sgxfive_map(sgx, &sgx->buf[fb->id], l->addr[fb->id],
				  fb->size);
		if (ret)
			goto err;
		bufs[fb->id] = sgx->buf[fb->id].va;
	}
	for (i = 0; i < XB_COUNT; i++) {
		ret = sgxfive_map(sgx, &sgx->xbuf[i],
				  sgx_heap_base[sgxfive_xbuf[i].heap] +
				  sgxfive_xbuf[i].off, sgxfive_xbuf[i].size);
		if (ret)
			goto err;
	}
	ret = sgx_fw_build(&sgx->fw, l, bufs);
	if (ret)
		goto err;
	/* The buffers are write-combined: drain them before the SGX reads. */
	wmb();

	sgx->loaded = true;
	drm_info(sgx->dev, "SGX firmware DDK %u.%u.%u loaded, EDM PD at %#x\n",
		 sgx->fw.ddk_major, sgx->fw.ddk_minor, sgx->fw.ddk_build,
		 l->pd_phys);
	return 0;
err:
	sgxfive_unload(sgx);
	return ret;
}

static ssize_t sgxfive_load_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	if (sgx->kmem_users) {
		ret = -EBUSY;	/* contexts live in the EDM's page directory */
	} else {
		if (sgx->loaded)
			sgxfive_unload(sgx);
		ret = sgxfive_load(sgx);
	}
	mutex_unlock(&sgx->lock);
	if (ret)
		drm_err(sgx->dev, "SGX firmware load failed: %d\n", ret);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_load_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_load_write,
};

/* The placement and PD the buffers were built for, as fwload.py layout JSON. */
static int sgxfive_layout_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	const struct sgx_fw_layout *l = &sgx->layout;
	unsigned int i;
	const char *sep = "";

	mutex_lock(&sgx->lock);
	if (!sgx->loaded) {
		mutex_unlock(&sgx->lock);
		return -ENODATA;
	}
	seq_puts(m, "{\"addr\": {");
	for (i = 0; i < SGX_FW_MAX_BUFFERS; i++) {
		if (l->addr_set & 1u << i) {
			seq_printf(m, "%s\"%u\": \"%#x\"", sep, i, l->addr[i]);
			sep = ", ";
		}
	}
	seq_puts(m, "}, \"heap_base\": {");
	sep = "";
	for (i = 0; i < SGX_FW_MAX_HEAPS; i++) {
		if (l->heap_set & 1u << i) {
			seq_printf(m, "%s\"%u\": \"%#x\"", sep, i, l->heap_base[i]);
			sep = ", ";
		}
	}
	seq_printf(m, "}, \"pd_phys\": \"%#x\"}\n", l->pd_phys);
	mutex_unlock(&sgx->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_layout);

static u32 *sgxfive_words(struct sgxfive *sgx, unsigned int id)
{
	return sgx->buf[id].va;
}

static u32 seq_rd(void *ctx, u32 reg)
{
	return sgx_rd(ctx, reg);
}

static void seq_wr(void *ctx, u32 reg, u32 val)
{
	sgx_wr(ctx, reg, val);
	(void)sgx_rd(ctx, reg);		/* post */
}

static void seq_udelay(void *ctx, unsigned int us)
{
	udelay(us);
}

static void seq_wmb(void *ctx)
{
	wmb(); /* the command and offsets before the kick */
}

/* C7, C9-C11: the kicker word, the host clock, InitStatus, then a barrier. */
static void seq_pre_kick(void *ctx)
{
	struct sgxfive *sgx = ctx;
	u32 *hc = sgxfive_words(sgx, BUF_HOST_CTL);

	WRITE_ONCE(*sgxfive_words(sgx, BUF_KICKER), 0);
	WRITE_ONCE(hc[HC_HOST_CLOCK], (u32)(jiffies * (1000000 / HZ)));
	WRITE_ONCE(hc[HC_INIT_STATUS], 0);
	wmb(); /* the EDM must see all of the above before the kick */
}

/* Poll a dword the EDM writes until (v & mask) == want, for up to 500 ms. */
static int sgxfive_wait_mem(const u32 *w, u32 mask, u32 want)
{
	ktime_t start = ktime_get();

	while ((READ_ONCE(*w) & mask) != want) {
		if (ktime_ms_delta(ktime_get(), start) > 500)
			return -ETIMEDOUT;
		usleep_range(50, 100);
		rmb(); /* re-read from memory */
	}
	return 0;
}

static const u32 sgxfive_dump_regs[] = {
	SGX_CORE_ID, SGX_CORE_REVISION, SGX_CLKGATECTL, SGX_CLKGATESTATUS,
	SGX_SOFT_RESET, SGX_EVENT_STATUS, SGX_EVENT_STATUS2,
	SGX_EVENT_HOST_ENABLE, SGX_BIF_CTRL, SGX_BIF_BANK0, SGX_BIF_INT_STAT,
	SGX_BIF_FAULT, SGX_BIF_MEM_REQ_STAT, SGX_BIF_DIR_LIST_BASE0,
	SGX_BIF_DIR_LIST_BASE15, SGX_USE0_SERV_EVENT, SGX_USE1_SERV_EVENT,
};

static_assert(ARRAY_SIZE(sgxfive_dump_regs) == SGXFIVE_DUMP_NREGS);

/* What SGXDumpDebugInfo shows: registers, HostCtl, TA3DCtl, CCB control. */
static void sgxfive_devstate(struct sgxfive *sgx, struct sgxfive_devstate *d)
{
	unsigned int i;

	d->loaded = sgx->loaded;
	d->live = sgx->live;
	d->sw_events = atomic_read(&sgx->sw_events);
	d->bif_faults = atomic_read(&sgx->bif_faults);
	d->ccb_sent = sgx->ccb_sent;
	d->ndeferred = sgx->ndeferred;
	for (i = 0; i < ARRAY_SIZE(sgxfive_dump_regs); i++) {
		d->regs[i].reg = sgxfive_dump_regs[i];
		d->regs[i].val = sgx_rd(sgx, sgxfive_dump_regs[i]);
	}
	if (!sgx->loaded)
		return;
	rmb(); /* fresh reads of what the EDM wrote */
	memcpy(d->hostctl, sgxfive_words(sgx, BUF_HOST_CTL), sizeof(d->hostctl));
	memcpy(d->ta3dctl, sgxfive_words(sgx, BUF_TA3D_CTL), sizeof(d->ta3dctl));
	memcpy(d->ccbctl, sgxfive_words(sgx, BUF_CCB_CTL), sizeof(d->ccbctl));
}

static void sgxfive_dump(struct sgxfive *sgx, struct drm_printer *p)
{
	struct sgxfive_devstate d;

	sgxfive_devstate(sgx, &d);
	sgxfive_devstate_print(p, &d);
}

/* Deinit script, then hold every block in reset; the EDM stops touching memory. */
static int sgxfive_power(struct sgxfive *sgx, u32 powercmd, u32 done);

/*
 * SGXPrePowerState(OFF): ask the EDM to power off, then run the deinit script.
 * Hold every block in reset afterwards either way, so the EDM is certain to
 * have stopped touching memory before its buffers are freed.
 */
static void sgxfive_halt(struct sgxfive *sgx)
{
	struct sgx_fw_write w[16];
	int n, i;

	n = sgx_fw_script(&sgx->fw, &sgx->layout, SGX_FW_DEINIT, w, ARRAY_SIZE(w));
	for (i = 0; i < n; i++)
		seq_wr(sgx, w[i].reg, w[i].val);
	seq_wr(sgx, SGX_SOFT_RESET, SGX_SOFT_RESET_ALL);
	WRITE_ONCE(sgx->live, false);
	drm_info(sgx->dev, "SGX stopped, %d events seen\n",
		 atomic_read(&sgx->sw_events));
	/* The BIF is held in reset: nothing deferred is reachable any more. */
	if (sgx->ndeferred)
		mod_delayed_work(system_dfl_wq, &sgx->reaper, 0);
}

static void sgxfive_stop(struct sgxfive *sgx)
{
	int ret;

	if (!sgx->live)
		return;
	ret = sgxfive_power(sgx, POWERCMD_POWEROFF, POWMAN_POWEROFF_COMPLETE);
	if (ret)
		drm_warn(sgx->dev, "SGX power-off request failed: %d\n", ret);
	sgxfive_halt(sgx);
}

static int sgxfive_boot(struct sgxfive *sgx)
{
	struct sgx_fw_write part1[16], part2[64];
	struct drm_printer p = drm_info_printer(sgx->dev->dev);
	struct sgx_seq s = {
		.rd = seq_rd, .wr = seq_wr, .udelay = seq_udelay,
		.pre_kick = seq_pre_kick, .ctx = sgx,
		.part1 = part1, .part2 = part2,
	};
	ktime_t start;
	u32 *hc;
	int n, ret;

	if (sgx->live)
		return -EBUSY;
	if (!sgx->loaded) {
		ret = sgxfive_load(sgx);
		if (ret)
			return ret;
	}
	n = sgx_fw_script(&sgx->fw, &sgx->layout, SGX_FW_PART1, part1,
			  ARRAY_SIZE(part1));
	if (n < 0)
		return n;
	s.npart1 = n;
	n = sgx_fw_script(&sgx->fw, &sgx->layout, SGX_FW_PART2, part2,
			  ARRAY_SIZE(part2));
	if (n < 0)
		return n;
	s.npart2 = n;
	s.clkgatectl = sgx->fw.meta[SGX_FW_META_CLKGATE];
	s.clkgatectl2 = sgx->fw.meta[SGX_FW_META_CLKGATE + 1];
	/* DIR_LIST_BASE0 gets gma500's PD back, as BRN28011 does in the DDK. */
	s.reset_pd = sgx->reset_pd;
	s.edm_pd = sgx->layout.pd_phys;

	/* HostCtl as SGXPostPowerState leaves it (B1-B5). */
	hc = sgxfive_words(sgx, BUF_HOST_CTL);
	WRITE_ONCE(hc[HC_POWER_STATUS], 0);
	WRITE_ONCE(hc[HC_HWR_SAMPLE_RATE], 10);
	WRITE_ONCE(hc[HC_UKERNEL_TIMER_CLOCK], SGX_CLOCK_HZ / 1000);
	WRITE_ONCE(hc[HC_APM_SAMPLE_RATE], 0);

	atomic_set(&sgx->sw_events, 0);
	atomic_set(&sgx->bif_faults, 0);
	WRITE_ONCE(sgx->live, true);
	drm_info(sgx->dev, "SGX boot: kicking the EDM\n");
	ret = sgx_seq_boot(&s);
	if (ret) {
		drm_err(sgx->dev, "SGX boot: BIF fault during reset\n");
		sgxfive_dump(sgx, &p);
		WRITE_ONCE(sgx->live, false);
		return ret;
	}
	mb(); /* OSMemoryBarrier after the kick */

	start = ktime_get();
	ret = sgxfive_wait_mem(&hc[HC_INIT_STATUS], EDM_INIT_COMPLETE,
			       EDM_INIT_COMPLETE);
	if (ret) {
		drm_err(sgx->dev, "SGX boot: no INIT_COMPLETE after 500 ms\n");
		sgxfive_dump(sgx, &p);
		return ret;
	}
	drm_info(sgx->dev, "SGX boot: EDM up after %lld us\n",
		 ktime_us_delta(ktime_get(), start));
	/* InitDevInfo: the first command invalidates the BIF PD cache. */
	sgx->cache_control = sgx->fw.meta[SGX_FW_META_CACHE_CONTROL];
	sgxfive_dump(sgx, &p);
	return 0;
}

/* With every block in reset: drop a latched BIF fault and stale events. */
static void sgxfive_clear_faults(struct sgxfive *sgx)
{
	u32 st = sgx_rd(sgx, SGX_EVENT_STATUS), st2 = sgx_rd(sgx, SGX_EVENT_STATUS2);

	drm_info(sgx->dev, "SGX reset: BIF_FAULT %#x EVENT_STATUS %#x/%#x\n",
		 sgx_rd(sgx, SGX_BIF_FAULT), st, st2);
	sgx_wr(sgx, SGX_BIF_CTRL, SGX_BIF_CTRL_CLEAR_FAULT);
	sgx_wr(sgx, SGX_BIF_CTRL, 0);
	sgx_wr(sgx, SGX_EVENT_HOST_CLEAR, st);
	sgx_wr(sgx, SGX_EVENT_HOST_CLEAR2, st2);
	udelay(1);
}

/*
 * The order of the job a hang is blamed on, or U64_MAX if none is unfinished:
 * with pd, a page directory a BIF fault went through, the oldest unfinished
 * job on that directory, else the oldest of all (sgxfive_ctx_suspect).
 */
static u64 sgxfive_suspect(struct sgxfive *sgx, u32 pd)
{
	u64 order;

	spin_lock_irq(&sgx->ctx_lock);
	sgxfive_ctx_suspect(&sgx->ctx_list, pd, &order);
	spin_unlock_irq(&sgx->ctx_lock);
	return order;
}

/* The order of the oldest job the firmware has not finished, or U64_MAX. */
u64 sgxfive_oldest(struct sgxfive *sgx)
{
	return sgxfive_suspect(sgx, 0);
}

/*
 * The page directory a BIF fault went through, from BIF_FAULT's directory
 * list and that list's DIR_LIST_BASE; 0 for the EDM's own list 15, which
 * names no client. Read while the faulting pipe is stalled, so the EDM has
 * not handed the list to another context.
 */
static u32 sgxfive_fault_pd(struct sgxfive *sgx, u32 bif_fault)
{
	const unsigned int list = sgx_bif_fault_list(bif_fault);

	if (list == SGX_BIF_EDM_LIST)
		return 0;
	return sgx_rd(sgx, sgx_bif_dir_list_reg(list)) & SGX_BIF_DIR_LIST_ADDR;
}

/*
 * The firmware hung on job suspect, which the detector why chose (the oldest
 * job on a BIF fault's directory, or the oldest of all): fail its context,
 * suspect with the hang's error and the jobs behind it with -ECANCELED. False
 * when suspect is no longer its context's oldest unfinished job, because
 * another detector already recovered from it. U64_MAX blames no one. When
 * there is a hang, cd (if any) gets every context's unfinished jobs first.
 */
static bool sgxfive_blame(struct sgxfive *sgx, u64 suspect, enum sgxfive_hang why,
			  struct sgxfive_coredump *cd)
{
	struct sgxfive_ctx *guilty;
	bool hang;

	spin_lock_irq(&sgx->ctx_lock);
	guilty = sgxfive_ctx_of_job(&sgx->ctx_list, suspect);
	hang = suspect == U64_MAX || guilty;
	if (hang && cd)
		sgxfive_coredump_ctxs(cd, &sgx->ctx_list, guilty);
	if (hang && guilty) {
		WRITE_ONCE(guilty->lost, true);
		sgxfive_fence_fail_guilty(guilty, sgxfive_hang_error(why));
		if (!guilty->client->banned &&
		    sgxfive_client_fault(guilty->client, jiffies))
			drm_warn(sgx->dev, "sgxfive: %u faults within a minute, file banned\n",
				 SGXFIVE_FAULT_BUDGET);
	}
	spin_unlock_irq(&sgx->ctx_lock);
	return hang;
}

/*
 * Recovery reset (HWR): halt the firmware without asking a wedged EDM to power
 * off, rebuild every firmware buffer from the file in place and boot again.
 * The buffers keep their pages and addresses, so the kernel memory of the
 * render node's contexts, mapped in the same directory, stays where it is.
 * Every context starts over: its HW context image is rebuilt and its client
 * CCB rewritten with its unfinished jobs, which run again after the boot.
 * With a suspect job (not U64_MAX), its context is lost instead (see
 * sgxfive_blame), and nothing is done if it has already been recovered from.
 * A reset first captures the error state into *dump (sgxfive_coredump.c),
 * NULL when there is none.
 */
static int sgxfive_reset(struct sgxfive *sgx, u64 suspect, enum sgxfive_hang why,
			 struct sgxfive_coredump **dump)
{
	u8 *bufs[SGX_FW_MAX_BUFFERS] = { NULL };
	struct sgxfive_coredump *cd;
	struct sgxfive_ctx *ctx;
	unsigned int i;
	int ret;

	*dump = NULL;
	if (!sgx->loaded)
		return -ENODEV;
	cd = sgxfive_coredump_alloc(sgx, why, suspect);
	if (!sgxfive_blame(sgx, suspect, why, cd)) {
		sgxfive_coredump_discard(cd);
		return 0;
	}
	if (cd) {
		cd->bif_int_stat = sgx_rd(sgx, SGX_BIF_INT_STAT);
		cd->bif_fault = sgx_rd(sgx, SGX_BIF_FAULT);
		sgxfive_devstate(sgx, &cd->dev);
		*dump = cd;
	}
	if (sgx->live)
		sgxfive_halt(sgx);
	sgxfive_clear_faults(sgx);
	for (i = 0; i < sgx->fw.nbuffers; i++) {
		unsigned int id = sgx->fw.buffers[i].id;

		bufs[id] = sgx->buf[id].va;
	}
	ret = sgx_fw_build(&sgx->fw, &sgx->layout, bufs);
	if (ret)
		return ret;
	/* The debugfs transfer context and its CCB start over too. */
	for (i = 0; i < XB_COUNT; i++)
		memset(sgx->xbuf[i].va, 0, sgxfive_xbuf[i].size);
	sgx->xfer_woff = 0;
	sgx->xfer_runs = 0;
	spin_lock_irq(&sgx->ctx_lock);
	list_for_each_entry(ctx, &sgx->ctx_list, link) {
		if (ctx->type == DRM_SGXFIVE_CTX_TYPE_RENDER) {
			ctx->replay = sgxfive_render_reset(ctx);
			continue;
		}
		sgx_xfer_ctx_init(sgxfive_ctx_cpu(ctx, SGXFIVE_CTX_HW_OFF),
				  sgxfive_vm_pd_phys(ctx->vm),
				  ctx->mem.addr + SGXFIVE_CTX_CCB_OFF,
				  ctx->mem.addr + SGXFIVE_CTX_CTL_OFF);
		ctx->replay = sgxfive_ctx_rewind(ctx);
	}
	spin_unlock_irq(&sgx->ctx_lock);
	wmb(); /* the write-combined buffers, before the SGX reads them */

	ret = sgxfive_boot(sgx);
	spin_lock_irq(&sgx->ctx_lock);
	list_for_each_entry(ctx, &sgx->ctx_list, link) {
		if (ret) {
			WRITE_ONCE(ctx->lost, true);	/* nothing runs any more */
			sgxfive_fence_fail_all(ctx, -EIO);
		} else if (ctx->replay && ctx->type == DRM_SGXFIVE_CTX_TYPE_RENDER) {
			sgxfive_kick_render(sgx, ctx->mem.addr + SGXFIVE_CTX_HW_OFF);
		} else if (ctx->replay) {
			sgxfive_kick_transfer(sgx, ctx->mem.addr + SGXFIVE_CTX_HW_OFF);
		}
	}
	spin_unlock_irq(&sgx->ctx_lock);
	return ret;
}

/*
 * Reset the firmware and recover the render node's contexts (see above); why
 * says which detector found the hang, for the error state.
 */
int sgxfive_recover(struct sgxfive *sgx, u64 suspect, enum sgxfive_hang why)
{
	struct sgxfive_coredump *cd;
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_reset(sgx, suspect, why, &cd);
	mutex_unlock(&sgx->lock);
	if (cd) {
		cd->result = ret;
		sgxfive_coredump_emit(cd);
	}
	if (ret)
		drm_err(sgx->dev, "SGX recovery failed: %d\n", ret);
	return ret;
}

/*
 * Kernel memory for the render node (contexts and their CCBs): pages mapped
 * write-combined in the EDM's page directory, from the kernel data heap above
 * the firmware's buffers and the debugfs test area.
 */
#define KMEM_START	0x400000
#define KMEM_END	0x05000000		/* REV1 kernel data heap size */

int sgxfive_kmem_alloc(struct sgxfive *sgx, struct drm_mm_node *node,
		       struct sgxfive_buf *b, u32 size)
{
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgx->pd ? 0 : -ENODEV;	/* the firmware must be loaded */
	if (!ret)
		ret = drm_mm_insert_node(&sgx->kmem, node, PAGE_ALIGN(size));
	if (!ret) {
		ret = sgxfive_map_pd(sgx, sgx->pd, b,
				     sgx_heap_base[HEAP_KERNEL_DATA] + node->start,
				     size);
		if (ret) {
			sgxfive_unmap(sgx, b);
			drm_mm_remove_node(node);
		}
	}
	if (!ret)
		sgx->kmem_users++;
	mutex_unlock(&sgx->lock);
	return ret;
}

void sgxfive_kmem_free(struct sgxfive *sgx, struct drm_mm_node *node,
		       struct sgxfive_buf *b)
{
	mutex_lock(&sgx->lock);
	sgxfive_unmap(sgx, b);
	drm_mm_remove_node(node);
	sgx->kmem_users--;
	mutex_unlock(&sgx->lock);
}

static int sgxfive_command(struct sgxfive *sgx, unsigned int cmd,
			   const u32 *data, unsigned int ndata);

/* Queue the firmware's transfer work for one render-node context. */
int sgxfive_kick_transfer(struct sgxfive *sgx, u32 hw_ctx_addr)
{
	const u32 data[] = { 0, hw_ctx_addr };

	lockdep_assert_held(&sgx->lock);
	sgx->cache_control |= SGX_CC_INVAL_DATA;
	return sgxfive_command(sgx, SGX_CMD_TRANSFER, data, ARRAY_SIZE(data));
}

/* Queue the firmware's TA work for one render context (§11.6). */
int sgxfive_kick_render(struct sgxfive *sgx, u32 hw_ctx_addr)
{
	const u32 data[] = { 0, hw_ctx_addr };

	lockdep_assert_held(&sgx->lock);
	sgx->cache_control |= SGX_CC_INVAL_DATA;
	return sgxfive_command(sgx, SGX_CMD_TA, data, ARRAY_SIZE(data));
}

/* SGXCleanupRequest: one CLEANUP command, acknowledged in HostCtl. */
static int sgxfive_cleanup(struct sgxfive *sgx, u32 type, u32 addr)
{
	u32 *hc = sgxfive_words(sgx, BUF_HOST_CTL);
	const u32 data[] = { type, addr };
	int ret;

	WRITE_ONCE(hc[HC_CLEANUP_STATUS],
		   READ_ONCE(hc[HC_CLEANUP_STATUS]) & ~CLEANUP_COMPLETE);
	ret = sgxfive_command(sgx, SGX_CMD_CLEANUP, data, ARRAY_SIZE(data));
	if (!ret)
		ret = sgxfive_wait_mem(&hc[HC_CLEANUP_STATUS], CLEANUP_COMPLETE,
				       CLEANUP_COMPLETE);
	WRITE_ONCE(hc[HC_CLEANUP_STATUS],
		   READ_ONCE(hc[HC_CLEANUP_STATUS]) & ~CLEANUP_COMPLETE);
	sgx->cache_control |= SGX_CC_INVAL_DATA;
	return ret;
}

/*
 * A render context goes: make the firmware drop what it keeps of it (its RT
 * data set, the context, the PB descriptor it may still have loaded,
 * design/sgx545_render_uapi.md §11.7). Nothing to do with the firmware off.
 */
int sgxfive_render_cleanup(struct sgxfive *sgx, u32 rtds, u32 ctx, u32 pbdesc)
{
	ktime_t start = ktime_get();
	int ret = 0;

	mutex_lock(&sgx->lock);
	if (sgx->live) {
		ret = sgxfive_cleanup(sgx, CLEANUP_RT, rtds);
		if (!ret)
			ret = sgxfive_cleanup(sgx, CLEANUP_RC, ctx);
		if (!ret)
			ret = sgxfive_cleanup(sgx, CLEANUP_PB, pbdesc);
		drm_dbg(sgx->dev, "sgxfive: render context cleanup %d after %lld us\n",
			ret, ktime_us_delta(ktime_get(), start));
	}
	mutex_unlock(&sgx->lock);
	return ret;
}

/*
 * Map (or unmap) every firmware buffer into a render VM's page directory at
 * its own address: the EDM runs through the context's directory while it
 * loads a parameter buffer (§11.3). Read-only, except the TA/3D control
 * block, which the EDM writes in that window (TA3DCtl[46] and [47], the
 * loaded PB): read-only, the write faults and the render stalls (seen on
 * hardware, probe R4; debugfs alias_ta3d_ro reproduces it).
 */
int sgxfive_alias_firmware(struct sgxfive *sgx, struct sgx_mmu_pd *pd, bool map)
{
	unsigned int id;
	int ret = 0;

	mutex_lock(&sgx->lock);
	for (id = 0; id < SGX_FW_MAX_BUFFERS && !ret; id++) {
		const struct sgxfive_buf *b = &sgx->buf[id];
		u32 flags = id == BUF_TA3D_CTL && !sgx->alias_ta3d_ro ? 0 : SGX_MMU_RO;

		if (!b->pd)
			continue;
		if (map)
			ret = sgx_mmu_map(pd, b->addr, b->pages, b->npages, flags);
		else
			sgx_mmu_unmap(pd, b->addr, b->npages);
	}
	if (ret)
		for (id = 0; id < SGX_FW_MAX_BUFFERS; id++)
			if (sgx->buf[id].pd)
				sgx_mmu_unmap(pd, sgx->buf[id].addr, sgx->buf[id].npages);
	sgx->cache_control |= SGX_CC_INVAL_BIF;
	mutex_unlock(&sgx->lock);
	return ret;
}

#define SGXFIVE_REAP_MS	10

/*
 * Free, once the firmware cannot reach it, memory it might have cached
 * translations for: the next kernel-CCB command invalidates the BIF caches
 * (design §3.3), and the item goes when that command has been processed,
 * that is when the EDM's read offset has passed it. With the firmware halted
 * the BIF is in reset, so it goes at once.
 */
void sgxfive_defer(struct sgxfive *sgx, struct sgxfive_deferred *d)
{
	mutex_lock(&sgx->lock);
	if (!sgx->live) {
		mutex_unlock(&sgx->lock);
		d->free(d);
		return;
	}
	sgx->cache_control |= SGX_CC_INVAL_BIF;
	d->after = sgx->ccb_sent + 1;
	list_add_tail(&d->link, &sgx->deferred);
	sgx->ndeferred++;
	mutex_unlock(&sgx->lock);
	queue_delayed_work(system_dfl_wq, &sgx->reaper,
			   msecs_to_jiffies(SGXFIVE_REAP_MS));
}

/* Under lock: move what may be freed to due. */
static void sgxfive_reap_locked(struct sgxfive *sgx, struct list_head *due)
{
	u64 done = U64_MAX;

	if (sgx->live) {
		u32 *ctl = sgxfive_words(sgx, BUF_CCB_CTL);

		rmb(); /* the read offset the EDM wrote */
		done = sgxfive_ccb_done(sgx->ccb_sent, READ_ONCE(ctl[0]),
					READ_ONCE(ctl[1]));
	}
	sgx->ndeferred -= sgxfive_reap(&sgx->deferred, done, due);
}

static void sgxfive_reaper(struct work_struct *work)
{
	struct sgxfive *sgx = container_of(to_delayed_work(work), struct sgxfive,
					   reaper);
	struct sgxfive_deferred *last;
	LIST_HEAD(due);
	bool more;

	mutex_lock(&sgx->lock);
	sgxfive_reap_locked(sgx, &due);
	last = list_last_entry_or_null(&sgx->deferred, struct sgxfive_deferred, link);
	/* No command has carried the invalidation yet: send one that does. */
	if (last && last->after > sgx->ccb_sent)
		sgxfive_command(sgx, SGX_CMD_PROCESS_QUEUES, NULL, 0);
	more = sgx->ndeferred;
	mutex_unlock(&sgx->lock);
	sgxfive_free_all(&due);
	if (more)
		queue_delayed_work(system_dfl_wq, &sgx->reaper,
				   msecs_to_jiffies(SGXFIVE_REAP_MS));
}

#define SGXFIVE_WATCHDOG_MS	50

/*
 * While jobs are in flight. A BIF fault under a job stalls the 3D pipe without
 * raising an interrupt (design/sgx545_transfer.md §4.2), so poll for one
 * instead of waiting for the job timeout. The job to blame is the oldest one
 * on the page directory the fault went through, the faulting VM's (the oldest
 * of all when the fault names none): a transfer the firmware ran ahead of an
 * older render is blamed, not the render. Not every fault is a hang: the
 * tiling engine and cache requestors fault at page 0 during dummy transfers
 * and the firmware carries on. So recover only when that job is still the
 * same, unfinished, a poll after the fault was seen. The directory is read
 * when a new BIF_FAULT value is first seen.
 */
static void sgxfive_watchdog(struct work_struct *work)
{
	struct sgxfive *sgx = container_of(to_delayed_work(work), struct sgxfive,
					   watchdog);
	u32 stat, fault;
	u64 suspect;

	if (sgxfive_oldest(sgx) == U64_MAX || !READ_ONCE(sgx->live))
		return;			/* the next job arms it again */
	stat = sgx_rd(sgx, SGX_BIF_INT_STAT);
	if (!(stat & SGX_BIF_FAULT_REQ)) {
		sgx->fault_suspect = U64_MAX;
		sgxfive_watchdog_arm(sgx);
		return;
	}
	fault = sgx_rd(sgx, SGX_BIF_FAULT);
	if (sgx->fault_suspect == U64_MAX || fault != sgx->fault_reg) {
		sgx->fault_reg = fault;
		sgx->fault_pd = sgxfive_fault_pd(sgx, fault);
	}
	suspect = sgxfive_suspect(sgx, sgx->fault_pd);
	if (suspect != sgx->fault_suspect) {
		sgx->fault_suspect = suspect;	/* look again next time */
	} else {
		drm_err(sgx->dev, "sgxfive: BIF fault %#x at %#x (PD %#x) stalls job %llu, recovering\n",
			stat, fault, sgx->fault_pd, suspect);
		sgxfive_recover(sgx, suspect, SGXFIVE_HANG_FAULT);
		sgx->fault_suspect = U64_MAX;
	}
	sgxfive_watchdog_arm(sgx);
}

void sgxfive_watchdog_arm(struct sgxfive *sgx)
{
	queue_delayed_work(system_dfl_wq, &sgx->watchdog,
			   msecs_to_jiffies(SGXFIVE_WATCHDOG_MS));
}

/* Load and boot the firmware if it is not running yet. */
int sgxfive_ensure_live(struct sgxfive *sgx)
{
	int ret = 0;

	mutex_lock(&sgx->lock);
	if (!sgx->live)
		ret = sgxfive_boot(sgx);	/* loads it first if needed */
	mutex_unlock(&sgx->lock);
	return ret;
}

/* Page tables changed: the next command makes the BIF refetch them. */
void sgxfive_need_bif_inval(struct sgxfive *sgx)
{
	mutex_lock(&sgx->lock);
	sgx->cache_control |= SGX_CC_INVAL_BIF;
	mutex_unlock(&sgx->lock);
}

/* Submit one kernel CCB command (SGXScheduleCCBCommand). */
static int sgxfive_command(struct sgxfive *sgx, unsigned int cmd,
			   const u32 *data, unsigned int ndata)
{
	struct sgx_seq s = {
		.wr = seq_wr, .wmb = seq_wmb, .ctx = sgx,
	};
	int ret;

	if (!sgx->live)
		return -ENODEV;
	ret = sgx_ccb_submit(&s, sgxfive_words(sgx, BUF_CCB),
			     sgxfive_words(sgx, BUF_CCB_CTL),
			     sgxfive_words(sgx, BUF_KICKER),
			     sgx->fw.meta[SGX_FW_META_KICK + cmd],
			     sgx->cache_control, data, ndata);
	if (!ret) {
		sgx->cache_control = 0;
		sgx->ccb_sent++;
	}
	return ret;
}

/*
 * SGXMKIF_CMD_POWER (SGXPrePowerState): POWEROFF or IDLE, wait for the EDM to
 * report the transition in HostCtl.ui32PowerStatus, then for the clocks to
 * gate (the init info's clock-gating status register and mask).
 */
static int sgxfive_power(struct sgxfive *sgx, u32 powercmd, u32 done)
{
	u32 *hc = sgxfive_words(sgx, BUF_HOST_CTL);
	u32 reg = sgx->fw.meta[SGX_FW_META_CLKGATE + 2];
	u32 mask = sgx->fw.meta[SGX_FW_META_CLKGATE + 3];
	const u32 data[] = { 0, powercmd };
	ktime_t start = ktime_get();
	int ret;

	WRITE_ONCE(hc[HC_POWER_STATUS], READ_ONCE(hc[HC_POWER_STATUS]) & ~done);
	ret = sgxfive_command(sgx, SGX_CMD_POWER, data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	ret = sgxfive_wait_mem(&hc[HC_POWER_STATUS], done, done);
	if (ret)
		return ret;
	while (sgx_rd(sgx, reg) & mask) {
		if (ktime_ms_delta(ktime_get(), start) > 500)
			return -ETIMEDOUT;
		usleep_range(50, 100);
	}
	drm_info(sgx->dev, "SGX %s after %lld us\n",
		 powercmd == POWERCMD_IDLE ? "idle" : "powered off",
		 ktime_us_delta(ktime_get(), start));
	return 0;
}

static ssize_t sgxfive_idle_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_power(sgx, POWERCMD_IDLE, POWMAN_IDLE_COMPLETE);
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_idle_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_idle_write,
};

static const char *const sgxfive_feature_names[] = {
	"core rev", "core id", "ddk version", "ddk build", "core id sw",
	"core rev sw", "build options",
};

static void sgxfive_check(struct drm_printer *p, const char *what, u32 got,
			  u32 want, int *bad)
{
	drm_printf(p, "%-16s %#010x %s\n", what, got,
		   got == want ? "ok" : "MISMATCH");
	if (got != want) {
		drm_printf(p, "%-16s expected %#010x\n", "", want);
		(*bad)++;
	}
}

/*
 * SGXGetMiscInfoUkernel with GET_STRUCT_SIZES: the EDM fills in the core and
 * DDK identity and the MKIF structure sizes it was built with. Check every
 * value against the registers and the firmware file.
 */
static int sgxfive_miscinfo(struct sgxfive *sgx, struct drm_printer *p)
{
	u32 *mi = sgxfive_words(sgx, BUF_MISC);
	const u32 data[] = { 0, sgx->layout.addr[BUF_MISC] };
	u32 want[7];
	ktime_t start;
	int i, ret, bad = 0;

	for (i = 1; i < MISCINFO_DWORDS; i++)
		WRITE_ONCE(mi[i], 0);
	WRITE_ONCE(mi[0], MISCINFO_GET_STRUCT_SIZES);
	ret = sgxfive_command(sgx, SGX_CMD_GETMISCINFO, data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	start = ktime_get();
	ret = sgxfive_wait_mem(&mi[0], MISCINFO_READY, MISCINFO_READY);
	if (ret) {
		drm_printf(p, "no MISCINFO_READY after 500 ms\n");
		sgxfive_dump(sgx, p);
		return ret;
	}
	drm_printf(p, "GETMISCINFO answered after %lld us\n",
		   ktime_us_delta(ktime_get(), start));

	want[0] = sgx_rd(sgx, SGX_CORE_REVISION);
	want[1] = sgx_rd(sgx, SGX_CORE_ID);
	/* PVRVERSION_MAJ/MIN/BRANCH: DDK 1.7 is branch 17 */
	want[2] = sgx->fw.ddk_major << 16 | sgx->fw.ddk_minor << 8 | 17;
	want[3] = sgx->fw.ddk_build;
	want[4] = 545;
	want[5] = want[0];		/* the ukernel's own revision: 1.0.13 */
	want[6] = sgx->fw.build_options;
	for (i = 0; i < 7; i++)
		sgxfive_check(p, sgxfive_feature_names[i], mi[1 + i], want[i],
			      &bad);
	for (i = 0; i < 13; i++) {
		char what[16];

		snprintf(what, sizeof(what), "struct size %d", i);
		sgxfive_check(p, what, mi[8 + i],
			      sgx->fw.meta[SGX_FW_META_STRUCT_SIZES + i], &bad);
	}
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "all values match");
	sgx->core_id_sw = mi[5];
	sgx->core_revision_sw = mi[6];
	sgx->ddk_version = mi[3];
	sgx->sw_valid = true;
	return bad ? -EIO : 0;
}

static int sgxfive_miscinfo_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_miscinfo(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_miscinfo);

/* Queue one dummy transfer, optionally behind a destination sync. */
static int sgxfive_xfer_submit(struct sgxfive *sgx,
			       const struct sgx_xfer_sync *dst, u32 *fence,
			       u32 *update, u32 *done)
{
	u8 *ccb = sgx->xbuf[XB_CCB].va, *ctx = sgx->xbuf[XB_CTX].va;
	u32 *ctl = sgx->xbuf[XB_CCB_CTL].va, *scr = sgx->xbuf[XB_SCRATCH].va;
	const u32 scr_addr = sgx->xbuf[XB_SCRATCH].addr;
	const u32 data[] = { 0, sgx->xbuf[XB_CTX].addr };
	u32 size;

	if (!sgx->live)
		return -ENODEV;
	if (!sgx->xfer_runs)
		sgx_xfer_ctx_init(ctx, sgx->layout.pd_phys,
				  sgx->xbuf[XB_CCB].addr, sgx->xbuf[XB_CCB_CTL].addr);
	*fence = 0x1234 + sgx->xfer_runs;
	*update = 0xcafe0000 + sgx->xfer_runs;
	WRITE_ONCE(scr[SCR_U], 0);
	WRITE_ONCE(scr[SCR_F], 0);
	size = sgx_xfer_dummy(ccb + sgx->xfer_woff, scr_addr + 4 * SCR_F, *fence,
			      scr_addr + 4 * SCR_U, *update, dst);
	*done = (sgx->xfer_woff + size) & (SGX_XFER_CCB_SIZE - 1);
	WRITE_ONCE(ctl[0], *done);		/* host write offset */
	sgx->xfer_woff = *done;
	sgx->xfer_runs++;
	sgx->cache_control |= SGX_CC_INVAL_DATA;
	return sgxfive_command(sgx, SGX_CMD_TRANSFER, data, ARRAY_SIZE(data));
}

/*
 * Sleep until the EDM's next SW_EVENT interrupt (500 ms at most), then check
 * the completion word once, with no polling: the firmware writes its fences
 * and sync counters before it raises the event (transfer §3.1 step 8).
 */
static int sgxfive_wait_event(struct sgxfive *sgx, struct drm_printer *p,
			      int sw0, const u32 *w, u32 want)
{
	ktime_t start = ktime_get();
	long left;

	left = wait_event_timeout(sgx->event_wq,
				  atomic_read(&sgx->sw_events) != sw0,
				  msecs_to_jiffies(500));
	rmb(); /* fresh reads of what the EDM wrote before the event */
	if (!left) {
		drm_printf(p, "no SW_EVENT after 500 ms\n");
		return -ETIMEDOUT;
	}
	drm_printf(p, "SW_EVENT after %lld us, completion word %s\n",
		   ktime_us_delta(ktime_get(), start),
		   READ_ONCE(*w) == want ? "already written" : "NOT YET WRITTEN");
	return READ_ONCE(*w) == want ? 0 : -EIO;
}

/* After a completed dummy: its writes, the CCB, the count and one SW_EVENT. */
static void sgxfive_xfer_verify(struct sgxfive *sgx, struct drm_printer *p,
				u32 fence, u32 update, u32 done, int sw0,
				int *bad)
{
	u32 *ctl = sgx->xbuf[XB_CCB_CTL].va, *scr = sgx->xbuf[XB_SCRATCH].va;
	u32 *ctx = sgx->xbuf[XB_CTX].va;

	drm_printf(p, "dummy transfer %u: ", sgx->xfer_runs - 1);
	if (sgxfive_wait_event(sgx, p, sw0, &scr[SCR_F], fence))
		(*bad)++;
	sgxfive_check(p, "status F", READ_ONCE(scr[SCR_F]), fence, bad);
	sgxfive_check(p, "update U", READ_ONCE(scr[SCR_U]), update, bad);
	sgxfive_check(p, "ccb read off", READ_ONCE(ctl[1]), done, bad);
	sgxfive_check(p, "ctx count", READ_ONCE(ctx[SGX_XFER_CTX_COUNT / 4]), 0,
		      bad);
	sgxfive_check(p, "sw events", atomic_read(&sgx->sw_events) - sw0, 1, bad);
}

/*
 * A dummy transfer (design/sgx545_transfer.md §4.1): no render, but the EDM
 * must queue the context, apply one memory update (U), write one status word
 * (F), advance the client CCB, drop the context's count and raise SW_EVENT.
 */
static int sgxfive_xfer_dummy(struct sgxfive *sgx, struct drm_printer *p)
{
	int sw0 = atomic_read(&sgx->sw_events), bad = 0, ret;
	u32 fence, update, done;

	ret = sgxfive_xfer_submit(sgx, NULL, &fence, &update, &done);
	if (ret)
		return ret;
	sgxfive_xfer_verify(sgx, p, fence, update, done, sw0, &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "dummy transfer complete");
	return bad ? -EIO : 0;
}

/*
 * The readiness test: a dummy behind a destination sync whose write counter is
 * one short must stay queued; once the host completes it and kicks
 * PROCESS_QUEUES it runs, and the EDM moves the write counter past its pending
 * value.
 */
static int sgxfive_xfer_sync(struct sgxfive *sgx, struct drm_printer *p)
{
	u32 *scr = sgx->xbuf[XB_SCRATCH].va, *ctl = sgx->xbuf[XB_CCB_CTL].va;
	u32 *ctx = sgx->xbuf[XB_CTX].va;
	const u32 scr_addr = sgx->xbuf[XB_SCRATCH].addr;
	const struct sgx_xfer_sync dst = {
		.read_pending = 0, .read_complete = scr_addr + 4 * SCR_SYNC_READ,
		.write_pending = 6, .write_complete = scr_addr + 4 * SCR_SYNC_WRITE,
	};
	u32 fence, update, done, before = READ_ONCE(ctl[1]);
	int sw0 = atomic_read(&sgx->sw_events), bad = 0, ret;

	WRITE_ONCE(scr[SCR_SYNC_READ], 0);
	WRITE_ONCE(scr[SCR_SYNC_WRITE], 5);	/* one write op outstanding */
	ret = sgxfive_xfer_submit(sgx, &dst, &fence, &update, &done);
	if (ret)
		return ret;
	msleep(50);
	rmb(); /* fresh reads of what the EDM wrote */
	sgxfive_check(p, "held: status F", READ_ONCE(scr[SCR_F]), 0, &bad);
	sgxfive_check(p, "held: read off", READ_ONCE(ctl[1]), before, &bad);
	sgxfive_check(p, "held: count", READ_ONCE(ctx[SGX_XFER_CTX_COUNT / 4]), 1,
		      &bad);

	WRITE_ONCE(scr[SCR_SYNC_WRITE], 6);	/* the write op completes */
	ret = sgxfive_command(sgx, SGX_CMD_PROCESS_QUEUES, NULL, 0);
	if (ret)
		return ret;
	sgxfive_xfer_verify(sgx, p, fence, update, done, sw0, &bad);
	/* A destination's read counter is left alone; only sources get it. */
	sgxfive_check(p, "sync read cmpl", READ_ONCE(scr[SCR_SYNC_READ]), 0, &bad);
	sgxfive_check(p, "sync write cmpl", READ_ONCE(scr[SCR_SYNC_WRITE]), 7, &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "sync wait and release correct");
	return bad ? -EIO : 0;
}

static int sgxfive_xfer_sync_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_xfer_sync(sgx, &p);
	if (ret)
		sgxfive_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_sync);

static int sgxfive_xfer_dummy_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_xfer_dummy(sgx, &p);
	if (ret)
		sgxfive_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_dummy);

/*
 * Where the transfer programs go (design/sgx545_transfer.md §4.2.7, §4.4.5):
 * PDS code at the PDS heap base, USE code at the pixel-shader code base, each
 * inside the one page mapped there. A fill and a copy use different PDS
 * primaries of different sizes, so each operation has its own placement and
 * only places its own templates; one transfer is in flight at a time.
 */
struct sgxfive_tq_slot {
	u8 area;		/* XB_PDS or XB_USE; 0 = not used */
	u16 off;
};

static const struct sgxfive_tq_slot sgxfive_fill_place[SGX_FW_TQ_MAX] = {
	[SGX_FW_TQ_PDS_PRIM] = { XB_PDS, 0x00 },
	[SGX_FW_TQ_PDS_SEC] = { XB_PDS, 0x20 },
	[SGX_FW_TQ_PDS_PIXEVENT] = { XB_PDS, 0x40 },
	[SGX_FW_TQ_USE_FILL] = { XB_USE, 0x00 },
	[SGX_FW_TQ_USE_SEC] = { XB_USE, 0x10 },
	[SGX_FW_TQ_USE_EOT] = { XB_USE, 0x20 },
	[SGX_FW_TQ_USE_EOR] = { XB_USE, 0x60 },
};

static const struct sgxfive_tq_slot sgxfive_copy_place[SGX_FW_TQ_MAX] = {
	[SGX_FW_TQ_PDS_PRIM_1SRC] = { XB_PDS, 0x00 },
	[SGX_FW_TQ_PDS_SEC] = { XB_PDS, 0x40 },
	[SGX_FW_TQ_PDS_PIXEVENT] = { XB_PDS, 0x60 },
	[SGX_FW_TQ_USE_COPY] = { XB_USE, 0x00 },
	[SGX_FW_TQ_USE_SEC] = { XB_USE, 0x10 },
	[SGX_FW_TQ_USE_EOT] = { XB_USE, 0x20 },
	[SGX_FW_TQ_USE_EOR] = { XB_USE, 0x60 },
};

/* Place one operation's templates; progs[] gets their CPU addresses. */
static int sgxfive_tq_place(struct sgxfive *sgx,
			    const struct sgxfive_tq_slot *place, u32 set,
			    u8 *progs[SGX_FW_TQ_MAX])
{
	struct sgx_fw_layout *l = &sgx->layout;
	unsigned int i;

	l->tq_set = 0;
	for (i = 0; i < SGX_FW_TQ_MAX; i++) {
		progs[i] = NULL;
		if (!(set & 1u << i) || !sgx_fw_tq(&sgx->fw, i) || !place[i].area)
			continue;
		l->tq_addr[i] = sgx->xbuf[place[i].area].addr + place[i].off;
		l->tq_set |= 1u << i;
		progs[i] = (u8 *)sgx->xbuf[place[i].area].va + place[i].off;
	}
	return sgx_fw_tq_need(&sgx->fw, l, set);
}

#define FILL_SENTINEL	0xdeadbeef

/*
 * Fill cases: a rectangle of a surface at the start of the destination area,
 * whose stride may be padded. Every dword of the three destination pages is
 * checked: the colour inside the rectangle, the sentinel everywhere else.
 */
static const struct sgxfive_fill_case {
	const char *name;
	u32 x, y, w, h, stride, colour;
} sgxfive_fill_cases[] = {
	{ "whole 64x32", 0, 0, 64, 32, 256, 0xff336699 },
	{ "32x16 at (8,4), stride 320", 8, 4, 32, 16, 320, 0xff00ff00 },
	{ "33x16 at (20,10), across tiles", 20, 10, 33, 16, 256, 0x80112233 },
	{ "one pixel at (63,31)", 63, 31, 1, 1, 256, 0x12345678 },
};

/*
 * Where a transfer writes: the destination's CPU and device addresses, the
 * page directory the transfer context names, and optionally one updates
 * target.
 */
struct sgxfive_fill_target {
	u32 *cpu;
	u32 addr;
	u32 pd_phys;
	u32 *upd_cpu;
	u32 upd_addr;
};

#define FILL_UPDATE	0xc0ffee00

/*
 * Queue one transfer whose programs and background object are in place and
 * wait for it: build the command with sgx_xfer_fill_cmd(), check it word for
 * word against the firmware file's TQ_CMD list, kick, sleep until SW_EVENT,
 * then check the fence, the CCB, the context count and the event count.
 */
static int sgxfive_tq_run(struct sgxfive *sgx, struct drm_printer *p,
			  struct sgx_xfer_fill *f,
			  const u32 params[SGX_FW_TQ_NUM_PARAMS],
			  const struct sgxfive_fill_target *t, int *bad)
{
	struct sgx_fw_layout *l = &sgx->layout;
	u32 *ctl = sgx->xbuf[XB_CCB_CTL].va, *scr = sgx->xbuf[XB_SCRATCH].va;
	u32 *ctx = sgx->xbuf[XB_CTX].va;
	u8 *cmd = (u8 *)sgx->xbuf[XB_CCB].va + sgx->xfer_woff;
	const u32 data[] = { 0, sgx->xbuf[XB_CTX].addr };
	struct sgx_fw_write cw[16];
	u32 size, done;
	int i, n, ret, sw0, wrong = 0;

	f->bg = sgx->xbuf[XB_BG].addr;
	f->req_base = sgx->xbuf[XB_BG].addr & 0xfff00000;
	f->pds_exec_base = sgx->xbuf[XB_PDS].addr;
	f->pixev_exec = l->tq_addr[SGX_FW_TQ_PDS_PIXEVENT];
	f->status_addr = sgx->xbuf[XB_SCRATCH].addr + 4 * SCR_F;
	f->fence_id = sgx->xfer_runs;
	if (!sgx->xfer_runs)
		sgx_xfer_ctx_init((u8 *)ctx, l->pd_phys, sgx->xbuf[XB_CCB].addr,
				  sgx->xbuf[XB_CCB_CTL].addr);
	/* The context is idle between transfers, so its PD can change. */
	WRITE_ONCE(ctx[SGX_XFER_CTX_PD / 4], t->pd_phys);
	WRITE_ONCE(scr[SCR_F], 0);
	if (t->upd_cpu)
		WRITE_ONCE(*t->upd_cpu, 0);

	size = sgx_xfer_fill_cmd(cmd, f);
	if (t->upd_cpu) {	/* applied by the EDM through the context's PD */
		u32 *u = (u32 *)(cmd + SGX_XFER_NUM_UPDATES);

		u[0] = 1;
		u[1] = t->upd_addr;
		u[2] = FILL_UPDATE + sgx->xfer_runs;
	}
	n = sgx_fw_tq_cmd(&sgx->fw, l, params, cw, ARRAY_SIZE(cw));
	if (n < 0)
		return n;
	for (i = 0; i < n; i++) {
		u32 *w = (u32 *)(cmd + cw[i].reg);

		if (*w != cw[i].val) {
			drm_printf(p, "command word %#x: built %#x, TQ_CMD %#x\n",
				   cw[i].reg, *w, cw[i].val);
			wrong++;
		}
	}
	if (wrong)
		return -EINVAL;
	drm_printf(p, "command matches the %d TQ_CMD words\n", n);

	done = (sgx->xfer_woff + size) & (SGX_XFER_CCB_SIZE - 1);
	WRITE_ONCE(ctl[0], done);
	sgx->xfer_woff = done;
	sgx->xfer_runs++;
	sw0 = atomic_read(&sgx->sw_events);
	/* New pages were mapped: the BIF must refetch PD and PT entries. */
	sgx->cache_control |= SGX_CC_INVAL_BIF | SGX_CC_INVAL_DATA;
	wmb(); /* programs, object, command and sentinels before the kick */
	ret = sgxfive_command(sgx, SGX_CMD_TRANSFER, data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	if (sgxfive_wait_event(sgx, p, sw0, &scr[SCR_F], f->fence_id + 1))
		(*bad)++;

	if (t->upd_cpu)
		sgxfive_check(p, "update word", READ_ONCE(*t->upd_cpu),
			      FILL_UPDATE + f->fence_id, bad);
	sgxfive_check(p, "status F", READ_ONCE(scr[SCR_F]), f->fence_id + 1, bad);
	sgxfive_check(p, "ccb read off", READ_ONCE(ctl[1]), done, bad);
	sgxfive_check(p, "ctx count", READ_ONCE(ctx[SGX_XFER_CTX_COUNT / 4]), 0,
		      bad);
	sgxfive_check(p, "sw events", atomic_read(&sgx->sw_events) - sw0, 1, bad);
	sgxfive_check(p, "bif faults", atomic_read(&sgx->bif_faults), 0, bad);
	return 0;
}

/*
 * Check every dword of the destination area (three pages): want(x, y) inside
 * the rectangle, the sentinel outside it.
 */
static void sgxfive_check_dst(struct drm_printer *p, const u32 *dst, u32 stride,
			      const struct sgx_xfer_rect *r, const u32 *src,
			      u32 src_stride, u32 colour, int *bad)
{
	unsigned int i, wrong = 0, first_bad = ~0u;

	for (i = 0; i < 3 * PAGE_SIZE / 4; i++) {
		u32 x = i % (stride / 4), y = i / (stride / 4), want = FILL_SENTINEL;

		if (x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1)
			want = src ? src[(y - r->y0) * (src_stride / 4) + x - r->x0] :
				     colour;
		if (READ_ONCE(dst[i]) != want) {
			wrong++;
			if (first_bad == ~0u)
				first_bad = i;
		}
	}
	if (first_bad != ~0u)
		drm_printf(p, "first wrong dword %u (x %u, y %u) = %#x\n", first_bad,
			   first_bad % (stride / 4), first_bad / (stride / 4),
			   READ_ONCE(dst[first_bad]));
	sgxfive_check(p, "wrong dwords", wrong, 0, bad);
}

/* A solid fill (§4.2) of one case. */
static int sgxfive_xfer_fill(struct sgxfive *sgx, struct drm_printer *p,
			     const struct sgxfive_fill_case *fc,
			     const struct sgxfive_fill_target *t)
{
	struct sgx_fw_layout *l = &sgx->layout;
	const struct sgx_fw_tq_fill tf = {
		.dest = t->addr, .width = fc->w, .height = fc->h,
		.stride = fc->stride, .colour = fc->colour, .x = fc->x, .y = fc->y,
	};
	const struct sgx_fw_tq *pri = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_PRIM);
	const struct sgx_fw_tq *sec = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_SEC);
	struct sgx_xfer_fill f = {
		.rect = { fc->x, fc->y, fc->x + fc->w, fc->y + fc->h },
	};
	u32 params[SGX_FW_TQ_NUM_PARAMS];
	u8 *progs[SGX_FW_TQ_MAX];
	unsigned int i;
	int ret, bad = 0;

	if (!sgx->live)
		return -ENODEV;
	if (!pri || !sec) {
		drm_printf(p, "the firmware file has no fill templates (v1)\n");
		return -ENOENT;
	}
	ret = sgxfive_tq_place(sgx, sgxfive_fill_place, SGX_FW_TQ_FILL_SET, progs);
	if (!ret)
		ret = sgx_fw_tq_fill_params(&tf, params);
	if (!ret)
		ret = sgx_fw_tq_build(&sgx->fw, l, params, progs);
	if (ret) {
		drm_printf(p, "building the fill programs failed: %d\n", ret);
		return ret;
	}
	sgx_xfer_bgobj(sgx->xbuf[XB_BG].va, sgx->xbuf[XB_PDS].addr,
		       l->tq_addr[SGX_FW_TQ_PDS_PRIM], pri->pds_data,
		       l->tq_addr[SGX_FW_TQ_PDS_SEC], sec->pds_data, &f.rect);
	for (i = 0; i < 3 * PAGE_SIZE / 4; i++)
		WRITE_ONCE(t->cpu[i], FILL_SENTINEL);

	drm_printf(p, "fill %s: ", fc->name);
	ret = sgxfive_tq_run(sgx, p, &f, params, t, &bad);
	if (ret)
		return ret;
	rmb(); /* fresh reads of what the GPU wrote */
	sgxfive_check_dst(p, t->cpu, fc->stride, &f.rect, NULL, 0, fc->colour,
			  &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "solid fill correct");
	return bad ? -EIO : 0;
}

/*
 * Copy cases (§4.4): a width x height rectangle whose first texel is at
 * (sx, sy) of the source area, to (x, y) of the destination area. Strides may
 * be padded.
 */
static const struct sgxfive_copy_case {
	const char *name;
	u32 sx, sy, src_stride, x, y, w, h, dst_stride;
} sgxfive_copy_cases[] = {
	{ "whole 64x32", 0, 0, 256, 0, 0, 64, 32, 256 },
	{ "32x16 from (5,3) stride 320 to (8,4) stride 288",
	  5, 3, 320, 8, 4, 32, 16, 288 },
	{ "33x16 across tiles", 0, 0, 256, 20, 10, 33, 16, 256 },
};

#define COPY_SRC_PAGES	2

/* A copy (§4.4) of one case, from a patterned source area. */
static int sgxfive_xfer_copy(struct sgxfive *sgx, struct drm_printer *p,
			     const struct sgxfive_copy_case *cc)
{
	struct sgx_fw_layout *l = &sgx->layout;
	const struct sgxfive_fill_target t = {
		.cpu = sgx->xbuf[XB_DST].va, .addr = sgx->xbuf[XB_DST].addr,
		.pd_phys = l->pd_phys,
	};
	u32 *src = sgx->xbuf[XB_SRC].va;
	u32 src_first = cc->sy * (cc->src_stride / 4) + cc->sx;
	const struct sgx_fw_tq_copy tc = {
		.src = sgx->xbuf[XB_SRC].addr + 4 * src_first,
		.dst = t.addr, .width = cc->w, .height = cc->h,
		.src_stride = cc->src_stride, .dst_stride = cc->dst_stride,
		.x = cc->x, .y = cc->y,
	};
	const struct sgx_fw_tq *pri = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_PRIM_1SRC);
	const struct sgx_fw_tq *sec = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_SEC);
	const struct sgx_xfer_rect whole = { 0, 0, cc->w, cc->h };
	struct sgx_xfer_fill f = {
		.rect = { cc->x, cc->y, cc->x + cc->w, cc->y + cc->h },
		.layers = 1,
	};
	u32 params[SGX_FW_TQ_NUM_PARAMS];
	u8 *progs[SGX_FW_TQ_MAX];
	unsigned int i;
	int ret, bad = 0;

	if (!sgx->live)
		return -ENODEV;
	if (!pri || !sec) {
		drm_printf(p, "the firmware file has no copy templates (v3)\n");
		return -ENOENT;
	}
	ret = sgxfive_tq_place(sgx, sgxfive_copy_place, SGX_FW_TQ_COPY_SET, progs);
	if (!ret)
		ret = sgx_fw_tq_copy_params(&tc, params);
	if (!ret)
		ret = sgx_fw_tq_build(&sgx->fw, l, params, progs);
	if (ret) {
		drm_printf(p, "building the copy programs failed: %d\n", ret);
		return ret;
	}
	/* The texture is exactly the copied rectangle: coordinates 0..1. */
	sgx_xfer_bgobj_copy(sgx->xbuf[XB_BG].va, sgx->xbuf[XB_PDS].addr,
			    l->tq_addr[SGX_FW_TQ_PDS_PRIM_1SRC], pri->pds_data,
			    l->tq_addr[SGX_FW_TQ_PDS_SEC], sec->pds_data, &f.rect,
			    &whole, cc->w, cc->h);
	/* Every byte varies: a swizzle or a skew shows. */
	for (i = 0; i < COPY_SRC_PAGES * PAGE_SIZE / 4; i++) {
		u32 x = i % (cc->src_stride / 4), y = i / (cc->src_stride / 4);

		WRITE_ONCE(src[i], 0xa5000000 | (y & 0xff) << 16 |
				   (x & 0xff) << 8 | ((x ^ y) & 0xff));
	}
	for (i = 0; i < 3 * PAGE_SIZE / 4; i++)
		WRITE_ONCE(t.cpu[i], FILL_SENTINEL);

	drm_printf(p, "copy %s: ", cc->name);
	ret = sgxfive_tq_run(sgx, p, &f, params, &t, &bad);
	if (ret)
		return ret;
	rmb(); /* fresh reads of what the GPU wrote */
	sgxfive_check_dst(p, t.cpu, cc->dst_stride, &f.rect, src + src_first,
			  cc->src_stride, 0, &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "copy correct");
	return bad ? -EIO : 0;
}

static int sgxfive_xfer_copy_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	unsigned int i;
	int ret = 0, r;

	mutex_lock(&sgx->lock);
	for (i = 0; i < ARRAY_SIZE(sgxfive_copy_cases); i++) {
		r = sgxfive_xfer_copy(sgx, &p, &sgxfive_copy_cases[i]);
		if (r && !ret)
			ret = r;
		if (r && r != -EIO)
			break;		/* the queue itself failed */
	}
	if (ret)
		sgxfive_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_copy);

static int sgxfive_xfer_fill_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	const struct sgxfive_fill_target t = {
		.cpu = sgx->xbuf[XB_DST].va, .addr = sgx->xbuf[XB_DST].addr,
		.pd_phys = sgx->layout.pd_phys,
	};
	unsigned int i;
	int ret = 0, r;

	mutex_lock(&sgx->lock);
	for (i = 0; i < ARRAY_SIZE(sgxfive_fill_cases); i++) {
		r = sgxfive_xfer_fill(sgx, &p, &sgxfive_fill_cases[i], &t);
		if (r && !ret)
			ret = r;
		if (r && r != -EIO)
			break;		/* the queue itself failed */
	}
	if (ret)
		sgxfive_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_fill);

/*
 * The isolation test: does a transfer's 3D work use the page directory its
 * context names? A client PD maps a destination at CLIENT_DST and an update
 * target at CLIENT_UPD, neither of which exists in the kernel PD. The fill
 * must land, and the update must be applied, through the client PD alone.
 *
 * Like the DDK's shared heaps, the client PD also maps every EDM buffer at its
 * kernel address: to apply updates the EDM points its own requester at the
 * context's directory list and keeps fetching its code and data through it.
 * Without the aliases it faults on its own code (seen on hardware: BIF_FAULT
 * context 2, page 0xe8009000).
 */
#define CLIENT_DST	0x20000000
#define CLIENT_UPD	0x20010000

static int sgxfive_client_pd(struct sgxfive *sgx)
{
	unsigned int i;
	int ret;

	/* Rebuilt each time, so a change of alias_ro takes effect. */
	for (i = 0; i < ARRAY_SIZE(sgx->cbuf); i++)
		sgxfive_unmap(sgx, &sgx->cbuf[i]);
	if (sgx->cpd)
		sgx_mmu_pd_destroy(sgx->cpd);
	sgx->cpd = sgx_mmu_pd_create();
	if (!sgx->cpd)
		return -ENOMEM;
	ret = sgxfive_map_pd(sgx, sgx->cpd, &sgx->cbuf[0], CLIENT_DST, 0x3000);
	if (!ret)
		ret = sgxfive_map_pd(sgx, sgx->cpd, &sgx->cbuf[1], CLIENT_UPD, 4);
	for (i = 0; i < SGX_FW_MAX_BUFFERS + XB_COUNT && !ret; i++) {
		struct sgxfive_buf *b = i < SGX_FW_MAX_BUFFERS ? &sgx->buf[i] :
					&sgx->xbuf[i - SGX_FW_MAX_BUFFERS];
		unsigned int x = i - SGX_FW_MAX_BUFFERS;
		/* What the job itself uses: its programs and background object. */
		bool job = i >= SGX_FW_MAX_BUFFERS &&
			   (x == XB_PDS || x == XB_USE || x == XB_BG);

		if (b->pd && (job || !sgx->no_alias))
			ret = sgx_mmu_map(sgx->cpd, b->addr, b->pages,
					  b->npages,
					  sgx->alias_ro ? SGX_MMU_RO : 0);
	}
	return ret;
}

static int sgxfive_xfer_iso_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	struct sgxfive_fill_target t;
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgx->live ? sgxfive_client_pd(sgx) : -ENODEV;
	if (!ret) {
		t = (struct sgxfive_fill_target) {
			.cpu = sgx->cbuf[0].va, .addr = CLIENT_DST,
			.pd_phys = sgx_mmu_pd_phys(sgx->cpd),
		};
		/* Updates run the EDM through the client PD: only with aliases. */
		if (!sgx->no_alias) {
			t.upd_cpu = sgx->cbuf[1].va;
			t.upd_addr = CLIENT_UPD;
		}
		drm_printf(&p, "client PD at %#x, EDM aliases %s\n", t.pd_phys,
			   sgx->no_alias ? "none, no updates" :
			   sgx->alias_ro ? "read-only" : "read-write");
		ret = sgxfive_xfer_fill(sgx, &p, &sgxfive_fill_cases[1], &t);
		drm_printf(&p, "%s\n", ret ? "ISOLATION TEST FAILED" :
			   sgx->no_alias ? "3D work used the client PD alone" :
			   "3D work and updates used the client PD");
	}
	if (ret)
		sgxfive_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_iso);

/*
 * The negative test: a client fill aimed at a kernel buffer that the client
 * PD only aliases read-only (the fill destination area, at its kernel
 * address). The PBE writes must fault and the buffer must keep its sentinel.
 * The faulting render wedges the EDM: write the reset node to recover.
 */
static int sgxfive_xfer_ro_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	struct sgxfive_fill_target t;
	unsigned int i, written = 0;
	u32 *dst;
	int ret;

	mutex_lock(&sgx->lock);
	sgx->alias_ro = true;
	ret = sgx->live ? sgxfive_client_pd(sgx) : -ENODEV;
	if (!ret) {
		dst = sgx->xbuf[XB_DST].va;
		t = (struct sgxfive_fill_target) {
			.cpu = dst, .addr = sgx->xbuf[XB_DST].addr,
			.pd_phys = sgx_mmu_pd_phys(sgx->cpd),
		};
		sgxfive_xfer_fill(sgx, &p, &sgxfive_fill_cases[0], &t);
		rmb(); /* what, if anything, reached memory */
		for (i = 0; i < 3 * PAGE_SIZE / 4; i++)
			written += READ_ONCE(dst[i]) != FILL_SENTINEL;
		drm_printf(&p, "dwords written through the read-only alias: %u\n",
			   written);
		drm_printf(&p, "BIF_INT_STAT %#x BIF_FAULT %#x\n",
			   sgx_rd(sgx, SGX_BIF_INT_STAT), sgx_rd(sgx, SGX_BIF_FAULT));
		drm_printf(&p, "%s\n", written ? "NOT PROTECTED" :
			   "kernel buffer protected; reset to recover");
	}
	mutex_unlock(&sgx->lock);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_xfer_ro);

/*
 * The first triangle (design/sgx545_3d.md, "First triangle"): the areas of
 * sgx_tri.h, mapped in the EDM's page directory at REV1 heap addresses when
 * the test first runs. The parameter heap holds the PB pages, their page
 * table, the scratch primitive block and the background object (its tag is
 * relative to BIF_3D_REQ_BASE, the heap base); TADATA the RT data's zeroed
 * buffers; the PDS and USSE heaps our programs, next to the transfer area's
 * first pages; GENERAL the VDM streams, the geometry and the colour buffer,
 * with a guard page after it. With dri/N/sgx/tri_own_pd the render context
 * gets a page directory of its own instead (sgxfive_tri_pd).
 */
enum {
	TB_PB, TB_PT, TB_PRIM, TB_BG, TB_TADATA, TB_CCB, TB_KD, TB_PDSV, TB_USEV,
	TB_PDSP, TB_USEP, TB_GEN, TB_FB, TB_COUNT
};

#define TRI_PARAM_BASE	0x10000000	/* 3D parameters heap (REV1) */

static const struct {
	u32 addr, size;
} sgxfive_tbuf[TB_COUNT] = {
	[TB_PB] = { TRI_PARAM_BASE + SGX_RENDER_PARAM_PB, SGX_RENDER_PB_PAGES * SZ_4K },
	[TB_PT] = { TRI_PARAM_BASE + SGX_RENDER_PARAM_PT, SGX_RENDER_PT_BYTES },
	[TB_PRIM] = { TRI_PARAM_BASE + SGX_RENDER_PARAM_PRIM, SZ_4K },
	[TB_BG] = { TRI_PARAM_BASE + SGX_RENDER_PARAM_BG, SZ_4K },
	[TB_TADATA] = { 0x20001000, SGX_TRI_TD_SIZE },	/* TA data, for 64 x 64 */
	[TB_CCB] = { 0x32140000, SGX_RENDER_CCB_SIZE + SGX_RENDER_CMD_STRIDE },
	[TB_KD] = { 0x32152000, SGX_TRI_KD_SIZE },		/* kernel data */
	[TB_PDSV] = { 0x30401000, SZ_4K },			/* PDS vertex */
	[TB_USEV] = { 0x3c001000, SZ_4K },			/* vertex shader */
	[TB_PDSP] = { 0x2e001000, SZ_4K },			/* PDS pixel */
	[TB_USEP] = { 0x37001000, SZ_4K },			/* pixel shader */
	[TB_GEN] = { 0x3e001000, SZ_4K },			/* general */
	[TB_FB] = { 0x3e010000, SGX_TRI_FB_SIZE + SZ_4K },
};

static_assert(ARRAY_SIZE(((struct sgxfive *)NULL)->tbuf) >= TB_COUNT);

#define TRI_COLOUR	0xff00ff00	/* the triangle: opaque green */
#define TRI_BG		0xff203040	/* the background object */
#define TRI_SENTINEL	0xdeadbeef

static int sgxfive_tri_map(struct sgxfive *sgx)
{
	unsigned int i;
	int ret;

	if (sgx->tbuf[TB_COUNT - 1].pages)
		return 0;
	for (i = 0; i < TB_COUNT; i++) {
		ret = sgxfive_map(sgx, &sgx->tbuf[i], sgxfive_tbuf[i].addr,
				  sgxfive_tbuf[i].size);
		if (ret)
			goto err;
	}
	sgx->tbuf[TB_FB].blob.size = SGX_TRI_FB_SIZE;	/* debugfs tri_fb */
	return 0;
err:
	for (i = 0; i < TB_COUNT; i++)
		sgxfive_unmap(sgx, &sgx->tbuf[i]);
	return ret;
}

/*
 * A page directory of the triangle's own: its areas, plus aliases of every
 * buffer of the EDM's directory (the EDM fetches its own code and data
 * through the context's directory while it builds the page table and patches
 * the background object, §12.5).
 */
static int sgxfive_tri_pd(struct sgxfive *sgx)
{
	unsigned int i;
	int ret = 0;

	if (sgx->tpd)
		return 0;
	sgx->tpd = sgx_mmu_pd_create();
	if (!sgx->tpd)
		return -ENOMEM;
	for (i = 0; i < SGX_FW_MAX_BUFFERS + XB_COUNT + TB_COUNT && !ret; i++) {
		struct sgxfive_buf *b =
			i < SGX_FW_MAX_BUFFERS ? &sgx->buf[i] :
			i < SGX_FW_MAX_BUFFERS + XB_COUNT ?
			&sgx->xbuf[i - SGX_FW_MAX_BUFFERS] :
			&sgx->tbuf[i - SGX_FW_MAX_BUFFERS - XB_COUNT];

		if (b->pd)
			ret = sgx_mmu_map(sgx->tpd, b->addr, b->pages, b->npages, 0);
	}
	return ret;
}

static void sgxfive_tri_addrs(struct sgxfive *sgx, struct sgx_tri_addrs *a,
			      struct sgx_render_addrs *r)
{
	const struct sgxfive_buf *t = sgx->tbuf;
	const u32 kd = t[TB_KD].addr, w = kd + SGX_TRI_KD_WORDS;

	*r = (struct sgx_render_addrs) {
		.pd_phys = sgx->tri_own_pd && sgx->tpd ? sgx_mmu_pd_phys(sgx->tpd) :
			   sgx->layout.pd_phys,
		.param_base = TRI_PARAM_BASE, .tadata = t[TB_TADATA].addr,
		.ctx = kd + SGX_TRI_KD_CTX, .ccb = t[TB_CCB].addr,
		.ccb_ctl = kd + SGX_TRI_KD_CCB_CTL, .rtds = kd + SGX_TRI_KD_RTDS,
		.pbdesc = kd + SGX_TRI_KD_PBDESC, .pbblock = kd + SGX_TRI_KD_PBBLOCK,
		.rd = kd + SGX_TRI_KD_RD,
		.ta_status = w + 4 * SGX_TRI_W_TA, .td_status = w + 4 * SGX_TRI_W_3D,
		.access = w + 4 * SGX_TRI_W_ACCESS, .pending = w + 4 * SGX_TRI_W_PENDING,
		.rt_status = w + 4 * SGX_TRI_W_RTSTATUS, .bgpri = w + 4 * SGX_TRI_W_BGPRI,
	};
	/* TA data for the test's own 64 x 64 target: SGX_TRI_TD_SIZE (KUnit). */
	sgx_render_tadata_layout(&r->td, SGX_TRI_W, SGX_TRI_H);
	*a = (struct sgx_tri_addrs) {
		.pdsv = t[TB_PDSV].addr, .usev = t[TB_USEV].addr,
		.pdsp = t[TB_PDSP].addr, .usep = t[TB_USEP].addr,
		.gen = t[TB_GEN].addr, .fb = t[TB_FB].addr,
		/* What the init script programs (and transfers keep). */
		.pds_exec_base = sgx_heap_base[HEAP_PDSPIXEL],
		.usep_base = sgx_heap_base[HEAP_PIXELSHADER],
		.usev_base = sgx_heap_base[HEAP_VERTEXSHADER],
	};
}

/*
 * Build the whole job: the scene (what a client would place), its register
 * words, then the kernel's side from them with the render node's builders
 * (sgx_render.c), sentinels and the commands.
 */
static unsigned int sgxfive_tri_build(struct sgxfive *sgx, bool two_kicks,
				      u32 ta_val, u32 td_val)
{
	struct sgxfive_buf *t = sgx->tbuf;
	struct sgx_tri_addrs a;
	struct sgx_render_addrs r;
	struct drm_sgxfive_render_cmd rc;
	const struct sgx_tri_cpu c = {
		.pdsv = t[TB_PDSV].va, .usev = t[TB_USEV].va, .pdsp = t[TB_PDSP].va,
		.usep = t[TB_USEP].va, .gen = t[TB_GEN].va,
	};
	u8 *kd = t[TB_KD].va, *ccb = t[TB_CCB].va;
	u32 *fb = t[TB_FB].va;
	struct sgx_render_kick k;
	unsigned int i, woff;

	sgxfive_tri_addrs(sgx, &a, &r);
	for (i = TB_PRIM; i < TB_FB; i++)
		memset(t[i].va, 0, sgxfive_tbuf[i].size);
	sgx_tri_scene(&c, &a, TRI_COLOUR, TRI_BG);
	sgx_tri_render_cmd(&rc, &a, a.gen + SGX_TRI_G_VDM);
	sgx_render_ctx((u32 *)(kd + SGX_TRI_KD_CTX), &r);
	sgx_render_pb((u32 *)(kd + SGX_TRI_KD_PBDESC), (u32 *)(kd + SGX_TRI_KD_PBBLOCK),
		      t[TB_PT].va, &r, READ_ONCE(sgx->render_pb_pages));
	sgx_render_rtds((u32 *)(kd + SGX_TRI_KD_RTDS), &r, rc.mte_screen);
	sgx_render_rd((u32 *)(kd + SGX_TRI_KD_RD), &r, &rc);
	sgx_render_bgobj(t[TB_BG].va, &rc);
	sgx_tri_words((u32 *)(kd + SGX_TRI_KD_WORDS), &a);
	for (i = 0; i < sgxfive_tbuf[TB_FB].size / 4; i++)
		fb[i] = TRI_SENTINEL;

	/*
	 * The single-kick form (§12.7 Q1), or the capture's two kicks: the
	 * geometry without the end-of-scene bit, then a kick that only ends the
	 * scene and asks for the render.
	 */
	sgx_render_scene_kick(&k, sgx->tri_runs, td_val);
	k.ta_val = ta_val;
	if (two_kicks) {
		k.flags = SGX_RENDER_TA_FIRST | SGX_RENDER_TA_INVAL;
		k.render_flags = 0;
		k.te_psg = 0;
		k.ta_val = ta_val - 1;
		k.td_val = td_val - 1;
	}
	woff = sgx_render_cmd(ccb, &r, &rc, &k);
	if (two_kicks) {
		k.flags = SGX_RENDER_TA_LAST | (SGX_RENDER_TA_INVAL & ~0x10);
		k.render_flags = SGX_RENDER_SCENE;
		k.te_psg = SGX_RENDER_TEPSG_END;
		k.ta_val = ta_val;
		k.td_val = td_val;
		rc.vdm_ctrl_stream_base = a.gen + SGX_TRI_G_VDM1;
		woff += sgx_render_cmd(ccb + woff, &r, &rc, &k);
	}
	return woff;
}

/* A few TA, DPM and 3D registers the EDM programs, for a stuck job. */
static const struct {
	u16 reg;
	const char *name;
} sgxfive_tri_regs[] = {
	{ 0x0008, "CLKGATESTATUS" }, { 0x0118, "EVENT_STATUS2" },
	{ 0x012c, "EVENT_STATUS" }, { 0x0210, "TE_SCREEN" }, { 0x0218, "te_psg" },
	{ 0x0220, "TE_TPC_BASE" }, { 0x0238, "VDM_CTRL_STREAM_BASE" },
	{ 0x023c, "MTE_CTRL" }, { 0x0274, "TE_STATE" }, { 0x0408, "ISP_RGN_BASE" },
	{ 0x0604, "DPM_3D_FREE_LIST" }, { 0x0618, "DPM_TA_ALLOC_PAGE_TABLE_BASE" },
	{ 0x061c, "DPM_TA_ALLOC_FREE_LIST" }, { 0x0630, "DPM_STATE_CONTEXT_ID" },
	{ 0x0724, "DPM_PAGE_STATUS" }, { 0x072c, "DPM_GLOBAL_PAGE_STATUS" },
	{ 0x0a74, "DMS_CTRL" }, { 0x0ab8, "PDS_EXEC_BASE" },
	{ 0x0c90, "BIF_TA_REQ_BASE" }, { 0x0cac, "BIF_3D_REQ_BASE" },
	{ 0x0c00, "BIF_CTRL" }, { 0x0c04, "BIF_INT_STAT" }, { 0x0c08, "BIF_FAULT" },
};

static void sgxfive_tri_state(struct sgxfive *sgx, struct drm_printer *p,
			      u32 woff)
{
	const u8 *kd = sgx->tbuf[TB_KD].va;
	const u32 *w = (const u32 *)(kd + SGX_TRI_KD_WORDS);
	const u32 *ctx = (const u32 *)(kd + SGX_TRI_KD_CTX);
	const u32 *ctl = (const u32 *)(kd + SGX_TRI_KD_CCB_CTL);
	const u32 *rt = (const u32 *)(kd + SGX_TRI_KD_RTDS + SGX_RENDER_RTDS_RT);
	const u32 *rtds = (const u32 *)(kd + SGX_TRI_KD_RTDS);
	const u32 *rd = (const u32 *)(kd + SGX_TRI_KD_RD);
	const u32 *pb = (const u32 *)(kd + SGX_TRI_KD_PBDESC);
	const u8 *cmd = sgx->tbuf[TB_CCB].va;
	unsigned int i;

	rmb(); /* what the EDM wrote */
	drm_printf(p, "TA status %#x, 3D status %#x, CCB write %#x read %#x\n",
		   READ_ONCE(w[SGX_TRI_W_TA]), READ_ONCE(w[SGX_TRI_W_3D]), woff,
		   READ_ONCE(ctl[1]));
	drm_printf(p, "ctx flags %#x TACount %u partial %#x complete %#x; cmd READY %u\n",
		   READ_ONCE(ctx[0]), READ_ONCE(ctx[SGX_RENDER_CTX_TACOUNT / 4]),
		   READ_ONCE(ctx[SGX_RENDER_CTX_PARTIAL / 4]),
		   READ_ONCE(ctx[SGX_RENDER_CTX_COMPLETE / 4]),
		   READ_ONCE(*(const u32 *)(cmd + SGX_RENDER_SH_CTRL)) & 1);
	drm_printf(p, "rtdata CommonStatus %#x rd %#x RTStatus %#x; set complete %u oom %u\n",
		   READ_ONCE(rt[SGX_RENDER_RT_COMMON / 4]), READ_ONCE(rt[SGX_RENDER_RT_RD / 4]),
		   READ_ONCE(w[SGX_TRI_W_RTSTATUS]), READ_ONCE(rtds[1]),
		   READ_ONCE(rtds[3]));
	drm_printf(p, "rd flags %#x TEState %#x access %#x; pb alloc %u HT %#x prev %#x\n",
		   READ_ONCE(rd[SGX_RENDER_RD_RFLAGS / 4]),
		   READ_ONCE(rd[SGX_RENDER_RD_TESTATE / 4]), READ_ONCE(w[SGX_TRI_W_ACCESS]),
		   READ_ONCE(pb[SGX_RENDER_PB_ALLOC / 4]), READ_ONCE(pb[SGX_RENDER_PB_HT / 4]),
		   READ_ONCE(pb[SGX_RENDER_PB_PREV / 4]));
	for (i = 0; i < ARRAY_SIZE(sgxfive_tri_regs); i++)
		drm_printf(p, "  %#06x %-28s %#010x\n", sgxfive_tri_regs[i].reg,
			   sgxfive_tri_regs[i].name, sgx_rd(sgx, sgxfive_tri_regs[i].reg));
}

/* Wait up to 2 s for the 3D status word, woken by each SW_EVENT. */
static bool sgxfive_tri_done(const u32 *w, u32 want)
{
	rmb(); /* re-read what the EDM wrote */
	return READ_ONCE(*w) == want;
}

/*
 * Check every pixel against the CPU reference: the triangle's colour inside,
 * the background's outside, either near an edge; and the guard page. Also
 * count the mismatches of the image flipped vertically, which would point at
 * the y convention, and draw the image as text.
 */
static int sgxfive_tri_check(struct sgxfive *sgx, struct drm_printer *p)
{
	const u32 *fb = sgx->tbuf[TB_FB].va;
	unsigned int x, y, i, in = 0, out = 0, edge = 0, flip = 0, guard = 0;
	unsigned int n_tri = 0, n_bg = 0, n_sent = 0, n_other = 0;
	char line[SGX_TRI_W + 1];

	rmb(); /* fresh reads of what the GPU wrote */
	for (y = 0; y < SGX_TRI_H; y++) {
		for (x = 0; x < SGX_TRI_W; x++) {
			u32 v = READ_ONCE(fb[y * SGX_TRI_W + x]);
			int e = sgx_tri_expect(x, y), f = sgx_tri_expect(x, SGX_TRI_H - 1 - y);

			n_tri += v == TRI_COLOUR;
			n_bg += v == TRI_BG;
			n_sent += v == TRI_SENTINEL;
			line[x] = v == TRI_COLOUR ? '#' : v == TRI_BG ? '.' :
				  v == TRI_SENTINEL ? 's' : '?';
			if (e == 1 && v != TRI_COLOUR)
				in++;
			else if (e == 0 && v != TRI_BG)
				out++;
			else if (e == -1 && v != TRI_COLOUR && v != TRI_BG)
				edge++;
			if ((f == 1 && v != TRI_COLOUR) || (f == 0 && v != TRI_BG))
				flip++;
		}
		line[SGX_TRI_W] = 0;
		drm_printf(p, "%s\n", line);
	}
	n_other = SGX_TRI_W * SGX_TRI_H - n_tri - n_bg - n_sent;
	for (i = SGX_TRI_FB_SIZE / 4; i < sgxfive_tbuf[TB_FB].size / 4; i++)
		guard += READ_ONCE(fb[i]) != TRI_SENTINEL;
	drm_printf(p, "pixels: %u triangle, %u background, %u sentinel, %u other; first %#x\n",
		   n_tri, n_bg, n_sent, n_other, READ_ONCE(fb[0]));
	drm_printf(p, "wrong: %u inside, %u outside, %u at edges; %u if flipped in y; guard %u\n",
		   in, out, edge, flip, guard);
	return in || out || edge || guard ? -EIO : 0;
}

/*
 * Kick the triangle and wait for it: one kernel CCB command per SGXMKIF_CMDTA
 * ({0x11d, cache control, 0, the render context}, §12.6), then the TA's and
 * the render's SW_EVENTs, and the 3D status word as the fence.
 */
static int sgxfive_tri(struct sgxfive *sgx, struct drm_printer *p, bool two_kicks)
{
	const u32 ta_val = 0x7a000002 + 2 * sgx->tri_runs;
	const u32 td_val = 0x3d000002 + 2 * sgx->tri_runs;
	u32 data[2], *ctl, *w;
	unsigned int woff, nkicks = two_kicks ? 2 : 1, i;
	int ret, sw0;
	ktime_t start;
	long left;

	if (!sgx->live) {
		ret = sgxfive_boot(sgx);
		if (ret)
			return ret;
	}
	ret = sgxfive_tri_map(sgx);
	if (!ret && sgx->tri_own_pd)
		ret = sgxfive_tri_pd(sgx);
	if (ret)
		return ret;
	/* The kernel CCB command's data: 0, the render context. */
	data[0] = 0;
	data[1] = sgx->tbuf[TB_KD].addr + SGX_TRI_KD_CTX;
	woff = sgxfive_tri_build(sgx, two_kicks, ta_val, td_val);
	ctl = (u32 *)((u8 *)sgx->tbuf[TB_KD].va + SGX_TRI_KD_CCB_CTL);
	w = (u32 *)((u8 *)sgx->tbuf[TB_KD].va + SGX_TRI_KD_WORDS);
	sgx->tri_runs++;
	drm_printf(p, "triangle %u, %s form, %s page directory: %u-byte TA CCB, PB %u pages\n",
		   sgx->tri_runs - 1, two_kicks ? "two-kick" : "single-kick",
		   sgx->tri_own_pd ? "its own" : "the EDM's", woff,
		   sgx_render_pb_pages(READ_ONCE(sgx->render_pb_pages)));

	WRITE_ONCE(ctl[0], woff);		/* host write offset */
	sw0 = atomic_read(&sgx->sw_events);
	atomic_set(&sgx->bif_faults, 0);
	start = ktime_get();
	for (i = 0; i < nkicks && !ret; i++) {
		/* New pages were mapped: the BIF must refetch PD and PT entries. */
		sgx->cache_control |= SGX_CC_INVAL_BIF | SGX_CC_INVAL_DATA;
		wmb(); /* the job, before the kick */
		ret = sgxfive_command(sgx, SGX_CMD_TA, data, ARRAY_SIZE(data));
	}
	if (ret)
		return ret;
	left = wait_event_timeout(sgx->event_wq,
				  sgxfive_tri_done(&w[SGX_TRI_W_3D], td_val),
				  msecs_to_jiffies(2000));
	drm_printf(p, "%s after %lld us, %d SW_EVENTs, %d BIF faults\n",
		   left ? "3D status written" : "NO 3D STATUS",
		   ktime_us_delta(ktime_get(), start),
		   atomic_read(&sgx->sw_events) - sw0, atomic_read(&sgx->bif_faults));
	sgxfive_tri_state(sgx, p, woff);
	if (READ_ONCE(w[SGX_TRI_W_TA]) != ta_val)
		drm_printf(p, "TA status: want %#x\n", ta_val);
	ret = sgxfive_tri_check(sgx, p);
	if (!left)
		return -ETIMEDOUT;
	drm_printf(p, "%s\n", ret ? "MISMATCHES" : "triangle correct");
	return ret;
}

static int sgxfive_tri_run(struct seq_file *m, bool two_kicks)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_tri(sgx, &p, two_kicks);
	if (ret)
		sgxfive_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	/* A job that never completed wedges the EDM: recover, with an error state. */
	if (ret == -ETIMEDOUT) {
		drm_printf(&p, "recovering (devcoredump)\n");
		sgxfive_recover(sgx, U64_MAX, SGXFIVE_HANG_MANUAL);
	}
	return ret == -EIO || ret == -ETIMEDOUT ? 0 : ret;
}

static int sgxfive_tri_show(struct seq_file *m, void *unused)
{
	return sgxfive_tri_run(m, false);
}

static int sgxfive_tri2_show(struct seq_file *m, void *unused)
{
	return sgxfive_tri_run(m, true);
}

/*
 * The report (the image as text) is larger than a page: give seq_file room
 * for it at once, or it runs the show function, and the job, again.
 */
#define TRI_REPORT_SIZE	SZ_16K

static int sgxfive_tri_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, sgxfive_tri_show, inode->i_private,
				TRI_REPORT_SIZE);
}

static int sgxfive_tri2_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, sgxfive_tri2_show, inode->i_private,
				TRI_REPORT_SIZE);
}

static const struct file_operations sgxfive_tri_fops = {
	.owner = THIS_MODULE, .open = sgxfive_tri_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};

static const struct file_operations sgxfive_tri2_fops = {
	.owner = THIS_MODULE, .open = sgxfive_tri2_open, .read = seq_read,
	.llseek = seq_lseek, .release = single_release,
};

/* From the host's SGX interrupt; true when the event belongs to the EDM. */
bool sgxfive_irq(struct sgxfive *sgx, u32 stat_1, u32 stat_2)
{
	u32 fault;

	if (!sgx || !READ_ONCE(sgx->live))
		return false;
	fault = sgx_rd(sgx, SGX_BIF_INT_STAT) & SGX_BIF_FAULT_REQ;
	if (fault) {
		atomic_inc(&sgx->bif_faults);
		drm_err_ratelimited(sgx->dev, "SGX BIF fault: stat %#x addr %#x\n",
				    fault, sgx_rd(sgx, SGX_BIF_FAULT));
	}
	/* SGX_ISRHandler: acknowledge only the EDM's software event. */
	if (stat_1 & SGX_EVENT_SW) {
		atomic_inc(&sgx->sw_events);
		wake_up_all(&sgx->event_wq);
		sgx_wr(sgx, SGX_EVENT_HOST_CLEAR, SGX_EVENT_SW | SGX_EVENT_MASTER);
		sgx_wr(sgx, SGX_EVENT_HOST_CLEAR2, 0);
		(void)sgx_rd(sgx, SGX_EVENT_HOST_CLEAR2);
		sgxfive_fences_irq(sgx);	/* render-node jobs completed */
	}
	return true;
}

static ssize_t sgxfive_boot_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_boot(sgx);
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_boot_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_boot_write,
};

static ssize_t sgxfive_stop_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;

	mutex_lock(&sgx->lock);
	sgxfive_stop(sgx);
	mutex_unlock(&sgx->lock);
	return len;
}

static const struct file_operations sgxfive_stop_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_stop_write,
};

static ssize_t sgxfive_reset_write(struct file *f, const char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	struct sgxfive *sgx = file_inode(f)->i_private;
	int ret;

	ret = sgxfive_recover(sgx, U64_MAX, SGXFIVE_HANG_MANUAL);
	return ret ? ret : len;
}

static const struct file_operations sgxfive_reset_fops = {
	.owner = THIS_MODULE,
	.write = sgxfive_reset_write,
};

static int sgxfive_status_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);

	mutex_lock(&sgx->lock);
	sgxfive_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_status);

/*
 * Every register of the snapshot table and the firmware control blocks, one
 * value a line, to diff before and after a job (sgxfive_regdump.c). Reads
 * only; the registers whose write starts work are not read.
 */
static int sgxfive_regs_show(struct seq_file *m, void *unused)
{
	struct sgxfive *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	struct sgxfive_devstate *d;
	struct sgxfive_regdump *rd;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	rd = kzalloc(sizeof(*rd), GFP_KERNEL);
	if (!d || !rd) {
		kfree(d);
		kfree(rd);
		return -ENOMEM;
	}
	mutex_lock(&sgx->lock);
	sgxfive_devstate(sgx, d);
	sgxfive_regdump_capture(rd, seq_rd, sgx);
	mutex_unlock(&sgx->lock);
	sgxfive_regdump_print(&p, rd, d);
	kfree(rd);
	kfree(d);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgxfive_regs);

/*
 * Create the SGX device for a host: the DRM device, the register block and
 * the page directory the host keeps in BIF directory list 0. Nothing touches
 * the hardware until the firmware is loaded and booted.
 */
struct sgxfive *sgxfive_create(const struct sgxfive_host *host)
{
	struct sgxfive *sgx = kzalloc_obj(*sgx);

	if (!sgx)
		return NULL;
	sgx->dev = host->drm;
	sgx->regs = host->regs;
	sgx->reset_pd = host->reset_pd;
	sgx->host_bridge_rev = host->host_bridge_rev;
	/* gma500 powers the SGX island at probe, so the identity reads. */
	sgx->core_id = sgx_rd(sgx, SGX_CORE_ID);
	sgx->core_revision = sgx_rd(sgx, SGX_CORE_REVISION);
	mutex_init(&sgx->lock);
	init_waitqueue_head(&sgx->event_wq);
	drm_mm_init(&sgx->kmem, KMEM_START, KMEM_END - KMEM_START);
	spin_lock_init(&sgx->ctx_lock);
	INIT_LIST_HEAD(&sgx->ctx_list);
	INIT_DELAYED_WORK(&sgx->watchdog, sgxfive_watchdog);
	INIT_LIST_HEAD(&sgx->deferred);
	INIT_DELAYED_WORK(&sgx->reaper, sgxfive_reaper);
	sgx->fault_suspect = U64_MAX;
	sgx->render_pb_pages = SGX_RENDER_PB_PAGES;
	if (sgxfive_sched_init(sgx)) {
		drm_mm_takedown(&sgx->kmem);
		mutex_destroy(&sgx->lock);
		kfree(sgx);
		return NULL;
	}
	return sgx;
}

void sgxfive_debugfs_init(struct sgxfive *sgx, struct dentry *root)
{
	struct dentry *dir = debugfs_create_dir("sgx", root);
	unsigned int i;

	debugfs_create_file("load", 0200, dir, sgx, &sgxfive_load_fops);
	debugfs_create_file("layout", 0400, dir, sgx, &sgxfive_layout_fops);
	debugfs_create_file("boot", 0200, dir, sgx, &sgxfive_boot_fops);
	debugfs_create_file("stop", 0200, dir, sgx, &sgxfive_stop_fops);
	debugfs_create_file("reset", 0200, dir, sgx, &sgxfive_reset_fops);
	debugfs_create_file("status", 0400, dir, sgx, &sgxfive_status_fops);
	debugfs_create_file("regs", 0400, dir, sgx, &sgxfive_regs_fops);
	debugfs_create_x32("heap_start", 0600, dir, &sgx->heap_start);
	debugfs_create_file("miscinfo", 0400, dir, sgx, &sgxfive_miscinfo_fops);
	debugfs_create_file("idle", 0200, dir, sgx, &sgxfive_idle_fops);
	debugfs_create_file("xfer_dummy", 0400, dir, sgx,
			    &sgxfive_xfer_dummy_fops);
	debugfs_create_file("xfer_sync", 0400, dir, sgx, &sgxfive_xfer_sync_fops);
	debugfs_create_file("xfer_fill", 0400, dir, sgx, &sgxfive_xfer_fill_fops);
	debugfs_create_file("xfer_copy", 0400, dir, sgx, &sgxfive_xfer_copy_fops);
	debugfs_create_file("xfer_iso", 0400, dir, sgx, &sgxfive_xfer_iso_fops);
	debugfs_create_bool("alias_ro", 0600, dir, &sgx->alias_ro);
	debugfs_create_bool("no_alias", 0600, dir, &sgx->no_alias);
	debugfs_create_bool("alias_ta3d_ro", 0600, dir, &sgx->alias_ta3d_ro);
	debugfs_create_file("xfer_ro", 0400, dir, sgx, &sgxfive_xfer_ro_fops);
	debugfs_create_file("tri", 0400, dir, sgx, &sgxfive_tri_fops);
	debugfs_create_file("tri2", 0400, dir, sgx, &sgxfive_tri2_fops);
	debugfs_create_blob("tri_fb", 0400, dir, &sgx->tbuf[TB_FB].blob);
	debugfs_create_bool("tri_own_pd", 0600, dir, &sgx->tri_own_pd);
	/* Pages of the PB of render contexts created from now on (0x23-0x422). */
	debugfs_create_x32("render_pb_pages", 0600, dir, &sgx->render_pb_pages);
	for (i = 0; i < SGX_FW_MAX_BUFFERS; i++) {
		char name[8];

		snprintf(name, sizeof(name), "buf%02u", i);
		debugfs_create_blob(name, 0400, dir, &sgx->buf[i].blob);
	}
}

void sgxfive_destroy(struct sgxfive *sgx)
{
	if (!sgx)
		return;
	cancel_delayed_work_sync(&sgx->watchdog);
	sgxfive_coredump_fini(sgx);
	mutex_lock(&sgx->lock);
	sgxfive_unload(sgx);
	mutex_unlock(&sgx->lock);
	/* Halted now: whatever is still deferred goes. */
	cancel_delayed_work_sync(&sgx->reaper);
	sgxfive_reaper(&sgx->reaper.work);
	sgxfive_sched_fini(sgx);
	mutex_destroy(&sgx->lock);
	drm_mm_takedown(&sgx->kmem);
	kfree(sgx);
}
