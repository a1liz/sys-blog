# liz-sys-blog

> Sys 技术细节知识库 —— 记录系统底层、运维、性能相关的技术深度文章。

## 目录结构

```
posts/
├── linux/             # 内核、系统调用、内存管理、进程调度
├── networking/        # TCP/IP、DNS、路由、防火墙
├── storage/           # 文件系统、磁盘、LVM、RAID
├── containers/        # Docker、K8s、namespace、cgroup
├── security/          # TLS、认证、PAM、SELinux、seccomp
├── performance/       # perf、eBPF、tracing、profiling、调优
├── tools/             # CLI 工具深度解析
└── troubleshooting/   # 排障实录、故障分析
assets/                # 图片、架构图
snippets/              # 代码/配置片段
drafts/                # 草稿
templates/             # 文章模板
```

## 文章索引

### Linux
- [给 Linux 内核做基准测试：Kernel Multi-Bench 的 Workload 图鉴与实战](posts/linux/kmb-workload-overview.md) — reboot 驱动的内核基准流水线；三大 MM 战场的 workload 解析与 8C/15G VM 适配实测

### Networking
（暂无文章）

### Storage
- [一块 KV Cache 的旅程:DGX Spark 上 GPU → pinned → SSD 的数据路径全取证](posts/storage/dgx-spark-kv-offload-data-path.md) — pagemap/FIEMAP/torch.profiler 逐跳取证 KV offload 数据路径;GPU↔DRAM 55 GiB/s,O_DIRECT 把 pinned↔NVMe 提升 3–13 倍

### Containers
（暂无文章）

### Security
（暂无文章）

### Performance
- [为什么你的程序跑得慢？——用 perf + Pin ILP 给 SPEC CPU 2017 做一次「全身体检」](posts/performance/spec-bottleneck-analysis.md) — SPEC 20 个 benchmark 的瓶颈分类：DEPENDENCY / MEMORY / FRONTEND

### Tools
（暂无文章）

### Troubleshooting
（暂无文章）

## 最近更新

- 2026-08-21: [给 Linux 内核做基准测试：Kernel Multi-Bench 的 Workload 图鉴与实战](posts/linux/kmb-workload-overview.md)
- 2026-07-19: [一块 KV Cache 的旅程:DGX Spark 上 GPU → pinned → SSD 的数据路径全取证](posts/storage/dgx-spark-kv-offload-data-path.md)
- 2026-06-29: [为什么你的程序跑得慢？——用 perf + Pin ILP 给 SPEC CPU 2017 做一次「全身体检」](posts/performance/spec-bottleneck-analysis.md)

## 使用方式

```bash
# 新建文章
cp templates/post-template.md posts/<category>/<slug>.md

# 本地预览（后续可接入 Hugo / mdBook 等）
# hugo serve 或 mdbook serve

# 直接预览 HTML 版（docs/html/）
python3 -m http.server 8080 -d docs/html/
# 从本地机器 SSH 隧道访问
ssh -L 8080:127.0.0.1:8080 -N user@<server>
```
