# UMDK 用户态：沐曦 GPU 显存申请

当前 Cursor 工作区仍是内核仓 `urma_driver`。下面这套改动按 openEuler **umdk** 目录布局写好，拷到用户态树即可。

## 铺到 umdk

```bash
# 你的 umdk 根目录，例如 /home/han/umdk
UMDK=/path/to/umdk
cp -a umdk/src "$UMDK/"
# 或：
cd "$UMDK" && patch -p1 < /path/to/urma_driver/umdk/umdk-metax.patch
```

新文件：

- `src/urma/tools/urma_perftest/perftest_metax.c`
- `src/urma/tools/urma_perftest/perftest_metax.h`

## 行为

1. **申请**：`mcSetDevice(dev)` + `mcMalloc(&ptr, len)`（MACA 运行时，头文件 `mc_runtime.h`）。
2. **注册**：`urma_seg_cfg_t.is_gpu_seg = 1`，经 `udrv_data` 传给内核 `struct udma_register_seg_ucmd`。
3. **用户态 grant**：GPU VA 不在 CPU 页表里，`udma_u_register_seg()` **跳过** `ummu_grant` / `ummu_ungrant`。
4. **内核**：`udma.ko gpu_vendor=metax` + `udma_metax_p2p_bridge.ko` 用 `metax_p2p_acquire_mem` pin。

长度按 `page_size`（默认 4K）向上对齐，和内核 MetaX `PAGE_SIZE` 对齐一致。

## 编译 perftest

```bash
# MACA 默认 /opt/maca；也可 export MACA_PATH=/usr/local/maca
cmake -S . -B build
cmake --build build --target urma_perftest
```

CMake 找到 `${MACA_PATH}/include/mc_runtime.h` 才会定义 `HAVE_METAX_MACA` 并链接 `mcruntime`。

## 跑

```bash
# 先装内核：metax.ko → udma.ko gpu_vendor=metax → udma_metax_p2p_bridge.ko
urma_perftest write_bw -d <ub_dev> --use_metax
urma_perftest write_bw -d <ub_dev> --use_metax=0   # 指定 GPU 0
```

不要和 `--va` / hugepage 一起用。
