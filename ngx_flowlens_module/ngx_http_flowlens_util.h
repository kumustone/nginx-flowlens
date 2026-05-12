#ifndef NGX_HTTP_FLOWLENS_UTIL_H
#define NGX_HTTP_FLOWLENS_UTIL_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

ngx_str_t trace_format_headers(ngx_pool_t* pool, ngx_list_t* headers);

#endif
