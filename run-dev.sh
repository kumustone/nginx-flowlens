#!/usr/bin/env bash
# 开发环境快速启停脚本
# Usage:
#   ./run-dev.sh               # 默认=start：启动（已运行则先 stop 再 start）
#   ./run-dev.sh install       # 首次安装并启动
#   ./run-dev.sh install -f    # 强制覆盖已有配置，重新安装
#   ./run-dev.sh start         # 启动（同默认行为）
#   ./run-dev.sh stop          # 停止
#   ./run-dev.sh restart       # stop + start
#   ./run-dev.sh status        # 查看运行状态

set -euo pipefail

PROJECT_ROOT=$(cd "$(dirname "$0")" && pwd)
PROXY_PREFIX="${PROJECT_ROOT}/.nginx-dev"
NGINX_BIN="${PROJECT_ROOT}/nginx/objs/nginx"
NGINX_SRC_CONF_DIR="${PROJECT_ROOT}/nginx/conf"
NGINX_SBIN="${PROXY_PREFIX}/sbin/nginx"
NGINX_CONF="${PROXY_PREFIX}/conf/nginx.conf"
NGINX_DEV_CONF="${PROJECT_ROOT}/examples/nginx.dev.conf"

# ============ helpers ============

is_running() {
    local pid_file="${PROXY_PREFIX}/logs/nginx.pid"
    if [[ -f "$pid_file" ]]; then
        local pid
        pid=$(cat "$pid_file" 2>/dev/null)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
    fi
    # master 可能已退出但 worker 残留（macOS 常见），检测 orphan worker
    local workers
    workers=$(pgrep -f "nginx: worker" 2>/dev/null) || true
    [[ -n "$workers" ]] && return 0
    return 1
}

# 检查 ngx_flowlens_module/ 下源码是否比二进制新
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

stop() {
    if ! is_running; then
        echo "Proxy not running"
        return 0
    fi

    echo "Stopping proxy..."

    # 直接 kill -9 master
    local pid_file="${PROXY_PREFIX}/logs/nginx.pid"
    if [[ -f "$pid_file" ]]; then
        local master_pid
        master_pid=$(cat "$pid_file" 2>/dev/null)
        if [[ -n "$master_pid" ]] && kill -0 "$master_pid" 2>/dev/null; then
            kill -9 "$master_pid" 2>/dev/null || true
        fi
    fi

    # 兜底：杀掉所有残留的 worker（macOS 上 worker 可能变成孤儿进程）
    local workers
    workers=$(pgrep -f "nginx: worker" 2>/dev/null) || true
    if [[ -n "$workers" ]]; then
        echo "$workers" | while read -r p; do
            kill -9 "$p" 2>/dev/null || true
        done
    fi

    echo "Stopped"
}

start() {
    if is_running; then
        stop
    fi

    build_if_needed

    # fresh install 时 nginx 尚未编译，mime.types 软链接可能未创建，兜底处理
    if [[ -f "${NGINX_SRC_CONF_DIR}/mime.types" && ! -L "${PROXY_PREFIX}/conf/mime.types" ]]; then
        ln -sf "${NGINX_SRC_CONF_DIR}/mime.types" "${PROXY_PREFIX}/conf/mime.types"
    fi

    if [[ ! -L "${NGINX_SBIN}" ]]; then
        echo "ERROR: not installed. Run '$0 install' first." >&2
        exit 1
    fi

    echo "Testing nginx config..."
    echo "  binary:  ${NGINX_SBIN}"
    echo "  config:  ${NGINX_CONF}"
    echo "  prefix:  ${PROXY_PREFIX}"
    if ! "${NGINX_SBIN}" -p "${PROXY_PREFIX}" -c "${NGINX_CONF}" -t >/dev/null 2>&1; then
        echo "ERROR: nginx config test failed" >&2
        "${NGINX_SBIN}" -p "${PROXY_PREFIX}" -c "${NGINX_CONF}" -t >&2 || true
        exit 1
    fi

    echo "Starting proxy..."
    "${NGINX_SBIN}" -p "${PROXY_PREFIX}" -c "${NGINX_CONF}"
    sleep 0.3

    if ! is_running; then
        echo "ERROR: nginx started but process not found. Check ${PROXY_PREFIX}/logs/error.log" >&2
        exit 1
    fi

    echo "Proxy started"
    echo "  prefix:  ${PROXY_PREFIX}"
    echo "  config:  ${NGINX_CONF}"
    echo "  logs:    ${PROXY_PREFIX}/logs/"
    echo "  pid:     $(cat "${PROXY_PREFIX}/logs/nginx.pid" 2>/dev/null)"
}

status() {
    if is_running; then
        local pid
        pid=$(cat "${PROXY_PREFIX}/logs/nginx.pid" 2>/dev/null)
        echo "running (pid ${pid})"
        echo "  prefix: ${PROXY_PREFIX}"
    else
        echo "stopped"
    fi
}

install() {
    local force=0
    if [[ "${1:-}" == "-f" ]]; then
        force=1
    fi

    if [[ -d "${PROXY_PREFIX}" && -L "${NGINX_SBIN}" && $force -eq 0 ]]; then
        echo "Already installed at ${PROXY_PREFIX}"
        echo "Use '$0 start' to restart, or '$0 install -f' to overwrite."
        exit 1
    fi

    if [[ $force -eq 1 && -d "${PROXY_PREFIX}" ]]; then
        stop
        echo "Removing existing installation..."
        rm -rf "${PROXY_PREFIX}"
    fi

    echo "Installing to ${PROXY_PREFIX}..."

    mkdir -p "${PROXY_PREFIX}"/logs
    mkdir -p "${PROXY_PREFIX}"/conf
    mkdir -p "${PROXY_PREFIX}"/dumps
    mkdir -p "${PROXY_PREFIX}"/sbin
    mkdir -p "${PROXY_PREFIX}"/html

    # 默认首页
    echo '<!DOCTYPE html><html><body>Hello from nginx-flowlens</body></html>' > "${PROXY_PREFIX}/html/index.html"

    # 软链接 nginx 二进制
    ln -sf "${NGINX_BIN}" "${NGINX_SBIN}"

    # 复制开发配置（root 用户时注入 user 指令，避免 macOS 非 root 运行产生 warning）
    cp "${NGINX_DEV_CONF}" "${NGINX_CONF}"
    local current_user
    current_user=$(id -un)
    if [[ "$current_user" == "root" ]]; then
        printf 'user %s;\n' "${current_user}" | cat - "${NGINX_CONF}" > "${NGINX_CONF}.tmp"
        mv "${NGINX_CONF}.tmp" "${NGINX_CONF}"
    fi

    if [[ -f "${NGINX_SRC_CONF_DIR}/mime.types" ]]; then
        ln -sf "${NGINX_SRC_CONF_DIR}/mime.types" "${PROXY_PREFIX}/conf/mime.types"
    else
        echo "WARNING: mime.types not found at ${NGINX_SRC_CONF_DIR}/mime.types" >&2
        echo "         nginx may fail to start." >&2
    fi

    echo "Installed."
    start
}

# ============ main ============

cmd="${1:-start}"
shift 2>/dev/null || true

case "$cmd" in
    install) install "${1:-}" ;;
    start)   start   ;;
    stop)    stop    ;;
    restart) stop; start ;;
    status)  status  ;;
    *)
        echo "Usage: $0 {install|install -f|start|stop|restart|status}"
        exit 1
        ;;
esac
