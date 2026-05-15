# nginx-flowlens

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Nginx](https://img.shields.io/badge/nginx-1.24%2B-green.svg)](https://nginx.org/)

[中文](README.zh-CN.md)

> **Think `tcpdump` for HTTP, built directly into nginx.**
>
> A zero-dependency, native C module that captures full HTTP request/response pairs (headers + body) at the gateway layer. No Lua VM, no sidecar, no external runtime. Default output is **TLV binary** (~9% overhead, production-ready); JSON is available for debugging.

```
Client ──► Nginx ──► Backend
              │
              ▼
        inspect.log  (TLV binary by default, JSON optional)
```

---

## Use Cases

| Scenario | Why nginx-flowlens |
|----------|-------------------|
| **Compliance audit** | Full request/response capture with raw body — meet gateway audit requirements (MLPS 2.0, PCI-DSS, financial compliance) without introducing a runtime dependency |
| **API observability** | Know exactly what your backend sent and received. No sampling, no truncation — every API call with complete body, logged at the gateway |
| **Debugging & forensics** | Replay exact HTTP conversations. When a bug only reproduces in production, the audit log has the full picture |

---

## Quick Start

```bash
# 5 seconds to your first audit log:
./run-dev.sh install && curl -s -X POST http://localhost:19099/ -d '{"test":true}' && python3 tools/tlv2json.py -i .nginx-dev/logs/inspect.log
```

```bash
# One command: compile + configure + start (no root, isolated under .nginx-dev/)
./run-dev.sh install

# Convert TLV log to readable JSON
python3 tools/tlv2json.py -i .nginx-dev/logs/inspect.log
```

```bash
./run-dev.sh start|stop|restart|status
```

**Want JSON output?** Edit `examples/nginx.dev.conf`, change `inspect_format tlv` to `inspect_format json`, then `./run-dev.sh install -f`.

**Add to your own nginx:**

```bash
./scripts/build.sh               # compile, binary at nginx/objs/nginx
./configure --add-module=/path/to/nginx-flowlens/ngx_flowlens_module ...
make -j$(nproc) && sudo make install
```

Example output:

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

## Features

**What's captured — uniformly, regardless of transfer encoding:**

- Static files, proxy responses, chunked transfer, SSE streams — all captured with the same structured schema
- Raw body before gzip/brotli (`top_body_filter` guarantees uncompressed content, even when the client receives compressed)
- HTTP/2 (h2c) support
- Subrequest filtering — audits main requests only, skips internal subrequests (auth_request, SSI, etc.)

**Smart body dumping — conditional, not all-or-nothing:**

- Content-type whitelist: `text/*`, `application/xml`, MS Office formats, `application/pdf`
- Size threshold: only bodies ≥ `inspect_dump_min_size` are offloaded
- Below threshold, the body stays inline in the log record

**Output & control:**

- Dual format: **TLV binary** (fast, default) or **JSON** (human-readable)
- Per-location `inspect on/off`
- Configurable body size cap (`inspect_max_body_size` per request/response independently)

---

## Performance

> macOS 15.4, Apple Silicon, wrk 4.2.0 (`-t4 -c100 -d20s`)

| Config | Small file (~600B) | Large file (1MB) |
|--------|-------------------|-------------------|
| baseline (stock nginx) | 32,441 RPS / 3.13ms | 6,985 RPS / 14.9ms |
| **inspect on (tlv)** | **29,389 RPS / 3.47ms** | **413 RPS / 240ms** |
| inspect on (json) | 2,811 RPS / 35.5ms | 95 RPS / 1.02s |

Default TLV: **~9% overhead** for small requests — production-ready. JSON for debugging only.

Large-file TLV overhead comes from body buffering (1MB per request under `-c100`); in practice, `inspect_max_body_size` and `inspect_dump` mitigate this.

```bash
cd benchmark && ./benchmark.sh
```

---

## Why a native C module?

Full HTTP audit logging can be done with OpenResty/Lua scripting. Here's why a compiled C module is different:

| Concern | OpenResty/Lua approach | nginx-flowlens |
|---------|----------------------|----------------|
| **Runtime dependency** | LuaJIT + resty libs | None — single nginx binary |
| **Compliance** | Dynamic execution = audit risk | Static C module, source-reviewable |
| **gzip/br body capture** | Depends on `gunzip on` + filter order | Always raw body (`top_body_filter`) |
| **Performance** | Lua GC, string immutability | Direct chain buffer, zero-copy where possible |
| **Subrequest** | Lua manual bookkeeping | `r != r->main` naturally filtered |

For environments where adding a language runtime to the gateway is not an option.

---

## Output Format

### JSON (`inspect_format json`)

One JSON object per line:

| Field | Type | Description |
|-------|------|-------------|
| `timestamp` | string | ISO8601 UTC with milliseconds |
| `server_name` | string | `Host` / `:authority` header |
| `client_ip` | string | Client connection IP |
| `request.method` | string | HTTP method |
| `request.uri` | string | Request URI path |
| `request.args` | string | Query string |
| `request.headers` | object | Key-value header map |
| `request.body` | string | Base64-encoded body (absent when dumped) |
| `request.dump_path` | string | Dump file path (replaces `body` when dumped) |
| `request.body_truncated` | boolean | Exceeded `inspect_max_body_size` |
| `request.body_len` | number | Captured body length (bytes) |
| `response.*` | — | Same structure as `request.*` |

### TLV (`inspect_format tlv`, default)

Compact binary format (~10× faster serialization than JSON).

```
[Magic 4B "INSP"][Version 1B 0x02][RecLen 4B BE][Type 1B][Len 4B BE][Value] … [End 0xFF]
```

Convert for analysis:

```bash
python3 tools/tlv2json.py -i inspect.log -o inspect.jsonl
python3 tools/tlv2json.py -i inspect.log -o - | jq '.request.method'
```

### Notes

- `inspect_max_body_size` caps both request and response bodies independently
- Body dump whitelist: `text/*`, `application/xml`, MS Office formats, `application/pdf`
- Dump files: `{req|resp}_{seq}_{YYYYMMDD_HHMMSS}_{filename}` under `inspect_dump_path`
- `inspect off` completely bypasses the module (~1% overhead)

---

## Configuration

```nginx
http {
    inspect on;
    inspect_log /var/log/nginx/inspect.log;
    inspect_format tlv;           # tlv (default, fast) or json

    # Optional: offload large bodies to files
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

| Directive | Context | Default | Description |
|-----------|---------|---------|-------------|
| `inspect` | http, server, location | `off` | Enable/disable audit |
| `inspect_log` | http, server, location | — | Log file path |
| `inspect_max_body_size` | http, server, location | `5m` | Max body size per request |
| `inspect_format` | http, server, location | `tlv` | `tlv` or `json` |
| `inspect_dump` | http, server, location | `off` | Dump large bodies to files |
| `inspect_dump_path` | http, server, location | — | Dump directory |
| `inspect_dump_min_size` | http, server, location | `1k` | Min body size to trigger dump |
| `inspect_buffer_size` | http, server, location | — | Buffered write buffer |
| `inspect_flush_time` | http, server, location | — | Buffered write flush interval |
| `inspect_log_per_worker` | http, server, location | `off` | Per-worker log files |

---

## Architecture

```
NGX_HTTP_ACCESS_PHASE  → capture request headers + read body
top_header_filter      → capture response headers
top_body_filter        → accumulate response body (before gzip/brotli)
log handler            → serialize to TLV or JSON, write via cycle->open_files
```

- **Access phase**: `ngx_http_read_client_request_body()` — synchronous for small bodies, async callback for large
- **Top body filter**: registered before compression, guarantees raw body capture
- **Per-request context**: `ngx_http_flowlens_ctx_t` on `r->pool`, body capped by `inspect_max_body_size`

---

## Testing

```bash
cd test && ./test.sh
```

20 tests covering: static files, proxy, chunked transfer, SSE, gzip raw-body, HTTP/2, body dump, TLV/JSON dual format.

---

## Roadmap

**P1 — Production:**
fd caching, async batch output, Kafka, sampling rate, conditional audit (`inspect_if $status >= 400`)

**P2 — Detection:**
Pluggable detection engine (C ABI, `dlopen`), built-in rules (phone, ID, email, AK/SK), Rust detection plugin

---

## Contributing

```bash
./scripts/build.sh          # compile
./scripts/build.sh -s       # static build
cd test && ./test.sh        # functional tests
cd benchmark && ./benchmark.sh
```

---

## License

Apache 2.0
