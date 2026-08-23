// See WxHttp.h for the contract.
//
// Phase 4 of the IDF 6 migration reduced this to an adapter. The transport,
// the CA bundle, the CDN redirect buffer and the chunked-body guarantee all
// live in core/Http now — including the BundleClient shim and the
// NetworkClient/NetworkClientSecure fork this file used to own, which
// esp_http_client makes unnecessary (it takes http:// and https:// URLs the
// same way, so SMOLBASE_WEATHER_HTTP is purely a URL choice via SCHEME).
#include "WxHttp.h"
#include "../core/Http.h"

namespace WxHttp {

Result getJson(const std::string& url, const JsonDocument& filter, JsonDocument& out) {
  Http::Request rq;
  rq.url = url.c_str();
  rq.filter = &filter;
  rq.timeoutMs = 10000;

  Http::Result hr = Http::json(rq, out);

  Result res;
  res.ok = hr.ok;
  res.code = hr.status;
  static_assert(sizeof(res.err) >= sizeof(hr.err), "err buffer shrank");
  memcpy(res.err, hr.err, sizeof(hr.err));
  return res;
}

} // namespace WxHttp
