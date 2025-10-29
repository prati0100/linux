// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 * Changyuan Lyu <changyuanl@google.com>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/file.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/liveupdate.h>
#include <linux/kexec_handover.h>
#include <linux/shmem_fs.h>
#include <linux/bits.h>
#include <linux/vmalloc.h>
#include "internal.h"

#define PRESERVED_PFN_MASK		GENMASK(63, 12)
#define PRESERVED_PFN_SHIFT		12
#define PRESERVED_FLAG_DIRTY		BIT(0)
#define PRESERVED_FLAG_UPTODATE		BIT(1)

#define PRESERVED_FOLIO_PFN(desc)	(((desc) & PRESERVED_PFN_MASK) >> PRESERVED_PFN_SHIFT)
#define PRESERVED_FOLIO_FLAGS(desc)	((desc) & ~PRESERVED_PFN_MASK)
#define PRESERVED_FOLIO_MKDESC(pfn, flags) (((pfn) << PRESERVED_PFN_SHIFT) | (flags))

struct memfd_luo_pfolio {
	/*
	 * The folio descriptor is made of 2 parts. The bottom 12 bits are used
	 * for storing flags, the others for storing the PFN.
	 */
	u64 foliodesc;
	u64 index;
};

struct memfd_luo_private {
	struct memfd_luo_pfolio *pfolios;
	u64 nr_folios;
};

static int memfd_luo_preserve_folios(struct memfd_luo_pfolio *pfolios,
				     struct folio **folios, u64 nr_folios)
{
	int err;
	long i;

	for (i = 0; i < nr_folios; i++) {
		struct memfd_luo_pfolio *pfolio = &pfolios[i];
		struct folio *folio = folios[i];
		unsigned int flags = 0;
		unsigned long pfn;

		err = kho_preserve_folio(folio);
		if (err)
			goto err_unpreserve;

		pfn = folio_pfn(folio);
		if (folio_test_dirty(folio))
			flags |= PRESERVED_FLAG_DIRTY;
		if (folio_test_uptodate(folio))
			flags |= PRESERVED_FLAG_UPTODATE;

		pfolio->foliodesc = PRESERVED_FOLIO_MKDESC(pfn, flags);
		pfolio->index = folio->index;
	}

	return 0;

err_unpreserve:
	i--;
	for (; i >= 0; i--)
		WARN_ON_ONCE(kho_unpreserve_folio(folios[i]));
	vfree(pfolios);
	return err;
}

static void memfd_luo_unpreserve_folios(struct memfd_luo_pfolio *pfolios,
					u64 nr_folios)
{
	long i;

	for (i = 0; i < nr_folios; i++) {
		const struct memfd_luo_pfolio *pfolio = &pfolios[i];
		struct folio *folio;

		if (!pfolio->foliodesc)
			continue;

		folio = pfn_folio(PRESERVED_FOLIO_PFN(pfolio->foliodesc));

		WARN_ON_ONCE(kho_unpreserve_folio(folio));
		unpin_folio(folio);
	}
}

static struct memfd_luo_pfolio *memfd_luo_fdt_folios(const void *fdt, u64 *nr_folios)
{
	const struct kho_vmalloc *vmalloc_handle;
	struct memfd_luo_pfolio *pfolios;
	const u64 *nr;
	int len;

	nr = fdt_getprop(fdt, 0, "nr_folios", &len);
	if (!nr || len != (sizeof(*nr))) {
		pr_err("invalid 'nr_folios' property\n");
		return NULL;
	}

	vmalloc_handle = fdt_getprop(fdt, 0, "folios", &len);
	if (!vmalloc_handle || len != sizeof(*vmalloc_handle)) {
		pr_err("invalid 'folios' property\n");
		return NULL;
	}
	
	pfolios = kho_restore_vmalloc(vmalloc_handle);
	if (!pfolios)
		return NULL;

	*nr_folios = *nr;
	return pfolios;
}

static void *memfd_luo_create_fdt(void)
{
	struct folio *fdt_folio;
	int err = 0;
	void *fdt;

	/*
	 * The FDT only contains a couple of properties and a kho_vmalloc
	 * object. One page should be enough for that.
	 */
	fdt_folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!fdt_folio)
		return NULL;

	fdt = folio_address(fdt_folio);

	err |= fdt_create(fdt, folio_size(fdt_folio));
	err |= fdt_finish_reservemap(fdt);
	err |= fdt_begin_node(fdt, "");
	if (err)
		goto free;

	return fdt;

free:
	folio_put(fdt_folio);
	return NULL;
}

static int memfd_luo_finish_fdt(void *fdt)
{
	int err;

	err = fdt_end_node(fdt);
	if (err)
		return err;

	return fdt_finish(fdt);
}

static int memfd_luo_preserve(struct liveupdate_file_op_args *args)
{
	struct memfd_luo_private *private = args->private;
	struct inode *inode = file_inode(args->file);
	struct memfd_luo_pfolio *pfolios;
	struct kho_vmalloc *kho_vmalloc;
	unsigned int max_folios;
	struct folio **folios;
	long size, nr_pinned;
	pgoff_t offset;
	int err = 0;
	void *fdt;
	u64 pos, nr_folios;

	inode_lock(inode);
	shmem_i_mapping_freeze(inode, true);

	size = i_size_read(inode);
	if ((PAGE_ALIGN(size) / PAGE_SIZE) > UINT_MAX) {
		err = -E2BIG;
		goto err_unlock;
	}

	/*
	 * Guess the number of folios based on inode size. Real number might end
	 * up being smaller if there are higher order folios.
	 */
	max_folios = PAGE_ALIGN(size) / PAGE_SIZE;
	folios = kvmalloc_array(max_folios, sizeof(*folios), GFP_KERNEL);
	if (!folios) {
		err = -ENOMEM;
		goto err_unfreeze;
	}

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
	nr_pinned = memfd_pin_folios(args->file, 0, size - 1, folios, max_folios,
				     &offset);
	if (nr_pinned < 0) {
		err = nr_pinned;
		pr_err("failed to pin folios: %d\n", err);
		goto err_free_folios;
	}
	nr_folios = nr_pinned;

	fdt = memfd_luo_create_fdt();
	if (!fdt) {
		err = -ENOMEM;
		goto err_unpin;
	}

	pos = args->file->f_pos;
	err = fdt_property(fdt, "pos", &pos, sizeof(pos));
	if (err)
		goto err_free_fdt;

	err = fdt_property(fdt, "size", &size, sizeof(size));
	if (err)
		goto err_free_fdt;

	err = fdt_property(fdt, "nr_folios", &nr_folios, sizeof(nr_folios));
	if (err)
		goto err_free_fdt;

	err = fdt_property_placeholder(fdt, "folios", sizeof(*kho_vmalloc),
				       (void **)&kho_vmalloc);
	if (err) {
		pr_err("Failed to reserve folios property in FDT: %s\n",
		       fdt_strerror(err));
		err = -ENOMEM;
		goto err_free_fdt;
	}

	pfolios = vcalloc(nr_folios, sizeof(*pfolios));
	if (!pfolios) {
		err = -ENOMEM;
		goto err_free_fdt;
	}
	private->pfolios = pfolios;
	private->nr_folios = nr_folios;

	err = memfd_luo_preserve_folios(pfolios, folios, nr_folios);
	if (err)
		goto err_free_pfolios;

	err = kho_preserve_vmalloc(pfolios, kho_vmalloc);
	if (err)
		goto err_unpreserve_folios;

	err = memfd_luo_finish_fdt(fdt);
	if (err)
		goto err_unpreserve_vmalloc;

	err = kho_preserve_folio(virt_to_folio(fdt));
	if (err)
		goto err_unpreserve_vmalloc;

	kvfree(folios);
	inode_unlock(inode);

	args->data = virt_to_phys(fdt);
	return 0;

err_unpreserve_vmalloc:
	kho_unpreserve_vmalloc(kho_vmalloc);
err_unpreserve_folios:
	memfd_luo_unpreserve_folios(pfolios, nr_folios);
err_free_pfolios:
	vfree(pfolios);
err_free_fdt:
	folio_put(virt_to_folio(fdt));
err_unpin:
	unpin_folios(folios, nr_folios);
err_free_folios:
	kvfree(folios);
err_unfreeze:
	shmem_i_mapping_freeze(inode, false);
err_unlock:
	inode_unlock(inode);
	return err;
}

static int memfd_luo_freeze(struct liveupdate_file_op_args *args)
{
	u64 pos = args->file->f_pos;
	void *fdt;
	int err;

	if (WARN_ON_ONCE(!args->data))
		return -EINVAL;

	fdt = phys_to_virt(args->data);

	/*
	 * The pos might have changed since prepare. Everything else stays the
	 * same.
	 */
	err = fdt_setprop(fdt, 0, "pos", &pos, sizeof(pos));
	if (err)
		return err;

	return 0;
}

static void memfd_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	const struct kho_vmalloc *vmalloc_handle;
	struct memfd_luo_private *private = args->private;
	struct inode *inode = file_inode(args->file);
	struct folio *fdt_folio;
	void *fdt;

	if (WARN_ON_ONCE(!args->data))
		return;

	inode_lock(inode);
	shmem_i_mapping_freeze(inode, false);

	fdt = phys_to_virt(args->data);
	fdt_folio = virt_to_folio(fdt);

	vmalloc_handle = fdt_getprop(fdt, 0, "folios", NULL);
	/*
	 * Less error checks here since FDT was created by this kernel so
	 * expecting it to be sane.
	 */
	WARN_ON_ONCE(!vmalloc_handle);
	if (vmalloc_handle)
		kho_unpreserve_vmalloc(vmalloc_handle);

	memfd_luo_unpreserve_folios(private->pfolios, private->nr_folios);

	kho_unpreserve_folio(fdt_folio);
	folio_put(fdt_folio);
	vfree(private->pfolios);
	inode_unlock(inode);
}

static struct folio *memfd_luo_get_fdt(u64 data)
{
	return kho_restore_folio((phys_addr_t)data);
}

static void memfd_luo_discard_folios(struct memfd_luo_pfolio *pfolios,
				     long nr_folios)
{
	
	unsigned int i;

	for (i = 0; i < nr_folios; i++) {
		const struct memfd_luo_pfolio *pfolio = &pfolios[i];
		struct folio *folio;
		phys_addr_t phys;

		if (!pfolio->foliodesc)
			continue;

		phys = PFN_PHYS(PRESERVED_FOLIO_PFN(pfolio->foliodesc));
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
	struct memfd_luo_pfolio *pfolios;
	struct folio *fdt_folio;
	const void *fdt;
	u64 nr_folios;

	if (args->reclaimed)
		return;

	fdt_folio = memfd_luo_get_fdt(args->data);
	if (!fdt_folio) {
		pr_err("failed to restore memfd FDT\n");
		return;
	}

	fdt = folio_address(fdt_folio);

	pfolios = memfd_luo_fdt_folios(fdt, &nr_folios);
	if (!pfolios)
		goto out;

	memfd_luo_discard_folios(pfolios, nr_folios);
	vfree(pfolios);

out:
	folio_put(fdt_folio);
}

static int memfd_luo_retrieve(struct liveupdate_file_op_args *args)
{
	struct memfd_luo_pfolio *pfolios;
	struct address_space *mapping;
	struct folio *folio, *fdt_folio;
	int len, ret = 0, i = 0;
	const u64 *pos, *size;
	struct inode *inode;
	struct file *file;
	const void *fdt;
	u64 nr_folios;

	fdt_folio = memfd_luo_get_fdt(args->data);
	if (!fdt_folio)
		return -ENOENT;

	fdt = page_to_virt(folio_page(fdt_folio, 0));

	pfolios = memfd_luo_fdt_folios(fdt, &nr_folios);
	if (!pfolios) {
		pr_err("failed to fetch preserved folio list\n");
		ret = -EINVAL;
		goto put_fdt;
	}

	size = fdt_getprop(fdt, 0, "size", &len);
	if (!size || len != sizeof(u64)) {
		pr_err("invalid 'size' property\n");
		ret = -EINVAL;
		goto put_folios;
	}

	pos = fdt_getprop(fdt, 0, "pos", &len);
	if (!pos || len != sizeof(u64)) {
		pr_err("invalid 'pos' property\n");
		ret = -EINVAL;
		goto put_folios;
	}

	file = shmem_file_setup("", 0, VM_NORESERVE);

	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		pr_err("failed to setup file: %d\n", ret);
		goto put_folios;
	}

	inode = file->f_inode;
	mapping = inode->i_mapping;
	vfs_setpos(file, *pos, MAX_LFS_FILESIZE);

	for (; i < nr_folios; i++) {
		const struct memfd_luo_pfolio *pfolio = &pfolios[i];
		phys_addr_t phys;
		u64 index;
		int flags;

		if (!pfolio->foliodesc)
			continue;

		phys = PFN_PHYS(PRESERVED_FOLIO_PFN(pfolio->foliodesc));
		folio = kho_restore_folio(phys);
		if (!folio) {
			pr_err("Unable to restore folio at physical address: %llx\n",
			       phys);
			goto put_file;
		}
		index = pfolio->index;
		flags = PRESERVED_FOLIO_FLAGS(pfolio->foliodesc);

		/* Set up the folio for insertion. */
		__folio_set_locked(folio);
		__folio_set_swapbacked(folio);

		ret = mem_cgroup_charge(folio, NULL, mapping_gfp_mask(mapping));
		if (ret) {
			pr_err("shmem: failed to charge folio index %d: %d\n",
			       i, ret);
			goto unlock_folio;
		}

		ret = shmem_add_to_page_cache(folio, mapping, index, NULL,
					      mapping_gfp_mask(mapping));
		if (ret) {
			pr_err("shmem: failed to add to page cache folio index %d: %d\n",
			       i, ret);
			goto unlock_folio;
		}

		if (flags & PRESERVED_FLAG_UPTODATE)
			folio_mark_uptodate(folio);
		if (flags & PRESERVED_FLAG_DIRTY)
			folio_mark_dirty(folio);

		ret = shmem_inode_acct_blocks(inode, 1);
		if (ret) {
			pr_err("shmem: failed to account folio index %d: %d\n",
			       i, ret);
			goto unlock_folio;
		}

		shmem_recalc_inode(inode, 1, 0);
		folio_add_lru(folio);
		folio_unlock(folio);
		folio_put(folio);
	}

	inode->i_size = *size;
	args->file = file;
	folio_put(fdt_folio);
	vfree(pfolios);
	return 0;

unlock_folio:
	folio_unlock(folio);
	folio_put(folio);
put_file:
	fput(file);
	i++;
put_folios:
	for (; i < nr_folios; i++) {
		const struct memfd_luo_pfolio *pfolio = &pfolios[i];

		folio = kho_restore_folio(PRESERVED_FOLIO_PFN(pfolio->foliodesc));
		if (folio)
			folio_put(folio);
	}

	vfree(pfolios);
put_fdt:
	folio_put(fdt_folio);
	return ret;
}

static bool memfd_luo_can_preserve(struct liveupdate_file_handler *handler,
				   struct file *file)
{
	struct inode *inode = file_inode(file);

	return shmem_file(file) && !inode->i_nlink;
}

static const struct liveupdate_file_ops memfd_luo_file_ops = {
	.freeze = memfd_luo_freeze,
	.finish = memfd_luo_finish,
	.retrieve = memfd_luo_retrieve,
	.preserve = memfd_luo_preserve,
	.unpreserve = memfd_luo_unpreserve,
	.can_preserve = memfd_luo_can_preserve,
	.owner = THIS_MODULE,
	.private_size = sizeof(struct memfd_luo_private),
};

static struct liveupdate_file_handler memfd_luo_handler = {
	.ops = &memfd_luo_file_ops,
	.compatible = "memfd-v1",
};

static int __init memfd_luo_init(void)
{
	int err;

	err = liveupdate_register_file_handler(&memfd_luo_handler);
	if (err)
		pr_err("Could not register luo filesystem handler: %d\n", err);

	return err;
}
late_initcall(memfd_luo_init);
