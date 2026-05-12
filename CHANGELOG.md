# Changelog

## v1.0.0

- Full HTTP request/response audit (headers + body)
- TLV binary output (default, ~9% overhead)
- JSON output (optional, human-readable)
- Body dump to files for large payloads
- HTTP/2 support
- Per-location configuration
- Top body filter registration (guaranteed raw body, before gzip/brotli)
- Subrequest filtering
- Test suite covering static files, proxy, chunked, gzip, HTTP/2
