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
| GPU 标记怎么从用户态传到内核 | `flag.value` 的 `BIT(14)` | **`ubcore_udata->udrv_data` 私有通道**（不占用 `flag.value` 任何位） |
| 原因 | 未核实内核 `ubcore_reg_seg_flag` 真实布局，且直接复用共享的 flag 字段 | 已从官方 `include/ub/urma/ubcore_types.h` 确认 bit14 是 `flag.bs.pa`（内核私有位）。与其在 `flag` 里找一个"确定安全"的位，不如干脆用 HW driver 自定义命令数据通道（`udrv_data`），`ubcore` 全程当 `void*` 不解析，彻底没有位冲突风险 |

`union ubcore_reg_seg_flag` 官方定义（`include/ub/urma/ubcore_types.h`），仅供参考，本方案不再依赖它的具体位布局：

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
		uint32_t pa : 1;        /* ← bit14，内核私有 */
		uint32_t reserved : 17; /* ← bit15~31 */
	} bs;
	uint32_t value;
};
```

**ABI**：用户态在 `urma_seg_cfg_t` 里新增一个**普通字段**（不是 bitfield）`is_gpu_seg`，`udma_u_register_seg()` 把它打进 `struct udma_register_seg_ucmd`，通过 `urma_cmd_udrv_priv_t.in_addr/in_len` 传给内核，内核侧用同名结构体 `copy_from_user` 读出来。`flag.value` 全程不变，不需要用户态或内核关心里面哪个位安全。

---

## 2. 端到端流程

```
用户态 seg_cfg.is_gpu_seg = 1（urma_seg_cfg_t 新增普通字段）
    ↓
udma_u_register_seg(): 跳过 ummu_grant()；把 is_gpu_seg 打进
                        struct udma_register_seg_ucmd，塞进 udata.in_addr
    ↓ urma_cmd_register_seg() → ioctl
              (arg.udata = udrv_data，arg.in.flag 完全不受影响)
uburma_cmd_register_seg(): fill_udata() 把 arg.udata 挂到 struct ubcore_udata
    ↓
ubcore_register_seg(): 用【原始未改的】cfg->flag 校验（udrv_data 是 void*，
                        ubcore 不解析，肯定不会因为 GPU 标记而校验失败）
    ↓ dev->ops->register_seg(dev, &tmp_cfg, udata)
udma_register_seg():
    udma_seg_ucmd_is_gpu(udata) 从 udata->udrv_data 读出 GPU 标记
    → 在本地 cfg(=tmp_cfg) 上打 UBCORE_REG_SEG_GPU_VA（udma.ko 内部私有位，
      从不回传用户态，也不会被 ubcore 再次校验）
    ↓
udma_init_seg_cfg(): seg->is_gpu_seg = udma_seg_cfg_is_gpu(cfg)
    ↓
udma_pin_pages_and_ioummu_map() / legacy 非 pin 分支
    ↓ udma_seg_cfg_is_gpu(cfg) == true
udma_gpu_pin_seg_pages()
    → nvidia_p2p_get_pages(0, 0, gpu_va, mapped_size, &page_table,
                           udma_gpu_p2p_free_callback, seg)
    → udma_gpu_build_sg_table(page_table, &seg->umem->append.sgt)  // 本地拼，不依赖 NV
      (PA 写进 sg_dma_address)
    ↓
seg->tid = tid
    ↓
udma_ioummu_remote_map / udma_iommu_local_map
    → udma_ioummu_map(l_tid, r_tid, prot, seg->addr=gpu_va, sgt)
    → ummu_sva_matt_map(domain, gpu_va, sgt, prot)   // 真正建表
```

**关键点**：`ubcore_check_register_seg_para()` 校验时看到的 `cfg->flag` 和没打 GPU 补丁之前完全一样（udrv_data 是独立字段，不影响 flag）；`UBCORE_REG_SEG_GPU_VA` 这个内部标记是 `udma_register_seg()` 在校验**已经通过之后**，才打到局部拷贝 `tmp_cfg` 上的，所以选哪个 bit 完全不重要——它既不会被 ubcore 二次校验，也不会出现在返回给用户的 `seg.attr` 里（`ubcore_register_seg()` 最终用的是**原始未改的** `cfg->flag`，不是传给 `udma_register_seg()` 的 `tmp_cfg`）。

---

## 3. 本分支修改文件

| 文件 | 作用 |
|------|------|
| `urma/hw/udma/udma_gpu_p2p.h` | `struct udma_register_seg_ucmd`（wire 格式）、`udma_seg_ucmd_is_gpu()`（从 udrv_data 解析）、`udma_seg_cfg_is_gpu()`（内部判定）、内部私有位 `UBCORE_REG_SEG_GPU_VA` |
| `urma/hw/udma/udma_gpu_p2p.c` | GPU pin/unpin 主逻辑 + 本地 SG 构建；调用 `udma_nv_p2p_bridge.ko` 的 wrapper，不直接碰 `nvidia_p2p_*` |
| `urma/hw/udma/udma_nv_p2p_bridge.h` | 桥接模块对外暴露的 wrapper 函数声明（`udma_nv_p2p_get_pages`/`put_pages`/`free_page_table`） |
| `urma/hw/udma/udma_nv_p2p_bridge.c` | **独立 `.ko`**，`MODULE_LICENSE("Dual BSD/GPL")`，直接静态链接 `nvidia.ko`，`EXPORT_SYMBOL` 自己的 wrapper |
| `urma/hw/udma/udma_segment.h` | GPU 段结构体扩展（`is_gpu_seg`、`struct udma_gpu_p2p_mem gpu`） |
| `urma/hw/udma/udma_segment.c` | `udma_register_seg()` 入口处从 `udata` 解析 GPU 标记并打到本地 `cfg`；`udma_init_seg_cfg` / `udma_pin_pages_and_ioummu_map` / legacy 路径 / `udma_unpin_seg_pages` 分流不变 |
| `urma/hw/udma/udma_main.c` | `MODULE_SOFTDEP("pre: udma_nv_p2p_bridge")` |
| `urma/hw/udma/Makefile` | 编译 `udma.ko`（含 `udma_gpu_p2p.o`）**和** `udma_nv_p2p_bridge.ko` 两个 `obj-m` 目标，`ccflags -I$(NV_P2P_HEADER_PATH)` |
| `urma/Makefile` | OOT 独立编译 `urma/` 支持（`NV_P2P_HEADER_PATH`/`NV_MODULE_SYMVERS`） |
| `Makefile`（根） | OOT 整树编译支持 |

对应 UMDK 用户态需要改的地方（不在本仓库）：

| 文件 | 改动 |
|------|------|
| `urma_types.h` | `urma_seg_cfg_t` 新增普通字段 `uint32_t is_gpu_seg;` |
| `udma_u_segment.c` | 定义与内核一致的 `struct udma_register_seg_ucmd`；GPU 时跳过 `ummu_grant`/`ummu_ungrant`；把 `is_gpu_seg` 打进 `ucmd`，通过 `urma_cmd_udrv_priv_t` 传给 `urma_cmd_register_seg()` |
| `perftest_resources.c` | GPU 变种设置 `seg_cfg.is_gpu_seg = 1` + 64KB 对齐 |

---

## 4. 依赖 & 闭源驱动注意事项

**已实测**: `nvidia_p2p_get_pages_persistent` / `nvidia_p2p_put_pages_persistent` / `nvidia_p2p_build_sg_table_phys` 在闭源驱动上 `insmod udma.ko` 会报：

```
udma: Unknown symbol nvidia_p2p_build_sg_table_phys (err -2)
udma: Unknown symbol nvidia_p2p_put_pages_persistent (err -2)
udma: Unknown symbol nvidia_p2p_get_pages_persistent (err -2)
```

原因：

- `nvidia_p2p_build_sg_table_phys` 从来就不是官方符号，只存在于本项目自建的 `nv_module_netlab`（开源 fork）里。
- `nvidia_p2p_get_pages_persistent`/`put_pages_persistent` 是较新加入的 API（GDS 相关），部分闭源驱动版本未导出。

**修复（当前分支已采用）**：改用 NVIDIA 从最早 GPUDirect RDMA 时代起就稳定导出、`nvidia-peermem.ko` 用了十几年的经典 API：

```c
int nvidia_p2p_get_pages(uint64_t p2p_token, uint32_t va_space,
        uint64_t virtual_address, uint64_t length,
        struct nvidia_p2p_page_table **page_table,
        void (*free_callback)(void *data), void *data);

int nvidia_p2p_put_pages(uint64_t p2p_token, uint32_t va_space,
        uint64_t virtual_address, struct nvidia_p2p_page_table *page_table);
```

唯一区别：`get_pages` 强制要求非 NULL 的 `free_callback`（GPU 内存被 App 提前 `cudaFree` 时异步调用）。`udma_gpu_p2p.c` 里的 `udma_gpu_p2p_free_callback()` 目前只是**最小实现**（打日志 + 标记 `force_freed`，不做完整的向上层失效传播）——这是已知简化，TODO 见该函数注释，只要测试流程严格 "register → 用 → unregister"、不提前 `cudaFree`，就不会触发。

SG 表不再依赖 NV 侧任何符号，`udma_gpu_build_sg_table()` 本地用 `page_table->pages[i]->physical_address`（官方公开稳定字段）自己拼 `sg_table`。

- URMA/UB 总线不是 PCI 设备，**不能**调用 `nvidia_p2p_dma_map_pages`（需要 `struct pci_dev *peer`）。只用 `get_pages` 返回的 `physical_address` 走 UMMU 自己的表即可，这对非 PCIe 第三方总线是唯一可行路径。

### 4.1 换成经典 API 之后仍然 `Unknown symbol`？—— GPL/proprietary 边界，切独立 Dual BSD/GPL 桥接模块

换成 `nvidia_p2p_get_pages`/`put_pages`/`free_page_table` 之后，在真实闭源驱动上依然报：

```
udma: module using GPL-only symbols uses symbols nvidia_p2p_get_pages from proprietary module nvidia.
udma: Unknown symbol nvidia_p2p_get_pages (err -2)
```

用 `cat /proc/kallsyms | grep nvidia_p2p_get_pages` 能确认符号**确实存在**于已加载的 `nvidia.ko`（`T nvidia_p2p_get_pages [nvidia]`，还有对应的 `__crc_`/`__ksymtab_` 项）——排除了"符号真的不存在"这个可能。

原因：`udma.ko` 是 `MODULE_LICENSE("GPL")`，`nvidia.ko` 是 proprietary（`TAINT_PROPRIETARY_MODULE`）。让纯 GPL 模块直接静态链接 proprietary 模块的符号，`insmod` 阶段的符号解析（GPL taint 判定 + `CONFIG_MODVERSIONS` 开启时的 CRC 校验，这台机器 `vermagic` 里有 `modversions`）不干净，报错统一都是 `Unknown symbol X (err -2)`，没法从报错本身区分具体原因。

**修复（当前分支已采用，参照传统 RDMA 厂商模式）**：Mellanox `nvidia-peermem.ko` 本身就不是纯 GPL，而是 `MODULE_LICENSE("Dual BSD/GPL")`——这正是业界验证了十几年的标准做法：**专门切一个独立的小模块负责直接链接 proprietary 驱动的符号，License 用 Dual BSD/GPL，让真正的 RDMA 网卡驱动（这里是 udma.ko）完全不碰 proprietary 符号，只依赖这个自己完全掌控的桥接模块。**

新增 `udma_nv_p2p_bridge.ko`：

```c
// udma_nv_p2p_bridge.c
MODULE_LICENSE("Dual BSD/GPL");   // 跟 nvidia-peermem.c 一模一样

int udma_nv_p2p_get_pages(uint64_t gpu_va, uint64_t length,
			  struct nvidia_p2p_page_table **page_table,
			  void (*free_callback)(void *data), void *data)
{
	return nvidia_p2p_get_pages(0, 0, gpu_va, length, page_table,
				    free_callback, data);
}
EXPORT_SYMBOL(udma_nv_p2p_get_pages);
// ... put_pages / free_page_table 同理
```

`udma.ko`（纯 GPL）里的 `udma_gpu_p2p.c` 只调 `udma_nv_p2p_get_pages()`/`udma_nv_p2p_put_pages()`/`udma_nv_p2p_free_page_table()` 这几个我们自己的 wrapper 符号——因为 `udma_nv_p2p_bridge.ko` 和 `udma.ko` 是同一个 `urma/hw/udma/Makefile` 里的两个 `obj-m` 目标，在**同一次** kbuild/modpost 里一起编译，彼此的符号互相可见，不需要额外的 `KBUILD_EXTRA_SYMBOLS` 拼接，CRC 天然一致（我们自己同时编两个 `.ko`，不存在"跨仓库/跨编译批次"的版本错配问题）。

真正触碰 `nvidia.ko` proprietary 符号、需要 `NV_MODULE_SYMVERS`/`KBUILD_EXTRA_SYMBOLS` 静态链接的，只剩 `udma_nv_p2p_bridge.ko` 这一个模块——它复刻的正是 `nvidia-peermem.ko` 已经验证过的 license 组合，静态解析理应正常工作。

加载顺序变成三级：`nvidia.ko` → `udma_nv_p2p_bridge.ko` → `udma.ko`（`udma_main.c` 的 `MODULE_SOFTDEP` 只声明了对 `udma_nv_p2p_bridge` 的直接依赖，`nvidia` 那一跳由 `udma_nv_p2p_bridge.ko` 自己的 `MODULE_SOFTDEP("pre: nvidia")` 负责；`softdep` 只有 `modprobe` 认，`insmod` 需要手动按顺序）。

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

加载顺序（三级）：

```bash
insmod nvidia.ko
insmod urma/hw/udma/udma_nv_p2p_bridge.ko
insmod urma/hw/udma/udma.ko
```

`MODULE_SOFTDEP` 已经在各自模块里声明好依赖关系，用 `modprobe` 会自动处理；`insmod` 需要手动按上面顺序。
