#ifndef NGX_HTTP_FLOWLENS_TLV_H
#define NGX_HTTP_FLOWLENS_TLV_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/*
 * TLV Binary Format Specification
 *
 * Record Header (9 bytes):
 *   [0..3]  Magic: "INSP"
 *   [4]     Version: 0x01
 *   [5..8]  Record Length: uint32 big-endian (total bytes AFTER this field)
 *
 * TLV Fields:
 *   [Type: 1 byte][Length: 2 bytes big-endian][Value: N bytes]
 *
 * Type 0xFF = End of record (no Length/Value follows)
 *
 * Field Types:
 *   0x01  timestamp       string
 *   0x02  server_name     string
 *   0x03  client_ip       string
 *   0x11  req_method      string
 *   0x12  req_uri         string
 *   0x13  req_args        string
 *   0x14  req_header_key  string (followed by 0x15)
 *   0x15  req_header_val  string
 *   0x16  req_body        string (base64)
 *   0x17  req_dump_path   string
 *   0x18  req_truncated   uint8 (0 or 1)
 *   0x19  req_body_len    uint32 big-endian
 *   0x21  resp_status     uint16 big-endian
 *   0x24  resp_header_key string (followed by 0x25)
 *   0x25  resp_header_val string
 *   0x26  resp_body       string (base64)
 *   0x27  resp_dump_path  string
 *   0x28  resp_truncated  uint8 (0 or 1)
 *   0x29  resp_body_len   uint32 big-endian
 */

u_char* inspect_event_to_tlv(ngx_http_request_t* r,
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
                             size_t* out_len);

#endif
