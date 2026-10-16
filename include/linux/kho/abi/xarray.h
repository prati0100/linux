/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pratyush Yadav <pratyush@kernel.org>
 */

#ifndef _LINUX_KHO_ABI_XARRAY_H
#define _LINUX_KHO_ABI_XARRAY_H

#include <linux/kho/abi/kexec_handover.h>
#include <linux/log2.h>
#include <linux/math.h>

#include <asm/page.h>

/**
 * DOC: KHO xarray ABI
 *
 * The ABI defined below is used for passing a KHO xarray from one kernel to the
 * next.
 *
 * - ``struct kho_xarray`` holds the physical address of the root node, or 0
 *   for an empty tree.
 * - The tree always has %KHO_XARRAY_DEPTH levels. Level
 *   ``KHO_XARRAY_DEPTH - 1`` is the root and level 0 holds the leaves.
 * - Every node and leaf is a single page, preserved as an order 0 folio.
 * - A key is at most %KHO_XARRAY_KEY_WDITH bits wide. It is split into
 *   %KHO_XARRAY_TBL_SHIFT bit indices, one per level, with the most
 *   significant bits indexing the root and the least significant bits
 *   indexing the leaf.
 * - Intermediate nodes (``struct kho_xarray_node``) hold the physical addresses
 *   of the nodes one level below, or 0 for none.
 * - Leaves (&struct kho_xarray_leaf) hold the values, where 0 means there is
 *   no entry.
 *
 * The KHO xarray has no version of its own. Any change to this layout,
 * including to %KHO_XARRAY_KEY_WDITH, is a breaking change for every user that
 * hands over a KHO xarray, and requires bumping their ABI versions.
 */

/**
 * enum kho_xarray_consts - Constants describing the KHO xarray layout.
 * @KHO_XARRAY_KEY_WDITH: Maximum width of a key, in bits.
 * @KHO_XARRAY_TBL_ENTRIES: Number of entries in a node or leaf.
 * @KHO_XARRAY_TBL_SHIFT: Number of key bits used as the index at each level.
 * @KHO_XARRAY_TBL_MASK: Mask for the index at each level.
 * @KHO_XARRAY_DEPTH: Number of levels in the tree.
 */
enum kho_xarray_consts {
	/* TODO: support dynamic width? */
	/* Adds up to a depth of 6. */
	KHO_XARRAY_KEY_WDITH = 54,

	KHO_XARRAY_TBL_ENTRIES = PAGE_SIZE / sizeof(u64),
	KHO_XARRAY_TBL_SHIFT = const_ilog2(KHO_XARRAY_TBL_ENTRIES),
	/* KHO_XARRAY_TBL_ENTRIES is a power of 2. */
	KHO_XARRAY_TBL_MASK = KHO_XARRAY_TBL_ENTRIES - 1,

	KHO_XARRAY_DEPTH = DIV_ROUND_UP(KHO_XARRAY_KEY_WDITH, KHO_XARRAY_TBL_SHIFT),
};

/*
 * An intermediate node of a KHO xarray. @table holds the physical addresses of
 * the nodes one level below, or 0 for none, indexed by the bits of the key for
 * this level.
 *
 * Not kernel-doc, since kernel-doc can't parse DECLARE_KHOSER_PTR() members.
 */
struct kho_xarray_node {
	DECLARE_KHOSER_PTR(table, struct kho_xarray_node *)[1 << KHO_XARRAY_TBL_SHIFT];
};

/**
 * struct kho_xarray_leaf - A leaf of a KHO xarray.
 * @values: Values of the entries, or 0 for no entry. Indexed by the lowest
 *          %KHO_XARRAY_TBL_SHIFT bits of the key.
 */
struct kho_xarray_leaf {
	u64 values[KHO_XARRAY_TBL_ENTRIES];
};

/*
 * A KHO xarray. @root holds the physical address of the root node, or 0 for an
 * empty tree.
 *
 * Not kernel-doc, since kernel-doc can't parse DECLARE_KHOSER_PTR() members.
 */
struct kho_xarray {
	/* TODO: size and flags */
	DECLARE_KHOSER_PTR(root, struct kho_xarray_node *);
};

#endif /* _LINUX_KHO_ABI_XARRAY_H */
