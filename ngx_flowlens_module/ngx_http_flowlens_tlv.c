#include "ngx_http_flowlens_tlv.h"

/* ===================== Constants ===================== */

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#define TLV_HEADER_SIZE 9
#define TLV_FIELD_HDR   5   /* Type(1) + Length(4) */
#define TLV_END_MARKER  1   /* 0xFF */

/* Field type codes */
#define TLV_TIMESTAMP       0x01
#define TLV_SERVER_NAME     0x02
#define TLV_CLIENT_IP       0x03
#define TLV_REQ_METHOD      0x11
#define TLV_REQ_URI         0x12
#define TLV_REQ_ARGS        0x13
#define TLV_REQ_HDR_KEY     0x14
#define TLV_REQ_HDR_VAL     0x15
#define TLV_REQ_BODY        0x16
#define TLV_REQ_DUMP_PATH   0x17
#define TLV_REQ_TRUNCATED   0x18
#define TLV_REQ_BODY_LEN    0x19
#define TLV_RESP_STATUS     0x21
#define TLV_RESP_HDR_KEY    0x24
#define TLV_RESP_HDR_VAL    0x25
#define TLV_RESP_BODY       0x26
#define TLV_RESP_DUMP_PATH  0x27
#define TLV_RESP_TRUNCATED  0x28
#define TLV_RESP_BODY_LEN   0x29
#define TLV_END             0xFF

/* ===================== Helpers ===================== */

static ngx_inline u_char*
tlv_write_header(u_char* p, uint8_t type, uint32_t len)
{
    *p++ = type;
    *p++ = (len >> 24) & 0xFF;
    *p++ = (len >> 16) & 0xFF;
    *p++ = (len >> 8) & 0xFF;
    *p++ = len & 0xFF;
    return p;
}

static ngx_inline u_char*
tlv_write_string(u_char* p, uint8_t type, u_char* data, size_t len)
{
    if (data == NULL || len == 0) {
        return tlv_write_header(p, type, 0);
    }
    p = tlv_write_header(p, type, (uint32_t)len);
    ngx_memcpy(p, data, len);
    return p + len;
}

static ngx_inline u_char*
tlv_write_str(u_char* p, uint8_t type, const ngx_str_t* s)
{
    return tlv_write_string(p, type, s->data, s->len);
}

static ngx_inline u_char*
tlv_write_u8(u_char* p, uint8_t type, uint8_t val)
{
    p = tlv_write_header(p, type, 1);
    *p++ = val;
    return p;
}

static ngx_inline u_char*
tlv_write_u16(u_char* p, uint8_t type, uint16_t val)
{
    p = tlv_write_header(p, type, 2);
    *p++ = (val >> 8) & 0xFF;
    *p++ = val & 0xFF;
    return p;
}

static ngx_inline u_char*
tlv_write_u32(u_char* p, uint8_t type, uint32_t val)
{
    p = tlv_write_header(p, type, 4);
    *p++ = (val >> 24) & 0xFF;
    *p++ = (val >> 16) & 0xFF;
    *p++ = (val >> 8) & 0xFF;
    *p++ = val & 0xFF;
    return p;
}

/* ===================== Base64 (in-place write) ===================== */

static size_t
base64_encoded_size(size_t raw_len)
{
    return ((raw_len + 2) / 3) * 4;
}

static u_char*
base64_encode_write(u_char* dst, const u_char* data, size_t len)
{
    size_t i;
    u_char buf[3];
    u_char* p = dst;

    i = 0;
    while (i < len) {
        size_t n = 0;
        for (; n < 3 && i < len; ++n, ++i) {
            buf[n] = data[i];
        }
        *p++ = base64_chars[buf[0] >> 2];
        if (n > 1) {
            *p++ = base64_chars[((buf[0] & 0x03) << 4) | (buf[1] >> 4)];
            if (n > 2) {
                *p++ = base64_chars[((buf[1] & 0x0F) << 2) | (buf[2] >> 6)];
                *p++ = base64_chars[buf[2] & 0x3F];
            } else {
                *p++ = base64_chars[(buf[1] & 0x0F) << 2];
                *p++ = '=';
            }
        } else {
            *p++ = base64_chars[(buf[0] & 0x03) << 4];
            *p++ = '=';
            *p++ = '=';
        }
    }
    return p;
}

/* ===================== Timestamp ===================== */

static size_t
iso8601_timestamp_len(void)
{
    /* "YYYY-MM-DDTHH:MM:SS.mmmZ\n" = 24 + 1 = 25 bytes */
    return 24;
}

static u_char*
iso8601_timestamp_write(u_char* dst, ngx_http_request_t* r)
{
    ngx_time_t* tp;
    ngx_tm_t    tm;

    tp = ngx_timeofday();
    ngx_gmtime(tp->sec, &tm);

    return ngx_sprintf(dst, "%4d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                       tm.ngx_tm_year, tm.ngx_tm_mon, tm.ngx_tm_mday,
                       tm.ngx_tm_hour, tm.ngx_tm_min, tm.ngx_tm_sec,
                       tp->msec);
}

/* ===================== Headers Parsing ===================== */

/*
 * Parse "key: value\nkey2: value2\n" format and write TLV pairs.
 * Returns pointer past last written byte, or NULL on error.
 */
static u_char*
parse_headers_write(u_char* p, u_char* raw, size_t raw_len,
                    uint8_t key_type, uint8_t val_type)
{
    u_char* end = raw + raw_len;
    u_char* line_start;
    u_char* line_end;
    u_char* colon;
    u_char* val_start;
    size_t  key_len;
    size_t  val_len;

    line_start = raw;
    while (line_start < end) {
        /* Find end of line */
        line_end = line_start;
        while (line_end < end && *line_end != '\n') {
            line_end++;
        }

        /* Find colon */
        colon = line_start;
        while (colon < line_end && *colon != ':') {
            colon++;
        }
        if (colon >= line_end) {
            line_start = line_end + 1;
            continue;
        }

        key_len = (size_t)(colon - line_start);

        /* Skip leading space after colon */
        val_start = colon + 1;
        while (val_start < line_end && *val_start == ' ') {
            val_start++;
        }
        val_len = (size_t)(line_end - val_start);

        if (key_len > 0) {
            p = tlv_write_string(p, key_type, line_start, key_len);
            p = tlv_write_string(p, val_type, val_start, val_len);
        }

        line_start = line_end + 1;
    }

    return p;
}

/* Calculate total size of headers when written as TLV pairs */
static size_t
headers_tlv_size(u_char* raw, size_t raw_len)
{
    u_char* end = raw + raw_len;
    u_char* line_start;
    u_char* line_end;
    u_char* colon;
    u_char* val_start;
    size_t  key_len;
    size_t  val_len;
    size_t  total = 0;

    line_start = raw;
    while (line_start < end) {
        line_end = line_start;
        while (line_end < end && *line_end != '\n') {
            line_end++;
        }

        colon = line_start;
        while (colon < line_end && *colon != ':') {
            colon++;
        }
        if (colon >= line_end) {
            line_start = line_end + 1;
            continue;
        }

        key_len = (size_t)(colon - line_start);

        val_start = colon + 1;
        while (val_start < line_end && *val_start == ' ') {
            val_start++;
        }
        val_len = (size_t)(line_end - val_start);

        if (key_len > 0) {
            total += TLV_FIELD_HDR + key_len;
            total += TLV_FIELD_HDR + val_len;
        }

        line_start = line_end + 1;
    }

    return total;
}

/* ===================== Main Serialization ===================== */

u_char*
inspect_event_to_tlv(ngx_http_request_t* r,
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
                     const ngx_str_t* resp_body_dump_path,
                     size_t* out_len)
{
    size_t   total, ts_len, b64_req_len = 0, b64_resp_len = 0;
    u_char*  buf;
    u_char*  p;
    ngx_str_t empty = ngx_null_string;
    ngx_str_t req_dump = ngx_null_string;
    ngx_str_t resp_dump = ngx_null_string;

    if (req_body_dump_path) {
        req_dump = *req_body_dump_path;
    }
    if (resp_body_dump_path) {
        resp_dump = *resp_body_dump_path;
    }

    /* --- Calculate total size --- */

    total = TLV_HEADER_SIZE;  /* magic + version + record_len */

    /* Top-level fields */
    ts_len = iso8601_timestamp_len();
    total += TLV_FIELD_HDR + ts_len;
    total += TLV_FIELD_HDR + (r->headers_in.server.len ? r->headers_in.server.len : 0);
    total += TLV_FIELD_HDR + (r->connection->addr_text.len ? r->connection->addr_text.len : 0);

    /* Request fields */
    total += TLV_FIELD_HDR + (req_method->len ? req_method->len : 0);
    total += TLV_FIELD_HDR + (req_uri->len ? req_uri->len : 0);
    total += TLV_FIELD_HDR + (req_args->len ? req_args->len : 0);
    total += headers_tlv_size(req_headers->data, req_headers->len);

    if (req_dump.data != NULL && req_dump.len > 0) {
        total += TLV_FIELD_HDR + req_dump.len;  /* dump_path */
    } else if (req_body_len > 0 && req_body_data != NULL) {
        b64_req_len = base64_encoded_size(req_body_len);
        total += TLV_FIELD_HDR + b64_req_len;  /* body base64 */
    } else {
        total += TLV_FIELD_HDR;  /* empty body */
    }
    total += TLV_FIELD_HDR + 1;  /* truncated */
    total += TLV_FIELD_HDR + 4;  /* body_len */

    /* Response fields */
    total += TLV_FIELD_HDR + 2;  /* status */
    total += headers_tlv_size(resp_headers->data, resp_headers->len);

    if (resp_dump.data != NULL && resp_dump.len > 0) {
        total += TLV_FIELD_HDR + resp_dump.len;  /* dump_path */
    } else if (resp_body_len > 0 && resp_body_data != NULL) {
        b64_resp_len = base64_encoded_size(resp_body_len);
        total += TLV_FIELD_HDR + b64_resp_len;  /* body base64 */
    } else {
        total += TLV_FIELD_HDR;  /* empty body */
    }
    total += TLV_FIELD_HDR + 1;  /* truncated */
    total += TLV_FIELD_HDR + 4;  /* body_len */

    total += TLV_END_MARKER;  /* 0xFF */

    /* --- Allocate buffer --- */
    buf = ngx_pnalloc(r->pool, total);
    if (buf == NULL) {
        return NULL;
    }

    /* --- Write record --- */
    p = buf + TLV_HEADER_SIZE;  /* skip header, fill later */

    /* Top-level */
    {
        u_char ts_buf[32];
        iso8601_timestamp_write(ts_buf, r);
        p = tlv_write_string(p, TLV_TIMESTAMP, ts_buf, ts_len);
    }
    p = tlv_write_str(p, TLV_SERVER_NAME,
                      r->headers_in.server.len ? &r->headers_in.server : &empty);
    p = tlv_write_str(p, TLV_CLIENT_IP,
                      r->connection->addr_text.len ? &r->connection->addr_text : &empty);

    /* Request */
    p = tlv_write_str(p, TLV_REQ_METHOD, req_method->len ? req_method : &empty);
    p = tlv_write_str(p, TLV_REQ_URI, req_uri->len ? req_uri : &empty);
    p = tlv_write_str(p, TLV_REQ_ARGS, req_args->len ? req_args : &empty);
    p = parse_headers_write(p, req_headers->data, req_headers->len,
                            TLV_REQ_HDR_KEY, TLV_REQ_HDR_VAL);

    if (req_dump.data != NULL && req_dump.len > 0) {
        p = tlv_write_str(p, TLV_REQ_DUMP_PATH, &req_dump);
    } else if (req_body_len > 0 && req_body_data != NULL) {
        p = tlv_write_header(p, TLV_REQ_BODY, (uint32_t)b64_req_len);
        p = base64_encode_write(p, req_body_data, req_body_len);
    } else {
        p = tlv_write_header(p, TLV_REQ_BODY, 0);
    }
    p = tlv_write_u8(p, TLV_REQ_TRUNCATED, req_body_truncated ? 1 : 0);
    p = tlv_write_u32(p, TLV_REQ_BODY_LEN, (uint32_t)req_body_len);

    /* Response */
    p = tlv_write_u16(p, TLV_RESP_STATUS, (uint16_t)resp_status);
    p = parse_headers_write(p, resp_headers->data, resp_headers->len,
                            TLV_RESP_HDR_KEY, TLV_RESP_HDR_VAL);

    if (resp_dump.data != NULL && resp_dump.len > 0) {
        p = tlv_write_str(p, TLV_RESP_DUMP_PATH, &resp_dump);
    } else if (resp_body_len > 0 && resp_body_data != NULL) {
        p = tlv_write_header(p, TLV_RESP_BODY, (uint32_t)b64_resp_len);
        p = base64_encode_write(p, resp_body_data, resp_body_len);
    } else {
        p = tlv_write_header(p, TLV_RESP_BODY, 0);
    }
    p = tlv_write_u8(p, TLV_RESP_TRUNCATED, resp_body_truncated ? 1 : 0);
    p = tlv_write_u32(p, TLV_RESP_BODY_LEN, (uint32_t)resp_body_len);

    /* End marker */
    *p++ = TLV_END;

    /* --- Fill header --- */
    {
        uint32_t record_len = (uint32_t)(p - buf - TLV_HEADER_SIZE);
        buf[0] = 'I';
        buf[1] = 'N';
        buf[2] = 'S';
        buf[3] = 'P';
        buf[4] = 0x02;  /* version: 4-byte field lengths */
        buf[5] = (record_len >> 24) & 0xFF;
        buf[6] = (record_len >> 16) & 0xFF;
        buf[7] = (record_len >> 8) & 0xFF;
        buf[8] = record_len & 0xFF;
    }

    *out_len = (size_t)(p - buf);
    return buf;
}
