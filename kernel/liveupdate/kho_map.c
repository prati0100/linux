// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Pratyush Yadav <pratyush@kernel.org>
 */

/**
 * DOC: KHO map
 *
 * A KHO map is a sparse mapping from integer keys (at most %KHO_MAP_KEY_WIDTH
 * wide) to non-zero u64 values, kept in memory that can be preserved with KHO.
 *
 * Internally it is a radix tree of fixed depth, with one page per node.
 *
 * A tree has two parts. ``struct kho_map_ser`` is its serialized form,
 * which points to the root node. To hand a tree over, embed its
 * ``struct kho_map_ser`` in the serialized payload. ``struct kho_map`` is
 * the runtime handle of the tree. It points to the ``struct kho_map_ser``
 * and is what all kho_map_*() functions operate on.
 *
 * A tree starts out unpreserved. kho_map_preserve() preserves every node of the
 * tree with KHO. From then on, nodes are preserved as soon as they are
 * allocated, so the tree is ready to be handed over at any point.
 * kho_map_destroy() frees the nodes, and unpreserves them if needed. A tree is
 * restored with kho_map_restore(), and comes out unpreserved.
 *
 * Things to keep in mind:
 *
 * - Nodes are never freed when their entries are removed, only by
 *   kho_map_destroy(). The memory used by a tree only grows until then.
 * - There is no internal locking. Callers must serialize all accesses to a
 *   tree, including lookups that race with modifications.
 */

#include <linux/io.h>
#include <linux/mm.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/map.h>
#include <linux/kho_map.h>

/**
 * kho_map_init() - Initialize an empty KHO map.
 * @map: The KHO map.
 * @ser: The serialized form of the tree. Can be handed over to the next kernel
 *       once the tree is populated.
 *
 * Any previous contents of @ser are discarded. @ser must stay around for as
 * long as @map is used.
 */
void kho_map_init(struct kho_map *map, struct kho_map_ser *ser)
{
	map->ser = ser;
	map->preserved = false;
	KHOSER_CLEAR_PTR(ser->root);
}

static unsigned int kho_map_node_idx(u64 key, unsigned int level)
{
	return (key >> (level * KHO_MAP_TBL_SHIFT)) & KHO_MAP_TBL_MASK;
}

static struct kho_map_node_ser *kho_map_alloc_node(struct kho_map *map)
{
	struct folio *folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

	if (!folio)
		return NULL;

	if (map->preserved && kho_preserve_folio(folio)) {
		folio_put(folio);
		return NULL;
	}

	return folio_address(folio);
}

static void kho_map_free_node(struct kho_map *map, void *node)
{
	struct folio *folio = virt_to_folio(node);

	if (map->preserved)
		kho_unpreserve_folio(folio);
	folio_put(folio);
}

static int kho_map_restore_folio(void *node)
{
	struct folio *folio = kho_restore_folio(virt_to_phys(node));

	if (!folio)
		return -EINVAL;

	if (WARN_ON_ONCE(folio_order(folio))) {
		folio_put(folio);
		return -EINVAL;
	}

	return 0;
}

static void __kho_map_destroy_node(struct kho_map *map,
				  struct kho_map_node_ser *node,
				  unsigned int level)
{
	unsigned int i;

	/* Level 0 nodes are leaves and hold values, not pointers. */
	if (level == 0) {
		kho_map_free_node(map, node);
		return;
	}

	for (i = 0; i < KHO_MAP_TBL_ENTRIES; i++) {
		struct kho_map_node_ser *next = KHOSER_LOAD_PTR(node->table[i]);

		if (next)
			__kho_map_destroy_node(map, next, level - 1);
	}

	kho_map_free_node(map, node);
}

/**
 * kho_map_destroy() - Free all nodes of a KHO map.
 * @map: The KHO map.
 *
 * Frees every node, leaving an empty tree behind that can be used again. If
 * the tree is preserved, the nodes are unpreserved first.
 *
 * Post-KHO, this can only be called after kho_map_restore().
 */
void kho_map_destroy(struct kho_map *map)
{
	struct kho_map_node_ser *root = KHOSER_LOAD_PTR(map->ser->root);

	if (!root)
		return;

	__kho_map_destroy_node(map, root, KHO_MAP_DEPTH - 1);
	KHOSER_CLEAR_PTR(map->ser->root);
}

/* Unpreserve @node and everything below it. */
static void __kho_map_unpreserve_node(struct kho_map_node_ser *node,
				     unsigned int level)
{
	unsigned int i;

	/* Level 0 nodes are leaves and hold values, not pointers. */
	for (i = 0; level && i < KHO_MAP_TBL_ENTRIES; i++) {
		struct kho_map_node_ser *next = KHOSER_LOAD_PTR(node->table[i]);

		if (next)
			__kho_map_unpreserve_node(next, level - 1);
	}

	kho_unpreserve_folio(virt_to_folio(node));
}

/*
 * Preserve @node and everything below it. On failure, everything that was
 * preserved is unpreserved again.
 */
static int __kho_map_preserve_node(struct kho_map_node_ser *node,
				  unsigned int level)
{
	unsigned int i, j;
	int err;

	err = kho_preserve_folio(virt_to_folio(node));
	if (err)
		return err;

	/* Level 0 nodes are leaves and hold values, not pointers. */
	if (level == 0)
		return 0;

	for (i = 0; i < KHO_MAP_TBL_ENTRIES; i++) {
		struct kho_map_node_ser *next = KHOSER_LOAD_PTR(node->table[i]);

		if (!next)
			continue;

		err = __kho_map_preserve_node(next, level - 1);
		if (err)
			goto err_unpreserve;
	}

	return 0;

err_unpreserve:
	/* next has already unpreserved itself, undo the entries before it. */
	for (j = 0; j < i; j++) {
		struct kho_map_node_ser *next = KHOSER_LOAD_PTR(node->table[j]);

		if (next)
			__kho_map_unpreserve_node(next, level - 1);
	}
	kho_unpreserve_folio(virt_to_folio(node));
	return err;
}

/**
 * kho_map_preserve() - Preserve a KHO map.
 * @map: The KHO map.
 *
 * Preserves every node of the tree with KHO, and marks the tree as preserved,
 * so that nodes allocated from now on are preserved as well. Afterwards the
 * tree can be handed over to the next kernel at any point, by handing over its
 * &struct kho_map_ser. Preserving a tree that is already preserved does
 * nothing.
 *
 * There is no way to unpreserve the tree short of kho_map_destroy().
 *
 * Context: Can sleep.
 * Return: 0 on success, -errno on failure. On failure the tree is left
 * unpreserved.
 */
int kho_map_preserve(struct kho_map *map)
{
	struct kho_map_node_ser *root = KHOSER_LOAD_PTR(map->ser->root);
	int err;

	if (map->preserved)
		return 0;

	if (root) {
		err = __kho_map_preserve_node(root, KHO_MAP_DEPTH - 1);
		if (err)
			return err;
	}

	map->preserved = true;
	return 0;
}

/*
 * Restore @node and everything below it.
 *
 * On failure, everything that was restored is freed again and the caller must
 * drop its reference to @node. Nodes that could not be restored are leaked.
 */
static int __kho_map_restore_node(struct kho_map *map,
				 struct kho_map_node_ser *node,
				 unsigned int level)
{
	unsigned int i, j;
	int err;

	err = kho_map_restore_folio(node);
	if (err)
		return err;

	/* Level 0 nodes are leaves and hold values, not pointers. */
	if (level == 0)
		return 0;

	for (i = 0; i < KHO_MAP_TBL_ENTRIES; i++) {
		struct kho_map_node_ser *next = KHOSER_LOAD_PTR(node->table[i]);

		if (!next)
			continue;

		err = __kho_map_restore_node(map, next, level - 1);
		if (err)
			goto err_destroy;
	}

	return 0;

err_destroy:
	/*
	 * next has already freed itself, and the entries after it were never
	 * restored. Cut them all off, then free what is left.
	 */
	for (j = i; j < KHO_MAP_TBL_ENTRIES; j++)
		KHOSER_CLEAR_PTR(node->table[j]);
	__kho_map_destroy_node(map, node, level);
	return err;
}

/**
 * kho_map_restore() - Restore KHO map.
 * @map: The KHO map to set up.
 * @ser: The serialized form of the tree, handed over by the previous kernel.
 *
 * Restores every node of the tree in @ser, and sets up @map to refer to it.
 * Afterwards @map can be used with all other kho_map_*() functions, exactly
 * like a tree built in this kernel. @ser must stay around for as long as @map
 * is used.
 *
 * The restored tree is unpreserved, so it is not handed over to the next kernel
 * again unless kho_map_preserve() is called on it.
 *
 * Return: 0 on success, or -errno on failure. On failure the tree is destroyed,
 * and nodes that could not be restored are leaked. @map is still set up and
 * refers to an empty tree.
 */
int kho_map_restore(struct kho_map *map, struct kho_map_ser *ser)
{
	struct kho_map_node_ser *root = KHOSER_LOAD_PTR(ser->root);
	int err;

	map->ser = ser;
	map->preserved = false;

	if (!root)
		return 0;

	err = __kho_map_restore_node(map, root, KHO_MAP_DEPTH - 1);
	if (err)
		KHOSER_CLEAR_PTR(map->ser->root);

	return err;
}

/**
 * kho_map_set() - Store a value in a KHO map.
 * @map: The KHO map.
 * @key: Key of the entry. At most %KHO_MAP_KEY_WIDTH bits wide.
 * @value: Value to store.
 *
 * Adds an entry, or replaces the value of an existing one.
 *
 * A value of 0 means the entry is not present, so storing 0 removes the entry.
 * Removing never allocates nodes and can't fail for keys in range. Nodes that
 * become empty are not freed, they stay in the tree until kho_map_destroy().
 *
 * Context: Can sleep when @value is not 0.
 * Return: 0 on success, -errno on failure.
 */
int kho_map_set(struct kho_map *map, u64 key, u64 value)
{
	struct kho_map_node_ser *root = KHOSER_LOAD_PTR(map->ser->root), *node;
	struct kho_map_leaf_ser *leaf;
	unsigned int i, idx;

	if (unlikely(fls64(key) > KHO_MAP_KEY_WIDTH))
		return -ERANGE;

	if (!root) {
		/* No tree, so the entry is already clear. */
		if (!value)
			return 0;

		root = kho_map_alloc_node(map);
		if (!root)
			return -ENOMEM;

		KHOSER_STORE_PTR(map->ser->root, root);
	}

	node = root;

	for (i = KHO_MAP_DEPTH - 1; i > 0; i--) {
		struct kho_map_node_ser *next;

		idx = kho_map_node_idx(key, i);

		next = KHOSER_LOAD_PTR(node->table[idx]);
		/* Node already exists, go down to the next level. */
		if (next) {
			node = next;
			continue;
		}

		/* No node on the path, so the entry is already clear. */
		if (!value)
			return 0;

		next = kho_map_alloc_node(map);
		if (!next)
			/* kho_map_destroy() should clean up leftover nodes. */
			return -ENOMEM;

		KHOSER_STORE_PTR(node->table[idx], next);
		node = next;
	}

	/* node now points to the leaf node. */
	leaf = (struct kho_map_leaf_ser *)node;
	idx = kho_map_node_idx(key, 0);
	leaf->values[idx] = value;

	return 0;
}

/**
 * kho_map_get() - Look up a value in a KHO map.
 * @map: The KHO map.
 * @key: Key of the entry.
 *
 * Return: The value of the entry, or 0 if there is no entry for @key or it is
 * out of range.
 */
u64 kho_map_get(struct kho_map *map, u64 key)
{
	struct kho_map_node_ser *node = KHOSER_LOAD_PTR(map->ser->root);
	struct kho_map_leaf_ser *leaf;
	unsigned int i;

	if (unlikely(fls64(key) > KHO_MAP_KEY_WIDTH))
		return 0;

	for (i = KHO_MAP_DEPTH - 1; node && i > 0; i--)
		node = KHOSER_LOAD_PTR(node->table[kho_map_node_idx(key, i)]);

	if (!node)
		return 0;

	leaf = (struct kho_map_leaf_ser *)node;
	return leaf->values[kho_map_node_idx(key, 0)];
}

static u64 __kho_map_next_leaf(struct kho_map_leaf_ser *leaf, u64 key, u64 *value)
{
	unsigned int i, idx;

	idx = kho_map_node_idx(key, 0);

	for (i = idx; i < KHO_MAP_TBL_ENTRIES; i++) {
		if (!leaf->values[i])
			continue;

		key = (key & ~KHO_MAP_TBL_MASK) | i;
		*value = leaf->values[i];
		return key;
	}

	return U64_MAX;
}

static u64 __kho_map_next_node(struct kho_map_node_ser *node, u64 key, u64 *value,
			       unsigned int level)
{
	/* Number of keys covered by a single entry at this level. */
	u64 span = 1UL << (level * KHO_MAP_TBL_SHIFT);
	unsigned int i;
	u64 found;

	/*
	 * The first entry is searched starting from key. Every following
	 * entry is searched from the first key of the subnode.
	 */
	for (i = kho_map_node_idx(key, level); i < KHO_MAP_TBL_ENTRIES;
	     i++, key = round_down(key, span) + span) {
		struct kho_map_node_ser *next;

		next = KHOSER_LOAD_PTR(node->table[i]);
		if (!next)
			continue;

		if (level == 1)
			found = __kho_map_next_leaf((struct kho_map_leaf_ser *)next, key, value);
		else
			found = __kho_map_next_node(next, key, value, level - 1);

		if (found != U64_MAX)
			return found;
	}

	return U64_MAX;
}

/**
 * kho_map_next() - Find the next entry in a KHO map.
 * @map: The KHO map.
 * @key: Key to start searching from.
 * @value: Set to the value of the entry found. Left alone if none is found.
 *
 * Finds the entry with the smallest key that is greater than or equal to @key.
 *
 * Return: The key of the entry found, or %U64_MAX if there is none. %U64_MAX
 * is never a valid key, since it is wider than %KHO_MAP_KEY_WIDTH.
 */
u64 kho_map_next(struct kho_map *map, u64 key, u64 *value)
{
	struct kho_map_node_ser *root = KHOSER_LOAD_PTR(map->ser->root);

	if (unlikely(!root))
		return U64_MAX;

	if (unlikely(fls64(key) > KHO_MAP_KEY_WIDTH))
		return U64_MAX;

	return __kho_map_next_node(root, key, value, KHO_MAP_DEPTH - 1);
}
