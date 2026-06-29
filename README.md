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
（暂无文章）

### Networking
（暂无文章）

### Storage
（暂无文章）

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

- 2026-06-29: [为什么你的程序跑得慢？——用 perf + Pin ILP 给 SPEC CPU 2017 做一次「全身体检」](posts/performance/spec-bottleneck-analysis.md)

## 使用方式

```bash
# 新建文章
cp templates/post-template.md posts/<category>/<slug>.md

# 本地预览（后续可接入 Hugo / mdBook 等）
# hugo serve 或 mdbook serve
```
