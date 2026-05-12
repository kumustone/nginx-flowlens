#!/usr/bin/env python3
"""
TLV to JSON converter for nginx-flowlens audit logs.

Reads binary TLV records from stdin or file and outputs JSON Lines.

Usage:
    python3 tlv2json.py < inspect.log
    python3 tlv2json.py -i inspect.log -o inspect.jsonl
    python3 tlv2json.py -i inspect.log -o - | jq .
"""

import sys
import struct
import base64
import json
import argparse
from typing import BinaryIO, Optional, Tuple

MAGIC = b"INSP"
VERSION_V1 = 0x01
VERSION_V2 = 0x02
SUPPORTED_VERSIONS = {VERSION_V1, VERSION_V2}

# Field type codes
TLV_TIMESTAMP = 0x01
TLV_SERVER_NAME = 0x02
TLV_CLIENT_IP = 0x03
TLV_REQ_METHOD = 0x11
TLV_REQ_URI = 0x12
TLV_REQ_ARGS = 0x13
TLV_REQ_HDR_KEY = 0x14
TLV_REQ_HDR_VAL = 0x15
TLV_REQ_BODY = 0x16
TLV_REQ_DUMP_PATH = 0x17
TLV_REQ_TRUNCATED = 0x18
TLV_REQ_BODY_LEN = 0x19
TLV_RESP_STATUS = 0x21
TLV_RESP_HDR_KEY = 0x24
TLV_RESP_HDR_VAL = 0x25
TLV_RESP_BODY = 0x26
TLV_RESP_DUMP_PATH = 0x27
TLV_RESP_TRUNCATED = 0x28
TLV_RESP_BODY_LEN = 0x29
TLV_END = 0xFF


def decode_string(data: bytes) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("utf-8", errors="replace")


def decode_base64(data: bytes) -> str:
    try:
        return base64.b64encode(data).decode("ascii")
    except Exception:
        return ""


def read_record(f: BinaryIO) -> Optional[Tuple[bytes, int]]:
    """Read one TLV record from file. Returns None on EOF."""
    header = f.read(9)
    if not header:
        return None
    if len(header) < 9:
        # Truncated header - skip and try next magic
        return None

    magic = header[0:4]
    version = header[4]
    record_len = struct.unpack(">I", header[5:9])[0]

    if magic != MAGIC or version not in SUPPORTED_VERSIONS:
        # Corrupted or not TLV - try to find next magic
        return _resync(f, header)

    body = f.read(record_len)
    if len(body) < record_len:
        # Truncated record
        return None

    return body, version


def _resync(f: BinaryIO, leftover: bytes) -> Optional[Tuple[bytes, int]]:
    """Try to find next valid magic in stream."""
    buf = leftover[1:]  # skip first byte
    while True:
        idx = buf.find(b"I")
        if idx == -1:
            more = f.read(4096)
            if not more:
                return None
            buf = buf + more
            continue

        if idx + 9 > len(buf):
            more = f.read(4096)
            if not more:
                return None
            buf = buf + more
            continue

        if buf[idx:idx + 4] == MAGIC:
            # Found potential magic, verify version
            version = buf[idx + 4]
            if version in SUPPORTED_VERSIONS:
                record_len = struct.unpack(">I", buf[idx + 5:idx + 9])[0]
                if record_len < 64 * 1024 * 1024:  # sanity check: < 64MB
                    body = f.read(record_len)
                    if len(body) == record_len:
                        return body, version
            # Not valid, continue searching after this I
            buf = buf[idx + 1:]
        else:
            buf = buf[idx + 1:]


def parse_record(body: bytes, version: int = VERSION_V2) -> dict:
    """Parse TLV body into JSON-like dict."""
    result = {
        "timestamp": "",
        "server_name": "",
        "client_ip": "",
        "request": {
            "method": "",
            "uri": "",
            "args": "",
            "headers": {},
            "body": "",
            "body_truncated": False,
            "body_len": 0,
        },
        "response": {
            "status": 0,
            "headers": {},
            "body": "",
            "body_truncated": False,
            "body_len": 0,
        },
    }

    i = 0
    pending_hdr_key = None
    req_hdrs = {}
    resp_hdrs = {}
    req_body_raw = b""
    resp_body_raw = b""

    field_hdr_size = 3 if version == VERSION_V1 else 5
    len_unpack = ">H" if version == VERSION_V1 else ">I"
    len_slice = 2 if version == VERSION_V1 else 4

    while i < len(body):
        ftype = body[i]
        if ftype == TLV_END:
            break
        if i + field_hdr_size > len(body):
            break

        flen = struct.unpack(len_unpack, body[i + 1:i + 1 + len_slice])[0]
        i += field_hdr_size

        if i + flen > len(body):
            break

        value = body[i:i + flen]
        i += flen

        if ftype == TLV_TIMESTAMP:
            result["timestamp"] = decode_string(value)
        elif ftype == TLV_SERVER_NAME:
            result["server_name"] = decode_string(value)
        elif ftype == TLV_CLIENT_IP:
            result["client_ip"] = decode_string(value)
        elif ftype == TLV_REQ_METHOD:
            result["request"]["method"] = decode_string(value)
        elif ftype == TLV_REQ_URI:
            result["request"]["uri"] = decode_string(value)
        elif ftype == TLV_REQ_ARGS:
            result["request"]["args"] = decode_string(value)
        elif ftype == TLV_REQ_HDR_KEY:
            pending_hdr_key = decode_string(value)
        elif ftype == TLV_REQ_HDR_VAL:
            if pending_hdr_key is not None:
                req_hdrs[pending_hdr_key] = decode_string(value)
                pending_hdr_key = None
        elif ftype == TLV_REQ_BODY:
            req_body_raw = value
        elif ftype == TLV_REQ_DUMP_PATH:
            result["request"]["dump_path"] = decode_string(value)
        elif ftype == TLV_REQ_TRUNCATED:
            result["request"]["body_truncated"] = bool(value[0]) if value else False
        elif ftype == TLV_REQ_BODY_LEN:
            result["request"]["body_len"] = struct.unpack(">I", value)[0] if len(value) >= 4 else 0
        elif ftype == TLV_RESP_STATUS:
            result["response"]["status"] = struct.unpack(">H", value)[0] if len(value) >= 2 else 0
        elif ftype == TLV_RESP_HDR_KEY:
            pending_hdr_key = decode_string(value)
        elif ftype == TLV_RESP_HDR_VAL:
            if pending_hdr_key is not None:
                resp_hdrs[pending_hdr_key] = decode_string(value)
                pending_hdr_key = None
        elif ftype == TLV_RESP_BODY:
            resp_body_raw = value
        elif ftype == TLV_RESP_DUMP_PATH:
            result["response"]["dump_path"] = decode_string(value)
        elif ftype == TLV_RESP_TRUNCATED:
            result["response"]["body_truncated"] = bool(value[0]) if value else False
        elif ftype == TLV_RESP_BODY_LEN:
            result["response"]["body_len"] = struct.unpack(">I", value)[0] if len(value) >= 4 else 0

    result["request"]["headers"] = req_hdrs
    result["response"]["headers"] = resp_hdrs

    # Decode base64 bodies to match original JSON format
    if req_body_raw:
        try:
            result["request"]["body"] = base64.b64encode(req_body_raw).decode("ascii")
        except Exception:
            result["request"]["body"] = ""

    if resp_body_raw:
        try:
            result["response"]["body"] = base64.b64encode(resp_body_raw).decode("ascii")
        except Exception:
            result["response"]["body"] = ""

    return result


def convert(input_file: BinaryIO, output_file: BinaryIO) -> int:
    count = 0
    while True:
        result = read_record(input_file)
        if result is None:
            break
        body, version = result
        try:
            record = parse_record(body, version)
            output_file.write(json.dumps(record, ensure_ascii=False).encode("utf-8"))
            output_file.write(b"\n")
            count += 1
        except Exception as e:
            print(f"Error parsing record: {e}", file=sys.stderr)
    return count


def main():
    parser = argparse.ArgumentParser(
        description="Convert nginx-flowlens TLV audit logs to JSON Lines"
    )
    parser.add_argument(
        "-i", "--input",
        default="-",
        help="Input TLV file (default: stdin)"
    )
    parser.add_argument(
        "-o", "--output",
        default="-",
        help="Output JSON Lines file (default: stdout)"
    )
    args = parser.parse_args()

    infile = sys.stdin.buffer if args.input == "-" else open(args.input, "rb")
    outfile = sys.stdout.buffer if args.output == "-" else open(args.output, "wb")

    try:
        count = convert(infile, outfile)
        print(f"Converted {count} records", file=sys.stderr)
    finally:
        if infile is not sys.stdin.buffer:
            infile.close()
        if outfile is not sys.stdout.buffer:
            outfile.close()


if __name__ == "__main__":
    main()
