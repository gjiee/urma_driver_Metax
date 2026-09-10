/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Minimal MetaX P2P declarations used by udma_metax_p2p_bridge.ko.
 * Matches the public EXPORT_SYMBOL ABI from the MetaX GPU driver
 * (metax_p2p_acquire_mem / get_mem / put_mem / release_mem / ...).
 *
 * This repo does not ship MetaX headers. Keep this surface tiny so the
 * bridge builds without the vendor tree.
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
