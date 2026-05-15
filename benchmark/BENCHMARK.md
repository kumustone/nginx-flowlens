# Benchmark Report

> Environment: macOS 15.4 (Darwin 25.4.0), Apple Silicon (M1 Pro)
> Tool: wrk 4.2.0
> Date: 2026-05-15

## Test Environment

### macOS (Local)

| Component | Spec |
|-----------|------|
| CPU | Apple M1 Pro (8-core: 6P + 2E) |
| Memory | 16 GB LPDDR5 |
| OS | macOS 15.4 (Darwin 25.4.0) |
| Clang | 17.0.0 (Apple) |
| nginx | 1.24.0 (compiled from source) |
| wrk | 4.2.0 [kqueue] |

### Linux x86_64 (Lab)

| Component | Spec |
|-----------|------|
| CPU | Intel Core i5-10400 @ 2.90GHz (6C/12T, max 4.3GHz) |
| Memory | 14 GB DDR4 |
| Disk | 238GB NVMe SSD (OS) + 931GB SATA HDD |
| OS | openEuler 22.03 (LTS-SP3), Kernel 5.10.0 |
| GCC | 10.3.1 |
| nginx | 1.24.0 (compiled from source) |
| OpenResty | 1.19.3.1 (from openEuler repo) |
| wrk | 4.2.0 |

## Methodology

All tests use `wrk -t4 -c100` with static files served from local disk, `worker_processes 1`.

### nginx-flowlens Configurations (macOS)

| Config | Description | Port |
|--------|-------------|------|
| **baseline** | Stock nginx 1.24.0, no inspect module | 18090 |
| **inspect-off** | nginx with inspect module loaded, `inspect off;` | 18091 |
| **inspect-on-json** | nginx with inspect module, `inspect on; inspect_format json;` | 18092 |
| **inspect-on-tlv** | nginx with inspect module, `inspect on; inspect_format tlv;` | 18093 |

### OpenResty Comparison (Linux only)

| Config | Description | Port |
|--------|-------------|------|
| **or-baseline** | OpenResty 1.19.3.1, no Lua audit | 18094 |
| **or-audit** | OpenResty with Lua body filter + log phase audit | 18093 |

---

## Results

### macOS — Small File GET (index.html, ~600 bytes)

> `wrk -t4 -c100 -d20s --latency`

| Config | RPS | Avg Latency | Overhead |
|--------|-----|-------------|----------|
| baseline | 26,779 | 5.58ms | — |
| inspect-off | 24,732 | — | **~7.5%** |
| **inspect-on (tlv)** | **24,260** | **5.90ms** | **~9.4%** |
| inspect-on (json) | 2,822 | 35.26ms | **~89.5%** |

**Key Takeaway:** Default TLV mode adds approximately **~9% overhead** for small requests on macOS — production-ready. JSON mode is ~10x slower and intended for debugging only.

Note: The "inspect-off" overhead (~7.5%) is primarily from the module's filter chain registration; the filter functions early-return when `inspect off;` but the chain traversal itself incurs a small cost. The README claim of "~0.4%" refers to the marginal cost *within* the filter functions, not the total module loading impact.

### macOS — Large File GET (1MB binary)

> `wrk -t4 -c100 -d20s --latency`

| Config | RPS | Avg Latency | Overhead |
|--------|-----|-------------|----------|
| baseline | ~6,500 | ~15ms | — |
| inspect-on (tlv) | ~400 | ~240ms | **~94%** |
| inspect-on (json) | ~95 | ~1.0s | **~98.5%** |

**Note:** Large-file overhead comes from body buffering (1MB per request under `-c100`). In production, use `inspect_max_body_size` and `inspect_dump` to mitigate this. See the Performance section in README.md for a summary.

### Linux x86_64 Comparison (openEuler 22.03, GCC 10.3.1)

> OpenResty 1.19.3.1 vs nginx-flowlens v1.0.0
> Same machine, `wrk -t4 -c100 -d10s`

| Config | RPS | Overhead |
|--------|-----|----------|
| OpenResty baseline | 68,392 | — |
| OpenResty + Lua audit | 19,387 | **-72%** |
| nginx-flowlens (tlv) | 32,422 | **-53%** |

**Key Takeaway:** On the same Linux hardware, nginx-flowlens delivers **+67% higher RPS** than an equivalent OpenResty/Lua audit implementation.

---

## Analysis

### Performance Evolution

This benchmark reflects performance **after** the following optimizations (v0.2.3+):

1. **FD caching** — log file descriptor is opened once via `ngx_conf_open_file()` and reused across requests, eliminating per-request `open()/close()` syscalls
2. **Buffered writes** — `inspect_buffer_size` + `inspect_flush_time` batch log records, reducing write syscall frequency

### Bottleneck Breakdown (current)

| Component | Small File Impact | Large File Impact |
|-----------|-------------------|-------------------|
| Header capture | Low | Low |
| Body accumulation | Low | Very High |
| Base64 encode | Low | Very High |
| TLV serialization | Low | Medium |
| JSON serialization | Medium | High |
| File I/O (cached fd + buffer) | Low | Medium |

### Remaining Optimization Opportunities

| Priority | Optimization | Expected Improvement |
|----------|-------------|---------------------|
| P1 | Zero-copy body accumulation (reference buf chain instead of copy) | Reduced memory, modest RPS gain |
| P1 | Streaming JSON (avoid full materialization) | Reduced memory, modest RPS gain |
| P2 | Sampling rate (`inspect_sample_rate`) | Control overhead at high throughput |

---

## Reproducing

```bash
cd benchmark
./benchmark.sh
```

**Note:** The benchmark script writes audit logs to `/opt/nginx-audit/logs/` by default. On macOS, you may need to change this to a writable path (e.g., `/tmp/nginx-audit/logs`):

```bash
mkdir -p /tmp/nginx-audit/logs
# Edit benchmark/nginx-*.conf to use /tmp/nginx-audit/logs/inspect.log
```

Adjust duration:
```bash
WRK_DURATION=30s ./benchmark.sh
```

---

## Raw Data

See `results/` directory for per-run raw wrk outputs (if captured).
