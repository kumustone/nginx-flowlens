#include "ngx_http_flowlens_json.h"
#include "third_party/json.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

/* ===================== Minimal Base64 ===================== */

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const unsigned char* data, size_t len) {
    std::string encoded;
    encoded.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    unsigned char buf[3];
    while (i < len) {
        size_t n = 0;
        for (; n < 3 && i < len; ++n, ++i) {
            buf[n] = data[i];
        }
        encoded.push_back(base64_chars[buf[0] >> 2]);
        if (n > 1) {
            encoded.push_back(base64_chars[((buf[0] & 0x03) << 4) | (buf[1] >> 4)]);
            if (n > 2) {
                encoded.push_back(base64_chars[((buf[1] & 0x0F) << 2) | (buf[2] >> 6)]);
                encoded.push_back(base64_chars[buf[2] & 0x3F]);
            } else {
                encoded.push_back(base64_chars[(buf[1] & 0x0F) << 2]);
                encoded.push_back('=');
            }
        } else {
            encoded.push_back(base64_chars[(buf[0] & 0x03) << 4]);
            encoded.push_back('=');
            encoded.push_back('=');
        }
    }
    return encoded;
}

/* ===================== Helpers ===================== */

static std::string ngx_str_to_std(const ngx_str_t& s) {
    if (s.data == NULL || s.len == 0) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(s.data), s.len);
}

static std::string ngx_ptr_to_std(u_char* data, size_t len) {
    if (data == NULL || len == 0) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(data), len);
}

static std::string iso8601_now() {
    std::time_t now = std::time(nullptr);
    std::tm     gmt;
    if (gmtime_r(&now, &gmt) == nullptr) {
        return "";
    }
    std::ostringstream oss;
    oss << std::put_time(&gmt, "%Y-%m-%dT%H:%M:%S");
    /* append milliseconds */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_nsec = 0;
    }
    oss << '.' << std::setfill('0') << std::setw(3) << (ts.tv_nsec / 1000000) << 'Z';
    return oss.str();
}

static std::string client_ip(ngx_http_request_t* r) {
    if (r->connection && r->connection->addr_text.data && r->connection->addr_text.len) {
        return ngx_ptr_to_std(r->connection->addr_text.data, r->connection->addr_text.len);
    }
    return "";
}

static std::string server_name(ngx_http_request_t* r) {
    if (r->headers_in.server.data && r->headers_in.server.len) {
        return ngx_ptr_to_std(r->headers_in.server.data, r->headers_in.server.len);
    }
    return "";
}

/* Parse "key: value\n" into JSON object */
static json parse_headers(const std::string& raw) {
    json obj = json::object();
    if (raw.empty()) {
        return obj;
    }
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) {
            nl = raw.size();
        }
        std::string line = raw.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) {
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key   = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        /* trim leading space after colon */
        size_t vstart = value.find_first_not_of(' ');
        if (vstart != std::string::npos) {
            value = value.substr(vstart);
        }
        obj[key] = value;
    }
    return obj;
}

/* ===================== Main ===================== */

std::string inspect_event_to_json(ngx_http_request_t* r,
                                const ngx_str_t& req_method,
                                const ngx_str_t& req_uri,
                                const ngx_str_t& req_args,
                                const ngx_str_t& req_headers,
                                u_char* req_body_data,
                                size_t req_body_len,
                                ngx_flag_t req_body_truncated,
                                const ngx_str_t& req_body_dump_path,
                                ngx_uint_t resp_status,
                                const ngx_str_t& resp_headers,
                                u_char* resp_body_data,
                                size_t resp_body_len,
                                ngx_flag_t resp_body_truncated,
                                const ngx_str_t& resp_body_dump_path) {
    json j;
    j["timestamp"]   = iso8601_now();
    j["server_name"] = server_name(r);
    j["client_ip"]   = client_ip(r);

    json req = json::object();
    req["method"]  = ngx_str_to_std(req_method);
    req["uri"]     = ngx_str_to_std(req_uri);
    req["args"]    = ngx_str_to_std(req_args);
    req["headers"] = parse_headers(ngx_str_to_std(req_headers));

    if (req_body_dump_path.data != NULL && req_body_dump_path.len > 0) {
        req["dump_path"]     = ngx_str_to_std(req_body_dump_path);
        req["body_truncated"] = req_body_truncated ? true : false;
        req["body_len"]      = req_body_len;
    } else if (req_body_len > 0 && req_body_data != NULL) {
        req["body"]          = base64_encode(req_body_data, req_body_len);
        req["body_truncated"] = req_body_truncated ? true : false;
        req["body_len"]      = req_body_len;
    } else {
        req["body"]          = "";
        req["body_truncated"] = req_body_truncated ? true : false;
        req["body_len"]      = 0;
    }
    j["request"]   = req;

    json resp = json::object();
    resp["status"]  = static_cast<int>(resp_status);
    resp["headers"] = parse_headers(ngx_str_to_std(resp_headers));

    if (resp_body_dump_path.data != NULL && resp_body_dump_path.len > 0) {
        resp["dump_path"]     = ngx_str_to_std(resp_body_dump_path);
        resp["body_truncated"] = resp_body_truncated ? true : false;
        resp["body_len"]      = resp_body_len;
    } else if (resp_body_len > 0 && resp_body_data != NULL) {
        resp["body"]          = base64_encode(resp_body_data, resp_body_len);
        resp["body_truncated"] = resp_body_truncated ? true : false;
        resp["body_len"]      = resp_body_len;
    } else {
        resp["body"]          = "";
        resp["body_truncated"] = resp_body_truncated ? true : false;
        resp["body_len"]      = 0;
    }
    j["response"]   = resp;

    return j.dump();
}

/* ===================== C Compatible Wrapper ===================== */

extern "C" char* inspect_event_to_json_c(ngx_http_request_t* r,
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
                                        const ngx_str_t* resp_body_dump_path) {
    ngx_str_t req_dump = ngx_null_string;
    ngx_str_t resp_dump = ngx_null_string;
    if (req_body_dump_path) {
        req_dump = *req_body_dump_path;
    }
    if (resp_body_dump_path) {
        resp_dump = *resp_body_dump_path;
    }

    std::string json_str = inspect_event_to_json(
        r, *req_method, *req_uri, *req_args, *req_headers,
        req_body_data, req_body_len, req_body_truncated,
        req_dump,
        resp_status, *resp_headers, resp_body_data, resp_body_len, resp_body_truncated,
        resp_dump);

    if (json_str.empty()) {
        return NULL;
    }

    /* Append newline for atomic single-write in multi-worker setups */
    json_str.push_back('\n');

    char* buf = (char*) ngx_pnalloc(r->pool, json_str.length() + 1);
    if (buf == NULL) {
        return NULL;
    }

    ngx_memcpy(buf, json_str.c_str(), json_str.length() + 1);
    return buf;
}
