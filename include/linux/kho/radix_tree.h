/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_KHO_RADIX_TREE_H
#define _LINUX_KHO_RADIX_TREE_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/mutex_types.h>
#include <linux/types.h>

/**
 * DOC: Kexec Handover Radix Tree
 *
 * This is a radix tree implementation for tracking numeric keys across kexec
 * transitions. It was developed for the KHO preserved memory map but is
 * designed for broader use by any subsystem that needs to track keys.
 * Conceptually speaking, the data structure is similar to a set. It tracks the
 * presence or absence of numeric keys.
 *
 * The radix tree is a multi-level tree where leaf nodes are bitmaps
 * representing individual keys.
 *
 * For the KHO preserved memory map, to allow pages of different sizes (orders)
 * to be stored efficiently in a single tree, it uses a unique key encoding
 * scheme. Each key is an unsigned long that combines a page's physical address
 * and its order.
 *
 * Client code is responsible for allocating the root node of the tree,
 * initializing the mutex lock, and managing its lifecycle. It must use the
 * tree data structures defined in the KHO ABI,
 * `include/linux/kho/abi/kexec_handover.h`.
 */

struct kho_radix_node;

struct kho_radix_tree {
	struct kho_radix_node *root;
	struct mutex lock; /* protects the tree's structure and root pointer */
};

#ifdef CONFIG_KEXEC_HANDOVER

int kho_radix_add_key(struct kho_radix_tree *tree, unsigned long key);
void kho_radix_del_key(struct kho_radix_tree *tree, unsigned long key);
unsigned long kho_radix_next(struct kho_radix_tree *tree, unsigned long key);
void *kho_radix_next_node(struct kho_radix_tree *tree, unsigned long *key,
			  unsigned int *level);
int kho_radix_init_tree(struct kho_radix_tree *tree, struct kho_radix_node *root);
void kho_radix_destroy_tree(struct kho_radix_tree *tree);

#else  /* #ifdef CONFIG_KEXEC_HANDOVER */

static inline int kho_radix_add_key(struct kho_radix_tree *tree, unsigned long key)
{
	return -EOPNOTSUPP;
}

static inline void kho_radix_del_key(struct kho_radix_tree *tree,
				     unsigned long key) { }

static inline unsigned long kho_radix_next(struct kho_radix_tree *tree,
					   unsigned long key)
{
	return ULONG_MAX;
}

static inline void *kho_radix_next_node(struct kho_radix_tree *tree,
					unsigned long *key, unsigned int *level)
{
	return NULL;
}

static inline int kho_radix_init_tree(struct kho_radix_tree *tree,
				      struct kho_radix_node *root)
{
	return 0;
}

static inline void kho_radix_destroy_tree(struct kho_radix_tree *tree) { }

#endif /* #ifdef CONFIG_KEXEC_HANDOVER */

/**
 * kho_radix_for_each_from() - Iterate over keys of a KHO radix tree from a key.
 * @tree: The KHO radix tree.
 * @key: unsigned long variable that holds the current key.
 * @start: Key to start iterating from.
 *
 * Visits the keys greater than or equal to @start, in increasing order. The
 * tree lock is only held while looking up each key, not across the loop body,
 * so the body may modify the tree. Keys added or removed during the iteration
 * may or may not be visited.
 */
#define kho_radix_for_each_from(tree, key, start)				\
	for ((key) = kho_radix_next(tree, start); (key) != ULONG_MAX;		\
	     (key) = kho_radix_next(tree, (key) + 1))

/**
 * kho_radix_for_each() - Iterate over all keys of a KHO radix tree.
 * @tree: The KHO radix tree.
 * @key: unsigned long variable that holds the current key.
 *
 * Visits the keys in increasing order. See kho_radix_for_each_from().
 */
#define kho_radix_for_each(tree, key)						\
	kho_radix_for_each_from(tree, key, 0)

/* Move the position of kho_radix_next_node() past the current node. */
static inline void *__kho_radix_next_node_after(struct kho_radix_tree *tree,
						unsigned long *key,
						unsigned int *level)
{
	if (*level) {
		(*level)--;
	} else {
		(*key)++;
		*level = UINT_MAX;
	}

	return kho_radix_next_node(tree, key, level);
}

/**
 * kho_radix_for_each_node() - Iterate over the nodes of a KHO radix tree.
 * @tree: The KHO radix tree.
 * @node: void * variable that points to the page holding the current node.
 * @key: unsigned long variable that holds the first key covered by @node.
 * @level: unsigned int variable that holds the level of @node. Level 0 is
 *         the leaf bitmaps, the root has the highest level.
 *
 * Visits every node of the tree, including the root and the leaves, in
 * pre-order: a node is visited before its children, and children are visited
 * in increasing order of keys. Like kho_radix_for_each(), the tree lock is not
 * held across the loop body.
 */
#define kho_radix_for_each_node(tree, node, key, level)				\
	for ((key) = 0, (level) = UINT_MAX,					\
	     (node) = kho_radix_next_node(tree, &(key), &(level));		\
	     (node);								\
	     (node) = __kho_radix_next_node_after(tree, &(key), &(level)))

#endif	/* _LINUX_KHO_RADIX_TREE_H */
