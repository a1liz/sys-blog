---
title: "一块 KV Cache 的旅程：DGX Spark 上 GPU → pinned → SSD 的数据路径全取证"
date: 2026-07-19
tags: [storage, nvme, dma, gpu, uma, kv-cache, vllm, dgx-spark, odirect, lmcache]
category: "storage"
summary: "在两块 DGX Spark(GB10 UMA)上为 DeepSeek V4 Flash 启用 SSD KV offloading 后,我们用 pagemap、FIEMAP、torch.profiler 三件工具逐跳取证了数据路径:GPU 显存 VA → pinned 页帧 → NVMe LBA。实测 GPU↔DRAM 的 copy engine 约 55 GiB/s,pinned↔NVMe 的 O_DIRECT 达 7.8–9.5 GiB/s(buffered 的 3–13 倍),并定位了 LMCache 真实搬运路径是「SM gather kernel + cudaMemcpyAsync」两级,而不是一次 plain cudaMemcpy。"
---

<!-- 正文 -->

## 问题:KV 放不下了,放 SSD 的代价是什么

双 DGX Spark 跑 DeepSeek V4 Flash(TP=2,FP8,300k 上下文),GPU KV 池受 `gpu_memory_utilization=0.85` 预算限制。UMA 架构下 CPU/GPU 共享同一片 128 GiB DRAM,"offload 到 CPU 内存"只是同片 DRAM 左右互搏,**唯一的真实容量增量在 NVMe**。

于是接入了 LMCache(`LMCacheMPConnector` + 每节点 `lmcache server`,L2 fs adapter 落盘)。功能验证通过后,一个自然的问题:*数据到底是怎么从 GPU 跑到 SSD 的?每一跳谁搬的、多快、能不能不搬?*

本文用三件工具把这条路逐跳拆开取证:

- `/proc/self/pagemap` — 把 pinned 缓冲的虚拟地址解析成 DRAM 物理页帧号;
- FIEMAP ioctl — 把盘上文件偏移解析成 NVMe 物理块地址;
- `torch.profiler` — 抓 GPU 侧真实启动的 kernel / memcpy 名字。

## 地图:三个站点,三种地址

先建立坐标系。这趟旅程只有三个站点,每个站点有自己的"地址语言":

| 站点 | 真实身份 | 类比 | 实测地址(本次取证) |
|---|---|---|---|
| GPU 显存 | KV 池,CUDA 驱动分配 | 工厂内部货架(厂内编号) | 虚拟地址 `0x32ee00000` |
| pinned 页 | L1 中转缓冲,cudaHostAlloc | 市区仓库的**锁定专区** | 虚拟地址 `0x339200000` → 物理页帧 `0x1826354`(物理地址 ≈103.5 GiB) |
| SSD 文件 | NVMe 上的 `.data` 文件 | 郊区冷库(LBA 货位) | 设备物理偏移 `0x13d5f000000`(LBA 2662301696) |

关键认知:DGX Spark(GB10)上**前两个站点是同一片 DRAM 芯片**,只是归属不同的分配器;第三个站点是真正的另一块硬件(三星 3.7TB NVMe)。

`/proc/self/maps` 里 pinned 缓冲长这样——匿名私有映射,cudaHostRegister 把它钉在物理页上:

```
339200000-33a200000 rw-s 00000000 00:01 54307   /dev/zero (deleted)
```

而 pagemap 显示它的页帧**并不连续**(内核零散发的 2 MiB 大页):`PFN 0x1826354`、`0x97c084`、`0x1effa6c`、`0xdeb16c`……这正是下一节"为什么要聚合"的伏笔之一。

## Hop 1:GPU → pinned —— 不是一次 cudaMemcpy,是两级

直觉上这一跳就是 `cudaMemcpy(DtoH)`。但拿 torch.profiler 抓一次真实传输(复现脚本 `kernel_repro.py`),看到的事件是:

```
void (anonymous namespace)::multi_layer_block_transfer_kernel<uint4, false, (GPUKVFormat)3>(
    MemoryObj4<uint4>, uint4**, long const*, int, PageBufferShapeDesc, int, int)
    (calls=1, cuda_time=10us)
cudaMemcpyAsync                  (calls=1)
Memcpy DtoH (Device -> Pinned)   (calls=1, cuda_time=2us)
```

两级,各有分工:

| 阶段 | 观测到的名字 | 执行者 | 干什么 |
|---|---|---|---|
| A | `multi_layer_block_transfer_kernel` | **SM 计算 kernel** | 从几十个**按层分开的 KV tensor** 里,按 block_ids 把碎片 gather 成一个连续暂存块(格式 `NL_X_NB_BS_HS`,MLA 布局) |
| B | `cudaMemcpyAsync` → `Memcpy DtoH (Device -> Pinned)` | **copy engine** | 暂存块 → pinned 主机页,直达 DMA,无 bounce |

为什么必须两级:vLLM 的 KV 池是**几十个 tensor**(每层一个),而落盘要的是**一个连续块**。`cudaMemcpy` 只会连续到连续,做不了跨 tensor 聚合——所以 LMCache 先用 SM kernel 做 gather(load 时反向 scatter),拼成连续块之后才轮到 copy engine。

代码侧对应 LMCache MP server 的 `gpu_transfer.py`:`lmc_ops.multi_layer_block_kv_transfer(...)` 之后紧跟 `lmcache_memcpy_async_d2h(...)`。

**pinned 在这里还有个隐藏作用**:若目标是 pageable 内存,CUDA runtime 会先拷进内部 pinned bounce buffer 再 DMA(多一次拷贝);目标是 pinned 页才是"一段直达"。kineto 标签 `Device -> Pinned` 正是证据。

实测这一跳 **54.8 GiB/s**——DRAM 内部搬家,不走任何总线。

## Hop 2:pinned → SSD —— NVMe 控制器接手,CPU 只下命令

这是最关乎"零拷贝"的一跳。开了 `O_DIRECT` 之后:

1. LMCache server 的工作线程发一条 `pwritev`,内容只有三样:**源**(pinned 页的物理地址)、**目的地**(文件 LBA)、**长度**;
2. CPU 转头去干别的——**NVMe 控制器用自己的 DMA 引擎,直接从那些 PFN 页帧把数据读走,写进 LBA 2662301696**;
3. 实测写 **7.84 GiB/s**、读 **9.50 GiB/s**。

对照老路(buffered):CPU 亲手把数据从 pinned 页**再抄一遍到内核 page cache**(多一次 CPU 拷贝),写只剩 2.84 GiB/s,读更是只有 0.70 GiB/s。O_DIRECT 把 CPU 从"搬运工"降级成"调度员"。

### 插曲:O_DIRECT 差点没生效

fs adapter 的 `use_odirect` 默认关闭,打开后日志却报:

```
Cannot use O_DIRECT for writing size 16480832, not aligned to block size 4096.
```

O_DIRECT 有三条硬规矩:**缓冲地址、文件偏移、传输长度都必须是磁盘块(4 KiB)的整数倍**——DMA 按整托盘运输,不收零头。而 KV 对象是 16480832 字节,模 4096 余 2624。

解法是物流常规操作——**垫满一个托盘再运**:申请页对齐的临时缓冲区,对象拷进去,尾部补 1472 个零字节凑整盘,NVMe 整盘运走;读回按真实长度裁掉垫料。代价一次 CPU memcpy(≈1.5ms),换来 NVMe DMA(≈2ms),远好于 page-cache 老路(≈23.6ms)。

盘上证据:真实对象 16480832 字节,12 个数据文件每个都是 **16482304** 字节(= 对象 + 1472 padding)。

## 回程:完全对称

SSD 命中时倒放:`preadv` 一条命令,NVMe DMA 把 LBA 数据直接塞回那几个 PFN 页帧(9.50 GiB/s),SM kernel scatter 回各层 tensor(CE memcpy 55 GiB/s),prefill 不用重算。实测同样本二次运行,首条请求 16.28s → 12.20s。

## 数字总表(1 GiB 每跳实测)

| 跳 | 路径 | 带宽 |
|---|---|---|
| ① GPU→pinned | SM gather + CE memcpy | 54.8 GiB/s |
| ② pinned→SSD | buffered 写 | 2.84 GiB/s |
| ② pinned→SSD | **O_DIRECT 写** | **7.84 GiB/s** |
| ③ SSD→pinned | buffered 读 | 0.70 GiB/s |
| ③ SSD→pinned | **O_DIRECT 读** | **9.50 GiB/s** |
| ④ pinned→GPU | CE memcpy | 55.0 GiB/s |

## 边界:为什么没有"真·GPU 直通 SSD"

GDS(GPU Direct Storage)才是字面意义的 GPU→SSD zero-copy。实测 GB10 上此路不通:`cuFileDriverOpen` 能成功,但平台探测判定 "GDS not supported"——`GPU_DIRECT_RDMA_SUPPORTED=0`、NVML 拿不到 PCIe BAR、`pci_p2pdma` 不支持。UMA SoC 的 GPU 没有独立 PCIe BAR / P2P DMA 寻址能力,GPU↔NVMe 直通 DMA 在架构上不存在。

所以 **GPU →(55 GiB/s DMA)→ pinned →(8–9.5 GiB/s NVMe DMA)→ SSD、CPU 零拷贝**,就是 GB10 的物理最优路径。

## 一句话版(给普通学生)

> GPU 和 SSD 都不会"自己找路":GPU 厂内编号、SSD 冷库货位,互相不认识。中间必须有个"锁定专区"(pinned 页)交接——GPU 的传送带(SM kernel 拼箱 + copy engine)把货放过去,NVMe 的传送带(DMA)再取走送冷库。CPU 全程只在旁边开单子,一箱货都没亲手搬。

## 复现方法

- `scripts/addr_trace.py` — 地址级取证(GPU VA / pinned VA / pagemap PFN / FIEMAP LBA + 各跳带宽)
- `scripts/kernel_repro.py` — torch.profiler 抓 `multi_layer_block_transfer_kernel` + `cudaMemcpyAsync` 的最小复现
- `scripts/hop_bench.py` — 1 GiB 四跳带宽基准(buffered vs O_DIRECT)
- 部署侧:LMCache `LMCacheMPConnector` + 每节点 `lmcache server`(`--l2-adapter '{"type":"fs","base_path":...,"use_odirect":true}'` + padding 补丁)
