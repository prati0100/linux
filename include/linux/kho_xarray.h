/* TODO: license, etc. */
#ifndef _LINUX_KHO_XARRAY_H
#define _LINUX_KHO_XARRAY_H

#include <linux/kho/abi/xarray.h>
#include <linux/mutex_types.h>

/* TODO: docs */

void kho_xa_init(struct kho_xarray *kxa);
void kho_xa_destroy(struct kho_xarray *kxa);

int kho_xa_set(struct kho_xarray *kxa, u64 key, u64 value);
void kho_xa_clear(struct kho_xarray *kxa, u64 key);

u64 kho_xa_next(struct kho_xarray *kxa, u64 key, u64 *value);
#define kho_xa_for_each(kxa, key, value)					\
	for ((key) = kho_xa_next(kxa, 0, &(value)); (key) != U64_MAX;	\
	     (key) = kho_xa_next(kxa, (key) + 1, &(value)))

#endif /* _LINUX_KHO_XARRAY_H */
