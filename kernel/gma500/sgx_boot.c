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
	bool loaded;
	bool live;			/* EDM booted: the IRQ events are ours */
	u32 heap_start;			/* first buffer's offset in each heap */
	atomic_t sw_events;		/* EVENT_STATUS.SW_EVENT interrupts */
	atomic_t bif_faults;
};

/* Buffer ids and HostCtl dwords (design/sgx545_fw_format.md, spec §13.10). */
enum { BUF_KICKER = 8, BUF_TA3D_CTL = 9, BUF_HOST_CTL = 10, BUF_CCB_CTL = 7 };
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

static void gma_sgx_free_buf(struct gma_sgx *sgx, unsigned int id)
{
	struct gma_sgx_buf *b = &sgx->buf[id];
	unsigned int i;

	if (!b->pages)
		return;
	if (sgx->pd && (sgx->layout.addr_set & 1u << id))
		psb_mmu_remove_pages(sgx->pd, sgx->layout.addr[id], b->npages,
				     0, 0);
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
		gma_sgx_free_buf(sgx, id);
	if (sgx->pd)
		psb_mmu_free_pagedir(sgx->pd);
	sgx->pd = NULL;
	release_firmware(sgx->blob);
	sgx->blob = NULL;
	memset(&sgx->layout, 0, sizeof(sgx->layout));
	sgx->loaded = false;
}

/* Allocate one buffer write-combined, map it for the CPU and the EDM. */
static int gma_sgx_alloc_buf(struct gma_sgx *sgx, const struct sgx_fw_buffer *fb)
{
	struct gma_sgx_buf *b = &sgx->buf[fb->id];
	unsigned int i;
	int ret;

	b->npages = DIV_ROUND_UP(fb->size, PAGE_SIZE);
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
	ret = psb_mmu_insert_pages(sgx->pd, b->pages, sgx->layout.addr[fb->id],
				   b->npages, 0, 0, 0);
	if (ret)
		return ret;
	b->blob.data = b->va;
	b->blob.size = fb->size;
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

		ret = gma_sgx_alloc_buf(sgx, fb);
		if (ret)
			goto err;
		bufs[fb->id] = sgx->buf[fb->id].va;
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
static void gma_sgx_stop(struct gma_sgx *sgx)
{
	struct sgx_fw_write w[16];
	int n, i;

	if (!sgx->live)
		return;
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
	while (!(READ_ONCE(hc[HC_INIT_STATUS]) & EDM_INIT_COMPLETE)) {
		if (ktime_ms_delta(ktime_get(), start) > 500) {
			drm_err(sgx->dev, "SGX boot: no INIT_COMPLETE after 500 ms\n");
			gma_sgx_dump(sgx, &p);
			return -ETIMEDOUT;
		}
		usleep_range(50, 100);
		rmb(); /* re-read HostCtl from memory */
	}
	drm_info(sgx->dev, "SGX boot: EDM up after %lld us\n",
		 ktime_us_delta(ktime_get(), start));
	gma_sgx_dump(sgx, &p);
	return 0;
}

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
