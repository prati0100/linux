/* TODO: License, copyright, etc. */
#ifndef _LINUX_KHO_ABI_XARRAY_H
#define _LINUX_KHO_ABI_XARRAY_H

#include <linux/kho/abi/kexec_handover.h>
#include <linux/log2.h>
#include <linux/math.h>

#include <asm/page.h>

/* TODO: doc */

enum kho_xarray_consts {
	/* Adds up to a depth of 6. */
	KHO_XARRAY_KEY_WDITH = 54,

	KHO_XARRAY_TBL_ENTRIES = PAGE_SIZE / sizeof(u64),
	KHO_XARRAY_TBL_SHIFT = const_ilog2(KHO_XARRAY_TBL_ENTRIES),
	/* KHO_XARRAY_TBL_ENTRIES is a power of 2. */
	KHO_XARRAY_TBL_MASK = KHO_XARRAY_TBL_ENTRIES - 1,

	KHO_XARRAY_DEPTH = DIV_ROUND_UP(KHO_XARRAY_KEY_WDITH, KHO_XARRAY_TBL_SHIFT),
};

struct kho_xarray_node {
	DECLARE_KHOSER_PTR(table, struct kho_xarray_node *)[1 << KHO_XARRAY_TBL_SHIFT];
};

struct kho_xarray_leaf {
	u64 values[KHO_XARRAY_TBL_ENTRIES];
};

struct kho_xarray {
	DECLARE_KHOSER_PTR(root, struct kho_xarray_node *);
};

#endif /* _LINUX_KHO_ABI_XARRAY_H */
