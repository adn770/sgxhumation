// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * SGX545 firmware loader, formats v1 and v2 (design/sgx545_fw_format.md).
 *
 * A straight port of tools/fwcutter/fwload.py and fwformat.unpack(), written
 * freestanding so the same file builds in the kernel and in userspace. Given
 * the file and the device addresses a driver chose, it produces the patched
 * buffer images, the init scripts, asInitDevData and the EDM metadata. With a
 * v2 file it also builds the transfer-queue programs of a solid fill (§9).
 *
 * Unlike the reference, the parser checks every record up front: a section
 * or a patch that would reach past its buffer, an unknown kind or encoding,
 * an image larger than its buffer. After sgx_fw_parse() succeeds, only the
 * layout can make a load fail.
 */
#include "sgx_fw.h"

#define HEADER_SIZE	32
#define SECTION_SIZE	16
#define RECORD_SIZE	16	/* buffer and expression records */
#define FORMAT_V1	1
#define FORMAT_V2	2	/* adds the transfer-queue content, §9 */
#define CODE_PC_BITS	19	/* pc field of a PDS code word, [22:4] */
#define TQ_PAGE		0x1000
#define DOUTU_RANGE	0x80000	/* USE code within 512 KiB of its heap base */

enum { CUT, FACT };
enum { ENC_WORD32, ENC_USSE_LIMM, ENC_CODE_BASE, ENC_EMITPIX_SB };

enum {
	S_IMAGE = 1, S_BUFFERS, S_PATCHES, S_VALUES, S_EDM_META,
	S_TQ_TEMPLATE, S_TQ_PATCHES, S_TQ_META
};

/* Expression kinds: v1 ones below K_NUM, then the transfer-queue ones. */
enum {
	K_CONST, K_ADDR, K_ADDR_SHR4, K_HEAP, K_PD_PHYS, K_CODE_WORD,
	K_PDS_CTL, K_PDS_ROWS, K_NUM,
	K_PARAM = K_NUM, K_DOUTU
};

struct expr {
	u8 buffer;	/* target buffer (PATCHES) */
	u8 enc;
	u8 kind;
	u8 param;	/* CODE_BASE: data master */
	u32 offset;	/* byte offset (PATCHES); register or index (VALUES) */
	u32 target;
	u32 addend;
};

static u16 get16(const u8 *p)
{
	return p[0] | p[1] << 8;
}

static u32 get32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static u64 get64(const u8 *p)
{
	return get32(p) | (u64)get32(p + 4) << 32;
}

static void put32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static void put64(u8 *p, u64 v)
{
	put32(p, v);
	put32(p + 4, v >> 32);
}

/* CRC-32 as zlib computes it: reflected, polynomial 0x04c11db7. */
u32 sgx_fw_crc32(const u8 *p, size_t len)
{
	u32 crc = ~0u;
	int k;

	while (len--) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = crc >> 1 ^ (0xedb88320 & -(crc & 1));
	}
	return ~crc;
}

/*
 * A value scattered over three instruction fields, each (instruction bit,
 * width, value bit). The imm32 of a USSE "mov rN, #imm32":
 */
static const u8 limm_fields[3][3] = { { 0, 21, 0 }, { 36, 5, 21 }, { 44, 6, 26 } };
/* and the 14-bit sideband immediate of emitpix2 (format §9.4): */
static const u8 sb_fields[3][3] = { { 22, 6, 0 }, { 35, 6, 6 }, { 54, 2, 12 } };

static u64 scatter_set(const u8 f[3][3], u64 insn, u32 v)
{
	int i;

	for (i = 0; i < 3; i++) {
		u64 mask = (1ull << f[i][1]) - 1;

		insn &= ~(mask << f[i][0]);
		insn |= (v >> f[i][2] & mask) << f[i][0];
	}
	return insn;
}

static u32 scatter_get(const u8 f[3][3], u64 insn)
{
	u32 v = 0;
	int i;

	for (i = 0; i < 3; i++) {
		u64 mask = (1ull << f[i][1]) - 1;

		v |= (u32)(insn >> f[i][0] & mask) << f[i][2];
	}
	return v;
}

u64 sgx_fw_limm_set(u64 insn, u32 imm)
{
	return scatter_set(limm_fields, insn, imm);
}

u32 sgx_fw_limm_get(u64 insn)
{
	return scatter_get(limm_fields, insn);
}

u64 sgx_fw_emitpix_sb_set(u64 insn, u32 sb)
{
	return scatter_set(sb_fields, insn, sb);
}

u32 sgx_fw_emitpix_sb_get(u64 insn)
{
	return scatter_get(sb_fields, insn);
}

static void expr_at(const struct sgx_fw_exprs *x, u32 n, struct expr *e)
{
	const u8 *r = x->rec + n * RECORD_SIZE;

	e->buffer = r[0];
	e->enc = r[1];
	e->kind = r[2];
	e->param = r[3];
	e->offset = get32(r + 4);
	e->target = get32(r + 8);
	e->addend = get32(r + 12);
}

static int find_buffer(const struct sgx_fw *fw, u32 id)
{
	unsigned int i;

	for (i = 0; i < fw->nbuffers; i++)
		if (fw->buffers[i].id == id)
			return i;
	return -1;
}

const struct sgx_fw_buffer *sgx_fw_buffer(const struct sgx_fw *fw,
					  unsigned int id)
{
	int i = find_buffer(fw, id);

	return i < 0 ? NULL : &fw->buffers[i];
}

static int parse_buffers(struct sgx_fw *fw, const u8 *p, u32 size)
{
	unsigned int i, n = size / RECORD_SIZE;

	if (size % RECORD_SIZE)
		return -EINVAL;
	if (n > SGX_FW_MAX_BUFFERS)
		return -EFBIG;
	fw->nbuffers = 0;
	for (i = 0; i < n; i++, p += RECORD_SIZE) {
		struct sgx_fw_buffer *b = &fw->buffers[i];

		if (p[0] >= SGX_FW_MAX_BUFFERS || find_buffer(fw, p[0]) >= 0)
			return -EINVAL;
		b->id = p[0];
		b->heap = p[1];
		b->pds_rows = p[2];
		b->size = get32(p + 4);
		b->align = get32(p + 8);
		b->image = NULL;
		b->image_size = 0;
		if (!b->align)
			return -EINVAL;
		fw->nbuffers++;
	}
	return 0;
}

/* Check one record; list is SGX_FW_NUM_LISTS for a patch. */
static int check_expr(const struct sgx_fw *fw, const struct expr *e,
		      unsigned int list)
{
	const struct sgx_fw_buffer *b;

	if (e->enc > ENC_CODE_BASE || e->kind >= K_NUM)
		return -EINVAL;
	if (e->enc == ENC_CODE_BASE && e->param > 0x7f)
		return -EINVAL;		/* the data master is 7 bits */
	switch (e->kind) {
	case K_ADDR:
	case K_ADDR_SHR4:
		if (e->target >= SGX_FW_MAX_BUFFERS)
			return -EINVAL;
		break;
	case K_HEAP:
		if (e->target >= SGX_FW_MAX_HEAPS)
			return -EINVAL;
		break;
	case K_CODE_WORD:
		if (e->target >> CODE_PC_BITS)
			return -EINVAL;
		break;
	case K_PDS_CTL:
	case K_PDS_ROWS:
		if (!sgx_fw_buffer(fw, e->target))
			return -EINVAL;
		break;
	}
	if (list == SGX_FW_DEV_DATA && e->offset >= SGX_FW_DEV_DATA_WORDS)
		return -EINVAL;
	if (list == SGX_FW_NUM_LISTS) {
		b = sgx_fw_buffer(fw, e->buffer);
		if (!b || (u64)e->offset + (e->enc == ENC_USSE_LIMM ? 8 : 4) >
			  b->size)
			return -EINVAL;
	}
	return 0;
}

static int check_exprs(const struct sgx_fw *fw, const struct sgx_fw_exprs *x,
		       unsigned int list)
{
	struct expr e;
	u32 i;
	int ret;

	for (i = 0; i < x->count; i++) {
		expr_at(x, i, &e);
		ret = check_expr(fw, &e, list);
		if (ret)
			return ret;
	}
	return 0;
}

static int exprs(struct sgx_fw_exprs *x, const u8 *p, u32 size)
{
	if (size % RECORD_SIZE)
		return -EINVAL;
	x->rec = p;
	x->count = size / RECORD_SIZE;
	return 0;
}

const struct sgx_fw_tq *sgx_fw_tq(const struct sgx_fw *fw, unsigned int id)
{
	if (id >= SGX_FW_TQ_MAX || !(fw->tq_present & 1u << id))
		return NULL;
	return &fw->tq[id];
}

/* TQ_META: one 16-byte record per template; a later section replaces it. */
static int parse_tq_meta(struct sgx_fw *fw, const u8 *p, u32 size)
{
	unsigned int i;

	if (size % RECORD_SIZE)
		return -EINVAL;
	fw->tq_present = 0;
	for (i = 0; i < size / RECORD_SIZE; i++, p += RECORD_SIZE) {
		struct sgx_fw_tq *t;

		if (p[0] >= SGX_FW_TQ_MAX || (fw->tq_present & 1u << p[0]))
			return -EINVAL;
		t = &fw->tq[p[0]];
		*t = (struct sgx_fw_tq){
			.heap = p[1], .temps = p[2], .pds_attrs = p[3],
			.size = get32(p + 4), .align = get32(p + 8),
			.pds_data = get16(p + 12), .flags = p[14],
		};
		if (!t->align || t->heap >= SGX_FW_MAX_HEAPS ||
		    t->pds_data > t->size)
			return -EINVAL;
		fw->tq_present |= 1u << p[0];
	}
	return 0;
}

/*
 * Check one transfer-queue record (format §9.4, §9.5): a TQ_PATCHES record
 * names its template in buffer; a TQ_CMD record a command word in offset.
 */
static int check_tq_expr(const struct sgx_fw *fw, const struct expr *e,
			 bool cmd)
{
	const struct sgx_fw_tq *t;

	if (cmd) {
		if (e->enc != ENC_WORD32 || e->offset % 4 ||
		    (u64)e->offset + 4 >
		    fw->meta[SGX_FW_META_STRUCT_SIZES + 2])
			return -EINVAL;		/* the TRANSFERCMD size */
	} else {
		t = sgx_fw_tq(fw, e->buffer);
		if (!t || (e->enc != ENC_WORD32 && e->enc != ENC_USSE_LIMM &&
			   e->enc != ENC_EMITPIX_SB))
			return -EINVAL;
		if ((u64)e->offset + (e->enc == ENC_WORD32 ? 4 : 8) > t->size)
			return -EINVAL;
	}
	if (e->param)
		return -EINVAL;
	switch (e->kind) {
	case K_CONST:
		return 0;
	case K_ADDR:
	case K_DOUTU:
		return sgx_fw_tq(fw, e->target) ? 0 : -EINVAL;
	case K_HEAP:
		return e->target < SGX_FW_MAX_HEAPS ? 0 : -EINVAL;
	case K_PARAM:
		return e->target < SGX_FW_TQ_NUM_PARAMS ? 0 : -EINVAL;
	}
	return -EINVAL;
}

static int check_tq(const struct sgx_fw *fw)
{
	const struct sgx_fw_exprs *x[2] = {
		&fw->tq_patches, &fw->values[SGX_FW_TQ_CMD]
	};
	struct expr e;
	unsigned int k;
	u32 i;
	int ret;

	for (k = 0; k < 2; k++) {
		for (i = 0; i < x[k]->count; i++) {
			expr_at(x[k], i, &e);
			ret = check_tq_expr(fw, &e, k);
			if (ret)
				return ret;
		}
	}
	return 0;
}

int sgx_fw_parse(struct sgx_fw *fw, const u8 *data, size_t len)
{
	static const char magic[] = "SGX545FW";
	struct sgx_fw_buffer *b;
	unsigned int n, nsec, i;
	int ret, k;

	*fw = (struct sgx_fw){ 0 };
	if (len < HEADER_SIZE)
		return -EINVAL;
	for (i = 0; i < sizeof(magic) - 1; i++)
		if (data[i] != (u8)magic[i])
			return -EINVAL;
	fw->version = get16(data + 8);
	if ((fw->version != FORMAT_V1 && fw->version != FORMAT_V2) ||
	    get32(data + 24) != len)
		return -EINVAL;
	if (sgx_fw_crc32(data + HEADER_SIZE, len - HEADER_SIZE) !=
	    get32(data + 28))
		return -EBADMSG;
	nsec = get16(data + 10);
	fw->ddk_major = data[12];
	fw->ddk_minor = data[13];
	fw->ddk_build = get32(data + 16);
	fw->build_options = get32(data + 20);
	if (HEADER_SIZE + (u64)SECTION_SIZE * nsec > len)
		return -EINVAL;

	/* Images go on in a second pass, once the buffers are known. */
	for (n = 0; n < nsec; n++) {
		const u8 *s = data + HEADER_SIZE + SECTION_SIZE * n;
		u16 type = get16(s), origin = get16(s + 2);
		u32 off = get32(s + 4), size = get32(s + 8), arg = get32(s + 12);
		const u8 *p;

		if (type < S_IMAGE || type > S_TQ_META ||
		    origin != (type == S_IMAGE || type == S_TQ_TEMPLATE ?
			       CUT : FACT))
			return -EINVAL;
		if (fw->version == FORMAT_V1 &&
		    (type >= S_TQ_TEMPLATE ||
		     (type == S_VALUES && arg == SGX_FW_TQ_CMD)))
			return -EINVAL;	/* transfer-queue content needs v2 */
		if ((u64)off + size > len)
			return -EINVAL;
		p = data + off;
		ret = 0;
		switch (type) {
		case S_BUFFERS:
			ret = parse_buffers(fw, p, size);
			break;
		case S_PATCHES:
			ret = exprs(&fw->patches, p, size);
			break;
		case S_VALUES:
			if (arg >= SGX_FW_NUM_LISTS)
				return -EINVAL;
			ret = exprs(&fw->values[arg], p, size);
			break;
		case S_EDM_META:
			if (size != 4 * SGX_FW_META_WORDS)
				return -EINVAL;
			for (i = 0; i < SGX_FW_META_WORDS; i++)
				fw->meta[i] = get32(p + 4 * i);
			break;
		case S_TQ_PATCHES:
			ret = exprs(&fw->tq_patches, p, size);
			break;
		case S_TQ_META:
			ret = parse_tq_meta(fw, p, size);
			break;
		}
		if (ret)
			return ret;
	}
	for (n = 0; n < nsec; n++) {
		const u8 *s = data + HEADER_SIZE + SECTION_SIZE * n;
		struct sgx_fw_tq *t;

		if (get16(s) == S_TQ_TEMPLATE) {
			if (!sgx_fw_tq(fw, get32(s + 12)))
				return -EINVAL;
			t = &fw->tq[get32(s + 12)];
			if (get32(s + 8) > t->size)
				return -EFBIG;
			t->image = data + get32(s + 4);
			t->image_size = get32(s + 8);
			continue;
		}
		if (get16(s) != S_IMAGE)
			continue;
		k = find_buffer(fw, get32(s + 12));
		if (k < 0)
			return -EINVAL;
		b = &fw->buffers[k];
		if (get32(s + 8) > b->size)
			return -EFBIG;
		b->image = data + get32(s + 4);
		b->image_size = get32(s + 8);
	}

	ret = check_exprs(fw, &fw->patches, SGX_FW_NUM_LISTS);
	for (i = 0; !ret && i < SGX_FW_TQ_CMD; i++)
		ret = check_exprs(fw, &fw->values[i], i);
	return ret ? ret : check_tq(fw);
}

/* The ukernel's instruction index from the EDM code base (format §3). */
static int uk_off(const struct sgx_fw_layout *l, u32 *off)
{
	u32 addr = l->addr[SGX_FW_UKERNEL];
	u32 base = l->heap_base[SGX_FW_HEAP_KERNEL_CODE];

	if (!(l->addr_set & 1u << SGX_FW_UKERNEL) ||
	    !(l->heap_set & 1u << SGX_FW_HEAP_KERNEL_CODE))
		return -ENOENT;
	if (addr < base || (addr - base) % 8)
		return -ERANGE;
	*off = (addr - base) / 8;
	return 0;
}

static int evaluate(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		    const struct expr *e, u32 *val)
{
	u64 v = 0;
	u32 off;
	int ret;

	switch (e->kind) {
	case K_CONST:
		v = e->addend;
		break;
	case K_ADDR:
	case K_ADDR_SHR4:
		if (!(l->addr_set & 1u << e->target))
			return -ENOENT;
		/* no wrap before the shift, like the reference */
		v = (u64)l->addr[e->target] + e->addend;
		if (e->kind == K_ADDR_SHR4)
			v >>= 4;
		break;
	case K_HEAP:
		if (!(l->heap_set & 1u << e->target))
			return -ENOENT;
		v = (u64)l->heap_base[e->target] + e->addend;
		break;
	case K_PD_PHYS:
		v = (u64)l->pd_phys + e->addend;
		break;
	case K_CODE_WORD:
		ret = uk_off(l, &off);
		if (ret)
			return ret;
		v = (u64)e->target + off;
		if (v >> CODE_PC_BITS)
			return -ERANGE;
		v = e->addend | v << 4;
		break;
	case K_PDS_CTL:
		v = (u64)sgx_fw_buffer(fw, e->target)->pds_rows << 23 | e->addend;
		break;
	case K_PDS_ROWS:
		v = sgx_fw_buffer(fw, e->target)->pds_rows;
		break;
	default:
		return -EINVAL;
	}
	*val = (u32)v;
	return 0;
}

/* The value as a WORD32 store or a VALUES entry holds it. */
static u32 encode(const struct expr *e, u32 v)
{
	if (e->enc == ENC_CODE_BASE)
		return (v >> 7 & 0x1ffffff) | (u32)e->param << 25;
	return v;
}

int sgx_fw_check(const struct sgx_fw *fw, const struct sgx_fw_layout *l)
{
	const struct sgx_fw_buffer *b;
	unsigned int i;
	u32 off;

	for (i = 0; i < fw->nbuffers; i++) {
		b = &fw->buffers[i];
		if (!(l->addr_set & 1u << b->id))
			return -ENOENT;
		if (l->addr[b->id] % b->align)
			return -ERANGE;
	}
	return uk_off(l, &off);
}

/*
 * Fill every buffer from its image, zero-padded, and apply the patches.
 * bufs[id] must point at size bytes for each buffer the file declares.
 */
int sgx_fw_build(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		 u8 *const bufs[SGX_FW_MAX_BUFFERS])
{
	const struct sgx_fw_buffer *b;
	struct expr e;
	unsigned int i;
	u32 k, v;
	u8 *p;
	int ret;

	ret = sgx_fw_check(fw, l);
	if (ret)
		return ret;
	for (i = 0; i < fw->nbuffers; i++) {
		b = &fw->buffers[i];
		p = bufs[b->id];
		if (!p)
			return -EINVAL;
		for (k = 0; k < b->size; k++)
			p[k] = k < b->image_size ? b->image[k] : 0;
	}
	for (k = 0; k < fw->patches.count; k++) {
		expr_at(&fw->patches, k, &e);
		ret = evaluate(fw, l, &e, &v);
		if (ret)
			return ret;
		p = bufs[e.buffer] + e.offset;
		if (e.enc == ENC_USSE_LIMM)
			put64(p, sgx_fw_limm_set(get64(p), v));
		else
			put32(p, encode(&e, v));
	}
	return 0;
}

/*
 * Evaluate a VALUES list in record order: (register, value) for the scripts,
 * (index, value) for dev data. Returns the number of entries; with out NULL,
 * only counts them.
 */
int sgx_fw_script(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		  enum sgx_fw_list list, struct sgx_fw_write *out,
		  unsigned int max)
{
	const struct sgx_fw_exprs *x;
	struct expr e;
	u32 i, v;
	int ret;

	if ((unsigned int)list >= SGX_FW_TQ_CMD)
		return -EINVAL;		/* TQ_CMD: sgx_fw_tq_cmd() */
	x = &fw->values[list];
	if (!out)
		return x->count;
	if (x->count > max)
		return -ENOSPC;
	ret = sgx_fw_check(fw, l);
	if (ret)
		return ret;
	for (i = 0; i < x->count; i++) {
		expr_at(x, i, &e);
		ret = evaluate(fw, l, &e, &v);
		if (ret)
			return ret;
		out[i].reg = e.offset;
		out[i].val = encode(&e, v);
	}
	return x->count;
}

/* asInitDevData: absent entries are 0, a later record wins. */
int sgx_fw_dev_data(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		    u32 out[SGX_FW_DEV_DATA_WORDS])
{
	const struct sgx_fw_exprs *x = &fw->values[SGX_FW_DEV_DATA];
	struct expr e;
	u32 i, v;
	int ret;

	ret = sgx_fw_check(fw, l);
	if (ret)
		return ret;
	for (i = 0; i < SGX_FW_DEV_DATA_WORDS; i++)
		out[i] = 0;
	for (i = 0; i < x->count; i++) {
		expr_at(x, i, &e);
		ret = evaluate(fw, l, &e, &v);
		if (ret)
			return ret;
		out[e.offset] = encode(&e, v);
	}
	return 0;
}

/*
 * A bump allocator per heap, as srvinit's allocations land in the harness: the
 * first buffer of each heap at base + start, every buffer aligned to at least a
 * page and rounded up to whole pages, in the given order. The caller sets the
 * heap bases; order must name every buffer exactly once. Alignments and the
 * page size must be powers of two.
 */
int sgx_fw_place(const struct sgx_fw_buffer *bufs, unsigned int n,
		 const u8 *order, unsigned int norder, u32 start, u32 page,
		 struct sgx_fw_layout *l)
{
	u64 next[SGX_FW_MAX_HEAPS] = { 0 };
	unsigned int i, j;

	if (norder != n || n > SGX_FW_MAX_BUFFERS)
		return -EINVAL;
	l->addr_set = 0;
	for (i = 0; i < norder; i++) {
		const struct sgx_fw_buffer *b = NULL;
		u64 align, a;

		for (j = 0; j < n; j++)
			if (bufs[j].id == order[i])
				b = &bufs[j];
		if (!b || b->id >= SGX_FW_MAX_BUFFERS ||
		    (l->addr_set & 1u << b->id))
			return -EINVAL;
		if (b->heap >= SGX_FW_MAX_HEAPS || !(l->heap_set & 1u << b->heap))
			return -ENOENT;
		if (!next[b->heap])
			next[b->heap] = (u64)l->heap_base[b->heap] + start;
		align = b->align > page ? b->align : page;
		if (!page || (page & (page - 1)) || (align & (align - 1)))
			return -EINVAL;		/* powers of two only */
		a = (next[b->heap] + align - 1) & ~(align - 1);
		next[b->heap] = a + (((u64)b->size + page - 1) & ~((u64)page - 1));
		if (next[b->heap] > 0x100000000ull)
			return -ERANGE;
		l->addr[b->id] = (u32)a;
		l->addr_set |= 1u << b->id;
	}
	return 0;
}

/* --- format v2: transfer-queue templates (format §9) ---------------------- */

/*
 * The PARAM values of an ARGB8888 fill of a whole linear surface (format
 * §9.6): the colour, then the five PBE state words.
 */
int sgx_fw_tq_fill_params(const struct sgx_fw_tq_fill *f,
			  u32 params[SGX_FW_TQ_NUM_PARAMS])
{
	if (f->width < 1 || f->width > 0x2000 || f->height < 1 ||
	    f->height > 0x2000)
		return -EINVAL;
	if (f->stride % 8 || f->stride < 4 * f->width || f->stride > 0x8000)
		return -EINVAL;
	if (f->dest % 4 || (u64)f->dest + (u64)f->stride * (f->height - 1) +
	    4 * f->width > 0x100000000ull)
		return -EINVAL;
	params[0] = f->colour;
	params[1] = 0;					/* XMIN, YMIN */
	params[2] = (f->width - 1) | (f->height - 1) << 16;
	params[3] = 0x8000 | (f->stride / 8 - 1) << 16;	/* COUNT, LINESTRIDE */
	params[4] = f->dest;				/* FBADDR */
	params[5] = 0x200;				/* TILERELATIVE */
	return 0;
}

/* Each template placed: aligned, in its heap, in one page if it must be. */
int sgx_fw_tq_check(const struct sgx_fw *fw, const struct sgx_fw_layout *l)
{
	const struct sgx_fw_tq *t;
	unsigned int id;
	u32 a;

	if (!fw->tq_present)
		return -ENOENT;
	for (id = 0; id < SGX_FW_TQ_MAX; id++) {
		t = sgx_fw_tq(fw, id);
		if (!t)
			continue;
		if (!(l->tq_set & 1u << id) || !(l->heap_set & 1u << t->heap))
			return -ENOENT;
		a = l->tq_addr[id];
		if (a % t->align || a < l->heap_base[t->heap] ||
		    (u64)a + t->size > 0x100000000ull)
			return -ERANGE;
		if ((t->flags & SGX_FW_TQ_NO_STRADDLE) &&
		    (a & (TQ_PAGE - 1)) + t->size > TQ_PAGE)
			return -ERANGE;
	}
	return 0;
}

static int tq_evaluate(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		       const u32 *params, const struct expr *e, u32 *val)
{
	const struct sgx_fw_tq *t;
	u64 v, off;

	switch (e->kind) {
	case K_CONST:
		v = e->addend;
		break;
	case K_ADDR:
		v = (u64)l->tq_addr[e->target] + e->addend;
		break;
	case K_HEAP:
		if (!(l->heap_set & 1u << e->target))
			return -ENOENT;
		v = (u64)l->heap_base[e->target] + e->addend;
		break;
	case K_PARAM:
		v = (u64)params[e->target] + e->addend;
		break;
	case K_DOUTU:
		/* EXE [19:4], CBASE [3:0], TRC [31:24]: transfer §4.2.4 */
		t = &fw->tq[e->target];
		off = (u64)l->tq_addr[e->target] + e->addend;
		if (off < l->heap_base[t->heap])
			return -ERANGE;
		off -= l->heap_base[t->heap];
		if (off >= DOUTU_RANGE || off % 8)
			return -ERANGE;
		v = (off << 1 & 0xffff0) | (off >> 19 & 0xf) |
		    (u64)((t->temps + 3) >> 2) << 24;
		break;
	default:
		return -EINVAL;
	}
	*val = (u32)v;
	return 0;
}

/*
 * Copy every template into progs[id] (size bytes each), zero-padded, and
 * apply TQ_PATCHES in file order for the given parameters.
 */
int sgx_fw_tq_build(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		    const u32 params[SGX_FW_TQ_NUM_PARAMS],
		    u8 *const progs[SGX_FW_TQ_MAX])
{
	const struct sgx_fw_tq *t;
	unsigned int id;
	struct expr e;
	u32 k, v;
	u8 *p;
	int ret;

	ret = sgx_fw_tq_check(fw, l);
	if (ret)
		return ret;
	for (id = 0; id < SGX_FW_TQ_MAX; id++) {
		t = sgx_fw_tq(fw, id);
		if (!t)
			continue;
		if (!progs[id])
			return -EINVAL;
		for (k = 0; k < t->size; k++)
			progs[id][k] = k < t->image_size ? t->image[k] : 0;
	}
	for (k = 0; k < fw->tq_patches.count; k++) {
		expr_at(&fw->tq_patches, k, &e);
		ret = tq_evaluate(fw, l, params, &e, &v);
		if (ret)
			return ret;
		p = progs[e.buffer] + e.offset;
		if (e.enc == ENC_USSE_LIMM)
			put64(p, sgx_fw_limm_set(get64(p), v));
		else if (e.enc == ENC_EMITPIX_SB)
			put64(p, sgx_fw_emitpix_sb_set(get64(p), v));
		else
			put32(p, v);
	}
	return 0;
}

/*
 * The fixed SGXMKIF_TRANSFERCMD words, (byte offset, value), in file order.
 * Returns their number; with out NULL, only counts them.
 */
int sgx_fw_tq_cmd(const struct sgx_fw *fw, const struct sgx_fw_layout *l,
		  const u32 params[SGX_FW_TQ_NUM_PARAMS],
		  struct sgx_fw_write *out, unsigned int max)
{
	const struct sgx_fw_exprs *x = &fw->values[SGX_FW_TQ_CMD];
	struct expr e;
	u32 i, v;
	int ret;

	if (!out)
		return x->count;
	if (x->count > max)
		return -ENOSPC;
	ret = sgx_fw_tq_check(fw, l);
	if (ret)
		return ret;
	for (i = 0; i < x->count; i++) {
		expr_at(x, i, &e);
		ret = tq_evaluate(fw, l, params, &e, &v);
		if (ret)
			return ret;
		out[i].reg = e.offset;
		out[i].val = v;
	}
	return x->count;
}
