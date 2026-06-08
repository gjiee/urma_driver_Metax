// SPDX-License-Identifier: GPL-2.0+
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */

#define dev_fmt(fmt) "UDMA: " fmt

#include <linux/io.h>
#include <linux/scatterlist.h>
#include <ub/urma/ubcore_types.h>

#include "udma_gpu_p2p.h"
#include "udma_gpu_p2p_debug.h"
#include "udma_jfs.h"

static bool gpu_p2p_probe_bar;
module_param(gpu_p2p_probe_bar, bool, 0644);
MODULE_PARM_DESC(gpu_p2p_probe_bar,
		 "POC: ioremap first GPU page and printk first u32 (default: false)");

void udma_gpu_p2p_registration_test(struct udma_dev *dev, struct udma_segment *seg)
{
	struct scatterlist *sg;
	uint32_t i;
	uint64_t pattern;

	if (!seg || !udma_seg_is_gpu(seg) || !seg->umem)
		return;

	if (atomic_cmpxchg(&seg->gpu.reg_test_done, 0, 1) != 0)
		return;

	dev_info(dev->dev,
		 "UDMA_GPU_P2P_TEST magic=0x%08x phase=register seg_va=0x%llx gpu_va=0x%llx nents=%u\n",
		 UDMA_GPU_P2P_DEBUG_MAGIC, seg->addr, seg->gpu.gpu_va,
		 seg->umem->append.sgt.nents);

	for_each_sg(seg->umem->append.sgt.sgl, sg, seg->umem->append.sgt.nents, i) {
		pattern = UDMA_GPU_P2P_DEBUG_MAGIC ^ i ^ sg_dma_address(sg);
		dev_info(dev->dev,
			 "UDMA_GPU_P2P_TEST magic=0x%08x phase=register sg[%u] dma=0x%llx len=0x%x pattern=0x%llx\n",
			 UDMA_GPU_P2P_DEBUG_MAGIC, i, sg_dma_address(sg),
			 sg_dma_len(sg), pattern);
	}

	if (gpu_p2p_probe_bar && seg->umem->append.sgt.nents > 0) {
		phys_addr_t phys = (phys_addr_t)sg_dma_address(seg->umem->append.sgt.sgl);
		void __iomem *mmio;
		u32 probe_val;

		mmio = ioremap(phys, PAGE_SIZE);
		if (mmio) {
			probe_val = ioread32(mmio);
			dev_info(dev->dev,
				 "UDMA_GPU_P2P_TEST magic=0x%08x phase=bar_probe phys=0x%llx read=0x%08x\n",
				 UDMA_GPU_P2P_DEBUG_MAGIC, (u64)phys, probe_val);
			iounmap(mmio);
		} else {
			dev_warn(dev->dev,
				 "UDMA_GPU_P2P_TEST magic=0x%08x phase=bar_probe ioremap failed phys=0x%llx\n",
				 UDMA_GPU_P2P_DEBUG_MAGIC, (u64)phys);
		}
	}
}

uint64_t udma_gpu_p2p_debug_resolve_sge(struct udma_dev *dev,
				      struct udma_segment *seg,
				      uint64_t user_va, uint32_t len)
{
	uint64_t dma = udma_gpu_resolve_dma_addr(seg, user_va);

	dev_info(dev->dev,
		 "UDMA_GPU_P2P_TEST magic=0x%08x phase=inject user_va=0x%llx dma=0x%llx len=%u pattern=0x%llx\n",
		 UDMA_GPU_P2P_DEBUG_MAGIC, user_va, dma, len,
		 (uint64_t)(UDMA_GPU_P2P_DEBUG_MAGIC ^ dma ^ len));

	return dma;
}

static struct udma_segment *udma_gpu_find_seg_from_wr(struct ubcore_jfs_wr *wr)
{
	struct ubcore_sge *sge;
	uint32_t num_sge;
	uint32_t i;

	switch (wr->opcode) {
	case UBCORE_OPC_SEND:
	case UBCORE_OPC_SEND_IMM:
	case UBCORE_OPC_SEND_INVALIDATE:
		sge = wr->send.src.sge;
		num_sge = wr->send.src.num_sge;
		break;
	case UBCORE_OPC_WRITE:
	case UBCORE_OPC_WRITE_IMM:
	case UBCORE_OPC_WRITE_ATOMIC:
		sge = wr->rw.src.sge;
		num_sge = wr->rw.src.num_sge;
		break;
	case UBCORE_OPC_READ:
		sge = wr->rw.dst.sge;
		num_sge = wr->rw.dst.num_sge;
		break;
	default:
		return NULL;
	}

	for (i = 0; i < num_sge; i++) {
		if (sge[i].tseg) {
			struct udma_segment *seg = to_udma_seg(sge[i].tseg);

			if (udma_seg_is_gpu(seg))
				return seg;
		}
	}

	return NULL;
}

void udma_gpu_p2p_debug_post_wr(struct udma_dev *dev, struct ubcore_jfs_wr *wr,
				struct udma_sqe_ctl *sqe)
{
	struct udma_segment *seg;
	struct udma_normal_sge *sge;
	uint32_t sge_num;
	uint32_t i;

	seg = udma_gpu_find_seg_from_wr(wr);
	if (!seg)
		return;

	if (atomic_cmpxchg(&seg->gpu.wr_test_done, 0, 1) != 0)
		return;

	sge_num = sqe->sge_num;
	sge = (struct udma_normal_sge *)(sqe + 1);

	dev_info(dev->dev,
		 "UDMA_GPU_P2P_TEST magic=0x%08x phase=post_wr opcode=%u sge_num=%u\n",
		 UDMA_GPU_P2P_DEBUG_MAGIC, wr->opcode, sge_num);

	for (i = 0; i < sge_num; i++) {
		dev_info(dev->dev,
			 "UDMA_GPU_P2P_TEST magic=0x%08x phase=post_wr sqe_sge[%u] va=0x%llx len=%u pattern=0x%llx\n",
			 UDMA_GPU_P2P_DEBUG_MAGIC, i, sge[i].va, sge[i].length,
			 (uint64_t)(UDMA_GPU_P2P_DEBUG_MAGIC ^ sge[i].va ^ sge[i].length));
	}
}
