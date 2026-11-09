// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Copyright (C) 2025 Pratyush Yadav <pratyush@kernel.org>
 */

/* The documentation for this is in mm/memfd_luo.c */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cleanup.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/liveupdate.h>
#include <linux/kexec_handover.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>
#include <linux/math64.h>
#include <linux/migrate.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/vmstat.h>
#include <linux/kho/abi/hugetlb.h>
#include <linux/kho_map.h>
#include <linux/spinlock.h>
#include <linux/memfd.h>
#include <uapi/linux/memfd.h>

#include "hugetlb_internal.h"
#include "hugetlb_vmemmap.h"
#include "internal.h"

struct hugetlb_flb_obj {
	/* Serializes access to ser and its hstates. */
	spinlock_t lock;
	struct hugetlb_ser *ser;
};

static int hugetlb_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_flb_obj *obj;
	u8 nr_hstates = 0;
	struct hstate *h;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	hugetlb_ser = kho_alloc_preserve(sizeof(*hugetlb_ser));
	if (IS_ERR(hugetlb_ser)) {
		kfree(obj);
		return PTR_ERR(hugetlb_ser);
	}

	spin_lock_init(&obj->lock);
	obj->ser = hugetlb_ser;

	for_each_hstate(h) {
		struct hugetlb_hstate_ser *hser = &hugetlb_ser->hstates[nr_hstates];

		hser->nr_pages = 0;
		hser->order = h->order;
		nr_hstates++;
	}

	hugetlb_ser->nr_hstates = nr_hstates;

	args->obj = obj;
	args->data = virt_to_phys(hugetlb_ser);

	return 0;
}

static void hugetlb_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	kho_unpreserve_free(phys_to_virt(args->data));
	kfree(args->obj);
}

static void hugetlb_flb_finish(struct liveupdate_flb_op_args *args)
{
	kho_restore_free(args->obj);
}

static int hugetlb_flb_retrieve(struct liveupdate_flb_op_args *args)
{
	/*
	 * There is nothing to deserialize. Just return the pointer to the
	 * serialization structure. It can be used directly.
	 */
	args->obj = phys_to_virt(args->data);
	return 0;
}

static const struct liveupdate_flb_ops hugetlb_luo_flb_ops = {
	.preserve = hugetlb_flb_preserve,
	.unpreserve = hugetlb_flb_unpreserve,
	.finish = hugetlb_flb_finish,
	.retrieve = hugetlb_flb_retrieve,
	.owner = THIS_MODULE,
};

static struct liveupdate_flb hugetlb_luo_flb = {
	.ops = &hugetlb_luo_flb_ops,
	.compatible = HUGETLB_FLB_COMPATIBLE,
};

static struct hugetlb_hstate_ser
*hugetlb_flb_get_hser(struct hugetlb_ser *hugetlb_ser, unsigned int order)
{
	for (u8 i = 0; i < hugetlb_ser->nr_hstates; i++) {
		if (hugetlb_ser->hstates[i].order == order)
			return &hugetlb_ser->hstates[i];
	}

	return NULL;
}

static int hugetlb_flb_add_folio(struct hstate *h)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_hstate_ser *hser;
	struct hugetlb_flb_obj *obj;
	int err = 0;

	err = liveupdate_flb_get_outgoing(&hugetlb_luo_flb, (void **)&obj);
	if (err)
		return err;

	hugetlb_ser = obj->ser;

	scoped_guard(spinlock, &obj->lock) {
		hser = hugetlb_flb_get_hser(hugetlb_ser, h->order);
		if (!hser)
			err = -ENOENT;
		else
			hser->nr_pages++;
	}

	liveupdate_flb_put_outgoing(&hugetlb_luo_flb);
	return err;
}

static int hugetlb_flb_del_folio(struct hstate *h)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_hstate_ser *hser;
	struct hugetlb_flb_obj *obj;
	int err = 0;

	err = liveupdate_flb_get_outgoing(&hugetlb_luo_flb, (void **)&obj);
	if (err)
		return err;

	hugetlb_ser = obj->ser;

	scoped_guard(spinlock, &obj->lock) {
		hser = hugetlb_flb_get_hser(hugetlb_ser, h->order);
		if (!hser)
			err = -ENOENT;
		else
			hser->nr_pages--;
	}

	liveupdate_flb_put_outgoing(&hugetlb_luo_flb);
	return err;
}

unsigned long __init hstate_liveupdate_pages(struct hstate *h)
{
	struct hugetlb_hstate_ser *hser;
	struct hugetlb_ser *hugetlb_ser;
	unsigned long nr_pages;
	int err;

	err = liveupdate_flb_get_incoming(&hugetlb_luo_flb, (void **)&hugetlb_ser);
	if (err)
		/* If FLB can't be fetched, assume no pages from liveupdate. */
		return 0;

	/* NOTE: No need for locking since this is read-only on incoming side. */
	hser = hugetlb_flb_get_hser(hugetlb_ser, h->order);
	nr_pages = hser ? hser->nr_pages : 0;

	liveupdate_flb_put_incoming(&hugetlb_luo_flb);
	return nr_pages;
}

static bool hugetlb_luo_can_preserve(struct liveupdate_file_handler *handler,
				     struct file *file)
{
	/*
	 * Only memfds for now. Files on mounts that userspace created can't be
	 * restored correctly yet.
	 */
	return is_file_hugetlbfs(file) &&
	       hugetlbfs_inode_is_internal(file_inode(file));
}

static void hugetlb_luo_unpreserve_folio(struct hstate *h, struct folio *folio)
{
	hugetlb_flb_del_folio(h);
	kho_unpreserve_folio(folio);
}

/* Preserve @folio and record it in @map, keyed by its hugepage index. */
static int hugetlb_luo_preserve_folio(struct hstate *h, struct folio *folio,
				      struct kho_map *map)
{
	union hugetlb_luo_folio_ser entry = {};
	int err;

	err = kho_preserve_folio(folio);
	if (err)
		return err;

	err = hugetlb_flb_add_folio(h);
	if (err)
		goto err_unpreserve;

	entry.pfn = folio_pfn(folio);
	err = kho_map_set(map, folio->index >> huge_page_order(h), entry.val);
	if (err)
		goto err_flb_del;

	return 0;

err_flb_del:
	hugetlb_flb_del_folio(h);
err_unpreserve:
	kho_unpreserve_folio(folio);
	return err;
}

/* Unpreserve and put all folios in @map, and destroy the map. */
static void hugetlb_luo_unpreserve_folios(struct hstate *h,
					  struct kho_map *map)
{
	union hugetlb_luo_folio_ser entry;
	u64 index;

	kho_map_for_each(map, index, entry.val) {
		struct folio *folio = pfn_folio(entry.pfn);

		hugetlb_luo_unpreserve_folio(h, folio);
		folio_put(folio);
	}

	kho_map_destroy(map);
}

/* How many times to retry isolating a folio someone else has isolated. */
#define HUGETLB_LUO_MIGRATE_RETRIES	3

/*
 * Move a folio out of ZONE_MOVABLE or CMA, so it can be held until the next
 * kernel. Called with a reference on the unlocked folio, which is always
 * dropped. The caller must look up the folio again afterwards.
 *
 * Returns 0 on success, -EBUSY if the folio could not be isolated (e.g.
 * because someone else has it isolated right now), or -ENOMEM if migration
 * failed.
 */
static int hugetlb_luo_migrate_folio(struct folio *folio)
{
	struct migration_target_control mtc = {
		.nid = NUMA_NO_NODE,
		/* Not movable, so the target is neither in ZONE_MOVABLE nor CMA. */
		.gfp_mask = GFP_USER | __GFP_NOWARN,
		.reason = MR_LONGTERM_PIN,
	};
	LIST_HEAD(source);
	bool isolated;

	isolated = folio_isolate_hugetlb(folio, &source);
	/* Isolation holds its own reference. */
	folio_put(folio);
	if (!isolated)
		return -EBUSY;

	if (migrate_pages(&source, alloc_migration_target, NULL,
			  (unsigned long)&mtc, MIGRATE_SYNC, MR_LONGTERM_PIN,
			  NULL)) {
		putback_movable_pages(&source);
		return -ENOMEM;
	}

	return 0;
}

/*
 * Look up the folio at hugepage index @idx, allocating it if needed, and make
 * sure it can be held until the next kernel. Folios that are not longterm
 * pinnable are migrated out first, retrying a few times if someone else has
 * them isolated.
 *
 * On success, returns 0 with *@foliop locked and holding a reference.
 */
static int hugetlb_luo_get_folio(struct file *file, pgoff_t idx,
				 struct folio **foliop)
{
	struct hstate *h = hstate_file(file);
	int err, busy_retries = 0;
	struct folio *folio;

	for (;;) {
		folio = filemap_lock_hugetlb_folio(h, file->f_mapping, idx);
		if (IS_ERR(folio)) {
			if (PTR_ERR(folio) != -ENOENT)
				return PTR_ERR(folio);

			/*
			 * Fill the hole, so a fault can't add a folio that is
			 * not preserved later. The new folio is unlocked, so
			 * look it up again. -EEXIST means someone else filled
			 * it in the meantime.
			 */
			folio = memfd_alloc_folio(file, idx << huge_page_order(h));
			if (IS_ERR(folio)) {
				if (PTR_ERR(folio) != -EEXIST)
					return PTR_ERR(folio);
			} else {
				folio_put(folio);
			}
			continue;
		}

		if (likely(folio_is_longterm_pinnable(folio)))
			break;

		folio_unlock(folio);
		err = hugetlb_luo_migrate_folio(folio);
		/* Someone else might have it isolated, retry a bit. */
		if (err == -EBUSY && ++busy_retries < HUGETLB_LUO_MIGRATE_RETRIES)
			continue;
		if (err)
			return err;
	}

	*foliop = folio;
	return 0;
}

/*
 * Preserve all folios of @file and record them in @map. Each preserved folio
 * holds a reference until it is unpreserved.
 */
static int hugetlb_luo_preserve_folios(struct file *file,
				       struct kho_map *map)
{
	struct inode *inode = file_inode(file);
	struct hstate *h = hstate_inode(inode);
	unsigned int pin_flags;
	pgoff_t idx, nr_hpages;
	int err = 0;

	/*
	 * Round up because fallocate() allows unaligned lengths while still
	 * allocating a full hugepage for the tail. If the file has zero size,
	 * then the folios map is left empty.
	 */
	nr_hpages = DIV_ROUND_UP_ULL(i_size_read(inode), huge_page_size(h));

	/*
	 * The folios are held until the next kernel, so keep newly allocated
	 * ones out of ZONE_MOVABLE and CMA -- which also ensures they don't
	 * fall in KHO bootmem. Existing ones are migrated by
	 * hugetlb_luo_get_folio().
	 *
	 * A side effect of this is that it allocates a folio for all indices in
	 * the file. This might waste memory on sparse memfds. If that is really
	 * a problem in the future, this loop can be taught to skip empty slots.
	 */
	pin_flags = memalloc_pin_save();
	for (idx = 0; idx < nr_hpages; idx++) {
		struct folio *folio;

		cond_resched();

		if (fatal_signal_pending(current)) {
			err = -EINTR;
			break;
		}

		err = hugetlb_luo_get_folio(file, idx, &folio);
		if (err)
			break;

		err = hugetlb_luo_preserve_folio(h, folio, map);
		folio_unlock(folio);
		if (err) {
			folio_put(folio);
			break;
		}
	}
	memalloc_pin_restore(pin_flags);

	if (err)
		hugetlb_luo_unpreserve_folios(h, map);

	return err;
}

/* Read the seals of @file, making sure this version can preserve them. */
static int hugetlb_luo_get_seals(struct file *file, u32 *sealsp)
{
	int seals = memfd_get_seals(file);

	if (seals < 0)
		return seals;

	if (seals & ~HUGETLB_LUO_ALL_SEALS)
		return -EOPNOTSUPP;

	*sealsp = seals;
	return 0;
}

static int hugetlb_luo_preserve(struct liveupdate_file_op_args *args)
{
	struct file *file = args->file;
	struct inode *inode = file_inode(file);
	struct hstate *h = hstate_inode(inode);
	struct hugetlb_luo_ser *memfd_ser;
	struct kho_map *folios;
	int err;

	/*
	 * Preserving stops everyone from resizing the file or punching holes
	 * in it until it is unpreserved. Only allow it to those who could write
	 * to the file themselves.
	 */
	err = inode_permission(file_mnt_idmap(file), inode, MAY_WRITE);
	if (err)
		return err;

	memfd_ser = kho_alloc_preserve(sizeof(*memfd_ser));
	if (IS_ERR(memfd_ser))
		return PTR_ERR(memfd_ser);

	/*
	 * The runtime handle of the folios map is needed until unpreserve(),
	 * so keep it in private_data.
	 */
	folios = kmalloc_obj(*folios);
	if (!folios) {
		err = -ENOMEM;
		goto err_free_memfd_ser;
	}

	kho_map_init(folios, &memfd_ser->folios);
	/*
	 * Preserve the empty tree right away, so every node allocated for the
	 * tree is auto-preserved.
	 */
	err = kho_map_preserve(folios);
	if (err)
		goto err_free_folios;

	inode_lock(inode);

	err = hugetlb_luo_get_seals(file, &memfd_ser->seals);
	if (err)
		goto err_unlock;

	hugetlb_i_freeze(inode, true);

	memfd_ser->size = i_size_read(inode);
	memfd_ser->pos = file->f_pos;
	memfd_ser->order = h->order;

	err = hugetlb_luo_preserve_folios(file, folios);
	if (err)
		goto err_unlock;

	inode_unlock(inode);

	args->serialized_data = virt_to_phys(memfd_ser);
	args->private_data = folios;
	return 0;

err_unlock:
	hugetlb_i_freeze(inode, false);
	inode_unlock(inode);
err_free_folios:
	kfree(folios);
err_free_memfd_ser:
	kho_unpreserve_free(memfd_ser);
	return err;
}

static void hugetlb_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	struct hugetlb_luo_ser *memfd_ser = phys_to_virt(args->serialized_data);
	struct inode *inode = file_inode(args->file);
	struct hstate *h = hstate_inode(inode);

	inode_lock(inode);
	hugetlb_luo_unpreserve_folios(h, args->private_data);
	kfree(args->private_data);
	hugetlb_i_freeze(inode, false);
	kho_unpreserve_free(memfd_ser);
	inode_unlock(inode);
}

static int hugetlb_luo_freeze(struct liveupdate_file_op_args *args)
{
	struct hugetlb_luo_ser *memfd_ser = phys_to_virt(args->serialized_data);

	/*
	 * The pos and seals might have changed since preserve. Everything else
	 * stays the same.
	 */
	memfd_ser->pos = args->file->f_pos;
	return hugetlb_luo_get_seals(args->file, &memfd_ser->seals);
}

/* Look up the hstate for @order, or return NULL if this kernel has none. */
static struct hstate *hugetlb_luo_order_hstate(unsigned int order)
{
	/* Don't overflow. */
	if (order >= BITS_PER_LONG - PAGE_SHIFT)
		return NULL;

	return size_to_hstate(PAGE_SIZE << order);
}

/*
 * Restore a preserved folio and prepare it to be added to the hstate of @order.
 * The returned folio has a frozen refcount.
 */
static struct folio *
hugetlb_luo_retrieve_folio(union hugetlb_luo_folio_ser entry, unsigned int order)
{
	struct folio *folio;

	if (entry.flags) {
		pr_warn("unexpected flags 0x%x on PFN 0x%llx\n",
			(unsigned int)entry.flags, (u64)entry.pfn);
		return NULL;
	}

	folio = kho_restore_folio(PFN_PHYS(entry.pfn));
	if (!folio)
		return NULL;

	if (folio_order(folio) != order) {
		pr_warn("unexpected order %u on PFN 0x%lx, expected %u\n",
			folio_order(folio), folio_pfn(folio), order);
		folio_put(folio);
		return NULL;
	}

	if (!folio_ref_freeze(folio, 1)) {
		pr_warn("unexpected refcount on PFN 0x%lx\n", folio_pfn(folio));
		return NULL;
	}

	init_new_hugetlb_folio(folio);
	return folio;
}

/*
 * Restore the folios in @map with keys starting from @start and return them to
 * the hstate. @map must already be restored.
 */
static void hugetlb_luo_discard_folios(struct kho_map *map, u64 start,
				       u8 order)
{
	union hugetlb_luo_folio_ser entry;
	LIST_HEAD(folio_list);
	struct hstate *h;
	u64 index;

	if (kho_map_next(map, start, &entry.val) == U64_MAX)
		return;

	h = hugetlb_luo_order_hstate(order);
	if (!h) {
		pr_warn("no hstate found for order %u\n", order);
		kho_map_for_each_from(map, index, entry.val, start) {
			struct folio *folio;

			folio = kho_restore_folio(PFN_PHYS(entry.pfn));
			if (folio)
				folio_put(folio);
		}
		return;
	}

	/* Return the folios back to the hstate. */
	kho_map_for_each_from(map, index, entry.val, start) {
		struct folio *folio = hugetlb_luo_retrieve_folio(entry, order);

		if (folio)
			list_add(&folio->lru, &folio_list);
	}

	prep_and_add_allocated_folios(h, &folio_list);
}

/*
 * Drop the references retrieve() holds on the folios added to the file, for
 * keys below @end.
 */
static void hugetlb_luo_put_retrieved_folios(struct kho_map *map, u64 end)
{
	union hugetlb_luo_folio_ser entry;
	u64 index;

	kho_map_for_each(map, index, entry.val) {
		if (index >= end)
			break;
		folio_put(pfn_folio(entry.pfn));
	}
}

static void hugetlb_luo_finish(struct liveupdate_file_op_args *args)
{
	struct hugetlb_luo_ser *memfd_ser;

	/* If retrieve failed, it already cleaned up everything it could. */
	if (args->retrieve_status < 0)
		return;

	if (!args->serialized_data)
		return;

	memfd_ser = phys_to_virt(args->serialized_data);

	if (args->retrieve_status > 0) {
		/*
		 * Retrieve was successful, release the folios and unfreeze the
		 * file so it can be used normally. retrieve() already restored
		 * the map and kept its handle in private_data.
		 */
		struct inode *inode = file_inode(args->file);
		struct kho_map *folios = args->private_data;

		inode_lock(inode);
		hugetlb_luo_put_retrieved_folios(folios, U64_MAX);
		hugetlb_i_freeze(inode, false);
		inode_unlock(inode);

		kho_map_destroy(folios);
		kfree(folios);
	} else {
		/* Retrieve was not called, discard the preserved folios. */
		struct kho_map folios;

		if (kho_map_restore(&folios, &memfd_ser->folios)) {
			pr_warn("unable to restore folios map\n");
			goto out;
		}

		hugetlb_luo_discard_folios(&folios, 0, memfd_ser->order);
		kho_map_destroy(&folios);
	}

out:
	kho_restore_free(memfd_ser);
}

/* Add the retrieved folios to the hstate as in-use folios. */
static void hugetlb_luo_add_folios(struct hstate *h,
				   struct list_head *folio_list)
{
	unsigned long flags;
	struct folio *folio, *tmp_f;

	hugetlb_vmemmap_optimize_folios(h, folio_list);

	spin_lock_irqsave(&hugetlb_lock, flags);
	list_for_each_entry_safe(folio, tmp_f, folio_list, lru) {
		account_new_hugetlb_folio(h, folio);
		folio_clear_hugetlb_freed(folio);
		folio_ref_unfreeze(folio, 1);
		list_move(&folio->lru, &h->hugepage_activelist);
	}
	spin_unlock_irqrestore(&hugetlb_lock, flags);
}

/*
 * Free a folio that is in the hstate but not in the page cache. Its NR_HUGETLB
 * was not counted, but free_huge_folio() decrements it unconditionally.
 */
static void hugetlb_luo_free_uncached_folio(struct hstate *h,
					    struct folio *folio)
{
	lruvec_stat_mod_folio(folio, NR_HUGETLB, pages_per_huge_page(h));
	folio_put(folio);
}

/*
 * Charge a retrieved folio, add it to the page cache, and record its index in
 * the reservation map. This follows what alloc_hugetlb_folio() and
 * hugetlbfs_fallocate() do for an index without a reservation: the cgroup
 * charges are committed to the folio, and the region is added without cgroup
 * info.
 *
 * The region is only added once the folio is in the page cache, so every
 * region has a folio behind it and releasing the file does not touch the
 * global reserve count (h->resv_huge_pages). The memfd comes from the internal
 * hugetlbfs mount, which has no subpool, so there is no subpool accounting
 * either.
 *
 * On success, the folio is locked and the caller must unlock it. On error, the
 * folio is not in the page cache and NR_HUGETLB is not counted, so the caller
 * can free it with hugetlb_luo_free_uncached_folio().
 */
static int hugetlb_luo_add_folio_to_file(struct inode *inode,
					 struct folio *folio, pgoff_t idx)
{
	struct hstate *h = hstate_inode(inode);
	struct resv_map *resv_map = inode_resv_map(inode);
	gfp_t gfp = htlb_alloc_mask(h) | __GFP_RETRY_MAYFAIL;
	struct hugetlb_cgroup *h_cg = NULL, *h_cg_rsvd = NULL;
	unsigned long nr_pages = pages_per_huge_page(h);
	long chg, regions_needed;
	int hidx = hstate_index(h);
	int err;

	chg = region_chg(resv_map, idx, idx + 1, &regions_needed);
	if (chg < 0)
		return chg;

	/*
	 * The index is already in the file. Can't happen since every index is
	 * in the map only once and the file is new.
	 */
	if (WARN_ON_ONCE(!chg)) {
		err = -EINVAL;
		goto err_region_abort;
	}

	if (hugetlb_cgroup_charge_cgroup_rsvd(hidx, nr_pages, &h_cg_rsvd)) {
		err = -ENOSPC;
		goto err_region_abort;
	}

	if (hugetlb_cgroup_charge_cgroup(hidx, nr_pages, &h_cg)) {
		hugetlb_cgroup_uncharge_cgroup_rsvd(hidx, nr_pages, h_cg_rsvd);
		err = -ENOSPC;
		goto err_region_abort;
	}

	spin_lock_irq(&hugetlb_lock);
	hugetlb_cgroup_commit_charge(hidx, nr_pages, h_cg, folio);
	hugetlb_cgroup_commit_charge_rsvd(hidx, nr_pages, h_cg_rsvd, folio);
	spin_unlock_irq(&hugetlb_lock);

	err = mem_cgroup_charge_hugetlb(folio, gfp);
	if (err)
		goto err_region_abort;

	__folio_mark_uptodate(folio);
	err = hugetlb_add_to_page_cache(folio, inode->i_mapping, idx);
	if (err)
		goto err_region_abort;

	lruvec_stat_mod_folio(folio, NR_HUGETLB, nr_pages);

	/* Can't fail since region_chg() already allocated the entries. */
	chg = region_add(resv_map, idx, idx + 1, regions_needed, NULL, NULL);
	WARN_ON_ONCE(chg != 1);
	return 0;

err_region_abort:
	region_abort(resv_map, idx, idx + 1, regions_needed);
	return err;
}

/*
 * @map must already be restored. It stays around until finish(), which drops
 * the references held on the folios.
 *
 * On error, all the folios are released. The ones already added to the page
 * cache are freed when the file is released.
 */
static int hugetlb_luo_retrieve_folios(struct file *file,
				       struct kho_map *map)
{
	struct inode *inode = file_inode(file);
	struct hstate *h = hstate_inode(inode);
	unsigned int order = huge_page_order(h);
	u64 nr_hpages = DIV_ROUND_UP_ULL(i_size_read(inode), huge_page_size(h));
	union hugetlb_luo_folio_ser entry;
	struct folio *folio;
	LIST_HEAD(list);
	u64 index, failed;
	int err;

	/*
	 * Make sure every folio lands inside the file, so releasing the file
	 * finds and frees all of them.
	 */
	kho_map_for_each(map, index, entry.val) {
		if (index >= nr_hpages) {
			pr_err("invalid folio index %llu\n", index);
			hugetlb_luo_discard_folios(map, 0, order);
			return -EINVAL;
		}
	}

	/* First prepare the folios and add them to the hstate. */
	kho_map_for_each(map, index, entry.val) {
		folio = hugetlb_luo_retrieve_folio(entry, order);
		if (!folio) {
			err = -EINVAL;
			goto err_discard;
		}

		list_add(&folio->lru, &list);
	}

	hugetlb_luo_add_folios(h, &list);

	/* Now that all the folios are prepared, add them to the file. */
	kho_map_for_each(map, index, entry.val) {
		folio = pfn_folio(entry.pfn);

		err = hugetlb_luo_add_folio_to_file(inode, folio, index);
		if (err) {
			pr_err("failed to add folio to file: %pe\n", ERR_PTR(err));
			goto err_release;
		}

		folio_set_hugetlb_migratable(folio);
		folio_unlock(folio);
		/*
		 * Keep the refcount on folio elevated. It is dropped in
		 * finish().
		 */
	}

	return 0;

err_release:
	failed = index;
	hugetlb_luo_put_retrieved_folios(map, failed);
	kho_map_for_each_from(map, index, entry.val, failed)
		hugetlb_luo_free_uncached_folio(h, pfn_folio(entry.pfn));
	return err;

err_discard:
	/* The folio at index was not restored. Discard the ones after it. */
	prep_and_add_allocated_folios(h, &list);
	hugetlb_luo_discard_folios(map, index + 1, order);
	return err;
}

static int hugetlb_luo_retrieve(struct liveupdate_file_op_args *args)
{
	struct hugetlb_luo_ser *memfd_ser;
	struct kho_map *folios;
	unsigned int flags;
	struct file *file;
	int err;

	if (!args->serialized_data)
		return -EINVAL;

	memfd_ser = phys_to_virt(args->serialized_data);

	/*
	 * The runtime handle of the folios map is needed until finish(), so
	 * keep it in private_data.
	 */
	folios = kmalloc_obj(*folios);
	if (!folios) {
		err = -ENOMEM;
		goto err_free_memfd_ser;
	}

	err = kho_map_restore(folios, &memfd_ser->folios);
	if (err) {
		pr_err("failed to restore folios map: %pe\n", ERR_PTR(err));
		goto err_free_folios;
	}

	if (memfd_ser->size > MAX_LFS_FILESIZE) {
		err = -EINVAL;
		goto err_discard_folios;
	}

	if (!hugetlb_luo_order_hstate(memfd_ser->order)) {
		pr_err("no hstate found for order %u\n", memfd_ser->order);
		err = -ENODEV;
		goto err_discard_folios;
	}

	if (memfd_ser->flags) {
		pr_err("unsupported flags 0x%x\n", memfd_ser->flags);
		err = -EOPNOTSUPP;
		goto err_discard_folios;
	}

	if (memfd_ser->seals & ~HUGETLB_LUO_ALL_SEALS) {
		pr_err("unsupported seals 0x%x\n", memfd_ser->seals);
		err = -EOPNOTSUPP;
		goto err_discard_folios;
	}

	/* Allow sealing so the preserved seals can be added. */
	flags = MFD_HUGETLB | MFD_ALLOW_SEALING;
	flags |= ((unsigned int)memfd_ser->order + PAGE_SHIFT) << MFD_HUGE_SHIFT;
	file = memfd_alloc_file("", flags);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_discard_folios;
	}

	err = memfd_add_seals(file, memfd_ser->seals);
	if (err) {
		pr_err("failed to add seals: %pe\n", ERR_PTR(err));
		fput(file);
		goto err_discard_folios;
	}

	vfs_setpos(file, memfd_ser->pos, MAX_LFS_FILESIZE);
	i_size_write(file_inode(file), memfd_ser->size);

	/*
	 * Keep the file frozen until finish(). Other preserved objects might
	 * depend on it.
	 *
	 * The file isn't visible to anyone yet, but take the lock anyway since
	 * hugetlb_i_freeze() requires it.
	 */
	inode_lock(file_inode(file));
	hugetlb_i_freeze(file_inode(file), true);
	inode_unlock(file_inode(file));

	err = hugetlb_luo_retrieve_folios(file, folios);
	if (err) {
		/* hugetlb_luo_retrieve_folios() released the folios. */
		fput(file);
		goto err_destroy_map;
	}

	/* memfd_ser and the folios map are freed in finish(). */
	args->file = file;
	args->private_data = folios;
	return 0;

err_discard_folios:
	hugetlb_luo_discard_folios(folios, 0, memfd_ser->order);
err_destroy_map:
	kho_map_destroy(folios);
err_free_folios:
	kfree(folios);
err_free_memfd_ser:
	kho_restore_free(memfd_ser);
	return err;
}

static unsigned long hugetlb_luo_get_id(struct file *file)
{
	return (unsigned long)file_inode(file);
}

static const struct liveupdate_file_ops hugetlb_luo_file_ops = {
	.can_preserve = hugetlb_luo_can_preserve,
	.preserve = hugetlb_luo_preserve,
	.unpreserve = hugetlb_luo_unpreserve,
	.freeze = hugetlb_luo_freeze,
	.finish = hugetlb_luo_finish,
	.retrieve = hugetlb_luo_retrieve,
	.get_id = hugetlb_luo_get_id,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler hugetlb_luo_handler = {
	.ops = &hugetlb_luo_file_ops,
	.compatible = HUGETLB_FILE_COMPATIBLE,
};

void __init hugetlb_luo_init(void)
{
	int err;

	if (!liveupdate_enabled())
		return;

	err = liveupdate_register_file_handler(&hugetlb_luo_handler);
	if (err) {
		pr_err("could not register file handler: %pe\n", ERR_PTR(err));
		return;
	}

	err = liveupdate_register_flb(&hugetlb_luo_handler, &hugetlb_luo_flb);
	if (err) {
		pr_err("could not register hugetlb FLB handler: %pe\n", ERR_PTR(err));
		liveupdate_unregister_file_handler(&hugetlb_luo_handler);
		return;
	}
}
