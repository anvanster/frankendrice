# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026, FrankenDrICE Contributors.
#
# FrankenDrICE E810 Network Driver - Build System
#

obj-m := frankendrice.o

frankendrice-y := fdice_main.o \
		  fdice_hw.o \
		  fdice_aq.o \
		  fdice_queue.o \
		  fdice_channel.o \
		  fdice_eq.o \
		  fdice_txrx.o \
		  fdice_page_pool.o \
		  fdice_health_core.o \
		  fdice_health_fw.o \
		  fdice_health_tx.o \
		  fdice_health_rx.o \
		  fdice_health_port.o \
		  fdice_fs_core.o \
		  fdice_fs_cmd.o \
		  fdice_fs_recipe.o \
		  fdice_fs_tc.o \
		  fdice_fs_fdir.o \
		  fdice_fs_ttc.o \
		  fdice_ddp.o \
		  fdice_flex_pipe.o \
		  fdice_ptp.o \
		  fdice_ptp_hw.o \
		  fdice_rdma.o \
		  fdice_iidc.o \
		  fdice_devlink.o \
		  fdice_devlink_region.o \
		  fdice_eswitch.o \
		  fdice_eswitch_br.o \
		  fdice_repr.o \
		  fdice_repr_tc.o \
		  fdice_netdev.o \
		  fdice_ethtool.o \
		  fdice_stats.o \
		  fdice_dcb.o \
		  fdice_dcb_lib.o

# Out-of-tree build support
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules W=1

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

# Sparse static analysis
sparse:
	$(MAKE) -C $(KDIR) M=$(CURDIR) C=2 CF="-D__CHECK_ENDIAN__" modules

.PHONY: all clean sparse
