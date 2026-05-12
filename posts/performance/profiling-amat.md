---
title: "AMAT 如何揭开 CPU 预取的秘密——从 ChampSim Profiling 到 Prefetcher 选择"
date: 2026-05-13
tags: [performance, cpu, cache, profiling, champ-sim]
category: "performance"
summary: "通过 ChampSim 对 SPEC CPU2006 400.perlbench 的逐指令级 profiling，用 AMAT（Average Memory Access Time）在 14 种 prefetcher 配置中选出每个 Load PC 的最优策略。附带真实汇编级案例：为什么 next_line 赢了 46% 的 AMAT，而 ip_stride 在某些指针链上比 no 还差。"
---

<!-- 正文 -->

## 问题：同一个程序，不同 PC 需要不同的预取策略

现代 CPU 的硬件 prefetcher 就像餐厅的服务员——提前把你可能要的菜端上来。但访存模式千变万化：

- **顺序扫描数组** → 下一个 cache line 就是你马上要的
- **链表/指针跳转** → 下一个地址和当前地址毫无关系
- **固定步长遍历结构体** → 每隔 N 个字节访问一次

只用一种 prefetcher 打天下，注定在大部分场景下是错的。

那怎么办？**按 Load PC 分别选最优 prefetcher**。问题是：怎么知道哪个 PC 适合哪个 prefetcher？

答案就是 AMAT。

---

## AMAT 是什么

**AMAT（Average Memory Access Time）** 就是一条 load 指令每次访问内存的平均延迟。

```
AMAT = (命中次数 × HIT_LATENCY + 未命中次数 × FILL_LATENCY) / 总访问次数
```

| 结果 | 延迟 |
|------|------|
| Cache Hit | `HIT_LATENCY`（几个 cycle） |
| Cache Miss | `FILL_LATENCY`（几十到上百 cycle） |

Prefetcher 的价值就是**把 Miss 变成 Hit**。

同一条 trace 用不同的 prefetcher 各跑一遍，每个 prefetcher 对同一个 PC 产生不同的 hit/miss 比例 → 不同的 AMAT。**AMAT 最低的那个，就是这个 PC 的最佳 prefetcher。**

---

## Profiling 是怎么做的

ChampSim 是一款 cycle-level 缓存模拟器。我们在编译时加 `-DHINT_PROFILING`，在缓存访问路径的关键点插入 profiler。

每一次 load 指令通过 `CACHE::try_hit()` 时：

```
太    Tag 查找
│   ┌──────────┐
│   │ Hit/Miss │──→ HIT_LATENCY / FILL_LATENCY
│   └──────────┘
│         │
│   ┌─────▼──────┐
│   │ Profiler   │  record_access(PC, hit, latency)
│   │ 累加统计    │  → access_count++, hit_count++/miss_count++
│   └────────────┘      total_latency += latency
│         │
│   ┌─────▼──────┐
│   │ Prefetcher │  预测下一步，提前搬数据
│   │ (旁路)     │
│   └────────────┘
│         │
▼     flush() → avg_amat = total_latency / access_count
```

程序跑完 100M 条指令后，profiler 把每个 PC 的统计 flush 成 JSONL：

```json
{"pc":"0x463148","access_count":127,"hit_count":98,"miss_count":29,
 "total_latency":50592,"avg_amat":398.4,
 "hit_ratio":0.772,"prefetch_accuracy":0.68,...}
```

---

## 真实案例：400.perlbench 的 4202 个 PC × 14 种 Prefetcher

我们对 SPEC CPU2006 的 `400.perlbench`（Perl 解释器）进行了完整 profiling。

**总览**：
- 4,202 个 Load PC 有 ≥2 种 prefetcher 的测量数据
- 覆盖 6 种 prefetcher 家族：`no`, `next_line`, `ip_stride`, `va_ampm_lite`, `ampm`, `stride`
- 产生 1,572 条训练样本（有汇编上下文的 PC）

**全局最优分布**：

| Prefetcher | PC 数 | 占比 | 说明 |
|---|---|---|---|
| `va_ampm_lite` | 2,439 | 58% | 复杂访存模式（全局历史+地址关联） |
| `no` | 767 | 18% | 不需要预取（随机访存或已在缓存） |
| `next_line` | 498 | 12% | 顺序/连续访存 |
| `ip_stride` | 209 | 5% | 固定步长访存 |
| `ampm` | 202 | 5% | 全局访存流模式 |
| `stride` | 87 | 2% | 基于 PC 的步长 |

---

## 案例 1：`next_line` 大胜 46% — 结构体字段的连续访问

```
Perl_pp_match @ 0x463148
────────────────────────────────────────────
  0x46313c: mov    %rax,0x78(%rsp)
  0x463141: xor    %eax,%eax
  0x463143: movzbl 0x24(%r8),%eax     ← 访问字段 +0x24
  >>>       mov    0x50(%r8),%r14     ← 访问字段 +0x50  (LOAD)
  0x46314c: mov    %eax,%edx
  0x46314e: and    $0x3,%edx
```

这是 `Perl_pp_match` 函数内部，`r8` 指向匹配引擎的状态结构体。两条指令分别访问 `+0x24` 和 `+0x50`，**偏移量递增 0x2C**，属于同一结构体内部的相邻字段。

```
AMAT 对比（cycle）
─────────────────────────────────
next_line:1  ████████████ 398.4  ← 最优
no:1         █████████████████ 580.4  (+46%)
ip_stride:2  ██████████████████ 600.3
ip_stride:1  ████████████████████ 673.4
stride:1     ████████████████████ 673.4
```

### 为什么会这样？

第一次 miss 发生时，`next_line` 把紧接着的下一行预取进来，后面的 `+0x50` 访问直接命中。而 `no` 每次 miss 都要等完整的 fill latency。差距高达 **182 个 cycle（46%）**。

---

## 案例 2：`next_line` 再胜 41% — 正则引擎的状态遍历

```
S_regmatch @ 0x4898cb
────────────────────────────────────────────
  0x4898bf: mov    %r14,%rcx
  0x4898c2: mov    %eax,0x18(%rsp)
  0x4898c6: jmp    488378 <S_regmatch+0x118>
  >>>       cmp    0x9753e(%rip),%r14   ← 对比当前输入位置与重新编译的边界
  0x4898d2: jb     488395 <S_regmatch+0x135>
  0x4898d8: jmp    488375 <S_regmatch+0x115>
```

这是 Perl 正则引擎的核心匹配循环 `S_regmatch`。`rip` 相对寻址访问全局状态变量 `PL_regeol`（正则引擎结束边界），每次迭代顺序访问。

```
AMAT 对比（cycle）
─────────────────────────────────
next_line:1  ████████████ 375.0  ← 最优
ampm:1       █████████████████ 527.0  (+41%)
ampm:2       █████████████████ 527.0
va_ampm_lite █████████████████ 527.0
ip_stride:2  █████████████████ 527.0
```

### 为什么会这样？

正则引擎的主循环是**高度可预测的顺序流**。`next_line` 的最简策略（每次都预取下一行）完美匹配，而更复杂的 `ampm` 和 `va_ampm_lite` 反而因为过度预取或错误预测浪费了带宽。

---

## 案例 3：`ip_stride` 胜出 — 方法调用的不连续步长

```
Perl_pp_method_named @ 0x4689ec
────────────────────────────────────────────
  0x4689e1: mov    0xb7db0(%rip),%rax   ← 加载 PL_op
  0x4689e8: mov    0x28(%rax),%rbp      ← op→op_next (偏移 +0x28)
  >>>       mov    0x0(%rbp),%rax       ← op_next→op_type (偏移 +0x00)
  0x4689f0: mov    0x18(%rax),%rax      ← op_type→op_ppaddr (偏移 +0x18)
  0x4689f4: mov    %eax,0x4(%rsp)
```

这是 Perl 的 `pp_method_named` —— 方法分发函数。访存模式是典型的**指针链间接跳转**：
1. 从全局表加载 `PL_op`
2. 偏移 `+0x28` 拿到 `op_next`
3. 回到偏移 `+0x00` 拿 `op_type`
4. 又跳到 `+0x18` 拿 `op_ppaddr`

**偏移量不断变化**：`+0x28` → `+0x00` → `+0x18` —— 不是顺序的。

```
AMAT 对比（cycle）
─────────────────────────────────
ip_stride:2  ████████████ 411.7  ← 最优
no:1         █████████████████ 468.0  (+14%)
ip_stride:1  █████████████████ 469.1
stride:1     █████████████████ 469.1
next_line:1  ██████████████████ 486.8
```

### 为什么会这样？

`ip_stride` 在 PC 级别记录了**上次这个 load 的地址差**。这个 PC 重复执行时（Perl 方法调用是高频操作），ip_stride 学习到固定步长模式，准确预取。而 `next_line` 不仅没用，反而**比 `no` 还差**——它预取了错误的行，占用了缓存带宽，拖累了真正需要的数据。

---

## 案例 4：`no` 反而最优 — 随机访存不值得预取

```
Perl_sv_catsv @ 0x4a1288
────────────────────────────────────────────
  >>>       mov    0x8(%rbx),%rdx      ← 随机指针解引用
  0x4a128c: test   %rdx,%rdx
  0x4a128f: je     4a12a0
```

```
AMAT 对比（cycle）
─────────────────────────────────
no:1         ████████████ 375.0  ← 最优
next_line:1  █████████████████ 437.5
ip_stride:1  ██████████████████ 450.0
stride:1     ██████████████████ 450.0
```

当访存模式随机时，任何预取都是浪费。`no` 不做任何事，反而 AMAT 最低。这告诉我们：**有时候什么都不做就是最好的策略。**

---

## AMAT 如何指导机器学习模型

最终的 `tuning_dataset.jsonl` 就是训练数据：

```json
{
  "instruction": "Given the following x86-64 assembly context for a load instruction
in function 'Perl_pp_match', predict the optimal prefetch policy.

Instructions before load:
  0x46313c: mov    %rax,0x78(%rsp)
  0x463141: xor    %eax,%eax
  0x463143: movzbl 0x24(%r8),%eax

Load instruction:
  mov    0x50(%r8),%r14

Instructions after load:
  0x46314c: mov    %eax,%edx
  0x46314e: and    $0x3,%edx
  ...",
  "label": "next_line:1",
  "pc": "0x463148"
}
```

模型学习到：看到 `r8+0x24` 然后 `r8+0x50` 这种相邻偏移的指令序列 → 选 `next_line`。看到指针链 `rip→offset→offset→offset` → 选 `ip_stride`。

---

## 核心洞察

**Per-PC 的访存模式高度稳定**——编译后的机器码在同一输入下总是走同样的路径。这意味着静态度量（汇编上下文）和动态行为（AMAT）之间的映射是可学习的。

| 原理 | 意义 |
|---|---|
| AMAT 是控制变量实验 | 同一 trace×不同 prefetcher → AMAT 差异纯粹来自 hit/miss 比例 |
| Prefetcher 是近似函数 | `f(PC_history, global_pattern) → address`——每个 prefetcher 的近似能力不同 |
| 最小 AMAT = 最优近似 | 对特定 PC 的访存模式，AMAT 最低的那个 prefetcher 提供了最好的近似 |

---

*本文数据来自 ChampSim profiling pipeline 对 SPEC CPU2006 400.perlbench 的实测。Pipeline 脚本位于 `ChampSim/tools/profiling/`，使用 Intel PIN 3.22 采集 trace，14 个 ChampSim 二进制分别模拟不同 prefetcher，全程可复现。*
