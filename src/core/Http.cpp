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
class ClientReader {
  esp_http_client_handle_t _c;

public:
  explicit ClientReader(esp_http_client_handle_t c) : _c(c) {}
  int read() {
    char ch;
    return esp_http_client_read(_c, &ch, 1) == 1 ? (int)(unsigned char)ch : -1;
  }
  size_t readBytes(char* buf, size_t len) {
    int n = esp_http_client_read(_c, buf, (int)len);
    return n > 0 ? (size_t)n : 0;
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
