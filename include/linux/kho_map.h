/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pratyush Yadav <pratyush@kernel.org>
 */

#ifndef _LINUX_KHO_MAP_H
#define _LINUX_KHO_MAP_H

#include <linux/kho/abi/map.h>
#include <linux/mutex_types.h>

/**
 * struct kho_map - A KHO map.
 * @ser: The serialized form of the tree. This is what gets handed over.
 * @preserved: The nodes of the tree are preserved with KHO. Set by
 *             kho_map_preserve().
 *
 * Runtime handle of a KHO map. It is not handed over, only @ser is. This is
 * what all the kho_map_*() functions operate on.
 */
struct kho_map {
	struct kho_map_ser *ser;
	bool preserved;
};

void kho_map_init(struct kho_map *map, struct kho_map_ser *ser);
void kho_map_destroy(struct kho_map *map);
int kho_map_restore(struct kho_map *map, struct kho_map_ser *ser);
int kho_map_preserve(struct kho_map *map);

int kho_map_set(struct kho_map *map, u64 key, u64 value);
u64 kho_map_get(struct kho_map *map, u64 key);

/**
 * kho_map_clear() - Remove an entry from a KHO map.
 * @map: The KHO map.
 * @key: Key of the entry to remove.
 *
 * Same as storing 0 with kho_map_set(). Never allocates memory and can't fail.
 * Keys that are out of range, or not present, are ignored.
 */
static inline void kho_map_clear(struct kho_map *map, u64 key)
{
	/* Storing 0 never allocates and can't fail. */
	kho_map_set(map, key, 0);
}

u64 kho_map_next(struct kho_map *map, u64 key, u64 *value);

/**
 * kho_map_for_each_from() - Iterate over entries of a KHO map from a key.
 * @map: The KHO map.
 * @key: u64 variable that holds the key of the current entry.
 * @value: u64 variable that holds the value of the current entry.
 * @start: Key to start iterating from.
 *
 * Visits the entries with keys greater than or equal to @start, in increasing
 * order of keys.
 */
#define kho_map_for_each_from(map, key, value, start)				\
	for ((key) = kho_map_next(map, start, &(value)); (key) != U64_MAX;	\
	     (key) = kho_map_next(map, (key) + 1, &(value)))

/**
 * kho_map_for_each() - Iterate over all entries of a KHO map.
 * @map: The KHO map.
 * @key: u64 variable that holds the key of the current entry.
 * @value: u64 variable that holds the value of the current entry.
 *
 * Visits the entries in increasing order of keys.
 */
#define kho_map_for_each(map, key, value)					\
	kho_map_for_each_from(map, key, value, 0)

#endif /* _LINUX_KHO_MAP_H */
