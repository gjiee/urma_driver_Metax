/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Local prototypes for MetaX P2P exports in metax.ko.
 * Compile-time only. Link against the installed KMD:
 *   /lib/modules/$(uname -r)/extra/metax.ko.xz
 *   /lib/modules/$(uname -r)/extra/Module.symvers
 */

#ifndef __UDMA_METAX_P2P_H__
#define __UDMA_METAX_P2P_H__

#include <linux/types.h>
#include <linux/scatterlist.h>

struct pci_dev;

int metax_p2p_acquire_mem(uint64_t addr, size_t size, void **handle,
			  int (*callback)(void *arg), void *arg);
void metax_p2p_release_mem(void *handle);
int metax_p2p_get_mem(void *handle, struct sg_table **sgt);
int metax_p2p_put_mem(void *handle, struct sg_table *sgt);
struct pci_dev *metax_p2p_get_pci_dev(void *handle);
uint64_t metax_p2p_get_bus_offset(void *handle);
uint32_t metax_p2p_get_page_size(void *handle);

#endif /* __UDMA_METAX_P2P_H__ */
