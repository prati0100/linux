/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <pratyush@kernel.org>
 */
#ifndef _LINUX_LIVEUPDATE_ABI_HUGETLB_H
#define _LINUX_LIVEUPDATE_ABI_HUGETLB_H

#include <linux/hugetlb.h>
#include <linux/kexec_handover.h>

/* TODO: Documentation */

/*
 * Keep the serialized max hstates separate from the kernel's HUGE_MAX_HSTATE to
 * keep the value stable.
 *
 * Currently x86 and arm64 are supported. x86 has HUGE_MAX_HSTATE as 2 and arm64
 * has 4. Pick 4 as the number to start with.
 */
#define HUGETLB_SER_MAX_HSTATES		4

static_assert(HUGETLB_SER_MAX_HSTATES >= HUGE_MAX_HSTATE);

/* TODO: Use u64/u32/etc for members? Remember: update variable types in code
 * as well then like loop counters. */
struct hugetlb_hstate_ser {
	/* Number of _preserved_ pages in the hstate. */
	unsigned long nr_pages;
	unsigned int order;
	unsigned int __reserved;
} __packed;

struct hugetlb_ser {
	struct hugetlb_hstate_ser hstates[HUGETLB_SER_MAX_HSTATES];
	unsigned short nr_hstates;
} __packed;

static_assert(sizeof(struct hugetlb_ser) <= PAGE_SIZE);

#define HUGETLB_FLB_NAME "hugetlb-v1"

struct huge_memfd_folio_ser {
	u64 pfn:52;
	u64 reserved:12;
	u64 index;
} __packed;

struct huge_memfd_ser {
	unsigned long size;
	unsigned long pos;
	unsigned long nr_folios;
	struct kho_vmalloc folios;
	unsigned int order;
};

#endif /* _LINUX_LIVEUPDATE_ABI_HUGETLB_H */
