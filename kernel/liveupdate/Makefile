# SPDX-License-Identifier: GPL-2.0

kho-y :=								\
		kexec_handover.o				\
		kho_block.o						\
		xarray.o

kho-$(CONFIG_KEXEC_HANDOVER_DEBUG) += kexec_handover_debug.o
kho-$(CONFIG_KEXEC_HANDOVER_DEBUGFS) += kexec_handover_debugfs.o

obj-$(CONFIG_KEXEC_HANDOVER)		+= kho.o

luo-y :=								\
		luo_core.o						\
		luo_file.o						\
		luo_flb.o						\
		luo_session.o

obj-$(CONFIG_LIVEUPDATE)		+= luo.o
