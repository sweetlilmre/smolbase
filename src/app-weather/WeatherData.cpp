// See WeatherData.h for the contract. Provider logic (parity with SmolTV-Pro,
// ticket #70): OWM current-weather only when the Secret Store holds a key —
// city as ?q= name or, when the setting's first char is a digit, ?id= OWM id —
// falling back to keyless Open-Meteo on ANY OWM failure (non-200 or parse).
// Open-Meteo needs lat/lon: names go through its keyless geocoder once per
// distinct city value, cached to the Config Store; a numeric OWM id can't be
// geocoded, so keyless + id-city yields no data (as in the original).
// On total failure nothing is promoted: the previous reading stays.
#include "WeatherData.h"
#include "../core/ConfigStore.h"
#include "../core/Net.h"
#include "../core/Platform.h"
#include "../core/Secrets.h"
#include "WeatherStatus.h"
#include "WeatherKeys.h"
#include "WxHttp.h"
#include <ArduinoJson.h>

namespace {

// Geocode cache keys (deliberately unregistered Config Store keys — machine
// state, not user settings). wx_geo_for remembers WHICH city value the cached
// lat/lon belong to. The failed-attempt latch is RAM-only: a typo isn't
// re-queried this boot, but a reboot retries — cheap self-healing.
constexpr const char* K_GEO_FOR  = "wx_geo_for";
constexpr const char* K_GEO_LAT  = "wx_lat";
constexpr const char* K_GEO_LON  = "wx_lon";
constexpr const char* K_GEO_NAME = "wx_geo_name";
constexpr const char* K_GEO_CC   = "wx_geo_cc";
// Registered keys and their defaults come from WeatherKeys.h (#98).
std::string geoTriedFor;
// The city value as of the last settings pass: onSettingsChanged compares
// against this — ONLY a city change refetches (#68 Q4); every other save
// re-renders from cache with no network traffic.
std::string lastCity;

// ---- cross-task handoff -----------------------------------------------------
// The fetch task writes `pending` under `ctx.mux` and flips pendingReady;
// loop() promotes it into `current`. ctx.args go the other way: loop()
// fills them before notifying, so the task never touches ConfigStore/Secrets
// (both are mutex-guarded and fine, but keeping ALL policy reads on core 1
// keeps this file honest about where decisions happen).
//
// FetchContext names the two halves of the protocol:
//   args: main→task, written before xTaskNotifyGive (the notify is the barrier)
//   geo:  geocode/harvested coords, task→main, written ONLY under mux at the
//         end-of-cycle promotion. (Mid-cycle the task threads its own local
//         Geo through the providers by parameter — #96.)
struct FetchContext {
  struct Args {
    char city[64];     // raw setting value (name or numeric OWM id)
    char key[72];      // OWM key, "" = keyless
    char lat[12], lon[12]; // cached coords, "" = unknown
    // The geocoder's canonical display name and country code, loaded from the
    // same cache as lat/lon. Without these a warm cache lost them: K_GEO_NAME
    // and K_GEO_CC were persisted and never read back, so every cycle that did
    // not re-geocode (i.e. all of them once the cache is warm, including after
    // every reboot) drew the city with no country beside it.
    char name[32], cc[4];
    bool geocode;      // this city value still needs a name→lat/lon attempt
  } args;
  struct Geo {
    bool fresh = false;
    bool retryGeocode = false; // attempt never completed: don't keep the latch
    char lat[12], lon[12], name[32], cc[4];
  } geo;
  // Guards the end-of-cycle handoff: pending, pendingReady, and geo.
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
FetchContext ctx;
WeatherData::Reading current, pending;
volatile bool pendingReady = false;
bool changedFlag = false;

TaskHandle_t fetchTask = nullptr;
uint32_t lastFetchMs = 0;
bool fetchDue = true; // fetch immediately on entry
bool fetchInFlight = false;
// Fetch-window hooks (#94): set once by begin(), fired by loop() — begin
// right before arming the task, end when the cycle's result is promoted.
std::function<void()> onFetchBegin, onFetchEnd;

// Fetch diagnostics for /api/debug/weather: last cycle's per-stage HTTP
// codes (0 = not run, -100 = begin/connect fail, -101 = parse fail), plus
// the TLS layer's own words for the last failed connect.
volatile int statGeoCode = 0, statOwmCode = 0, statMeteoCode = 0;
volatile uint32_t statAttempts = 0, statSuccesses = 0;
char statLastErr[96] = "";

// ---- helpers (task side) ----------------------------------------------------

std::string urlEncode(const char* s) {
  std::string out;
  for (const char* p = s; *p; ++p) {
    if (isalnum((unsigned char)*p)) out += *p;
    else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", (unsigned char)*p);
      out += b;
    }
  }
  return out;
}

// Transport + diagnostics shim: WxHttp owns the GET (#96); this records the
// stage code and, on a failed connect, the transport's words — both for
// /api/debug/weather.
bool getJson(const std::string& url, const JsonDocument& filter, JsonDocument& out,
             volatile int& code) {
  WxHttp::Result res = WxHttp::getJson(url, filter, out);
  code = res.code;
  if (res.code < 0 && res.err[0]) {
    // Under mux (#99): the debug route reads statLastErr from core 0.
    taskENTER_CRITICAL(&ctx.mux);
    strlcpy(statLastErr, res.err, sizeof(statLastErr));
    taskEXIT_CRITICAL(&ctx.mux);
  }
  return res.ok;
}

// WMO weathercode → the 9 OWM icon-prefix artworks + local condition text
// (SmolTV-Pro's exact mapping): one table so icon and text can't drift apart.
// First matching [lo, hi] range wins.
struct WmoMap {
  uint8_t lo, hi, icon;
  const char* text;
};
constexpr WmoMap WMO_MAP[] = {
    {0, 0, 1, "Clear"},        {1, 2, 2, "Partly Cloudy"}, {3, 3, 4, "Overcast"},
    {45, 48, 50, "Fog"},       {51, 57, 9, "Drizzle"},     {61, 67, 10, "Rain"},
    {71, 77, 13, "Snow"},      {80, 82, 9, "Showers"},     {85, 86, 13, "Snow Showers"},
    {95, 255, 11, "Thunderstorm"},
};
const WmoMap& wmo(int c) {
  for (const WmoMap& m : WMO_MAP)
    if (c >= m.lo && c <= m.hi) return m;
  return WMO_MAP[0]; // unknown → clear (parity fallback)
}

bool geocode(const FetchContext::Args& args, FetchContext::Geo& g) {
  JsonDocument filter;
  filter["results"][0]["latitude"] = true;
  filter["results"][0]["longitude"] = true;
  filter["results"][0]["name"] = true;
  filter["results"][0]["country_code"] = true;
  JsonDocument doc;
  std::string url = std::string(WxHttp::SCHEME) + "geocoding-api.open-meteo.com/v1/search?name=" +
               urlEncode(args.city) + "&count=1&language=en&format=json";
  if (!getJson(url, filter, doc, statGeoCode) || doc["results"][0].isNull()) return false;
  snprintf(g.lat, sizeof(g.lat), "%.4f", doc["results"][0]["latitude"].as<double>());
  snprintf(g.lon, sizeof(g.lon), "%.4f", doc["results"][0]["longitude"].as<double>());
  strlcpy(g.name, doc["results"][0]["name"] | "", sizeof(g.name));
  strlcpy(g.cc, doc["results"][0]["country_code"] | "", sizeof(g.cc));
  g.fresh = true;
  return true;
}

bool fetchOwm(const FetchContext::Args& args, WeatherData::Reading& r, FetchContext::Geo& g) {
  JsonDocument filter;
  filter["main"] = true;
  filter["wind"]["speed"] = true;
  filter["weather"][0] = true;
  filter["sys"]["country"] = true;
  filter["coord"] = true;
  filter["name"] = true;
  JsonDocument doc;
  bool byId = isdigit((unsigned char)args.city[0]);
  std::string url = std::string(WxHttp::SCHEME) + "api.openweathermap.org/data/2.5/weather?" +
               (byId ? "id=" : "q=") + urlEncode(args.city) +
               "&appid=" + args.key + "&units=metric&lang=en";
  if (!getJson(url, filter, doc, statOwmCode) || doc["main"].isNull()) return false;
  r.tempC = doc["main"]["temp"] | 0.0f;
  r.tempMinC = doc["main"]["temp_min"] | 0.0f;
  r.tempMaxC = doc["main"]["temp_max"] | 0.0f;
  r.feelsC = doc["main"]["feels_like"] | 0.0f;
  r.humidity = doc["main"]["humidity"] | 0;
  r.pressureHpa = doc["main"]["pressure"] | 0;
  r.windMs = doc["wind"]["speed"] | 0.0f;
  strlcpy(r.condition, doc["weather"][0]["main"] | "", sizeof(r.condition));
  // OWM icon codes are "<NN><d|n>" — "01d", "10n" — and the sprite table keys on
  // the two leading digits. This is Arduino's String(..).substring(0,2).toInt():
  // a non-numeric or short value yields 0, exactly as toInt() did.
  const char* icon = doc["weather"][0]["icon"] | "01";
  // Short-circuit: icon[1] is only read once icon[0] is known to be a digit, so
  // a one-character or empty value cannot run past the terminator.
  const bool twoDigits =
      icon[0] >= '0' && icon[0] <= '9' && icon[1] >= '0' && icon[1] <= '9';
  r.iconCode = twoDigits ? (uint8_t)((icon[0] - '0') * 10 + (icon[1] - '0')) : 0;
  strlcpy(r.city, doc["name"] | args.city, sizeof(r.city));
  strlcpy(r.country, doc["sys"]["country"] | "", sizeof(r.country));
  r.keyless = false;
  r.valid = true;
  // Harvest coords: OWM answers for an id-city give the keyless path its
  // lat/lon for the day the key dies (the geocoder can't resolve an id).
  if (!args.lat[0] && !doc["coord"]["lat"].isNull()) {
    snprintf(g.lat, sizeof(g.lat), "%.4f", doc["coord"]["lat"].as<double>());
    snprintf(g.lon, sizeof(g.lon), "%.4f", doc["coord"]["lon"].as<double>());
    strlcpy(g.name, r.city, sizeof(g.name));
    strlcpy(g.cc, r.country, sizeof(g.cc));
    g.fresh = true;
  }
  return true;
}

bool fetchOpenMeteo(const FetchContext::Args& args, const FetchContext::Geo& geo,
                    WeatherData::Reading& r) {
  const char* lat = args.lat[0] ? args.lat : (geo.fresh ? geo.lat : "");
  const char* lon = args.lon[0] ? args.lon : (geo.fresh ? geo.lon : "");
  if (!lat[0]) return false; // id-city, keyless, no cached coords: no data
  JsonDocument filter;
  filter["current_weather"] = true;
  filter["daily"]["temperature_2m_max"][0] = true;
  filter["daily"]["temperature_2m_min"][0] = true;
  JsonDocument doc;
  std::string url = std::string(WxHttp::SCHEME) + "api.open-meteo.com/v1/forecast?latitude=" + lat +
               "&longitude=" + lon +
               "&current_weather=true&daily=temperature_2m_max,temperature_2m_min,weathercode"
               "&forecast_days=1&timezone=auto";
  if (!getJson(url, filter, doc, statMeteoCode) || doc["current_weather"].isNull()) return false;
  r.tempC = doc["current_weather"]["temperature"] | 0.0f;
  r.windMs = (doc["current_weather"]["windspeed"] | 0.0f) / 3.6f; // km/h → m/s
  const WmoMap& m = wmo(doc["current_weather"]["weathercode"] | 0);
  r.iconCode = m.icon;
  strlcpy(r.condition, m.text, sizeof(r.condition));
  r.tempMaxC = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  r.tempMinC = doc["daily"]["temperature_2m_min"][0] | 0.0f;
  r.feelsC = 0; // free tier has none — the three zeros are the keyless tell
  r.humidity = 0;
  r.pressureHpa = 0;
  // Fresh geocode wins, then the cache, then the raw setting value.
  const char* dispName = geo.fresh && geo.name[0] ? geo.name
                       : (args.name[0] ? args.name : args.city);
  const char* dispCc = geo.fresh && geo.cc[0] ? geo.cc : args.cc;
  strlcpy(r.city, dispName, sizeof(r.city));
  strlcpy(r.country, dispCc, sizeof(r.country));
  r.keyless = true;
  r.valid = true;
  return true;
}

void fetchTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    statAttempts = statAttempts + 1;
    statGeoCode = 0; statOwmCode = 0; statMeteoCode = 0; // 0 = stage not run this cycle
    WeatherData::Reading r;
    FetchContext::Geo g = {};
    if (ctx.args.geocode) geocode(ctx.args, g);
    bool ok = (ctx.args.key[0] && fetchOwm(ctx.args, r, g)) || fetchOpenMeteo(ctx.args, g, r);
    if (ok) statSuccesses = statSuccesses + 1;
    // A geocode that never completed (network-level failure: begin/connect,
    // not an HTTP verdict) must not burn the one-attempt-per-city latch —
    // the boot race showed a pre-WiFi cycle latching Durban forever.
    bool geoRetryable = ctx.args.geocode && statGeoCode <= 0 && statGeoCode != -101;
    taskENTER_CRITICAL(&ctx.mux);
    if (ok) pending = r;
    ctx.geo = g;
    ctx.geo.retryGeocode = geoRetryable;
    pendingReady = true; // even on failure: loop() must clear fetchInFlight
    taskEXIT_CRITICAL(&ctx.mux);
  }
}

} // namespace

namespace WeatherData {

void begin(std::function<void()> fetchBegin, std::function<void()> fetchEnd) {
  onFetchBegin = fetchBegin;
  onFetchEnd = fetchEnd;
  geoTriedFor = "";
  lastCity = ConfigStore::getString(WxKeys::CITY, WxKeys::DEF_CITY);
  // 10 KB stack: full handshakes completed on 12 KB, the high-water probe
  // never saw more than ~5 KB used, and every KB parked here is heap the
  // ~49 KB TLS peak (measured: heapMinEver 208 B) cannot use.
  xTaskCreate(fetchTaskFn, "wx_fetch", 10240, nullptr, 1, &fetchTask);
}

void loop() {
  // Promote a finished fetch.
  if (pendingReady) {
    taskENTER_CRITICAL(&ctx.mux);
    pendingReady = false;
    bool ok = pending.valid;
    if (ok) {
      current = pending;
      pending = Reading{};
    }
    FetchContext::Geo g = ctx.geo;
    ctx.geo.fresh = false;
    taskEXIT_CRITICAL(&ctx.mux);
    fetchInFlight = false;
    if (ok) changedFlag = true;
    if (g.retryGeocode) geoTriedFor = ""; // incomplete attempt: allow another
    if (g.fresh) { // persist the geocode/harvest for reboots and keyless days
      ConfigStore::setString(K_GEO_LAT,  g.lat);
      ConfigStore::setString(K_GEO_LON,  g.lon);
      ConfigStore::setString(K_GEO_NAME, g.name);
      ConfigStore::setString(K_GEO_CC,   g.cc);
      // Stamp the city the fetch RAN FOR — reading the live setting here
      // would mis-tag the cache when the city changes mid-fetch.
      ConfigStore::setString(K_GEO_FOR, ctx.args.city);
      ConfigStore::save();
    }
    // Cycle over (success or failure): the RAM freed for the TLS peak can
    // come back. Fires after the geo persistence so the save also ran lean.
    if (onFetchEnd) onFetchEnd();
  }

  // Schedule: honored wx_interval (#68 — fixing SmolTV-Pro's ignored w_i).
  uint32_t intervalMs =
      (uint32_t)ConfigStore::getInt(WxKeys::INTERVAL, WxKeys::DEF_INTERVAL_MIN) * 60000UL;
  if (!fetchDue && Platform::millis() - lastFetchMs >= intervalMs) fetchDue = true;
  // Never arm without a network: the boot cycle otherwise fires pre-WiFi,
  // fails, and (worse) used to burn the geocode latch on a dead link.
  if (!fetchDue || fetchInFlight || !fetchTask || !Net::isUp()) return;

  // Arm the task: all policy reads happen here, on core 1.
  std::string city = ConfigStore::getString(WxKeys::CITY, WxKeys::DEF_CITY);
  if (!city.length()) return;
  std::string cachedFor = ConfigStore::getString(K_GEO_FOR, "");
  bool haveCoords = cachedFor == city && ConfigStore::getString(K_GEO_LAT, "").length();
  bool isName = !isdigit((unsigned char)city[0]);
  // One geocode attempt per distinct city value (RAM latch; reboot retries).
  ctx.args.geocode = isName && !haveCoords && geoTriedFor != city;
  if (ctx.args.geocode) geoTriedFor = city;
  strlcpy(ctx.args.city, city.c_str(), sizeof(ctx.args.city));
  strlcpy(ctx.args.key, Secrets::get(WxKeys::OWM_KEY).c_str(), sizeof(ctx.args.key));
  strlcpy(ctx.args.lat, haveCoords ? ConfigStore::getString(K_GEO_LAT, "").c_str() : "",
          sizeof(ctx.args.lat));
  strlcpy(ctx.args.lon, haveCoords ? ConfigStore::getString(K_GEO_LON, "").c_str() : "",
          sizeof(ctx.args.lon));
  strlcpy(ctx.args.name, haveCoords ? ConfigStore::getString(K_GEO_NAME, "").c_str() : "",
          sizeof(ctx.args.name));
  strlcpy(ctx.args.cc, haveCoords ? ConfigStore::getString(K_GEO_CC, "").c_str() : "",
          sizeof(ctx.args.cc));
  // Window opens NOW — same pass, before the notify — so the caller can free
  // RAM ahead of the TLS peak. This covers the interval-driven promotion of
  // fetchDue above, which the old caller-side fetchQueued() check missed (#94).
  if (onFetchBegin) onFetchBegin();
  fetchDue = false;
  fetchInFlight = true;
  lastFetchMs = Platform::millis();
  xTaskNotifyGive(fetchTask);
}

const Reading* takeChanged() {
  if (!changedFlag) return nullptr;
  changedFlag = false;
  return &current;
}

void forceRefresh() { fetchDue = true; }

void onSettingsChanged() {
  // ONLY a city change refetches (#68 Q4): compare against the value we last
  // saw, not the geocode cache — so switching back to a cached city still
  // refetches, a re-saved failed geocode gets its fresh attempt, and unit/
  // colour saves never touch the network.
  std::string city = ConfigStore::getString(WxKeys::CITY, WxKeys::DEF_CITY);
  if (city != lastCity) {
    lastCity = city;
    geoTriedFor = "";
    fetchDue = true;
  }
}

} // namespace WeatherData

// ---- status surface (#99) ------------------------------------------------------
// Declared in WeatherStatus.h; defined here beside the anonymous-namespace
// state it reads — a separate TU would force that state into a shared header.
// This is the ONE consumer allowed off the main loop (httpd task, core 0):
// the fields the fetch task and main loop write are copied under the fetch
// mux, then serialized outside it. ConfigStore and Secrets are internally
// mutex-guarded, so those reads need nothing further.
void WeatherStatus::json(JsonObject out) {
  WeatherData::Reading r;
  char lastErr[sizeof(statLastErr)];
  uint32_t attempts, successes;
  int geoCode, owmCode, meteoCode;
  bool inFlight;
  taskENTER_CRITICAL(&ctx.mux);
  r = current;
  strlcpy(lastErr, statLastErr, sizeof(lastErr));
  attempts = statAttempts;
  successes = statSuccesses;
  geoCode = statGeoCode;
  owmCode = statOwmCode;
  meteoCode = statMeteoCode;
  inFlight = fetchInFlight;
  taskEXIT_CRITICAL(&ctx.mux);

  // Grouped the way the core's /api/status is, by the question each answers:
  // what is on screen, where it came from, and whether fetching is healthy.
  JsonObject reading = out["reading"].to<JsonObject>();
  reading["valid"] = r.valid;
  reading["keyless"] = r.keyless;
  reading["city"] = r.city;
  reading["country"] = r.country;
  reading["condition"] = r.condition;
  reading["iconCode"] = r.iconCode;
  reading["tempC"] = r.tempC;
  // Only OWM supplies these; the keyless open-meteo path leaves them at 0, and
  // reporting a hard 0 for "not measured" is how a diagnostic misleads.
  if (!r.keyless) {
    reading["humidity"] = r.humidity;
    reading["pressureHpa"] = r.pressureHpa;
  }

  JsonObject source = out["source"].to<JsonObject>();
  source["cityCfg"] = ConfigStore::getString(WxKeys::CITY, WxKeys::DEF_CITY);
  source["geoFor"] = ConfigStore::getString(K_GEO_FOR, "");
  source["lat"] = ConfigStore::getString(K_GEO_LAT, "");
  source["lon"] = ConfigStore::getString(K_GEO_LON, "");
  source["keyPresent"] = Secrets::has(WxKeys::OWM_KEY); // presence only, never the value

  JsonObject fetch = out["fetch"].to<JsonObject>();
  fetch["attempts"] = attempts;
  fetch["successes"] = successes;
  fetch["inFlight"] = inFlight;
  fetch["msSince"] = Platform::millis() - lastFetchMs;
  fetch["geoCode"] = geoCode; // 0 not run, -100 connect, -101 parse, else HTTP
  fetch["owmCode"] = owmCode;
  fetch["meteoCode"] = meteoCode;
  fetch["lastErr"] = (const char*)lastErr;
  // The fetch task's low-water stack, in BYTES: ESP-IDF's
  // uxTaskGetStackHighWaterMark returns bytes, not the words vanilla FreeRTOS
  // documents (task.h says so explicitly). The `* 4` that used to be here
  // overstated the free stack fourfold — the wrong direction for a number whose
  // whole job is to warn before an overflow. This is the one resource figure
  // the core cannot report for us; the heap belongs to /api/status's own `heap`
  // group, on Platform::'s rulers.
  if (fetchTask) fetch["stackFreeB"] = uxTaskGetStackHighWaterMark(fetchTask);
}
