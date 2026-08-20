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
	struct folio *folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
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

	/* If the entry is empty, count it as used now. */
	if (!leaf->values[idx])
		(*count)++;

	leaf->values[idx] = value;
}

static void kho_xa_clear_node(struct kho_xarray_node *node, unsigned int idx)
{
	struct folio *folio = virt_to_folio(node);
	unsigned long *count = folio_get_private(folio);

	KHOSER_CLEAR_PTR(node->table[idx]);
	(*count)--;
}

static void kho_xa_clear_leaf(struct kho_xarray_leaf *leaf, unsigned int idx)
{
	struct folio *folio = virt_to_folio(leaf);
	unsigned long *count = folio_get_private(folio);

	/* If the entry is used, count it as free now. */
	if (leaf->values[idx])
		(*count)--;

	leaf->values[idx] = 0;
}

static unsigned long kho_xa_node_count(struct kho_xarray_node *node)
{
	struct folio *folio = virt_to_folio(node);
	unsigned long *count = folio_get_private(folio);

	return *count;
}

static unsigned long kho_xa_leaf_count(struct kho_xarray_leaf *leaf)
{
	struct folio *folio = virt_to_folio(leaf);
	unsigned long *count = folio_get_private(folio);

	return *count;
}

static bool __kho_xa_cleanup_nodes(struct kho_xarray_node *node, u64 key,
				   unsigned int level)
{
	unsigned int idx = kho_xa_node_idx(key, level);
	struct kho_xarray_node *next;
	struct folio *folio;
	bool empty;

	next = KHOSER_LOAD_PTR(node->table[idx]);

	if (level == 1)
		empty = !!kho_xa_leaf_count((struct kho_xarray_leaf *)next);
	else
		empty = __kho_xa_cleanup_nodes(next, key, level - 1);

	if (!empty)
		return false;

	kho_xa_clear_node(node, idx);

	folio = virt_to_folio(next);
	folio_put(folio);

	return !!kho_xa_node_count(node);
}

static void kho_xa_cleanup_nodes(struct kho_xarray *kxa, u64 key)
{
	struct kho_xarray_node *root;
	struct folio *folio;
	bool empty;

	root = KHOSER_LOAD_PTR(kxa->root);
	empty = __kho_xa_cleanup_nodes(root, key, KHO_XARRAY_DEPTH - 1);

	if (!empty)
		return;

	KHOSER_CLEAR_PTR(kxa->root);

	folio = virt_to_folio(root);
	folio_put(folio);
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
	kho_xa_set_leaf(leaf, value, idx);

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
	kho_xa_clear_leaf(leaf, idx);
	if (!kho_xa_leaf_count(leaf))
		kho_xa_cleanup_nodes(kxa, key);
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
	unsigned int i, idx, shift;
	u64 prefix, found;

	idx = kho_xa_node_idx(key, level);
	shift = level * KHO_XARRAY_TBL_SHIFT;
	/* TODO: Make this easier to grok. */
	prefix = key & ~((1UL << (shift + KHO_XARRAY_TBL_SHIFT)) - 1);

	for (i = idx; i < KHO_XARRAY_TBL_ENTRIES; i++, key = prefix | (i << shift)) {
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
