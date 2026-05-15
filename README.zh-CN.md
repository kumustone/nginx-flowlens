# nginx-flowlens

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Nginx](https://img.shields.io/badge/nginx-1.24%2B-green.svg)](https://nginx.org/)

[English](README.md)

> **可以理解为 nginx 内置的 HTTP 层"tcpdump"。**
>
> 零依赖、原生 Nginx C 模块，在网关层捕获完整的 HTTP 请求/响应对（请求头 + 请求体），输出结构化审计日志。无需 Lua VM、Sidecar、外部运行时。默认输出 **TLV 二进制格式**（约 9% 性能开销，生产就绪）；JSON 可选用于调试。

```
Client ──► Nginx ──► Backend
              │
              ▼
        inspect.log  （默认 TLV 二进制，JSON 可选）
```

---

## 使用场景

| 场景 | 为什么用 nginx-flowlens |
|------|------------------------|
| **合规审计** | 完整捕获请求/响应，包括原始 body —— 满足网关审计要求（等保 2.0、PCI-DSS、金融合规），不引入额外运行时 |
| **API 可观测性** | 准确知道后端发送和接收了什么。不采样、不截断 —— 每个 API 调用的完整 body 都在网关层记录 |
| **调试与取证** | 回放完整的 HTTP 对话。当 bug 只在线上复现时，审计日志里有全貌 |

---

## 快速开始

```bash
# 5 秒看到第一条审计日志：
./run-dev.sh install && curl -s -X POST http://localhost:19099/ -d '{"test":true}' && python3 tools/tlv2json.py -i .nginx-dev/logs/inspect.log
```

```bash
# 一条命令：编译 + 配置 + 启动（无需 root，隔离在 .nginx-dev/ 下）
./run-dev.sh install

# 将 TLV 日志转为可读 JSON
python3 tools/tlv2json.py -i .nginx-dev/logs/inspect.log
```

```bash
./run-dev.sh start|stop|restart|status
```

**想直接输出 JSON？** 编辑 `examples/nginx.dev.conf`，将 `inspect_format tlv` 改为 `inspect_format json`，然后 `./run-dev.sh install -f`。

**集成到已有 nginx：**

```bash
./scripts/build.sh               # 仅编译，二进制文件在 nginx/objs/nginx
./configure --add-module=/path/to/nginx-flowlens/ngx_flowlens_module ...
make -j$(nproc) && sudo make install
```

示例输出：

```json
{
  "timestamp": "2026-04-21T09:40:59.628Z",
  "server_name": "localhost",
  "client_ip": "127.0.0.1",
  "request": {
    "method": "POST", "uri": "/", "args": "",
    "headers": {"Content-Type": "application/json", "Host": "localhost:19099"},
    "body": "eyJ1c2VyIjoiYWxpY2UiLCJhY3Rpb24iOiJsb2dpbiJ9",
    "body_truncated": false, "body_len": 42
  },
  "response": {
    "status": 200,
    "headers": {"Content-Type": "text/html"},
    "body": "...",
    "body_truncated": false, "body_len": 615
  }
}
```

---

## 功能特性

**捕获内容 —— 无论何种传输编码，统一结构化记录：**

- 静态文件、代理响应、chunked 分块传输、SSE 流 —— 全部以同一结构化 schema 捕获
- gzip/brotli 压缩前的原始 body（`top_body_filter` 保证捕获未压缩内容，即使客户端收到的是压缩后数据）
- HTTP/2（h2c）支持
- 子请求过滤 —— 仅审计主请求，跳过内部子请求（auth_request、SSI 等）

**智能 body 落盘 —— 按条件触发，非全量 dump：**

- Content-Type 白名单：`text/*`、`application/xml`、MS Office 格式、`application/pdf`
- 大小阈值：仅 ≥ `inspect_dump_min_size` 的 body 才落盘
- 低于阈值时，body 内联在日志记录中

**输出与控制：**

- 双格式：**TLV 二进制**（高性能，默认）或 **JSON**（人可读）
- 按 location 开关审计（`inspect on/off`）
- 请求/响应体大小上限可配（`inspect_max_body_size`，请求和响应独立限制）

---

## 性能

> macOS 15.4, Apple Silicon, wrk 4.2.0（`-t4 -c100 -d20s`）

| 配置 | 小文件（~600B） | 大文件（1MB） |
|------|----------------|--------------|
| 基线（原生 nginx） | 32,441 RPS / 3.13ms | 6,985 RPS / 14.9ms |
| **inspect on（tlv）** | **29,389 RPS / 3.47ms** | **413 RPS / 240ms** |
| inspect on（json） | 2,811 RPS / 35.5ms | 95 RPS / 1.02s |

默认 TLV 格式小请求场景约 **9% 性能开销** —— 生产可用。JSON 仅建议调试用。

大文件 TLV 开销来自 body 缓冲（`-c100` 下每请求 1MB）；实际使用中可通过 `inspect_max_body_size` 和 `inspect_dump` 缓解。

```bash
cd benchmark && ./benchmark.sh
```

---

## 为什么选择原生 C 模块？

用 OpenResty/Lua 脚本也能实现 HTTP 审计日志。编译型 C 模块的区别在于：

| 关注点 | OpenResty/Lua 方案 | nginx-flowlens |
|--------|-------------------|----------------|
| **运行时依赖** | LuaJIT + resty 库 | 无 —— 单个 nginx 二进制文件 |
| **合规性** | 动态执行层 = 审计风险 | 静态 C 模块，可源码审计 |
| **gzip/br 响应体捕获** | 依赖 `gunzip on` + 过滤链顺序 | 始终捕获原始响应体（`top_body_filter` 保证） |
| **性能开销** | Lua GC、字符串不可变 | 直接操作 chain buffer，尽可能零拷贝 |
| **子请求处理** | Lua 需手动维护状态 | `r != r->main` 天然过滤 |

适用于无法接受向网关引入语言运行时的受监管环境。

---

## 输出格式

### JSON（`inspect_format json`）

每请求输出一行 JSON：

| 字段 | 类型 | 说明 |
|------|------|------|
| `timestamp` | string | ISO8601 UTC，含毫秒 |
| `server_name` | string | `Host` / `:authority` 头值 |
| `client_ip` | string | 客户端连接 IP |
| `request.method` | string | HTTP 方法 |
| `request.uri` | string | 请求 URI 路径 |
| `request.args` | string | 查询字符串 |
| `request.headers` | object | 键值对请求头 |
| `request.body` | string | Base64 编码的请求体（落盘时无此字段） |
| `request.dump_path` | string | 落盘文件路径（替换 `body`） |
| `request.body_truncated` | boolean | 超过 `inspect_max_body_size` 时为 `true` |
| `request.body_len` | number | 捕获的请求体长度（字节） |
| `response.*` | — | 结构同上 |

### TLV（`inspect_format tlv`，默认）

紧凑二进制格式，序列化速度约为 JSON 的 10 倍。

```
[Magic 4B "INSP"][Version 1B 0x02][RecLen 4B BE][Type 1B][Len 4B BE][Value] … [End 0xFF]
```

转换为 JSON：

```bash
python3 tools/tlv2json.py -i inspect.log -o inspect.jsonl
python3 tools/tlv2json.py -i inspect.log -o - | jq '.request.method'
```

### 注意事项

- `inspect_max_body_size` 分别限制请求体和响应体
- 落盘白名单：`text/*`、`application/xml`、MS Office 格式、`application/pdf`
- 落盘文件：`{req|resp}_{seq}_{YYYYMMDD_HHMMSS}_{filename}` 存于 `inspect_dump_path`
- `inspect off` 完全绕过模块（~1% 开销）

---

## 配置

```nginx
http {
    inspect on;
    inspect_log /var/log/nginx/inspect.log;
    inspect_format tlv;           # tlv（默认，高性能）或 json

    # 可选：大请求体落盘
    inspect_dump on;
    inspect_dump_path /var/log/nginx/dumps;
    inspect_dump_min_size 1k;

    server {
        listen 80;
        location / {
            proxy_pass http://backend;
        }
        location /static/ { inspect off; }
        location /health  { inspect off; }
    }
}
```

| 指令 | 上下文 | 默认值 | 说明 |
|------|--------|--------|------|
| `inspect` | http, server, location | `off` | 启用/禁用审计 |
| `inspect_log` | http, server, location | — | 审计日志文件路径 |
| `inspect_max_body_size` | http, server, location | `5m` | 单请求捕获最大请求体 |
| `inspect_format` | http, server, location | `tlv` | `tlv` 或 `json` |
| `inspect_dump` | http, server, location | `off` | 大请求体落盘 |
| `inspect_dump_path` | http, server, location | — | 落盘目录 |
| `inspect_dump_min_size` | http, server, location | `1k` | 触发落盘的最小请求体大小 |
| `inspect_buffer_size` | http, server, location | — | 缓冲写入缓冲区大小 |
| `inspect_flush_time` | http, server, location | — | 缓冲写入刷新间隔 |
| `inspect_log_per_worker` | http, server, location | `off` | 每 worker 独占日志文件 |

---

## 架构设计

```
NGX_HTTP_ACCESS_PHASE  → 捕获请求头 + 读取请求体
top_header_filter      → 捕获响应头
top_body_filter        → 累积响应体（在 gzip/brotli 之前）
日志处理器              → 序列化为 TLV 或 JSON，通过 cycle->open_files 写入
```

- **Access phase**：`ngx_http_read_client_request_body()` —— 小请求体同步、大请求体异步回调
- **Top body filter**：注册在压缩过滤器之前，保证捕获原始响应体
- **每请求上下文**：`ngx_http_flowlens_ctx_t` 分配在 `r->pool`，受 `inspect_max_body_size` 限制

---

## 测试

```bash
cd test && ./test.sh
```

20 个测试覆盖：静态文件、代理后端、chunked 传输、SSE 流、gzip 原始响应体、HTTP/2、请求体落盘、TLV/JSON 双格式。

---

## 路线图

**P1 — 生产就绪：**
fd 缓存、异步批量输出、Kafka、采样率控制、条件审计（`inspect_if $status >= 400`）

**P2 — 检测平台：**
可插拔检测引擎（C ABI、`dlopen`）、内置规则（手机号、身份证、邮箱、AK/SK）、Rust 检测插件

---

## 参与贡献

```bash
./scripts/build.sh          # 编译
./scripts/build.sh -s       # 静态编译
cd test && ./test.sh        # 功能测试
cd benchmark && ./benchmark.sh
```

---

## 开源协议

Apache 2.0
