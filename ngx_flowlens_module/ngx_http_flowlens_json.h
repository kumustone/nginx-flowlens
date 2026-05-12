#ifndef NGX_HTTP_FLOWLENS_JSON_H
#define NGX_HTTP_FLOWLENS_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

char* inspect_event_to_json_c(ngx_http_request_t* r,
                            const ngx_str_t* req_method,
                            const ngx_str_t* req_uri,
                            const ngx_str_t* req_args,
                            const ngx_str_t* req_headers,
                            u_char* req_body_data,
                            size_t req_body_len,
                            ngx_flag_t req_body_truncated,
                            const ngx_str_t* req_body_dump_path,
                            ngx_uint_t resp_status,
                            const ngx_str_t* resp_headers,
                            u_char* resp_body_data,
                            size_t resp_body_len,
                            ngx_flag_t resp_body_truncated,
                            const ngx_str_t* resp_body_dump_path);

#ifdef __cplusplus
}
#endif

#endif
