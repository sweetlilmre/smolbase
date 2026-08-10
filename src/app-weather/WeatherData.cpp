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
#include "../core/Secrets.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#if !SMOLBASE_WEATHER_HTTP
#include <NetworkClientSecure.h>
#endif

namespace {

#if SMOLBASE_WEATHER_HTTP
constexpr const char* SCHEME = "http://";
#else
constexpr const char* SCHEME = "https://";
#endif

// Geocode cache (raw Config Store keys, deliberately unregistered — machine
// state, not user settings): wx_geo_for remembers WHICH city value the cached
// lat/lon belong to. The failed-attempt latch is RAM-only: a typo isn't
// re-queried this boot, but a reboot retries — cheap self-healing.
String geoTriedFor;

// ---- cross-task handoff -----------------------------------------------------
// The fetch task writes `pending` under the spinlock and flips pendingReady;
// loop() promotes it into `current`. `fetchArgs` go the other way: loop()
// fills them before notifying, so the task never touches ConfigStore/Secrets
// (both are mutex-guarded and fine, but keeping ALL policy reads on core 1
// keeps this file honest about where decisions happen).
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
WeatherData::Reading current, pending;
volatile bool pendingReady = false;
bool changedFlag = false;

struct FetchArgs {
  char city[64];     // raw setting value (name or numeric OWM id)
  char key[72];      // OWM key, "" = keyless
  char lat[12], lon[12]; // cached coords, "" = unknown
  bool geocode;      // this city value still needs a name→lat/lon attempt
};
FetchArgs fetchArgs;
struct GeoResult { // task → loop, alongside `pending`
  bool fresh = false;
  char lat[12], lon[12], name[32], cc[4];
};
GeoResult geoResult;

TaskHandle_t fetchTask = nullptr;
uint32_t lastFetchMs = 0;
bool fetchDue = true; // fetch immediately on entry
bool fetchInFlight = false;

// ---- helpers (task side) ----------------------------------------------------

String urlEncode(const char* s) {
  String out;
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

// One serial GET → filtered parse → disconnect (research doc: never two TLS
// connections at once; stream-parse, don't buffer the body).
bool getJson(const String& url, const JsonDocument& filter, JsonDocument& out) {
#if SMOLBASE_WEATHER_HTTP
  NetworkClient client;
#else
  NetworkClientSecure client; // no setCACert/setInsecure: default ESP bundle
#endif
  HTTPClient http;
  http.setTimeout(10000);
  http.setConnectTimeout(10000);
  if (!http.begin(client, url)) return false;
  bool ok = false;
  if (http.GET() == HTTP_CODE_OK) {
    ok = deserializeJson(out, http.getStream(),
                         DeserializationOption::Filter(filter)) == DeserializationError::Ok;
  }
  http.end();
  return ok;
}

// WMO weathercode → the 9 OWM icon-prefix artworks + local condition text
// (SmolTV-Pro's exact mapping).
uint8_t wmoIcon(int c) {
  if (c == 0) return 1;
  if (c <= 2) return 2;
  if (c == 3) return 4;
  if (c == 45 || c == 48) return 50;
  if (c >= 51 && c <= 57) return 9;
  if (c >= 61 && c <= 67) return 10;
  if (c >= 71 && c <= 77) return 13;
  if (c >= 80 && c <= 82) return 9;
  if (c == 85 || c == 86) return 13;
  if (c >= 95) return 11;
  return 1;
}
const char* wmoText(int c) {
  if (c == 0) return "Clear";
  if (c <= 2) return "Partly Cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c == 85 || c == 86) return "Snow Showers";
  if (c >= 95) return "Thunderstorm";
  return "Unknown";
}

bool geocode(GeoResult& g) {
  JsonDocument filter;
  filter["results"][0]["latitude"] = true;
  filter["results"][0]["longitude"] = true;
  filter["results"][0]["name"] = true;
  filter["results"][0]["country_code"] = true;
  JsonDocument doc;
  String url = String(SCHEME) + "geocoding-api.open-meteo.com/v1/search?name=" +
               urlEncode(fetchArgs.city) + "&count=1&language=en&format=json";
  if (!getJson(url, filter, doc) || doc["results"][0].isNull()) return false;
  snprintf(g.lat, sizeof(g.lat), "%.4f", doc["results"][0]["latitude"].as<double>());
  snprintf(g.lon, sizeof(g.lon), "%.4f", doc["results"][0]["longitude"].as<double>());
  strlcpy(g.name, doc["results"][0]["name"] | "", sizeof(g.name));
  strlcpy(g.cc, doc["results"][0]["country_code"] | "", sizeof(g.cc));
  g.fresh = true;
  return true;
}

bool fetchOwm(WeatherData::Reading& r, GeoResult& g) {
  JsonDocument filter;
  filter["main"] = true;
  filter["wind"]["speed"] = true;
  filter["weather"][0] = true;
  filter["sys"]["country"] = true;
  filter["coord"] = true;
  filter["name"] = true;
  JsonDocument doc;
  bool byId = isdigit((unsigned char)fetchArgs.city[0]);
  String url = String(SCHEME) + "api.openweathermap.org/data/2.5/weather?" +
               (byId ? "id=" : "q=") + urlEncode(fetchArgs.city) +
               "&appid=" + fetchArgs.key + "&units=metric&lang=en";
  if (!getJson(url, filter, doc) || doc["main"].isNull()) return false;
  r.tempC = doc["main"]["temp"] | 0.0f;
  r.tempMinC = doc["main"]["temp_min"] | 0.0f;
  r.tempMaxC = doc["main"]["temp_max"] | 0.0f;
  r.feelsC = doc["main"]["feels_like"] | 0.0f;
  r.humidity = doc["main"]["humidity"] | 0;
  r.pressureHpa = doc["main"]["pressure"] | 0;
  r.windMs = doc["wind"]["speed"] | 0.0f;
  strlcpy(r.condition, doc["weather"][0]["main"] | "", sizeof(r.condition));
  r.iconCode = (uint8_t)String(doc["weather"][0]["icon"] | "01").substring(0, 2).toInt();
  strlcpy(r.city, doc["name"] | fetchArgs.city, sizeof(r.city));
  strlcpy(r.country, doc["sys"]["country"] | "", sizeof(r.country));
  r.keyless = false;
  r.valid = true;
  // Harvest coords: OWM answers for an id-city give the keyless path its
  // lat/lon for the day the key dies (the geocoder can't resolve an id).
  if (!fetchArgs.lat[0] && !doc["coord"]["lat"].isNull()) {
    snprintf(g.lat, sizeof(g.lat), "%.4f", doc["coord"]["lat"].as<double>());
    snprintf(g.lon, sizeof(g.lon), "%.4f", doc["coord"]["lon"].as<double>());
    strlcpy(g.name, r.city, sizeof(g.name));
    strlcpy(g.cc, r.country, sizeof(g.cc));
    g.fresh = true;
  }
  return true;
}

bool fetchOpenMeteo(WeatherData::Reading& r) {
  const char* lat = fetchArgs.lat[0] ? fetchArgs.lat : (geoResult.fresh ? geoResult.lat : "");
  const char* lon = fetchArgs.lon[0] ? fetchArgs.lon : (geoResult.fresh ? geoResult.lon : "");
  if (!lat[0]) return false; // id-city, keyless, no cached coords: no data
  JsonDocument filter;
  filter["current_weather"] = true;
  filter["daily"]["temperature_2m_max"][0] = true;
  filter["daily"]["temperature_2m_min"][0] = true;
  JsonDocument doc;
  String url = String(SCHEME) + "api.open-meteo.com/v1/forecast?latitude=" + lat +
               "&longitude=" + lon +
               "&current_weather=true&daily=temperature_2m_max,temperature_2m_min,weathercode"
               "&forecast_days=1&timezone=auto";
  if (!getJson(url, filter, doc) || doc["current_weather"].isNull()) return false;
  r.tempC = doc["current_weather"]["temperature"] | 0.0f;
  r.windMs = (doc["current_weather"]["windspeed"] | 0.0f) / 3.6f; // km/h → m/s
  int code = doc["current_weather"]["weathercode"] | 0;
  r.iconCode = wmoIcon(code);
  strlcpy(r.condition, wmoText(code), sizeof(r.condition));
  r.tempMaxC = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  r.tempMinC = doc["daily"]["temperature_2m_min"][0] | 0.0f;
  r.feelsC = 0; // free tier has none — the three zeros are the keyless tell
  r.humidity = 0;
  r.pressureHpa = 0;
  strlcpy(r.city, geoResult.fresh ? geoResult.name : fetchArgs.city, sizeof(r.city));
  strlcpy(r.country, geoResult.fresh ? geoResult.cc : "", sizeof(r.country));
  r.keyless = true;
  r.valid = true;
  return true;
}

void fetchTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    WeatherData::Reading r;
    GeoResult g = {};
    if (fetchArgs.geocode) geocode(g); // failure falls through: OWM may still answer a name
    geoResult = g;                     // task-side scratch for fetchOpenMeteo
    bool ok = (fetchArgs.key[0] && fetchOwm(r, g)) || fetchOpenMeteo(r);
    taskENTER_CRITICAL(&lock);
    if (ok) pending = r;
    geoResult = g;
    pendingReady = true; // even on failure: loop() must clear fetchInFlight
    taskEXIT_CRITICAL(&lock);
  }
}

String unit(const char* key, const char* def) { return ConfigStore::getString(key, def); }

} // namespace

namespace WeatherData {

void begin() {
  geoTriedFor = "";
  // TLS handshake work happens heap-side, but mbedTLS still wants real stack.
  xTaskCreate(fetchTaskFn, "wx_fetch", 12288, nullptr, 1, &fetchTask);
}

void loop() {
  // Promote a finished fetch.
  if (pendingReady) {
    taskENTER_CRITICAL(&lock);
    pendingReady = false;
    bool ok = pending.valid;
    if (ok) {
      current = pending;
      pending = Reading{};
    }
    GeoResult g = geoResult;
    geoResult.fresh = false;
    taskEXIT_CRITICAL(&lock);
    fetchInFlight = false;
    if (ok) changedFlag = true;
    if (g.fresh) { // persist the geocode/harvest for reboots and keyless days
      ConfigStore::setString("wx_lat", g.lat);
      ConfigStore::setString("wx_lon", g.lon);
      ConfigStore::setString("wx_geo_name", g.name);
      ConfigStore::setString("wx_geo_cc", g.cc);
      ConfigStore::setString("wx_geo_for", ConfigStore::getString("city", "Durban"));
      ConfigStore::save();
    }
  }

  // Schedule: honored wx_interval (#68 — fixing SmolTV-Pro's ignored w_i).
  uint32_t intervalMs = (uint32_t)ConfigStore::getInt("wx_interval", 20) * 60000UL;
  if (!fetchDue && millis() - lastFetchMs >= intervalMs) fetchDue = true;
  if (!fetchDue || fetchInFlight || !fetchTask) return;

  // Arm the task: all policy reads happen here, on core 1.
  String city = ConfigStore::getString("city", "Durban");
  if (!city.length()) return;
  String cachedFor = ConfigStore::getString("wx_geo_for", "");
  bool haveCoords = cachedFor == city && ConfigStore::getString("wx_lat", "").length();
  bool isName = !isdigit((unsigned char)city[0]);
  // One geocode attempt per distinct city value (RAM latch; reboot retries).
  fetchArgs.geocode = isName && !haveCoords && geoTriedFor != city;
  if (fetchArgs.geocode) geoTriedFor = city;
  strlcpy(fetchArgs.city, city.c_str(), sizeof(fetchArgs.city));
  strlcpy(fetchArgs.key, Secrets::get("owm_api_key").c_str(), sizeof(fetchArgs.key));
  strlcpy(fetchArgs.lat, haveCoords ? ConfigStore::getString("wx_lat", "").c_str() : "",
          sizeof(fetchArgs.lat));
  strlcpy(fetchArgs.lon, haveCoords ? ConfigStore::getString("wx_lon", "").c_str() : "",
          sizeof(fetchArgs.lon));
  fetchDue = false;
  fetchInFlight = true;
  lastFetchMs = millis();
  xTaskNotifyGive(fetchTask);
}

const Reading& reading() { return current; }

bool changed() {
  bool c = changedFlag;
  changedFlag = false;
  return c;
}

void forceRefresh() { fetchDue = true; }

void onSettingsChanged() {
  // A city change invalidates the geocode latch and refetches immediately
  // (#68 Q4); every other setting re-renders from cache, no fetch.
  String city = ConfigStore::getString("city", "Durban");
  if (ConfigStore::getString("wx_geo_for", "") != city && geoTriedFor != city) {
    geoTriedFor = "";
    fetchDue = true;
  }
}

// ---- display formatting (SmolTV-Pro's exact output constants) ---------------

String fmtTemp(float c) {
  if (unit("unit_temp", "C") == "F") return String((int)lroundf(c * 1.8f + 32)) + "\xC2\xB0" "F";
  return String((int)lroundf(c)) + "\xC2\xB0" "C";
}

String fmtWind(float ms) {
  String u = unit("unit_wind", "ms");
  if (u == "kmh") return String(ms * 3.6f, 2) + " km/h";
  if (u == "mph") return String(ms * 2.2367f, 2) + " mile/h"; // parity constant
  return String(ms, 2) + " m/s";
}

String fmtPress(int hpa) {
  String u = unit("unit_press", "hpa");
  if (u == "kpa") return String(hpa / 10) + " kPa";
  if (u == "mmhg") return String((int)lroundf(hpa * 0.75f)) + " mmHg"; // parity constant
  if (u == "inhg") return String((int)lroundf(hpa * 0.0295300425f)) + " inHg";
  return String(hpa) + " hPa";
}

} // namespace WeatherData
