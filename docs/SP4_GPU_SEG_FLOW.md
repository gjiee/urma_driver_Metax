# SP4 — GPU VA 段注册补丁说明

> **来源**: `driver/ub/urma` 从 openEuler 官方仓库
> `https://atomgit.com/openeuler/kernel` 分支 `openEuler-24.03-LTS-SP4`
> 原样同步（`urma/` 整目录替换），随后在此基础上打 GPU P2P 补丁。
> **分支名**: `cursor/urma-sp4-sync-7c8e`

---

## 1. 与 `UB_PCIe_v1` 分支的区别

| | `UB_PCIe_v1` | 本分支 (SP4) |
|---|---|---|
| `urma/` 基线 | 早期自带快照 (commit `7934326`) | openEuler 官方 `openEuler-24.03-LTS-SP4` 原始代码 |
| GPU flag 位 | `BIT(14)` | **`BIT(15)`** |
| 原因 | 未核实内核 `ubcore_reg_seg_flag` 真实布局 | 已从官方 `include/ub/urma/ubcore_types.h` 确认 bit14 是 `flag.bs.pa`（内核私有位），bit15 才是 `reserved` 区间起点，不会与任何已知字段冲突 |

`union ubcore_reg_seg_flag` 官方定义（`include/ub/urma/ubcore_types.h`）：

```c
union ubcore_reg_seg_flag {
	struct {
		uint32_t token_policy : 3;
		uint32_t cacheable : 1;
		uint32_t dsva : 1;
		uint32_t access : 6;
		uint32_t non_pin : 1;
		uint32_t user_iova : 1;
		uint32_t token_id_valid : 1;
		uint32_t pa : 1;        /* ← bit14，内核私有，用户态不能碰 */
		uint32_t reserved : 17; /* ← bit15~31，我们的 GPU 标记用 bit15 */
	} bs;
	uint32_t value;
};
```

**用户态（UMDK）必须把 `URMA_REG_SEG_GPU_VA` 定义成 `(1u << 15)`，不是 `(1u << 14)`。**

---

## 2. 端到端流程（与 `UB_PCIe_v1` 一致，仅 bit 号不同）

```
用户态 seg_cfg.flag.value |= URMA_REG_SEG_GPU_VA (bit15)
    ↓ ioctl (uburma_cmd_register_seg → uburma_fill_attr)
ubcore_register_seg(): 校验 cfg->flag（bit15 落在 reserved，校验必过）
    ↓ dev->ops->register_seg = udma_register_seg
udma_init_seg_cfg(): seg->is_gpu_seg = udma_seg_cfg_is_gpu(cfg)  // 判 bit15
    ↓
udma_pin_pages_and_ioummu_map() / legacy udma_register_seg 非 pin 分支
    ↓ udma_seg_cfg_is_gpu(cfg) == true
udma_gpu_pin_seg_pages()
    → nvidia_p2p_get_pages_persistent(gpu_va, mapped_size, &page_table, 0)
    → nvidia_p2p_build_sg_table_phys(page_table, &seg->umem->append.sgt)
      (PA 写进 sg_dma_address)
    ↓
seg->tid = tid
    ↓
udma_ioummu_remote_map / udma_iommu_local_map
    → udma_ioummu_map(l_tid, r_tid, prot, seg->addr=gpu_va, sgt)
    → ummu_sva_matt_map(domain, gpu_va, sgt, prot)   // 真正建表
```

---

## 3. 本分支修改文件

| 文件 | 作用 |
|------|------|
| `urma/hw/udma/udma_gpu_p2p.h` | ABI 宏（`UBCORE_REG_SEG_GPU_VA = BIT(15)`）、GPU 段判定 |
| `urma/hw/udma/udma_gpu_p2p.c` | NV P2P pin/unpin + SG 构建 |
| `urma/hw/udma/udma_segment.h` | GPU 段结构体扩展（`is_gpu_seg`、`struct udma_gpu_p2p_mem gpu`） |
| `urma/hw/udma/udma_segment.c` | 注册/注销分流（`udma_init_seg_cfg` / `udma_pin_pages_and_ioummu_map` / legacy 路径 / `udma_unpin_seg_pages`） |
| `urma/hw/udma/udma_main.c` | `MODULE_SOFTDEP("pre: nvidia")` |
| `urma/hw/udma/Makefile` | 编译 `udma_gpu_p2p.o`，`ccflags -I$(NV_P2P_HEADER_PATH)` |
| `urma/Makefile` | OOT 独立编译 `urma/` 支持（`NV_P2P_HEADER_PATH`/`NV_MODULE_SYMVERS`） |
| `Makefile`（根） | OOT 整树编译支持 |

---

## 4. 依赖 & 闭源驱动注意事项

- `nvidia_p2p_get_pages_persistent` / `nvidia_p2p_put_pages_persistent` 是 NVIDIA **官方稳定导出符号**，开源/闭源 `nvidia.ko` 都有，不需要改 NVIDIA 驱动源码。
- `nvidia_p2p_build_sg_table_phys` **不是官方符号**，只存在于本项目自建的 `nv_module_netlab`（开源 fork）里。如果你的 GPU 不支持 open-gpu-kernel-modules（例如无 GSP 的卡，只能用闭源驱动），把 `udma_gpu_p2p.c` 里这一行换成本地实现（不依赖任何 NV 侧新增符号，只用 `struct nvidia_p2p_page_table`/`nvidia_p2p_page` 官方结构体自己拼 `sg_table`）。
- URMA/UB 总线不是 PCI 设备，**不能**调用 `nvidia_p2p_dma_map_pages`（需要 `struct pci_dev *peer`）。只用 `get_pages_persistent` 返回的 `physical_address` 走 UMMU 自己的表即可，这对非 PCIe 第三方总线是唯一可行路径。

---

## 5. 编译

```bash
cd urma_driver
make NV_P2P_HEADER_PATH=/path/to/nv-p2p-header-dir \
     NV_MODULE_SYMVERS=/path/to/your/nvidia/Module.symvers
```

`urma/` 单独编译：

```bash
cd urma
make NV_P2P_HEADER_PATH=... NV_MODULE_SYMVERS=...
```

加载顺序：`insmod nvidia.ko` → `insmod udma.ko`（`MODULE_SOFTDEP` 已声明，多数发行版 `modprobe` 会自动处理）。
