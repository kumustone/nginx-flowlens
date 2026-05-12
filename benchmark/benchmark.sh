#!/bin/bash

set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BENCHMARK_DIR="${PROJECT_ROOT}/benchmark"

PREFIX_BASELINE="/tmp/nginx-baseline"
PREFIX_INSPECT="/tmp/nginx-inspect"

WRK_THREADS=4
WRK_CONNECTIONS=100
WRK_DURATION=20s

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

function check_tools() {
    if ! command -v wrk &>/dev/null; then
        echo -e "${RED}Error: wrk not found. Install with: brew install wrk${NC}"
        exit 1
    fi
}

function build_baseline() {
    echo "=== Building baseline nginx (no inspect module) ==="
    "${PROJECT_ROOT}/scripts/build.sh" -c >/dev/null
    NO_INSPECT=1 "${PROJECT_ROOT}/scripts/build.sh" -n >/dev/null
    mkdir -p "${PREFIX_BASELINE}/sbin" "${PREFIX_BASELINE}/conf" "${PREFIX_BASELINE}/logs"
    cp "${PROJECT_ROOT}/nginx/objs/nginx" "${PREFIX_BASELINE}/sbin/nginx"
    cp "${PROJECT_ROOT}/nginx/conf/mime.types" "${PREFIX_BASELINE}/conf/mime.types" 2>/dev/null || true
    cp "${BENCHMARK_DIR}/nginx-baseline.conf" "${PREFIX_BASELINE}/conf/nginx.conf"
    echo -e "${GREEN}Baseline built: ${PREFIX_BASELINE}/sbin/nginx${NC}"
}

function build_inspect() {
    echo "=== Building inspect nginx ==="
    "${PROJECT_ROOT}/scripts/build.sh" -c >/dev/null
    "${PROJECT_ROOT}/scripts/build.sh" -n >/dev/null
    mkdir -p "${PREFIX_INSPECT}/sbin" "${PREFIX_INSPECT}/conf" "${PREFIX_INSPECT}/logs"
    cp "${PROJECT_ROOT}/nginx/objs/nginx" "${PREFIX_INSPECT}/sbin/nginx"
    cp "${PROJECT_ROOT}/nginx/conf/mime.types" "${PREFIX_INSPECT}/conf/mime.types" 2>/dev/null || true
    echo -e "${GREEN}Inspect built: ${PREFIX_INSPECT}/sbin/nginx${NC}"
}

function prepare_html() {
    local html_dir_baseline="${PREFIX_BASELINE}/html"
    local html_dir_inspect="${PREFIX_INSPECT}/html"

    mkdir -p "${html_dir_baseline}" "${html_dir_inspect}"

    if [[ ! -f "${html_dir_baseline}/index.html" ]]; then
        cat > "${html_dir_baseline}/index.html" <<'EOF'
<!DOCTYPE html>
<html>
<head><title>Benchmark</title></head>
<body><h1>nginx-flowlens benchmark</h1></body>
</html>
EOF
    fi
    cp "${html_dir_baseline}/index.html" "${html_dir_inspect}/index.html"

    dd if=/dev/urandom of="${html_dir_baseline}/1mb.bin" bs=1M count=1 2>/dev/null
    cp "${html_dir_baseline}/1mb.bin" "${html_dir_inspect}/1mb.bin"
}

function start_nginx() {
    local prefix=$1
    local conf=$2
    cp "${conf}" "${prefix}/conf/nginx.conf"
    "${prefix}/sbin/nginx" -p "${prefix}" -c "${prefix}/conf/nginx.conf" -t >/dev/null 2>&1
    "${prefix}/sbin/nginx" -p "${prefix}" -c "${prefix}/conf/nginx.conf"
    sleep 0.5
}

function stop_nginx() {
    local prefix=$1
    "${prefix}/sbin/nginx" -p "${prefix}" -s stop 2>/dev/null || true
    sleep 0.3
}

function run_wrk() {
    local url=$1
    local label=$2
    local output

    echo "  Running wrk: ${label} ..." >&2
    output=$(wrk -t${WRK_THREADS} -c${WRK_CONNECTIONS} -d${WRK_DURATION} --latency "${url}" 2>/dev/null)

    local rps
    local latency_avg
    local latency_50
    local latency_99

    rps=$(echo "$output" | grep 'Requests/sec' | awk '{print $2}')
    latency_avg=$(echo "$output" | grep '^[[:space:]]*Latency' | head -1 | awk '{print $2}')
    latency_50=$(echo "$output" | grep '50%' | awk '{print $2}')
    latency_99=$(echo "$output" | grep '99%' | awk '{print $2}')

    echo "${label}:${rps}:${latency_avg}:${latency_50}:${latency_99}"
}

function benchmark_scenario() {
    local name=$1
    local path=$2
    local baseline_result
    local inspect_off_result
    local inspect_on_json_result
    local inspect_on_tlv_result

    echo ""
    echo "=== Scenario: ${name} ==="

    stop_nginx "${PREFIX_BASELINE}" 2>/dev/null || true
    start_nginx "${PREFIX_BASELINE}" "${BENCHMARK_DIR}/nginx-baseline.conf"
    baseline_result=$(run_wrk "http://127.0.0.1:18090${path}" "baseline")
    stop_nginx "${PREFIX_BASELINE}"

    stop_nginx "${PREFIX_INSPECT}" 2>/dev/null || true
    start_nginx "${PREFIX_INSPECT}" "${BENCHMARK_DIR}/nginx-inspect-off.conf"
    inspect_off_result=$(run_wrk "http://127.0.0.1:18091${path}" "inspect-off")
    stop_nginx "${PREFIX_INSPECT}"

    stop_nginx "${PREFIX_INSPECT}" 2>/dev/null || true
    start_nginx "${PREFIX_INSPECT}" "${BENCHMARK_DIR}/nginx-inspect-on.conf"
    inspect_on_json_result=$(run_wrk "http://127.0.0.1:18092${path}" "inspect-on-json")
    stop_nginx "${PREFIX_INSPECT}"

    stop_nginx "${PREFIX_INSPECT}" 2>/dev/null || true
    start_nginx "${PREFIX_INSPECT}" "${BENCHMARK_DIR}/nginx-inspect-tlv.conf"
    inspect_on_tlv_result=$(run_wrk "http://127.0.0.1:18093${path}" "inspect-on-tlv")
    stop_nginx "${PREFIX_INSPECT}"

    echo ""
    echo "  Results:"
    printf "    %-18s %12s %12s %12s %12s\n" "Config" "RPS" "Avg Latency" "P50" "P99"
    echo "    --------------------------------------------------------------------"

    IFS=: read -r _ rps lat_avg lat_50 lat_99 <<< "$baseline_result"
    printf "    %-18s %12s %12s %12s %12s\n" "baseline" "${rps}" "${lat_avg}" "${lat_50}" "${lat_99}"

    IFS=: read -r _ rps lat_avg lat_50 lat_99 <<< "$inspect_off_result"
    printf "    %-18s %12s %12s %12s %12s\n" "inspect-off" "${rps}" "${lat_avg}" "${lat_50}" "${lat_99}"

    IFS=: read -r _ rps lat_avg lat_50 lat_99 <<< "$inspect_on_json_result"
    printf "    %-18s %12s %12s %12s %12s\n" "inspect-on (json)" "${rps}" "${lat_avg}" "${lat_50}" "${lat_99}"

    IFS=: read -r _ rps lat_avg lat_50 lat_99 <<< "$inspect_on_tlv_result"
    printf "    %-18s %12s %12s %12s %12s\n" "inspect-on (tlv)" "${rps}" "${lat_avg}" "${lat_50}" "${lat_99}"
}

# Cleanup any existing instances
stop_nginx "${PREFIX_BASELINE}" 2>/dev/null || true
stop_nginx "${PREFIX_INSPECT}" 2>/dev/null || true

check_tools
build_baseline
build_inspect
prepare_html

echo ""
echo "========================================"
echo "  nginx-flowlens Benchmark Suite"
echo "========================================"
echo "  wrk: ${WRK_THREADS} threads, ${WRK_CONNECTIONS} connections, ${WRK_DURATION}"
echo ""

benchmark_scenario "Small file GET" "/"
benchmark_scenario "Large file GET (1MB)" "/1mb.bin"

echo ""
echo "========================================"
echo "  Benchmark complete"
echo "========================================"
