/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pratyush Yadav <pratyush@kernel.org>
 */

#ifndef _LINUX_KHO_XARRAY_H
#define _LINUX_KHO_XARRAY_H

#include <linux/kho/abi/xarray.h>
#include <linux/mutex_types.h>

void kho_xa_init(struct kho_xarray *kxa);
void kho_xa_destroy(struct kho_xarray *kxa);
int kho_xa_restore(struct kho_xarray *kxa);

int kho_xa_set(struct kho_xarray *kxa, u64 key, u64 value);
u64 kho_xa_get(struct kho_xarray *kxa, u64 key);

/**
 * kho_xa_clear() - Remove an entry from a KHO xarray.
 * @kxa: The KHO xarray.
 * @key: Key of the entry to remove.
 *
 * Same as storing 0 with kho_xa_set(). Never allocates memory and can't fail.
 * Keys that are out of range, or not present, are ignored.
 */
static inline void kho_xa_clear(struct kho_xarray *kxa, u64 key)
{
	/* Storing 0 never allocates and can't fail. */
	kho_xa_set(kxa, key, 0);
}

u64 kho_xa_next(struct kho_xarray *kxa, u64 key, u64 *value);

/**
 * kho_xa_for_each_from() - Iterate over entries of a KHO xarray from a key.
 * @kxa: The KHO xarray.
 * @key: u64 variable that holds the key of the current entry.
 * @value: u64 variable that holds the value of the current entry.
 * @start: Key to start iterating from.
 *
 * Visits the entries with keys greater than or equal to @start, in increasing
 * order of keys.
 */
#define kho_xa_for_each_from(kxa, key, value, start)				\
	for ((key) = kho_xa_next(kxa, start, &(value)); (key) != U64_MAX;	\
	     (key) = kho_xa_next(kxa, (key) + 1, &(value)))

/**
 * kho_xa_for_each() - Iterate over all entries of a KHO xarray.
 * @kxa: The KHO xarray.
 * @key: u64 variable that holds the key of the current entry.
 * @value: u64 variable that holds the value of the current entry.
 *
 * Visits the entries in increasing order of keys.
 */
#define kho_xa_for_each(kxa, key, value)					\
	kho_xa_for_each_from(kxa, key, value, 0)

#endif /* _LINUX_KHO_XARRAY_H */
