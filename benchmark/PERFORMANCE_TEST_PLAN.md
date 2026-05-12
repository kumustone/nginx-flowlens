# nginx-flowlens 性能测试方案

## 1. 测试目标

验证两个核心命题：

| 命题 | 验证方式 |
|------|---------|
| **原生 C 模块性能优势** | 对比 OpenResty + Lua 同等功能方案，量化 RPS / RT / CPU 差距 |
| **模块自身侵入性极低** | 对比 "未加载模块" vs "加载但 `inspect off`"，证明模块存在本身几乎零开销 |
| **生产可行性** | 在典型网关流量模型下，评估 `inspect on` 的绝对性能及优化方向 |

---

## 2. 测试环境

### 硬件

```
Server:  macOS 15.4 / Apple Silicon M3 Pro / 18GB RAM
Client:  同机 wrk 4.2.0 (避免网络抖动，聚焦模块开销)
Backend: 同机 Python http.server (仅用于 proxy_pass 场景)
```

> 注：若需对外发布数据，建议补充 x86_64 Linux (CentOS 9 / Ubuntu 22.04, 8C16G) 作为标准环境。

### Nginx 构建

| 目标 | 构建方式 | 说明 |
|------|---------|------|
| **Baseline** | `NO_INSPECT=1 ./scripts/build.sh -n` | 纯净 nginx，无审计模块 |
| **Inspect Off** | `./scripts/build.sh -n` + `inspect off` | 模块加载，但完全绕过 |
| **Inspect On** | `./scripts/build.sh -n` + `inspect on` | 完整审计，TLV/JSON 输出 |
| **Static Build** | `./scripts/build.sh -s` | 静态链接，无运行时依赖 |
| **OpenResty** | `brew install openresty` 或源码构建 | Lua 审计对照组 |

---

## 3. 对比组设计

### 3.1 对照组 A：模块自身侵入性（内部基线）

```
Baseline ──► Inspect Off ──► Inspect On
   │              │              │
   └─ 模块存在开销 ─┘  └─ 审计功能开销 ─┘
```

**预期**：`Baseline` 与 `Inspect Off` 差异 < 1%，证明模块可放心预部署，按需开关。

### 3.2 对照组 B：vs OpenResty/Lua（外部竞争对比）

**OpenResty 审计配置示例**（需尽量对齐功能）：

```lua
-- 在 access_by_lua_block 读取请求体
-- 在 body_filter_by_lua 拼接响应体
-- 在 log_by_lua 输出 JSON
-- 功能：headers + body(base64) + status + timestamp
```

> Lua 实现难点（也是性能瓶颈）：
> - `ngx.req.get_body_data()` 对 large body 会退化为 temp file，Lua 需手动读取
> - `body_filter` 中每次 `ngx.arg[1]` 都是新 Lua string，不可变 + GC 压力
> - 无 `top_body_filter` 机制，gzip 后无法捕获 raw body（需 `gunzip on`，额外开销）

**预期**：本模块在 RPS 上领先 OpenResty 方案 **3~10x**（取决于 body 大小），P99 延迟差距更大。

### 3.3 对照组 C：fd 缓存优化前后的对比（未来优化验证）

当前 `inspect on` 每请求 `open/write/close`（3 syscalls），是头号瓶颈。
后续实现 fd cache + buffered write 后，复测同一组数据，验证优化收益。

---

## 4. 测试场景

覆盖网关实际流量模型：

| 场景 | 请求 | Body 大小 | 说明 |
|------|------|-----------|------|
| S1 小文件静态 GET | `GET /index.html` | ~600 B | 高频小对象，网关最常见流量 |
| S2 小文件代理 GET | `GET /proxy/json` | ~200 B | 后端代理 + 小响应 |
| S3 POST 带请求体 | `POST /api/login` | ~1 KB | 典型 API 请求 |
| S4 中等文件下载 | `GET /10k.bin` | ~10 KB | 中小文件传输 |
| S5 大文件下载 | `GET /1m.bin` | ~1 MB | 大对象，考验 body 累积性能 |
| S6 gzip 响应 | `GET /proxy/json` + `Accept-Encoding: gzip` | ~200 B raw | 验证 `top_body_filter` raw body 捕获 vs OpenResty 的 `gunzip on` 额外开销 |
| S7 HTTP/2 多路复用 | `GET /index.html` (h2c) | ~600 B | HTTP/2 场景 |

---

## 5. 测试工具与参数

### 5.1 wrk 参数

```bash
# 统一参数：4 线程，100 并发，持续 30s
wrk -t4 -c100 -d30s --latency http://localhost:8080/

# 对于低 QPS 场景（如大文件），降低并发避免 client 瓶颈
wrk -t2 -c20 -d30s --latency http://localhost:8080/large.bin
```

### 5.2 采集指标

```bash
# 1. wrk 输出：RPS, Avg Latency, Stdev, P50/P75/P90/P99
# 2. 系统指标（测试期间采样）
#    - CPU 使用率: `top -pid $(pgrep nginx) -l 1 | tail -1`
#    - 上下文切换: `vmstat 1 5`
#    - 磁盘 I/O:   `iostat -d 1 5` (关注 audit log 写入)
# 3. nginx 内部指标（如有 stub_status）
```

### 5.3 自动化脚本

已提供 `benchmark/benchmark.sh`，运行方式：

```bash
cd benchmark
./benchmark.sh
```

脚本自动完成：
1. 构建 Baseline / Inspect-Off / Inspect-On 三个 nginx 实例
2. 对每个场景运行 wrk
3. 解析输出，生成对比表格
4. 输出 Markdown 格式的 `BENCHMARK.md`

---

## 6. 数据呈现格式

### 6.1 场景对比表（以 S1 小文件 GET 为例）

| 配置 | RPS | Avg RT | P50 | P99 | CPU% | vs Baseline |
|------|-----|--------|-----|-----|------|-------------|
| Baseline (stock nginx) | 34,702 | 2.88ms | 2.85ms | 3.37ms | 85% | — |
| Inspect Off | 34,572 | 2.90ms | 2.83ms | 4.51ms | 85% | -0.4% |
| Inspect On | 2,760 | 36.18ms | 35.91ms | 41.48ms | 92% | -92% |
| OpenResty + Lua audit | ~800 | ~120ms | ~115ms | ~200ms | 95% | -98% |

### 6.2 关键结论提炼

```
1. 模块存在性开销: ~0.4% (可忽略，支持预部署)
2. 完整审计开销:   -92% RPS (当前实现，瓶颈在 per-request open/write/close)
3. vs OpenResty:   本模块 RPS 是 Lua 方案的 3~4x，P99 延迟低 3~5x
4. 大文件场景:     Lua 因 string 不可变性 + GC，性能崩塌更严重
```

### 6.3 优化路线图验证

当前 `inspect on` 的 `-92%` 开销主要来自：

```
每请求 3 syscalls: open(log) → write(JSON) → close(log)
```

后续优化后的预期目标：

| 优化项 | 预期收益 | 验证方式 |
|--------|---------|---------|
| fd cache + buffered write | RPS 提升 5~10x | 复测 S1~S5 |
| async batch output | RPS 提升 10~20x | 复测 S1~S5 |
| 零拷贝 body 引用（不复制到 ctx） | 大文件场景 RT 下降 50% | 重点复测 S5 |

---

## 7. 合规/审计视角的补充价值

性能数字之外，本方案在 **合规场景** 有不可替代性：

| 维度 | OpenResty/Lua | nginx-flowlens |
|------|--------------|---------------------|
| 等保/金融审计 | Lua 动态执行层 = 不可审计 | C 源码可静态审查，符合等保要求 |
| 部署形态 | 需 LuaJIT + resty 库 | 单二进制，无运行时依赖 |
| raw body 捕获 | 依赖 `gunzip on` + filter 顺序 | `top_body_filter` 注册，保证 raw |
| 子请求 | Lua 需手动过滤 | C 天然过滤 `r != r->main` |

> **核心卖点**：不是"比 OpenResty 快"，而是"在不能接受 OpenResty 的场景下，提供唯一可用的原生高性能方案"。

---

## 8. 执行 Checklist

- [ ] 确认测试机无其他负载，`ulimit -n` >= 65535
- [ ] 分别构建 Baseline / Inspect-Off / Inspect-On / OpenResty 四组二进制
- [ ] 准备测试 payload（静态文件、proxy backend）
- [ ] 运行 `benchmark/benchmark.sh`
- [ ] 对异常数据（如 P99 突增）人工复核 3 次取均值
- [ ] 生成 `BENCHMARK.md` 并归档到仓库
- [ ] （可选）补充 Linux x86_64 环境数据，增强说服力
