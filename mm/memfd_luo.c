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
 *   All data stored in the file is preserved.
 *
 * File Size
 *   The size of the file is preserved. Holes in the file are filled by
 *   allocating pages for them during preservation.
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
#include <linux/shmem_fs.h>
#include <linux/memfd.h>
#include <uapi/linux/memfd.h>

#include "internal.h"

static int memfd_luo_preserve_folios(struct file *file, struct kho_xarray *kxa)
{
	struct inode *inode = file_inode(file);
	unsigned int max_folios;
	long i, size, nr_pinned;
	struct folio **folios;
	int err = -EINVAL;
	pgoff_t offset;
	u64 nr_folios;

	kho_xa_init(kxa);

	size = i_size_read(inode);
	/* If the file has zero size, then the folios xarray is left empty. */
	if (!size)
		return 0;

	/*
	 * Guess the number of folios based on inode size. Real number might end
	 * up being smaller if there are higher order folios.
	 */
	max_folios = PAGE_ALIGN(size) / PAGE_SIZE;
	folios = kvmalloc_objs(*folios, max_folios);
	if (!folios)
		return -ENOMEM;

	/*
	 * Pin the folios so they don't move around behind our back. This also
	 * ensures none of the folios are in CMA -- which ensures they don't
	 * fall in KHO bootmem. It also moves swapped out folios back to memory.
	 *
	 * A side effect of doing this is that it allocates a folio for all
	 * indices in the file. This might waste memory on sparse memfds. If
	 * that is really a problem in the future, we can have a
	 * memfd_pin_folios() variant that does not allocate a page on empty
	 * slots.
	 */
	nr_pinned = memfd_pin_folios(file, 0, size - 1, folios, max_folios,
				     &offset);
	if (nr_pinned < 0) {
		err = nr_pinned;
		pr_err("failed to pin folios: %d\n", err);
		goto err_free_folios;
	}
	nr_folios = nr_pinned;

	for (i = 0; i < nr_folios; i++) {
		union memfd_luo_folio_entry entry = {};
		struct folio *folio = folios[i];

		err = kho_preserve_folio(folio);
		if (err)
			goto err_unpreserve;

		folio_lock(folio);

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
		 * If the folio is not uptodate, it was fallocated but never
		 * used. Saving this flag at prepare() doesn't work since it
		 * might change later when someone uses the folio.
		 *
		 * Since we have taken the performance penalty of allocating,
		 * zeroing, and pinning all the folios in the holes, take a bit
		 * more and zero all non-uptodate folios too.
		 *
		 * NOTE: For someone looking to improve preserve performance,
		 * this is a good place to look.
		 */
		if (!folio_test_uptodate(folio)) {
			folio_zero_range(folio, 0, folio_size(folio));
			flush_dcache_folio(folio);
			folio_mark_uptodate(folio);
		}

		folio_unlock(folio);

		entry.pfn = folio_pfn(folio);
		entry.flags = MEMFD_LUO_FOLIO_DIRTY | MEMFD_LUO_FOLIO_UPTODATE;
		err = kho_xa_set(kxa, folio->index, entry.val);
		if (err) {
			kho_unpreserve_folio(folio);
			goto err_unpreserve;
		}
	}

	kvfree(folios);
	return 0;

err_unpreserve:
	for (i = i - 1; i >= 0; i--)
		kho_unpreserve_folio(folios[i]);
	kho_xa_destroy(kxa);
	unpin_folios(folios, nr_folios);
err_free_folios:
	kvfree(folios);

	return err;
}

static void memfd_luo_unpreserve_folios(struct kho_xarray *kxa)
{
	union memfd_luo_folio_entry entry;
	u64 index;

	kho_xa_for_each(kxa, index, entry.val) {
		struct folio *folio = pfn_folio(entry.pfn);

		kho_unpreserve_folio(folio);
		unpin_folio(folio);
	}

	kho_xa_destroy(kxa);
}

/*
 * Free the nodes of a KHO xarray that was handed over from the previous kernel.
 * The xarray must be restored first, since only a restored xarray can be
 * destroyed. On failure kho_xa_restore() already destroys what it can.
 */
static void memfd_luo_free_xa(struct kho_xarray *kxa)
{
	if (kho_xa_restore(kxa)) {
		pr_warn("Unable to restore folios xarray\n");
		return;
	}

	kho_xa_destroy(kxa);
}

static int memfd_luo_preserve(struct liveupdate_file_op_args *args)
{
	struct inode *inode = file_inode(args->file);
	struct memfd_luo_ser *ser;
	u64 inode_size;
	int err = 0, seals;

	inode_lock(inode);
	shmem_freeze(inode, true);

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

	ser->pos = args->file->f_pos;
	inode_size = i_size_read(inode);

	/*
	 * memfd_pin_folios() caps at UINT_MAX folios; refuse larger
	 * files to avoid silently preserving only a prefix.
	 */
	if (DIV_ROUND_UP_ULL(inode_size, PAGE_SIZE) > UINT_MAX) {
		err = -EFBIG;
		goto err_free_ser;
	}

	ser->size = inode_size;
	ser->seals = seals;

	err = memfd_luo_preserve_folios(args->file, &ser->folios);
	if (err)
		goto err_free_ser;

	inode_unlock(inode);

	args->serialized_data = virt_to_phys(ser);

	return 0;

err_free_ser:
	kho_unpreserve_free(ser);
err_unlock:
	shmem_freeze(inode, false);
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
	 * The pos might have changed since prepare. Everything else stays the
	 * same.
	 */
	ser->pos = args->file->f_pos;

	return 0;
}

static void memfd_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	struct inode *inode = file_inode(args->file);
	struct memfd_luo_ser *ser;

	if (WARN_ON_ONCE(!args->serialized_data))
		return;

	inode_lock(inode);
	shmem_freeze(inode, false);

	ser = phys_to_virt(args->serialized_data);

	memfd_luo_unpreserve_folios(&ser->folios);

	kho_unpreserve_free(ser);
	inode_unlock(inode);
}

/*
 * Restore and free all folios in @kxa with keys starting from @start. Used
 * when the folios can't be added to a file.
 */
static void memfd_luo_discard_folios(struct kho_xarray *kxa, u64 start)
{
	union memfd_luo_folio_entry entry;
	u64 index;

	for (index = kho_xa_next(kxa, start, &entry.val); index != U64_MAX;
	     index = kho_xa_next(kxa, index + 1, &entry.val)) {
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

	memfd_luo_discard_folios(&ser->folios, 0);
	memfd_luo_free_xa(&ser->folios);

	kho_restore_free(ser);
}

static int memfd_luo_retrieve_folios(struct file *file, struct kho_xarray *kxa)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	long npages, nr_added_pages = 0;
	union memfd_luo_folio_entry entry;
	struct folio *folio;
	int err = -EIO;
	u64 index;

	kho_xa_for_each(kxa, index, entry.val) {
		unsigned long flags = entry.flags;
		phys_addr_t phys = PFN_PHYS(entry.pfn);

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

	memfd_luo_free_xa(&ser->folios);
	args->file = file;
	kho_restore_free(ser);

	return 0;

put_file:
	fput(file);
free_ser:
	memfd_luo_free_xa(&ser->folios);
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
