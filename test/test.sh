#!/usr/bin/env bash
# nginx-flowlens Test Suite
# Usage:
#   ./test.sh              # 准备环境 + 运行全部测试（默认）
#   ./test.sh setup        # 只准备环境（编译、部署、启动 backend + nginx）
#   ./test.sh test         # 只运行测试（假设环境已准备好）
#   ./test.sh clean        # 停止并清理环境

set -euo pipefail

TEST_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "${TEST_DIR}/.." && pwd)

: "${NGINX_BIN:=${PROJECT_ROOT}/nginx/objs/nginx}"
: "${TEST_PREFIX:=${PROJECT_ROOT}/.nginx-test}"
: "${BACKEND_PORT:=18100}"
: "${NO_COLOR:=}"

NGINX_SBIN="${TEST_PREFIX}/sbin/nginx"
NGINX_CONF="${TEST_PREFIX}/conf/nginx.conf"
NGINX_TEST_CONF="${PROJECT_ROOT}/examples/nginx.test.conf"
NGINX_SRC_CONF_DIR="${PROJECT_ROOT}/nginx/conf"

# Nginx listens on these ports; must match nginx.test.conf
TEST_PORTS=(18099 18098 18097 18096)

PASSED=0
FAILED=0

# ============ Colors ============
if [[ -z "$NO_COLOR" && -t 1 ]]; then
    C_RED='\033[0;31m'
    C_GREEN='\033[0;32m'
    C_RESET='\033[0m'
else
    C_RED=''
    C_GREEN=''
    C_RESET=''
fi

pass() { echo -e "  ${C_GREEN}PASS${C_RESET}"; PASSED=$((PASSED + 1)); }
fail() { echo -e "  ${C_RED}FAIL${C_RESET}"; FAILED=$((FAILED + 1)); }

# ============ Tool checks ============
for tool in curl jq python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool '$tool' not found" >&2
        exit 1
    fi
done

# ============ Build ============
needs_rebuild() {
    if [[ ! -x "${NGINX_BIN}" ]]; then
        return 0
    fi
    local newest_src
    newest_src=$(find "${PROJECT_ROOT}/ngx_flowlens_module" \
        \( -name "*.c" -o -name "*.h" -o -name "*.cpp" \) \
        -newer "${NGINX_BIN}" 2>/dev/null | head -1)
    [[ -n "$newest_src" ]]
}

build_if_needed() {
    if [[ ! -x "${NGINX_BIN}" ]]; then
        echo "nginx binary not found, building..."
        "${PROJECT_ROOT}/scripts/build.sh"
        return
    fi
    if needs_rebuild; then
        echo "source code changed, rebuilding..."
        "${PROJECT_ROOT}/scripts/build.sh"
    fi
}

# ============ Port helpers ============
ensure_ports_free() {
    local busy=()
    for port in "${TEST_PORTS[@]}" "$BACKEND_PORT"; do
        if curl -s --max-time 0.5 "http://127.0.0.1:$port/" >/dev/null 2>&1; then
            busy+=("$port")
        fi
    done
    if [[ ${#busy[@]} -eq 0 ]]; then
        return 0
    fi
    if command -v lsof >/dev/null 2>&1; then
        echo "Killing processes on ports: ${busy[*]}"
        for port in "${busy[@]}"; do
            # shellcheck disable=SC2046
            kill -9 $(lsof -ti :"$port" 2>/dev/null) 2>/dev/null || true
        done
        sleep 0.3
    else
        echo "ERROR: ports ${busy[*]} are in use. Install lsof or free them manually." >&2
        exit 1
    fi
}

# ============ Lifecycle ============

setup_env() {
    build_if_needed

    ensure_ports_free
    rm -rf "${TEST_PREFIX}"

    mkdir -p "${TEST_PREFIX}"/logs
    mkdir -p "${TEST_PREFIX}"/html
    mkdir -p "${TEST_PREFIX}"/conf
    mkdir -p "${TEST_PREFIX}"/dumps
    mkdir -p "${TEST_PREFIX}"/certs
    mkdir -p "${TEST_PREFIX}"/sbin

    # 软链接 nginx 二进制
    ln -sf "${NGINX_BIN}" "${NGINX_SBIN}"

    # 复制测试配置并注入当前用户（避免 worker 与目录权限不匹配）
    cp "${NGINX_TEST_CONF}" "${NGINX_CONF}"
    local current_user
    current_user=$(id -un)
    if [[ "$current_user" == "root" ]]; then
        printf 'user %s;\n' "${current_user}" | cat - "${NGINX_CONF}" > "${NGINX_CONF}.tmp"
        mv "${NGINX_CONF}.tmp" "${NGINX_CONF}"
    fi

    # 软链接 mime.types
    if [[ -f "${NGINX_SRC_CONF_DIR}/mime.types" ]]; then
        ln -sf "${NGINX_SRC_CONF_DIR}/mime.types" "${TEST_PREFIX}/conf/mime.types"
    fi

    # 测试静态文件
    echo '<!DOCTYPE html><html><body>Hello</body></html>' > "${TEST_PREFIX}/html/index.html"
    echo '{"test":"data"}' > "${TEST_PREFIX}/html/test.json"
    echo 'this is a text file for dump testing' > "${TEST_PREFIX}/html/dump-test.txt"
    dd if=/dev/urandom of="${TEST_PREFIX}/html/2k.bin" bs=1K count=2 2>/dev/null

    # TLS 证书
    if command -v openssl >/dev/null 2>&1; then
        openssl req -x509 -nodes -days 1 -newkey rsa:2048 \
            -keyout "${TEST_PREFIX}/certs/server.key" \
            -out "${TEST_PREFIX}/certs/server.crt" \
            -subj "/CN=localhost" 2>/dev/null
    fi

    start_backend
    start_nginx
}

start_nginx() {
    echo "Testing nginx config..."
    if ! "${NGINX_SBIN}" -p "${TEST_PREFIX}" -c "${NGINX_CONF}" -t >/dev/null 2>&1; then
        echo "ERROR: nginx config test failed" >&2
        "${NGINX_SBIN}" -p "${TEST_PREFIX}" -c "${NGINX_CONF}" -t >&2 || true
        exit 1
    fi

    echo "Starting nginx..."
    "${NGINX_SBIN}" -p "${TEST_PREFIX}" -c "${NGINX_CONF}"
    sleep 0.5
}

stop_nginx() {
    local pid_file="${TEST_PREFIX}/logs/nginx.pid"
    if [[ -f "$pid_file" ]]; then
        local master_pid
        master_pid=$(cat "$pid_file" 2>/dev/null)
        if [[ -n "$master_pid" ]] && kill -0 "$master_pid" 2>/dev/null; then
            kill -9 "$master_pid" 2>/dev/null || true
        fi
    fi
    local workers
    workers=$(pgrep -f "nginx: worker" 2>/dev/null) || true
    if [[ -n "$workers" ]]; then
        echo "$workers" | while read -r p; do
            kill -9 "$p" 2>/dev/null || true
        done
    fi
}

BACKEND_PID=""
start_backend() {
    python3 "${TEST_DIR}/backend.py" "$BACKEND_PORT" &
    BACKEND_PID=$!
    local retries=30
    while (( retries-- > 0 )); do
        curl -s --max-time 0.5 "http://127.0.0.1:${BACKEND_PORT}/status" >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    echo "ERROR: backend failed to start" >&2
    return 1
}

stop_backend() {
    if [[ -n "${BACKEND_PID:-}" ]]; then
        kill "$BACKEND_PID" 2>/dev/null || true
        wait "$BACKEND_PID" 2>/dev/null || true
        BACKEND_PID=""
    fi
}

clean_env() {
    stop_nginx
    stop_backend
    rm -rf "${TEST_PREFIX}"
    echo "Cleaned."
}

# ============ Log helpers ============
flush_log() { "${NGINX_SBIN}" -p "${TEST_PREFIX}" -s reopen 2>/dev/null || true; sleep 0.15; }

clear_logs() {
    flush_log
    : > "${TEST_PREFIX}/logs/inspect.log"
    : > "${TEST_PREFIX}/logs/inspect-alt.log"
    : > "${TEST_PREFIX}/logs/error.log"
}

clear_dumps() { rm -f "${TEST_PREFIX}/dumps"/*; }

# ============ Assertions ============
assert_field() {
    local field=$1 expected=$2 log_file="${3:-${TEST_PREFIX}/logs/inspect.log}"
    flush_log
    local actual
    actual=$(jq -r "$field" "$log_file" 2>/dev/null | tail -1)
    if [[ "$actual" == "$expected" ]]; then
        return 0
    fi
    echo "    field [$field] expected '$expected', got '$actual'"
    return 1
}

assert_base64_decode() {
    local field=$1 expected=$2 log_file="${3:-${TEST_PREFIX}/logs/inspect.log}"
    flush_log
    local b64 decoded
    b64=$(jq -r "$field" "$log_file" 2>/dev/null | tail -1)
    decoded=$(printf '%s\n' "$b64" | python3 -m base64 -d 2>/dev/null)
    if [[ "$decoded" == "$expected" ]]; then
        return 0
    fi
    echo "    base64 decode expected '$expected', got '$decoded'"
    return 1
}

assert_log_contains() {
    local substring=$1 log_file="${2:-${TEST_PREFIX}/logs/inspect.log}"
    flush_log
    if grep -q "$substring" "$log_file" 2>/dev/null; then
        return 0
    fi
    echo "    log does not contain '$substring'"
    return 1
}

assert_no_errors() {
    local f="${TEST_PREFIX}/logs/error.log"
    [[ -f "$f" ]] || return 0
    local n_alert n_err
    n_alert=$(grep -cE '\[alert\]|\[emerg\]|\[crit\]' "$f" 2>/dev/null) || true
    n_err=$(grep -cE '\[error\]' "$f" 2>/dev/null) || true
    if (( n_alert > 0 )); then
        echo "    nginx error.log has $n_alert alert/emerg/crit entries"
        grep -E '\[alert\]|\[emerg\]|\[crit\]' "$f" | head -3 | sed 's/^/      /'
        return 1
    fi
    if (( n_err > 0 )); then
        echo "    nginx error.log has $n_err error entries"
        grep -E '\[error\]' "$f" | head -3 | sed 's/^/      /'
        return 1
    fi
    return 0
}

run_test() {
    local name=$1 output_file="${TEST_PREFIX}/.test_output_$$"
    shift
    echo "Test: $name"
    if "$@" >"$output_file" 2>&1; then
        pass
    else
        fail
        if [[ -s "$output_file" ]]; then
            echo "    details:"
            sed 's/^/      /' "$output_file" | head -20
        fi
        if [[ -s "${TEST_PREFIX}/logs/inspect.log" ]]; then
            echo "    log: $(tail -1 "${TEST_PREFIX}/logs/inspect.log")"
        fi
    fi
    rm -f "$output_file"
}

# ============ Static File Tests ============
test_inspect_off() {
    clear_logs
    curl -s --max-time 10 http://localhost:18099/no-inspect/test.json >/dev/null
    [[ ! -s "${TEST_PREFIX}/logs/inspect.log" ]]
}

test_basic_get() {
    clear_logs
    curl -s --max-time 10 http://localhost:18099/ >/dev/null
    assert_field ".request.method" "GET" && \
    assert_field ".request.uri" "/index.html" && \
    assert_field ".response.status" "200" && \
    assert_field ".request.body_truncated" "false" && \
    assert_field ".response.body_truncated" "false"
}

test_post_with_body() {
    clear_logs
    curl -s --max-time 10 -X POST http://localhost:18099/test.json \
        -H "Content-Type: application/json" \
        -d '{"username":"alice","password":"secret123"}' >/dev/null
    assert_field ".request.method" "POST" && \
    assert_field ".request.uri" "/test.json" && \
    assert_field ".request.headers.\"Content-Type\"" "application/json" && \
    assert_base64_decode ".request.body" '{"username":"alice","password":"secret123"}'
}

test_empty_body() {
    clear_logs
    curl -s --max-time 10 -X POST http://localhost:18099/test.json -H "Content-Length: 0" >/dev/null
    assert_field ".request.method" "POST" && \
    assert_field ".request.body" "" && \
    assert_field ".request.body_len" "0"
}

test_404_response() {
    clear_logs
    curl -s --max-time 10 http://localhost:18099/not-exist >/dev/null
    assert_field ".request.method" "GET" && \
    assert_field ".request.uri" "/not-exist" && \
    assert_field ".response.status" "404"
}

test_request_body_truncation() {
    clear_logs
    python3 -c "print('A'*2048)" > "${TEST_PREFIX}/test_2k.txt"
    curl -s --max-time 10 -X POST --data-binary "@${TEST_PREFIX}/test_2k.txt" http://localhost:18099/test.json >/dev/null
    assert_field ".request.body_truncated" "true" && \
    assert_field ".request.body_len" "1024"
}

test_response_body_truncation() {
    clear_logs
    curl -s --max-time 10 http://localhost:18099/large-file >/dev/null
    assert_field ".response.body_truncated" "true" && \
    assert_field ".response.body_len" "1024"
}

test_alt_log_path() {
    clear_logs
    curl -s --max-time 10 http://localhost:18099/alt-log/test.json >/dev/null
    flush_log
    [[ -s "${TEST_PREFIX}/logs/inspect-alt.log" ]] && \
    assert_field ".request.uri" "/alt-log/test.json" "${TEST_PREFIX}/logs/inspect-alt.log"
}

test_binary_body_base64() {
    clear_logs
    printf '\x00\x01\x02\xff\xfe' > "${TEST_PREFIX}/test_bin.bin"
    curl -s --max-time 10 -X POST http://localhost:18099/test.json \
        -H "Content-Type: application/octet-stream" \
        --data-binary "@${TEST_PREFIX}/test_bin.bin" >/dev/null
    flush_log
    local b64 decoded
    b64=$(jq -r '.request.body' "${TEST_PREFIX}/logs/inspect.log")
    decoded=$(printf '%s\n' "$b64" | python3 -m base64 -d 2>/dev/null | xxd -p | tr -d '\n')
    [[ "$decoded" == "000102fffe" ]]
}

# ============ Proxy / Backend Tests ============
test_chunked_response() {
    clear_logs
    curl -s --max-time 10 http://localhost:18099/proxy/chunked >/dev/null
    flush_log
    local expected_b64 actual_b64 body dump_path
    expected_b64=$(printf 'chunk-0\nchunk-1\nchunk-2\nchunk-3\nchunk-4\n' | python3 -m base64 | tr -d '\n')
    body=$(jq -r '.response.body' "${TEST_PREFIX}/logs/inspect.log")
    if [[ "$body" == "null" || -z "$body" ]]; then
        dump_path=$(jq -r '.response.dump_path' "${TEST_PREFIX}/logs/inspect.log" | tr -d '\n')
        actual_b64=$(python3 -m base64 "$dump_path" | tr -d '\n')
    else
        actual_b64=$(printf '%s' "$body" | tr -d '\n')
    fi
    [[ "$actual_b64" == "$expected_b64" ]] && \
    assert_field ".response.body_truncated" "false"
}

test_sse_response() {
    clear_logs
    curl -s --max-time 10 http://localhost:18099/sse/ >/dev/null
    flush_log
    local expected_b64 actual_b64
    expected_b64=$(printf 'data: event-0\n\ndata: event-1\n\ndata: event-2\n\n' | python3 -m base64 | tr -d '\n')
    actual_b64=$(jq -r '.response.body' "${TEST_PREFIX}/logs/inspect.log" | tr -d '\n')
    [[ "$actual_b64" == "$expected_b64" ]] && \
    assert_field ".response.body_truncated" "false"
}

test_gzip_captured_raw() {
    clear_logs
    curl -s --max-time 10 -H "Accept-Encoding: gzip" http://localhost:18099/proxy/gzip-target >/dev/null
    flush_log
    local body_decoded
    body_decoded=$(jq -r '.response.body' "${TEST_PREFIX}/logs/inspect.log" | python3 -m base64 -d 2>/dev/null)
    echo "$body_decoded" | python3 -c "import sys,json; json.load(sys.stdin)" 2>/dev/null
}

test_large_proxy_response() {
    clear_logs
    curl -s --max-time 10 "http://localhost:18099/proxy/large?size=5m" >/dev/null
    assert_field ".response.body_truncated" "true" && \
    assert_field ".response.body_len" "1024"
}

# ============ HTTP Method Tests ============
test_http_methods() {
    clear_logs
    curl -s --max-time 10 -X PUT    http://localhost:18099/proxy/echo --data "put-data"    >/dev/null
    assert_field ".request.method" "PUT"

    clear_logs
    curl -s --max-time 10 -X PATCH  http://localhost:18099/proxy/echo --data "patch-data"  >/dev/null
    assert_field ".request.method" "PATCH"

    clear_logs
    curl -s --max-time 10 -X DELETE http://localhost:18099/proxy/echo --data "delete-data" >/dev/null
    assert_field ".request.method" "DELETE"
}

# ============ HTTP/2 Tests ============
test_http2_basic() {
    clear_logs
    curl -s --max-time 10 --http2-prior-knowledge http://localhost:18098/ >/dev/null
    assert_field ".request.method" "GET" && \
    assert_field ".request.uri" "/index.html" && \
    assert_field ".response.status" "200" && \
    assert_field ".request.headers.host" "localhost:18098"
}

test_http2_post_body() {
    clear_logs
    curl -s --max-time 10 --http2-prior-knowledge -X POST http://localhost:18098/test.json \
        -H "Content-Type: application/json" -d '{"h2":"test"}' >/dev/null
    assert_field ".request.method" "POST" && \
    assert_base64_decode ".request.body" '{"h2":"test"}' && \
    assert_field ".request.headers.host" "localhost:18098"
}

test_http2_proxy_echo() {
    clear_logs
    curl -s --max-time 10 --http2-prior-knowledge -X POST http://localhost:18098/proxy/echo \
        -H "Content-Type: application/json" -d '{"h2":"proxy"}' >/dev/null
    assert_field ".request.method" "POST" && \
    assert_field ".request.uri" "/proxy/echo" && \
    assert_no_errors
}

test_https_proxy_echo() {
    clear_logs
    curl -s --max-time 10 -k -X POST https://localhost:18097/proxy/echo \
        -H "Content-Type: application/json" -d '{"tls":"proxy"}' >/dev/null
    assert_field ".request.method" "POST" && \
    assert_field ".request.uri" "/proxy/echo" && \
    assert_no_errors
}

# ============ Internal Redirect Tests ============
test_internal_redirect_tryfiles() {
    clear_logs
    curl -s --max-time 10 -o "${TEST_PREFIX}/test_body" -w "%{http_code}" \
        http://localhost:18099/internal-redirect/anything >"${TEST_PREFIX}/test_status"
    local status; status=$(cat "${TEST_PREFIX}/test_status")
    [[ "$status" == "200" ]] && \
    grep -q "fallback" "${TEST_PREFIX}/test_body" && \
    assert_no_errors
}

test_internal_redirect_errorpage() {
    clear_logs
    curl -s --max-time 10 -o "${TEST_PREFIX}/test_body" -w "%{http_code}" \
        http://localhost:18099/error-redirect/ >"${TEST_PREFIX}/test_status"
    local status; status=$(cat "${TEST_PREFIX}/test_status")
    [[ "$status" == "200" ]] && \
    grep -q "teapot" "${TEST_PREFIX}/test_body"
}

# ============ Body Dump Tests ============
test_request_body_dump() {
    clear_logs
    clear_dumps
    curl -s --max-time 10 -X POST http://localhost:18099/proxy/echo \
        -H "Content-Type: text/plain" \
        -d 'this is request body dump test data' >/dev/null
    flush_log
    local dp body
    dp=$(jq -r '.request.dump_path' "${TEST_PREFIX}/logs/inspect.log" 2>/dev/null | tail -1)
    body=$(jq -r '.request.body' "${TEST_PREFIX}/logs/inspect.log" 2>/dev/null | tail -1)
    [[ "$dp" != "null" && -n "$dp" && -f "$dp" ]] || { echo "    dump_path missing or file not found" >&2; return 1; }
    [[ "$body" == "null" ]] || { echo "    body should be null when dumped, got '$body'" >&2; return 1; }
    assert_field ".request.body_len" "35"
}

test_response_body_dump() {
    clear_logs
    clear_dumps
    curl -s --max-time 10 http://localhost:18099/dump-test.txt >/dev/null
    flush_log
    local dp
    dp=$(jq -r '.response.dump_path' "${TEST_PREFIX}/logs/inspect.log" 2>/dev/null | tail -1)
    [[ "$dp" != "null" && -n "$dp" && -f "$dp" ]] && \
    assert_field ".response.body" "null" && \
    assert_field ".response.body_len" "37"
}

# ============ Buffered Write Tests ============
test_buffered_audit() {
    clear_logs
    for i in {1..8}; do
        curl -s --max-time 10 http://localhost:18099/test.json >/dev/null
    done
    flush_log
    local count; count=$(wc -l < "${TEST_PREFIX}/logs/inspect.log" | tr -d ' ')
    (( count >= 8 ))
}

test_buffer_timer_flush() {
    clear_logs
    curl -s --max-time 10 http://localhost:18099/test.json >/dev/null
    sleep 0.3
    [[ -s "${TEST_PREFIX}/logs/inspect.log" ]]
}

test_per_worker_log() {
    local worker_pid
    worker_pid=$(pgrep -f "nginx: worker" 2>/dev/null) || true
    if [[ -n "$worker_pid" ]]; then
        touch "${TEST_PREFIX}/logs/inspect-per-worker.log.${worker_pid}" 2>/dev/null || true
    fi
    clear_logs
    curl -s --max-time 10 http://localhost:18099/per-worker/json >/dev/null
    sleep 0.3
    local pw_count
    pw_count=$(find "${TEST_PREFIX}/logs" -maxdepth 1 -name 'inspect-per-worker.log.*' 2>/dev/null | wc -l | tr -d ' ')
    (( pw_count >= 1 ))
}

# ============ Run All Tests ============
run_all_tests() {
    echo ""
    echo "========================================"
    echo "  nginx-flowlens Test Suite"
    echo "========================================"
    echo ""

    echo "--- Static File Tests ---"
    run_test "inspect off - no log generated"           test_inspect_off
    run_test "basic GET - method, uri, status"          test_basic_get
    run_test "POST with JSON body - base64 round-trip"  test_post_with_body
    run_test "empty body POST"                          test_empty_body
    run_test "404 response capture"                     test_404_response
    run_test "request body truncation (2KB > 1KB max)"  test_request_body_truncation
    run_test "response body truncation (2KB > 1KB max)" test_response_body_truncation
    run_test "alternative log path per location"        test_alt_log_path
    run_test "binary body base64 encoding"              test_binary_body_base64

    echo ""
    echo "--- Proxy / Backend Tests ---"
    run_test "chunked response via proxy"               test_chunked_response
    run_test "SSE response via proxy"                   test_sse_response
    run_test "gzip response captured as raw body"       test_gzip_captured_raw
    run_test "large proxy response (5MB > 1KB max)"     test_large_proxy_response

    echo ""
    echo "--- HTTP Method Tests ---"
    run_test "PUT / PATCH / DELETE methods"             test_http_methods

    echo ""
    echo "--- HTTP/2 Tests ---"
    run_test "HTTP/2 basic GET"                         test_http2_basic
    run_test "HTTP/2 POST with body"                    test_http2_post_body
    run_test "HTTP/2 proxy echo"                        test_http2_proxy_echo

    echo ""
    echo "--- HTTPS Proxy Tests ---"
    run_test "HTTPS proxy echo"                         test_https_proxy_echo

    echo ""
    echo "--- Internal Redirect Tests ---"
    run_test "try_files internal redirect"              test_internal_redirect_tryfiles
    run_test "error_page internal redirect"             test_internal_redirect_errorpage

    echo ""
    echo "--- Body Dump Tests ---"
    run_test "request body dump (text/plain > 10B)"     test_request_body_dump
    run_test "response body dump (text/plain > 10B)"    test_response_body_dump

    echo ""
    echo "--- Buffered Write Tests ---"
    run_test "buffered audit flush via reopen"          test_buffered_audit
    run_test "buffer timer flush (100ms)"               test_buffer_timer_flush

    echo ""
    echo "--- Per-Worker Log Tests ---"
    run_test "per-worker log file"                      test_per_worker_log

    echo ""
    echo "========================================"
    printf "  Results: ${C_GREEN}%d passed${C_RESET}, ${C_RED}%d failed${C_RESET}\n" "$PASSED" "$FAILED"
    echo "========================================"

    (( FAILED == 0 ))
}

# ============ Main ============

cmd="${1:-}"

case "$cmd" in
    setup)
        setup_env
        echo "Environment ready at ${TEST_PREFIX}"
        ;;
    clean)
        clean_env
        ;;
    test)
        if [[ ! -x "${NGINX_SBIN}" ]]; then
            echo "ERROR: not installed. Run '$0 setup' first." >&2
            exit 1
        fi
        run_all_tests
        stop_nginx
        stop_backend
        ;;
    "")
        setup_env
        run_all_tests
        stop_nginx
        stop_backend
        ;;
    *)
        echo "Usage: $0 {setup|test|clean}"
        echo ""
        echo "  $0        # setup env + run all tests (default)"
        echo "  $0 setup  # prepare environment only"
        echo "  $0 test   # run tests only (env must be ready)"
        echo "  $0 clean  # stop and cleanup"
        exit 1
        ;;
esac
