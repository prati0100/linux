// SPDX-License-Identifier: GPL-2.0-only
/*
 * KHO preservation pattern generator for benchmarking.
 *
 * Creates KHO preservations with a controlled physical layout so that the
 * cost of reserving preserved memory in the next kernel can be measured as a
 * function of the number and the layout of preservations.
 *
 * Every block is a naturally aligned range of 2^order pages preserved with a
 * single kho_preserve_pages() call, which results in exactly one key in the
 * KHO preserved memory radix tree. Blocks are carved out of large physically
 * contiguous chunks obtained with alloc_contig_pages(); the pages between
 * blocks are freed again.
 *
 * Parameters (kho_bench.<name>, also /sys/module/kho_bench/parameters/<name>):
 *   pattern    contig | alt | mixed | inverted | random
 *   count      number of blocks (= radix tree keys) to create
 *   order      block order for contig and alt
 *   stride     alt: one block every @stride blocks (>= 1)
 *   max_order  mixed: block orders are random in [0, max_order]
 *   seed       PRNG seed for mixed and random
 *   fill       stamp every word of each page instead of two words per page
 *
 * Patterns (N = count):
 *   contig     N blocks of @order back to back, no gaps
 *   alt        N blocks of @order, block start every @stride << @order pages
 *   mixed      N blocks of random order in [0, @max_order], each followed by
 *              a gap of at least one page
 *   inverted   ceil(N/2) order-0 blocks with one page gaps, then floor(N/2)
 *              order-1 blocks with two page gaps, all of them at higher
 *              physical addresses than the order-0 blocks
 *   random     order-0 blocks; every even page of a chunk is kept with
 *              probability 1/4, so there is always a gap between blocks
 *
 * debugfs (/sys/kernel/debug/kho_bench/):
 *   generate   write 1: create the preservations in this kernel
 *   verify     write 1: restore the preservations handed over by the previous
 *              kernel, check their contents and free them
 *   status     read: state of the generator
 *
 * At boot, the next kernel only looks up the generator metadata (O(1)) so
 * that it does not perturb the boot time measurement.
 */

#define pr_fmt(fmt) "KHO_BENCH: kho_bench: " fmt

#include <linux/debugfs.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/prandom.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "kho_bench."

#define KB_NAME		"kho_bench"
#define KB_META_MAGIC	0x4b42454eU	/* KBEN */
#define KB_META_VERSION	1
#define KB_STAMP	0x4b484f42454e4348ULL

#define KB_CHUNK_ORDER_MAX	18	/* 1 GiB with 4K pages */
#define KB_CHUNK_ORDER_MIN	13	/* 32 MiB with 4K pages */
#define KB_MAX_CHUNKS		2048

enum kb_pattern {
	KB_CONTIG,
	KB_ALT,
	KB_MIXED,
	KB_INVERTED,
	KB_RANDOM,
	KB_NR_PATTERNS,
};

static const char * const kb_pattern_names[KB_NR_PATTERNS] = {
	[KB_CONTIG]	= "contig",
	[KB_ALT]	= "alt",
	[KB_MIXED]	= "mixed",
	[KB_INVERTED]	= "inverted",
	[KB_RANDOM]	= "random",
};

static char pattern[16] = "alt";
module_param_string(pattern, pattern, sizeof(pattern), 0644);
static unsigned long count;
module_param(count, ulong, 0644);
static unsigned int order;
module_param(order, uint, 0644);
static unsigned int stride = 2;
module_param(stride, uint, 0644);
static unsigned int max_order = 10;
module_param(max_order, uint, 0644);
static unsigned int seed = 1;
module_param(seed, uint, 0644);
static bool fill;
module_param(fill, bool, 0644);

/* Handed over to the next kernel. */
struct kb_chunk {
	u64 base_pfn;
	u64 nr_pages;
	u64 nr_blocks;
	u32 phase;		/* inverted: 0 for order-0 part, 1 for order-1 */
	u32 rnd[4];		/* PRNG state at the start of the chunk */
	u32 pad;
};

struct kb_meta {
	u32 magic;
	u32 version;
	u32 pattern;
	u32 order;
	u32 stride;
	u32 max_order;
	u32 seed;
	u32 fill;
	u64 count;
	u64 pages;
	u64 min_pfn;
	u64 max_pfn;
	u32 nr_chunks;
	u32 pad;
	struct kb_chunk chunk[];
};

/* Layout parameters shared by the generator and the verifier. */
struct kb_params {
	enum kb_pattern pattern;
	unsigned int order;
	unsigned int stride;
	unsigned int max_order;
};

struct kb_iter {
	unsigned long pfn;	/* cursor */
	unsigned long end;	/* end of the chunk */
	unsigned int phase;
	struct rnd_state rnd;
};

/* Allocated, still unused chunk. */
struct kb_range {
	unsigned long base_pfn;
	unsigned long nr_pages;
};

static DEFINE_MUTEX(kb_lock);
static bool kb_generated;
static bool kb_found;
static bool kb_verified;
static struct kb_meta *kb_in_meta;

/*
 * Returns the next block of the chunk described by @it, or false if the next
 * block does not fit in the chunk. Blocks are returned in ascending pfn order.
 */
static bool kb_next(const struct kb_params *p, struct kb_iter *it,
		    unsigned long *pfn, unsigned int *ord)
{
	unsigned long start;
	unsigned int o;

	switch (p->pattern) {
	case KB_CONTIG:
	case KB_ALT:
		o = p->order;
		start = ALIGN(it->pfn, 1UL << o);
		if (start + (1UL << o) > it->end)
			return false;
		it->pfn = start + ((p->pattern == KB_ALT ?
				    (unsigned long)p->stride : 1UL) << o);
		break;
	case KB_MIXED:
		o = prandom_u32_state(&it->rnd) % (p->max_order + 1);
		start = ALIGN(it->pfn, 1UL << o);
		if (start + (1UL << o) > it->end)
			return false;
		it->pfn = start + (1UL << o) + 1;
		break;
	case KB_INVERTED:
		o = it->phase;
		start = ALIGN(it->pfn, 1UL << o);
		if (start + (1UL << o) > it->end)
			return false;
		it->pfn = start + (2UL << o);
		break;
	case KB_RANDOM:
		o = 0;
		for (;;) {
			start = ALIGN(it->pfn, 2);
			if (start + 1 > it->end)
				return false;
			it->pfn = start + 2;
			if (!(prandom_u32_state(&it->rnd) & 3))
				break;
		}
		break;
	default:
		return false;
	}

	*pfn = start;
	*ord = o;
	return true;
}

static u64 kb_word(unsigned long pfn, unsigned int i)
{
	return KB_STAMP ^ ((u64)pfn << 12) ^ i;
}

static void kb_stamp(unsigned long pfn, unsigned int ord, bool full)
{
	const unsigned int words = PAGE_SIZE / sizeof(u64);

	for (unsigned long q = pfn; q < pfn + (1UL << ord); q++) {
		u64 *v = page_address(pfn_to_page(q));

		if (full) {
			for (unsigned int i = 0; i < words; i++)
				v[i] = kb_word(q, i);
		} else {
			v[0] = kb_word(q, 0);
			v[words - 1] = kb_word(q, words - 1);
		}
	}
}

/* Returns the pfn of the first bad page or 0 if all pages are intact. */
static unsigned long kb_check(unsigned long pfn, unsigned int ord, bool full)
{
	const unsigned int words = PAGE_SIZE / sizeof(u64);

	for (unsigned long q = pfn; q < pfn + (1UL << ord); q++) {
		const u64 *v = page_address(pfn_to_page(q));

		if (full) {
			for (unsigned int i = 0; i < words; i++)
				if (v[i] != kb_word(q, i))
					return q;
		} else if (v[0] != kb_word(q, 0) ||
			   v[words - 1] != kb_word(q, words - 1)) {
			return q;
		}
	}

	return 0;
}

static int kb_parse_pattern(void)
{
	for (int i = 0; i < KB_NR_PATTERNS; i++)
		if (sysfs_streq(pattern, kb_pattern_names[i]))
			return i;
	return -EINVAL;
}

/* Chunk allocation state of one generate request. */
struct kb_gen {
	unsigned int chunk_order;
	struct kb_range *pool;		/* preallocated chunks, sorted */
	unsigned int nr_pool, pool_next;
	struct kb_range *parked;	/* chunks overlapping KHO bootmem */
	unsigned int nr_parked;
};

static int kb_alloc_chunk(struct kb_gen *g, struct kb_range *r)
{
	while (g->chunk_order >= KB_CHUNK_ORDER_MIN) {
		unsigned long nr = 1UL << g->chunk_order;
		struct page *page = NULL;
		phys_addr_t phys;
		int nid;

		/* alloc_contig_pages() needs a valid preferred node. */
		for_each_node_state(nid, N_MEMORY) {
			page = alloc_contig_pages(nr, GFP_KERNEL | __GFP_NOWARN,
						  nid, NULL);
			if (page)
				break;
		}
		if (!page) {
			g->chunk_order--;
			continue;
		}

		phys = page_to_phys(page);
		if (kho_bootmem_overlap(phys, nr << PAGE_SHIFT)) {
			/*
			 * KHO bootmem cannot hold preservations. Keep the chunk
			 * allocated so it is not returned again and free it
			 * when generation is done.
			 */
			if (g->nr_parked == KB_MAX_CHUNKS) {
				free_contig_range(page_to_pfn(page), nr);
				return -ENOMEM;
			}
			g->parked[g->nr_parked].base_pfn = page_to_pfn(page);
			g->parked[g->nr_parked].nr_pages = nr;
			g->nr_parked++;
			continue;
		}

		r->base_pfn = page_to_pfn(page);
		r->nr_pages = nr;
		return 0;
	}

	return -ENOMEM;
}

static int kb_get_chunk(struct kb_gen *g, struct kb_range *r)
{
	if (g->pool_next < g->nr_pool) {
		*r = g->pool[g->pool_next++];
		return 0;
	}
	if (g->nr_pool)
		pr_warn("inverted: preallocated chunks exhausted, layout may not be fully inverted\n");
	return kb_alloc_chunk(g, r);
}

static int kb_range_cmp(const void *a, const void *b)
{
	const struct kb_range *ra = a, *rb = b;

	return cmp_int(ra->base_pfn, rb->base_pfn);
}

/*
 * For the inverted pattern all order-0 blocks must be below all order-1
 * blocks, so allocate all chunks first and use them in ascending order.
 */
static int kb_prealloc_inverted(struct kb_gen *g, unsigned long n)
{
	unsigned long need = DIV_ROUND_UP(n, 2) * 2 + (n / 2) * 4;
	unsigned long have = 0, largest = 0;
	struct kb_range r;
	int err;

	while (have < need + largest) {
		if (g->nr_pool == KB_MAX_CHUNKS)
			return -ENOMEM;
		err = kb_alloc_chunk(g, &r);
		if (err)
			return err;
		g->pool[g->nr_pool++] = r;
		have += r.nr_pages;
		largest = max(largest, r.nr_pages);
	}

	sort(g->pool, g->nr_pool, sizeof(*g->pool), kb_range_cmp, NULL);
	return 0;
}

/* Undo the preservations of a chunk and free its blocks. */
static void kb_release_chunk(const struct kb_params *p,
			     const struct kb_chunk *c)
{
	struct kb_iter it = {
		.pfn = c->base_pfn,
		.end = c->base_pfn + c->nr_pages,
		.phase = c->phase,
	};
	unsigned long pfn;
	unsigned int ord;

	memcpy(&it.rnd, c->rnd, sizeof(it.rnd));
	for (u64 i = 0; i < c->nr_blocks; i++) {
		if (WARN_ON(!kb_next(p, &it, &pfn, &ord)))
			break;
		kho_unpreserve_pages(pfn_to_page(pfn), 1UL << ord);
		free_contig_range(pfn, 1UL << ord);
	}
}

static int kb_generate(void)
{
	unsigned long left[2], total = 0, pages = 0;
	unsigned long min_pfn = ULONG_MAX, max_pfn = 0;
	struct kb_chunk *chunks = NULL;
	struct kb_meta *meta = NULL;
	unsigned int nr_chunks = 0;
	struct kb_gen g = { .chunk_order = KB_CHUNK_ORDER_MAX };
	struct kb_params p;
	struct rnd_state rnd;
	unsigned int phase = 0;
	size_t meta_size;
	u64 t0 = local_clock();
	int pat, err = 0;

	if (!kho_is_enabled())
		return -EOPNOTSUPP;
	if (kb_generated)
		return -EBUSY;

	pat = kb_parse_pattern();
	if (pat < 0 || !count)
		return -EINVAL;
	if ((pat == KB_CONTIG || pat == KB_ALT) && order > MAX_PAGE_ORDER)
		return -EINVAL;
	if (pat == KB_ALT && !stride)
		return -EINVAL;
	if (pat == KB_MIXED && max_order > MAX_PAGE_ORDER)
		return -EINVAL;

	p = (struct kb_params){
		.pattern = pat,
		.order = order,
		.stride = stride,
		.max_order = max_order,
	};

	if (pat == KB_INVERTED) {
		left[0] = DIV_ROUND_UP(count, 2);
		left[1] = count / 2;
	} else {
		left[0] = count;
		left[1] = 0;
	}

	chunks = kvcalloc(KB_MAX_CHUNKS, sizeof(*chunks), GFP_KERNEL);
	g.pool = kvcalloc(KB_MAX_CHUNKS, sizeof(*g.pool), GFP_KERNEL);
	g.parked = kvcalloc(KB_MAX_CHUNKS, sizeof(*g.parked), GFP_KERNEL);
	if (!chunks || !g.pool || !g.parked) {
		err = -ENOMEM;
		goto out_free_arrays;
	}

	if (pat == KB_INVERTED) {
		err = kb_prealloc_inverted(&g, count);
		if (err)
			goto out_release;
	}

	prandom_seed_state(&rnd, seed);

	while (total < count) {
		struct kb_chunk *c;
		struct kb_iter it;
		struct kb_range r;
		unsigned long cursor, pfn;
		unsigned int ord;

		if (!left[phase])
			phase++;

		if (nr_chunks == KB_MAX_CHUNKS) {
			err = -ENOSPC;
			goto out_release;
		}

		err = kb_get_chunk(&g, &r);
		if (err)
			goto out_release;

		c = &chunks[nr_chunks++];
		c->base_pfn = r.base_pfn;
		c->nr_pages = r.nr_pages;
		c->nr_blocks = 0;
		c->phase = phase;
		memcpy(c->rnd, &rnd, sizeof(c->rnd));

		it = (struct kb_iter){
			.pfn = r.base_pfn,
			.end = r.base_pfn + r.nr_pages,
			.phase = phase,
			.rnd = rnd,
		};
		cursor = r.base_pfn;

		while (left[phase] && kb_next(&p, &it, &pfn, &ord)) {
			if (pfn > cursor)
				free_contig_range(cursor, pfn - cursor);

			kb_stamp(pfn, ord, fill);
			err = kho_preserve_pages(pfn_to_page(pfn), 1UL << ord);
			if (err) {
				/* Not preserved: free it with the chunk tail. */
				cursor = pfn;
				free_contig_range(cursor, it.end - cursor);
				goto out_release;
			}

			cursor = pfn + (1UL << ord);
			c->nr_blocks++;
			left[phase]--;
			total++;
			pages += 1UL << ord;
			min_pfn = min(min_pfn, pfn);
			max_pfn = max(max_pfn, cursor - 1);

			if (!(total & 1023))
				cond_resched();
		}

		/* Carry the PRNG state over to the next chunk. */
		rnd = it.rnd;

		if (cursor < it.end)
			free_contig_range(cursor, it.end - cursor);
	}

	meta_size = struct_size(meta, chunk, nr_chunks);
	meta = kho_alloc_preserve(meta_size);
	if (IS_ERR(meta)) {
		err = PTR_ERR(meta);
		meta = NULL;
		goto out_release;
	}

	*meta = (struct kb_meta){
		.magic = KB_META_MAGIC,
		.version = KB_META_VERSION,
		.pattern = pat,
		.order = order,
		.stride = stride,
		.max_order = max_order,
		.seed = seed,
		.fill = fill,
		.count = total,
		.pages = pages,
		.min_pfn = min_pfn,
		.max_pfn = max_pfn,
		.nr_chunks = nr_chunks,
	};
	memcpy(meta->chunk, chunks, nr_chunks * sizeof(*chunks));

	err = kho_add_subtree(KB_NAME, meta, meta_size);
	if (err) {
		kho_unpreserve_free(meta);
		goto out_release;
	}

	kb_generated = true;
	pr_info("generated pattern=%s count=%lu chunks=%u pages=%lu pfn=[%#lx-%#lx] t=%llums\n",
		kb_pattern_names[pat], total, nr_chunks, pages, min_pfn,
		max_pfn, div_u64(local_clock() - t0, NSEC_PER_MSEC));
	goto out_free_unused;

out_release:
	pr_err("generate pattern=%s count=%lu failed after %lu blocks: %d\n",
	       kb_pattern_names[pat], count, total, err);
	for (unsigned int i = 0; i < nr_chunks; i++)
		kb_release_chunk(&p, &chunks[i]);
out_free_unused:
	for (unsigned int i = g.pool_next; i < g.nr_pool; i++)
		free_contig_range(g.pool[i].base_pfn, g.pool[i].nr_pages);
	for (unsigned int i = 0; i < g.nr_parked; i++)
		free_contig_range(g.parked[i].base_pfn, g.parked[i].nr_pages);
out_free_arrays:
	kvfree(g.parked);
	kvfree(g.pool);
	kvfree(chunks);
	return err;
}

static bool kb_meta_valid(const struct kb_meta *m, size_t size)
{
	if (size < sizeof(*m) || m->magic != KB_META_MAGIC ||
	    m->version != KB_META_VERSION || m->pattern >= KB_NR_PATTERNS ||
	    m->nr_chunks > KB_MAX_CHUNKS)
		return false;
	return size >= struct_size(m, chunk, m->nr_chunks);
}

static int kb_verify(void)
{
	unsigned long blocks = 0, bad = 0, restore_fail = 0;
	unsigned long first_bad_pfn = 0;
	const char *reason = NULL;
	struct kb_meta *m = kb_in_meta;
	struct kb_params p;
	u64 pages = 0, t0 = local_clock();
	u64 expect_count, expect_pages;
	const char *name;
	bool full;

	if (!kb_found) {
		pr_info("VERIFY FAIL reason=no-handover-data\n");
		return -ENOENT;
	}
	if (kb_verified)
		return -EALREADY;
	kb_verified = true;

	p = (struct kb_params){
		.pattern = m->pattern,
		.order = m->order,
		.stride = m->stride,
		.max_order = m->max_order,
	};
	full = m->fill;
	name = kb_pattern_names[m->pattern];
	expect_count = m->count;
	expect_pages = m->pages;

	for (u32 i = 0; i < m->nr_chunks; i++) {
		const struct kb_chunk *c = &m->chunk[i];
		struct kb_iter it = {
			.pfn = c->base_pfn,
			.end = c->base_pfn + c->nr_pages,
			.phase = c->phase,
		};
		unsigned long pfn, bad_pfn;
		unsigned int ord;

		memcpy(&it.rnd, c->rnd, sizeof(it.rnd));
		for (u64 j = 0; j < c->nr_blocks; j++) {
			struct page *page;

			if (!kb_next(&p, &it, &pfn, &ord)) {
				reason = reason ?: "layout-mismatch";
				bad++;
				break;
			}

			page = kho_restore_pages(PFN_PHYS(pfn), 1UL << ord);
			if (!page) {
				reason = reason ?: "restore-failed";
				first_bad_pfn = first_bad_pfn ?: pfn;
				restore_fail++;
				continue;
			}

			bad_pfn = kb_check(pfn, ord, full);
			if (bad_pfn) {
				reason = reason ?: "content-mismatch";
				first_bad_pfn = first_bad_pfn ?: bad_pfn;
				bad++;
			}

			free_contig_range(pfn, 1UL << ord);
			blocks++;
			pages += 1UL << ord;
			if (!(blocks & 1023))
				cond_resched();
		}
	}

	if (!reason && (blocks != expect_count || pages != expect_pages))
		reason = "count-mismatch";

	kho_restore_free(m);
	kb_in_meta = NULL;

	if (reason) {
		pr_info("VERIFY FAIL pattern=%s reason=%s expected_blocks=%llu blocks_ok=%lu bad=%lu restore_failed=%lu first_bad_pfn=%#lx\n",
			name, reason, expect_count, blocks - bad, bad,
			restore_fail, first_bad_pfn);
		return -EIO;
	}

	pr_info("VERIFY PASS pattern=%s count=%llu blocks=%lu pages=%llu full=%d t=%llums\n",
		name, expect_count, blocks, pages, full,
		div_u64(local_clock() - t0, NSEC_PER_MSEC));
	return 0;
}

static ssize_t kb_trigger(const char __user *ubuf, size_t len,
			  int (*fn)(void))
{
	bool val;
	int err;

	err = kstrtobool_from_user(ubuf, len, &val);
	if (err)
		return err;
	if (!val)
		return len;

	mutex_lock(&kb_lock);
	err = fn();
	mutex_unlock(&kb_lock);

	return err ? err : len;
}

static ssize_t kb_generate_write(struct file *file, const char __user *ubuf,
				 size_t len, loff_t *ppos)
{
	return kb_trigger(ubuf, len, kb_generate);
}

static ssize_t kb_verify_write(struct file *file, const char __user *ubuf,
			       size_t len, loff_t *ppos)
{
	return kb_trigger(ubuf, len, kb_verify);
}

static const struct file_operations kb_generate_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = kb_generate_write,
	.llseek = default_llseek,
};

static const struct file_operations kb_verify_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = kb_verify_write,
	.llseek = default_llseek,
};

static int kb_status_show(struct seq_file *s, void *unused)
{
	mutex_lock(&kb_lock);
	seq_printf(s, "generated=%d found=%d verified=%d\n", kb_generated,
		   kb_found, kb_verified);
	if (kb_in_meta)
		seq_printf(s, "in: pattern=%s count=%llu pages=%llu chunks=%u fill=%u\n",
			   kb_pattern_names[kb_in_meta->pattern],
			   kb_in_meta->count, kb_in_meta->pages,
			   kb_in_meta->nr_chunks, kb_in_meta->fill);
	mutex_unlock(&kb_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kb_status);

static int __init kho_bench_init(void)
{
	struct dentry *dir;
	phys_addr_t phys;
	size_t size;

	if (!kho_is_enabled())
		return 0;

	/* O(1) at boot: only look up the metadata handed over by kernel A. */
	if (!kho_retrieve_subtree(KB_NAME, &phys, &size)) {
		struct kb_meta *m = phys_to_virt(phys);

		if (kb_meta_valid(m, size)) {
			kb_in_meta = m;
			kb_found = true;
			pr_info("found pattern=%s count=%llu pages=%llu chunks=%u\n",
				kb_pattern_names[m->pattern], m->count,
				m->pages, m->nr_chunks);
		} else {
			pr_err("invalid handover metadata\n");
		}
	}

	dir = debugfs_create_dir(KB_NAME, NULL);
	debugfs_create_file("generate", 0200, dir, NULL, &kb_generate_fops);
	debugfs_create_file("verify", 0200, dir, NULL, &kb_verify_fops);
	debugfs_create_file("status", 0444, dir, NULL, &kb_status_fops);

	return 0;
}
late_initcall(kho_bench_init);
