/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_HANDOVER_INTERNAL_H
#define LINUX_KEXEC_HANDOVER_INTERNAL_H

#include <linux/kexec_handover.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/kho_radix_tree.h>
#include <asm/page.h>

#define KHO_MAX_ORDER (64 - PAGE_SHIFT)

#ifdef CONFIG_KEXEC_HANDOVER_DEBUGFS
#include <linux/debugfs.h>

struct kho_debugfs {
	struct dentry *dir;
	struct dentry *sub_fdt_dir;
	struct list_head fdt_list;
};

#else
struct kho_debugfs {};
#endif

extern struct kho_scratch *kho_scratch;
extern unsigned int kho_scratch_cnt;

struct kho_out {
	void *fdt;
	struct mutex lock; /* protects KHO FDT */

	struct kho_radix_tree radix_tree;
	struct kho_debugfs dbg;

	struct {
		unsigned long mem_preserved;
		unsigned long order_preservations[KHO_MAX_ORDER];
	} stats;
};

extern struct kho_out kho_out;

#ifdef CONFIG_KEXEC_HANDOVER_DEBUGFS
int kho_debugfs_init(void);
void kho_in_debugfs_init(struct kho_debugfs *dbg, const void *fdt);
int kho_out_debugfs_init(struct kho_debugfs *dbg);
int kho_debugfs_blob_add(struct kho_debugfs *dbg, const char *name,
			 const void *blob, size_t size, bool root);
void kho_debugfs_blob_remove(struct kho_debugfs *dbg, void *blob);
#else
static inline int kho_debugfs_init(void) { return 0; }
static inline void kho_in_debugfs_init(struct kho_debugfs *dbg,
				       const void *fdt) { }
static inline int kho_out_debugfs_init(struct kho_debugfs *dbg) { return 0; }
static inline int kho_debugfs_blob_add(struct kho_debugfs *dbg,
				       const char *name, const void *blob,
				       size_t size, bool root) { return 0; }
static inline void kho_debugfs_blob_remove(struct kho_debugfs *dbg,
					   void *blob) { }
#endif /* CONFIG_KEXEC_HANDOVER_DEBUGFS */

#endif /* LINUX_KEXEC_HANDOVER_INTERNAL_H */
