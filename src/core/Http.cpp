// See Http.h for the contract and the three pitfalls this owns.
#include "Http.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>    // SPIKE: temporary phase-split instrumentation
#include <esp_timer.h>  // SPIKE: temporary phase-split instrumentation
#include <esp_tls.h>    // SPIKE
#include <lwip/netdb.h> // SPIKE
#include <lwip/sockets.h> // SPIKE
#include <freertos/FreeRTOS.h> // SPIKE
#include <freertos/task.h>     // SPIKE

namespace Http {

// SPIKE: identical X.509 benchmark to the one in the arduino-esp32 v0.3.3
// build. See spike_x509.inc.
#define SPIKE_LOG(fmt, ...) ESP_LOGW("spike", fmt, ##__VA_ARGS__)
#define SPIKE_MS() ((unsigned long)(esp_timer_get_time() / 1000))
#define SPIKE_X509_LIB_PROF 1
#include "spike_x509.inc"

// SPIKE: PSA-vs-legacy ECDSA verify, in its own TU. See
// spike_psa_vs_legacy.cpp.
extern "C" void spike_psa_vs_legacy(void);

// SPIKE: split TCP connect from the TLS handshake, mirroring the probe added to
// the arduino-esp32 v0.3.3 build so the two can be compared. A plain TCP
// connect to the same host and port gives the network cost; a full esp_tls
// connection gives network + handshake; the difference is the handshake.
static void spikeTlsOnce(const char* tag) {
  const char* host = "api.github.com";
  esp_tls_cfg_t cfg = {};
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  const int64_t a = esp_timer_get_time();
  esp_tls_t* t = esp_tls_init();
  const int r = t ? esp_tls_conn_new_sync(host, (int)strlen(host), 443, &cfg, t) : -1;
  const int64_t b = esp_timer_get_time();
  if (t) esp_tls_conn_destroy(t);
  ESP_LOGW("spike", "%s=%lld ms ok=%d", tag, (b - a) / 1000, r);
}

static void spikeConnectSplit() {
  const char* host = "api.github.com";
  {
    const int64_t a = esp_timer_get_time();
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int ok = -1;
    if (getaddrinfo(host, "443", &hints, &res) == 0 && res) {
      const int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
      if (fd >= 0) {
        ok = connect(fd, res->ai_addr, res->ai_addrlen);
        close(fd);
      }
      freeaddrinfo(res);
    }
    ESP_LOGW("spike", "tcp_connect=%lld ms ok=%d",
             (esp_timer_get_time() - a) / 1000, ok == 0 ? 1 : 0);
  }
  spikeTlsOnce("tcp+tls_connect");
}

// SPIKE: the same handshake on a task pinned to a chosen core. Elliptic-curve
// maths measured 1.75x slower on core 0 than on core 1 (see
// spike_psa_vs_legacy.cpp), and core 0 is where the WiFi driver lives and where
// the httpd task that serves /api/update/check happens to run. If that carries
// through to a whole handshake, this is a fix and not just a curiosity.
// Run A-B-A in one session: cross-session comparisons are worthless here.
struct SpikeTlsArgs {
  TaskHandle_t waiter;
  const char* tag;
};

static void spikeTlsTask(void* arg) {
  SpikeTlsArgs* a = static_cast<SpikeTlsArgs*>(arg);
  spikeTlsOnce(a->tag);
  xTaskNotifyGive(a->waiter);
  vTaskDelete(nullptr);
}

static void spikeTlsOnCore(int core, const char* tag) {
  SpikeTlsArgs args = {xTaskGetCurrentTaskHandle(), tag};
  if (xTaskCreatePinnedToCore(spikeTlsTask, "spiketls", 16384, &args, 5, nullptr,
                              core) != pdPASS) {
    ESP_LOGW("spike", "[spike-tls] task create failed (core %d)", core);
    return;
  }
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void spikeTlsCoreMatrix() {
  spikeTlsOnCore(0, "tls_core0_a");
  spikeTlsOnCore(1, "tls_core1");
  spikeTlsOnCore(0, "tls_core0_b");
}

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

  spike_x509_bench();                            // SPIKE
  spike_psa_vs_legacy();                         // SPIKE
  spikeConnectSplit();                           // SPIKE
  spikeTlsCoreMatrix();                          // SPIKE
  const int64_t t_spike0 = esp_timer_get_time(); // SPIKE
  esp_err_t err = esp_http_client_open(c, (int)bodyLen);
  const int64_t t_spike1 = esp_timer_get_time(); // SPIKE: connect + TLS handshake
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
  const int64_t t_spike2 = esp_timer_get_time(); // SPIKE: headers
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

  // SPIKE: temporary phase split. Which phase of the request the time is in --
  // connect+TLS, headers, or the JSON body -- so the handshake cost can be
  // compared against what the offline primitive measurements predict.
  const int64_t t_spike3 = esp_timer_get_time();
  ESP_LOGW("spike", "open(connect+tls)=%lld headers=%lld body=%lld total=%lld ms status=%d",
           (t_spike1 - t_spike0) / 1000, (t_spike2 - t_spike1) / 1000,
           (t_spike3 - t_spike2) / 1000, (t_spike3 - t_spike0) / 1000, res.status);

  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return res;
}

} // namespace Http
