// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <pratyush@kernel.org>
 */

/* TODO: Docs */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/file.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/kexec_handover.h>
#include <linux/shmem_fs.h>
#include <linux/bits.h>
#include <linux/hugetlb.h>
#include <linux/vmalloc.h>
#include <linux/kho/abi/hugetlb.h>

#include "internal.h"
#include "hugetlb_internal.h"

/* TODO: HugeTLB CMA support. */

struct hugetlb_flb_obj {
	struct hugetlb_folio_ser *folios[HUGETLB_SER_MAX_HSTATES];
	/* Stores pointer to struct hugetlb_folio_ser for each preserved PFN. */
	struct xarray pfn_to_ser;
};

struct huge_memfd_private {
	struct huge_memfd_folio_ser *folios_ser;
	unsigned long nr_folios;
};

static void hugetlb_flb_unpreserve_folio(struct hugetlb_folio_ser *folio_ser,
					 struct hugetlb_flb_obj *obj)
{
	struct folio *folio = pfn_folio(folio_ser->pfn);
	void *entry;

	kho_unpreserve_folio(folio);
	entry = xa_erase(&obj->pfn_to_ser, folio_ser->pfn);
	WARN_ON_ONCE(entry != folio_ser); /* TODO: Keep this? */
}

static int hugetlb_flb_preserve_folio(struct folio *folio,
				      struct hugetlb_folio_ser *folio_ser,
				      struct hugetlb_flb_obj *obj)
{
	unsigned long pfn = folio_pfn(folio);
	int err;

	err = kho_preserve_folio(folio);
	if (err)
		return err;

	folio_ser->pfn = pfn;
	/*
	 * Mark all folios as free for now. When files are preserved, they will
	 * unset it for their folios.
	 */
	folio_ser->free = 1;

	/*
	 * Keep track of the serialized state for each folio. This saves file
	 * preservation callbacks from having to scan the whole list for each
	 * and every folio they preserve.
	 */
	err = xa_err(xa_store(&obj->pfn_to_ser, pfn, folio_ser, GFP_KERNEL));
	if (err) {
		kho_unpreserve_folio(folio);
		return err;
	}

	return 0;
}

static void hugetlb_flb_unpreserve_hstate(struct hugetlb_hstate_ser *hser,
					  struct hugetlb_flb_obj *obj,
					  struct hugetlb_folio_ser *folios_ser)
{
	for (long i = 0; i < hser->nr_pages; i++)
		hugetlb_flb_unpreserve_folio(&folios_ser[i], obj);

	kho_unpreserve_vmalloc(&hser->folios);
	vfree(folios_ser);
}

static int hugetlb_flb_preserve_hstate(struct hstate *h,
				       struct hugetlb_hstate_ser *hser,
				       struct hugetlb_folio_ser **out_folios_ser,
				       struct hugetlb_flb_obj *obj)
{
	struct hugetlb_folio_ser *folios_ser;
	struct folio *folio;
	long idx = 0;
	int err;

	hser->nr_pages = h->nr_huge_pages;
	hser->order = h->order;

	folios_ser = vcalloc(h->nr_huge_pages, sizeof(struct hugetlb_folio_ser));
	if (!folios_ser)
		return -ENOMEM;

	list_for_each_entry(folio, &h->hugepage_activelist, lru) {

		err = hugetlb_flb_preserve_folio(folio, &folios_ser[idx], obj);
		if (err)
			goto err_unpreserve;
		idx++;
	}

	for (int i = 0; i < MAX_NUMNODES; ++i) {
		list_for_each_entry(folio, &h->hugepage_freelists[i], lru) {
			err = hugetlb_flb_preserve_folio(folio, &folios_ser[idx], obj);
			if (err)
				goto err_unpreserve;
			idx++;
		}
	}

	if (WARN_ON_ONCE(idx != h->nr_huge_pages)) {
		err = -EINVAL;
		goto err_unpreserve;
	}

	err = kho_preserve_vmalloc(folios_ser, &hser->folios);
	if (err)
		goto err_unpreserve;

	*out_folios_ser = folios_ser;
	return 0;

err_unpreserve:
	for (long i = 0; i < idx; i++)
		hugetlb_flb_unpreserve_folio(&folios_ser[i], obj);

	vfree(folios_ser);
	return err;
}

/* TODO: Freeze hstates after this. */
static int hugetlb_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_flb_obj *obj;
	unsigned short nr_hstates = 0;
	struct folio *hugetlb_ser_folio;
	struct hstate *h;
	int err;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	xa_init(&obj->pfn_to_ser);

	hugetlb_ser_folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!hugetlb_ser_folio) {
		err = -ENOMEM;
		goto err_free_obj;
	}

	err = kho_preserve_folio(hugetlb_ser_folio);
	if (err)
		goto err_free_hugetlb_ser;

	hugetlb_ser = folio_address(hugetlb_ser_folio);

	for_each_hstate(h) {
		struct hugetlb_folio_ser *folios_ser;

		/* No point in preserving a hstate with no pages. */
		if (!h->nr_huge_pages)
			continue;

		err = hugetlb_flb_preserve_hstate(h, &hugetlb_ser->hstates[nr_hstates],
						  &folios_ser, obj);
		if (err)
			goto err_unpreserve;

		obj->folios[nr_hstates] = folios_ser;
		nr_hstates++;
	}

	hugetlb_ser->nr_hstates = nr_hstates;

	args->obj = obj;
	args->data = virt_to_phys(hugetlb_ser);

	return 0;

err_unpreserve:
	for (short i = 0; i < nr_hstates; i++)
		hugetlb_flb_unpreserve_hstate(&hugetlb_ser->hstates[i],
					      obj, obj->folios[i]);
	kho_unpreserve_folio(hugetlb_ser_folio);
err_free_hugetlb_ser:
	folio_put(hugetlb_ser_folio);
err_free_obj:
	kfree(obj);
	return err;
}

static void hugetlb_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	struct hugetlb_ser *hugetlb_ser = phys_to_virt(args->data);
	struct folio *folio = virt_to_folio(hugetlb_ser);
	struct hugetlb_flb_obj *obj = args->obj;

	for (short i = 0; i < hugetlb_ser->nr_hstates; i++)
		hugetlb_flb_unpreserve_hstate(&hugetlb_ser->hstates[i], obj,
					      obj->folios[i]);
	kho_unpreserve_folio(folio);
	folio_put(folio);
	/* TODO: Put these assertions under debug config? */
	WARN_ON_ONCE(!xa_empty(&obj->pfn_to_ser));
	kfree(obj);
}

static void hugetlb_flb_finish(struct liveupdate_flb_op_args *args)
{
	/* No live state needed on the retrieve side. So nothing to do here. */
}

static int hugetlb_flb_retrieve_hstate(struct hugetlb_hstate_ser *hser)
{
	struct hstate *h = size_to_hstate(PAGE_SIZE << hser->order);
	struct hugetlb_folio_ser *folios_ser;
	LIST_HEAD(free_folios);
	LIST_HEAD(busy_folios);

	if (!h) {
		pr_warn("no hstate found for order %u\n", hser->order);
		return -EINVAL;
	}

	folios_ser = kho_restore_vmalloc(&hser->folios);
	if (!folios_ser) {
		pr_warn("failed to restore hstate order %u\n", hser->order);
		/* TODO: These failures should either stop hugetlb
		 * preservation completely or panic. */
		return -EINVAL;
	}

	for (unsigned long f = 0; f < hser->nr_pages; f++) {
		struct folio *folio;

		/* TODO: Gigantic hugepages have different struct page
		 * init style. See __alloc_bootmem_huge_page(). */
		folio = kho_restore_folio(PFN_PHYS(folios_ser[f].pfn));
		if (!folio) {
			pr_warn("failed to restore folio for hstate order %d\n",
				hser->order);
			return -EINVAL;
		}
		init_new_hugetlb_folio(folio);
		/* TODO: Properly figure out freezing. */
		folio_ref_freeze(folio, 1);
		if (folios_ser[f].free)
			list_add(&folio->lru, &free_folios);
		else
			list_add(&folio->lru, &busy_folios);
	}

	/* TODO: Should I use add_hugetlb_folio() instead? */
	prep_and_add_allocated_folios(h, &free_folios);
	prep_and_add_busy_folios(h, &busy_folios);

	vfree(folios_ser);

	return 0;
}

static int hugetlb_flb_retrieve(struct liveupdate_flb_op_args *args)
{
	struct hugetlb_ser *hugetlb_ser;
	struct folio *hugetlb_ser_folio;
	int err = 0;

	hugetlb_ser_folio = kho_restore_folio(args->data);
	if (!hugetlb_ser_folio)
		return -EINVAL;

	hugetlb_ser = folio_address(hugetlb_ser_folio);

	for (short i = 0; i < hugetlb_ser->nr_hstates; i++) {
		err = hugetlb_flb_retrieve_hstate(&hugetlb_ser->hstates[i]);
		/*
		 * If tihs fails, stop trying to retrieve other hstates, since
		 * something has really gone wrong and the system can be in a
		 * bad state. But keep the ones already reteieved around.
		 */
		/* TODO: Make sure retrieve of files of failed hstates also fails. */
		if (err)
			goto out;
	}

	/*
	 * HACK: LUO FLB needs an object or it will retry the retrieve. But
	 * there is not a real need for any live state after retrieve. So just
	 * use ZERO_SIZE_PTR to satisfy FLB.
	 */
	args->obj = ZERO_SIZE_PTR;
out:
	folio_put(hugetlb_ser_folio);
	return err;
}

static struct liveupdate_flb_ops hugetlb_luo_flb_ops = {
	.preserve = hugetlb_flb_preserve,
	.unpreserve = hugetlb_flb_unpreserve,
	.finish = hugetlb_flb_finish,
	.retrieve = hugetlb_flb_retrieve,
};

static struct liveupdate_flb hugetlb_luo_flb = {
	.ops = &hugetlb_luo_flb_ops,
	.compatible = "hugetlb",
};

/*
 * Initially, all folios are marked as free in the serialized data. File
 * preservation callbacks should call this on each folio in the preserved file
 * so it gets marked as not free and is not released to the allocator after live
 * update.
 */
static int hugetlb_flb_mark_busy(struct folio *folio)
{
	struct hugetlb_flb_obj *obj;
	struct hugetlb_folio_ser *folio_ser;
	int err;

	err = liveupdate_flb_get_outgoing(&hugetlb_luo_flb, (void **)&obj);
	if (err)
		return err;

	folio_ser = xa_load(&obj->pfn_to_ser, folio_pfn(folio));
	if (xa_is_err(folio_ser))
		return xa_err(folio_ser);

	folio_ser->free = 0;

	return 0;
}

static bool huge_memfd_can_preserve(struct liveupdate_file_handler *handler,
				    struct file *file)
{
	struct inode *inode = file_inode(file);

	return is_file_hugepages(file) && !inode->i_nlink;
}

static int
huge_memfd_preserve_folios(struct huge_memfd_ser *memfd_ser, struct file *file,
			   unsigned long *nr_foliosp,
			   struct huge_memfd_folio_ser **out_folios_ser)
{
	struct huge_memfd_folio_ser *folios_ser;
	struct inode *inode = file_inode(file);
	struct hstate *h = hstate_inode(inode);
	unsigned int max_folios;
	long i, nr_folios, size;
	struct folio **folios;
	pgoff_t offset;
	int err;

	size = i_size_read(inode);

	if (!size) {
		*nr_foliosp = 0;
		*out_folios_ser = NULL;
		memset(&memfd_ser->folios, 0, sizeof(memfd_ser->folios));
		return 0;
	}

	/* Calculate number of folios in the file based on its size. */
	max_folios = size / huge_page_size(h);
	folios = kvmalloc_array(max_folios, sizeof(*folios), GFP_KERNEL);
	if (!folios)
		return -ENOMEM;

	/*
	 * Pin the folios so they don't move around behind our back. This also
	 * ensures none of the folios are in CMA -- which ensures they don't
	 * fall in KHO scratch memory. It also moves swapped out folios back to
	 * memory.
	 *
	 * A side effect of doing this is that it allocates a folio for all
	 * indices in the file. This might waste memory on sparse memfds. If
	 * that is really a problem in the future, we can have a
	 * memfd_pin_folios() variant that does not allocate a page on empty
	 * slots.
	 */
	nr_folios = memfd_pin_folios(file, 0, size - 1, folios, max_folios, &offset);
	if (nr_folios < 0) {
		err = nr_folios;
		goto err_free_folios;
	}

	/* HACK */
	WARN_ON_ONCE(nr_folios != max_folios);

	folios_ser = vcalloc(nr_folios, sizeof(*folios_ser));
	if (!folios_ser) {
		err = -ENOMEM;
		goto err_unpin;
	}

	for (i = 0; i < nr_folios; i++) {
		/* TODO: The naming of folios_ser and folio_ser is very easy to
		 * mix up. */
		struct huge_memfd_folio_ser *folio_ser = &folios_ser[i];
		struct folio *folio = folios[i];

		/*
		 * The folio should already have been KHO-preserved by FLB. Just
		 * mark it as busy now.
		 */
		err = hugetlb_flb_mark_busy(folio);
		if (err)
			goto err_unpreserve;

		folio_set_hugetlb_luo(folio);

		folio_ser->pfn = folio_pfn(folio);
		folio_ser->index = folio->index;

		printk("preserving pfn: 0x%lx index: 0x%lx\n",
		       (unsigned long)folio_ser->pfn,
		       (unsigned long)folio_ser->index);
	}

	err = kho_preserve_vmalloc(folios_ser, &memfd_ser->folios);
	if (err)
		goto err_unpreserve;

	memfd_ser->nr_folios = nr_folios;
	*nr_foliosp = nr_folios;
	*out_folios_ser = folios_ser;
	return 0;

err_unpreserve:
	for (i = i - 1; i >= 0; i--)
		kho_unpreserve_folio(folios[i]);
	vfree(folios_ser);
err_unpin:
	unpin_folios(folios, nr_folios);
err_free_folios:
	kvfree(folios);
	return err;
}

static int huge_memfd_preserve(struct liveupdate_file_op_args *args)
{
	struct file *file = args->file;
	struct inode *inode = file_inode(file);
	struct hstate *h = hstate_inode(inode);
	struct huge_memfd_folio_ser *folios_ser;
	struct huge_memfd_private *private;
	struct huge_memfd_ser *memfd_ser;
	struct folio *folio;
	unsigned long nr_folios;
	int err;

	private = kmalloc(sizeof(*private), GFP_KERNEL);
	if (!private)
		return -ENOMEM;

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!folio) {
		err = -ENOMEM;
		goto err_free_private;
	}

	err = kho_preserve_folio(folio);
	if (err)
		goto err_put_folio;

	memfd_ser = folio_address(folio);
	inode_lock(inode);

	/* TODO: Freeze inode */

	memfd_ser->size = i_size_read(inode);
	memfd_ser->pos = file->f_pos;
	memfd_ser->order = h->order;

	if (err)
		goto err_unlock;

	err = huge_memfd_preserve_folios(memfd_ser, file, &nr_folios, &folios_ser);
	if (err)
		goto err_unlock;

	inode_unlock(inode);

	private->folios_ser = folios_ser;
	private->nr_folios = nr_folios;
	args->private_data = private;
	args->serialized_data = virt_to_phys(memfd_ser);

	return 0;

err_unlock:
	inode_unlock(inode);
err_put_folio:
	folio_put(folio);
err_free_private:
	kfree(private);
	return err;
}

static void huge_memfd_unpreserve(struct liveupdate_file_op_args *args)
{

}

static void huge_memfd_finish(struct liveupdate_file_op_args *args)
{

}

static int huge_memfd_setup_rsrv(struct inode *inode)
{
	struct hstate *h = hstate_inode(inode);
	long chg, regions_needed, add;
	/*
	 * NOTE: Setting up the reservations for the whole file works right now
	 * because during preserve all the folios are filled in when pinning.
	 * Whenever that changes, this needs to be updated as well.
	 */
	long from = 0, to = inode->i_size >> huge_page_shift(h);
	struct resv_map *resv_map;
	struct hugetlb_cgroup *h_cg;

	resv_map = inode_resv_map(inode);
	chg = region_chg(resv_map, from, to, &regions_needed);
	printk("chg: %ld regions needed: %ld\n", chg, regions_needed);
	if (chg < 0)
		return chg;

	/* TODO: Double-check accounting.  */
	/*
	 * TODO: Make sure the rsvd charging is needed even when pages are
	 * already allocated.
	 */
	if (hugetlb_cgroup_charge_cgroup_rsvd(hstate_index(h),
					      chg * pages_per_huge_page(h),
					      &h_cg) < 0)
		goto err_todo;

	/*
	 * No need for hugetlb_acct_memory(). Only the allocated folios are
	 * added right now, and those are already accounted for during hstate
	 * init as non-free.
	 *
	 * No need for subpool reservations as well since the memfds come from
	 * the internal mounts of hugetlbfs and that doesn't have subpools.
	 */
	add = region_add(resv_map, from, to, regions_needed, h, h_cg);
	printk("added: %ld\n", add);
	if (add < 0)
		goto err_todo;

	hugetlb_cgroup_put_rsvd_cgroup(h_cg);

	return 0;

err_todo:
	/* TODO */
	return -EINVAL;
}

static int huge_memfd_retrieve_folios(struct file *file,
				      struct huge_memfd_ser *memfd_ser)
{
	struct huge_memfd_folio_ser *folios_ser;
	struct inode *inode = file_inode(file);
	struct address_space *mapping;
	struct folio *folio;
	unsigned long nr_folios;
	long i = 0;
	int err;

	if (!memfd_ser->size)
		return 0;

	printk("order: %d\n", memfd_ser->order);

	folios_ser = kho_restore_vmalloc(&memfd_ser->folios);
	if (!folios_ser)
		return -ENOMEM;

	nr_folios = memfd_ser->nr_folios;
	mapping = inode->i_mapping;

	for (i = 0; i < nr_folios; i++) {
		struct huge_memfd_folio_ser *folio_ser = &folios_ser[i];

		printk("restoring pfn: 0x%lx index: 0x%lx\n",
		       (unsigned long)folio_ser->pfn,
		       (unsigned long)folio_ser->index);

		/*
		 * FLB already restored and prepped the folio, but double-check
		 * first.
		 */
		folio = pfn_folio(folio_ser->pfn);
		if (!folio_test_hugetlb_luo(folio)) {
			err = -EINVAL;
			folio_put(folio);
			printk("folio not hugetlb!\n");
			goto err_restore_folios;
		}

		folio_clear_hugetlb_luo(folio);
		/* TODO: make sure this always holds. */
		__folio_mark_uptodate(folio);
		/* TODO: If we are having to shift by order here, maybe just
		 * save the huge-size index instead of page cache index when
		 * preserving? */
		err = hugetlb_add_to_page_cache(folio, mapping,
						folio_ser->index >> memfd_ser->order);
		if (err) {
			printk("Failed to add to page cache: %pe\n", ERR_PTR(err));
			folio_put(folio);
			goto err_restore_folios;
		}
		/* TODO: charge the cgroup. */
		folio_unlock(folio);
		folio_put(folio);
	}

	vfree(folios_ser);
	return 0;

	/* TODO: Revisit. Make sure error handling is done right. */
err_restore_folios:
	/* TODO: Free the other folios of this file. */

	vfree(folios_ser);
	return err;
}

static int huge_memfd_retrieve(struct liveupdate_file_op_args *args)
{
	struct huge_memfd_ser *memfd_ser;
	struct folio *folio;
	struct file *file;
	int err;

	printk("In %s\n", __func__);

	folio = kho_restore_folio(args->serialized_data);
	if (!folio)
		return -ENOENT;

	memfd_ser = folio_address(folio);

	file = hugetlb_file_setup("", 0, VM_NORESERVE, HUGETLB_ANONHUGE_INODE,
				  memfd_ser->order + PAGE_SHIFT);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_put_folio;
	}

	vfs_setpos(file, memfd_ser->pos, MAX_LFS_FILESIZE);
	file->f_inode->i_size = memfd_ser->size;

	err = huge_memfd_setup_rsrv(file_inode(file));
	if (err)
		goto err_put_file;

	if (memfd_ser->nr_folios) {
		err = huge_memfd_retrieve_folios(file, memfd_ser);
		if (err)
			goto err_put_file;
	}

	args->file = file;
	folio_put(folio);
	return 0;

err_put_file:
	fput(file);
err_put_folio:
	folio_put(folio);
	return err;
}

static const struct liveupdate_file_ops huge_memfd_luo_ops = {
	.can_preserve = huge_memfd_can_preserve,
	.preserve = huge_memfd_preserve,
	.unpreserve = huge_memfd_unpreserve,
	.finish = huge_memfd_finish,
	.retrieve = huge_memfd_retrieve,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler huge_memfd_handler = {
	.ops = &huge_memfd_luo_ops,
	.compatible = "huge-memfd-v1",
};

void __init hugetlb_luo_init(void)
{
	void *obj;
	int err;

	if (!liveupdate_enabled())
		return;

	err = liveupdate_register_file_handler(&huge_memfd_handler);
	if (err) {
		pr_err("could not register file handler: %pe\n", ERR_PTR(err));
		return;
	}

	err = liveupdate_register_flb(&huge_memfd_handler, &hugetlb_luo_flb);
	if (err) {
		pr_err("could not register hugetlb FLB handler: %pe\n", ERR_PTR(err));
		liveupdate_unregister_file_handler(&huge_memfd_handler);
		return;
	}

	/*
	 * Trigger the FLB retrieve now so that the hstates are deserialized and
	 * free pages added to the pool. This is needed for non-LUO hugepage
	 * files to work. Without this, they will not have any hugepages to
	 * allocate from until the first hugetlb LUO file gets retrieved.
	 *
	 * If this fails, there is no real recovery. Live updated hugetlb files
	 * will not be available, and possibly no huge pages at all.
	 */
	err = liveupdate_flb_get_incoming(&hugetlb_luo_flb, &obj);
	if (err != -ENODATA && err != -ENOENT)
		pr_warn("could not retrtieve FLB data. hugetlb in unknown state.\n");
}
