// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Pratyush Yadav <pratyush@kernel.org>
 */

/**
 * DOC: KHO xarray (eXtensible array)
 *
 * TODO: Should I choose a less confusing name?
 *
 * The KHO xarray is a sparse map from integer keys (at most
 * %KHO_XARRAY_KEY_WDITH wide) to non-zero u64 values, kept in memory that is
 * preserved with KHO.
 *
 * Internally it is a radix tree of fixed depth, with one page per node. Each
 * node is preserved as soon as it is allocated, so the tree is ready to be
 * handed over at any point. A tree is referenced by a ``struct kho_xarray``. To
 * hand a tree over, embed its ``struct kho_xarray`` in the serialized payload.
 *
 * Things to keep in mind:
 *
 * - Nodes are never freed when their entries are removed, only by
 *   kho_xa_destroy(). The memory used by a tree only grows until then.
 * - There is no internal locking. Callers must serialize all accesses to a
 *   tree, including lookups that race with modifications.
 */

#include <linux/io.h>
#include <linux/mm.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/xarray.h>
#include <linux/kho_xarray.h>

/**
 * kho_xa_init() - Initialize an empty KHO xarray.
 * @kxa: The KHO xarray.
 */
void kho_xa_init(struct kho_xarray *kxa)
{
	KHOSER_CLEAR_PTR(kxa->root);
}

static unsigned int kho_xa_node_idx(u64 key, unsigned int level)
{
	return (key >> (level * KHO_XARRAY_TBL_SHIFT)) & KHO_XARRAY_TBL_MASK;
}

static struct kho_xarray_node *kho_xa_alloc_node(void)
{
	struct folio *folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

	if (!folio)
		return NULL;

	if (kho_preserve_folio(folio)) {
		folio_put(folio);
		return NULL;
	}

	return folio_address(folio);
}

static void kho_xa_free_node(void *node)
{
	struct folio *folio = virt_to_folio(node);

	kho_unpreserve_folio(folio);
	folio_put(folio);
}

static int kho_xa_restore_folio(void *node)
{
	struct folio *folio = kho_restore_folio(virt_to_phys(node));
	int err;

	if (!folio)
		return -EINVAL;

	if (WARN_ON_ONCE(folio_order(folio))) {
		err = -EINVAL;
		goto err_put_folio;
	}

	/* Re-preserve the folio so all other kho_xa_*() functions work. */
	err = kho_preserve_folio(folio);
	if (err)
		goto err_put_folio;

	return 0;

err_put_folio:
	folio_put(folio);
	return err;
}

static void __kho_xa_destroy_node(struct kho_xarray_node *node,
				  unsigned int level)
{
	unsigned int i;

	/* Level 0 nodes are leaves and hold values, not pointers. */
	if (level == 0) {
		kho_xa_free_node(node);
		return;
	}

	for (i = 0; i < KHO_XARRAY_TBL_ENTRIES; i++) {
		struct kho_xarray_node *next = KHOSER_LOAD_PTR(node->table[i]);

		if (next)
			__kho_xa_destroy_node(next, level - 1);
	}

	kho_xa_free_node(node);
}

/**
 * kho_xa_destroy() - Free all nodes of a KHO xarray.
 * @kxa: The KHO xarray.
 *
 * Unpreserves and frees every node, leaving an empty tree behind that can be
 * used again.
 *
 * Post-KHO, this can only be called after kho_xa_restore() has succeeded.
 */
void kho_xa_destroy(struct kho_xarray *kxa)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root);

	if (!root)
		return;

	__kho_xa_destroy_node(root, KHO_XARRAY_DEPTH - 1);
	KHOSER_CLEAR_PTR(kxa->root);
}

/*
 * Restore @node and everything below it.
 *
 * On failure, everything that was restored is freed again and the caller must
 * drop its reference to @node. Nodes that could not be restored are leaked.
 */
static int __kho_xa_restore_node(struct kho_xarray_node *node,
				 unsigned int level)
{
	unsigned int i, j;
	int err;

	err = kho_xa_restore_folio(node);
	if (err)
		return err;

	/* Level 0 nodes are leaves and hold values, not pointers. */
	if (level == 0)
		return 0;

	for (i = 0; i < KHO_XARRAY_TBL_ENTRIES; i++) {
		struct kho_xarray_node *next = KHOSER_LOAD_PTR(node->table[i]);

		if (!next)
			continue;

		err = __kho_xa_restore_node(next, level - 1);
		if (err)
			goto err_destroy;
	}

	return 0;

err_destroy:
	/*
	 * next has already freed itself, and the entries after it were never
	 * restored. Cut them all off, then free what is left.
	 */
	for (j = i; j < KHO_XARRAY_TBL_ENTRIES; j++)
		KHOSER_CLEAR_PTR(node->table[j]);
	__kho_xa_destroy_node(node, level);
	return err;
}

/**
 * kho_xa_restore() - Restore KHO xarray.
 * @kxa: The KHO xarray
 *
 * Restores every node of the tree and preserves it again. Afterwards the tree
 * can be used with all other kho_xa_*() functions, exactly like a tree built
 * in this kernel.
 *
 * kho_xa_get(), kho_xa_next() and kho_xa_for_each() can be used before calling
 * this, but nothing that modifies or frees the tree.
 *
 * Return: 0 on success, or -errno on failure. On failure the tree is destroyed,
 * and nodes that could not be restored are leaked.
 */
int kho_xa_restore(struct kho_xarray *kxa)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root);
	int err;

	if (!root)
		return 0;

	err = __kho_xa_restore_node(root, KHO_XARRAY_DEPTH - 1);
	if (err)
		KHOSER_CLEAR_PTR(kxa->root);

	return err;
}

/**
 * kho_xa_set() - Store a value in a KHO xarray.
 * @kxa: The KHO xarray.
 * @key: Key of the entry. At most %KHO_XARRAY_KEY_WDITH bits wide.
 * @value: Value to store.
 *
 * Adds an entry, or replaces the value of an existing one.
 *
 * A value of 0 means the entry is not present, so storing 0 removes the entry.
 * Removing never allocates nodes and can't fail for keys in range. Nodes that
 * become empty are not freed, they stay in the tree until kho_xa_destroy().
 *
 * Context: Can sleep when @value is not 0.
 * Return: 0 on success, -errno on failure.
 */
int kho_xa_set(struct kho_xarray *kxa, u64 key, u64 value)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root), *node;
	struct kho_xarray_leaf *leaf;
	unsigned int i, idx;

	if (unlikely(fls64(key) > KHO_XARRAY_KEY_WDITH))
		return -ERANGE;

	if (!root) {
		/* No tree, so the entry is already clear. */
		if (!value)
			return 0;

		root = kho_xa_alloc_node();
		if (!root)
			return -ENOMEM;

		KHOSER_STORE_PTR(kxa->root, root);
	}

	node = root;

	for (i = KHO_XARRAY_DEPTH - 1; i > 0; i--) {
		struct kho_xarray_node *next;

		idx = kho_xa_node_idx(key, i);

		next = KHOSER_LOAD_PTR(node->table[idx]);
		/* Node already exists, go down to the next level. */
		if (next) {
			node = next;
			continue;
		}

		/* No node on the path, so the entry is already clear. */
		if (!value)
			return 0;

		next = kho_xa_alloc_node();
		if (!next)
			/* kho_xa_destroy() should clean up leftover nodes. */
			return -ENOMEM;

		KHOSER_STORE_PTR(node->table[idx], next);
		node = next;
	}

	/* node now points to the leaf node. */
	leaf = (struct kho_xarray_leaf *)node;
	idx = kho_xa_node_idx(key, 0);
	leaf->values[idx] = value;

	return 0;
}

/**
 * kho_xa_get() - Look up a value in a KHO xarray.
 * @kxa: The KHO xarray.
 * @key: Key of the entry.
 *
 * Return: The value of the entry, or 0 if there is no entry for @key or it is
 * out of range.
 */
u64 kho_xa_get(struct kho_xarray *kxa, u64 key)
{
	struct kho_xarray_node *node = KHOSER_LOAD_PTR(kxa->root);
	struct kho_xarray_leaf *leaf;
	unsigned int i;

	if (unlikely(fls64(key) > KHO_XARRAY_KEY_WDITH))
		return 0;

	for (i = KHO_XARRAY_DEPTH - 1; node && i > 0; i--)
		node = KHOSER_LOAD_PTR(node->table[kho_xa_node_idx(key, i)]);

	if (!node)
		return 0;

	leaf = (struct kho_xarray_leaf *)node;
	return leaf->values[kho_xa_node_idx(key, 0)];
}

static u64 __kho_xa_next_leaf(struct kho_xarray_leaf *leaf, u64 key, u64 *value)
{
	unsigned int i, idx;

	idx = kho_xa_node_idx(key, 0);

	for (i = idx; i < KHO_XARRAY_TBL_ENTRIES; i++) {
		if (!leaf->values[i])
			continue;

		key = (key & ~KHO_XARRAY_TBL_MASK) | i;
		*value = leaf->values[i];
		return key;
	}

	return U64_MAX;
}

static u64 __kho_xa_next_node(struct kho_xarray_node *node, u64 key, u64 *value,
			      unsigned int level)
{
	/* Number of keys covered by a single entry at this level. */
	u64 span = 1UL << (level * KHO_XARRAY_TBL_SHIFT);
	unsigned int i;
	u64 found;

	/*
	 * The first entry is searched starting from key. Every following
	 * entry is searched from the first key of the subnode.
	 */
	for (i = kho_xa_node_idx(key, level); i < KHO_XARRAY_TBL_ENTRIES;
	     i++, key = round_down(key, span) + span) {
		struct kho_xarray_node *next;

		next = KHOSER_LOAD_PTR(node->table[i]);
		if (!next)
			continue;

		if (level == 1)
			found = __kho_xa_next_leaf((struct kho_xarray_leaf *)next, key, value);
		else
			found = __kho_xa_next_node(next, key, value, level - 1);

		if (found != U64_MAX)
			return found;
	}

	return U64_MAX;
}

/**
 * kho_xa_next() - Find the next entry in a KHO xarray.
 * @kxa: The KHO xarray.
 * @key: Key to start searching from.
 * @value: Set to the value of the entry found. Left alone if none is found.
 *
 * Finds the entry with the smallest key that is greater than or equal to @key.
 *
 * Return: The key of the entry found, or %U64_MAX if there is none. %U64_MAX
 * is never a valid key, since it is wider than %KHO_XARRAY_KEY_WDITH.
 */
u64 kho_xa_next(struct kho_xarray *kxa, u64 key, u64 *value)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root);

	if (unlikely(!root))
		return U64_MAX;

	if (unlikely(fls64(key) > KHO_XARRAY_KEY_WDITH))
		return U64_MAX;

	return __kho_xa_next_node(root, key, value, KHO_XARRAY_DEPTH - 1);
}
