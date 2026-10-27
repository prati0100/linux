/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pratyush Yadav <pratyush@kernel.org>
 */

#ifndef _LINUX_KHO_ABI_MAP_H
#define _LINUX_KHO_ABI_MAP_H

#include <linux/kho/abi/kexec_handover.h>
#include <linux/log2.h>
#include <linux/math.h>

#include <asm/page.h>

/**
 * DOC: KHO map ABI
 *
 * The ABI defined below is used for passing a KHO map from one kernel to the
 * next.
 *
 * - ``struct kho_map_ser`` holds the physical address of the root node, or
 *   0 for an empty tree.
 * - The tree always has %KHO_MAP_DEPTH levels. Level
 *   ``KHO_MAP_DEPTH - 1`` is the root and level 0 holds the leaves.
 * - Every node and leaf is a single page, preserved as an order 0 folio.
 * - A key is at most %KHO_MAP_KEY_WIDTH bits wide. It is split into
 *   %KHO_MAP_TBL_SHIFT bit indices, one per level, with the most
 *   significant bits indexing the root and the least significant bits
 *   indexing the leaf.
 * - Intermediate nodes (``struct kho_map_node_ser``) hold the physical
 *   addresses of the nodes one level below, or 0 for none.
 * - Leaves (&struct kho_map_leaf_ser) hold the values, where 0 means there
 *   is no entry.
 *
 * The KHO map has no version of its own. Any change to this layout,
 * including to %KHO_MAP_KEY_WIDTH, is a breaking change for every user that
 * hands over a KHO map, and requires bumping their ABI versions.
 */

/**
 * enum kho_map_consts - Constants describing the KHO map layout.
 * @KHO_MAP_KEY_WIDTH: Maximum width of a key, in bits.
 * @KHO_MAP_TBL_ENTRIES: Number of entries in a node or leaf.
 * @KHO_MAP_TBL_SHIFT: Number of key bits used as the index at each level.
 * @KHO_MAP_TBL_MASK: Mask for the index at each level.
 * @KHO_MAP_DEPTH: Number of levels in the tree.
 */
enum kho_map_consts {
	/* TODO: support dynamic width? */
	/* Adds up to a depth of 6. */
	KHO_MAP_KEY_WIDTH = 54,

	KHO_MAP_TBL_ENTRIES = PAGE_SIZE / sizeof(u64),
	KHO_MAP_TBL_SHIFT = const_ilog2(KHO_MAP_TBL_ENTRIES),
	/* KHO_MAP_TBL_ENTRIES is a power of 2. */
	KHO_MAP_TBL_MASK = KHO_MAP_TBL_ENTRIES - 1,

	KHO_MAP_DEPTH = DIV_ROUND_UP(KHO_MAP_KEY_WIDTH, KHO_MAP_TBL_SHIFT),
};

/*
 * An intermediate node of a KHO map. @table holds the physical addresses of
 * the nodes one level below, or 0 for none, indexed by the bits of the key for
 * this level.
 *
 * Not kernel-doc, since kernel-doc can't parse DECLARE_KHOSER_PTR() members.
 */
struct kho_map_node_ser {
	DECLARE_KHOSER_PTR(table, struct kho_map_node_ser *)[1 << KHO_MAP_TBL_SHIFT];
};

/**
 * struct kho_map_leaf_ser - A leaf of a KHO map.
 * @values: Values of the entries, or 0 for no entry. Indexed by the lowest
 *          %KHO_MAP_TBL_SHIFT bits of the key.
 */
struct kho_map_leaf_ser {
	u64 values[KHO_MAP_TBL_ENTRIES];
};

/*
 * The serialized form of a KHO map. @root holds the physical address of the
 * root node, or 0 for an empty tree. Users embed this in their own serialized
 * state to hand the tree over.
 *
 * Not kernel-doc, since kernel-doc can't parse DECLARE_KHOSER_PTR() members.
 */
struct kho_map_ser {
	/* TODO: size and flags */
	DECLARE_KHOSER_PTR(root, struct kho_map_node_ser *);
};

#endif /* _LINUX_KHO_ABI_MAP_H */
