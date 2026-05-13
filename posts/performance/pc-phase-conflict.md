---
title: "同一个 PC 的相位冲突 —— 完整分类与可复现案例"
date: 2026-05-14
tags: [performance, cpu, cache, profiling, assembly, compiler, training-data]
category: "performance"
summary: "同一 load PC 在不同执行阶段可能呈现截然不同的访存模式。分成两大类：编译器因素（跨文件编译、函数指针、超大函数等 5 种）和语义因素（参数变化、间接索引、多阶段算法等 4 种）。通过跨编译单元和 qsort 函数指针两个实验证明，给出排查策略。"
---

<!-- 正文 -->

## 先解释几个缩写

后面会反复出现，先说清楚：

**TU（Translation Unit / 编译单元）**：一个 `.c` 文件经过预处理器展开后的产物。`gcc -c foo.c` 编译一个 TU，产出 `foo.o`。大多数项目由几十上百个 TU 组成，编译时每个 TU 各自独立处理。

**LTO（Link-Time Optimization / 链接时优化）**：把优化推迟到链接阶段。不加 LTO 时，`gcc -c a.c` 完全看不到 `b.c` 里的函数体。加 `-flto` 后，链接器拿到所有 `.o` 的中间表示，可以跨文件内联。

**PGO（Profile-Guided Optimization / 剖面引导优化）**：先跑一遍程序收集热点数据，再基于这份数据重编译。可以帮编译器判断"这个函数指针实际指向谁"或"这个函数值不值得内联"，从而做出更优决策。

**PLT（Procedure Linkage Table / 过程链接表）**：动态库（`.so`）的跳板。程序调用 `memcpy` 时，先跳到 PLT 里的一条 stub，stub 再通过 GOT 跳到真正的 `memcpy` 实现。调用方的 `call` 指令指向的是 PLT stub 的固定 PC。

---

## 问题

在 ChampSim profiling pipeline 中，每条 load 指令由一个 PC 标识。Stage 6 对每个 PC 产出一个 `best_prefetch` 标签，隐含假设：**同一 PC 的访存模式是稳定的**。

但同一个 PC 完全可能在不同时间表现出不同的访存模式。以下是所有可能性的完整分类。

---

## 分类总览

| 类别 A：编译器因素 | 类别 B：语义因素 |
|---|---|
| 多个调用上下文 → 一个 PC | 函数自身就是多模的 |
| **A1** 跨编译单元调用 ✅ LTO | **B1** 参数随时间变化 ❌ |
| **A2** 函数指针 / 虚函数 ⚠️ PGO | **B2** 间接索引访问 ❌ |
| **A3** 动态库 (.so) 函数 ✅ 静态链接 | **B3** 多阶段算法 ❌ |
| **A4** 函数体过大，内联拒绝 ⚠️ LTO+PGO | **B4** 交替数据结构 ⚠️ loop fission |
| **A5** 递归函数 ⚠️ 尾递归可消 | |

> ✅ = 编译器优化可消解 &nbsp;&nbsp; ⚠️ = 部分可消解 &nbsp;&nbsp; ❌ = 不可消解

---

## 类别 A：编译器因素——多个调用上下文被迫共享一个 PC

这类冲突的本质是：程序员写了多个不同的调用，每个调用本应有各自的访存模式，但编译器**做不到**或**选择不**为它们生成独立的副本，导致它们被迫共享同一个 PC。

---

### A1：跨编译单元调用（无 LTO）

**本质**：函数体在另一个 `.c` 文件里。不加 LTO 时，编译器在处理调用方这个 TU 的时候，根本看不见被调函数的函数体，只能生成一个 `call` 指令指向外部符号。所有调用方共享同一个 PC。

**实验：worker.c + main.c 分文件编译**

源码目录：
```
worker.h   — 函数声明
worker.c   — 实现在另一个编译单元
main.c     — 调用方
```

`worker.c`（另一个编译单元）：
```c
int sum_strided(int *arr, int n, int stride) {
    int sum = 0;
    for (int i = 0; i < n; i += stride) {
        sum += arr[i];  // ← 关键的 Load PC
    }
    return sum;
}
```

`main.c`：
```c
volatile int r1 = sum_strided(data, 512, 1);   // stride=1 → 顺序  (+4/iter)
volatile int r2 = sum_strided(data, 512, 4);   // stride=4 → 跨步  (+16/iter)
volatile int r3 = sum_strided(data, 512, 16);  // stride=16→ 稀疏  (+64/iter)
```

编译命令（无 LTO）：
```bash
gcc -no-pie -O2 -c worker.c -o worker.o
gcc -no-pie -O2 -c main.c   -o main.o
gcc -no-pie worker.o main.o -o demo
```

反汇编 `main.o` 中的调用方：
```asm
  401166:  call   4013d0 <sum_strided>    ; stride=1  → 步进 +4/iter
  401193:  call   4013d0 <sum_strided>    ; stride=4  → 步进 +16/iter
  4011c0:  call   4013d0 <sum_strided>    ; stride=16 → 步进 +64/iter
```

三处 `call 4013d0` 全部指向同一地址。`sum_strided` 内部的 load 指令在 `0x4013ea`：

```asm
00000000004013d0 <sum_strided>:
  4013d8:  movslq %edx,%r8           ; stride → r8
  4013df:  shl    $0x2,%r8            ; r8 = stride * 4（字节偏移）
  4013e8:  add    %edx,%eax           ; i += stride
  4013ea:  add    (%rdi),%ecx         ; ← 唯一的 Load PC：0x4013ea
  4013ec:  add    %r8,%rdi            ; rdi += stride*4
  4013f1:  jg     4013e8              ; loop
```

运行时地址 trace：

```
stride=1 (顺序)           stride=4 (跨步)           stride=16 (稀疏)
arr[0]  @ ...570         arr[0]  @ ...570         arr[0]  @ ...570
arr[1]  @ ...574  +4     arr[4]  @ ...580  +16    arr[16] @ ...5b0  +64
arr[2]  @ ...578  +4     arr[8]  @ ...590  +16    arr[32] @ ...5f0  +64
arr[3]  @ ...57c  +4     arr[12] @ ...5a0  +16    arr[48] @ ...630  +64
```

**后果**：PC `0x4013ea` 在 profiler 中混合了三种 stride 的 AMAT 记录。如果 `next_line` 对 +4 最优，它对 +64 几乎完全无用。选出的 `best_prefetch` 是对三种模式的折中值。

**能否消解**：✅ 加 `-flto`，或把函数体移到同一个 `.c` 文件。

---

### A2：函数指针 / 虚函数

**本质**：`call *%rax` 跳到哪里去，编译器静态分析推不出来。即使函数体在同一个 TU 内、编译器完全看得到，也不知道该内联谁。

**实验：qsort 调用 cmp_func**

`cmp_func` 的地址被传给 C 标准库的 `qsort`。`qsort` 内部通过函数指针调用它——这个间接调用发生在 libc 的 `.so` 里，编译时不可能被消解。

```c
int cmp_func(const void *a, const void *b) {
    int va = *(const int *)a;  // ← LOAD PC #1：a 指向哪里，由 qsort 的分区算法决定
    int vb = *(const int *)b;  // ← LOAD PC #2：b 同样跳来跳去
    return (va > vb) - (va < vb);
}
```

```c
qsort(data, 512, sizeof(int), cmp_func);
```

反汇编 `cmp_func`：
```asm
0000000000401320 <cmp_func>:
  401324:  mov    (%rsi),%eax        ; ← Load PC #1：b 的值
  401326:  cmp    %eax,(%rdi)        ; ← Load PC #2：a 的值（cmp 隐含 load）
  40132e:  movzbl %dl,%edx
  401334:  sub    %edx,%eax
  401336:  ret
```

PC `0x401324` 和 `0x401326` 在 qsort 执行期间会被调用几万次，`a` 和 `b` 的地址由 qsort 内部的快排分区算法决定——一会儿指向数组左端（比较 pivot），一会儿指向右端（分区交换），一会儿递归进入子数组。访存模式完全随机。

**能否消解**：⚠️ PGO 可以告诉编译器"这个函数指针 99% 指向 cmp_func"，然后做 speculative devirtualization（生成 `if (fp == cmp_func) call cmp_func else call *fp`）。但这是概率性的，不保证。

---

### A3：动态库函数

**本质**：`.so` 在运行时加载，编译时链接的只是一个 PLT stub。程序里所有 `call memcpy@plt` 都指向同一个 stub PC。

**能否消解**：✅ 静态链接（`.a` 替代 `.so`），或利用 IFUNC resolver 在加载时选择特定实现。

### A4：函数体过大

**本质**：即使函数体在同 TU 内、gcc 完全看得到，如果函数太大（几千行，多层循环），内联后的代码膨胀远超收益，gcc 会拒绝内联。SPEC 中的巨型函数如 `S_regmatch`（perlbench 的核心正则匹配循环）就是典型。

**能否消解**：⚠️ LTO + PGO 可以让编译器更准确地估算热度，裁掉冷路径后内联热路径。

### A5：递归函数

**本质**：理论上不可能完全展开为有限层。同一个 PC 在不同递归深度访问不同的树节点/链表节点。

**能否消解**：⚠️ 尾递归可以被优化为迭代（消除递归），一般递归无法完全拆 PC。

---

## 类别 B：语义因素——函数自身逻辑天然是多模式的

这类冲突的本质是：**不是谁的调用、怎么编译的问题，而是函数本身的语义就决定了同一个 PC 会在不同时间做不同的事**。

---

### B1：同一 call site，参数随时间变化

**本质**：和前一类不同——这里只有**一个** call site（汇编里一条 `call` 指令），没有多个调用方。但参数值（比如 `stride`）在不同时间传入的值不同，导致同一 PC 的访存模式随时间漂移。

```c
// 只有一个 call site，但 stride 来自一个变化的数据源
int strides[] = {1, 4, 16};
for (int phase = 0; phase < 3; phase++) {
    sum_strided(data, 512, strides[phase]);  // 同一个 call，不同参数
}
```

汇编里就一条 `call sum_strided`，编译器不可能为 `stride=1`、`stride=4`、`stride=16` 各生成一份 `sum_strided` 的副本——参数值在运行时才确定，静态分析推不出来。

更常见的真实场景：一个图像处理函数，`kernel_size` 由用户输入或配置文件指定。同一个高斯模糊函数，跑 `kernel=3` 时访存模式是密集邻域，跑 `kernel=15` 时是稀疏大跨度。

**能否消解**：❌ 编译器不能为每种可能的参数值生成一个函数副本。

---

### B2：间接索引访问（data-dependent index）

**本质**：访问地址 = `base + indices[i] * sizeof(T)`。`indices` 的内容是运行时数据，编译器完全无法预测。

```c
int sum_indirect(int *arr, int *indices, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[indices[i]];  // ← 唯一的 Load PC
    }                            // indices 顺序 → 顺序访问 arr
    return sum;                  // indices 随机 → 随机访问 arr
}
```

反汇编：
```asm
0000000000401410 <sum_indirect>:
  401428:  movslq (%rsi),%rdx           ; rdx = indices[i]
  40142b:  add    $0x4,%rsi             ; i++
  40142f:  add    (%rdi,%rdx,4),%eax    ; ← 唯一的 Load PC：0x40142f
  401432:  cmp    %rsi,%rcx             ;     arr[indices[i]]
  401435:  jne    401428                ; loop
```

运行时：

```
Phase A: indices = [0, 1, 2, 3, ...]
indices[0]=0    arr[0] @ ...a70
indices[1]=1    arr[1] @ ...a74   ← +4（顺序）
indices[2]=2    arr[2] @ ...a78   ← +4

Phase B: indices = [387, 12, 501, 88, ...]（随机打乱）
indices[0]=387  arr[387] @ ...d7c
indices[1]=12   arr[12]  @ ...a90   ← 大跳回
indices[2]=501  arr[501] @ ...f74   ← 又大跳
```

同一个 PC `0x40142f`，Phase A 是完美顺序（适合 `next_line`），Phase B 是完全随机（什么都不做最好）。profiler 记录会混合两者，单一标签无法覆盖。

这种模式在真实代码中极其常见：稀疏矩阵的列索引遍历、哈希表的 bucket chain、图的邻接表——只要是"通过索引数组间接访问另一个数组"的模式，都属于此类。

**能否消解**：❌ 编译器无法预测运行时数据的内容。

---

### B3：多阶段算法

**本质**：同一个循环，但程序逻辑在不同阶段处理不同性质的数据。

典型的如 LZ 系列解压算法：literal copy 阶段是顺序访问，back-reference copy 阶段是从已输出数据中按 offset 随机跳转。两个阶段运行在同一个循环体、同一个 load PC 上。

**能否消解**：❌ 一个循环就是同一个 PC，不可能按阶段拆分。只能靠 profiling 的时间分片标签或 soft label。

### B4：交替数据结构

**本质**：同一个循环体交替访问两个不同数组，访问模式在两者间快速切换。

**能否消解**：⚠️ loop fission（把一个循环拆成两个）可以分离，但这会破坏数据局部性，属于手动权衡。

---

## 两类对比

| | 类别 A（编译器因素） | 类别 B（语义因素） |
|---|---|---|
| 问题来源 | 多个调用方被迫共享同一个 PC | 函数自身逻辑就包含多种模式 |
| 调用方数量 | 多个（或通过指针间接调用） | 可以只有一个 |
| 能否被编译优化消解 | 部分可以（LTO、PGO、静态链接） | 大部分不能 |
| 应对策略 | LTO、PGO、合并源文件、手动内联 | soft label、时间分片、call-chain 特征 |

---

## 对 Profiling Pipeline 的影响

当前 Stage 6 对每个 PC 产出单一 `best_prefetch` 标签。在上述任何场景下，这个标签都可能是多种模式的折中。

**排查步骤**（当某 benchmark 的 GBDT 精度异常低时）：

1. 拉出该 PC 在各 prefetcher 下的 AMAT 分布。如果所有 prefetcher 表现平平（没有明显胜者），说明模式不稳定。
2. `objdump -d binary | grep 'call.*<func>'` 看该函数是否有多个 call site。
3. 看该函数内部是否用了间接索引（`arr[indices[i]]`）、或在不同阶段切换行为。
4. 如果是类别 A → 尝试开 LTO 重编译。如果是类别 B → 考虑在训练数据中引入 call-chain 上下文或 soft label。

---

## 可复现实验

完整源码在 `snippets/` 下：

```bash
# A1: 跨编译单元 —— 3 种 stride 共享 1 个 PC
gcc -no-pie -O2 -c worker.c -o worker.o
gcc -no-pie -O2 -c main.c   -o main.o
gcc -no-pie worker.o main.o -o demo
objdump -d demo | grep 'call.*sum_strided'   # 三处全部指向同一 PC

# A2: 函数指针 —— qsort 回调 cmp_func
gcc -no-pie -O2 -g qsort_demo.c -o qsort_demo
objdump -d qsort_demo | grep -A8 '<cmp_func>' # 两条 load PC
# cmp_func 的 load PC 在 qsort 内部被通过指针重复调用几万次
```

---

*本文源于对 "per-PC label 在真实代码库中是否可靠" 的系统性追问。PC 相位冲突可归为两大类 9 种情况：编译器因素（5 种，部分可消解）和语义因素（4 种，基本不可消解）。了解每种情况的机制和边界，是排查训练精度异常的基础。*
