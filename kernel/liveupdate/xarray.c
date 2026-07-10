/* TODO: License, copyright, etc. */

#include <linux/io.h>
#include <linux/mm.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/xarray.h>
#include <linux/kho_xarray.h>
#include <linux/pagemap.h>

void kho_xa_init(struct kho_xarray *kxa)
{
	KHOSER_CLEAR_PTR(kxa->root);
}

void kho_xa_destroy(struct kho_xarray *kxa)
{
	/* TODO */
}

static unsigned int kho_xa_node_idx(u64 key, unsigned int level)
{
	return (key >> (level * KHO_XARRAY_TBL_SHIFT)) & KHO_XARRAY_TBL_MASK;
}

static struct kho_xarray_node *kho_xa_alloc_node(void)
{
	struct folio *folio = folio_alloc(GFP_KERNEL, 0);
	unsigned long *count;

	if (!folio)
		return NULL;

	count = kzalloc_obj(*count);
	if (!count)
		goto err_put_folio;

	if (kho_preserve_folio(folio))
		goto err_free_count;

	folio_attach_private(folio, count);
	return folio_address(folio);

err_free_count:
	kfree(count);
err_put_folio:
	folio_put(folio);

	return NULL;
}

static void kho_xa_set_node(struct kho_xarray_node *node,
			    struct kho_xarray_node *next,
			    unsigned int idx)
{
	struct folio *folio = virt_to_folio(node);
	unsigned long *count = folio_get_private(folio);

	KHOSER_STORE_PTR(node->table[idx], next);
	(*count)++;
}

static void kho_xa_set_leaf(struct kho_xarray_leaf *leaf, u64 value,
			    unsigned int idx)
{
	struct folio *folio = virt_to_folio(leaf);
	unsigned long *count = folio_get_private(folio);

	leaf->values[idx] = value;
	(*count)++;
}

static void kho_xa_clear_node(struct kho_xarray_node *node, unsigned int idx)
{
	struct folio *folio = virt_to_folio(node);
	unsigned long *count = folio_get_private(folio);

	KHOSER_CLEAR_PTR(node->table[idx]);
	(*count)--;
}

static void kho_xa_cleanup_nodes(struct kho_xarray_node *root, u64 key)
{
	struct kho_xarray_node *nodes[KHO_XARRAY_DEPTH] = { NULL };
	struct kho_xarray_node *node = root;
	struct kho_xarray_leaf *leaf = NULL;

	for (unsigned int i = KHO_XARRAY_DEPTH - 1; i > 0; i--) {
		unsigned int idx;

		nodes[i] = node;

		idx = kho_xa_node_idx(key, i);
		node = KHOSER_LOAD_PTR(node->table[idx]);
		if (!node)
			break;
	}

	/* If the loop exits with a valid node, it must be the leaf */
	if (node)
		leaf = (struct kho_xarray_leaf *)node;

	for (unsigned int i = KHO_XARRAY_DEPTH - 1; i > 0; i--) {
		unsigned long *count;

		if (!nodes[i])
			continue;

	}
}

int kho_xa_set(struct kho_xarray *kxa, u64 key, u64 value)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root), *node;
	struct kho_xarray_leaf *leaf;
	unsigned int i, idx;
	int err;

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

		/* Node doesn't exist, need to allocate it. */
		next = kho_xa_alloc_node();
		if (!next) {
			err = -ENOMEM;
			goto err_free_nodes;
		}

		kho_xa_set_node(node, next, idx);
		node = next;
	}

	/* node now points to the leaf node. */
	leaf = (struct kho_xarray_leaf *)node;
	idx = kho_xa_node_idx(key, 0);
	/* TODO: set_leaf()? */
	leaf->values[idx] = value;

	return 0;

err_free_nodes:
	/* TODO */
	return err;
}

void kho_xa_clear(struct kho_xarray *kxa, u64 key)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root), *node;
	struct kho_xarray_leaf *leaf;
	unsigned int i, idx;

	if (unlikely(fls64(key) > KHO_XARRAY_KEY_WDITH))
		return;

	if (!root)
		return;


	for (i = KHO_TREE_MAX_DEPTH - 1; i > 0; i--) {
		idx = kho_xa_node_idx(key, i);
		node = KHOSER_LOAD_PTR(node->table[idx]);
		if (!node)
			return;
	}

	leaf = (struct kho_xarray_leaf *)node;
	idx = kho_xa_node_idx(key, 0);
	leaf->values[idx] = 0;

	/* TODO: Free nodes. */
}

static u64 __kho_xa_next_node(struct kho_xarray_node *node, u64 *key, u64 *value,
			      unsigned int level)
{
	unsigned int i, idx, shift;

	idx = kho_xa_node_idx(*key, level);
	shift = level * KHO_XARRAY_TBL_SHIFT;

	for (i = idx; i < KHO_XARRAY_TBL_ENTRIES; i++) {
		struct kho_xarray_node *next;

		next = KHOSER_LOAD_PTR(node->table[i]);
		if (!next)
			continue;

		if (level == 1) {
			__kho_xa_next_leaf(next, key, )
		}
	}
}

u64 kho_xa_next(struct kho_xarray *kxa, u64 key, u64 *value)
{
	struct kho_xarray_node *root = KHOSER_LOAD_PTR(kxa->root);

	if (!root)
		return U64_MAX;

}
