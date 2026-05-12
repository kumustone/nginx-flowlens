#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_string.h>

#include "ngx_http_flowlens_util.h"
#include "ngx_http_flowlens_tlv.h"
#include "ngx_http_flowlens_json.h"

static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt   ngx_http_next_body_filter;

/* Per-worker log fd (lazily opened when per_worker is enabled) */
static ngx_fd_t ngx_http_flowlens_worker_fd = NGX_INVALID_FILE;
static ngx_str_t ngx_http_flowlens_worker_path = ngx_null_string;

typedef struct {
    u_char*     start;
    u_char*     pos;
    u_char*     last;
    ngx_event_t* event;
    ngx_msec_t  flush;
    ngx_flag_t  is_json;   /* 1 = newline-delimited (json), 0 = binary (tlv) */
} ngx_http_flowlens_log_buf_t;

typedef struct {
    ngx_flag_t enable;
    ngx_str_t  inspect_log;
    size_t     max_body_size;

    ngx_flag_t dump_enable;
    ngx_str_t  dump_path;
    size_t     dump_min_size;

    ngx_open_file_t* log_file;
    size_t     buffer_size;
    ngx_msec_t flush_time;

    ngx_uint_t format;  /* 0 = json, 1 = tlv */
    ngx_flag_t per_worker;
} ngx_http_flowlens_conf_t;

typedef struct {
    ngx_http_flowlens_conf_t* conf;

    ngx_str_t req_method;
    ngx_str_t req_uri;
    ngx_str_t req_args;
    ngx_str_t req_headers;

    u_char* req_body_buf;
    size_t  req_body_len;
    size_t  req_body_capacity;

    ngx_uint_t resp_status;
    ngx_str_t  resp_headers;

    u_char* resp_body_buf;
    size_t  resp_body_len;
    size_t  resp_body_capacity;

    ngx_flag_t header_done;
    ngx_flag_t body_done;
    ngx_flag_t req_body_done;

    ngx_flag_t req_body_truncated;
    ngx_flag_t resp_body_truncated;

    ngx_str_t  req_body_dump_path;
    ngx_str_t  resp_body_dump_path;

    ngx_str_t  capture_uri;      /* snapshot for internal redirect detection */
} ngx_http_flowlens_ctx_t;

static void* ngx_http_flowlens_create_main_conf(ngx_conf_t* cf);
static void* ngx_http_flowlens_create_loc_conf(ngx_conf_t* cf);
static char* ngx_http_flowlens_merge_loc_conf(ngx_conf_t* cf, void* parent, void* child);
static ngx_int_t ngx_http_flowlens_module_init(ngx_conf_t* cf);
static char* ngx_conf_set_inspect_format(ngx_conf_t* cf, ngx_command_t* cmd, void* conf);

static ngx_int_t ngx_http_flowlens_request_handler(ngx_http_request_t* r);
static ngx_int_t ngx_http_flowlens_header_filter(ngx_http_request_t* r);
static ngx_int_t ngx_http_flowlens_body_filter(ngx_http_request_t* r, ngx_chain_t* in);
static void ngx_http_flowlens_body_handler(ngx_http_request_t* r);

static void trace_collect_request(ngx_http_request_t* r, ngx_http_flowlens_ctx_t* ctx);
static ngx_int_t trace_append_data(ngx_http_request_t* r, u_char** buf, size_t* len,
                                    size_t* capacity, size_t max, u_char* data, size_t data_len,
                                    ngx_flag_t* truncated);
static ngx_int_t trace_append_file_buf(ngx_http_request_t* r, u_char** buf, size_t* len,
                                        size_t* capacity, size_t max, ngx_buf_t* b,
                                        ngx_flag_t* truncated);
static void trace_send_log(ngx_http_request_t* r, ngx_http_flowlens_ctx_t* ctx);
static void trace_inspect_flush(ngx_open_file_t* file, ngx_log_t* log);
static void trace_inspect_flush_handler(ngx_event_t* ev);
static void ngx_http_flowlens_exit_process(ngx_cycle_t* cycle);

static ngx_str_t trace_detect_file_type(ngx_http_request_t* r, ngx_flag_t is_request,
                                        ngx_str_t* content_type, u_char* body, size_t body_len);
static ngx_str_t trace_generate_filename_auto(ngx_http_request_t* r, ngx_flag_t is_request,
                                              ngx_str_t* ext);
static ngx_flag_t trace_dump_body(ngx_http_request_t* r, ngx_str_t* dump_path, u_char* body, size_t body_len, ngx_str_t* filename, ngx_str_t* out_path);

/* ===================== Config Directives ===================== */

static ngx_command_t ngx_http_flowlens_commands[] = {
    { ngx_string("inspect"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, enable),
      NULL },

    { ngx_string("inspect_log"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, inspect_log),
      NULL },

    { ngx_string("inspect_max_body_size"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, max_body_size),
      NULL },

    { ngx_string("inspect_dump"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, dump_enable),
      NULL },

    { ngx_string("inspect_dump_path"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, dump_path),
      NULL },

    { ngx_string("inspect_dump_min_size"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, dump_min_size),
      NULL },

    { ngx_string("inspect_buffer_size"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, buffer_size),
      NULL },

    { ngx_string("inspect_flush_time"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, flush_time),
      NULL },

    { ngx_string("inspect_format"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_inspect_format,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, format),
      NULL },

    { ngx_string("inspect_log_per_worker"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flowlens_conf_t, per_worker),
      NULL },

    ngx_null_command
};

/* ===================== Module Context ===================== */

static ngx_http_module_t ngx_http_flowlens_module_ctx = {
    NULL,                           /* preconfiguration */
    ngx_http_flowlens_module_init,     /* postconfiguration */
    ngx_http_flowlens_create_main_conf,/* create main config */
    NULL,                           /* init main config */
    NULL,                           /* create server config */
    NULL,                           /* merge server config */
    ngx_http_flowlens_create_loc_conf, /* create location config */
    ngx_http_flowlens_merge_loc_conf   /* merge location config */
};

/* ===================== Module Definition ===================== */

ngx_module_t ngx_http_flowlens_module = {
    NGX_MODULE_V1,
    &ngx_http_flowlens_module_ctx,
    ngx_http_flowlens_commands,
    NGX_HTTP_MODULE,
    NULL, /* init master */
    NULL, /* init module */
    NULL, /* init process */
    NULL, /* init thread */
    NULL, /* exit thread */
    ngx_http_flowlens_exit_process, /* exit process */
    NULL, /* exit master */
    NGX_MODULE_V1_PADDING
};

/* ===================== Config Functions ===================== */

static char* ngx_conf_set_inspect_format(ngx_conf_t* cf, ngx_command_t* cmd, void* conf) {
    ngx_http_flowlens_conf_t* ilcf = conf;
    ngx_str_t* value = cf->args->elts;

    if (value[1].len == 4 && ngx_strncasecmp(value[1].data, (u_char*)"json", 4) == 0) {
        ilcf->format = 0;
    } else if (value[1].len == 3 && ngx_strncasecmp(value[1].data, (u_char*)"tlv", 3) == 0) {
        ilcf->format = 1;
    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid value \"%V\" in \"%V\" directive, "
                           "must be \"json\" or \"tlv\"",
                           &value[1], &cmd->name);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static void* ngx_http_flowlens_create_main_conf(ngx_conf_t* cf) {
    ngx_http_flowlens_conf_t* conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_flowlens_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->enable = NGX_CONF_UNSET;
    conf->max_body_size = NGX_CONF_UNSET_SIZE;
    conf->dump_enable = NGX_CONF_UNSET;
    conf->dump_min_size = NGX_CONF_UNSET_SIZE;
    conf->buffer_size = NGX_CONF_UNSET_SIZE;
    conf->flush_time = NGX_CONF_UNSET_MSEC;
    conf->format = NGX_CONF_UNSET_UINT;
    conf->per_worker = NGX_CONF_UNSET;

    return conf;
}

static void* ngx_http_flowlens_create_loc_conf(ngx_conf_t* cf) {
    ngx_http_flowlens_conf_t* conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_flowlens_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->enable = NGX_CONF_UNSET;
    conf->max_body_size = NGX_CONF_UNSET_SIZE;
    conf->dump_enable = NGX_CONF_UNSET;
    conf->dump_min_size = NGX_CONF_UNSET_SIZE;
    conf->buffer_size = NGX_CONF_UNSET_SIZE;
    conf->flush_time = NGX_CONF_UNSET_MSEC;
    conf->format = NGX_CONF_UNSET_UINT;
    conf->per_worker = NGX_CONF_UNSET;

    return conf;
}

static char* ngx_http_flowlens_merge_loc_conf(ngx_conf_t* cf, void* parent, void* child) {
    ngx_http_flowlens_conf_t* prev = parent;
    ngx_http_flowlens_conf_t* conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_size_value(conf->max_body_size, prev->max_body_size, 5 * 1024 * 1024);
    ngx_conf_merge_value(conf->dump_enable, prev->dump_enable, 0);
    ngx_conf_merge_size_value(conf->dump_min_size, prev->dump_min_size, 1024);
    ngx_conf_merge_size_value(conf->buffer_size, prev->buffer_size, 0);
    ngx_conf_merge_msec_value(conf->flush_time, prev->flush_time, 0);
    ngx_conf_merge_uint_value(conf->format, prev->format, 1);  /* default tlv */
    ngx_conf_merge_value(conf->per_worker, prev->per_worker, 0);

    if (conf->inspect_log.data == NULL && prev->inspect_log.data != NULL) {
        conf->inspect_log = prev->inspect_log;
    }

    if (conf->dump_path.data == NULL && prev->dump_path.data != NULL) {
        conf->dump_path = prev->dump_path;
    }

    /* Convert relative dump_path to absolute (based on cycle->prefix) */
    if (conf->dump_path.data != NULL && conf->dump_path.len > 0) {
        if (ngx_conf_full_name(cf->cycle, &conf->dump_path, 0) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    /* Register inspect_log with cycle->open_files for fd caching + SIGHUP reopen */
    if (conf->inspect_log.data != NULL && conf->inspect_log.len > 0) {
        conf->log_file = ngx_conf_open_file(cf->cycle, &conf->inspect_log);
        if (conf->log_file == NULL) {
            return NGX_CONF_ERROR;
        }

        /* Allocate per-worker buffer if configured */
        if (conf->buffer_size > 0) {
            ngx_http_flowlens_log_buf_t* buffer;

            if (conf->log_file->data) {
                /* Buffer already allocated for this log file by another location */
                return NGX_CONF_OK;
            }

            buffer = ngx_pcalloc(cf->pool, sizeof(ngx_http_flowlens_log_buf_t));
            if (buffer == NULL) {
                return NGX_CONF_ERROR;
            }

            buffer->start = ngx_pnalloc(cf->pool, conf->buffer_size);
            if (buffer->start == NULL) {
                return NGX_CONF_ERROR;
            }

            buffer->pos = buffer->start;
            buffer->last = buffer->start + conf->buffer_size;
            buffer->is_json = (conf->format == 0);

            if (conf->flush_time > 0) {
                buffer->event = ngx_pcalloc(cf->pool, sizeof(ngx_event_t));
                if (buffer->event == NULL) {
                    return NGX_CONF_ERROR;
                }

                buffer->event->data = conf->log_file;
                buffer->event->handler = trace_inspect_flush_handler;
                buffer->event->log = &cf->cycle->new_log;
                buffer->event->cancelable = 1;
                buffer->flush = conf->flush_time;
            }

            conf->log_file->flush = trace_inspect_flush;
            conf->log_file->data = buffer;
        }
    }

    return NGX_CONF_OK;
}

/* ===================== Module Init ===================== */

static ngx_int_t ngx_http_flowlens_module_init(ngx_conf_t* cf) {
    ngx_http_handler_pt*       h;
    ngx_http_core_main_conf_t* cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_http_flowlens_request_handler;

    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter  = ngx_http_flowlens_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter  = ngx_http_flowlens_body_filter;

    return NGX_OK;
}

/* ===================== Request Handler ===================== */

static ngx_int_t ngx_http_flowlens_request_handler(ngx_http_request_t* r) {
    ngx_http_flowlens_conf_t* conf;
    ngx_http_flowlens_ctx_t*  ctx;
    ngx_int_t              rc;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_flowlens_module);
    if (!conf || !conf->enable) {
        return NGX_OK;
    }

    /* skip subrequests */
    if (r != r->main) {
        return NGX_OK;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_flowlens_module);
    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_flowlens_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }
        ctx->conf = conf;
        ngx_http_set_ctx(r, ctx, ngx_http_flowlens_module);
    }

    if (ctx->req_body_done) {
        return NGX_OK;
    }

    trace_collect_request(r, ctx);

    rc = ngx_http_read_client_request_body(r, ngx_http_flowlens_body_handler);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    ngx_http_finalize_request(r, NGX_DONE);
    return NGX_DONE;
}

static void trace_collect_request(ngx_http_request_t* r, ngx_http_flowlens_ctx_t* ctx) {
    ctx->req_method = r->method_name;
    ctx->req_uri    = r->uri;
    ctx->req_args   = r->args;
    ctx->capture_uri = r->uri;

    ctx->req_headers = trace_format_headers(r->pool, &r->headers_in.headers);
}

/* ===================== Header Filter ===================== */

static ngx_int_t ngx_http_flowlens_header_filter(ngx_http_request_t* r) {
    ngx_http_flowlens_ctx_t* ctx;
    ngx_http_flowlens_conf_t* conf;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_flowlens_module);
    if (!conf || !conf->enable) {
        return ngx_http_next_header_filter(r);
    }

    if (r != r->main) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_flowlens_module);
    if (ctx == NULL) {
        return ngx_http_next_header_filter(r);
    }

    /* Skip 100 Continue — capture only the final response */
    if (r->headers_out.status == NGX_HTTP_CONTINUE) {
        return ngx_http_next_header_filter(r);
    }

    /* Detect internal redirect (e.g., try_files, error_page) and reset response state.
     * try_files/error_page do not change r->uri, so we also check r->internal.
     */
    if (ctx->capture_uri.len != r->uri.len || ctx->capture_uri.data != r->uri.data || r->internal) {
        ctx->header_done = 0;
        ctx->body_done = 0;
        ctx->resp_status = 0;
        ngx_str_null(&ctx->resp_headers);
        ctx->resp_body_buf = NULL;
        ctx->resp_body_len = 0;
        ctx->resp_body_capacity = 0;
        ctx->resp_body_truncated = 0;
        ngx_str_null(&ctx->resp_body_dump_path);
        ctx->capture_uri = r->uri;
    }

    if (ctx->header_done) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "inspect: header_filter called twice for same request, "
                      "uri=\"%V\", status=%ui",
                      &r->uri, r->headers_out.status);
        return ngx_http_next_header_filter(r);
    }
    ctx->header_done = 1;

    ctx->resp_status = r->headers_out.status;
    ctx->resp_headers = trace_format_headers(r->pool, &r->headers_out.headers);

    return ngx_http_next_header_filter(r);
}

/* ===================== Body Filter ===================== */

static ngx_int_t ngx_http_flowlens_body_filter(ngx_http_request_t* r, ngx_chain_t* in) {
    ngx_http_flowlens_ctx_t* ctx;
    ngx_http_flowlens_conf_t* conf;
    ngx_chain_t* cl;
    ngx_flag_t last = 0;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_flowlens_module);
    if (!conf || !conf->enable) {
        return ngx_http_next_body_filter(r, in);
    }

    if (r != r->main) {
        return ngx_http_next_body_filter(r, in);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_flowlens_module);
    if (ctx == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    /* Detect state corruption: body_done set but filter called again on keepalive */
    if (ctx->body_done && r->connection->requests > 1) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "inspect: body_filter called after body_done on "
                      "keepalive connection request #%ui, "
                      "possible state pollution from previous request",
                      r->connection->requests);
    }

    /* Detect internal redirect and reset response body state.
     * try_files/error_page do not change r->uri, so we also check r->internal.
     */
    if (ctx->capture_uri.len != r->uri.len || ctx->capture_uri.data != r->uri.data || r->internal) {
        ctx->body_done = 0;
        ctx->resp_body_buf = NULL;
        ctx->resp_body_len = 0;
        ctx->resp_body_capacity = 0;
        ctx->resp_body_truncated = 0;
        ngx_str_null(&ctx->resp_body_dump_path);
        ctx->capture_uri = r->uri;
    }

    /* collect response body chunks */
    for (cl = in; cl; cl = cl->next) {
        ngx_buf_t* b = cl->buf;

        if (ngx_buf_in_memory(b) && b->pos && b->last && b->last > b->pos) {
            if (trace_append_data(r, &ctx->resp_body_buf, &ctx->resp_body_len,
                                  &ctx->resp_body_capacity, ctx->conf->max_body_size,
                                  b->pos, (size_t)(b->last - b->pos),
                                  &ctx->resp_body_truncated) != NGX_OK)
            {
                ctx->resp_body_truncated = 1;
            }
        }

        if (b->in_file) {
            if (trace_append_file_buf(r, &ctx->resp_body_buf, &ctx->resp_body_len,
                                      &ctx->resp_body_capacity, ctx->conf->max_body_size, b,
                                      &ctx->resp_body_truncated) != NGX_OK)
            {
                ctx->resp_body_truncated = 1;
            }
        }

        if (b->last_buf) {
            last = 1;
        }

        if (b->last_in_chain && !b->last_buf) {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "flowlens: last_in_chain without last_buf, "
                          "possible incomplete response capture, uri=\"%V\"",
                          &r->uri);
        }
    }

    /* pass through to next filter without modification */
    if (last && !ctx->body_done) {
        ctx->body_done = 1;
        trace_send_log(r, ctx);
    }

    return ngx_http_next_body_filter(r, in);
}

static ngx_int_t trace_append_data(ngx_http_request_t* r, u_char** buf, size_t* len,
                                    size_t* capacity, size_t max, u_char* data, size_t data_len,
                                    ngx_flag_t* truncated) {
    size_t need;
    u_char* new_buf;

    if (data_len == 0) {
        return NGX_OK;
    }

    if (*len >= max) {
        if (truncated) {
            *truncated = 1;
        }
        return NGX_OK; /* already at max, drop */
    }

    if (data_len > max - *len) {
        data_len = max - *len;
        if (truncated) {
            *truncated = 1;
        }
    }

    need = *len + data_len;

    if (*buf == NULL) {
        /* First allocation: start small, cap at max */
        *capacity = ngx_min(max, ngx_max((size_t)1024, need));
        *buf = ngx_pnalloc(r->pool, *capacity);
        if (*buf == NULL) {
            return NGX_ERROR;
        }
    } else if (need > *capacity) {
        /* Grow by 2x until need is met, cap at max */
        size_t new_cap = *capacity;
        while (new_cap < need && new_cap < max) {
            new_cap *= 2;
        }
        new_cap = ngx_min(new_cap, max);

        new_buf = ngx_pnalloc(r->pool, new_cap);
        if (new_buf == NULL) {
            return NGX_ERROR;
        }
        ngx_memcpy(new_buf, *buf, *len);
        *buf = new_buf;
        *capacity = new_cap;
    }

    ngx_memcpy(*buf + *len, data, data_len);
    *len += data_len;

    return NGX_OK;
}

static ngx_int_t trace_append_file_buf(ngx_http_request_t* r, u_char** buf, size_t* len,
                                        size_t* capacity, size_t max, ngx_buf_t* b,
                                        ngx_flag_t* truncated) {
    size_t     file_len;
    ssize_t    n;
    u_char*    tmp;

    if (b->file == NULL || b->file->fd == NGX_INVALID_FILE) {
        return NGX_OK;
    }

    if (b->file_last <= b->file_pos) {
        return NGX_OK;
    }

    file_len = (size_t)(b->file_last - b->file_pos);

    if (*len >= max) {
        if (truncated) {
            *truncated = 1;
        }
        return NGX_OK;
    }

    if (file_len > max - *len) {
        file_len = max - *len;
        if (truncated) {
            *truncated = 1;
        }
    }

    tmp = ngx_pnalloc(r->pool, file_len);
    if (tmp == NULL) {
        return NGX_ERROR;
    }

    n = ngx_read_file(b->file, tmp, file_len, b->file_pos);
    if (n < 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                      "inspect: ngx_read_file failed");
        return NGX_ERROR;
    }
    if (n == 0) {
        return NGX_OK;
    }

    size_t need = *len + (size_t)n;

    if (*buf == NULL) {
        *capacity = ngx_min(max, ngx_max((size_t)1024, need));
        *buf = ngx_pnalloc(r->pool, *capacity);
        if (*buf == NULL) {
            return NGX_ERROR;
        }
    } else if (need > *capacity) {
        size_t new_cap = *capacity;
        while (new_cap < need && new_cap < max) {
            new_cap *= 2;
        }
        new_cap = ngx_min(new_cap, max);

        u_char* new_buf = ngx_pnalloc(r->pool, new_cap);
        if (new_buf == NULL) {
            return NGX_ERROR;
        }
        ngx_memcpy(new_buf, *buf, *len);
        *buf = new_buf;
        *capacity = new_cap;
    }

    ngx_memcpy(*buf + *len, tmp, (size_t)n);
    *len += (size_t)n;

    return NGX_OK;
}

static void ngx_http_flowlens_body_handler(ngx_http_request_t* r) {
    ngx_http_flowlens_ctx_t* ctx;
    ngx_chain_t*          cl;
    ngx_buf_t*            b;

    ctx = ngx_http_get_module_ctx(r, ngx_http_flowlens_module);
    if (ctx == NULL) {
        ngx_http_finalize_request(r, NGX_ERROR);
        return;
    }

    if (ctx->req_body_done) {
        /* body_handler may be called twice if ngx_http_read_client_request_body
         * is invoked again (e.g., by a subrequest or internal redirect).
         * Simply return — do NOT finalize the request here. */
        return;
    }
    ctx->req_body_done = 1;

    if (r->request_body && r->request_body->bufs) {
        for (cl = r->request_body->bufs; cl; cl = cl->next) {
            b = cl->buf;
            if (ngx_buf_in_memory(b) && b->pos && b->last && b->last > b->pos) {
                if (trace_append_data(r, &ctx->req_body_buf, &ctx->req_body_len,
                                      &ctx->req_body_capacity, ctx->conf->max_body_size,
                                      b->pos, (size_t)(b->last - b->pos),
                                      &ctx->req_body_truncated) != NGX_OK)
                {
                    ctx->req_body_truncated = 1;
                }
            }
            if (b->in_file) {
                if (trace_append_file_buf(r, &ctx->req_body_buf, &ctx->req_body_len,
                                          &ctx->req_body_capacity, ctx->conf->max_body_size, b,
                                          &ctx->req_body_truncated) != NGX_OK)
                {
                    ctx->req_body_truncated = 1;
                }
            }
        }
    }

    /* Restart the phase engine. request_handler calls
     * ngx_http_finalize_request(r, NGX_DONE) to suspend the outer
     * phase engine; we must manually drive it to continue.
     */
    r->write_event_handler = ngx_http_core_run_phases;
    ngx_http_core_run_phases(r);
}

/* ===================== File Type Detection ===================== */

typedef struct {
    ngx_str_t mime;
    ngx_str_t ext;
} ngx_http_flowlens_mime_ext_t;

static ngx_http_flowlens_mime_ext_t mime_ext_map[] = {
    { ngx_string("application/pdf"),                     ngx_string(".pdf") },
    { ngx_string("application/msword"),                  ngx_string(".doc") },
    { ngx_string("application/vnd.ms-powerpoint"),       ngx_string(".ppt") },
    { ngx_string("application/vnd.ms-excel"),            ngx_string(".xls") },
    { ngx_string("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"), ngx_string(".xlsx") },
    { ngx_string("application/vnd.openxmlformats-officedocument.presentationml.presentation"), ngx_string(".pptx") },
    { ngx_string("application/vnd.openxmlformats-officedocument.wordprocessingml.document"), ngx_string(".docx") },
    { ngx_string("application/zip"),                     ngx_string(".zip") },
    { ngx_string("application/x-rar-compressed"),        ngx_string(".rar") },
    { ngx_string("application/gzip"),                    ngx_string(".tar.gz") },
    { ngx_string("application/x-tar"),                   ngx_string(".tar") },
    { ngx_string("image/jpeg"),                          ngx_string(".jpg") },
    { ngx_string("image/png"),                           ngx_string(".png") },
    { ngx_string("image/gif"),                           ngx_string(".gif") },
    { ngx_string("text/plain"),                          ngx_string(".txt") },
    { ngx_null_string, ngx_null_string }
};

typedef struct {
    u_char* sig;
    size_t  len;
    ngx_str_t ext;
} ngx_http_flowlens_sig_ext_t;

static ngx_http_flowlens_sig_ext_t sig_ext_map[] = {
    { (u_char*)"%PDF",              4,  ngx_string(".pdf") },
    { (u_char*)"\xD0\xCF\x11\xE0",  8,  ngx_string(".doc") },
    { (u_char*)"PK\x03\x04",        4,  ngx_string(".zip") },
    { (u_char*)"Rar!",              4,  ngx_string(".rar") },
    { (u_char*)"\x1F\x8B",         2,  ngx_string(".tar.gz") },
    { (u_char*)"\x89PNG",           4,  ngx_string(".png") },
    { (u_char*)"\xFF\xD8\xFF",     3,  ngx_string(".jpg") },
    { (u_char*)"GIF87a",            6,  ngx_string(".gif") },
    { (u_char*)"GIF89a",            6,  ngx_string(".gif") },
    { NULL, 0, ngx_null_string }
};

static ngx_flag_t trace_str_start_with(ngx_str_t* s, const char* prefix, size_t prefix_len) {
    return s->len >= prefix_len && ngx_strncasecmp(s->data, (u_char*)prefix, prefix_len) == 0;
}

static ngx_flag_t trace_str_contains(ngx_str_t* s, char *substr, size_t substr_len) {
    if (s->len < substr_len) return 0;
    return ngx_strnstr(s->data, substr, s->len) != NULL;
}

static ngx_str_t trace_find_cd_filename_ext(ngx_http_request_t* r, ngx_flag_t is_request) {
    ngx_str_t cd_key = ngx_string("Content-Disposition");
    ngx_list_part_t* part;
    ngx_table_elt_t* header;
    ngx_uint_t j;
    ngx_str_t ext = ngx_null_string;
    u_char* start;
    u_char* end;
    u_char* dot;

    part = is_request ? &r->headers_in.headers.part : &r->headers_out.headers.part;
    header = part->elts;

    for (j = 0; ; j++) {
        if (j >= part->nelts) {
            if (part->next == NULL) break;
            part = part->next;
            header = part->elts;
            j = 0;
        }
        if (header[j].key.len == cd_key.len
            && ngx_strncasecmp(header[j].key.data, cd_key.data, cd_key.len) == 0)
        {
            if (header[j].value.len == 0) break;
            /* Case-insensitive search for "filename=" (RFC 6266 parameter names are case-insensitive).
             * Some servers send "fileName=" instead of "filename=". */
            {
                u_char* p = header[j].value.data;
                size_t  vlen = header[j].value.len;
                size_t  i;
                start = NULL;
                for (i = 0; i + 9 <= vlen; i++) {
                    if ((p[i] == 'f' || p[i] == 'F')
                        && (p[i+1] == 'i' || p[i+1] == 'I')
                        && (p[i+2] == 'l' || p[i+2] == 'L')
                        && (p[i+3] == 'e' || p[i+3] == 'E')
                        && (p[i+4] == 'n' || p[i+4] == 'N')
                        && (p[i+5] == 'a' || p[i+5] == 'A')
                        && (p[i+6] == 'm' || p[i+6] == 'M')
                        && (p[i+7] == 'e' || p[i+7] == 'E')
                        && p[i+8] == '=')
                    {
                        start = p + i;
                        break;
                    }
                }
            }
            if (start == NULL) break;
            start += 9;
            if (*start == '"') start++;
            end = ngx_strlchr(start, header[j].value.data + header[j].value.len, '"');
            if (end == NULL) end = header[j].value.data + header[j].value.len;
            if (end <= start) break;
            dot = ngx_strlchr(start, end, '.');
            if (dot != NULL && end - dot > 1) {
                ext.data = dot;
                ext.len = (size_t)(end - dot);
            }
            break;
        }
    }
    return ext;
}

static ngx_str_t trace_get_uri_ext(ngx_http_request_t* r) {
    ngx_str_t ext = ngx_null_string;
    u_char* dot;
    u_char* qmark;
    size_t path_len;

    if (r->uri.len == 0 || r->uri.data == NULL) return ext;

    qmark = ngx_strlchr(r->uri.data, r->uri.data + r->uri.len, '?');
    path_len = qmark ? (size_t)(qmark - r->uri.data) : r->uri.len;

    dot = ngx_strlchr(r->uri.data, r->uri.data + path_len, '.');
    if (dot != NULL && r->uri.data + path_len - dot > 1) {
        ext.data = dot;
        ext.len = (size_t)(r->uri.data + path_len - dot);
    }
    return ext;
}

static ngx_str_t trace_match_mime_ext(ngx_str_t* content_type) {
    ngx_uint_t i;
    for (i = 0; mime_ext_map[i].mime.len > 0; i++) {
        if (content_type->len == mime_ext_map[i].mime.len
            && ngx_strncasecmp(content_type->data, mime_ext_map[i].mime.data, content_type->len) == 0)
        {
            return mime_ext_map[i].ext;
        }
    }
    return (ngx_str_t){ 0, NULL };
}

static ngx_str_t trace_match_sig_ext(u_char* body, size_t body_len) {
    ngx_uint_t i;
    for (i = 0; sig_ext_map[i].sig != NULL; i++) {
        if (body_len >= sig_ext_map[i].len
            && ngx_memcmp(body, sig_ext_map[i].sig, sig_ext_map[i].len) == 0)
        {
            return sig_ext_map[i].ext;
        }
    }
    return (ngx_str_t){ 0, NULL };
}

static ngx_str_t trace_match_ext_map(ngx_str_t* ext) {
    ngx_uint_t i;
    for (i = 0; mime_ext_map[i].mime.len > 0; i++) {
        if (ext->len == mime_ext_map[i].ext.len
            && ngx_strncasecmp(ext->data, mime_ext_map[i].ext.data, ext->len) == 0)
        {
            return mime_ext_map[i].ext;
        }
    }
    return (ngx_str_t){ 0, NULL };
}

/* Detect file type for request body (upload) or response body (download).
 * Returns file extension (including dot, e.g. ".pdf") if body is a file.
 * Returns empty string if not a file or cannot determine.
 */
static ngx_str_t trace_detect_file_type(ngx_http_request_t* r,
                                        ngx_flag_t is_request,
                                        ngx_str_t* content_type,
                                        u_char* body,
                                        size_t body_len) {
    ngx_str_t ext = ngx_null_string;
    ngx_str_t ct = ngx_null_string;
    size_t semicolon_pos = 0;

    /* Normalize Content-Type: remove everything after ';' */
    if (content_type && content_type->data && content_type->len > 0) {
        for (semicolon_pos = 0; semicolon_pos < content_type->len; semicolon_pos++) {
            if (content_type->data[semicolon_pos] == ';') break;
        }
        ct.data = content_type->data;
        ct.len = semicolon_pos;
    }

    if (is_request) {
        /* Request phase: upload detection */
        /* 1. multipart/form-data or application/octet-stream */
        if (trace_str_contains(&ct, "multipart/form-data", 19)
            || trace_str_contains(&ct, "application/octet-stream", 24))
        {
            ext = trace_get_uri_ext(r);
            return ext.len > 0 ? ext : (ngx_str_t){ sizeof(".bin") - 1, (u_char*)".bin" };
        }

        /* 2. Exact MIME match */
        ext = trace_match_mime_ext(&ct);
        if (ext.len > 0) return ext;

        /* 3. Large body (>1MB) and not a known text type */
        if (body_len > 1024 * 1024
            && !trace_str_start_with(&ct, "text/", 5)
            && !trace_str_contains(&ct, "json", 4)
            && !trace_str_contains(&ct, "xml", 3)
            && !trace_str_contains(&ct, "javascript", 10))
        {
            return (ngx_str_t){ sizeof(".bin") - 1, (u_char*)".bin" };
        }

        return (ngx_str_t){ 0, NULL };
    }

    /* Response phase: download detection */
    /* 1. Content-Disposition: attachment (strong signal) */
    ext = trace_find_cd_filename_ext(r, 0);
    if (ext.len > 0) return ext;

    /* 2. Exact MIME match */
    ext = trace_match_mime_ext(&ct);
    if (ext.len > 0) return ext;

    /* 3. Generic application/ type (excluding json/xml/javascript) */
    if (trace_str_start_with(&ct, "application/", 12)
        && !trace_str_contains(&ct, "json", 4)
        && !trace_str_contains(&ct, "xml", 3)
        && !trace_str_contains(&ct, "javascript", 10))
    {
        ext = trace_get_uri_ext(r);
        return ext.len > 0 ? ext : (ngx_str_t){ sizeof(".bin") - 1, (u_char*)".bin" };
    }

    /* 4. application/octet-stream: try URL extension */
    if (trace_str_contains(&ct, "application/octet-stream", 24)) {
        ext = trace_get_uri_ext(r);
        if (ext.len > 0) {
            ngx_str_t mapped = trace_match_ext_map(&ext);
            return mapped.len > 0 ? mapped : ext;
        }
        return (ngx_str_t){ sizeof(".bin") - 1, (u_char*)".bin" };
    }

    /* 5. Magic bytes in body */
    if (body_len > 0) {
        ext = trace_match_sig_ext(body, body_len);
        if (ext.len > 0) return ext;
    }

    /* 6. Large body (>10KB) with no conclusive type */
    if (body_len > 10 * 1024
        && (ct.len == 0 || trace_str_contains(&ct, "application/octet-stream", 24)))
    {
        return (ngx_str_t){ sizeof(".bin") - 1, (u_char*)".bin" };
    }

    return (ngx_str_t){ 0, NULL };
}

static ngx_str_t trace_generate_filename_auto(ngx_http_request_t* r,
                                              ngx_flag_t is_request,
                                              ngx_str_t* ext) {
    static ngx_atomic_t seq = 0;
    ngx_str_t filename;
    u_char* p;
    ngx_uint_t s;
    ngx_time_t* tp;
    ngx_tm_t tm;

    s = ngx_atomic_fetch_add(&seq, 1);
    tp = ngx_timeofday();
    ngx_gmtime(tp->sec, &tm);

    /* Format: {req|resp}_{seq}_{timestamp}{ext}
     * prefix(4) + _(1) + seq(10) + _(1) + YYYYMMDD_HHMMSS(14) + ext + NUL(1) */
    filename.len = 4 + 1 + 10 + 1 + 14 + ext->len + 1;
    filename.data = ngx_pnalloc(r->pool, filename.len);
    if (filename.data == NULL) {
        filename.len = 0;
        return filename;
    }

    p = ngx_sprintf(filename.data, "%s_%010ui_%04d%02d%02d%02d%02d%02d%V",
                    is_request ? "req" : "resp",
                    s,
                    tm.ngx_tm_year, tm.ngx_tm_mon, tm.ngx_tm_mday,
                    tm.ngx_tm_hour, tm.ngx_tm_min, tm.ngx_tm_sec,
                    ext);
    filename.len = (size_t)(p - filename.data);
    filename.data[filename.len] = '\0';

    return filename;
}

static ngx_flag_t trace_dump_body(ngx_http_request_t* r, ngx_str_t* dump_path, u_char* body, size_t body_len, ngx_str_t* filename, ngx_str_t* out_path) {
    u_char* full_path;
    size_t  path_len;
    ngx_fd_t fd;
    ssize_t n;

    if (dump_path == NULL || dump_path->data == NULL || dump_path->len == 0) {
        return 0;
    }
    if (filename == NULL || filename->data == NULL || filename->len == 0) {
        return 0;
    }

    path_len = dump_path->len + 1 + filename->len;
    full_path = ngx_pnalloc(r->pool, path_len + 1);
    if (full_path == NULL) {
        return 0;
    }

    ngx_snprintf(full_path, path_len + 1, "%V/%V", dump_path, filename);
    full_path[path_len] = '\0';

    fd = ngx_open_file(full_path, NGX_FILE_WRONLY, NGX_FILE_CREATE_OR_OPEN, 0644);
    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                      "inspect: failed to open dump file %s", full_path);
        return 0;
    }

    n = ngx_write_fd(fd, body, body_len);
    ngx_close_file(fd);

    if (n < 0 || (size_t)n != body_len) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                      "inspect: failed to write dump file %s", full_path);
        return 0;
    }

    out_path->data = full_path;
    out_path->len  = path_len;
    return 1;
}

/* ===================== Send Log ===================== */

static void trace_send_log(ngx_http_request_t* r, ngx_http_flowlens_ctx_t* ctx) {
    ngx_http_flowlens_conf_t* conf = ctx->conf;
    u_char*  log_data = NULL;
    size_t   log_len = 0;
    char*    json_str = NULL;
    ngx_fd_t fd = NGX_INVALID_FILE;
    ssize_t  n;

    /* Request body dump */
    if (conf->dump_enable && ctx->req_body_len >= conf->dump_min_size) {
        ngx_str_t req_ct = ngx_null_string;
        if (r->headers_in.content_type) {
            req_ct = r->headers_in.content_type->value;
        }
        ngx_str_t req_ext = trace_detect_file_type(r, 1, &req_ct,
                                                    ctx->req_body_buf,
                                                    ctx->req_body_len);
        if (req_ext.len > 0) {
            ngx_str_t req_filename = trace_generate_filename_auto(r, 1, &req_ext);
            if (req_filename.len > 0) {
                trace_dump_body(r, &conf->dump_path, ctx->req_body_buf,
                                ctx->req_body_len, &req_filename,
                                &ctx->req_body_dump_path);
            }
        }
    }

    /* Response body dump */
    if (conf->dump_enable && ctx->resp_body_len >= conf->dump_min_size) {
        ngx_str_t* resp_ct = &r->headers_out.content_type;
        ngx_str_t resp_ext = trace_detect_file_type(r, 0, resp_ct,
                                                     ctx->resp_body_buf,
                                                     ctx->resp_body_len);
        if (resp_ext.len > 0) {
            ngx_str_t resp_filename = trace_generate_filename_auto(r, 0, &resp_ext);
            if (resp_filename.len > 0) {
                trace_dump_body(r, &conf->dump_path, ctx->resp_body_buf,
                                ctx->resp_body_len, &resp_filename,
                                &ctx->resp_body_dump_path);
            }
        }
    }

    /* Serialize according to format */
    if (conf->format == 0) {
        /* JSON format */
        json_str = inspect_event_to_json_c(
            r,
            &ctx->req_method,
            &ctx->req_uri,
            &ctx->req_args,
            &ctx->req_headers,
            ctx->req_body_buf,
            ctx->req_body_len,
            ctx->req_body_truncated,
            &ctx->req_body_dump_path,
            ctx->resp_status,
            &ctx->resp_headers,
            ctx->resp_body_buf,
            ctx->resp_body_len,
            ctx->resp_body_truncated,
            &ctx->resp_body_dump_path);

        if (json_str == NULL) {
            return;
        }
        log_data = (u_char*)json_str;
        log_len = ngx_strlen(json_str);
    } else {
        /* TLV format (default) */
        log_data = inspect_event_to_tlv(
            r,
            &ctx->req_method,
            &ctx->req_uri,
            &ctx->req_args,
            &ctx->req_headers,
            ctx->req_body_buf,
            ctx->req_body_len,
            ctx->req_body_truncated,
            &ctx->req_body_dump_path,
            ctx->resp_status,
            &ctx->resp_headers,
            ctx->resp_body_buf,
            ctx->resp_body_len,
            ctx->resp_body_truncated,
            &ctx->resp_body_dump_path,
            &log_len);

        if (log_data == NULL) {
            return;
        }
    }

    /* If no inspect_log configured, fall back to error_log */
    if (conf->inspect_log.data == NULL || conf->inspect_log.len == 0) {
        if (conf->format == 0) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "[INSPECT] %s", json_str);
        } else {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "[INSPECT] TLV %uz bytes", log_len);
        }
        return;
    }

    /* Determine fd: per-worker, shared cached, or per-request fallback */
    if (conf->per_worker) {
        if (ngx_http_flowlens_worker_fd == NGX_INVALID_FILE
            || ngx_http_flowlens_worker_path.len != conf->inspect_log.len
            || ngx_memcmp(ngx_http_flowlens_worker_path.data, conf->inspect_log.data,
                          conf->inspect_log.len) != 0)
        {
            /* Close stale fd if path changed (e.g., reload) */
            if (ngx_http_flowlens_worker_fd != NGX_INVALID_FILE) {
                ngx_close_file(ngx_http_flowlens_worker_fd);
                ngx_http_flowlens_worker_fd = NGX_INVALID_FILE;
            }

            u_char* pw_path = ngx_pnalloc(r->pool, conf->inspect_log.len + 1 + NGX_INT64_LEN + 1);
            if (pw_path == NULL) {
                goto fallback_error_log;
            }
            u_char* p = ngx_sprintf(pw_path, "%V.%P", &conf->inspect_log, ngx_pid);
            ngx_str_t pw_path_str;
            pw_path_str.data = pw_path;
            pw_path_str.len = (size_t)(p - pw_path);

            ngx_http_flowlens_worker_fd = ngx_open_file(pw_path_str.data,
                                                        NGX_FILE_APPEND,
                                                        NGX_FILE_CREATE_OR_OPEN,
                                                        0644);
            if (ngx_http_flowlens_worker_fd == NGX_INVALID_FILE) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                              "inspect: failed to open per-worker log %V", &pw_path_str);
                goto fallback_error_log;
            }

            ngx_http_flowlens_worker_path.data = ngx_pnalloc(ngx_cycle->pool, pw_path_str.len);
            if (ngx_http_flowlens_worker_path.data == NULL) {
                ngx_close_file(ngx_http_flowlens_worker_fd);
                ngx_http_flowlens_worker_fd = NGX_INVALID_FILE;
                goto fallback_error_log;
            }
            ngx_memcpy(ngx_http_flowlens_worker_path.data, pw_path_str.data, pw_path_str.len);
            ngx_http_flowlens_worker_path.len = pw_path_str.len;
        }
        fd = ngx_http_flowlens_worker_fd;
    } else if (conf->log_file && conf->log_file->fd != NGX_INVALID_FILE) {
        fd = conf->log_file->fd;
    } else {
        fd = ngx_open_file(conf->inspect_log.data,
                           NGX_FILE_APPEND,
                           NGX_FILE_CREATE_OR_OPEN,
                           0644);
        if (fd == NGX_INVALID_FILE) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                          "inspect: failed to open %V, fallback to error_log", &conf->inspect_log);
            goto fallback_error_log;
        }
    }

    /* Buffered write path (only for shared fd; per-worker uses direct write) */
    if (!conf->per_worker && conf->log_file && conf->log_file->data && conf->buffer_size > 0) {
        ngx_http_flowlens_log_buf_t* buffer = conf->log_file->data;

        if (log_len > (size_t)(buffer->last - buffer->start)) {
            /* Record larger than entire buffer: write directly */
            n = ngx_write_fd(fd, log_data, log_len);
            if (n < 0 || (size_t)n != log_len) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                              "inspect: failed to write to %V",
                              &conf->inspect_log);
                if (conf->format == 0) {
                    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                                  "[INSPECT] %s", json_str);
                } else {
                    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                                  "[INSPECT] TLV %uz bytes", log_len);
                }
            }
            if (conf->log_file == NULL || fd != conf->log_file->fd) {
                ngx_close_file(fd);
            }
            return;
        }

        if (log_len > (size_t)(buffer->last - buffer->pos)) {
            /* Not enough room: flush buffer first */
            trace_inspect_flush(conf->log_file, r->connection->log);
        }

        if (log_len <= (size_t)(buffer->last - buffer->pos)) {
            buffer->pos = ngx_cpymem(buffer->pos, log_data, log_len);

            if (buffer->event && buffer->pos == buffer->start + log_len) {
                ngx_add_timer(buffer->event, buffer->flush);
            }

            return;
        }

        /* Should not reach here after flush, but fallback to direct write */
        n = ngx_write_fd(fd, log_data, log_len);
        if (n < 0 || (size_t)n != log_len) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                          "inspect: failed to write to %V",
                          &conf->inspect_log);
            if (conf->format == 0) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "[INSPECT] %s", json_str);
            } else {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "[INSPECT] TLV %uz bytes", log_len);
            }
        }
        if (conf->log_file == NULL || fd != conf->log_file->fd) {
            ngx_close_file(fd);
        }
        return;
    }

    /* Direct write path (no buffer, or per-worker mode) */
    n = ngx_write_fd(fd, log_data, log_len);
    if (n < 0 || (size_t)n != log_len) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, ngx_errno,
                      "inspect: failed to write to %V, fallback to error_log", &conf->inspect_log);
        goto fallback_error_log;
    }

    /* Only close if we opened per-request (fallback path) */
    if (!conf->per_worker && (conf->log_file == NULL || fd != conf->log_file->fd)) {
        ngx_close_file(fd);
    }
    return;

fallback_error_log:
    if (conf->format == 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "[INSPECT] %s", json_str);
    } else {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "[INSPECT] TLV %uz bytes", log_len);
    }
    if (!conf->per_worker && (conf->log_file == NULL || fd != conf->log_file->fd)
        && fd != NGX_INVALID_FILE)
    {
        ngx_close_file(fd);
    }
}

/* ===================== Buffered Write Flush ===================== */

static void trace_inspect_flush(ngx_open_file_t* file, ngx_log_t* log) {
    size_t                       len, flush_len;
    ssize_t                      n;
    ngx_http_flowlens_log_buf_t*  buffer;
    u_char*                      boundary;

    buffer = file->data;

    len = buffer->pos - buffer->start;

    if (len == 0) {
        return;
    }

    flush_len = len;

    /* For newline-delimited formats (JSON), flush only complete records */
    if (buffer->is_json) {
        boundary = buffer->pos;
        while (boundary > buffer->start && boundary[-1] != '\n') {
            boundary--;
        }
        if (boundary > buffer->start) {
            flush_len = (size_t)(boundary - buffer->start);
        }
        /* If no newline found, flush the whole buffer (single oversized record) */
    }

    n = ngx_write_fd(file->fd, buffer->start, flush_len);
    if (n == -1 || (size_t) n != flush_len) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      ngx_write_fd_n " to \"%s\" failed, fallback to error_log",
                      file->name.data);
        /* Fallback: output buffered audit records to error_log to prevent data loss */
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "[INSPECT] %*s", (int)flush_len, buffer->start);
    }

    if (flush_len < len) {
        /* Move incomplete record to start of buffer */
        size_t remainder = len - flush_len;
        ngx_memmove(buffer->start, buffer->start + flush_len, remainder);
        buffer->pos = buffer->start + remainder;
    } else {
        buffer->pos = buffer->start;
    }

    if (buffer->event && buffer->event->timer_set) {
        ngx_del_timer(buffer->event);
    }
}

static void trace_inspect_flush_handler(ngx_event_t* ev) {
    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "inspect log buffer flush handler");

    trace_inspect_flush(ev->data, ev->log);
}

/* ===================== Process Exit ===================== */

static void ngx_http_flowlens_exit_process(ngx_cycle_t* cycle) {
    ngx_list_part_t*         part;
    ngx_open_file_t*         file;
    ngx_uint_t               i;

    /* Flush any pending buffered audit records */
    part = &cycle->open_files.part;
    file = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            file = part->elts;
            i = 0;
        }

        if (file[i].flush == trace_inspect_flush && file[i].data != NULL) {
            trace_inspect_flush(&file[i], cycle->log);
        }
    }

    /* Close per-worker fd */
    if (ngx_http_flowlens_worker_fd != NGX_INVALID_FILE) {
        ngx_close_file(ngx_http_flowlens_worker_fd);
        ngx_http_flowlens_worker_fd = NGX_INVALID_FILE;
    }
}
