# 深入理解程序地址空间：ASLR、PIE 与虚拟内存

## 前言

在使用 ChampSim 进行 Profiling 时，我们遇到一个关键问题：Pin 工具抓取 trace 中的 PC（程序计数器）地址，能否与 `objdump -d` 反汇编中的地址对应上？

这个问题牵扯出三个核心概念：

- **PIE vs Non-PIE**：程序加载时地址是否固定
- **ASLR**：操作系统是否随机化加载地址
- **虚拟内存**：为什么多个程序可以用同一个地址而不会冲突

本文用可复现的命令行实验，从零开始讲清楚这些概念。

---

## 1. 静态视角：objdump 看到的是什么

任何可执行文件在编译链接时，链接器会给每一条指令分配一个"期望地址"。

```bash
# 编译两个版本：PIE（默认）和 Non-PIE
echo 'int x; int main() { return x; }' | gcc -x c - -o /tmp/demo_pie
echo 'int x; int main() { return x; }' | gcc -no-pie -x c - -o /tmp/demo_nopie

# 查看文件类型
file /tmp/demo_pie
# → ELF 64-bit LSB pie executable
file /tmp/demo_nopie
# → ELF 64-bit LSB executable
```

`readelf -h` 更精确地告诉我们：

```bash
$ readelf -h /tmp/demo_pie | grep Type
  Type:  DYN (Position-Independent Executable file)    ← PIE

$ readelf -h /tmp/demo_nopie | grep Type
  Type:  EXEC (Executable file)                        ← Non-PIE
```

`objdump -d` 查看 main 函数的静态地址：

```bash
$ objdump -d /tmp/demo_pie | grep '<main>:' | head -1
  0000000000001129 <main>:         ← 小地址，从 0 开始

$ objdump -d /tmp/demo_nopie | grep '<main>:' | head -1
  0000000000401106 <main>:         ← 固定地址 0x401106
```

**关键差异**：PIE 程序的 objdump 地址从 `0x1000` 左右开始（接近 0）；Non-PIE 程序固定在 `0x400000` 附近开始。这只是"期望"——运行时不一定是这个地址。

---

## 2. 运行时视角：真实加载地址

写一个小程序，让它自己打印自己 main 函数的运行时地址：

```c
// show_addr.c
#include <stdio.h>
int main(void) {
    printf("运行时 main 的地址: %p\n", (void*)main);
    return 0;
}
```

分别编译为 PIE 和 Non-PIE，各跑 5 次：

```bash
$ gcc -o /tmp/show_pie show_addr.c
$ gcc -no-pie -o /tmp/show_nopie show_addr.c

$ for i in 1 2 3 4 5; do /tmp/show_pie; done
运行时 main 的地址: 0x5b23111ce149
运行时 main 的地址: 0x591465e92149      ← 每次不同!
运行时 main 的地址: 0x6066d4540149
运行时 main 的地址: 0x5691fa362149
运行时 main 的地址: 0x5df41b7d1149

$ for i in 1 2 3 4 5; do /tmp/show_nopie; done
运行时 main 的地址: 0x401136
运行时 main 的地址: 0x401136           ← 永远相同!
运行时 main 的地址: 0x401136
运行时 main 的地址: 0x401136
运行时 main 的地址: 0x401136
```

PIE 程序每次加载到不同的随机地址（`0x5b23...` → `0x5914...` → `0x6066...`），Non-PIE 程序永远在 `0x401136`。

---

## 3. 为什么会这样：PIE 与 ASLR

### 3.1 PIE（Position-Independent Executable）

PIE 是一种编译方式。编译时，所有代码使用**相对寻址**而非绝对地址。这样无论程序被加载到内存中的哪个位置，都能正确运行。

```
Non-PIE:  call 0x401200      ← 写死了目标地址
PIE:      call *%rip+0x1234  ← 相对于当前指令位置计算
```

现代 Linux 发行版的 gcc 默认编译 PIE 程序（安全考虑）。

### 3.2 ASLR（Address Space Layout Randomization）

ASLR 是操作系统内核的特性，控制在加载程序时是否随机化地址。查看当前设置：

```bash
$ cat /proc/sys/kernel/randomize_va_space
2

# 0 = 关闭全部随机化
# 1 = 只随机化共享库和栈
# 2 = 全部随机化（包括 PIE 主程序）
```

**PIE 需要 ASLR 来发挥效果**：PIE 使程序"可以"被加载到任意地址，ASLR 实际"执行"随机化。Non-PIE 程序不受 ASLR 影响——即使 ASLR=2，Non-PIE 程序仍然加载到固定地址。

### 3.3 关系总结

| 编译方式 | objdump 地址 | 运行时地址（ASLR=2） | 运行时地址（ASLR=0） |
|---------|-------------|---------------------|---------------------|
| `gcc`（PIE） | `0x1129` | 每次随机，如 `0x5b...1149` | 固定（但不同于 objdump） |
| `gcc -no-pie` | `0x401136` | **永远** `0x401136` | **永远** `0x401136` |

---

## 4. 高地址 vs 低地址：谁在程序的地址空间里

一个进程的虚拟地址空间里不只有主程序，还有动态链接器（ld.so）、C 库（libc）等共享库，以及栈。它们分布在不同的地址区域。

用 Pin trace 10M 条指令来看 perlbench 程序：

```
前 100K 条指令（程序启动早期）:
  0x79341dxxxxxx  : 100,000 PCs    ← 全部在 ld.so 的动态链接器代码中!

后 100K 条指令（进入 main 之后）:
  0x00xxxxxx      :  86,565 PCs    ← 主程序代码（低地址）
  0x793408xxxxxx  :  13,435 PCs    ← 共享库代码（高地址）
```

为什么前 10M 条指令都在 ld.so 里？因为程序启动时，内核先加载 `ld-linux-x86-64.so.2`（动态链接器），由它负责：

1. 加载依赖的 .so 文件（libc 等）
2. 做符号重定位
3. 最后才跳转到 main

这个启动过程本身就消耗了几百万到上千万条指令。

**通用规律**：

```
0x0000000000400000 - 0x0000000001000000 : 主程序（Non-PIE 固定在这里）
0x55xxxxxxxxxxxxxx - 0x56xxxxxxxxxxxxxx : 主程序（PIE，地址随机）
0x7fxxxxxxxxxxxxxx - 0x7fxxxxxxxxxxxxxx : 共享库（libc, libm, ...）
0x7ffff7fc000000+                        : ld.so（动态链接器）
0x7fffffffxxxx                           : 栈
```

---

## 5. 不冲突的秘密：虚拟内存

Non-PIE 程序都固定在 `0x401000` 开始加载。如果同时跑 3 个，它们的代码段都在同一个地址——不会冲突吗？

### 实验验证

```bash
$ echo 'int main() { usleep(100000); return 0; }' | gcc -no-pie -x c - -o /tmp/demo
$ /tmp/demo & /tmp/demo & /tmp/demo &
$ for pid in $(jobs -p); do
    cat /proc/$pid/maps | grep 'r-xp.*demo' | awk '{print $1}'
  done

  PID 623369: 00401000-00402000
  PID 623370: 00401000-00402000    ← 三个进程，完全相同的代码段地址!
  PID 623371: 00401000-00402000
```

三个进程的代码都在 `0x00401000-0x00402000`，却能同时正确运行。

### 原理：MMU 做翻译

CPU 内部有一个硬件叫 **MMU**（Memory Management Unit），负责将每个进程的**虚拟地址**翻译为**物理地址**。

```
进程 A 执行 0x401106 时:
  0x401106 (虚拟) ── MMU 查页表 ──→ 0x1a3f0106 (物理 RAM)

进程 B 执行 0x401106 时:
  0x401106 (虚拟) ── MMU 查页表 ──→ 0x2b8c0106 (物理 RAM)
                                       ↑
                             不同的物理内存页!
```

每个进程有自己独立的**页表**（Page Table），MMU 为不同进程将相同的虚拟地址映射到不同的物理页。

### 类比

```
小区 A 的 1-101 室     ← 进程 A 的 0x401000
小区 B 的 1-101 室     ← 进程 B 的 0x401000
小区 C 的 1-101 室     ← 进程 C 的 0x401000
```

门牌号一样，但邮递员（MMU）知道它们在城市的完全不同的经纬度。

### 这是怎么做到的

每当 CPU 切换进程时（context switch），操作系统会切换 MMU 使用的页表指针（`CR3` 寄存器）。切换后，同一虚拟地址自动指向新进程的物理内存。

---

## 6. 实际观察工具速查

### 查看程序类型

```bash
file /path/to/binary              # 快速看 pie / non-pie
readelf -h /path/to/binary | grep Type   # 精确看 DYN / EXEC
```

### 查看静态反汇编地址

```bash
objdump -d /path/to/binary | head -20
# 地址 0x40xxxx → Non-PIE，运行时 = 这个地址
# 地址 0x1xxx   → PIE，运行时 ≠ 这个地址
```

### 查看运行时加载地址

```bash
/path/to/binary &
cat /proc/$!/maps | grep 'r-xp' | head -5
# 第一列是实际虚拟地址范围
kill $! 2>/dev/null
```

### 查看 ASLR 状态

```bash
cat /proc/sys/kernel/randomize_va_space
# 0=关闭  1=部分  2=完全
```

### 查看进程完整内存布局

```bash
cat /proc/<PID>/maps
# 每一行: 地址范围 权限 偏移 设备 inode 路径
```

---

## 7. 对 ChampSim Profiling 的意义

在我们的 Pipeline 中：

1. SPEC2006 程序用 `-no-pie` 编译 → Non-PIE 二进制
2. `objdump -d` 的地址 = 运行时地址（Non-PIE 固定加载）
3. Pin tracer 抓到的 PC 可以直接与 objdump 反汇编匹配
4. SimPoints 是**基于指令序号**的（第 N 亿条指令），与 PC 地址无关

实际验证结果（10M 指令 trace，perlbench）：

```
从 trace 中提取的 Load PC: 714 个
与 objdump 反汇编匹配:     714 个
匹配率:                    100%
```

Pipeline 的 PC 匹配问题得到彻底解决。

---

## 总结

| 概念 | 一句话解释 |
|------|-----------|
| PIE | 代码使用相对寻址，可以被加载到任意位置 |
| Non-PIE | 代码使用绝对地址，固定在 0x400000 加载 |
| ASLR | 内核在加载时随机化地址（PIE 生效的前提） |
| objdump 地址 | 链接器分配的"期望"地址 |
| 运行时地址 | 加 `-no-pie` 则与 objdump 一致，否则被 ASLR 随机化 |
| 高地址 / 低地址 | 低地址 = 主程序，高地址（0x7f...）= 共享库和 ld.so |
| 虚拟内存 | 每个进程有独立的地址空间，同地址不冲突 |
| MMU | CPU 硬件，每时每刻把虚拟地址翻译为物理地址 |
