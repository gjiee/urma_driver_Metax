# URMA / UDMA GPU 直访与 UMMU 映射 —— 专利点技术交底提纲

> **性质**：本文件面向 **专利挖掘 / 技术交底**，叙述可主张的**机制与方法**，不是工程实现清单、不是性能调优笔记。  
> **范围**：UMDK（用户态 URMA）、`urma_driver`（uburma/ubcore/udma）、修改版 NVIDIA P2P（`nv_module_netlab`）、与 UMMU/IOMMU（`ummu_sva_matt_map`）协作。  
> **声明**：以下为技术侧候选创新点归纳，**不构成法律意见**；是否具备新颖性/创造性需检索现有技术并由专利代理人撰写权利要求。

---

## 1. 要解决的技术问题（问题陈述）

在 **UnifiedBus / URMA** 体系中，远端内存访问以 **段（segment）** 为中心，数据面地址模型为：

```text
WQE：远程地址 = UBVA（进程/设备可见 VA） + token_id（安全对象）
硬件：VA + tid → 查 UMMU/IOTLB → 物理访存
```

该模型原生面向 **主机用户态 VA + pin_user_pages + UMMU matt**。  
要把 **GPU 设备内存（HBM）** 纳入同一套 URMA 语义时，会出现经典 IB GPUDirect RDMA **无法直接套用** 的断层：

| 经典 IB GDR | URMA / UB 现状问题 |
|-------------|-------------------|
| `ibv_reg_mr` + rkey/lkey | 无等价 MR；是 **seg + token + UBVA** |
| `nvidia-peermem` 挂在 `ib_umem` | UB 路径 **不走** InfiniBand umem/peermem client |
| NIC MTT 填 **peer DMA 地址** | UB NIC 数据面填的是 **VA+tid**，靠 **UMMU** 翻译 |
| 跨节点靠 rkey 交换 | 跨节点靠 **export UBVA=GPU VA + token**，import **不再 pin** |

**核心问题**：如何在不改变（或最小改变）URMA 数据面 WQE 语义的前提下，使 **CUDA GPU VA** 成为合法的 UBVA，并让 UMMU 将「GPU VA + token」解析到 **GPU 侧可访存物理地址**，从而完成本机及潜在跨节点的 GPU RMA。

---

## 2. 与现有技术的对照（便于划清“非专利点”）

下列内容多属现有技术或工业常法，**不宜单独作为主权利要求**，仅作背景或从属：

- NVIDIA `nvidia_p2p_get_pages` / `*_persistent`、64KB GPU 页约束  
- `nvidia-peermem` + mlx5/`ib_umem` 的 peer_memory_client 模式  
- dma-buf 导出 + `ibv_reg_dmabuf_mr` / 附件映射  
- Host bounce / shadow + `cuMemcpy`（NCCL `NET_DISABLE_GDR`、swiotlb 同类思路）  
- 单纯“GPU 内存做 RDMA 缓冲区”的产品目标  

**本申请工作的差异化轴心**：不是“又做了一种 GDR”，而是 **在 UBVA+token+UMMU 地址体系下，把 GPU 物理页接进 matt，并保持 WQE 仍携带 GPU VA**。

---

## 3. 候选专利点（机制级，按主张强度排序）

### 专利点 A（主）：GPU VA 作为 UBVA 键的 UMMU 双域建表方法

**一句话**：将 GPU 分配得到的设备虚拟地址直接作为 URMA 段的 UBVA；在注册时用同一套 GPU 物理页散列表，分别（或按需）写入 **本地上下文 tid 域** 与 **token 远端域** 的 UMMU matt，使后续 WQE 仅携带「GPU VA + token_id」即可完成本地/远端访存翻译。

**机制要素**：

1. 段注册输入包含：GPU VA、长度、token_id，以及“设备内存段”判别信息；  
2. 从 GPU 驱动获取与该 VA 对应的物理页集合；  
3. 以 **GPU VA 为翻译键**（而非主机 CPU VA、亦非把 PA 直接写入 WQE）；  
4. 调用 UMMU 映射接口，建立至少：  
   - **Local matt**：服务本端发起方用本地 tid 访问；  
   - **Remote matt**（非 LOCAL_ONLY 时）：服务对端仅持有 token、无需再 pin 即可按 Owner 的 GPU VA 访问；  
5. 数据面 **不修改** 为“WQE 填 DMA PA”模型，硬件仍按 VA+tid 查表。

**相对现有技术的差异**：

- IB GDR：注册结果体现在 rkey/MTT 的 DMA 映射；数据面不强调“GPU VA 即总线 VA”。  
- 本方法：GPU VA **一等公民化** 为 UB 地址空间中的段基址，并与 **token 安全模型 + import-不-pin** 绑定。

**可主张形态（示意）**：一种异构内存段注册方法 / 一种基于 UMMU 的设备内存远端访问方法 / 一种 token 域与本地域双映射的 GPU 段建立装置。

---

### 专利点 B（主）：无 struct page 的“物理地址型 SG”接入 UMMU 的适配方法

**一句话**：对 GPU P2P 得到的物理地址表，构造 `sg_page = NULL`、`sg_dma_address = GPU_PA` 的 scatterlist，并把它作为 UMMU `matt_map` 的输入，从而在 **无主机 page 结构、且不做 NIC PCI `dma_map_pages`** 的条件下，完成设备内存到总线翻译表的安装。

**机制要素**：

1. 通过 GPU P2P API 取得 persistent page table（物理地址列表）；  
2. 合成专用 SG：页指针为空，DMA 地址字段承载 **GPU PA**（或后续可替换为 BAR/UBA 形态地址）；  
3. UMMU/IOMMU 路径识别并接受该类 SG，按 VA→该物理/总线地址写入 matt；  
4. 可选扩展：在 remote map 前将 GPU PA **变换** 为 decoder/UB/AXI 可寻址的 BAR/UBA（跨节点或跨总线形态），再写入远端域。

**相对现有技术的差异**：

- 常规 peermem：倾向 `nvidia_p2p_dma_map_pages(pdev)` 得到 **对该 PCI 设备可见的 DMA 地址**，再进 NIC MTT。  
- 本方法：面向 **UMMU SVA matt** 消费者，显式采用 **PA-as-dma_address + null page** 的适配契约，把“谁做 DMA map”从 NIC PCI 对移到 **UB 地址翻译平面**。

**可主张形态（示意）**：一种将设备物理页表适配为 IOMMU/UMMU 映射输入的方法；一种免主机 page 结构的设备内存注册方法。

---

### 专利点 C（主/组合）：旁路 peermem client，由 UB NIC 驱动直连 GPU P2P 完成段生命周期

**一句话**：在 UnifiedBus NIC 驱动（udma）内直接调用 GPU 厂商 P2P pin/unpin，完成段的 pin→建表→unpin 闭环，而不经过 InfiniBand `peer_memory_client` / `nvidia_peermem.ko`。

**机制要素**：

1. 段注册路径根据“GPU 段”标志分支；  
2. GPU 分支：驱动内 `get_pages_persistent` → 构造 SG → UMMU map；  
3. 段注销：先 UMMU unmap，再 `put_pages_persistent`；  
4. 模块依赖关系保证 GPU 驱动先于 NIC 驱动提供符号。

**相对现有技术的差异**：所有权从“IB 子系统 umem 回调”转移到 **UB 段对象生命周期**，与 token_id、LOCAL_ONLY、remote matt 同事务提交/回滚。

> 单独看“驱动调 P2P API”创造性可能偏弱；**与专利点 A/B 组合主张**时更稳。

---

### 专利点 D（从属/系统）：设备内存段的判别与安全绑定（flag + token 门控）

**一句话**：在通用 `REGISTER_SEG` ABI 上用标志位区分主机 VA 与 GPU VA；GPU 路径强制绑定有效 `token_id`，使设备内存段天然纳入 URMA token 访问控制，而非另建一套 GPU MR 命名空间。

**机制要素**：

- 用户/内核约定的 GPU_VA 标志（如 flag bit14）；  
- GPU 段注册失败条件：无 token / 非对齐 / 页大小不符；  
- Export 给对端的仍是 **GPU VA + token_id**，PA 永不导出。

**差异**：把“异构内存类型”编码进 **既有段注册 ABI + token 模型**，避免并行的 dmabuf/fd 专用通道（后者可作为另一方案并存）。

---

### 专利点 E（从属/演进）：跨总线地址形态转换（GPU PA → BAR/UBA/AXI 可寻址）

**一句话**：当 UB 发起方与 GPU 不在同一可直接译码的地址平面（例如需经 Root Complex / 需 BAR 窗口）时，在写入 UMMU remote/local matt 之前，将 P2P 得到的 GPU PA 转换为对 UB decoder 或对端可见的总线地址，再完成映射。

**机制要素**：

- 检测访问范围：LOCAL_ONLY vs 跨节点/跨 RC；  
- 地址变换级：PA → BAR aperture / UBA / 其他 fabric 地址；  
- 变换后的地址进入 matt，WQE 侧仍可保持 GPU VA（VA→变换后总线址）。

**现状**：代码与文档中已标识需求，**实现未闭合**——适合作为 **续案/从属权利要求** 或预埋交底，不宜写为“已实施主权利要求”的唯一支撑。

---

### 专利点 F（应用层/弱）：GPU 语义与 NIC 宿主缓冲分离的双模式测量与回退

**一句话**：同一应用缓冲呈现为 GPU 指针，同时维护 host shadow；通过可配置策略选择：

- **直访模式**：注册 GPU VA（专利点 A–D）；  
- **代理模式**：仅注册 shadow，在 post/完成路径插入 D2H/H2D；  
- **纯代理测量模式**：同代理分配，但关闭 stage，用于隔离 NIC↔DRAM 与 GPU stage 的代价。

**专利性评估**：bounce/proxy 本身现有技术强；若有可主张点，更可能在于 **与 URMA 段模型结合的模式切换及“纯 shadow 对照测量”方法**（偏测试/系统方法，创造性通常弱于 A/B）。建议作从属或另案，不作为核心。

---

## 4. 系统级方法权利要求骨架（组合叙述）

下列“方法步骤”便于代理人改写成独立权利要求（中/美格式需再加工）：

**方法 M1（GPU 段注册与访问）**

1. 在加速器上分配设备内存，得到设备虚拟地址 GVA；  
2. 分配 URMA token，构造段描述：基址=GVA，类型=设备内存；  
3. 由 UB 设备驱动向 GPU 驱动请求锁定 GVA 对应物理页；  
4. 将物理页表示为无主机 page 的地址列表结构；  
5. 以 GVA 为键，在 UMMU 中建立与所述 token 关联的翻译项（及可选的本地上下文翻译项）；  
6. 发起方提交带有目标地址=GVA、对象=token 的远端写/读描述符；  
7. 硬件根据 GVA 与 token 查询 UMMU 完成对设备内存的 DMA。

**方法 M2（对端零 pin 访问）**

1. Owner 导出 {GVA, len, token}；  
2. Peer import 仅绑定 token，不锁定物理页；  
3. Peer 以 Owner 的 GVA 为远端地址发起 WRITE/READ；  
4. 访存翻译发生在 Owner 侧已建立的 remote matt。

**装置**：UB NIC 驱动模块、UMMU 管理模块、GPU P2P 适配模块、用户态 URMA 库中的对应单元。

---

## 5. 建议的权利要求簇（给代理人）

| 簇 | 建议独立项重心 | 从属方向 |
|----|----------------|----------|
| **簇 1** | GPU VA = UBVA + UMMU matt（本地/远端） | token 门控、LOCAL_ONLY、import 不 pin、WQE 仍携 VA |
| **簇 2** | null-page + PA-as-sg_dma_address 接入 UMMU | 与 PCI dma_map 路径的互斥/可选、64KB 页、persistent pin |
| **簇 3** | UB 驱动直连 P2P 的段生命周期 | 与 peermem client / dmabuf 路径的并列或切换 |
| **簇 4（续案）** | PA→BAR/UBA 变换后再 matt | 跨 RC、跨节点、AXI/UB decoder |
| **簇 5（弱）** | proxy / proxy_pure 对照与回退 | 与直访模式的自动降级策略 |

---

## 6. 明确建议“不要写成专利点”的工程事项

- 编译开关、模块加载顺序、`MODULE_SOFTDEP`  
- perftest 参数名（`peermem`/`proxy_pure`）、日志、对齐 pad 的代码细节  
- 某次 ABI 未接 bit14、grant 误调等 **缺陷/修复**  
- 单纯性能数字（x8 降级、~6.5 GiB/s）——属部署/拓扑事实，不是方法发明  
- 与标准 CUDA/GDR 教程重复的“先 H2D 再 RDMA”教程式描述  

---

## 7. 实施与仓库对应（供交底附件，非权利要求）

| 机制 | 主要落点 |
|------|----------|
| GPU pin + SG 合成 | `urma_driver`：`udma_gpu_p2p.c`；`nv_module_netlab`：`nvidia_p2p_build_sg_table_phys` |
| 双域 matt | `udma_segment.c` → `udma_ioummu_remote_map` / `udma_iommu_local_map` → `ummu_sva_matt_map` |
| 数据面 VA+tid | `udma_jfs.c`（刻意不改 WQE 填法） |
| 用户态模式/代理 | UMDK：`perftest_gpu_alloc.*`、`perftest_resources.c`、staging 热路径 |
| 流程说明 | `docs/GPU_P2P_REGISTER_FULL_FLOW.md` 等 |

---

## 8. 一句话总览（可放交底首页）

**在 UnifiedBus/URMA 的「UBVA + token + UMMU」体系下，提出一种将 GPU 设备虚拟地址直接注册为段地址、以无主机 page 的 GPU 物理地址散列表填充 UMMU 本地/远端翻译域、并保持数据面描述符仍携带 GPU VA 与 token 的方法与装置；从而在不依赖 InfiniBand peermem 客户端、不将 DMA PA 写入 WQE 的前提下，实现对本机及潜在跨节点 GPU 内存的统一 RMA 访问。**

---

## 9. 待代理人/检索确认的风险点

1. 是否已有“非 IB 网络栈 + GPU P2P + SMMU/IOMMU VA 映射”的近似专利（含国产互连、CXL、自定义 RDMA）。  
2. `sg_page=NULL` + PA 填 `sg_dma_address` 再喂 IOMMU 是否已被其他驱动公开使用。  
3. “import 不 pin、仅 token”若已在 URMA 主机段专利中覆盖，则 GPU 扩展可能仅适合从属项。  
4. 与 NVIDIA 自身 GDR/P2P 专利族的权利要求边界，避免将厂商 API 步骤写成独立发明的唯一特征。  

---

*文档版本：基于 Scheme A 设计与实现对齐整理；跨节点 PA→BAR/UBA 为规划机制，写入权利要求时需按实施状态区分“已实现 / 可预期”。*
