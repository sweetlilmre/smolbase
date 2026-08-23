// One JSON-over-HTTP(S) transport for the whole firmware (IDF 6 migration,
// phase 4). Replaces four near-identical `BundleClient : NetworkClientSecure`
// subclasses — in WxHttp, CgmFetch, GhUpdate and AssetUpdate — that each
// re-learned the same pitfalls.
//
// Built on esp_http_client, which is what AssetUpdate::fetchTar and
// esp_https_ota already use, so this is consolidation onto the shape half the
// codebase had independently arrived at rather than a new dependency. It is
// also framework-neutral: nothing here changes when Arduino goes away.
//
// Three things this owns so no caller re-learns them:
//
// 1. THE CA BUNDLE. crt_bundle_attach engages the stock IDF bundle baked into
//    libmbedtls, which carries ISRG Root X1 and USERTrust RSA CA — the roots
//    our providers actually use (#82). No per-caller shim.
//
// 2. THE CDN REDIRECT BUFFER. buffer_size_tx defaults to 512 bytes, but
//    GitHub's signed CDN URL is ~1.2 KB, so a REDIRECTED request line does not
//    fit and the request fails with the status stuck at 302. Cost three failed
//    spike runs in phase 0 to rediscover; GhUpdate.cpp and AssetUpdate.cpp
//    already carried the comment.
//
// 3. THE CHUNKED-BODY TRAP, and why it does NOT apply here. Arduino's raw
//    stream handed ArduinoJson the chunk-size line ("2000\r\n"), which parses
//    as a complete JSON number — a silent wrong answer rather than an error
//    (#96), which is why WxHttp buffered via getString(). That trap belongs to
//    Arduino's WiFiClient, not to esp_http_client: this client tracks
//    `is_chunked` itself and esp_http_client_read returns DE-CHUNKED body
//    bytes (verified in esp_http_client.c; AssetUpdate::fetchTar has depended
//    on it in production all along).
//
//    So the response is stream-parsed straight into the filtered document.
//    That matters beyond tidiness: GitHub's release JSON for this repo is
//    tens of KB, and buffering it whole alongside a TLS session would not fit
//    the heap. GhUpdate and AssetUpdate were already streaming for exactly
//    this reason; peak heap is now bounded by the FILTERED doc, not the body.
//
// No scheme fork: esp_http_client takes http:// and https:// URLs the same
// way, so the SMOLBASE_WEATHER_HTTP escape hatch is now purely a URL choice.
//
// Threading: one request at a time, please. The TLS heap peak barely fits as
// it is, and nothing here serializes callers for you.
#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <string>

namespace Http {

struct Header {
  const char* name;
  const char* value;
};

// Negative status codes, keeping WxHttp's established contract.
constexpr int ERR_INIT = -100;  // client init / connect failed
constexpr int ERR_PARSE = -101; // body did not parse through the filter

struct Result {
  bool ok = false;   // HTTP 200 AND the body parsed
  int status = 0;    // HTTP status, or one of the ERR_* codes above
  char err[96] = ""; // transport's words when status < 0
};

struct Request {
  const char* url = nullptr;
  const JsonDocument* filter = nullptr; // optional parse filter
  const Header* headers = nullptr;
  size_t headerCount = 0;
  const char* userAgent = "smolbase-esp32";
  const char* body = nullptr; // non-null => POST with this body
  const char* contentType = "application/json";
  int timeoutMs = 10000;
};

// Performs the request and parses the response into `out`.
Result json(const Request& req, JsonDocument& out);

} // namespace Http
