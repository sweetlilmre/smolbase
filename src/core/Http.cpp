// See Http.h for the contract and the three pitfalls this owns.
#include "Http.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

namespace Http {

namespace {

// ArduinoJson reader over esp_http_client: read() / readBytes() is the whole
// concept ArduinoJson needs. Bytes arrive de-chunked (see Http.h, pitfall 3).
//
// BUFFERED, and that is not a nicety — it restores what the Arduino path had
// for free. ArduinoJson scans a document mostly one byte at a time. The Arduino
// version passed `*http.getStreamPtr()` to deserializeJson, and that Stream was
// an arduino-esp32 NetworkClient, which owns a NetworkClientRxBuffer: a
// heap-allocated 1436-byte (TCP MSS) buffer whose whole purpose is to make
// Stream::read() cheap. Phase 4a replaced the Stream with a raw
// esp_http_client_read and silently dropped that buffer, turning GitHub's ~30 KB
// release JSON into ~30,000 round trips through mbedTLS — measured at 2.07 s of
// the 8.49 s /api/update/check took.
//
// Same size and same placement as NetworkClientRxBuffer on purpose: 1436 is one
// TCP segment, and it lives on the heap because this is constructed on the httpd
// task, whose stack is not the place for it.
class ClientReader {
  static constexpr size_t kBufSize = 1436; // arduino-esp32 NetworkClientRxBuffer
  esp_http_client_handle_t _c;
  char* _buf;
  size_t _pos = 0, _len = 0;

  // True when at least one byte is available in _buf.
  bool fill() {
    if (_pos < _len) return true;
    if (!_buf) return false;
    const int n = esp_http_client_read(_c, _buf, (int)kBufSize);
    if (n <= 0) {
      _pos = _len = 0;
      return false;
    }
    _pos = 0;
    _len = (size_t)n;
    return true;
  }

public:
  explicit ClientReader(esp_http_client_handle_t c)
      : _c(c), _buf(static_cast<char*>(malloc(kBufSize))) {}
  ~ClientReader() { free(_buf); }
  ClientReader(const ClientReader&) = delete;
  ClientReader& operator=(const ClientReader&) = delete;

  // Allocation failure degrades to the unbuffered behaviour rather than losing
  // the response: slow, but a JSON fetch that works beats one that does not.
  int read() {
    if (!_buf) {
      char ch;
      return esp_http_client_read(_c, &ch, 1) == 1 ? (int)(unsigned char)ch : -1;
    }
    if (!fill()) return -1;
    return (int)(unsigned char)_buf[_pos++];
  }
  size_t readBytes(char* dst, size_t len) {
    if (!_buf) {
      const int n = esp_http_client_read(_c, dst, (int)len);
      return n > 0 ? (size_t)n : 0;
    }
    size_t done = 0;
    while (done < len) {
      if (!fill()) break;
      size_t take = _len - _pos;
      if (take > len - done) take = len - done;
      memcpy(dst + done, _buf + _pos, take);
      _pos += take;
      done += take;
    }
    return done;
  }
};

void setErr(Result& r, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r.err, sizeof(r.err), fmt, ap);
  va_end(ap);
}

} // namespace

Result json(const Request& req, JsonDocument& out) {
  Result res;
  if (!req.url) {
    res.status = ERR_INIT;
    setErr(res, "no url");
    return res;
  }

  esp_http_client_config_t cfg = {};
  cfg.url = req.url;
  // Harmless on plain http:// — only the TLS transport consults it.
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = req.timeoutMs;
  cfg.method = req.body ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  // Pitfall 2 (see Http.h): the default 512 B tx buffer cannot hold GitHub's
  // ~1.2 KB signed CDN redirect URL.
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) {
    res.status = ERR_INIT;
    setErr(res, "client init failed");
    return res;
  }

  if (req.userAgent) esp_http_client_set_header(c, "User-Agent", req.userAgent);
  for (size_t i = 0; i < req.headerCount; ++i)
    esp_http_client_set_header(c, req.headers[i].name, req.headers[i].value);

  size_t bodyLen = 0;
  if (req.body) {
    bodyLen = strlen(req.body);
    esp_http_client_set_header(c, "Content-Type", req.contentType);
    esp_http_client_set_post_field(c, req.body, (int)bodyLen);
  }

  esp_err_t err = esp_http_client_open(c, (int)bodyLen);
  if (err != ESP_OK) {
    res.status = ERR_INIT;
    setErr(res, "open: %s (errno %d)", esp_err_to_name(err), esp_http_client_get_errno(c));
    esp_http_client_cleanup(c);
    return res;
  }
  if (bodyLen && esp_http_client_write(c, req.body, (int)bodyLen) < 0) {
    res.status = ERR_INIT;
    setErr(res, "write body failed");
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return res;
  }

  esp_http_client_fetch_headers(c);
  res.status = esp_http_client_get_status_code(c);

  if (res.status == 200) {
    // Pitfall 3 (see Http.h): stream straight into the filtered doc. Safe
    // here because esp_http_client_read returns de-chunked bytes, and
    // necessary because GitHub's release JSON does not fit the heap whole.
    ClientReader reader(c);
    DeserializationError de =
        req.filter ? deserializeJson(out, reader, DeserializationOption::Filter(*req.filter))
                   : deserializeJson(out, reader);
    res.ok = de == DeserializationError::Ok && !out.isNull();
    if (!res.ok) {
      res.status = ERR_PARSE;
      setErr(res, "json: %s", de.c_str());
    }
  }

  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return res;
}

} // namespace Http
