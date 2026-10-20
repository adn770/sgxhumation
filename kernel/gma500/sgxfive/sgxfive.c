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
#include <linux/wait.h>
#include <asm/set_memory.h>

#include <drm/drm_file.h>
#include <drm/drm_mm.h>
#include <drm/drm_print.h>

#include "sgxfive.h"
#include "sgxfive_ctx.h"
#include "sgxfive_priv.h"
#include "sgx_fw.h"
#include "sgx_mmu.h"
#include "sgx_seq.h"
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

static void sgxfive_dump_words(struct drm_printer *p, const char *name,
			       const u32 *w, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i += 8)
		drm_printf(p, "%s[%2u]: %*ph\n", name, i,
			   (int)(min(8u, n - i) * 4), &w[i]);
}

/* What SGXDumpDebugInfo shows: registers, HostCtl, TA3DCtl, CCB control. */
static void sgxfive_dump(struct sgxfive *sgx, struct drm_printer *p)
{
	unsigned int i;

	drm_printf(p, "loaded %d live %d sw_events %d bif_faults %d\n",
		   sgx->loaded, sgx->live, atomic_read(&sgx->sw_events),
		   atomic_read(&sgx->bif_faults));
	for (i = 0; i < ARRAY_SIZE(sgxfive_dump_regs); i++)
		drm_printf(p, "reg %#05x = %#010x\n", sgxfive_dump_regs[i],
			   sgx_rd(sgx, sgxfive_dump_regs[i]));
	if (!sgx->loaded)
		return;
	rmb(); /* fresh reads of what the EDM wrote */
	sgxfive_dump_words(p, "hostctl", sgxfive_words(sgx, BUF_HOST_CTL),
			   HC_DWORDS);
	sgxfive_dump_words(p, "ta3dctl", sgxfive_words(sgx, BUF_TA3D_CTL),
			   TA3D_CTL_DWORDS);
	sgxfive_dump_words(p, "ccbctl", sgxfive_words(sgx, BUF_CCB_CTL), 2);
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

/* With blame: the oldest unfinished job hung the firmware; fail its context. */
static void sgxfive_blame(struct sgxfive *sgx)
{
	struct sgxfive_ctx *ctx, *guilty = NULL;
	u64 order, oldest = U64_MAX;

	spin_lock_irq(&sgx->ctx_lock);
	list_for_each_entry(ctx, &sgx->ctx_list, link) {
		if (sgxfive_ctx_oldest(ctx, &order) && order < oldest) {
			oldest = order;
			guilty = ctx;
		}
	}
	if (guilty) {
		WRITE_ONCE(guilty->lost, true);
		sgxfive_fence_fail_all(guilty, -EIO);
	}
	spin_unlock_irq(&sgx->ctx_lock);
}

/*
 * Recovery reset (HWR): halt the firmware without asking a wedged EDM to power
 * off, rebuild every firmware buffer from the file in place and boot again.
 * The buffers keep their pages and addresses, so the kernel memory of the
 * render node's contexts, mapped in the same directory, stays where it is.
 * Every context starts over: its HW context image is rebuilt and its client
 * CCB rewritten with its unfinished jobs, which run again after the boot.
 * With blame, the context of the oldest unfinished job, the one the firmware
 * hung on as it runs one transfer at a time, is lost instead.
 */
static int sgxfive_reset(struct sgxfive *sgx, bool blame)
{
	u8 *bufs[SGX_FW_MAX_BUFFERS] = { NULL };
	struct sgxfive_ctx *ctx;
	unsigned int i;
	int ret;

	if (!sgx->loaded)
		return -ENODEV;
	if (blame)
		sgxfive_blame(sgx);
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
		} else if (ctx->replay) {
			sgxfive_kick_transfer(sgx, ctx->mem.addr + SGXFIVE_CTX_HW_OFF);
		}
	}
	spin_unlock_irq(&sgx->ctx_lock);
	return ret;
}

/* Reset the firmware and recover the render node's contexts (see above). */
int sgxfive_recover(struct sgxfive *sgx, bool blame)
{
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgxfive_reset(sgx, blame);
	mutex_unlock(&sgx->lock);
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
	if (!ret)
		sgx->cache_control = 0;
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

	ret = sgxfive_recover(sgx, false);
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
	debugfs_create_file("xfer_ro", 0400, dir, sgx, &sgxfive_xfer_ro_fops);
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
	mutex_lock(&sgx->lock);
	sgxfive_unload(sgx);
	mutex_unlock(&sgx->lock);
	sgxfive_sched_fini(sgx);
	mutex_destroy(&sgx->lock);
	drm_mm_takedown(&sgx->kmem);
	kfree(sgx);
}
