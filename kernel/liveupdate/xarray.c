/* TODO: License, copyright, etc. */

#include <linux/io.h>
#include <linux/mm.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/xarray.h>
#include <linux/kho_xarray.h>

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

/*
 * Take over a tree handed over by the previous kernel. All nodes are restored
 * and preserved again, so afterwards the tree can be used with all the other
 * kho_xa_*() functions, and is carried over to the next kernel.
 *
 * kho_xa_next() can be used before calling this, but nothing that modifies or
 * frees the tree.
 *
 * On failure, the tree is destroyed. Nodes that could not be restored are
 * leaked.
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

/* TODO: Handle value == 0. Two options: first, can treat setting 0 as
 * clearing. So no caller can have a "present" entry with value 0. Second,
 * allow only 63 bit values and then track the "present" bit internally.
 *
 * Not sure which one to pick.
 */
int kho_xa_set(struct kho_xarray *kxa, u64 key, u64 value)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root), *node;
	struct kho_xarray_leaf *leaf;
	unsigned int i, idx;

	if (unlikely(fls64(key) > KHO_XARRAY_KEY_WDITH))
		return -ERANGE;

	might_sleep();

	if (!root) {
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

/*
 * Empty nodes are not freed here, they stay in the tree until
 * kho_xa_destroy().
 */
void kho_xa_clear(struct kho_xarray *kxa, u64 key)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root), *node;
	struct kho_xarray_leaf *leaf;
	unsigned int i, idx;

	if (unlikely(fls64(key) > KHO_XARRAY_KEY_WDITH))
		return;

	if (!root)
		return;

	node = root;
	for (i = KHO_XARRAY_DEPTH - 1; i > 0; i--) {
		idx = kho_xa_node_idx(key, i);
		node = KHOSER_LOAD_PTR(node->table[idx]);
		if (!node)
			return;
	}

	leaf = (struct kho_xarray_leaf *)node;
	idx = kho_xa_node_idx(key, 0);
	leaf->values[idx] = 0;
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

u64 kho_xa_next(struct kho_xarray *kxa, u64 key, u64 *value)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root);

	if (unlikely(!root))
		return U64_MAX;

	if (unlikely(fls64(key) > KHO_XARRAY_KEY_WDITH))
		return U64_MAX;

	return __kho_xa_next_node(root, key, value, KHO_XARRAY_DEPTH - 1);
}
