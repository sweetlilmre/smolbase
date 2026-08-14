// See WxHttp.h for the contract.
#include "WxHttp.h"
#include <HTTPClient.h>
#if !SMOLBASE_WEATHER_HTTP
#include <NetworkClientSecure.h>
#include <esp_crt_bundle.h>
#endif

namespace {

#if !SMOLBASE_WEATHER_HTTP
// The 3.3.11 core attaches no CA by default; attach_ssl_certificate_bundle()
// engages the stock IDF bundle baked into libmbedtls. The stock bundle in
// IDF 5.5.5 (July 2026) contains ISRG Root X1 and USERTrust RSA CA —
// the roots our two providers actually use — verified against the linked
// x509_crt_bundle.S.obj before removing the custom bundle (#82).
class BundleClient : public NetworkClientSecure {
public:
  BundleClient() { attach_ssl_certificate_bundle(sslclient.get(), true); _use_ca_bundle = true; }
};
#endif

} // namespace

namespace WxHttp {

Result getJson(const String& url, const JsonDocument& filter, JsonDocument& out) {
  Result res;
  NetworkClient plain;
#if !SMOLBASE_WEATHER_HTTP
  BundleClient tls; // stock IDF bundle (see shim above)
  bool secure = url.startsWith("https");
  NetworkClient& client = secure ? tls : plain;
#else
  bool secure = false;
  NetworkClient& client = plain;
#endif
  HTTPClient http;
  http.setTimeout(10000);
  http.setConnectTimeout(10000);
  if (!http.begin(client, url)) {
    res.code = -100;
    return res;
  }
  res.code = http.GET();
  if (res.code == HTTP_CODE_OK) {
    // getString(), not getStream(): plain-HTTP responses arrive chunked, and
    // the raw stream's chunk-size line ("2000\r\n") parses as a complete JSON
    // number — a silent wrong-answer. Bodies here are ≤2 KB; buffering is
    // cheaper than a chunked-decoding stream wrapper.
    String body = http.getString();
    res.ok = deserializeJson(out, body, DeserializationOption::Filter(filter)) ==
             DeserializationError::Ok && !out.isNull();
    if (!res.ok) res.code = -101;
  } else if (res.code < 0) {
    int n = snprintf(res.err, sizeof(res.err), "%s | ",
                     HTTPClient::errorToString(res.code).c_str());
#if !SMOLBASE_WEATHER_HTTP
    if (secure && n > 0 && n < (int)sizeof(res.err))
      tls.lastError(res.err + n, sizeof(res.err) - n); // mbedTLS's words
#endif
  }
  http.end();
  return res;
}

} // namespace WxHttp
