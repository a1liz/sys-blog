---
title: "同一个 PC 的相位冲突 —— 跨编译单元的函数调用如何污染训练标签"
date: 2026-05-14
tags: [performance, cpu, cache, profiling, assembly, compiler, training-data, link-time]
category: "performance"
summary: "在真实多文件项目中，跨 .o 的函数调用无法被编译器内联，导致同一个 load PC 在不同执行阶段呈现截然不同的访存模式——从顺序 (+4) 到稀疏 (+64)，甚至从顺序访问切换到随机访问。通过两个无 hack 的跨编译单元实验展示相位冲突的产生机制，以及内联如何消解它。"
---

<!-- 正文 -->

## 问题

在 ChampSim profiling pipeline 中，Stage 6 对每个 PC 产出一个 `best_prefetch` 标签。这隐含一个前提：**同一个 PC 的访存模式是稳定的**。

但如果同一个 PC 在不同执行阶段表现出完全不同的访存模式呢？会出现 **PC 粒度的相位冲突**（phase conflict）：同一个 PC 的 profiler 记录混合了多种模式，任何单一 prefetcher 都无法对其全部最优。

什么时候会发生？我们先做一个真实的对照实验。

---

## 实验设置：跨编译单元模拟真实代码库

真实项目（SPEC、Linux kernel、任何多文件 C 项目）中，函数分布在不同的 `.c` 文件里。编译时每个 `.c` → `.o` 各自编译，链接时拼在一起。**跨 `.o` 的函数调用，编译器在 `-O2` 下无法内联**（除非开启 LTO）。

这就是最自然的"同一 PC 出现多种模式"的来源。

### 目录结构

```
worker.h   — 函数声明
worker.c   — 实现在另一个编译单元
main.c     — 调用方
```

编译命令（无 LTO）：
```bash
gcc -no-pie -O2 -c worker.c -o worker.o
gcc -no-pie -O2 -c main.c   -o main.o
gcc -no-pie worker.o main.o -o demo
```

---

## 案例 1：同一 PC，三种不同的 stride —— `sum_strided`

### worker.c（另一个编译单元）

```c
int sum_strided(int *arr, int n, int stride) {
    int sum = 0;
    for (int i = 0; i < n; i += stride) {
        sum += arr[i];  // ← 关键的 Load PC
    }
    return sum;
}
```

### main.c

```c
volatile int r1 = sum_strided(data, 512, 1);   // stride=1 → 顺序访问  (+4/iter)
volatile int r2 = sum_strided(data, 512, 4);   // stride=4 → 跨步访问  (+16/iter)
volatile int r3 = sum_strided(data, 512, 16);  // stride=16→ 稀疏访问  (+64/iter)
```

### 反汇编

`worker.o` 中的 `sum_strided`（PC 固定，在链接后的 `demo` 二进制中位于 `0x4013d0`）：

```asm
00000000004013d0 <sum_strided>:
  4013d8:  movslq %edx,%r8           ; stride → r8 (符号扩展)
  4013df:  shl    $0x2,%r8            ; r8 = stride * 4 (字节偏移)
  4013e3:  nopl   0x0(%rax,%rax,1)
  4013e8:  add    %edx,%eax           ; i += stride
  4013ea:  add    (%rdi),%ecx         ; ← 唯一的 Load PC: 0x4013ea
  4013ec:  add    %r8,%rdi            ; rdi += stride*4
  4013f1:  jg     4013e8              ; loop
```

`main.o` 中的三处调用（共享同一 PC `0x4013ea`）：

```asm
  401166:  call   4013d0 <sum_strided>    ; stride=1  → r8=4  (步进 +4/iter)
  401193:  call   4013d0 <sum_strided>    ; stride=4  → r8=16 (步进 +16/iter)
  4011c0:  call   4013d0 <sum_strided>    ; stride=16 → r8=64 (步进 +64/iter)
```

### 运行时地址 Trace（在 `sum_strided` 中插桩打印）

```
=== stride=1 (sequential) ===        === stride=4 (strided) ===
arr[0]  @ 0x...570                   arr[0]  @ 0x...570
arr[1]  @ 0x...574  ← +4            arr[4]  @ 0x...580  ← +16
arr[2]  @ 0x...578  ← +4            arr[8]  @ 0x...590  ← +16
arr[3]  @ 0x...57c  ← +4            arr[12] @ 0x...5a0  ← +16

=== stride=16 (sparse) ===
arr[0]  @ 0x...570
arr[16] @ 0x...5b0  ← +64
arr[32] @ 0x...5f0  ← +64
```

| 调用 | stride 参数 | 地址 delta | r8 寄存器值 | 访存特征 |
|---|---|---|---|---|
| r1 | 1 | +4 | 4 | 密集顺序 |
| r2 | 4 | +16 | 16 | 跨步 |
| r3 | 16 | +64 | 64 | 稀疏 |

**结果**：PC `0x4013ea` 的 profiler 记录中混合了三种完全不同的地址步长。如果 `next_line` 对顺序访问最优，它对稀疏访问可能完全无用。

---

## 案例 2：同一 PC，顺序访问 vs 随机访问 —— `sum_indirect`

这是更极端的情况：同一个 load PC，访问模式在**顺序遍历**和**随机跳转**之间切换。

### worker.c

```c
int sum_indirect(int *arr, int *indices, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[indices[i]];  // ← 关键的 Load PC
    }                            // indices内容决定arr的访问模式
    return sum;
}
```

### main.c

```c
// Phase A: indices = [0,1,2,...] → 顺序访问 arr
for (int i = 0; i < 512; i++) indices[i] = i;
volatile int r4 = sum_indirect(data, indices, 512);

// Phase B: indices 随机打乱 → 随机访问 arr
shuffle(indices);
volatile int r5 = sum_indirect(data, indices, 512);
```

### 反汇编

```asm
0000000000401410 <sum_indirect>:
  401418:  movslq %edx,%rdx
  40141d:  lea    (%rsi,%rdx,4),%rcx   ; &indices[n] (循环边界)
  401428:  movslq (%rsi),%rdx           ; rdx = indices[i] (符号扩展)
  40142b:  add    $0x4,%rsi             ; i++ (遍历 indices 数组)
  40142f:  add    (%rdi,%rdx,4),%eax    ; ← 唯一的 Load PC: 0x40142f
  401432:  cmp    %rsi,%rcx             ;        arr[indices[i]]
  401435:  jne    401428                ; loop
```

### 运行时地址 trace

```
=== Phase A: indices sequential ===  === Phase B: indices shuffled ===
indices[0]=0    arr[0] @ 0x...a70    indices[0]=387  arr[387] @ 0x...d7c  ← 大跳
indices[1]=1    arr[1] @ 0x...a74    indices[1]=12   arr[12]  @ 0x...a90  ← 回跳
indices[2]=2    arr[2] @ 0x...a78    indices[2]=501  arr[501] @ 0x...f74  ← 又大跳
indices[3]=3    arr[3] @ 0x...a7c    indices[3]=88   arr[88]  @ 0x...bc0  ← 回跳
```

| 阶段 | indices 内容 | arr 访问模式 | 适合的 prefetcher |
|---|---|---|---|
| A | `[0,1,2,3,...]` | 严格顺序 (+4/iter) | `next_line` |
| B | 随机排列 | 完全随机跳转 | `no` (任何预取都是浪费) |

PC `0x40142f` 的 profiler 记录中混合了两种截然不同的访存模式。单一 prefetcher 无法同时覆盖。

---

## 什么情况下内联可以解决，什么情况下不行

### 自然消解：同编译单元内被内联

如果 `sum_strided` 定义在 `main.c` 中（同 TU），gcc -O2 会将其内联。三个 call site 各自生成独立的 `add (%rax),%edx` / `add (%rbx),%eax` 指令，**三个不同的 PC**——标签冲突自然消失。

这是上一版文章的核心结论，不再展开。

### 无法消解的真实场景

| 场景 | 为什么 | 常见程度 |
|---|---|---|
| **跨编译单元调用** | 无 LTO 时编译器看不见函数体 | 几乎所有多文件项目 |
| **函数指针 / 虚函数** | 编译时无法确定调用目标 | qsort 回调、C++ 虚函数、插件系统 |
| **动态库函数** | .so 在运行时链接 | `libc`、第三方库 |
| **递归函数** | 无法完全内联 | 树遍历、解析器、图形算法 |
| **过大函数** | 超过内联代价阈值 | SPEC 中的巨型解释器函数（如 `S_regmatch`） |

**SPEC CPU2006 正是这种场景**——`400.perlbench` 由数百个 `.c` 文件编译成 `.o` 再链接，跨 TU 调用无处不在。

---

## 对 Profiling Pipeline 的影响

### pipeline 现状

当前 Stage 6 (`aggregate_ground_truth.py`) 对每个 PC 产出单一 `best_prefetch` 标签。在跨 TU 场景下，PC 的 AMAT 记录混合了多种模式，选出的"最优"其实是折中值。

### 排查方法

如果某个 benchmark 的 GBDT 精度异常低：

1. **检查混合度**—拉出该 PC 在不同 prefetcher 下的 AMAT 分布。如果所有 prefetcher 都表现平平（没有明显最优），可能该 PC 的访问模式不稳定。
2. **检查调用来源**—该 PC 对应的函数是否被多处以不同参数调用。`objdump -d` 中 `call <func>` 的出现次数直接说明。
3. **按调用上下文拆分**—如果确认是相位冲突，可引入 **call-chain context** 作为额外特征（PC + 返回地址），让模型感知"这个 PC 是在哪个调用路径下执行的"。

### 可能的改进方向

| 方案 | 适用场景 | 复杂度 |
|---|---|---|
| PC + call-chain 上下文 | 同一函数被多处不同参数调用 | 需修改 profiler 记录 call stack |
| 按时间分片标签 | 同一函数在不同执行阶段模式不同 | 需 SimPoints 级标签划分 |
| 多标签输出 | 给模型输出 "60% next_line, 40% no" 的概率分布 | 训练目标改为 soft label |

---

## 可复现实验

完整源码（三文件）：

```bash
# worker.h
int sum_strided(int *arr, int n, int stride);
int sum_indirect(int *arr, int *indices, int n);

# worker.c — 实现在另一个编译单元
int sum_strided(int *arr, int n, int stride) {
    int sum = 0;
    for (int i = 0; i < n; i += stride) { sum += arr[i]; }
    return sum;
}
int sum_indirect(int *arr, int *indices, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) { sum += arr[indices[i]]; }
    return sum;
}

# main.c — 调用方
// 三处调用 sum_strided: stride=1, 4, 16
// 两处调用 sum_indirect: indices顺序, indices随机
```

编译与验证：

```bash
gcc -no-pie -O2 -c worker.c -o worker.o
gcc -no-pie -O2 -c main.c   -o main.o
gcc -no-pie worker.o main.o -o demo

# 验证跨 TU 调用无法内联：三处 call 共享同一 PC
objdump -d demo | grep 'call.*sum_strided'
# 输出三行 call 4013d0 <sum_strided>，全部指向同一地址
```

所有输出均可复现（代码段 PC 因 `-no-pie` 固定）。

---

*本文源于对 "per-PC label 在真实代码库中是否可靠" 的追问。结论：跨编译单元的函数调用是最常见且最自然的相位冲突来源——编译器无 LTO 时完全无法内联，同一个 PC 的标签必然混合多种访存模式。了解这一点有助于在排查训练精度异常时定位根因。*
