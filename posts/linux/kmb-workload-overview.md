---
title: "给 Linux 内核做基准测试：Kernel Multi-Bench 的 Workload 图鉴与实战"
date: 2026-08-21
tags: [linux, kernel, memory-management, benchmark, zram, mthp, swap, valkey, mongodb, ycsb]
category: "linux"
summary: "解析 Kernel Multi-Bench（KMB）框架的设计思路——用 systemd + grubby 在真实硬件上反复重启、每个脚本独占一次干净开机，消除跨测试干扰；逐套件剖析其预设 workload 分别模拟匿名页压力（ZRAM/mTHP）、分层内存 swap（PMEM/BRD）、文件页 LRU 回收（MongoDB/YCSB）三大内核 MM 战场；并在 8C/15G 虚拟机上完成适配与首轮基线实测。"
---

<!-- 正文 -->

## 问题：怎么在多个内核之间做公平的性能对比？

内核子系统的改动（内存管理、调度器、块层）效果往往只有几个百分点，而测试环境的噪声——上次测试残留的页缓存、温度导致的降频、系统后台任务——轻松就能盖过真实差异。跑一次基准容易，跑一次**可信**的基准很难。

常见的野路子是在同一台机器上换内核重启，手动跑几个测试。问题在于：

1. **交叉污染**：上一个测试留下的页缓存、碎片化的内存、被降频的 CPU，都会渗进下一个测试；
2. **热漂移**：连续压测导致机器发热降频，越跑越慢；
3. **流程不可复现**：手动编排容易漏步骤，测出来的数字没法对别人解释。

[Kernel Multi-Bench（KMB）](https://github.com/) 给出的答案是：**把"重启"变成流水线的基本单元**——每个测试脚本独占一次干净开机，跑完就重启，由 systemd + grubby 全自动驱动，直到所有脚本在所有内核上跑完。

---

## 方法：一个 reboot 驱动的测试流水线

### 框架设计

```
kmultibench run [inventory] <suite-dir> <run-name>
   │
   ├─ bench-run        解析参数，无清单时自动发现 /boot/ 下的内核
   ├─ bench-preflight  校验清单，跑 .prepare.sh 准备依赖
   ├─ bench-start      保存状态，启用 systemd 服务，重启进第 1 个内核
   │
   └─ [每次开机，systemd 调 kmultibench next]
        bench-next     → 恢复状态，跑下一个脚本，tee 到日志，推进游标，重启
                       → 脚本跑完 → 切下一个内核再重启
                       → 内核跑完 → 清理退出
```

几个值得注意的设计决策：

- **一脚本一重启**：重启是最彻底的隔离——页缓存清空、碎片整理、频率复位。脚本甚至可以随意改内核 cmdline、切 sysfs 开关、加载需要重启的模块，因为反正每次开机都是新的。
- **状态序列化到 `$KMB_ENV`**（`/var/lib/kmultibench/kmultibench.running.env`），每次开机重新 source，整个流水线抗重启。
- **脚本自包含可直接跑**：每个测试脚本都能脱离框架单独执行，开发和调试不需要跑整个循环。
- systemd unit 用 `ConditionPathExists=/kmultibench.runner` 保证只在 benchmark 进行中激活，`Type=idle` 等其他启动任务完成再开跑。

限制也要说清楚：**内核 panic 不处理**（机器挂住需要人工干预，可以配合 kdump 改进）；重启循环意味着测试吞吐量低——用干净换可信。

### 重启次数的规律

```
重启次数 = 1（进入第一个内核） + Σ(每个内核的脚本数)
```

单内核 × 4 脚本 = 5 次重启；2 内核 × 4 脚本 = 9 次。

---

## Workload 图鉴：三套件覆盖内核 MM 三大战场

KMB 的预设套件不是随便找的负载拼盘，而是精心覆盖了内核内存管理的三条主战线：**匿名页换出**、**分层内存 swap**、**文件页回收**。

### 套件一：匿名内存压力 `anon-memory-pressure-basic-RAMX2G-CPUX12`

为 2G 内存小机器设计，**所有工作集都故意超过物理内存**，逼出持续的回收/换出流量。

| 脚本 | 负载 | 模拟场景 | 资源配置 |
|------|------|----------|----------|
| `10-blk-zram-4k-j12` | 内核编译（tinyconfig，-j12，3 轮） | 真实开发者编译负载；THP 全关作 4K 基线 | ZRAM swap；THP=never |
| `11-blk-zram-64k-j12` | 同上 | **mTHP 64K 大页对照**——测大页收益 | ZRAM swap；THP 64K=always |
| `20-redis` | valkey GET 压测（300 万 key ≈ 3G）+ bgsave | 内存数据库/缓存服务，数据集超内存压出 swap 抖动 | cgroup 隔离；zswap 关 |
| `30-vm-scala-global` | usemem 2 线程 × 1536M（共 3G） | 纯匿名页分配/触碰压力 | cgroup max；ZRAM；THP 64K=always |

10 和 11 是一组对照实验：同一负载只在 THP 配置上差一步，3 轮取均值——这正是 KMB "每脚本一重启"设计的典型用法。

### 套件二：swap 介质对比 `anon-usemem-swap-basic-RAMX64G-CPUX32`

64G 裸机专用。核心思路是用 `memmap=` 内核参数从物理内存里挖 48G 出来伪装成 PMEM 设备（模拟 CXL/分层内存），当作 swap 用；再用 BRD（内存盘）做对照组，分离"swap 介质"这个变量。

| 脚本 | 负载 | 模拟场景 |
|------|------|----------|
| `10-prepare-pmem` | grubby 加 memmap，挖 48G 伪 PMEM | 建立设备（靠框架的重启生效） |
| `20/30-usemem-pmem-*` | 单线程 52G / 32 线程 × 1536M | 线性/并行冷数据换入换出 |
| `40-cleanup-pmem` | 移除 memmap | — |
| `50/60-usemem-brd-*` | 同上，swap 换成 BRD | swap 后端介质对照 |
| `70-usemem-brd-36t-rand` | 36 线程 + **随机访问**（27G） | 非顺序页访问换页，更接近真实负载 |

### 套件三：文件页回收 `mongodb-ycsb-basic-RAMX12G-CPUX16`

与前两套的匿名页互补，打的是**文件页 LRU / refault** 路径。配比设计相当讲究：

- MongoDB 跑在 docker 里，memory cap 12G；
- **WiredTiger 缓存被压到 cap 的 1/4**（默认是 1/2）——让热集留在内核页缓存而不是 mongod 的匿名内存里，确保压到的是内核而不是数据库自己的缓存；
- 1100 万条记录（~12.5G 磁盘）> 页缓存预算（~8.5G）——内核必须边服务边回收；
- 每设备 IOPS 限 5 万——防止纯 IO 打满掩盖 MM 行为；
- **固定 op 数而非固定时长**——`workingset_refault_*` 计数随时间亚线性增长（shadow entry 本身也会被回收），等量工作才让 IO 量和 refault 计数跨轮可比。

| 脚本 | YCSB 负载 | 模拟场景 |
|------|-----------|----------|
| `10-workloada` | 50/50 读写 | 混合交易（订单） |
| `20-workloadb` | 95/5 | 读多写少（内容站） |
| `30-workloadc` | 100% 读 | 纯读缓存（用户画像） |
| `40-workloadd` | 95 读 + 5 插入 | 读最新（feed/时间线） |
| `50-workloade` | 95 扫描 + 5 插入 | 短范围扫描（列表页） |
| `60-workloadf` | 读-改-写 | 事务型（计数器/库存） |

### 一页速览

| 维度 | anon-pressure | usemem-swap | mongodb-ycsb |
|------|---------------|-------------|--------------|
| 内存类型 | 匿名页 | 匿名页 | **文件页** |
| 规模 | 2G / 12C | 64G / 32C | 12G / 16C |
| swap | ZRAM | pmem / BRD | 关闭（靠 cap 逼回收） |
| 内核观察点 | mTHP、zram、回收延迟 | swap 路径、多线程缺页 | LRU/refault、workingset |

---

## 实战：在 8C/15G 虚拟机上落地

### 适配原则

预设套件是按特定硬件规格调好配比的（工作集 ≈ 1.3~1.5 × 可用内存），换机器不能直接跑，要**等比缩放保持设计意图**：

| 参数 | 原值（2G/12C） | 适配值（15G/8C） |
|------|----------------|------------------|
| 编译并发 | `-j 12` | `-j 8` |
| valkey 数据集 | 300 万 key（3G） | 1200 万 key（目标 12G） |
| usemem | 2 × 1536M | 2 × 10G |

### 踩过的坑

Ubuntu 24.04 上跑这套东西，一天之内能踩的坑基本踩全了：

1. **grubby 缺失**：Ubuntu 默认不带，`apt install grubby` 解决——这是 KMB 切内核的根基。
2. **`make -C` 死循环**：`furniture/kmb.mk` 用相对路径向上找 `.kmb-root`，`dirname .` 还是 `.`，永远到不了终止条件。在仓库根跑 `make prepare` 没事，`make -C furniture/xxx` 必卡死。教训：**聚合 prepare 按目标拆开跑**——`make usemem` 用普通用户，`sudo make valkey-prepare` 用 root，避免 root 属主污染 git 目录。
3. **polkit 密码轰炸**：非 root 手动跑脚本时，`stabilize_performance` 停 systemd timer 每停一个弹一次密码。测试脚本就是设计成 root 跑的，别挣扎。
4. **systemd 托管的 valkey 抢端口**：发行版包装的 valkey-server 会开机自启占 6379，`systemctl disable --now valkey` 之后 framework 自己管理的实例才能正常起停。
5. **大数据集加载慢于框架假设**：`bench.sh` 起 server 后只 `sleep 3` 就开始压测，3G 的 rdb 够用，12G 的要加载一分钟——压测全打在 `LOADING` 状态上，rps 全零。修法是轮询 `info persistence` 等 `loading:0`。

### 首轮基线（单内核 7.0.0-30-generic）

| 负载 | 结果 |
|------|------|
| 内核编译 · THP=never | 38.03 / 44.28 / 39.43 s（均值 40.58s） |
| 内核编译 · mTHP 64K | 36.97 / 38.60 / 41.32 s（均值 38.96s，**快约 4%**） |
| usemem 2×10G | 22.13s · 189% CPU · 峰值 RSS 7.6G |
| valkey 12.6M key | 冷启动 12–19k rps（24ms 延迟）；预热后瞬时 457k rps；bgsave 33 分钟 |

两个值得展开的现象：

**mTHP 64K 的正向收益**：编译负载 CPU 利用率只有 520~580%（8 vCPU），说明瓶颈在内存路径而非 CPU 饱和——正是大页起作用的场景。4% 的差距方向符合预期（更少的 TLB miss 和缺页次数），但单内核单轮 run 且在 VM 里，只能当趋势参考，真正的结论要等双内核对比。

**valkey 的冷热 30 倍差距**：数据集实际占 16.2G（每 key 约 1.35KB，我按 1KB 估的元数据开销漏了），略超 15G 物理内存。于是前半段 GET 全在触发 swap-in 缺页，延迟 24ms、rps 12–19k；工作集逐渐 fault 回内存后，瞬时 rps 爬到 457k。这个 30 倍的差距本身就是一个非常直观的内存局部性教材——也说明**数据集配比是这个测试的灵魂**，差一点就测到完全不同的东西。

---

## 结语

KMB 的价值不在某个具体负载，而在它把"可信的内核对比"变成了一条无人值守的流水线：每个数字背后都是一次干净的开机、固定的环境 stabilizing、可复现的脚本。三套预设 workload 正好覆盖内核 MM 的三大战场，配比设计（工作集超内存比例、WT cache 压缩、IOPS 限速）都值得抄作业。

下一步：装好 docker 跑 mongodb-ycsb 套件补齐文件页场景，然后编译一个打了自己补丁的内核进来，单内核基线就升级成真正的 A/B 对比——那才是这套框架完全体。
