// HTTP(S) JSON transport for the weather app (#96): one serial GET →
// buffered body → filtered parse → disconnect. Zero weather knowledge: give
// it a URL, a filter, and a doc.
//
// The pitfalls this file used to own — the stock IDF CA bundle attachment
// (#82) and the chunked-body trap — now live in core/Http, shared with the
// GCM and GitHub-OTA paths that had each re-learned them independently. What
// remains here is the weather app's SCHEME choice and Result shape.
#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include <string>

namespace WxHttp {

// Scheme for URL building, matched to the transport fork. Both providers
// over HTTPS (charter); SMOLBASE_WEATHER_HTTP=1 drops everything to plain
// HTTP — the researched last-resort switch.
#if SMOLBASE_WEATHER_HTTP
constexpr const char* SCHEME = "http://";
#else
constexpr const char* SCHEME = "https://";
#endif

struct Result {
  bool ok = false;   // HTTP 200 AND the body parsed through the filter
  int code = 0;      // HTTP status; -100 = begin/connect failed, -101 = parse failed
  char err[96] = ""; // transport's words for a failed connect (mbedTLS included)
};

// Never two connections at once — the TLS heap peak barely fits as it is.
Result getJson(const std::string& url, const JsonDocument& filter, JsonDocument& out);

} // namespace WxHttp
