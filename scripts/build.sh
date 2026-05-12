#!/bin/bash
#
# Compile-only build script for nginx with the inspect module.
# Output: ${PROJECT_ROOT}/nginx/objs/nginx
#
# This script does NOT run `make install`. Nothing is written outside
# the project's own nginx/ source directory. Deployment (copying the
# binary, generating runtime config, etc.) is left to run-dev.sh /
# test.sh / benchmark.sh.

set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
NGINX_DIR="${PROJECT_ROOT}/nginx"
NGINX_VERSION="1.28.0"
NGINX_TARBALL="nginx-${NGINX_VERSION}.tar.gz"
NGINX_DOWNLOAD_URL="https://nginx.org/download/${NGINX_TARBALL}"

function show_usage() {
    cat <<'EOF'
Usage: ./scripts/build.sh [OPTIONS]

Compiles nginx with the inspect module. The binary is left at
${PROJECT_ROOT}/nginx/objs/nginx — it is NOT installed anywhere.

Options:
  -n    Normal build (default): download nginx source, configure, compile
  -s    Static build: static-link C/C++ runtime (musl recommended)
  -c    Clean build artifacts (nginx/ source dir)
  -h    Show this help message

Environment variables:
  NGINX_SRC     Path to external nginx source directory (skip download)
  NO_INSPECT    Set to 1 to build without inspect module (benchmark baseline)
  PCRE_SRC      Path to PCRE source for static build
  ZLIB_SRC      Path to zlib source for static build
  OPENSSL_SRC   Path to OpenSSL source for static build

Examples:
  ./scripts/build.sh                       # Standard build (default)
  ./scripts/build.sh -s                    # Static build
  ./scripts/build.sh -c && ./scripts/build.sh    # Clean then rebuild
EOF
}

function clean_build() {
    echo "######## Cleaning build artifacts ########"
    if [[ -d "${NGINX_DIR}" ]]; then
        echo "Removing ${NGINX_DIR}"
        rm -rf "${NGINX_DIR}"
    fi
    echo "######## Clean complete ########"
}

function download_nginx() {
    echo "######## Downloading Nginx ${NGINX_VERSION} ########"
    cd "${PROJECT_ROOT}"
    local tarball_path="${PROJECT_ROOT}/${NGINX_TARBALL}"
    
    # Clean up on exit
    trap "rm -f '${tarball_path}'" EXIT
    
    if command -v curl &>/dev/null; then
        curl -fSL "${NGINX_DOWNLOAD_URL}" -o "${NGINX_TARBALL}" || {
            echo "Error: Failed to download nginx source"
            exit 1
        }
    elif command -v wget &>/dev/null; then
        wget "${NGINX_DOWNLOAD_URL}" -O "${NGINX_TARBALL}" || {
            echo "Error: Failed to download nginx source"
            exit 1
        }
    else
        echo >&2 "Error: neither curl nor wget is available."
        exit 1
    fi

    tar -xzf "${NGINX_TARBALL}" || {
        echo "Error: Failed to extract nginx source"
        exit 1
    }
    mv "nginx-${NGINX_VERSION}" "nginx"
    rm -f "${NGINX_TARBALL}"
    trap - EXIT  # Remove trap
    echo "######## Nginx ${NGINX_VERSION} downloaded and extracted ########"
}

function ensure_nginx_source() {
    if [[ -n "${NGINX_SRC}" ]]; then
        if [[ ! -d "${NGINX_SRC}" ]]; then
            echo >&2 "Error: NGINX_SRC directory does not exist: ${NGINX_SRC}"
            exit 1
        fi
        NGINX_DIR="${NGINX_SRC}"
        echo "######## Using external nginx source: ${NGINX_DIR} ########"
    elif [[ ! -d "${NGINX_DIR}" ]]; then
        download_nginx
    else
        echo "######## Using existing nginx source: ${NGINX_DIR} ########"
    fi
}

function configure_nginx() {
    local STATIC_BUILD="${STATIC_BUILD:-0}"
    echo "######## Configuring Nginx ########"
    # --prefix is set to the source dir so configure validates a
    # writable path. Since we never run `make install`, the value only
    # affects nginx's compiled-in default paths — and run-dev.sh /
    # test.sh always override these via `nginx -p <runtime_prefix>`.
    echo "configure prefix (compile-only, never installed): ${NGINX_DIR}"
    cd "${NGINX_DIR}"

    local cc_opt="-O3 -g -fstack-protector-all"
    local ld_opt=""
    local static_flags=""
    local pcre_flag=""
    local zlib_flag=""

    if [[ "${STATIC_BUILD}" == "1" ]]; then
        echo "Static build enabled"
        static_flags="-static"
        ld_opt="-static-libgcc -static-libstdc++ ${ld_opt}"

        if [[ -n "${PCRE_SRC}" && -d "${PCRE_SRC}" ]]; then
            pcre_flag="--with-pcre=${PCRE_SRC}"
        fi
        if [[ -n "${ZLIB_SRC}" && -d "${ZLIB_SRC}" ]]; then
            zlib_flag="--with-zlib=${ZLIB_SRC}"
        fi
        if [[ -n "${OPENSSL_SRC}" && -d "${OPENSSL_SRC}" ]]; then
            static_flags="${static_flags} --with-openssl=${OPENSSL_SRC}"
        fi
    fi

    if [[ "$(uname -s)" == "Darwin" ]]; then
        local brew_openssl brew_pcre2
        brew_openssl=$(brew --prefix openssl 2>/dev/null || true)
        brew_pcre2=$(brew --prefix pcre2 2>/dev/null || true)
        if [[ -n "${brew_openssl}" ]]; then
            cc_opt="${cc_opt} -I${brew_openssl}/include"
            ld_opt="-L${brew_openssl}/lib ${ld_opt}"
        fi
        if [[ -n "${brew_pcre2}" ]]; then
            cc_opt="${cc_opt} -I${brew_pcre2}/include"
            ld_opt="-L${brew_pcre2}/lib ${ld_opt}"
        fi
    fi

    if [[ -n "${static_flags}" ]]; then
        ld_opt="${ld_opt} ${static_flags}"
    fi

    local add_module=""
    if [[ "${NO_INSPECT}" != "1" ]]; then
        add_module="--add-module=${PROJECT_ROOT}/ngx_flowlens_module"
    fi

    ./configure \
        --prefix="${NGINX_DIR}" \
        --with-stream \
        --with-http_gzip_static_module \
        --with-http_gunzip_module \
        --with-http_auth_request_module \
        --with-http_stub_status_module \
        --with-http_ssl_module \
        --with-http_sub_module \
        --with-http_v2_module \
        --with-debug \
        ${pcre_flag} \
        ${zlib_flag} \
        ${add_module} \
        --with-cc-opt="${cc_opt}" \
        --with-ld-opt="${ld_opt}" \
        --with-openssl-opt='no-shared'
    cd "${PROJECT_ROOT}"
}

function build_nginx() {
    echo "######## Compiling Nginx ########"
    cd "${NGINX_DIR}"
    local ncpu=$(command -v nproc &>/dev/null && nproc || sysctl -n hw.ncpu)
    
    make -j"${ncpu}" || {
        echo "Error: Build failed"
        exit 1
    }

    # Verify binary was created
    if [[ ! -f "${NGINX_DIR}/objs/nginx" ]]; then
        echo "Error: Build failed - nginx binary not found at ${NGINX_DIR}/objs/nginx"
        exit 1
    fi

    cd "${PROJECT_ROOT}"
    echo "######## Nginx compiled successfully ########"
    echo "Binary:      ${NGINX_DIR}/objs/nginx"
    echo "mime.types:  ${NGINX_DIR}/conf/mime.types"
    echo ""
    echo "To run, use run-dev.sh / test.sh — they pass -p <runtime_prefix> -c <config>."
}

function parse_args() {
    local do_clean=0
    local do_build=1

    while getopts "nsch" opt; do
        case ${opt} in
            n )
                do_build=1
                ;;
            s )
                STATIC_BUILD=1
                do_build=1
                ;;
            c )
                do_clean=1
                do_build=0
                ;;
            h )
                show_usage
                exit 0
                ;;
            \? )
                show_usage
                exit 1
                ;;
        esac
    done

    if [[ ${do_clean} -eq 1 ]]; then
        clean_build
    fi

    if [[ ${do_build} -eq 1 ]]; then
        ensure_nginx_source
        configure_nginx
        build_nginx
    fi
}

parse_args "$@"
