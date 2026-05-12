# Benchmark Report

> Environment: macOS 15.4 (Darwin 25.4.0), Apple Silicon (M1/M2/M3)
> Tool: wrk 4.2.0
> Date: 2026-04-27

## Test Environment

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

### macOS (Local)

| Component | Spec |
|-----------|------|
| CPU | Apple M1 Pro (8-core: 6P + 2E) |
| Memory | 16 GB LPDDR5 |
| OS | macOS 26.4.1 (Darwin 25.4.0) |
| Clang | 17.0.0 (Apple) |
| nginx | 1.24.0 (compiled from source) |
| wrk | 4.2.0 [kqueue] |

## Methodology

### nginx-flowlens Configurations

| Config | Description | Port |
|--------|-------------|------|
| **baseline** | Stock nginx 1.24.0, no inspect module | 18090 |
| **inspect-off** | nginx with inspect module loaded, `inspect off;` | 18091 |
| **inspect-on** | nginx with inspect module loaded, `inspect on;` | 18092 |

### OpenResty Comparison (Linux only)

| Config | Description | Port |
|--------|-------------|------|
| **or-baseline** | OpenResty 1.19.3.1, no Lua audit | 18094 |
| **or-audit** | OpenResty with Lua body filter + log phase audit | 18093 |

All tests use:
- `wrk -t4 -c100 -d20s --latency` (macOS)
- `wrk -t4 -c100 -d10s` (Linux quick comparison)
- `worker_processes 1`
- Static files served from local disk

## Results

### Linux x86_64 Comparison (openEuler 22.03, GCC 10.3.1)

> OpenResty 1.19.3.1 vs nginx-flowlens v1.0.0
> Same machine, same `wrk` parameters (`-t4 -c100 -d10s`)

| Config | RPS | Overhead |
|--------|-----|----------|
| OpenResty baseline | 68,392 | — |
| OpenResty + Lua audit | 19,387 | **-72%** |
| nginx-flowlens | 32,422 | **-53%** |

**Key Takeaway:** On the same Linux hardware, nginx-flowlens delivers **+67% higher RPS** than an equivalent OpenResty/Lua audit implementation.

---

### macOS Results (Apple Silicon)

#### Small File GET (index.html, ~600 bytes)

| Config | RPS | Avg Latency | P50 | P99 |
|--------|-----|-------------|-----|-----|
| baseline | 34,702 | 2.88ms | 2.85ms | 3.37ms |
| inspect-off | 34,572 | 2.90ms | 2.83ms | 4.51ms |
| inspect-on | 2,760 | 36.18ms | 35.91ms | 41.48ms |

**Overhead:**
- inspect-off vs baseline: **~0.4%** RPS drop (negligible)
- inspect-on vs baseline: **-92%** RPS, **+12.6x** avg latency

### Large File GET (1MB binary)

| Config | RPS | Avg Latency | P50 | P99 |
|--------|-----|-------------|-----|-----|
| baseline | 7,134 | 8.27ms | 7.79ms | 15.65ms |
| inspect-off | 7,287 | 9.00ms | 8.35ms | 15.76ms |
| inspect-on | 108 | 906.49ms | 917.79ms | 1.04s |

**Overhead:**
- inspect-off vs baseline: **~2%** RPS delta (negligible)
- inspect-on vs baseline: **-98.5%** RPS, **+109x** avg latency

## Analysis

### Key Findings

1. **Module presence alone costs almost nothing**
   When `inspect off;`, the module's filter functions are skipped early (line 193, 273 in `ngx_http_flowlens_module.c`). The overhead is within noise.

2. **File I/O per request is the dominant bottleneck**
   Current implementation opens, writes, and closes the log file on every request (`ngx_open_file` → `ngx_write_fd` → `ngx_close_file`). This is ~3 syscalls per request, all synchronous and blocking in a single-worker setup.

3. **Large response body amplification**
   For 1MB responses, the module must:
   - Accumulate the entire body chain into a memory buffer
   - Base64-encode the buffer (allocates another ~1.33MB)
   - Serialize full JSON (another allocation)
   - Write all of the above to disk
   This creates massive memory pressure and I/O latency.

### Bottleneck Breakdown (estimated)

| Component | Small File Impact | Large File Impact |
|-----------|-------------------|-------------------|
| Header capture | Low | Low |
| Body accumulation | Low | Very High |
| Base64 encode | Low | Very High |
| JSON serialization | Medium | High |
| `open()` per request | **Dominant** | **Dominant** |
| `write()` per request | **Dominant** | **Dominant** |
| `close()` per request | **Dominant** | **Dominant** |

## Optimization Roadmap

| Priority | Optimization | Expected Improvement |
|----------|-------------|---------------------|
| P0 | Cache log file fd (open once, write many, reopen on SIGHUP) | **+5-10x RPS** |
| P1 | Async batch output (buffer N records, flush on timer/count) | **+10-20x RPS** |
| P1 | Replace per-request `open/write/close` with buffered fwrite | **+3-5x RPS** |
| P2 | Zero-copy body accumulation (reference buf chain instead of copy) | Reduced memory, modest RPS gain |
| P2 | Streaming JSON (avoid full materialization) | Reduced memory, modest RPS gain |

## Reproducing

```bash
cd benchmark
./benchmark.sh
```

Adjust duration:
```bash
WRK_DURATION=30s ./benchmark.sh
```

## Raw Data

See `results/` directory for per-run raw wrk outputs (if captured).
