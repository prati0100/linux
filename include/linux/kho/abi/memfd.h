/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

#ifndef _LINUX_KHO_ABI_MEMFD_H
#define _LINUX_KHO_ABI_MEMFD_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/types.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/kho/abi/xarray.h>

/**
 * DOC: memfd Live Update ABI
 *
 * memfd uses the ABI defined below for preserving its state across a kexec
 * reboot using the LUO.
 *
 * The state is serialized into a packed structure `struct memfd_luo_ser`
 * which is handed over to the next kernel via the KHO mechanism.
 *
 * The folios of the file are stored in a KHO xarray. The value at a folio index
 * is a ``union memfd_luo_folio_ser``. A value of 0 means there is no folio at
 * that offset.
 *
 * This interface is a contract. Any modification to the structure layout
 * constitutes a breaking change. Such changes require incrementing the
 * version number in the MEMFD_LUO_FH_COMPATIBLE string.
 */

/**
 * MEMFD_LUO_FOLIO_DIRTY - The folio is dirty.
 *
 * This flag indicates the folio contains data from user. A non-dirty folio is
 * one that was allocated (say using fallocate(2)) but not written to.
 */
#define MEMFD_LUO_FOLIO_DIRTY		BIT(0)

/**
 * MEMFD_LUO_FOLIO_UPTODATE - The folio is up-to-date.
 *
 * An up-to-date folio has been zeroed out. shmem zeroes out folios on first
 * use. This flag tracks which folios need zeroing.
 */
#define MEMFD_LUO_FOLIO_UPTODATE	BIT(1)

/**
 * union memfd_luo_folio_ser - Serialized state of a single folio.
 * @val:   The raw value stored in the KHO xarray.
 * @pfn:   The page frame number of the folio.
 * @flags: Flags to describe the state of the folio.
 */
union memfd_luo_folio_ser {
	u64 val;
	struct {
		u64 pfn:52;
		u64 flags:12;
	};
};

/* It must fit into a KHO xarray entry. */
static_assert(sizeof(union memfd_luo_folio_ser) == sizeof(u64));

/*
 * The set of seals this version supports preserving. If support for any new
 * seals is needed, add it here and bump version.
 */
#define MEMFD_LUO_ALL_SEALS (F_SEAL_SEAL | \
			     F_SEAL_SHRINK | \
			     F_SEAL_GROW | \
			     F_SEAL_WRITE | \
			     F_SEAL_FUTURE_WRITE | \
			     F_SEAL_EXEC)

/**
 * struct memfd_luo_ser - Main serialization structure for a memfd.
 * @pos:       The file's current position (f_pos).
 * @size:      The total size of the file in bytes (i_size).
 * @seals:     The seals present on the memfd. The seals are uABI so it is safe
 *             to directly use them in the ABI.
 * @flags:     Flags for the file. Unused flag bits must be set to 0.
 * @folios:    Folios in the file.
 */
struct memfd_luo_ser {
	u64 pos;
	u64 size;
	u32 seals;
	u32 flags;
	struct kho_xarray folios;
} __packed;

/* The compatibility string for memfd file handler */
#define MEMFD_LUO_FH_COMPATIBLE	"memfd-v3"

#endif /* _LINUX_KHO_ABI_MEMFD_H */
