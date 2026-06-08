# GPU P2P Segment Registration — User-Space ABI (Phase-0 / Scheme A)

## Flag

Set **bit 14** in `urma_reg_seg_flag.value` / ioctl `flag` field:

```c
#define UBCORE_REG_SEG_GPU_VA  (1U << 14)

seg_cfg.flag.value |= UBCORE_REG_SEG_GPU_VA;
```

Kernel header: `urma/hw/udma/udma_gpu_p2p.h`

## Register segment (PEERMEM / GPU VA path)

```c
cuMemAlloc(&d_ptr, ALIGN_UP(size, 65536));
/* CUDA GPUDirect RDMA / third-party P2P registration required */

seg_cfg.va   = (uint64_t)d_ptr;
seg_cfg.len  = aligned_size;
seg_cfg.flag.value |= UBCORE_REG_SEG_GPU_VA;
seg_cfg.flag.bs.access = URMA_ACCESS_LOCAL_ONLY;  /* Phase-0: skip remote UMMU */
urma_register_seg(ctx, &seg_cfg);
```

## UMDK manual changes

1. **`udma_u_register_seg()`** — if `cfg->flag.value & UBCORE_REG_SEG_GPU_VA`, **skip `ummu_grant()`**.
2. Pass GPU VA through existing `URMA_CMD_REGISTER_SEG` TLV (`va`, `len`, `flag`).
3. Align GPU allocation to **64KB**; length multiple of **64KB**.
4. Do **not** CPU-dereference GPU VA in perftest / UMQ datapath.

## IOCTL

No new TLV in Phase-0. Reuse `UBURMA_CMD_REGISTER_SEG` with GPU VA in `va` and `UBCORE_REG_SEG_GPU_VA` in `flag`.

## dmesg markers

| Tag | Meaning |
|-----|---------|
| `NV_P2P_DEBUG magic=0xdeadbeef` | NV page table VA/PA/BAR/DMA dump |
| `UDMA_GPU_P2P_TEST magic=0xdeadbeef` | URMA register / inject / post_wr verification |
