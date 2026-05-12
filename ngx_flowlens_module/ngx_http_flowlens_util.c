#include "ngx_http_flowlens_util.h"

ngx_str_t trace_format_headers(ngx_pool_t* pool, ngx_list_t* headers) {
    ngx_str_t result = ngx_null_string;
    ngx_list_part_t* part;
    ngx_table_elt_t* h;
    ngx_uint_t i;
    size_t cap = 1024;
    size_t used = 0;
    u_char* p;
    u_char* new_p;

    if (headers == NULL) {
        return result;
    }

    p = ngx_pnalloc(pool, cap);
    if (p == NULL) {
        return result;
    }

    part = &headers->part;
    h = part->elts;

    for (i = 0;; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].key.len == 0 || h[i].key.data == NULL) {
            continue;
        }

        /* Count newlines in value to determine escape overhead */
        size_t value_nl = 0;
        for (size_t k = 0; k < h[i].value.len; k++) {
            if (h[i].value.data[k] == '\n') value_nl++;
        }

        /* key: value\n */
        size_t need = h[i].key.len + 2 + h[i].value.len + value_nl + 1;

        if (used + need > cap) {
            /* grow by 2x until sufficient */
            while (used + need > cap) {
                cap *= 2;
            }
            new_p = ngx_pnalloc(pool, cap);
            if (new_p == NULL) {
                return result;
            }
            ngx_memcpy(new_p, p, used);
            p = new_p;
        }

        p = ngx_cpymem(p, h[i].key.data, h[i].key.len);
        *p++ = ':';
        *p++ = ' ';
        for (size_t k = 0; k < h[i].value.len; k++) {
            if (h[i].value.data[k] == '\n') {
                *p++ = ' ';
            } else {
                *p++ = h[i].value.data[k];
            }
        }
        *p++ = '\n';
        used += need;
    }

    if (used == 0) {
        return result;
    }

    result.data = p - used;
    result.len = used;
    return result;
}
