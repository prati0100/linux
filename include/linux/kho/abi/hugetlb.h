/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Copyright (C) 2025 Pratyush Yadav <pratyush@kernel.org>
 */

#ifndef _LINUX_KHO_ABI_HUGETLB_H
#define _LINUX_KHO_ABI_HUGETLB_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/fcntl.h>
#include <linux/hugetlb.h>
#include <linux/types.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/kho/abi/map.h>

/**
 * DOC: hugetlb file live update ABI
 *
 * This header defines the ABI for preserving the state of the hugetlb subsystem
 * and of hugetlb files across a kexec reboot using LUO.
 *
 * The folios of a hugetlb file are stored in a KHO map, keyed by the
 * index of the hugepage in the file (the file offset divided by the hugepage
 * size). The value is a ``union hugetlb_luo_folio_ser``. A value of 0 means
 * there is no folio at that index.
 *
 * This interface is a contract. Any modification to the structure layout
 * constitutes a breaking change. Such changes require incrementing the version
 * number in the HUGETLB_FLB_COMPATIBLE or HUGETLB_FILE_COMPATIBLE strings for
 * hugetlb FLB or hugetlb files, respectively.
 */

/*
 * Keep the serialized max hstates separate from the kernel's HUGE_MAX_HSTATE to
 * keep the value stable.
 *
 * Currently x86 and arm64 are supported. x86 has HUGE_MAX_HSTATE as 2 and arm64
 * has 4. Pick 4 as the number to start with.
 */
#define HUGETLB_SER_MAX_HSTATES		4

static_assert(HUGETLB_SER_MAX_HSTATES >= HUGE_MAX_HSTATE);

/**
 * struct hugetlb_hstate_ser: Serialized state of a hstate.
 * @nr_pages:     Number of preserved pages in the hstate.
 * @order:        Order of the hstate this struct describes.
 *
 * The only state needed for hstates is the number of pages that are preserved
 * from this hstate. The preserved pages are added to the hstate when the file
 * is retrieved. This information gets used in early boot to calculate the
 * remaining pages that must be allocated by the normal path.
 */
struct hugetlb_hstate_ser {
	/* Number of _preserved_ pages in the hstate. */
	u64 nr_pages;
	u8 order;
} __packed;

/**
 * struct hugetlb_ser - The main serialization structure for HugeTLB FLB.
 * @hstates:      Array of serialized hstates.
 * @nr_hstates:   Number of serialized hstates in the array.
 */
struct hugetlb_ser {
	struct hugetlb_hstate_ser hstates[HUGETLB_SER_MAX_HSTATES];
	u8 nr_hstates;
} __packed;

static_assert(sizeof(struct hugetlb_ser) <= PAGE_SIZE);

#define HUGETLB_FLB_COMPATIBLE "hugetlb-v1"

/**
 * union hugetlb_luo_folio_ser - Serialized state of a single folio.
 * @val:          The raw value stored in the KHO map.
 * @pfn:          The page frame number of the folio.
 * @flags:        Flags for the folio. Currently none are defined, so this
 *                must be 0.
 */
union hugetlb_luo_folio_ser {
	u64 val;
	struct {
		u64 pfn:52;
		u64 flags:12;
	};
};

/* It must fit into a KHO map entry. */
static_assert(sizeof(union hugetlb_luo_folio_ser) == sizeof(u64));

/*
 * The set of seals this version supports preserving. If support for any new
 * seals is needed, add it here and bump version.
 */
#define HUGETLB_LUO_ALL_SEALS (F_SEAL_SEAL | \
			       F_SEAL_SHRINK | \
			       F_SEAL_GROW | \
			       F_SEAL_WRITE | \
			       F_SEAL_FUTURE_WRITE | \
			       F_SEAL_EXEC)

/*
 * The file was on a hugetlbfs mount created by userspace, not on one of the
 * internal mounts that back memfds.
 */
#define HUGETLB_LUO_FLAG_HUGETLBFS	BIT(0)

#define HUGETLB_LUO_ALL_FLAGS		HUGETLB_LUO_FLAG_HUGETLBFS

/**
 * struct hugetlb_luo_ser - Main serialization structure of a hugetlb file.
 * @size:         The total size of the file in bytes (i_size).
 * @pos:          The file's current position (f_pos).
 * @folios:       Folios in the file.
 * @seals:        The seals present on the file. The seals are uABI so it is
 *                safe to directly use them in the ABI. Only seals in
 *                %HUGETLB_LUO_ALL_SEALS may be set.
 * @order:        Order of the hugepages that back this file.
 * @flags:        Flags describing the file. Only flags in
 *                %HUGETLB_LUO_ALL_FLAGS may be set.
 * @reserved:     Explicit padding. Must be set to 0.
 *
 * @pos is only used for memfds. For files with %HUGETLB_LUO_FLAG_HUGETLBFS it
 * is 0, and @seals is always exactly ``F_SEAL_SEAL``.
 */
struct hugetlb_luo_ser {
	u64 size;
	u64 pos;
	struct kho_map_ser folios;
	u32 seals;
	u8 order;
	u8 flags;
	u8 reserved[2];
};

static_assert(sizeof(struct hugetlb_luo_ser) <= PAGE_SIZE);

#define HUGETLB_FILE_COMPATIBLE "hugetlb-file-v1"

#endif /* _LINUX_KHO_ABI_HUGETLB_H */
