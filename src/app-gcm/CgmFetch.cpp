// LibreLinkUp CGM fetch layer.
// Threading (ADR 0001): runFetch runs on the cgm_fetch FreeRTOS task.
// g_pending is the only cross-task shared state; it is guarded by g_mux.
// ConfigStore and Secrets are mutex-guarded and safe from either core.
// The task never touches Display or Screen — it writes g_pending, sets
// g_pendingReady, and waits for the next notify.
#include "CgmFetch.h"
#include "../core/Platform.h"
#include "CgmKeys.h"
#include "../core/ConfigStore.h"
#include "../core/Net.h"
#include "../core/Secrets.h"
#include <ArduinoJson.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>
#include <psa/crypto.h>
#include <cmath>
#include <cstdio>
#include "../core/Http.h"
#include <cstring>
#include <ctime>
#include <string>

namespace {

// ---- auth state (task-side, anonymous namespace) ----------------------------

static std::string s_token;
static std::string s_accountIdHash;
static std::string s_baseUrl     = "https://api.libreview.io";
static long     s_expiryEpoch    = 0; // JWT exp - 300 s buffer
static bool     s_loggedIn       = false;

// ---- helpers ----------------------------------------------------------------

// Decode the middle segment of a JWT (base64url, no padding) and extract fields.
// Returns false if decoding or parsing fails.
static bool jwtPayload(const std::string& token, long& expOut, std::string& regionOut) {
    // std::string::find returns npos, not -1 - the absent case cannot be
    // folded into signed comparisons the way Arduino String allowed.
    const size_t d1 = token.find('.');
    if (d1 == std::string::npos) return false;
    const size_t d2 = token.find('.', d1 + 1);
    if (d2 == std::string::npos) return false;
    std::string seg = token.substr(d1 + 1, d2 - (d1 + 1));
    // base64url → base64 (replace - with +, _ with /)
    for (char& ch : seg) {
        if (ch == '-') ch = '+';
        else if (ch == '_') ch = '/';
    }
    // add padding
    while (seg.length() % 4) seg += '=';
    size_t outLen = 0;
    uint8_t buf[512];
    // sizeof(buf) - 1: a full 512-byte decode would make the NUL below write
    // one past the end of buf. Pre-existing; fixed in passing.
    int rc = mbedtls_base64_decode(buf, sizeof(buf) - 1, &outLen,
                                   (const uint8_t*)seg.c_str(), seg.length());
    if (rc != 0) return false;
    buf[outLen] = 0;
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)buf) != DeserializationError::Ok) return false;
    expOut    = doc["exp"] | 0L;
    regionOut = doc["region"].as<std::string>();
    return true;
}

// SHA-256 of userId → lowercase hex string (the account-id header value).
// PSA, not mbedtls_sha256(): IDF 6 ships mbedTLS 4, where the hash primitives
// moved to TF-PSA-Crypto and <mbedtls/sha256.h> is private. Returns "" on
// failure -- the caller treats an empty hash as "no account-id header".
static std::string sha256Hex(const std::string& s) {
    uint8_t hash[32];
    size_t hashLen = 0;
    if (psa_crypto_init() != PSA_SUCCESS) return {};
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)s.c_str(), s.length(), hash,
                         sizeof(hash), &hashLen) != PSA_SUCCESS ||
        hashLen != sizeof(hash))
        return {};
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return hex;
}

static constexpr const char* LLU_USER_AGENT = "LibreLinkUp/4.16.0 (Android)";
static constexpr size_t LLU_HDR_CAP = 6;

// Fills LLU's app-identity headers, plus auth when withAuth and a token is
// held. authBuf owns the "Bearer ..." value and MUST outlive the request --
// Http::Header only borrows pointers. Returns the count written.
static size_t lluHeaders(Http::Header* out, size_t cap, bool withAuth, std::string& authBuf) {
    size_t n = 0;
    auto add = [&](const char* k, const char* v) { if (n < cap) out[n++] = {k, v}; };
    add("product",         "llu.android");
    add("version",         "4.16.0");
    add("accept-language", "en-US");
    add("cache-control",   "no-cache");
    if (withAuth && !s_token.empty()) {
        authBuf = "Bearer " + s_token;
        add("Authorization", authBuf.c_str());
        if (!s_accountIdHash.empty()) add("account-id", s_accountIdHash.c_str());
    }
    return n;
}

// Is our cached token still good?
static bool tokenValid() {
    if (!s_loggedIn || s_token.empty()) return false;
    time_t now = time(nullptr);
    if (now < 1000000000L) return true; // SNTP not yet synced — assume valid
    return now < (time_t)s_expiryEpoch;
}

// ---- login (may redirect to a regional server, up to 3 attempts) -----------

static bool lluLogin(const char* email, const char* pass) {
    s_loggedIn = false;
    s_token.clear();

    for (int attempt = 0; attempt < 3; attempt++) {
        const std::string url = s_baseUrl + "/llu/auth/login";

        // Serialize body via ArduinoJson to handle escaping
        JsonDocument bodyDoc;
        bodyDoc["email"]    = email;
        bodyDoc["password"] = pass;
        std::string body;
        serializeJson(bodyDoc, body);

        JsonDocument filter;
        filter["data"]["redirect"]          = true;
        filter["data"]["region"]            = true;
        filter["data"]["authTicket"]["token"] = true;
        filter["data"]["user"]["id"]        = true;

        JsonDocument doc;
        Http::Header hdrs[LLU_HDR_CAP];
        std::string authBuf;
        Http::Request rq;
        rq.url     = url.c_str();
        rq.filter  = &filter;
        rq.headers = hdrs;
        // withAuth=false: s_token was cleared above, and a login must never
        // present a stale bearer.
        rq.headerCount = lluHeaders(hdrs, LLU_HDR_CAP, false, authBuf);
        rq.userAgent   = LLU_USER_AGENT;
        rq.body        = body.c_str();
        rq.timeoutMs   = 12000;
        if (!Http::json(rq, doc).ok) return false;

        // Case 1: explicit redirect response
        if (doc["data"]["redirect"] | false) {
            const std::string region = doc["data"]["region"].as<std::string>();
            if (region.empty()) return false;
            s_baseUrl = "https://api-" + region + ".libreview.io";
            continue;
        }

        const char* tok = doc["data"]["authTicket"]["token"] | "";
        if (!tok || tok[0] == 0) return false;

        // Case 2: JWT carries a region and we are still on the global base
        long expiry = 0;
        std::string jwtRegion;
        jwtPayload(tok, expiry, jwtRegion);
        if (!jwtRegion.empty() && s_baseUrl == "https://api.libreview.io") {
            s_baseUrl = "https://api-" + jwtRegion + ".libreview.io";
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
    std::string patientId;
    float  currentVal = 0;
    uint8_t trendArrow = 0;

    for (int retry = 0; retry < 2; retry++) {
        JsonDocument filter;
        filter["data"][0]["patientId"]                               = true;
        filter["data"][0]["glucoseMeasurement"]["Value"]             = true;
        filter["data"][0]["glucoseMeasurement"]["ValueInMgPerDl"]    = true;
        filter["data"][0]["glucoseMeasurement"]["TrendArrow"]        = true;

        JsonDocument doc;
        const std::string url = s_baseUrl + "/llu/connections";
        Http::Header hdrs[LLU_HDR_CAP];
        std::string authBuf;
        Http::Request rq;
        rq.url         = url.c_str();
        rq.filter      = &filter;
        rq.headers     = hdrs;
        rq.headerCount = lluHeaders(hdrs, LLU_HDR_CAP, true, authBuf);
        rq.userAgent   = LLU_USER_AGENT;
        rq.timeoutMs   = 12000;
        const Http::Result hr = Http::json(rq, doc);

        if (hr.status == 401) {
            s_loggedIn = false;
            s_token.clear();
            // One re-login attempt for expired tokens; if that fails it's an auth error.
            if (retry == 0 && lluLogin(args.email, args.pass)) continue;
            loginFail(); return;
        }
        if (!hr.ok) { fail(); return; }

        JsonObject conn = doc["data"][0];
        if (conn.isNull()) { fail(); return; }

        patientId = conn["patientId"].as<std::string>();
        if (patientId.empty()) { fail(); return; }

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

    if (patientId.empty()) { fail(); return; }

    // ---- GET /llu/connections/{patientId}/graph -----------------------------
    {
        JsonDocument filter;
        filter["data"]["graphData"][0]["Value"]          = true;
        filter["data"]["graphData"][0]["ValueInMgPerDl"] = true;

        JsonDocument doc;
        const std::string url = s_baseUrl + "/llu/connections/" + patientId + "/graph";
        Http::Header hdrs[LLU_HDR_CAP];
        std::string authBuf;
        Http::Request rq;
        rq.url         = url.c_str();
        rq.filter      = &filter;
        rq.headers     = hdrs;
        rq.headerCount = lluHeaders(hdrs, LLU_HDR_CAP, true, authBuf);
        rq.userAgent   = LLU_USER_AGENT;
        rq.timeoutMs   = 12000;
        if (!Http::json(rq, doc).ok) { fail(); return; }

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
    result.lastOkMs    = Platform::millis();

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
            // noCredentials must be false: a fetch was attempted, so creds exist.
            g_current.error         = true;
            g_current.loginError    = fetched.loginError;
            g_current.noCredentials = false;
            g_current.lastOkMs      = fetched.lastOkMs;
        }
        g_changedFlag    = true;
        g_fetchInFlight  = false;
    }

    // Schedule next fetch
    uint32_t intervalMs =
        (uint32_t)ConfigStore::getInt(CgmKeys::INTERVAL, CgmKeys::DEF_INTERVAL) * 60000UL;
    if (!g_fetchDue && (Platform::millis() - g_lastFetchMs >= intervalMs))
        g_fetchDue = true;

    // Arm the task — only when idle, network up, credentials present, and not auth-failed.
    if (!g_fetchDue || g_fetchInFlight || g_loginFailed || !g_task || !Net::isUp()) return;

    std::string email = Secrets::get(CgmKeys::EMAIL);
    std::string pass  = Secrets::get(CgmKeys::PASSWORD);
    // Basic sanity: both fields non-empty and email contains '@' with at least
    // one '.' after it — catches typos before attempting a TLS round-trip.
    // std::string::find returns npos, not -1, so the absent case cannot be
    // folded into the signed-index arithmetic Arduino String allowed here.
    // Semantics preserved exactly: '@' not first, and a '.' at least two
    // positions after it (i.e. a non-empty label between them).
    const size_t atPos = email.find('@');
    const size_t dotPos =
        atPos == std::string::npos ? std::string::npos : email.find('.', atPos + 1);
    bool credOk = !email.empty() && !pass.empty()
                  && atPos != std::string::npos && atPos > 0
                  && dotPos != std::string::npos && dotPos > atPos + 1;
    if (!credOk) {
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
    g_lastFetchMs   = Platform::millis();
    xTaskNotifyGive(g_task);
}

const GcmData* takeChanged() {
    if (!g_changedFlag) return nullptr;
    g_changedFlag = false;
    return &g_current;
}

void forceRefresh() {
    g_loginFailed = false;
    g_fetchDue    = true;
    // Transition the screen to "connecting..." immediately rather than keeping
    // a stale error/noCredentials state visible until the fetch lands.
    if (g_current.error || g_current.noCredentials) {
        g_current.clear();
        g_changedFlag = true;
    }
}

} // namespace CgmFetch
