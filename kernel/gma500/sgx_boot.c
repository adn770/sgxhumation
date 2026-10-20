// SPDX-License-Identifier: GPL-2.0-only
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
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/set_memory.h>
#include <asm/smp.h>

#include <drm/drm_file.h>
#include <drm/drm_print.h>

#include "mmu.h"
#include "psb_drv.h"
#include "sgx_boot.h"
#include "sgx_fw.h"
#include "sgx_seq.h"
#include "sgx_xfer.h"

#define SGX_FW_NAME	"sgx545_ukernel.fw"

/* sgxconfig.h heap bases, by sgxapi_km.h heap id. */
enum {
	HEAP_KERNEL_CODE = 2,
	HEAP_KERNEL_DATA = 3,
	HEAP_PIXELSHADER = 4,
	HEAP_VERTEXSHADER = 5,
	HEAP_PDSPIXEL = 6,
};

static const u32 sgx_heap_base[] = {
	[HEAP_KERNEL_CODE] = 0xe8000000,
	[HEAP_KERNEL_DATA] = 0xf0000000,
	[HEAP_PIXELSHADER] = 0xf4000000,
	[HEAP_VERTEXSHADER] = 0xfc000000,
	[HEAP_PDSPIXEL] = 0xe4000000,
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

struct gma_sgx_buf {
	struct page **pages;
	unsigned int npages;
	bool wc;		/* pages are set write-combined */
	void *va;
	u32 addr;		/* device virtual, once mapped for the EDM */
	bool mapped;
	struct debugfs_blob_wrapper blob;
};

struct gma_sgx {
	struct drm_device *dev;
	struct mutex lock;		/* serialises load and teardown */
	const struct firmware *blob;
	struct sgx_fw fw;
	struct sgx_fw_layout layout;
	struct psb_mmu_pd *pd;
	struct gma_sgx_buf buf[SGX_FW_MAX_BUFFERS];
	struct gma_sgx_buf xbuf[8];	/* transfer-queue test area, XB_* */
	u32 xfer_woff;			/* client CCB write offset */
	u32 xfer_runs;
	bool loaded;
	bool live;			/* EDM booted: the IRQ events are ours */
	u32 heap_start;			/* first buffer's offset in each heap */
	u32 cache_control;		/* SGXMKIF_CC_* for the next command */
	atomic_t sw_events;		/* EVENT_STATUS.SW_EVENT interrupts */
	atomic_t bif_faults;
};

/* Buffer ids and HostCtl dwords (design/sgx545_fw_format.md, spec §13.10). */
enum {
	BUF_CCB = 6, BUF_CCB_CTL = 7, BUF_KICKER = 8, BUF_TA3D_CTL = 9,
	BUF_HOST_CTL = 10, BUF_MISC = 11,
};

enum {
	HC_INIT_STATUS = 0, HC_POWER_STATUS = 1, HC_HWR_SAMPLE_RATE = 5,
	HC_UKERNEL_TIMER_CLOCK = 6, HC_APM_SAMPLE_RATE = 7, HC_HOST_CLOCK = 13,
	HC_ASSERT_FAIL = 14, HC_DWORDS = 17,
};

#define TA3D_CTL_DWORDS		(0xec / 4)
#define EDM_INIT_COMPLETE	0x1
#define SGX_CLOCK_HZ		400000000	/* SYS_SGX_CLOCK_SPEED */
#define SGX_EVENT_SW		0x00004000
#define SGX_EVENT_MASTER	0x80000000

/* SGXMKIF_CMD_* index into the kick table, and the misc-info protocol. */
#define SGX_CMD_TRANSFER	1
#define SGX_CMD_POWER		3
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
	XB_COUNT
};

static const struct {
	u32 addr, size;
} gma_sgx_xbuf[XB_COUNT] = {
	[XB_CCB] = { 0xf0100000, SGX_XFER_CCB_SIZE + SGX_XFER_CMD_SIZE },
	[XB_CCB_CTL] = { 0xf0120000, 8 },
	[XB_CTX] = { 0xf0121000, SGX_XFER_CTX_SIZE },
	[XB_SCRATCH] = { 0xf0122000, 0x40 },
	[XB_PDS] = { 0xe4000000, 0x1000 },
	[XB_USE] = { 0xf4000000, 0x1000 },
	[XB_BG] = { 0xf0130000, 4 * SGX_XFER_BGOBJ_DWORDS },
	[XB_DST] = { 0xf0200000, 0x3000 },
};

static void gma_sgx_unmap(struct gma_sgx *sgx, struct gma_sgx_buf *b)
{
	unsigned int i;

	if (!b->pages)
		return;
	if (sgx->pd && b->mapped)
		psb_mmu_remove_pages(sgx->pd, b->addr, b->npages, 0, 0);
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

static void gma_sgx_stop(struct gma_sgx *sgx);

static void gma_sgx_unload(struct gma_sgx *sgx)
{
	unsigned int id;

	gma_sgx_stop(sgx);

	for (id = 0; id < SGX_FW_MAX_BUFFERS; id++)
		gma_sgx_unmap(sgx, &sgx->buf[id]);
	for (id = 0; id < XB_COUNT; id++)
		gma_sgx_unmap(sgx, &sgx->xbuf[id]);
	sgx->xfer_woff = 0;
	if (sgx->pd)
		psb_mmu_free_pagedir(sgx->pd);
	sgx->pd = NULL;
	release_firmware(sgx->blob);
	sgx->blob = NULL;
	memset(&sgx->layout, 0, sizeof(sgx->layout));
	sgx->loaded = false;
}

/* Allocate a zeroed buffer write-combined, map it for the CPU and the EDM. */
static int gma_sgx_map(struct gma_sgx *sgx, struct gma_sgx_buf *b, u32 addr,
		       u32 size)
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
	ret = psb_mmu_insert_pages(sgx->pd, b->pages, addr, b->npages, 0, 0, 0);
	if (ret)
		return ret;
	b->addr = addr;
	b->mapped = true;
	b->blob.data = b->va;
	b->blob.size = size;
	return 0;
}

static int gma_sgx_load(struct gma_sgx *sgx)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(sgx->dev);
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
	sgx->pd = psb_mmu_alloc_pd(dev_priv->mmu, 1, 0);
	if (!sgx->pd) {
		ret = -ENOMEM;
		goto err;
	}
	l->pd_phys = page_to_phys(sgx->pd->p);

	for (i = 0; i < sgx->fw.nbuffers; i++) {
		const struct sgx_fw_buffer *fb = &sgx->fw.buffers[i];

		ret = gma_sgx_map(sgx, &sgx->buf[fb->id], l->addr[fb->id],
				  fb->size);
		if (ret)
			goto err;
		bufs[fb->id] = sgx->buf[fb->id].va;
	}
	for (i = 0; i < XB_COUNT; i++) {
		ret = gma_sgx_map(sgx, &sgx->xbuf[i], gma_sgx_xbuf[i].addr,
				  gma_sgx_xbuf[i].size);
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
	gma_sgx_unload(sgx);
	return ret;
}

static ssize_t gma_sgx_load_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct gma_sgx *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	if (sgx->loaded)
		gma_sgx_unload(sgx);
	ret = gma_sgx_load(sgx);
	mutex_unlock(&sgx->lock);
	if (ret)
		drm_err(sgx->dev, "SGX firmware load failed: %d\n", ret);
	return ret ? ret : len;
}

static const struct file_operations gma_sgx_load_fops = {
	.owner = THIS_MODULE,
	.write = gma_sgx_load_write,
};

/* The placement and PD the buffers were built for, as fwload.py layout JSON. */
static int gma_sgx_layout_show(struct seq_file *m, void *unused)
{
	struct gma_sgx *sgx = m->private;
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
DEFINE_SHOW_ATTRIBUTE(gma_sgx_layout);

static u32 *gma_sgx_words(struct gma_sgx *sgx, unsigned int id)
{
	return sgx->buf[id].va;
}

static u32 seq_rd(void *ctx, u32 reg)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(ctx);

	return PSB_RSGX32(reg);
}

static void seq_wr(void *ctx, u32 reg, u32 val)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(ctx);

	PSB_WSGX32(val, reg);
	(void)PSB_RSGX32(reg);		/* post */
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
	struct gma_sgx *sgx = to_drm_psb_private(ctx)->sgx;
	u32 *hc = gma_sgx_words(sgx, BUF_HOST_CTL);

	WRITE_ONCE(*gma_sgx_words(sgx, BUF_KICKER), 0);
	WRITE_ONCE(hc[HC_HOST_CLOCK], (u32)(jiffies * (1000000 / HZ)));
	WRITE_ONCE(hc[HC_INIT_STATUS], 0);
	wmb(); /* the EDM must see all of the above before the kick */
}

/* Poll a dword the EDM writes until (v & mask) == want, for up to 500 ms. */
static int gma_sgx_wait_mem(const u32 *w, u32 mask, u32 want)
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

static const u32 gma_sgx_dump_regs[] = {
	SGX_CORE_ID, SGX_CORE_REVISION, SGX_CLKGATECTL, SGX_CLKGATESTATUS,
	SGX_SOFT_RESET, SGX_EVENT_STATUS, SGX_EVENT_STATUS2,
	SGX_EVENT_HOST_ENABLE, SGX_BIF_CTRL, SGX_BIF_BANK0, SGX_BIF_INT_STAT,
	SGX_BIF_FAULT, SGX_BIF_MEM_REQ_STAT, SGX_BIF_DIR_LIST_BASE0,
	SGX_BIF_DIR_LIST_BASE15, SGX_USE0_SERV_EVENT, SGX_USE1_SERV_EVENT,
};

static void gma_sgx_dump_words(struct drm_printer *p, const char *name,
			       const u32 *w, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i += 8)
		drm_printf(p, "%s[%2u]: %*ph\n", name, i,
			   (int)(min(8u, n - i) * 4), &w[i]);
}

/* What SGXDumpDebugInfo shows: registers, HostCtl, TA3DCtl, CCB control. */
static void gma_sgx_dump(struct gma_sgx *sgx, struct drm_printer *p)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(sgx->dev);
	unsigned int i;

	drm_printf(p, "loaded %d live %d sw_events %d bif_faults %d\n",
		   sgx->loaded, sgx->live, atomic_read(&sgx->sw_events),
		   atomic_read(&sgx->bif_faults));
	for (i = 0; i < ARRAY_SIZE(gma_sgx_dump_regs); i++)
		drm_printf(p, "reg %#05x = %#010x\n", gma_sgx_dump_regs[i],
			   PSB_RSGX32(gma_sgx_dump_regs[i]));
	if (!sgx->loaded)
		return;
	rmb(); /* fresh reads of what the EDM wrote */
	gma_sgx_dump_words(p, "hostctl", gma_sgx_words(sgx, BUF_HOST_CTL),
			   HC_DWORDS);
	gma_sgx_dump_words(p, "ta3dctl", gma_sgx_words(sgx, BUF_TA3D_CTL),
			   TA3D_CTL_DWORDS);
	gma_sgx_dump_words(p, "ccbctl", gma_sgx_words(sgx, BUF_CCB_CTL), 2);
}

/* Deinit script, then hold every block in reset; the EDM stops touching memory. */
static int gma_sgx_power(struct gma_sgx *sgx, u32 powercmd, u32 done);

/*
 * SGXPrePowerState(OFF): ask the EDM to power off, then run the deinit script.
 * Hold every block in reset afterwards either way, so the EDM is certain to
 * have stopped touching memory before its buffers are freed.
 */
static void gma_sgx_stop(struct gma_sgx *sgx)
{
	struct sgx_fw_write w[16];
	int n, i, ret;

	if (!sgx->live)
		return;
	ret = gma_sgx_power(sgx, POWERCMD_POWEROFF, POWMAN_POWEROFF_COMPLETE);
	if (ret)
		drm_warn(sgx->dev, "SGX power-off request failed: %d\n", ret);
	n = sgx_fw_script(&sgx->fw, &sgx->layout, SGX_FW_DEINIT, w, ARRAY_SIZE(w));
	for (i = 0; i < n; i++)
		seq_wr(sgx->dev, w[i].reg, w[i].val);
	seq_wr(sgx->dev, SGX_SOFT_RESET, SGX_SOFT_RESET_ALL);
	WRITE_ONCE(sgx->live, false);
	drm_info(sgx->dev, "SGX stopped, %d events seen\n",
		 atomic_read(&sgx->sw_events));
}

static int gma_sgx_boot(struct gma_sgx *sgx)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(sgx->dev);
	struct sgx_fw_write part1[16], part2[64];
	struct drm_printer p = drm_info_printer(sgx->dev->dev);
	struct sgx_seq s = {
		.rd = seq_rd, .wr = seq_wr, .udelay = seq_udelay,
		.pre_kick = seq_pre_kick, .ctx = sgx->dev,
		.part1 = part1, .part2 = part2,
	};
	ktime_t start;
	u32 *hc;
	int n, ret;

	if (sgx->live)
		return -EBUSY;
	if (!sgx->loaded) {
		ret = gma_sgx_load(sgx);
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
	s.reset_pd = page_to_phys(psb_mmu_get_default_pd(dev_priv->mmu)->p);
	s.edm_pd = sgx->layout.pd_phys;

	/* HostCtl as SGXPostPowerState leaves it (B1-B5). */
	hc = gma_sgx_words(sgx, BUF_HOST_CTL);
	WRITE_ONCE(hc[HC_POWER_STATUS], 0);
	WRITE_ONCE(hc[HC_HWR_SAMPLE_RATE], 10);
	WRITE_ONCE(hc[HC_UKERNEL_TIMER_CLOCK], SGX_CLOCK_HZ / 1000);
	WRITE_ONCE(hc[HC_APM_SAMPLE_RATE], 0);
	/* The PD and page tables were written cached: make them visible. */
	wbinvd_on_all_cpus();

	atomic_set(&sgx->sw_events, 0);
	atomic_set(&sgx->bif_faults, 0);
	WRITE_ONCE(sgx->live, true);
	drm_info(sgx->dev, "SGX boot: kicking the EDM\n");
	ret = sgx_seq_boot(&s);
	if (ret) {
		drm_err(sgx->dev, "SGX boot: BIF fault during reset\n");
		gma_sgx_dump(sgx, &p);
		WRITE_ONCE(sgx->live, false);
		return ret;
	}
	mb(); /* OSMemoryBarrier after the kick */

	start = ktime_get();
	ret = gma_sgx_wait_mem(&hc[HC_INIT_STATUS], EDM_INIT_COMPLETE,
			       EDM_INIT_COMPLETE);
	if (ret) {
		drm_err(sgx->dev, "SGX boot: no INIT_COMPLETE after 500 ms\n");
		gma_sgx_dump(sgx, &p);
		return ret;
	}
	drm_info(sgx->dev, "SGX boot: EDM up after %lld us\n",
		 ktime_us_delta(ktime_get(), start));
	/* InitDevInfo: the first command invalidates the BIF PD cache. */
	sgx->cache_control = sgx->fw.meta[SGX_FW_META_CACHE_CONTROL];
	gma_sgx_dump(sgx, &p);
	return 0;
}

/* Submit one kernel CCB command (SGXScheduleCCBCommand). */
static int gma_sgx_command(struct gma_sgx *sgx, unsigned int cmd,
			   const u32 *data, unsigned int ndata)
{
	struct sgx_seq s = {
		.wr = seq_wr, .wmb = seq_wmb, .ctx = sgx->dev,
	};
	int ret;

	if (!sgx->live)
		return -ENODEV;
	ret = sgx_ccb_submit(&s, gma_sgx_words(sgx, BUF_CCB),
			     gma_sgx_words(sgx, BUF_CCB_CTL),
			     gma_sgx_words(sgx, BUF_KICKER),
			     sgx->fw.meta[SGX_FW_META_KICK + cmd],
			     sgx->cache_control, data, ndata);
	if (!ret)
		sgx->cache_control = 0;
	return ret;
}

/*
 * SGXMKIF_CMD_POWER (SGXPrePowerState): POWEROFF or IDLE, wait for the EDM to
 * report the transition in HostCtl.ui32PowerStatus, then for the clocks to
 * gate (the init info's clock-gating status register and mask).
 */
static int gma_sgx_power(struct gma_sgx *sgx, u32 powercmd, u32 done)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(sgx->dev);
	u32 *hc = gma_sgx_words(sgx, BUF_HOST_CTL);
	u32 reg = sgx->fw.meta[SGX_FW_META_CLKGATE + 2];
	u32 mask = sgx->fw.meta[SGX_FW_META_CLKGATE + 3];
	const u32 data[] = { 0, powercmd };
	ktime_t start = ktime_get();
	int ret;

	WRITE_ONCE(hc[HC_POWER_STATUS], READ_ONCE(hc[HC_POWER_STATUS]) & ~done);
	ret = gma_sgx_command(sgx, SGX_CMD_POWER, data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	ret = gma_sgx_wait_mem(&hc[HC_POWER_STATUS], done, done);
	if (ret)
		return ret;
	while (PSB_RSGX32(reg) & mask) {
		if (ktime_ms_delta(ktime_get(), start) > 500)
			return -ETIMEDOUT;
		usleep_range(50, 100);
	}
	drm_info(sgx->dev, "SGX %s after %lld us\n",
		 powercmd == POWERCMD_IDLE ? "idle" : "powered off",
		 ktime_us_delta(ktime_get(), start));
	return 0;
}

static ssize_t gma_sgx_idle_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct gma_sgx *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	ret = gma_sgx_power(sgx, POWERCMD_IDLE, POWMAN_IDLE_COMPLETE);
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations gma_sgx_idle_fops = {
	.owner = THIS_MODULE,
	.write = gma_sgx_idle_write,
};

static const char *const gma_sgx_feature_names[] = {
	"core rev", "core id", "ddk version", "ddk build", "core id sw",
	"core rev sw", "build options",
};

static void gma_sgx_check(struct drm_printer *p, const char *what, u32 got,
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
static int gma_sgx_miscinfo(struct gma_sgx *sgx, struct drm_printer *p)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(sgx->dev);
	u32 *mi = gma_sgx_words(sgx, BUF_MISC);
	const u32 data[] = { 0, sgx->layout.addr[BUF_MISC] };
	u32 want[7];
	ktime_t start;
	int i, ret, bad = 0;

	for (i = 1; i < MISCINFO_DWORDS; i++)
		WRITE_ONCE(mi[i], 0);
	WRITE_ONCE(mi[0], MISCINFO_GET_STRUCT_SIZES);
	ret = gma_sgx_command(sgx, SGX_CMD_GETMISCINFO, data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	start = ktime_get();
	ret = gma_sgx_wait_mem(&mi[0], MISCINFO_READY, MISCINFO_READY);
	if (ret) {
		drm_printf(p, "no MISCINFO_READY after 500 ms\n");
		gma_sgx_dump(sgx, p);
		return ret;
	}
	drm_printf(p, "GETMISCINFO answered after %lld us\n",
		   ktime_us_delta(ktime_get(), start));

	want[0] = PSB_RSGX32(SGX_CORE_REVISION);
	want[1] = PSB_RSGX32(SGX_CORE_ID);
	/* PVRVERSION_MAJ/MIN/BRANCH: DDK 1.7 is branch 17 */
	want[2] = sgx->fw.ddk_major << 16 | sgx->fw.ddk_minor << 8 | 17;
	want[3] = sgx->fw.ddk_build;
	want[4] = 545;
	want[5] = want[0];		/* the ukernel's own revision: 1.0.13 */
	want[6] = sgx->fw.build_options;
	for (i = 0; i < 7; i++)
		gma_sgx_check(p, gma_sgx_feature_names[i], mi[1 + i], want[i],
			      &bad);
	for (i = 0; i < 13; i++) {
		char what[16];

		snprintf(what, sizeof(what), "struct size %d", i);
		gma_sgx_check(p, what, mi[8 + i],
			      sgx->fw.meta[SGX_FW_META_STRUCT_SIZES + i], &bad);
	}
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "all values match");
	return bad ? -EIO : 0;
}

static int gma_sgx_miscinfo_show(struct seq_file *m, void *unused)
{
	struct gma_sgx *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = gma_sgx_miscinfo(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(gma_sgx_miscinfo);

/* Queue one dummy transfer, optionally behind a destination sync. */
static int gma_sgx_xfer_submit(struct gma_sgx *sgx,
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
	return gma_sgx_command(sgx, SGX_CMD_TRANSFER, data, ARRAY_SIZE(data));
}

/* After a completed dummy: its writes, the CCB, the count and one SW_EVENT. */
static void gma_sgx_xfer_verify(struct gma_sgx *sgx, struct drm_printer *p,
				u32 fence, u32 update, u32 done, int sw0,
				int *bad)
{
	u32 *ctl = sgx->xbuf[XB_CCB_CTL].va, *scr = sgx->xbuf[XB_SCRATCH].va;
	u32 *ctx = sgx->xbuf[XB_CTX].va;
	ktime_t start = ktime_get();
	int ret;

	ret = gma_sgx_wait_mem(&scr[SCR_F], ~0u, fence);
	drm_printf(p, "dummy transfer %u: %s after %lld us\n", sgx->xfer_runs - 1,
		   ret ? "NO STATUS WRITE" : "status written",
		   ktime_us_delta(ktime_get(), start));
	while (atomic_read(&sgx->sw_events) == sw0 &&
	       ktime_ms_delta(ktime_get(), start) < 500)
		usleep_range(50, 100);
	rmb(); /* fresh reads of what the EDM wrote */
	gma_sgx_check(p, "status F", READ_ONCE(scr[SCR_F]), fence, bad);
	gma_sgx_check(p, "update U", READ_ONCE(scr[SCR_U]), update, bad);
	gma_sgx_check(p, "ccb read off", READ_ONCE(ctl[1]), done, bad);
	gma_sgx_check(p, "ctx count", READ_ONCE(ctx[SGX_XFER_CTX_COUNT / 4]), 0,
		      bad);
	gma_sgx_check(p, "sw events", atomic_read(&sgx->sw_events) - sw0, 1, bad);
}

/*
 * A dummy transfer (design/sgx545_transfer.md §4.1): no render, but the EDM
 * must queue the context, apply one memory update (U), write one status word
 * (F), advance the client CCB, drop the context's count and raise SW_EVENT.
 */
static int gma_sgx_xfer_dummy(struct gma_sgx *sgx, struct drm_printer *p)
{
	int sw0 = atomic_read(&sgx->sw_events), bad = 0, ret;
	u32 fence, update, done;

	ret = gma_sgx_xfer_submit(sgx, NULL, &fence, &update, &done);
	if (ret)
		return ret;
	gma_sgx_xfer_verify(sgx, p, fence, update, done, sw0, &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "dummy transfer complete");
	return bad ? -EIO : 0;
}

/*
 * The readiness test: a dummy behind a destination sync whose write counter is
 * one short must stay queued; once the host completes it and kicks
 * PROCESS_QUEUES it runs, and the EDM moves the write counter past its pending
 * value.
 */
static int gma_sgx_xfer_sync(struct gma_sgx *sgx, struct drm_printer *p)
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
	ret = gma_sgx_xfer_submit(sgx, &dst, &fence, &update, &done);
	if (ret)
		return ret;
	msleep(50);
	rmb(); /* fresh reads of what the EDM wrote */
	gma_sgx_check(p, "held: status F", READ_ONCE(scr[SCR_F]), 0, &bad);
	gma_sgx_check(p, "held: read off", READ_ONCE(ctl[1]), before, &bad);
	gma_sgx_check(p, "held: count", READ_ONCE(ctx[SGX_XFER_CTX_COUNT / 4]), 1,
		      &bad);

	WRITE_ONCE(scr[SCR_SYNC_WRITE], 6);	/* the write op completes */
	ret = gma_sgx_command(sgx, SGX_CMD_PROCESS_QUEUES, NULL, 0);
	if (ret)
		return ret;
	gma_sgx_xfer_verify(sgx, p, fence, update, done, sw0, &bad);
	/* A destination's read counter is left alone; only sources get it. */
	gma_sgx_check(p, "sync read cmpl", READ_ONCE(scr[SCR_SYNC_READ]), 0, &bad);
	gma_sgx_check(p, "sync write cmpl", READ_ONCE(scr[SCR_SYNC_WRITE]), 7, &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "sync wait and release correct");
	return bad ? -EIO : 0;
}

static int gma_sgx_xfer_sync_show(struct seq_file *m, void *unused)
{
	struct gma_sgx *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = gma_sgx_xfer_sync(sgx, &p);
	if (ret)
		gma_sgx_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(gma_sgx_xfer_sync);

static int gma_sgx_xfer_dummy_show(struct seq_file *m, void *unused)
{
	struct gma_sgx *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = gma_sgx_xfer_dummy(sgx, &p);
	if (ret)
		gma_sgx_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(gma_sgx_xfer_dummy);

/*
 * Where the solid fill's seven programs go (design/sgx545_transfer.md §4.2.7):
 * PDS code at the PDS heap base, USE code at the pixel-shader code base, each
 * inside the one page mapped there.
 */
static const struct {
	u8 area;		/* XB_PDS or XB_USE */
	u16 off;
} gma_sgx_tq_place[SGX_FW_TQ_MAX] = {
	[SGX_FW_TQ_PDS_PRIM] = { XB_PDS, 0x00 },
	[SGX_FW_TQ_PDS_SEC] = { XB_PDS, 0x20 },
	[SGX_FW_TQ_PDS_PIXEVENT] = { XB_PDS, 0x40 },
	[SGX_FW_TQ_USE_FILL] = { XB_USE, 0x00 },
	[SGX_FW_TQ_USE_SEC] = { XB_USE, 0x10 },
	[SGX_FW_TQ_USE_EOT] = { XB_USE, 0x20 },
	[SGX_FW_TQ_USE_EOR] = { XB_USE, 0x60 },
};

#define FILL_W		64
#define FILL_H		32
#define FILL_COLOUR	0xff336699
#define FILL_SENTINEL	0xdeadbeef

/*
 * A solid fill (§4.2): 64 x 32 ARGB8888 into the first two destination pages.
 * The command comes from sgx_xfer_fill_cmd() and is checked word for word
 * against the firmware file's TQ_CMD list before the kick.
 */
static int gma_sgx_xfer_fill(struct gma_sgx *sgx, struct drm_printer *p)
{
	struct sgx_fw_layout *l = &sgx->layout;
	u8 *progs[SGX_FW_TQ_MAX] = { NULL };
	u32 params[SGX_FW_TQ_NUM_PARAMS];
	const struct sgx_fw_tq_fill tf = {
		.dest = sgx->xbuf[XB_DST].addr, .width = FILL_W, .height = FILL_H,
		.stride = 4 * FILL_W, .colour = FILL_COLOUR,
	};
	const struct sgx_fw_tq *pri = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_PRIM);
	const struct sgx_fw_tq *sec = sgx_fw_tq(&sgx->fw, SGX_FW_TQ_PDS_SEC);
	struct sgx_xfer_fill f = {
		.rect = { 0, 0, FILL_W, FILL_H },
		.bg = sgx->xbuf[XB_BG].addr,
		.req_base = sgx->xbuf[XB_BG].addr & 0xfff00000,
		.pds_exec_base = sgx->xbuf[XB_PDS].addr,
		.status_addr = sgx->xbuf[XB_SCRATCH].addr + 4 * SCR_F,
		.fence_id = sgx->xfer_runs,
	};
	struct sgx_fw_write cw[16];
	u32 *ctl = sgx->xbuf[XB_CCB_CTL].va, *scr = sgx->xbuf[XB_SCRATCH].va;
	u32 *dst = sgx->xbuf[XB_DST].va, *ctx = sgx->xbuf[XB_CTX].va;
	u8 *cmd = (u8 *)sgx->xbuf[XB_CCB].va + sgx->xfer_woff;
	const u32 data[] = { 0, sgx->xbuf[XB_CTX].addr };
	unsigned int i, good = 0, guard = 0, first_bad = ~0u;
	int n, ret, sw0, bad = 0;
	ktime_t start;
	u32 size, done;

	if (!sgx->live)
		return -ENODEV;
	if (!pri || !sec) {
		drm_printf(p, "the firmware file has no transfer templates (v1)\n");
		return -ENOENT;
	}
	for (i = 0; i < SGX_FW_TQ_MAX; i++) {
		if (!sgx_fw_tq(&sgx->fw, i))
			continue;
		l->tq_addr[i] = sgx->xbuf[gma_sgx_tq_place[i].area].addr +
				gma_sgx_tq_place[i].off;
		l->tq_set |= 1u << i;
		progs[i] = (u8 *)sgx->xbuf[gma_sgx_tq_place[i].area].va +
			   gma_sgx_tq_place[i].off;
	}
	f.pixev_exec = l->tq_addr[SGX_FW_TQ_PDS_PIXEVENT];
	ret = sgx_fw_tq_fill_params(&tf, params);
	if (!ret)
		ret = sgx_fw_tq_build(&sgx->fw, l, params, progs);
	if (ret) {
		drm_printf(p, "building the fill programs failed: %d\n", ret);
		return ret;
	}
	sgx_xfer_bgobj(sgx->xbuf[XB_BG].va, f.pds_exec_base,
		       l->tq_addr[SGX_FW_TQ_PDS_PRIM], pri->pds_data,
		       l->tq_addr[SGX_FW_TQ_PDS_SEC], sec->pds_data, &f.rect);
	for (i = 0; i < 3 * PAGE_SIZE / 4; i++)
		WRITE_ONCE(dst[i], FILL_SENTINEL);
	if (!sgx->xfer_runs)
		sgx_xfer_ctx_init((u8 *)ctx, l->pd_phys, sgx->xbuf[XB_CCB].addr,
				  sgx->xbuf[XB_CCB_CTL].addr);
	WRITE_ONCE(scr[SCR_F], 0);

	size = sgx_xfer_fill_cmd(cmd, &f);
	n = sgx_fw_tq_cmd(&sgx->fw, l, params, cw, ARRAY_SIZE(cw));
	if (n < 0)
		return n;
	for (i = 0; i < n; i++) {
		u32 *w = (u32 *)(cmd + cw[i].reg);

		if (*w != cw[i].val) {
			drm_printf(p, "command word %#x: built %#x, TQ_CMD %#x\n",
				   cw[i].reg, *w, cw[i].val);
			bad++;
		}
	}
	if (bad)
		return -EINVAL;
	drm_printf(p, "command matches the %d TQ_CMD words\n", n);

	done = (sgx->xfer_woff + size) & (SGX_XFER_CCB_SIZE - 1);
	WRITE_ONCE(ctl[0], done);
	sgx->xfer_woff = done;
	sgx->xfer_runs++;
	sw0 = atomic_read(&sgx->sw_events);
	/* New pages were mapped: the BIF must refetch PD and PT entries. */
	sgx->cache_control |= SGX_CC_INVAL_BIF | SGX_CC_INVAL_DATA;
	wmb(); /* programs, object, command and sentinel before the kick */
	ret = gma_sgx_command(sgx, SGX_CMD_TRANSFER, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	start = ktime_get();
	ret = gma_sgx_wait_mem(&scr[SCR_F], ~0u, f.fence_id + 1);
	drm_printf(p, "fill: %s after %lld us\n",
		   ret ? "NO COMPLETION" : "complete",
		   ktime_us_delta(ktime_get(), start));
	while (atomic_read(&sgx->sw_events) == sw0 &&
	       ktime_ms_delta(ktime_get(), start) < 500)
		usleep_range(50, 100);
	rmb(); /* fresh reads of what the GPU wrote */

	for (i = 0; i < 2 * PAGE_SIZE / 4; i++) {
		if (READ_ONCE(dst[i]) == FILL_COLOUR)
			good++;
		else if (first_bad == ~0u)
			first_bad = i;
	}
	for (; i < 3 * PAGE_SIZE / 4; i++)
		guard += READ_ONCE(dst[i]) == FILL_SENTINEL;
	drm_printf(p, "dst[0..7]: %*ph\n", 32, dst);
	if (first_bad != ~0u)
		drm_printf(p, "first wrong dword %u = %#x\n", first_bad,
			   READ_ONCE(dst[first_bad]));
	gma_sgx_check(p, "filled dwords", good, 2 * PAGE_SIZE / 4, &bad);
	gma_sgx_check(p, "guard intact", guard, PAGE_SIZE / 4, &bad);
	gma_sgx_check(p, "status F", READ_ONCE(scr[SCR_F]), f.fence_id + 1, &bad);
	gma_sgx_check(p, "ccb read off", READ_ONCE(ctl[1]), done, &bad);
	gma_sgx_check(p, "ctx count", READ_ONCE(ctx[SGX_XFER_CTX_COUNT / 4]), 0,
		      &bad);
	gma_sgx_check(p, "sw events", atomic_read(&sgx->sw_events) - sw0, 1, &bad);
	gma_sgx_check(p, "bif faults", atomic_read(&sgx->bif_faults), 0, &bad);
	drm_printf(p, "%s\n", bad ? "MISMATCHES" : "solid fill correct");
	return bad ? -EIO : 0;
}

static int gma_sgx_xfer_fill_show(struct seq_file *m, void *unused)
{
	struct gma_sgx *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	int ret;

	mutex_lock(&sgx->lock);
	ret = gma_sgx_xfer_fill(sgx, &p);
	if (ret)
		gma_sgx_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return ret == -EIO ? 0 : ret;
}
DEFINE_SHOW_ATTRIBUTE(gma_sgx_xfer_fill);

/* Called from gma_sgx_interrupt; true when the event belongs to the EDM. */
bool gma_sgx_irq(struct drm_device *dev, u32 stat_1, u32 stat_2)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct gma_sgx *sgx = dev_priv->sgx;
	u32 fault;

	if (!sgx || !READ_ONCE(sgx->live))
		return false;
	fault = PSB_RSGX32(SGX_BIF_INT_STAT) & SGX_BIF_FAULT_REQ;
	if (fault) {
		atomic_inc(&sgx->bif_faults);
		drm_err_ratelimited(dev, "SGX BIF fault: stat %#x addr %#x\n",
				    fault, PSB_RSGX32(SGX_BIF_FAULT));
	}
	/* SGX_ISRHandler: acknowledge only the EDM's software event. */
	if (stat_1 & SGX_EVENT_SW) {
		atomic_inc(&sgx->sw_events);
		PSB_WSGX32(SGX_EVENT_SW | SGX_EVENT_MASTER, SGX_EVENT_HOST_CLEAR);
		PSB_WSGX32(0, SGX_EVENT_HOST_CLEAR2);
		(void)PSB_RSGX32(SGX_EVENT_HOST_CLEAR2);
	}
	return true;
}

static ssize_t gma_sgx_boot_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct gma_sgx *sgx = file_inode(f)->i_private;
	int ret;

	mutex_lock(&sgx->lock);
	ret = gma_sgx_boot(sgx);
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations gma_sgx_boot_fops = {
	.owner = THIS_MODULE,
	.write = gma_sgx_boot_write,
};

static ssize_t gma_sgx_stop_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct gma_sgx *sgx = file_inode(f)->i_private;

	mutex_lock(&sgx->lock);
	gma_sgx_stop(sgx);
	mutex_unlock(&sgx->lock);
	return len;
}

static const struct file_operations gma_sgx_stop_fops = {
	.owner = THIS_MODULE,
	.write = gma_sgx_stop_write,
};

static int gma_sgx_status_show(struct seq_file *m, void *unused)
{
	struct gma_sgx *sgx = m->private;
	struct drm_printer p = drm_seq_file_printer(m);

	mutex_lock(&sgx->lock);
	gma_sgx_dump(sgx, &p);
	mutex_unlock(&sgx->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gma_sgx_status);

void gma_sgx_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct gma_sgx *sgx;
	struct dentry *dir;
	unsigned int i;

	if (!IS_CDV(dev) || dev_priv->sgx)
		return;
	sgx = kzalloc_obj(*sgx);
	if (!sgx)
		return;
	sgx->dev = dev;
	mutex_init(&sgx->lock);
	dev_priv->sgx = sgx;

	dir = debugfs_create_dir("sgx", minor->debugfs_root);
	debugfs_create_file("load", 0200, dir, sgx, &gma_sgx_load_fops);
	debugfs_create_file("layout", 0400, dir, sgx, &gma_sgx_layout_fops);
	debugfs_create_file("boot", 0200, dir, sgx, &gma_sgx_boot_fops);
	debugfs_create_file("stop", 0200, dir, sgx, &gma_sgx_stop_fops);
	debugfs_create_file("status", 0400, dir, sgx, &gma_sgx_status_fops);
	debugfs_create_x32("heap_start", 0600, dir, &sgx->heap_start);
	debugfs_create_file("miscinfo", 0400, dir, sgx, &gma_sgx_miscinfo_fops);
	debugfs_create_file("idle", 0200, dir, sgx, &gma_sgx_idle_fops);
	debugfs_create_file("xfer_dummy", 0400, dir, sgx,
			    &gma_sgx_xfer_dummy_fops);
	debugfs_create_file("xfer_sync", 0400, dir, sgx, &gma_sgx_xfer_sync_fops);
	debugfs_create_file("xfer_fill", 0400, dir, sgx, &gma_sgx_xfer_fill_fops);
	for (i = 0; i < SGX_FW_MAX_BUFFERS; i++) {
		char name[8];

		snprintf(name, sizeof(name), "buf%02u", i);
		debugfs_create_blob(name, 0400, dir, &sgx->buf[i].blob);
	}
}

void gma_sgx_fini(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct gma_sgx *sgx = dev_priv->sgx;

	if (!sgx)
		return;
	mutex_lock(&sgx->lock);
	gma_sgx_unload(sgx);
	mutex_unlock(&sgx->lock);
	mutex_destroy(&sgx->lock);
	kfree(sgx);
	dev_priv->sgx = NULL;
}
