// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

/**
 * DOC: Memfd Preservation via LUO
 *
 * Overview
 * ========
 *
 * Memory file descriptors (memfd) can be preserved over a kexec using the Live
 * Update Orchestrator (LUO) file preservation. This allows userspace to
 * transfer its memory contents to the next kernel after a kexec.
 *
 * The preservation is not intended to be transparent. Only select properties of
 * the file are preserved. All others are reset to default. The preserved
 * properties are described below.
 *
 * .. note::
 *    The LUO API is not stabilized yet, so the preserved properties of a memfd
 *    are also not stable and are subject to backwards incompatible changes.
 *
 * .. note::
 *    Currently a memfd backed by Hugetlb is not supported. Memfds created
 *    with ``MFD_HUGETLB`` will be rejected.
 *
 * Preserved Properties
 * ====================
 *
 * The following properties of the memfd are preserved across kexec:
 *
 * File Contents
 *   All data stored in the file is preserved. The file stays fully usable after
 *   preservation. Folios added to the file later, e.g. by faulting in a hole,
 *   writing to it, or ``fallocate()``, are preserved as well. Folios removed by
 *   truncation or hole punching are no longer preserved.
 *
 * File Size
 *   The size of the file at freeze time is preserved. Holes in the file are
 *   not filled and stay holes in the next kernel.
 *
 * File Position
 *   The current file position is preserved, allowing applications to continue
 *   reading/writing from their last position.
 *
 * File Status Flags
 *   memfds are always opened with ``O_RDWR`` and ``O_LARGEFILE``. This property
 *   is maintained.
 *
 * Seals
 *   File seals set on the memfd are preserved and re-applied on restore.
 *   Only seals known to this LUO version (see ``MEMFD_LUO_ALL_SEALS``) may
 *   be present; preservation fails with ``-EOPNOTSUPP`` otherwise.
 *
 * Non-Preserved Properties
 * ========================
 *
 * All properties which are not preserved must be assumed to be reset to
 * default. This section describes some of those properties which may be more of
 * note.
 *
 * ``FD_CLOEXEC`` flag
 *   A memfd can be created with the ``MFD_CLOEXEC`` flag that sets the
 *   ``FD_CLOEXEC`` on the file. This flag is not preserved and must be set
 *   again after restore via ``fcntl()``.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/memfd.h>
#include <linux/kho_xarray.h>
#include <linux/liveupdate.h>
#include <linux/migrate.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/shmem_fs.h>
#include <linux/swap.h>
#include <linux/memfd.h>
#include <uapi/linux/memfd.h>

#include "internal.h"

/**
 * struct memfd_luo_private - Runtime state of a preserved memfd.
 * @lock:   Serializes all access to @folios, and protects @dead.
 * @folios: The KHO xarray tracking the preserved folios of the file.
 * @ref:    Reference count. Hooks called from shmem hold a reference while they
 *          use the struct.
 * @dead:   The file has been unpreserved and @folios is gone.
 *
 * shmem calls memfd_luo_track_folio() and memfd_luo_untrack_removed() when a
 * preserved inode gains or loses folios. Those find this struct by looking up
 * the inode in memfd_luo_inodes.
 */
struct memfd_luo_private {
	struct mutex lock;
	struct kho_xarray *folios;
	refcount_t ref;
	bool dead;
};

/* Maps inodes of preserved memfds to their struct memfd_luo_private. */
static DEFINE_XARRAY(memfd_luo_inodes);

static struct memfd_luo_private *memfd_luo_get_private(struct inode *inode)
{
	struct memfd_luo_private *priv;

	xa_lock(&memfd_luo_inodes);
	priv = xa_load(&memfd_luo_inodes, (unsigned long)inode);
	if (priv)
		refcount_inc(&priv->ref);
	xa_unlock(&memfd_luo_inodes);

	return priv;
}

static void memfd_luo_put_private(struct memfd_luo_private *priv)
{
	if (refcount_dec_and_test(&priv->ref))
		kfree(priv);
}

static void memfd_luo_put_folios(struct kho_xarray *kxa)
{
	union memfd_luo_folio_ser entry;
	u64 index;

	kho_xa_for_each(kxa, index, entry.val) {
		struct folio *folio = pfn_folio(entry.pfn);

		kho_unpreserve_folio(folio);
		folio_put(folio);
	}
}

/* How many times to retry isolating a folio someone else has isolated. */
#define MEMFD_LUO_MIGRATE_RETRIES	3

/*
 * Move a folio out of ZONE_MOVABLE or CMA, so it can be held until the next
 * kernel. Called with a reference on the unlocked folio, which is always
 * dropped. The caller must look up the folio again afterwards.
 *
 * Returns 0 on success, -EBUSY if the folio could not be isolated (e.g.
 * because someone else has it isolated right now), or -ENOMEM if migration
 * failed.
 */
static int memfd_luo_migrate_folio(struct folio *folio)
{
	struct migration_target_control mtc = {
		.nid = NUMA_NO_NODE,
		/* Not movable, so the target is neither in ZONE_MOVABLE nor CMA. */
		.gfp_mask = GFP_USER | __GFP_NOWARN,
		.reason = MR_LONGTERM_PIN,
	};
	LIST_HEAD(source);
	bool isolated;

	/* Flush per-CPU LRU batches holding the folio so isolation works. */
	lru_cache_drain_for_folio(folio, 1, NULL);
	isolated = isolate_folio_to_list(folio, &source);
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

static int memfd_luo_get_folio(struct inode *inode, pgoff_t index,
			       struct folio **foliop)
{
	int err, busy_retries = 0;
	struct folio *folio;

	for (;;) {
		/*
		 * SGP_FALLOC doesn't stop at i_size, so folios fallocated
		 * beyond EOF are found too. It also clears fallocated folios
		 * that are not uptodate yet. This is only called for indices
		 * that have an entry, so it only allocates if the entry went
		 * away in the meantime. Such a folio is tracked like any other.
		 */
		err = shmem_get_folio(inode, index, 0, &folio, SGP_FALLOC);
		if (err)
			return err;

		if (likely(folio_is_longterm_pinnable(folio)))
			break;

		folio_unlock(folio);
		err = memfd_luo_migrate_folio(folio);
		/* Someone else might have it isolated, retry a bit. */
		if (err == -EBUSY && ++busy_retries < MEMFD_LUO_MIGRATE_RETRIES)
			continue;
		if (err)
			return err;
	}

	*foliop = folio;
	return 0;
}

/*
 * Preserve a locked folio and add it to @kxa. On success, @kxa holds its own
 * reference to the folio. Folios that are already tracked are left alone.
 *
 * Must be called with the lock of the owning struct memfd_luo_private held.
 */
static int memfd_luo_preserve_folio(struct folio *folio, struct kho_xarray *kxa)
{
	union memfd_luo_folio_ser entry = {};
	int err;

	entry.val = kho_xa_get(kxa, folio->index);
	if (entry.val) {
		/* Tracked folios hold a reference, so they can't be replaced. */
		WARN_ON_ONCE(entry.pfn != folio_pfn(folio));
		return 0;
	}

	err = kho_preserve_folio(folio);
	if (err)
		return err;

	/*
	 * A dirty folio is one which has been written to. A clean folio
	 * is its opposite. Since a clean folio does not carry user
	 * data, it can be freed by page reclaim under memory pressure.
	 *
	 * Saving the dirty flag at prepare() time doesn't work since it
	 * can change later. Saving it at freeze() also won't work
	 * because the dirty bit is normally synced at unmap and there
	 * might still be a mapping of the file at freeze().
	 *
	 * To see why this is a problem, say a folio is clean at
	 * preserve, but gets dirtied later. The preserved flags will mark
	 * it as clean. After retrieve, the next kernel might try to
	 * reclaim this folio under memory pressure, losing user data.
	 *
	 * Unconditionally mark it dirty to avoid this problem. This
	 * comes at the cost of making clean folios un-reclaimable after
	 * live update.
	 */
	folio_mark_dirty(folio);

	/*
	 * The folio might not be uptodate yet when it was just allocated for a
	 * write. The writer makes it uptodate before unlocking it.
	 */
	entry.pfn = folio_pfn(folio);
	entry.flags = MEMFD_LUO_FOLIO_DIRTY | MEMFD_LUO_FOLIO_UPTODATE;
	err = kho_xa_set(kxa, folio->index, entry.val);
	if (err) {
		kho_unpreserve_folio(folio);
		return err;
	}

	folio_get(folio);
	return 0;
}

/**
 * memfd_luo_track_folio() - Track a folio newly added to a preserved memfd.
 * @inode: The inode of the memfd.
 * @folio: The locked folio, already in the page cache of @inode.
 *
 * Called by shmem after a folio was added to the page cache of an inode marked
 * with SHMEM_F_LUO_PRESERVED, e.g. when a hole is faulted in. Must be called
 * after the last point where shmem might remove the folio again on error.
 *
 * Return: 0 on success, -EAGAIN if the folio can't be held until the next
 * kernel and the caller should remove it and allocate a new one, or another
 * -errno on failure.
 */
int memfd_luo_track_folio(struct inode *inode, struct folio *folio)
{
	struct memfd_luo_private *priv;
	int err = 0;

	priv = memfd_luo_get_private(inode);
	if (!priv)
		return 0;

	/*
	 * shmem doesn't allocate from ZONE_MOVABLE or CMA for preserved
	 * inodes, but this folio might have been allocated before the inode
	 * got preserved.
	 */
	if (unlikely(!folio_is_longterm_pinnable(folio))) {
		err = -EAGAIN;
		goto out;
	}

	mutex_lock(&priv->lock);
	if (!priv->dead)
		err = memfd_luo_preserve_folio(folio, priv->folios);
	mutex_unlock(&priv->lock);

out:
	memfd_luo_put_private(priv);
	return err;
}

/**
 * memfd_luo_untrack_removed() - Stop tracking folios removed from a memfd.
 * @inode: The inode of the memfd.
 * @start: First index to check.
 * @end:   Index after the last one to check.
 *
 * Called by shmem after folios in [@start, @end) might have been removed from
 * the page cache of an inode marked with SHMEM_F_LUO_PRESERVED, e.g. by
 * truncation or hole punching. Tracked folios that are no longer in the page
 * cache are unpreserved and their reference is dropped.
 *
 * Tracked folios hold a reference, so they can't be split. A large folio is
 * thus either removed as a whole, or not at all.
 */
void memfd_luo_untrack_removed(struct inode *inode, pgoff_t start, pgoff_t end)
{
	struct memfd_luo_private *priv;
	union memfd_luo_folio_ser entry;
	u64 index;

	priv = memfd_luo_get_private(inode);
	if (!priv)
		return;

	mutex_lock(&priv->lock);
	if (priv->dead)
		goto out;

	kho_xa_for_each_from(priv->folios, index, entry.val, start) {
		struct folio *folio = pfn_folio(entry.pfn);

		if (index >= end)
			break;

		/* The reference keeps the folio from going away or getting reused. */
		if (READ_ONCE(folio->mapping) == inode->i_mapping)
			continue;

		kho_xa_clear(priv->folios, index);
		kho_unpreserve_folio(folio);
		folio_put(folio);
	}

out:
	mutex_unlock(&priv->lock);
	memfd_luo_put_private(priv);
}

static int memfd_luo_preserve_folios(struct file *file,
				     struct memfd_luo_private *priv)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	unsigned int pin_flags;
	pgoff_t index = 0;
	int err = 0;

	/*
	 * The folios are held until the next kernel, so keep newly allocated
	 * ones (e.g. on swapin) out of ZONE_MOVABLE and CMA -- which also
	 * ensures they don't fall in KHO bootmem. Existing ones are migrated
	 * below.
	 *
	 * Only walk the entries that exist. Holes stay holes, folios that get
	 * added to them from now on are tracked by memfd_luo_track_folio().
	 * Those might show up in this walk too, memfd_luo_preserve_folio()
	 * skips them.
	 */
	pin_flags = memalloc_pin_save();
	while (xa_find(&mapping->i_pages, &index, ULONG_MAX, XA_PRESENT)) {
		struct folio *folio;

		cond_resched();

		if (fatal_signal_pending(current)) {
			err = -EINTR;
			break;
		}

		err = memfd_luo_get_folio(inode, index, &folio);
		if (err)
			break;

		mutex_lock(&priv->lock);
		err = memfd_luo_preserve_folio(folio, priv->folios);
		mutex_unlock(&priv->lock);

		index = folio_next_index(folio);
		folio_unlock(folio);
		folio_put(folio);
		/* Reached the end of the index space. */
		if (err || !index)
			break;
	}
	memalloc_pin_restore(pin_flags);

	return err;
}

/*
 * Make shmem stop calling into memfd_luo for @inode, wait for the calls in
 * flight, and drop all tracked folios.
 *
 * Must be called with the inode lock held.
 */
static void memfd_luo_stop_tracking(struct inode *inode,
				    struct memfd_luo_private *priv)
{
	shmem_set_luo_preserved(inode, false);
	xa_erase(&memfd_luo_inodes, (unsigned long)inode);

	mutex_lock(&priv->lock);
	priv->dead = true;
	memfd_luo_put_folios(priv->folios);
	kho_xa_destroy(priv->folios);
	mutex_unlock(&priv->lock);

	memfd_luo_put_private(priv);
}

static int memfd_luo_preserve(struct liveupdate_file_op_args *args)
{
	struct inode *inode = file_inode(args->file);
	struct memfd_luo_private *priv;
	struct memfd_luo_ser *ser;
	int err = 0, seals;

	inode_lock(inode);

	/* Allocate the main serialization structure in preserved memory */
	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		err = PTR_ERR(ser);
		goto err_unlock;
	}

	seals = memfd_get_seals(args->file);
	if (seals < 0) {
		err = seals;
		goto err_free_ser;
	}

	/* Make sure the file only has the seals supported by this version. */
	if (seals & ~MEMFD_LUO_ALL_SEALS) {
		err = -EOPNOTSUPP;
		goto err_free_ser;
	}

	ser->seals = seals;
	kho_xa_init(&ser->folios);

	priv = kzalloc_obj(*priv);
	if (!priv) {
		err = -ENOMEM;
		goto err_free_ser;
	}

	mutex_init(&priv->lock);
	refcount_set(&priv->ref, 1);
	priv->folios = &ser->folios;

	err = xa_insert(&memfd_luo_inodes, (unsigned long)inode, priv,
			GFP_KERNEL);
	if (err) {
		kfree(priv);
		goto err_free_ser;
	}

	/* From here on, shmem reports new folios to memfd_luo_track_folio(). */
	shmem_set_luo_preserved(inode, true);

	err = memfd_luo_preserve_folios(args->file, priv);
	if (err)
		goto err_stop_tracking;

	/* Updated again at freeze(), the file can still change until then. */
	ser->pos = args->file->f_pos;
	ser->size = i_size_read(inode);

	inode_unlock(inode);

	args->serialized_data = virt_to_phys(ser);
	args->private_data = priv;

	return 0;

err_stop_tracking:
	memfd_luo_stop_tracking(inode, priv);
err_free_ser:
	kho_unpreserve_free(ser);
err_unlock:
	inode_unlock(inode);
	return err;
}

static int memfd_luo_freeze(struct liveupdate_file_op_args *args)
{
	struct memfd_luo_ser *ser;

	if (WARN_ON_ONCE(!args->serialized_data))
		return -EINVAL;

	ser = phys_to_virt(args->serialized_data);

	/*
	 * The pos and size might have changed since preserve. The folios are
	 * kept up to date as they change.
	 */
	ser->pos = args->file->f_pos;
	ser->size = i_size_read(file_inode(args->file));

	return 0;
}

static void memfd_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	struct inode *inode = file_inode(args->file);
	struct memfd_luo_ser *ser;

	if (WARN_ON_ONCE(!args->serialized_data))
		return;

	ser = phys_to_virt(args->serialized_data);

	inode_lock(inode);
	memfd_luo_stop_tracking(inode, args->private_data);
	kho_unpreserve_free(ser);
	inode_unlock(inode);
}

/*
 * Restore and free all folios in @kxa with keys starting from @start. @kxa must
 * already be restored.
 */
static void memfd_luo_discard_folios(struct kho_xarray *kxa, u64 start)
{
	union memfd_luo_folio_ser entry;
	u64 index;

	kho_xa_for_each_from(kxa, index, entry.val, start) {
		phys_addr_t phys = PFN_PHYS(entry.pfn);
		struct folio *folio;

		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_warn_ratelimited("Unable to restore folio at physical address: %llx\n",
					    phys);
			continue;
		}

		folio_put(folio);
	}
}

static void memfd_luo_finish(struct liveupdate_file_op_args *args)
{
	struct memfd_luo_ser *ser;

	/*
	 * If retrieve was successful, nothing to do. If it failed, retrieve()
	 * already cleaned up everything it could. So nothing to do there
	 * either. Only need to clean up when retrieve was not called.
	 */
	if (args->retrieve_status)
		return;

	ser = phys_to_virt(args->serialized_data);
	if (!ser)
		return;

	if (kho_xa_restore(&ser->folios)) {
		pr_warn("Unable to restore folios xarray\n");
		goto out;
	}

	memfd_luo_discard_folios(&ser->folios, 0);
	kho_xa_destroy(&ser->folios);

out:
	kho_restore_free(ser);
}

static int memfd_luo_retrieve_folios(struct file *file, struct kho_xarray *kxa)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	long npages, nr_added_pages = 0;
	union memfd_luo_folio_ser entry;
	struct folio *folio;
	int err = -EIO;
	u64 index;

	err = kho_xa_restore(kxa);
	if (err) {
		pr_err("failed to restore folios xarray: %pe\n", ERR_PTR(err));
		return err;
	}

	kho_xa_for_each(kxa, index, entry.val) {
		phys_addr_t phys = PFN_PHYS(entry.pfn);
		unsigned long flags = entry.flags;

		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_err("Unable to restore folio at physical address: %llx\n",
			       phys);
			err = -EIO;
			goto put_folios;
		}

		/* Set up the folio for insertion. */
		__folio_set_locked(folio);
		__folio_set_swapbacked(folio);

		err = mem_cgroup_charge(folio, NULL, mapping_gfp_mask(mapping));
		if (err) {
			pr_err("shmem: failed to charge folio index %llu: %d\n",
			       index, err);
			goto unlock_folio;
		}

		err = shmem_add_to_page_cache(folio, mapping, index, NULL,
					      mapping_gfp_mask(mapping));
		if (err) {
			pr_err("shmem: failed to add to page cache folio index %llu: %d\n",
			       index, err);
			goto unlock_folio;
		}

		if (flags & MEMFD_LUO_FOLIO_UPTODATE)
			folio_mark_uptodate(folio);
		if (flags & MEMFD_LUO_FOLIO_DIRTY)
			folio_mark_dirty(folio);

		npages = folio_nr_pages(folio);
		err = shmem_inode_acct_blocks(inode, npages);
		if (err) {
			pr_err("shmem: failed to account folio index %llu(%ld pages): %d\n",
			       index, npages, err);
			goto remove_from_cache;
		}

		nr_added_pages += npages;
		folio_add_lru(folio);
		folio_unlock(folio);
		folio_put(folio);
	}

	shmem_recalc_inode(inode, nr_added_pages, 0);
	kho_xa_destroy(kxa);

	return 0;

remove_from_cache:
	filemap_remove_folio(folio);
unlock_folio:
	folio_unlock(folio);
	folio_put(folio);
put_folios:
	/*
	 * Note: don't free the folios already added to the file. They will be
	 * freed when the file is freed. Free the ones not added yet here.
	 */
	memfd_luo_discard_folios(kxa, index + 1);

	shmem_recalc_inode(inode, nr_added_pages, 0);
	kho_xa_destroy(kxa);

	return err;
}

static int memfd_luo_retrieve(struct liveupdate_file_op_args *args)
{
	struct memfd_luo_ser *ser;
	struct file *file;
	int err;

	ser = phys_to_virt(args->serialized_data);
	if (!ser)
		return -EINVAL;

	/* Make sure the file only has seals supported by this version. */
	if (ser->seals & ~MEMFD_LUO_ALL_SEALS) {
		err = -EOPNOTSUPP;
		goto free_ser;
	}

	/*
	 * The seals are preserved. Allow sealing here so they can be added
	 * later.
	 */
	file = memfd_alloc_file("", MFD_ALLOW_SEALING);
	if (IS_ERR(file)) {
		pr_err("failed to setup file: %pe\n", file);
		err = PTR_ERR(file);
		goto free_ser;
	}

	err = memfd_add_seals(file, ser->seals);
	if (err) {
		pr_err("failed to add seals: %pe\n", ERR_PTR(err));
		goto put_file;
	}

	vfs_setpos(file, ser->pos, MAX_LFS_FILESIZE);
	i_size_write(file_inode(file), ser->size);

	err = memfd_luo_retrieve_folios(file, &ser->folios);
	if (err)
		goto put_file;

	args->file = file;
	kho_restore_free(ser);

	return 0;

put_file:
	fput(file);
free_ser:
	kho_restore_free(ser);
	return err;
}

static bool memfd_luo_can_preserve(struct liveupdate_file_handler *handler,
				   struct file *file)
{
	struct inode *inode = file_inode(file);

	return shmem_file(file) && !inode->i_nlink;
}

static unsigned long memfd_luo_get_id(struct file *file)
{
	return (unsigned long)file_inode(file);
}

static const struct liveupdate_file_ops memfd_luo_file_ops = {
	.freeze = memfd_luo_freeze,
	.finish = memfd_luo_finish,
	.retrieve = memfd_luo_retrieve,
	.preserve = memfd_luo_preserve,
	.unpreserve = memfd_luo_unpreserve,
	.can_preserve = memfd_luo_can_preserve,
	.get_id = memfd_luo_get_id,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler memfd_luo_handler = {
	.ops = &memfd_luo_file_ops,
	.compatible = MEMFD_LUO_FH_COMPATIBLE,
};

static int __init memfd_luo_init(void)
{
	int err = liveupdate_register_file_handler(&memfd_luo_handler);

	if (err && err != -EOPNOTSUPP) {
		pr_err("Could not register luo filesystem handler: %pe\n",
		       ERR_PTR(err));

		return err;
	}

	return 0;
}
late_initcall(memfd_luo_init);
