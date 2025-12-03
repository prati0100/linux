/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <pratyush@kernel.org>
 */
#ifndef _LINUX_LIVEUPDATE_ABI_HUGETLB_H
#define _LINUX_LIVEUPDATE_ABI_HUGETLB_H

#include <linux/hugetlb.h>
#include <linux/kexec_handover.h>

/* TODO: Versioning */
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

struct hugetlb_folio_ser {
	u64 pfn:52;
	u64 free:1;
	u64 reserved:11;
};

struct hugetlb_hstate_ser {
	unsigned long nr_pages;
	unsigned int order;
	unsigned int __reserved;
	/* TODO: Can I make better type system for kho vmalloc? */
	struct kho_vmalloc folios;
};

struct hugetlb_ser {
	unsigned short nr_hstates;
	/* TODO: take care of padding? */
	struct hugetlb_hstate_ser hstates[HUGETLB_SER_MAX_HSTATES];
};

static_assert(sizeof(struct hugetlb_ser) <= PAGE_SIZE);

#define HUGETLB_FLB_NAME "hugetlb"

struct huge_memfd_folio_ser {
	u64 pfn:52;
	u64 reserved:12;
	/* TODO: Do we even need index? Or can we assume that from the position
	 * in array? */
	u64 index;
};

struct huge_memfd_ser {
	unsigned long size;
	unsigned long pos;
	unsigned long nr_folios;
	struct kho_vmalloc folios;
	unsigned int order;
};

#endif /* _LINUX_LIVEUPDATE_ABI_HUGETLB_H */
