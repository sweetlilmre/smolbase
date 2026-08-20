// LibreLinkUp CGM fetch layer.
// Threading (ADR 0001): runFetch runs on the cgm_fetch FreeRTOS task.
// g_pending is the only cross-task shared state; it is guarded by g_mux.
// ConfigStore and Secrets are mutex-guarded and safe from either core.
// The task never touches Display or Screen — it writes g_pending, sets
// g_pendingReady, and waits for the next notify.
#include "CgmFetch.h"
#include "CgmKeys.h"
#include "../core/ConfigStore.h"
#include "../core/Net.h"
#include "../core/Secrets.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

class BundleClient : public NetworkClientSecure {
public:
    BundleClient() {
        attach_ssl_certificate_bundle(sslclient.get(), true);
        _use_ca_bundle = true;
    }
};
#define CGM_MKCONN BundleClient _conn

// ---- auth state (task-side, anonymous namespace) ----------------------------

static String   s_token;
static String   s_accountIdHash;
static String   s_baseUrl        = "https://api.libreview.io";
static long     s_expiryEpoch    = 0; // JWT exp - 300 s buffer
static bool     s_loggedIn       = false;

// ---- helpers ----------------------------------------------------------------

// Decode the middle segment of a JWT (base64url, no padding) and extract fields.
// Returns false if decoding or parsing fails.
static bool jwtPayload(const String& token, long& expOut, String& regionOut) {
    int d1 = token.indexOf('.');
    if (d1 < 0) return false;
    int d2 = token.indexOf('.', d1 + 1);
    if (d2 < 0) return false;
    String seg = token.substring(d1 + 1, d2);
    // base64url → base64 (replace - with +, _ with /)
    for (int i = 0; i < (int)seg.length(); i++) {
        if (seg[i] == '-') seg[i] = '+';
        else if (seg[i] == '_') seg[i] = '/';
    }
    // add padding
    while (seg.length() % 4) seg += '=';
    size_t outLen = 0;
    uint8_t buf[512];
    int rc = mbedtls_base64_decode(buf, sizeof(buf), &outLen,
                                   (const uint8_t*)seg.c_str(), seg.length());
    if (rc != 0) return false;
    buf[outLen] = 0;
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)buf) != DeserializationError::Ok) return false;
    expOut    = doc["exp"] | 0L;
    regionOut = doc["region"] | "";
    return true;
}

// SHA-256 of userId → lowercase hex string (the account-id header value).
static String sha256Hex(const String& s) {
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t*)s.c_str(), s.length(), hash, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return String(hex);
}

// Add the LLU app-identity headers to an HTTPClient.
static void addLluHeaders(HTTPClient& http) {
    http.setUserAgent("LibreLinkUp/4.16.0 (Android)");
    http.addHeader("product",          "llu.android");
    http.addHeader("version",          "4.16.0");
    http.addHeader("accept-language",  "en-US");
    http.addHeader("cache-control",    "no-cache");
}

// Add auth headers when a token is present.
static void addAuthHeaders(HTTPClient& http) {
    if (s_token.isEmpty()) return;
    http.addHeader("Authorization", String("Bearer ") + s_token);
    if (!s_accountIdHash.isEmpty())
        http.addHeader("account-id", s_accountIdHash);
}

// Is our cached token still good?
static bool tokenValid() {
    if (!s_loggedIn || s_token.isEmpty()) return false;
    time_t now = time(nullptr);
    if (now < 1000000000L) return true; // SNTP not yet synced — assume valid
    return now < (time_t)s_expiryEpoch;
}

// ---- login (may redirect to a regional server, up to 3 attempts) -----------

static bool lluLogin(const char* email, const char* pass) {
    s_loggedIn = false;
    s_token    = "";

    for (int attempt = 0; attempt < 3; attempt++) {
        CGM_MKCONN;
        HTTPClient http;
        http.setTimeout(12000);
        http.setConnectTimeout(12000);
        String url = s_baseUrl + "/llu/auth/login";
        if (!http.begin(_conn, url)) return false;
        addLluHeaders(http);
        http.addHeader("content-type", "application/json");

        // Serialize body via ArduinoJson to handle escaping
        JsonDocument bodyDoc;
        bodyDoc["email"]    = email;
        bodyDoc["password"] = pass;
        String body;
        serializeJson(bodyDoc, body);

        int code = http.POST(body);
        if (code != 200) { http.end(); return false; }

        JsonDocument filter;
        filter["data"]["redirect"]          = true;
        filter["data"]["region"]            = true;
        filter["data"]["authTicket"]["token"] = true;
        filter["data"]["user"]["id"]        = true;

        JsonDocument doc;
        String resp = http.getString();
        http.end();

        if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))
                != DeserializationError::Ok) return false;

        // Case 1: explicit redirect response
        if (doc["data"]["redirect"] | false) {
            String region = doc["data"]["region"] | "";
            if (region.isEmpty()) return false;
            s_baseUrl = String("https://api-") + region + ".libreview.io";
            continue;
        }

        const char* tok = doc["data"]["authTicket"]["token"] | "";
        if (!tok || tok[0] == 0) return false;

        // Case 2: JWT carries a region and we are still on the global base
        long expiry = 0;
        String jwtRegion;
        jwtPayload(tok, expiry, jwtRegion);
        if (!jwtRegion.isEmpty() && s_baseUrl == "https://api.libreview.io") {
            s_baseUrl = String("https://api-") + jwtRegion + ".libreview.io";
            continue; // re-login on the regional host for a regional token
        }

        // Successful auth
        s_token        = tok;
        s_expiryEpoch  = expiry - 300;
        const char* uid = doc["data"]["user"]["id"] | "";
        if (uid && uid[0]) s_accountIdHash = sha256Hex(uid);
        s_loggedIn = true;
        return true;
    }
    return false;
}

// ---- task comms -------------------------------------------------------------

struct FetchArgs {
    char email[128];
    char pass[128];
    bool mgdl;
};

static FetchArgs         g_args;
static portMUX_TYPE      g_mux          = portMUX_INITIALIZER_UNLOCKED;
static GcmData           g_pending;
static GcmData           g_current;
static volatile bool     g_pendingReady = false;
static volatile bool     g_loginFailed  = false;  // sticky; cleared by forceRefresh()
static bool              g_changedFlag  = false;
static bool              g_fetchInFlight = false;
static bool              g_fetchDue     = true;
static uint32_t          g_lastFetchMs  = 0;
static TaskHandle_t      g_task         = nullptr;

// ---- fetch cycle (runs on cgm_fetch task) -----------------------------------

static void runFetch(const FetchArgs& args) {
    GcmData result;
    result.clear();
    strlcpy(result.name, "CGM", sizeof(result.name));

    auto post = [&]() {
        taskENTER_CRITICAL(&g_mux);
        g_pending = result;
        g_pendingReady = true;
        taskEXIT_CRITICAL(&g_mux);
    };
    auto fail = [&]() { result.error = true; post(); };
    auto loginFail = [&]() {
        result.error      = true;
        result.loginError = true;
        g_loginFailed     = true;
        post();
    };

    // Ensure we have a valid token
    if (!tokenValid()) {
        if (!lluLogin(args.email, args.pass)) { loginFail(); return; }
    }

    // ---- GET /llu/connections -----------------------------------------------
    String patientId;
    float  currentVal = 0;
    uint8_t trendArrow = 0;

    for (int retry = 0; retry < 2; retry++) {
        CGM_MKCONN;
        HTTPClient http;
        http.setTimeout(12000);
        http.setConnectTimeout(12000);
        if (!http.begin(_conn, s_baseUrl + "/llu/connections")) { fail(); return; }
        addLluHeaders(http);
        addAuthHeaders(http);
        int code = http.GET();

        if (code == 401) {
            http.end();
            s_loggedIn = false;
            s_token    = "";
            // One re-login attempt for expired tokens; if that fails it's an auth error.
            if (retry == 0 && lluLogin(args.email, args.pass)) continue;
            loginFail(); return;
        }
        if (code != 200) { http.end(); fail(); return; }

        JsonDocument filter;
        filter["data"][0]["patientId"]                               = true;
        filter["data"][0]["glucoseMeasurement"]["Value"]             = true;
        filter["data"][0]["glucoseMeasurement"]["ValueInMgPerDl"]    = true;
        filter["data"][0]["glucoseMeasurement"]["TrendArrow"]        = true;

        JsonDocument doc;
        String resp = http.getString();
        http.end();

        if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))
                != DeserializationError::Ok) { fail(); return; }

        JsonObject conn = doc["data"][0];
        if (conn.isNull()) { fail(); return; }

        patientId = conn["patientId"] | "";
        if (patientId.isEmpty()) { fail(); return; }

        JsonObject gm = conn["glucoseMeasurement"];
        if (args.mgdl) {
            float mgdl = gm["ValueInMgPerDl"] | 0.0f;
            if (mgdl == 0) mgdl = (gm["Value"] | 0.0f) * 18.0f;
            currentVal = roundf(mgdl);
        } else {
            currentVal = roundf((gm["Value"] | 0.0f) * 10.0f) / 10.0f;
        }
        int ta = gm["TrendArrow"] | 3;
        trendArrow = (ta >= 1 && ta <= 5) ? (uint8_t)ta : 3;
        break;
    }

    if (patientId.isEmpty()) { fail(); return; }

    // ---- GET /llu/connections/{patientId}/graph -----------------------------
    {
        CGM_MKCONN;
        HTTPClient http;
        http.setTimeout(12000);
        http.setConnectTimeout(12000);
        String url = s_baseUrl + "/llu/connections/" + patientId + "/graph";
        if (!http.begin(_conn, url)) { fail(); return; }
        addLluHeaders(http);
        addAuthHeaders(http);
        int code = http.GET();
        if (code != 200) { http.end(); fail(); return; }

        JsonDocument filter;
        filter["data"]["graphData"][0]["Value"]          = true;
        filter["data"]["graphData"][0]["ValueInMgPerDl"] = true;

        JsonDocument doc;
        String resp = http.getString();
        http.end();

        if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))
                != DeserializationError::Ok) { fail(); return; }

        JsonArray graphData = doc["data"]["graphData"].as<JsonArray>();
        uint8_t total = (uint8_t)graphData.size();
        // take at most 23 historical points (leave room to append current)
        uint8_t count = (total > 23) ? 23 : total;
        uint8_t start = total - count;
        for (uint8_t i = 0; i < count; i++) {
            JsonObject r = graphData[start + i];
            float v;
            if (args.mgdl) {
                v = r["ValueInMgPerDl"] | 0.0f;
                if (v == 0) v = (r["Value"] | 0.0f) * 18.0f;
                v = roundf(v);
            } else {
                v = roundf((r["Value"] | 0.0f) * 10.0f) / 10.0f;
            }
            result.spark[result.sparkCount++] = v;
        }
    }

    // Append current if not already the last point
    if (result.sparkCount == 0 || result.spark[result.sparkCount - 1] != currentVal) {
        if (result.sparkCount < 24)
            result.spark[result.sparkCount++] = currentVal;
        else
            result.spark[23] = currentVal; // overwrite last slot
    }

    // ---- in-range -----------------------------------------------------------
    if (args.mgdl)
        result.inRange = (currentVal >= 72.0f && currentVal <= 162.0f) ? 1 : 0;
    else
        result.inRange  = (currentVal >= 4.0f  && currentVal <= 9.0f)  ? 1 : 0;

    result.glucose     = currentVal;
    result.trendArrow  = trendArrow;
    result.mgdl        = args.mgdl;
    result.valid       = true;
    result.error       = false;
    result.lastOkMs    = millis();

    taskENTER_CRITICAL(&g_mux);
    g_pending      = result;
    g_pendingReady = true;
    taskEXIT_CRITICAL(&g_mux);
}

// ---- FreeRTOS task ----------------------------------------------------------

static void fetchTaskFn(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        FetchArgs args;
        taskENTER_CRITICAL(&g_mux);
        args = g_args;
        taskEXIT_CRITICAL(&g_mux);
        runFetch(args);
    }
}

} // namespace

// ---- public API -------------------------------------------------------------

namespace CgmFetch {

void begin() {
    // 14 KB stack: TLS handshakes peak around 10-12 KB; keeps the same headroom
    // the weather fetch uses, confirmed adequate on hardware (#82, #94).
    xTaskCreate(fetchTaskFn, "cgm_fetch", 14336, nullptr, 1, &g_task);
}

void loop() {
    // Promote a finished fetch (spinlock — task may write from any core).
    if (g_pendingReady) {
        GcmData fetched;
        taskENTER_CRITICAL(&g_mux);
        g_pendingReady = false;
        fetched = g_pending;
        taskEXIT_CRITICAL(&g_mux);

        if (fetched.valid) {
            g_current = fetched;
        } else {
            // Keep stale reading; propagate error type from the failed fetch.
            g_current.error      = true;
            g_current.loginError = fetched.loginError;
            g_current.lastOkMs   = fetched.lastOkMs;
        }
        g_changedFlag    = true;
        g_fetchInFlight  = false;
    }

    // Schedule next fetch
    uint32_t intervalMs =
        (uint32_t)ConfigStore::getInt(CgmKeys::INTERVAL, CgmKeys::DEF_INTERVAL) * 60000UL;
    if (!g_fetchDue && (millis() - g_lastFetchMs >= intervalMs))
        g_fetchDue = true;

    // Arm the task — only when idle, network up, credentials present, and not auth-failed.
    if (!g_fetchDue || g_fetchInFlight || g_loginFailed || !g_task || !Net::isUp()) return;

    String email = Secrets::get(CgmKeys::EMAIL);
    String pass  = Secrets::get(CgmKeys::PASSWORD);
    if (email.isEmpty() || pass.isEmpty()) {
        if (!g_current.noCredentials) {
            g_current.clear();
            g_current.noCredentials = true;
            g_changedFlag = true;
        }
        return;
    }

    bool mgdl = (ConfigStore::getString(CgmKeys::UNIT, CgmKeys::DEF_UNIT) == "mgdl");

    taskENTER_CRITICAL(&g_mux);
    strlcpy(g_args.email, email.c_str(), sizeof(g_args.email));
    strlcpy(g_args.pass,  pass.c_str(),  sizeof(g_args.pass));
    g_args.mgdl = mgdl;
    taskEXIT_CRITICAL(&g_mux);

    g_fetchDue      = false;
    g_fetchInFlight = true;
    g_lastFetchMs   = millis();
    xTaskNotifyGive(g_task);
}

const GcmData* takeChanged() {
    if (!g_changedFlag) return nullptr;
    g_changedFlag = false;
    return &g_current;
}

void forceRefresh() {
    g_loginFailed             = false;  // allow retry after credential change
    g_current.noCredentials   = false;  // re-check credentials on next loop
    g_fetchDue                = true;
}

} // namespace CgmFetch
