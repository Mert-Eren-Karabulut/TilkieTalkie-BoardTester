#include "RequestManager.h"
#include "ReverbClient.h"
#include "CaBundle.h"
#include "PsramJson.h"
#include "AudioController.h"
#include "PromptPackManager.h"
#include "Faults.h"
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_log.h>
#include <algorithm>

static const char *TAG = "api";

// Initialize static members
const char* RequestManager::NVS_NAMESPACE = "requestmgr";
const char* RequestManager::NVS_UID_MAPPING_KEY = "uid_mappings";
const char* RequestManager::NVS_UID_LRU_KEY = "uid_lru";
volatile bool RequestManager::portalReachableFlag = false;
const char* RequestManager::SD_DATA_DIR = "/.requestmanager";
const char* RequestManager::SD_MANIFEST_DIR = "/.requestmanager/manifests";
const char* RequestManager::SD_PENDING_MANIFEST_DIR = "/.requestmanager/pending_manifests";
static const unsigned long RECHECK_FIRST_MS = 3UL * 60UL * 1000UL;        // after boot (plus up to 60 s jitter)
static const unsigned long RECHECK_INTERVAL_MS = 12UL * 60UL * 60UL * 1000UL; // the portal also pushes sync-figure
// Status handling (requestAsync): re-send delays for 5xx, the 429 default and the 403 refresh rate.
static const unsigned long SERVER_RETRY_MS[] = {30UL * 1000UL, 2UL * 60UL * 1000UL, 10UL * 60UL * 1000UL};
static const int SERVER_RETRIES = 3;
static const int THROTTLE_RETRIES = 2;
static const int RETRY_AFTER_DEFAULT_S = 60;
static const int PORTAL_UNREACHABLE_AFTER_5XX = 4;       // the first 5xx plus three retries
static const unsigned long TOKEN_REFRESH_403_MIN_MS = 10UL * 60UL * 1000UL;
static const char *RESPONSE_HEADERS[] = {"Retry-After"};
static const unsigned long TOKEN_BACKOFF_MS = 15UL * 60UL * 1000UL;     // 404 / 401 / 429 from the token POST
static const unsigned long TOKEN_RETRY_SHORT_MS = 60UL * 1000UL;        // transport error or 5xx
static const unsigned long FORCED_REFRESH_MIN_MS = 10UL * 60UL * 1000UL;
static const unsigned long DAY_MS = 24UL * 60UL * 60UL * 1000UL;
static const uint16_t TOKEN_REFRESH_DAYS = 80;                          // the portal's tokens live 90 days

namespace {
struct LockGuard {
    SemaphoreHandle_t m;
    explicit LockGuard(SemaphoreHandle_t h) : m(h) { if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY); }
    ~LockGuard() { if (m) xSemaphoreGiveRecursive(m); }
};

// Cut to at most maxBytes on a UTF-8 character boundary (a cut inside "ş" would make the whole
// frame invalid JSON for the portal).
String utf8Truncate(const String &in, size_t maxBytes)
{
    if (in.length() <= maxBytes) {
        return in;
    }
    size_t cut = maxBytes;
    while (cut > 0 && (((uint8_t)in[cut]) & 0xC0) == 0x80) {
        cut--;
    }
    return in.substring(0, cut);
}

String jsonValueToString(JsonVariantConst value)
{
    if (value.isNull()) {
        return String();
    }

    if (value.is<const char*>()) {
        return value.as<String>();
    }

    if (value.is<String>()) {
        return value.as<String>();
    }

    if (value.is<int>()) {
        return String(value.as<int>());
    }

    if (value.is<long>()) {
        return String(value.as<long>());
    }

    if (value.is<unsigned int>()) {
        return String(value.as<unsigned int>());
    }

    if (value.is<unsigned long>()) {
        return String(value.as<unsigned long>());
    }

    String serialized;
    serializeJson(value, serialized);
    return serialized;
}
}

// Singleton instance getter
RequestManager &RequestManager::getInstance(const String &baseUrl)
{
    static RequestManager instance(baseUrl);
    return instance;
}

// Constructor
RequestManager::RequestManager(const String &baseUrl)
    : timeout(8000)
    , nvsHandle(0)
    , figureDownloadCompleteCallback(nullptr)
{
    httpLock = xSemaphoreCreateRecursiveMutex();
    jobLock = xSemaphoreCreateRecursiveMutex();
    setBaseUrl(baseUrl); // https:// stays https:// (verified against the CA bundle)
}

// Destructor
RequestManager::~RequestManager()
{
    http.end();
    saveUidMappings();
    
    if (nvsHandle != 0) {
        nvs_close(nvsHandle);
    }
    
    activeDownloads.clear();
    uidLru.clear();
}

// Initialization
bool RequestManager::begin()
{
    ESP_LOGI(TAG, "Initializing...");
    
    if (!initializeNVS()) {
        ESP_LOGW(TAG, "Failed to initialize NVS");
        return false;
    }
    
    loadUidMappings();

    // NVS could not be opened by ConfigManager at boot (it ran before Faults existed).
    if (ConfigManager::getInstance().takeOpenFailure()) {
        Faults::raise(Fault::CONFIG_CORRUPT, "config");
    }
    
    FileManager &fileManager = FileManager::getInstance();
    fileManager.createDirectory(SD_DATA_DIR);
    fileManager.createDirectory(SD_MANIFEST_DIR);
    fileManager.createDirectory(SD_PENDING_MANIFEST_DIR);
    fileManager.setDownloadCompleteCallback(staticFileDownloadCallback);

    initConnection();
    begun = true;
    nextTokenRetryAt = millis() + TOKEN_BACKOFF_MS; // without a token: the next attempt in 15 min (Reverb may ask sooner)

    ESP_LOGI(TAG, "%s", authToken.length() > 0 ? "Initialized with token" : "No token available");
    return authToken.length() > 0;
}

// Configuration methods
void RequestManager::setBaseUrl(const String &url)
{
    baseUrl = url;
    if (baseUrl.startsWith("https://")) {
        // The hub token travels in every request: verify the portal's certificate.
        secureClient.setCACertBundle(caBundleStart(), caBundleSize());
        client = &secureClient;
    } else {
        client = &plainClient;
    }
    ESP_LOGI(TAG, "API base %s (%s)", baseUrl.c_str(),
                  client == &secureClient ? "TLS, CA bundle" : "plain HTTP");
}

void RequestManager::setAuthToken(const String &token)
{
    LockGuard guard(httpLock); // the worker reads it while building headers
    this->authToken = token;
}

void RequestManager::setTimeout(int timeoutMs)
{
    this->timeout = timeoutMs;
}

void RequestManager::update()
{
    raiseDeferredFaults();
    dispatchCompletedJobs();
    serviceToken();
    serviceLinkCheck();
    serviceManifestRecheck();

    if (!figureDownloadCompleteCallback || pendingCompletions.empty()) {
        return;
    }

    PendingFigureCompletion completion = pendingCompletions.front();
    pendingCompletions.erase(pendingCompletions.begin());
    figureDownloadCompleteCallback(
        completion.uid,
        completion.figureName,
        completion.success,
        completion.error,
        completion.figureData,
        completion.isUpdate
    );
}

// Consolidated network check
bool RequestManager::isNetworkReady()
{
    if (WiFi.status() != WL_CONNECTED || !portalReachableFlag) {
        return false;
    }
    return WiFi.localIP()[0] != 0;
}

void RequestManager::setDefaultHeaders()
{
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "TilkieTalkie/1.0");

    if (authToken.length() > 0) {
        http.addHeader("Authorization", "Bearer " + authToken);
    }
}

JsonDocument RequestManager::parseResponse(const String& response)
{
    JsonDocument doc(&psramJsonAllocator);
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
        const String message = String("JSON parsing error: ") + error.c_str();
        ESP_LOGE(TAG, "%s", message.c_str());
        
        JsonDocument errorDoc(&psramJsonAllocator);
        errorDoc["error"] = true;
        errorDoc["message"] = message;
        return errorDoc;
    }

    return doc;
}

// One HTTP exchange. Serialized by httpLock: the synchronous API runs it on the caller (boot,
// token refresh), the asynchronous API on the request worker task - never two at once.
int RequestManager::performHttp(const String& endpoint, const String& method, const String* payload, String& body, String& error,
                                const String& ifNoneMatch, int* retryAfterS)
{
    body = String();
    error = String();
    if (!isNetworkReady()) {
        error = "Network not ready";
        return 0;
    }
    LockGuard guard(httpLock);

    // Protected endpoints need the hub token first (the token endpoints themselves do not).
    const bool tokenEndpoint = endpoint.endsWith("/token") || endpoint.endsWith("/validate-token");
    if (authToken.length() == 0 && !tokenEndpoint) {
        initConnection();
        if (authToken.length() == 0) {
            error = "Authentication failed";
            return 0;
        }
    }

    String url = baseUrl + endpoint;
    http.end();
    if (!http.begin(*client, url)) {
        error = "Failed to connect to " + url;
        return 0;
    }

    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.collectHeaders(RESPONSE_HEADERS, 1);
    setDefaultHeaders();
    if (ifNoneMatch.length() > 0) {
        http.addHeader("If-None-Match", "\"" + ifNoneMatch + "\"");
    }

    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset(); // only the loop task is subscribed // harmless on the worker task (not subscribed); matters on the loop task
    int code = (method == "POST" && payload) ? http.POST(*payload) : http.GET();
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset(); // only the loop task is subscribed

    if (code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_SEND_HEADER_FAILED ||
        code == HTTPC_ERROR_SEND_PAYLOAD_FAILED || code == HTTPC_ERROR_NOT_CONNECTED ||
        code == HTTPC_ERROR_CONNECTION_LOST) {
        // A kept-alive connection the server has since closed (nginx drops idle ones after
        // 65 s) fails at the first write. Drop it and try once more on a fresh connection.
        http.end();
        client->stop();
        ESP_LOGW(TAG, "%s %s failed (%d) on a reused connection - retrying", method.c_str(), endpoint.c_str(), code);
        if (!http.begin(*client, url)) {
            error = "Failed to connect to " + url;
            return 0;
        }
        http.setTimeout(timeout);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.collectHeaders(RESPONSE_HEADERS, 1);
        setDefaultHeaders();
        if (ifNoneMatch.length() > 0) {
            http.addHeader("If-None-Match", "\"" + ifNoneMatch + "\"");
        }
        code = (method == "POST" && payload) ? http.POST(*payload) : http.GET();
    }

    if (code == 401 && !tokenEndpoint) {
        // Token revoked server-side (e.g. another hub of the same user fetched a new one):
        // re-acquire once and retry, instead of staying dead until the next reboot.
        static bool retrying = false;
        http.end();
        if (!retrying) {
            retrying = true;
            ESP_LOGI(TAG, "401 - refreshing hub token and retrying once");
            authToken = "";
            ConfigManager::getInstance().clearJWTToken();
            initConnection();
            int retried = authToken.length() > 0 ? performHttp(endpoint, method, payload, body, error, ifNoneMatch, retryAfterS) : 0;
            retrying = false;
            if (authToken.length() > 0) {
                return retried;
            }
        }
        error = "Unauthorized";
        deferFault((uint16_t)Fault::TOKEN_REJECTED, endpoint.substring(0, 39).c_str(), true);
        return code;
    }

    if (code == 403 && !tokenEndpoint) {
        // A hub endpoint refusing the token (hub unbound, token revoked on the portal): before
        // 2026-10 only 401 refreshed it, so fault uploads and OTA checks failed until a reboot.
        // Refresh once per 10 min and retry this request once; otherwise the 403 stands.
        static bool retrying403 = false;
        static unsigned long lastRefresh403 = 0;
        http.end();
        if (!retrying403 && (lastRefresh403 == 0 || millis() - lastRefresh403 >= TOKEN_REFRESH_403_MIN_MS)) {
            lastRefresh403 = millis() ? millis() : 1;
            retrying403 = true;
            ESP_LOGI(TAG, "403 on %s - refreshing the device token and retrying once", endpoint.c_str());
            const bool refreshed = forceTokenRefresh("403");
            int retried = refreshed ? performHttp(endpoint, method, payload, body, error, ifNoneMatch, retryAfterS) : code;
            retrying403 = false;
            if (refreshed) {
                return retried;
            }
        }
        error = "Forbidden";
        return code;
    }

    if (code > 0) {
        if (code == 429 && retryAfterS) {
            // Seconds only (the portal's limiter sends seconds); an HTTP date or nothing -> 60 s.
            const String ra = http.header("Retry-After");
            long v = ra.length() > 0 ? ra.toInt() : 0;
            if (v <= 0) v = RETRY_AFTER_DEFAULT_S;
            if (v > 3600) v = 3600;
            *retryAfterS = (int)v;
        }
        if ((code >= 200 && code < 300) || code == 304) {
            portalSuccessAt = millis() ? millis() : 1; // the portal was reached (OTA image confirmation)
        }
        // 204/304 and Content-Length: 0 carry no body. Asking HTTPClient for one anyway makes it
        // wait for the server to close the keep-alive connection: 65 s behind nginx.
        if (code != 204 && code != 304 && http.getSize() != 0) {
            body = http.getString();
        }
        ESP_LOGI(TAG, "%s %s -> %d (%u bytes)", method.c_str(), endpoint.c_str(), code, (unsigned)body.length());
        if (body.length() > 0 && body.length() < 600 && !endpoint.endsWith("/token")) {
            ESP_LOGD(TAG, "body: %s", body.c_str()); // small bodies only (never the token)
        }
        http.end();
        return code;
    }

    static const char* errorMessages[] = {
        "Connection refused", "Send header failed", "Send payload failed",
        "Not connected", "Connection lost", "No stream", "No HTTP server",
        "Too less RAM", "Encoding error", "Stream write error", "Read timeout"
    };
    const char* errorDetail = "Unknown error";
    int errorIndex = -code - 1;
    if (errorIndex >= 0 && errorIndex < 11) {
        errorDetail = errorMessages[errorIndex];
    }
    error = String("HTTP ") + method + " failed: " + String(code) + " (" + errorDetail + ")";
    ESP_LOGW(TAG, "%s", error.c_str());
    {
        char d[40];
        snprintf(d, sizeof(d), "%d %.30s", code, endpoint.c_str());
        deferFault((uint16_t)Fault::PORTAL_UNREACHABLE, d, false);
    }
    http.end();
    return code;
}

JsonDocument RequestManager::responseToDocument(int code, const String& body, const String& error)
{
    if (code > 0 && error.isEmpty()) {
        if (body.isEmpty()) {
            return JsonDocument(&psramJsonAllocator); // e.g. 204: nothing to parse, the status says it all
        }
        return parseResponse(body);
    }
    JsonDocument errorDoc(&psramJsonAllocator);
    errorDoc["error"] = true;
    errorDoc["message"] = error.isEmpty() ? String("HTTP ") + String(code) : error;
    return errorDoc;
}

// Synchronous request on the calling task (blocks it for up to the HTTP timeout).
JsonDocument RequestManager::executeHttpRequest(const String& endpoint, const String& method, const String* payload)
{
    String body, error;
    int code = performHttp(endpoint, method, payload, body, error);
    return responseToDocument(code, body, error);
}

void RequestManager::deferFault(uint16_t code, const char *detail, bool speak)
{
    DeferredFault f;
    f.code = code;
    strlcpy(f.detail, detail ? detail : "", sizeof(f.detail));
    f.speak = speak;
    LockGuard guard(jobLock);
    if (deferredFaults.size() < 8) { // Faults dedupes repeats anyway; never grow without bound
        deferredFaults.push_back(f);
    }
}

void RequestManager::raiseDeferredFaults()
{
    std::vector<DeferredFault> pending;
    {
        LockGuard guard(jobLock);
        if (deferredFaults.empty()) {
            return;
        }
        pending.swap(deferredFaults);
    }
    for (const DeferredFault &f : pending) {
        Faults::raise(static_cast<Fault>(f.code), f.detail[0] ? f.detail : nullptr, f.speak);
    }
}

// ---- Request worker -----------------------------------------------------------------------
// Requests that nothing waits for (figure manifests, OTA checks, status posts) run on a
// worker task that lives only while there is work; the loop task keeps the LEDs, audio and
// the WebSocket alive meanwhile. Callbacks run on the loop task from update().

bool RequestManager::requestAsync(const String &endpoint, const String &method, const String &payload, AsyncCallback cb,
                                  const String &ifNoneMatch, Retry retry)
{
    if (!isNetworkReady()) {
        return false;
    }
    AsyncJob *job = new AsyncJob();
    job->endpoint = endpoint;
    job->method = method;
    job->payload = payload;
    job->ifNoneMatch = ifNoneMatch;
    job->cb = cb;
    job->retry = retry;
    bool startWorker = false;
    {
        LockGuard guard(jobLock);
        jobQueue.push_back(job);
        if (!workerRunning) {
            workerRunning = true;
            startWorker = true;
        }
    }
    if (startWorker && xTaskCreatePinnedToCore(workerEntry, "req", 8192, this, 4, nullptr, 0) != pdPASS) {
        ESP_LOGW(TAG, "request worker could not start - running on the caller");
        workerLoop(); // the old blocking behaviour, results still delivered through update()
    }
    return true;
}

bool RequestManager::postAsync(const String &endpoint, const JsonDocument &data, AsyncCallback cb, Retry retry)
{
    String jsonString;
    serializeJson(data, jsonString);
    return requestAsync(endpoint, "POST", jsonString, cb, String(), retry);
}

bool RequestManager::hasPendingRequests()
{
    LockGuard guard(jobLock);
    return workerRunning || !jobQueue.empty() || !jobDone.empty();
}

bool RequestManager::isRequestPending(const String &endpoint)
{
    LockGuard guard(jobLock);
    if (activeEndpoint == endpoint) {
        return true;
    }
    for (const AsyncJob *job : jobQueue) {
        if (job->endpoint == endpoint) {
            return true;
        }
    }
    return false;
}

void RequestManager::workerEntry(void *arg)
{
    static_cast<RequestManager*>(arg)->workerLoop();
    vTaskDelete(nullptr);
}

// Worker side of the status handling (see Retry in RequestManager.h). Returns true when the
// job was put back into the queue with a later notBefore instead of being answered.
bool RequestManager::requeueIfRetrying(AsyncJob *job)
{
    const unsigned long now = millis();
    const int code = job->status;
    if (code == 429) {
        const int ra = job->retryAfterS > 0 ? job->retryAfterS : RETRY_AFTER_DEFAULT_S;
        throttledUntil = (now + (unsigned long)ra * 1000UL) | 1UL;
        ESP_LOGW(TAG, "%s throttled (429), portal requests paused for %d s", job->endpoint.c_str(), ra);
        if (job->retry != Retry::NONE && job->throttleRetries < THROTTLE_RETRIES) {
            job->throttleRetries++;
            job->notBefore = throttledUntil.load();
        } else {
            return false;
        }
    } else if (code >= 500 && code < 600) {
        const int run = ++consecutive5xx;
        if (run == PORTAL_UNREACHABLE_AFTER_5XX) {
            char d[40];
            snprintf(d, sizeof(d), "http=%d %.26s", code, job->endpoint.c_str());
            deferFault((uint16_t)Fault::PORTAL_UNREACHABLE, d, false);
        }
        if (job->retry == Retry::BACKGROUND && job->serverRetries < SERVER_RETRIES) {
            const unsigned long wait = SERVER_RETRY_MS[job->serverRetries];
            job->serverRetries++;
            job->notBefore = (now + wait) | 1UL;
            ESP_LOGW(TAG, "%s -> %d, retry %d/%d in %lu s", job->endpoint.c_str(), code, job->serverRetries, SERVER_RETRIES,
                     wait / 1000);
        } else {
            return false;
        }
    } else {
        if (code > 0) {
            consecutive5xx = 0; // the portal answers properly again
        }
        return false;
    }
    job->body = String();
    job->error = String();
    LockGuard guard(jobLock);
    jobQueue.push_back(job);
    return true;
}

void RequestManager::workerLoop()
{
    for (;;) {
        AsyncJob *job = nullptr;
        bool throttledAnswer = false;
        {
            LockGuard guard(jobLock);
            if (jobQueue.empty()) {
                workerRunning = false;
                activeEndpoint = String();
                break;
            }
            // First job that may run now: its own retry delay is over and the portal is not
            // throttling us (a NONE-policy job is answered 429 at once instead of waiting).
            const unsigned long now = millis();
            const unsigned long throttle = throttledUntil.load();
            const bool throttled = throttle != 0 && (long)(now - throttle) < 0;
            for (auto it = jobQueue.begin(); it != jobQueue.end(); ++it) {
                AsyncJob *candidate = *it;
                if (candidate->notBefore != 0 && (long)(now - candidate->notBefore) < 0) {
                    continue;
                }
                if (throttled && candidate->retry != Retry::NONE) {
                    continue;
                }
                job = candidate;
                throttledAnswer = throttled; // only a NONE job gets here while throttled
                jobQueue.erase(it);
                break;
            }
            if (job) {
                activeEndpoint = job->endpoint;
                jobStartedAt = millis();
            }
        }
        if (!job) {
            vTaskDelay(pdMS_TO_TICKS(250)); // only delayed jobs left: they keep the worker alive
            continue;
        }
        if (throttledAnswer) {
            job->status = 429;
            job->body = String();
            job->error = String();
            const long left = (long)(throttledUntil.load() - millis()) / 1000;
            job->retryAfterS = left > 0 ? (int)left : 1;
        } else {
            job->retryAfterS = 0;
            job->status = performHttp(job->endpoint, job->method, job->method == "POST" ? &job->payload : nullptr, job->body,
                                      job->error, job->ifNoneMatch, &job->retryAfterS);
            if (requeueIfRetrying(job)) {
                LockGuard guard(jobLock);
                activeEndpoint = String();
                jobStartedAt = 0;
                continue;
            }
        }
        {
            LockGuard guard(jobLock);
            jobDone.push_back(job);
            activeEndpoint = String();
            jobStartedAt = 0;
        }
    }
}

void RequestManager::dispatchCompletedJobs()
{
    for (;;) {
        AsyncJob *job = nullptr;
        {
            LockGuard guard(jobLock);
            if (jobDone.empty()) {
                break;
            }
            job = jobDone.front();
            jobDone.pop_front();
        }
        JsonDocument doc = responseToDocument(job->status, job->body, job->error);
        if (job->status == 429) {
            doc["_retry_after"] = job->retryAfterS > 0 ? job->retryAfterS : RETRY_AFTER_DEFAULT_S;
        }
        if (job->cb) {
            job->cb(doc, job->status); // may queue further requests
        }
        delete job;
    }
    static unsigned long lastStuckLog = 0;
    if (millis() - lastStuckLog > 30000) {
        // The worker writes both under jobLock: copy them out under it.
        String endpoint;
        unsigned long startedAt = 0;
        {
            LockGuard guard(jobLock);
            startedAt = jobStartedAt;
            if (startedAt) {
                endpoint = activeEndpoint;
            }
        }
        if (startedAt && millis() - startedAt > 30000) {
            lastStuckLog = millis();
            ESP_LOGI(TAG, "request %s busy for %lu s", endpoint.c_str(), (millis() - startedAt) / 1000);
        }
    }
}

// Utility methods
bool RequestManager::isConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

String RequestManager::hubPath() const
{
    char buf[32];
    snprintf(buf, sizeof(buf), "/hubs/%llu", ESP.getEfuseMac());
    return String(buf);
}

// ---- Device token (docs/PROTOCOL-V2.md §2) -------------------------------------------------


String RequestManager::getJWTToken()
{
    LockGuard guard(httpLock);
    if (ConfigManager::getInstance().hasFactorySecret()) {
        return fetchDeviceTokenWithSecret();
    }
    return fetchLegacyToken();
}

// Legacy path for units without a factory secret (the fleet before enrollment): unchanged
// GET /hubs/{uuid}/token, answered with the owner's hub token while the portal allows it.
String RequestManager::fetchLegacyToken()
{
    String endpoint = hubPath() + "/token";
    ESP_LOGI(TAG, "Requesting JWT token from endpoint: %s", endpoint.c_str());
    
    // Temporarily clear auth token so the request goes out without the old one
    String savedToken = authToken;
    authToken = "";
    String body, error;
    const int code = performHttp(endpoint, "GET", nullptr, body, error);
    authToken = savedToken;
    JsonDocument doc = responseToDocument(code, body, error);
    
    if (doc["status"] == "success") {
        ESP_LOGI(TAG, "JWT token obtained");
        lastTokenIssuedAt = millis() ? millis() : 1;
        return doc["token"].as<String>();
    }
    if (code == 404) {
        // The portal hands out the legacy token only for a hub bound to exactly one user: this
        // is the "online but not linked" case (it used to look like "offline" to the child).
        linkStateValue = LinkState::NOT_LINKED;
        pendingBound = 0;
    }
    
    ESP_LOGW(TAG, "JWT token request failed (%d): %s", code, doc["message"].as<const char*>() ? doc["message"].as<const char*>() : "?");
    return String();
}

// POST /hubs/{uuid}/token, X-Hub-Secret, no body -> {"token": "...", "expires_in": s}.
String RequestManager::fetchDeviceTokenWithSecret()
{
    const unsigned long now = millis();
    if (tokenBlockedUntil && (long)(now - tokenBlockedUntil) < 0) {
        return String(); // backing off after 404/401/429 (or a transient failure)
    }
    if (!isNetworkReady()) {
        return String();
    }
    const String secret = ConfigManager::getInstance().factorySecret();
    const String endpoint = hubPath() + "/token";
    const String url = baseUrl + endpoint;
    http.end();
    if (!http.begin(*client, url)) {
        tokenBlockedUntil = now + TOKEN_RETRY_SHORT_MS;
        return String();
    }
    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS); // the secret goes to this URL only
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "TilkieTalkie/1.0");
    http.addHeader("X-Hub-Secret", secret); // never logged
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    int code = http.POST((uint8_t *)nullptr, 0);
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    String body;
    if (code > 0 && code != 204 && http.getSize() != 0) {
        body = http.getString();
    }
    http.end();
    ESP_LOGI(TAG, "POST %s -> %d", endpoint.c_str(), code); // never the body (it carries the token)

    if (code == 200 || code == 201) {
        JsonDocument doc(&psramJsonAllocator);
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            const String token = doc["token"].as<String>();
            if (token.length() > 0 && token != "null") {
                const long expires = doc["expires_in"] | 0L;
                ESP_LOGI(TAG, "device token obtained (expires in %ld d)", expires / 86400L);
                tokenBlockedUntil = 0;
                lastTokenIssuedAt = millis() ? millis() : 1;
                return token;
            }
        }
        ESP_LOGW(TAG, "token answer without a token");
        tokenBlockedUntil = millis() + TOKEN_RETRY_SHORT_MS;
        return String();
    }
    if (code == 404 || code == 401) {
        // 404: the portal does not know this unit's secret (not enrolled there); 401: wrong secret.
        ESP_LOGW(TAG, "device token refused (%d): %s - retry in 15 min", code, code == 404 ? "not enrolled" : "secret rejected");
        deferFault((uint16_t)Fault::NOT_ENROLLED, code == 404 ? "404" : "secret", false);
        tokenBlockedUntil = millis() + TOKEN_BACKOFF_MS;
    } else if (code == 429) {
        ESP_LOGW(TAG, "device token request throttled (429) - waiting 15 min");
        tokenBlockedUntil = millis() + TOKEN_BACKOFF_MS;
    } else {
        if (code <= 0) {
            char d[40];
            snprintf(d, sizeof(d), "%d %.30s", code, endpoint.c_str());
            deferFault((uint16_t)Fault::PORTAL_UNREACHABLE, d, false);
        }
        tokenBlockedUntil = millis() + TOKEN_RETRY_SHORT_MS;
    }
    return String();
}

// Validation of a stored token. Enrolled units ask GET /hubs/{uuid} (a device principal may
// call it, and the answer carries the account link too); the legacy path keeps validate-token.
int RequestManager::checkStoredToken(const String &token)
{
    LockGuard guard(httpLock);
    const bool device = ConfigManager::getInstance().hasFactorySecret();
    const String endpoint = device ? hubPath() : hubPath() + "/validate-token";
    
    http.end();
    String url = baseUrl + endpoint;
    
    if (!http.begin(*client, url)) {
        return 0;
    }
    
    http.setTimeout(timeout);
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "TilkieTalkie/1.0");
    http.addHeader("Authorization", "Bearer " + token);
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset(); // only the loop task is subscribed
    int httpResponseCode = http.GET();
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset(); // only the loop task is subscribed
    String body;
    if (device && httpResponseCode == 200 && http.getSize() != 0) {
        body = http.getString();
    }
    http.end();
    if (device && httpResponseCode == 200) {
        JsonDocument doc(&psramJsonAllocator);
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            JsonVariantConst b = doc["bound"];
            pendingBound = b.is<bool>() ? (b.as<bool>() ? 1 : 0) : 1;
        }
    }
    return httpResponseCode;
}

bool RequestManager::validateToken(const String& token)
{
    const int httpResponseCode = checkStoredToken(token);
    if (httpResponseCode <= 0) {
        // No answer (DNS not ready right after connecting, portal down): that says nothing about
        // the token. Keep it; a token the portal really rejects fails with 401 later and is
        // refreshed then. Fetching a new token here revoked a good one on every flaky boot.
        ESP_LOGI(TAG, "token check got no answer (%d) - keeping the stored token", httpResponseCode);
        return true;
    }
    return httpResponseCode == 200;
}

void RequestManager::initConnection()
{
    LockGuard guard(httpLock);
    ConfigManager &config = ConfigManager::getInstance();
    String storedToken = config.getJWTToken();

    if (storedToken.length() > 0) {
        const int code = checkStoredToken(storedToken);
        if (code == 403) {
            // A hub endpoint refused a token it knows (unbound, revoked ability): refresh once
            // per 10 min, otherwise keep using it.
            setAuthToken(storedToken);
            forceTokenRefresh("validation 403");
            return;
        }
        if (code != 401) {
            // 200, or no answer (that says nothing about the token: a flaky boot must not revoke it).
            if (code <= 0) {
                ESP_LOGI(TAG, "token check got no answer (%d) - keeping the stored token", code);
            }
            ESP_LOGI(TAG, "Using stored JWT token");
            setAuthToken(storedToken);
            if (config.hasFactorySecret() && config.getTokenDays() >= TOKEN_REFRESH_DAYS) {
                forceTokenRefresh("80 days");
            }
            if (!nextLinkCheckAt) nextLinkCheckAt = millis() + 2000; // the bound check right after boot
            return;
        }
    }
    
    config.clearJWTToken(); // invalid (401) or none
    
    String newToken = getJWTToken();
    if (newToken.length() > 0) {
        ESP_LOGI(TAG, "New JWT token obtained");
        config.setJWTToken(newToken);
        setAuthToken(newToken);
        nextLinkCheckAt = millis() + 2000;
    } else {
        setAuthToken(String());
    }
}

bool RequestManager::forceTokenRefresh(const char *why)
{
    LockGuard guard(httpLock);
    const unsigned long now = millis();
    if (lastForcedRefreshAt && now - lastForcedRefreshAt < FORCED_REFRESH_MIN_MS) {
        return false;
    }
    lastForcedRefreshAt = now ? now : 1;
    ESP_LOGI(TAG, "refreshing the device token (%s)", why ? why : "?");
    const String fresh = getJWTToken();
    if (fresh.length() > 0) {
        ConfigManager::getInstance().setJWTToken(fresh); // resets token_days
        setAuthToken(fresh);
    }
    return true;
}

String RequestManager::currentOrFreshToken()
{
    LockGuard guard(httpLock);
    if (lastTokenIssuedAt && millis() - lastTokenIssuedAt < FORCED_REFRESH_MIN_MS && authToken.length() > 0) {
        return authToken;
    }
    forceTokenRefresh("channel auth refused");
    return authToken.length() > 0 ? authToken : ConfigManager::getInstance().getJWTToken();
}

void RequestManager::forgetToken()
{
    setAuthToken(String());
    linkStateValue = LinkState::UNKNOWN;
}

bool RequestManager::unbindBestEffort()
{
    LockGuard guard(httpLock);
    String token = authToken.length() > 0 ? authToken : ConfigManager::getInstance().getJWTToken();
    if (token.isEmpty() || !isNetworkReady()) {
        return false;
    }
    const String url = baseUrl + hubPath() + "/unbind";
    http.end();
    if (!http.begin(*client, url)) {
        return false;
    }
    http.setTimeout(3000);
    http.setConnectTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "TilkieTalkie/1.0");
    http.addHeader("Authorization", "Bearer " + token);
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    const int code = http.POST(String("{}"));
    http.end();
    ESP_LOGI(TAG, "unbind -> %d", code);
    return code >= 200 && code < 300;
}

// Loop task. A unit without a token (not enrolled on the portal, throttled, offline at boot)
// tries again every 15 min; a unit with one ages it by a day every 24 h of uptime and renews
// it at 80 days (the 401 path covers the time spent asleep, which this counter does not see).
void RequestManager::serviceToken()
{
    if (!begun) {
        return;
    }
    const unsigned long now = millis();
    if (pendingBound >= 0) {
        JsonDocument doc;
        doc["bound"] = pendingBound == 1;
        pendingBound = -1;
        applyBound(doc.as<JsonVariantConst>());
    }
    if (authToken.isEmpty()) {
        if (isNetworkReady() && (long)(now - nextTokenRetryAt) >= 0) {
            nextTokenRetryAt = now + TOKEN_BACKOFF_MS;
            initConnection();
        }
        return;
    }
    if (nextTokenDayAt == 0) {
        nextTokenDayAt = now + DAY_MS;
    } else if ((long)(now - nextTokenDayAt) >= 0) {
        nextTokenDayAt = now + DAY_MS;
        ConfigManager &config = ConfigManager::getInstance();
        const uint16_t days = config.getTokenDays() + 1;
        config.setTokenDays(days); // the one NVS write of the day
        if (config.hasFactorySecret() && days >= TOKEN_REFRESH_DAYS) {
            forceTokenRefresh("80 days");
        }
    }
}

void RequestManager::applyBound(JsonVariantConst doc)
{
    // Device principal: {"bound": bool, ...}. A legacy owner token gets the full hub resource
    // (possibly under "data"): read "bound" if it is there, otherwise the owner's token proves it.
    JsonVariantConst b = doc["bound"];
    if (!b.is<bool>()) b = doc["data"]["bound"];
    const bool bound = b.is<bool>() ? b.as<bool>() : true;
    if (!bound) {
        // One event when it is first seen and then once a day (the legacy path re-learns it on
        // every token retry, every 15 min). The speaker hands it to UxScenes, which says the
        // not_linked prompt once per boot after the hello, when idle, and at most once a day.
        static unsigned long lastRaisedAt = 0;
        const bool first = linkStateValue != LinkState::NOT_LINKED;
        linkStateValue = LinkState::NOT_LINKED;
        if (first || !lastRaisedAt || millis() - lastRaisedAt >= DAY_MS) {
            lastRaisedAt = millis() ? millis() : 1;
            ESP_LOGI(TAG, "hub is not linked to an account");
            Faults::raise(Fault::NOT_LINKED, "bound=false");
        }
    } else {
        if (linkStateValue == LinkState::NOT_LINKED) {
            ESP_LOGI(TAG, "hub is linked to an account now");
        }
        linkStateValue = LinkState::LINKED;
        Faults::clear(Fault::NOT_LINKED);
    }
}

void RequestManager::serviceLinkCheck()
{
    const unsigned long now = millis();
    if (!begun || !nextLinkCheckAt || (long)(now - nextLinkCheckAt) < 0 || authToken.isEmpty() || !isNetworkReady()) {
        return;
    }
    const String endpoint = hubPath();
    if (isRequestPending(endpoint)) {
        return;
    }
    nextLinkCheckAt = now + DAY_MS;
    getAsync(endpoint, [this](JsonDocument &doc, int status) {
        if (status == 200) {
            applyBound(doc.as<JsonVariantConst>());
        } else {
            ESP_LOGI(TAG, "link check failed (%d) - again in 30 min", status);
            nextLinkCheckAt = millis() + 30UL * 60UL * 1000UL;
        }
    });
}

void RequestManager::getCheckFigureTracks(const String &uid)
{
    ESP_LOGI(TAG, "Processing figure tracks request");

    // Cache-first: whatever is already on the card plays immediately; the backend is then
    // asked for changes. A down portal, an expired token or slow WiFi must never keep a
    // known figure silent.
    const bool emitted = emitFromCache(uid);

    if (isNetworkReady()) {
        processOnlineFigureRequest(uid, emitted);
    } else if (!emitted) {
        processOfflineFigureRequest(uid);
    }
}

void RequestManager::processOnlineFigureRequest(const String &uid, bool alreadyEmitted)
{
    // The manifest fetch runs on the request worker; handleUnitManifest() continues on the
    // loop task once the answer is in. A tap repeated while the answer is pending is one request.
    const String endpoint = "/units/" + uid;
    if (isRequestPending(endpoint)) {
        ESP_LOGI(TAG, "manifest request already pending for this figure");
        return;
    }
    // INTERACTIVE: a tap waits for this answer; a 5xx is answered at once (the cache already
    // plays), a 429 waits for Retry-After and is re-sent.
    bool started = getAsync(endpoint, [this, uid, alreadyEmitted](JsonDocument &doc, int status) {
        handleUnitManifest(uid, alreadyEmitted, doc, status);
    }, Retry::INTERACTIVE);
    if (!started) {
        JsonDocument doc(&psramJsonAllocator);
        doc["error"] = true;
        doc["message"] = "Network not ready";
        handleUnitManifest(uid, alreadyEmitted, doc, 0);
    }
}

void RequestManager::handleUnitManifest(const String &uid, bool alreadyEmitted, JsonDocument &doc, int status)
{
    // Status first (docs/PROTOCOL-V2.md §4): 410 = the card was deleted on the portal, purge it;
    // 404 = no such unit (the cache, if any, is kept); anything else that is not 200 (offline,
    // 401/403 after the token refresh, 429 after its re-sends, 5xx) = the portal could not answer,
    // which is not a statement about the card: the cache is kept.
    if (status == 410) {
        purgeRevokedUnit(uid);
        if (!alreadyEmitted && figureDownloadCompleteCallback) {
            Figure emptyFigure;
            queueFigureDownloadCompletion(uid, "revoked", false, "No figure: card revoked on the portal", emptyFigure);
        }
        return;
    }
    if (status == 404) {
        if (!alreadyEmitted && figureDownloadCompleteCallback) {
            Figure emptyFigure;
            queueFigureDownloadCompletion(uid, "null", false, "No figure data found", emptyFigure);
        }
        return;
    }
    if (status != 200 && !doc["error"].as<bool>()) {
        ESP_LOGW(TAG, "manifest fetch for %s answered %d", uid.c_str(), status);
        if (!alreadyEmitted && figureDownloadCompleteCallback) {
            Figure emptyFigure;
            queueFigureDownloadCompletion(uid, "Unknown", false,
                                          String("Backend unreachable (http=") + status + ") and nothing cached", emptyFigure);
        }
        return;
    }
    if (doc["error"].as<bool>()) {
        ESP_LOGW(TAG, "manifest fetch failed (%s)", doc["message"].as<const char*>() ? doc["message"].as<const char*>() : "?");
        if (!alreadyEmitted && figureDownloadCompleteCallback) {
            Figure emptyFigure;
            queueFigureDownloadCompletion(uid, "Unknown", false, "Backend unreachable and nothing cached", emptyFigure);
        }
        return;
    }

    JsonObject figure = doc["figure"].as<JsonObject>();
    if (figure.isNull()) {
        if (!alreadyEmitted && figureDownloadCompleteCallback) {
            Figure emptyFigure;
            queueFigureDownloadCompletion(uid, "null", false, "No figure data found", emptyFigure);
        }
        return;
    }

    FileManager &fileManager = FileManager::getInstance();
    String figureId = jsonValueToString(figure["id"]);
    String figureName = figure["name"].as<String>();
    JsonArray assetManifest = doc["asset_manifest"].as<JsonArray>();
    if (assetManifest.isNull()) {
        if (!alreadyEmitted && figureDownloadCompleteCallback) {
            Figure emptyFigure;
            queueFigureDownloadCompletion(uid, figureName, false, "Manifest is missing asset_manifest", emptyFigure);
        }
        return;
    }

    storeUidToFigureIdMapping(uid, figureId);

    // Unchanged manifest (same manifest_checksum as the cached copy): nothing to re-save,
    // just make sure every asset is present.
    JsonDocument previousManifest(&psramJsonAllocator);
    String previousManifestJson;
    bool haveCached = loadUnitManifest(uid, previousManifest);
    const String newChecksum = doc["manifest_checksum"].as<String>();
    const String oldChecksum = haveCached ? previousManifest["manifest_checksum"].as<String>() : String();
    const bool unchanged = haveCached && !newChecksum.isEmpty() && newChecksum == oldChecksum;
    if (haveCached) {
        serializeJson(previousManifest, previousManifestJson);
    }

    std::vector<String> trackPaths;
    int assetsToDownload = 0;
    int assetsAlreadyCached = 0;
    for (JsonVariant assetVar : assetManifest) {
        JsonObject asset = assetVar.as<JsonObject>();
        String storagePath = normalizeStoragePath(asset["storage_path"].as<String>());
        String url = asset["url"].as<String>();
        String checksum = asset["checksum"].as<String>();
        if (storagePath.isEmpty()) {
            continue;
        }
        if (checksum.isEmpty()) {
            // Unverifiable content over plain HTTP is never downloaded (PROTOCOL-V2 §8: the
            // portal always sends a checksum). A copy already on the card stays playable.
            ESP_LOGW(TAG, "asset %s has no checksum - not downloaded", storagePath.c_str());
            Faults::raise(Fault::DOWNLOAD_FAILED, "nosha", false);
            if (!fileManager.isPresent(storagePath)) {
                continue;
            }
        }
        trackPaths.push_back(storagePath);
        // A file replaced on the portal keeps its path but gets a new checksum: the copy on the
        // card is stale. Unless it is the track playing right now, drop it and download again.
        // A file queued for deletion counts as absent (it is downloaded again once it is gone).
        bool present = fileManager.isPresent(storagePath);
        if (present && !checksum.isEmpty()) {
            const String known = fileManager.requiredFileChecksum(storagePath);
            if (!known.isEmpty() && known != checksum) {
                if (AudioController::getInstance().getCurrentTrack() == storagePath) {
                    ESP_LOGI(TAG, "%s was replaced on the portal but is playing - refreshed on the next tap", storagePath.c_str());
                } else {
                    ESP_LOGI(TAG, "%s was replaced on the portal - downloading the new file", storagePath.c_str());
                    // deleteFile() removes the file only: drop the stale entry explicitly; it is
                    // re-added with the new checksum just below.
                    fileManager.removeRequiredFile(storagePath, false);
                    fileManager.deleteFile(storagePath);
                    present = false;
                }
            }
        }
        fileManager.addRequiredFile(storagePath, url, checksum, false); // flushed once below
        ESP_LOGI(TAG, "asset %s (%s) %s", storagePath.c_str(), asset["asset_type"].as<const char*>() ? asset["asset_type"].as<const char*>() : "?",
                      present ? "present" : "missing");
        // Existence only: integrity was verified (sha256) when the file was downloaded.
        // Re-hashing every cached track here used to block the loop for tens of seconds.
        if (!present) {
            if (url.length() > 0) {
                fileManager.scheduleDownload(url, storagePath, checksum, false); // flushed once below
                assetsToDownload++;
            }
        } else {
            assetsAlreadyCached++;
        }
    }

    fileManager.flushRequiredFiles(); // one SD write for the whole manifest instead of one per asset
    fileManager.flushDownloadQueue();

    if (unchanged && assetsToDownload == 0) {
        ESP_LOGI(TAG, "manifest for %s unchanged, all %d assets present", figureName.c_str(), assetsAlreadyCached);
        if (!alreadyEmitted) {
            // Nothing was playable from the cache (should not happen when all assets exist) - announce now.
            Figure figureData = buildFigureFromManifest(doc, true);
            queueFigureDownloadCompletion(uid, figureName, !figureData.contents.empty() || !figureData.customTracks.empty(),
                                          "", figureData, false);
        }
        return;
    }

    String pendingManifestJson;
    bool hasPending = false;
    if (!unchanged) {
        serializeJson(doc, pendingManifestJson);
        hasPending = savePendingUnitManifest(uid, doc);
        if (!hasPending) {
            ESP_LOGW(TAG, "failed to cache pending manifest (continuing without stale-asset cleanup)");
        }
    }

    Figure figureData = buildFigureFromManifest(doc);
    startTrackingFigure(uid, figureName, figureId, trackPaths, figureData, hasPending, pendingManifestJson, previousManifestJson, alreadyEmitted);
    if (hasPending) {
        requestInventory(); // the portal's stories list shows the figure as downloading
    }

    if (assetsToDownload > 0) {
        ESP_LOGI(TAG, "%d asset(s) to download for %s (%d already cached)", assetsToDownload, figureName.c_str(), assetsAlreadyCached);
        if (!alreadyEmitted && figureEventCallback) {
            figureEventCallback(uid, FigureEvent::DOWNLOADS_STARTED);
        }
    }
}

bool RequestManager::emitFromCache(const String &uid)
{
    JsonDocument manifest(&psramJsonAllocator);
    if (!loadUnitManifest(uid, manifest)) {
        return false;
    }
    Figure cached = buildFigureFromManifest(manifest, true);
    if (cached.contents.empty() && cached.customTracks.empty()) {
        return false;
    }
    ESP_LOGI(TAG, "cache hit for %s (%s) - playing what is on the card",
                  uid.c_str(), cached.name.c_str());
    queueFigureDownloadCompletion(uid, cached.name, true, "", cached, false);
    return true;
}

RequestManager::Figure RequestManager::filterExisting(const Figure &figure) const
{
    FileManager &fileManager = FileManager::getInstance();
    Figure out;
    out.id = figure.id;
    out.name = figure.name;
    out.description = figure.description;
    out.type = figure.type;
    out.manifestChecksum = figure.manifestChecksum;
    for (const auto &content : figure.contents) {
        Content c = content;
        c.episodes.clear();
        for (const auto &episode : content.episodes) {
            Episode e = episode;
            e.tracks.clear();
            for (const auto &track : episode.tracks) {
                if (fileManager.isPresent(track.localPath)) {
                    e.tracks.push_back(track);
                }
            }
            if (!e.tracks.empty()) {
                c.episodes.push_back(e);
            }
        }
        if (!c.episodes.empty()) {
            out.contents.push_back(c);
        }
    }
    for (const auto &track : figure.customTracks) {
        if (fileManager.isPresent(track.localPath)) {
            out.customTracks.push_back(track);
        }
    }
    return out;
}

bool RequestManager::firstTrackExists(const Figure &figure) const
{
    FileManager &fileManager = FileManager::getInstance();
    for (const auto &content : figure.contents) {
        for (const auto &episode : content.episodes) {
            for (const auto &track : episode.tracks) {
                return fileManager.isPresent(track.localPath);
            }
        }
    }
    if (!figure.customTracks.empty()) {
        return fileManager.isPresent(figure.customTracks.front().localPath);
    }
    return false;
}

void RequestManager::emitTrackerState(FigureDownloadTracker &tracker)
{
    Figure playable = filterExisting(tracker.figureData);
    if (playable.contents.empty() && playable.customTracks.empty()) {
        return;
    }
    queueFigureDownloadCompletion(tracker.uid, tracker.figureName, true, "", playable, tracker.emittedAny);
    tracker.emittedAny = true;
}

void RequestManager::processOfflineFigureRequest(const String &uid)
{
    // Only reached when there is no network AND emitFromCache() found nothing playable.
    ESP_LOGI(TAG, "offline and nothing cached for this figure");
    Figure emptyFigure;
    queueFigureDownloadCompletion(uid, "Unknown", false, "Offline and no cached tracks", emptyFigure);
}

void RequestManager::setFigureDownloadCompleteCallback(FigureDownloadCompleteCallback callback)
{
    this->figureDownloadCompleteCallback = callback;
}

void RequestManager::queueFigureDownloadCompletion(const String &uid, const String &figureName, bool success, const String &error, const Figure &figureData, bool isUpdate)
{
    PendingFigureCompletion completion;
    completion.uid = uid;
    completion.figureName = figureName;
    completion.success = success;
    completion.error = error;
    completion.figureData = figureData;
    completion.isUpdate = isUpdate;
    pendingCompletions.push_back(completion);
}

void RequestManager::startTrackingFigure(const String &uid, const String &figureName, const String &figureId, 
                                          const std::vector<String> &trackPaths, const Figure &figureData,
                                          bool hasPendingManifest, const String &pendingManifestJson,
                                          const String &previousManifestJson, bool alreadyEmitted)
{
    // Remove completed trackers
    activeDownloads.erase(
        std::remove_if(activeDownloads.begin(), activeDownloads.end(),
            [](const FigureDownloadTracker& t) { return t.completed; }),
        activeDownloads.end()
    );
    
    // Remove existing tracker for this UID
    activeDownloads.erase(
        std::remove_if(activeDownloads.begin(), activeDownloads.end(),
            [&uid](const FigureDownloadTracker& t) { return t.uid == uid; }),
        activeDownloads.end()
    );
    
    FigureDownloadTracker tracker;
    tracker.uid = uid;
    tracker.figureName = figureName;
    tracker.figureId = figureId;
    tracker.totalTracks = trackPaths.size();
    tracker.trackPaths = trackPaths;
    tracker.figureData = figureData;
    tracker.hasPendingManifest = hasPendingManifest;
    tracker.pendingManifestJson = pendingManifestJson;
    tracker.previousManifestJson = previousManifestJson;
    tracker.emittedAny = alreadyEmitted;
    
    FileManager &fileManager = FileManager::getInstance();
    for (const String &path : trackPaths) {
        if (fileManager.isPresent(path)) {
            tracker.tracksReady++;
        }
    }
    
    bool allReady = (tracker.tracksReady >= tracker.totalTracks && tracker.totalTracks > 0);
    ESP_LOGI(TAG, "tracking %s: %d asset(s), %d present, pending manifest %s, announced %s",
                  figureName.c_str(), tracker.totalTracks, tracker.tracksReady,
                  hasPendingManifest ? "yes" : "no", alreadyEmitted ? "yes" : "no");
    
    if (allReady) {
        tracker.completed = true;
        finalizeTrackedManifest(tracker, true);
        queueFigureDownloadCompletion(uid, figureName, true, "", figureData, tracker.emittedAny);
        tracker.emittedAny = true;
    } else if (tracker.totalTracks == 0) {
        // The backend reports no assets for this unit. Deliberately NOT promoted over the
        // cached manifest (which would also garbage-collect the files): an empty list can be a
        // revoked binding or a backend hiccup, and wiping a child's figure on a hiccup is worse
        // than playing stale content. Revisit once the API carries an explicit "revoked" state.
        tracker.completed = true;
        finalizeTrackedManifest(tracker, false);
        ESP_LOGW(TAG, "WARNING backend reports no assets for %s (%s); cached content kept",
                      figureName.c_str(), uid.c_str());
        if (!tracker.emittedAny) {
            queueFigureDownloadCompletion(uid, figureName, false, "Figure has no tracks", figureData);
        }
    } else if (!tracker.emittedAny && firstTrackExists(figureData)) {
        // Play what is already here while the rest downloads.
        emitTrackerState(tracker);
    }
    
    activeDownloads.push_back(tracker);
}

void RequestManager::checkFigureDownloadStatus(const String &uid)
{
    for (auto &tracker : activeDownloads) {
        if (tracker.uid == uid && !tracker.completed) {
            if (tracker.tracksReady + tracker.tracksFailed >= tracker.totalTracks) {
                tracker.completed = true;
                bool anyReady = tracker.tracksReady > 0;
                // The manifest is promoted even if some assets failed permanently: it still
                // describes the figure; the missing files are retried on the next tap.
                finalizeTrackedManifest(tracker, anyReady);
                if (anyReady) {
                    if (tracker.tracksFailed > 0) {
                        ESP_LOGW(TAG, "%s: %d track(s) could not be downloaded, playing the rest",
                                      tracker.figureName.c_str(), tracker.tracksFailed);
                    }
                    emitTrackerState(tracker);
                } else if (!tracker.emittedAny) {
                    queueFigureDownloadCompletion(uid, tracker.figureName, false, "No track could be downloaded", tracker.figureData);
                }
            }
            break;
        }
    }
}

void RequestManager::onTrackDownloadComplete(const String &path, bool success)
{
    // Credit every tracker that references this path (figures can share assets).
    for (auto &tracker : activeDownloads) {
        if (tracker.completed) {
            continue;
        }
        bool referenced = false;
        for (const String &trackPath : tracker.trackPaths) {
            if (trackPath == path) {
                referenced = true;
                break;
            }
        }
        if (!referenced) {
            continue;
        }
        if (success) {
            tracker.tracksReady++;
            // First playable state, or an update with one more track: announce it (the
            // playback side merges without restarting).
            if (!tracker.emittedAny) {
                if (firstTrackExists(tracker.figureData)) {
                    emitTrackerState(tracker);
                }
            } else if (tracker.tracksReady + tracker.tracksFailed < tracker.totalTracks) {
                emitTrackerState(tracker);
            }
        } else {
            tracker.tracksFailed++;
        }
        checkFigureDownloadStatus(tracker.uid);
    }
}

void RequestManager::staticFileDownloadCallback(const String& url, const String& path, bool success, const String& error)
{
    if (path.startsWith(PromptPackManager::STAGING_DIR))
    {
        PromptPackManager::getInstance().onFileDone(path, success); // voice prompt pack, not a story
        return;
    }
    RequestManager &instance = RequestManager::getInstance("http://portal.tilkietalkie.com/api");
    instance.onTrackDownloadComplete(path, success);
}

void RequestManager::storeUidToFigureIdMapping(const String &uid, const String &figureId)
{
    for (auto it = uidLru.begin(); it != uidLru.end(); ++it) {
        if (it->first != uid) continue;
        const bool same = it->second == figureId;
        uidLru.erase(it);
        uidLru.emplace_back(uid, figureId); // most recent last
        if (same) {
            return; // only the order changed: no NVS write on every tap (persisted with the next change)
        }
        saveUidMappings();
        return;
    }
    uidLru.emplace_back(uid, figureId);
    while (uidLru.size() > UID_MAP_MAX) {
        uidLru.erase(uidLru.begin()); // least recently used
    }
    saveUidMappings();
}

String RequestManager::getFigureIdFromUid(const String &uid)
{
    for (const auto &e : uidLru) {
        if (e.first == uid) return e.second;
    }
    // Not mapped (evicted, or never fetched this way): the cached manifest knows it too.
    JsonDocument manifest(&psramJsonAllocator);
    if (loadUnitManifest(uid, manifest)) {
        return jsonValueToString(manifest["figure"]["id"]);
    }
    return String();
}

void RequestManager::forgetAllUidMappings()
{
    uidLru.clear();
    if (!initializeNVS()) return;
    nvs_erase_key(nvsHandle, NVS_UID_LRU_KEY);
    nvs_erase_key(nvsHandle, NVS_UID_MAPPING_KEY);
    nvs_commit(nvsHandle);
    ESP_LOGI(TAG, "uid mappings cleared");
}

// NVS operations
bool RequestManager::initializeNVS()
{
    if (nvsHandle != 0) {
        return true;
    }
    // The partition is initialised (and, if it was full, repaired without losing the factory
    // identity) by ConfigManager during static construction; erasing it here would wipe it all.
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvsHandle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    return true;
}

bool RequestManager::saveUidMappings()
{
    if (nvsHandle == 0) return false;
    
    JsonDocument doc(&psramJsonAllocator);
    JsonArray list = doc.to<JsonArray>();
    for (const auto &e : uidLru) {
        JsonArray pair = list.add<JsonArray>();
        pair.add(e.first);
        pair.add(e.second);
    }
    String jsonString;
    serializeJson(doc, jsonString);
    
    esp_err_t err = nvs_set_str(nvsHandle, NVS_UID_LRU_KEY, jsonString.c_str());
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE || err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // This namespace cannot be written any more: erase it alone (never the partition, which
        // also holds the factory identity and the WiFi credentials) and write the list again.
        ESP_LOGW(TAG, "NVS full writing the uid mappings - erasing namespace %s", NVS_NAMESPACE);
        nvs_erase_all(nvsHandle);
        nvs_commit(nvsHandle);
        deferFault((uint16_t)Fault::CONFIG_CORRUPT, "requestmgr full, erased", false);
        err = nvs_set_str(nvsHandle, NVS_UID_LRU_KEY, jsonString.c_str());
    }
    if (err != ESP_OK) return false;
    
    return nvs_commit(nvsHandle) == ESP_OK;
}

// Reads a string key into a PSRAM buffer (caller frees with heap_caps_free); null when absent.
// Parsed straight from PSRAM: an oversized legacy value must not need a copy in internal RAM.
static char *readNvsString(nvs_handle_t h, const char *key)
{
    size_t size = 0;
    if (nvs_get_str(h, key, nullptr, &size) != ESP_OK || size == 0) return nullptr;
    char *buf = (char *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) return nullptr;
    if (nvs_get_str(h, key, buf, &size) != ESP_OK) {
        heap_caps_free(buf);
        return nullptr;
    }
    return buf;
}

bool RequestManager::loadUidMappings()
{
    if (nvsHandle == 0) return false;
    uidLru.clear();

    char *json = readNvsString(nvsHandle, NVS_UID_LRU_KEY);
    if (json) {
        JsonDocument doc(&psramJsonAllocator);
        const bool ok = deserializeJson(doc, json) == DeserializationError::Ok;
        heap_caps_free(json);
        if (ok) {
            for (JsonArrayConst pair : doc.as<JsonArrayConst>()) {
                if (pair.size() == 2) uidLru.emplace_back(pair[0].as<String>(), jsonValueToString(pair[1]));
            }
        }
        while (uidLru.size() > UID_MAP_MAX) uidLru.erase(uidLru.begin());
        ESP_LOGI(TAG, "Loaded %u UID mappings", (unsigned)uidLru.size());
        return true;
    }

    // One-time migration of the old unbounded {"uid":"id",...} object (any size). Units whose
    // manifest is still on the card count as the most recent; the rest is dropped first.
    json = readNvsString(nvsHandle, NVS_UID_MAPPING_KEY);
    if (!json) return true;
    size_t total = 0;
    {
        JsonDocument doc(&psramJsonAllocator);
        const bool ok = deserializeJson(doc, json) == DeserializationError::Ok;
        heap_caps_free(json);
        if (ok) {
            std::vector<std::pair<String, String>> cached;
            FileManager &fm = FileManager::getInstance();
            for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
                total++;
                String uid = pair.key().c_str();
                String id = jsonValueToString(pair.value());
                if (fm.fileExists(getUnitManifestPath(uid))) cached.emplace_back(uid, id); else uidLru.emplace_back(uid, id);
            }
            uidLru.insert(uidLru.end(), cached.begin(), cached.end());
            while (uidLru.size() > UID_MAP_MAX) uidLru.erase(uidLru.begin());
        }
    }
    if (saveUidMappings()) {
        nvs_erase_key(nvsHandle, NVS_UID_MAPPING_KEY);
        nvs_commit(nvsHandle);
    }
    ESP_LOGI(TAG, "uid mappings migrated: %u of %u kept", (unsigned)uidLru.size(), (unsigned)total);
    return true;
}

bool RequestManager::saveUnitManifest(const String &uid, const JsonDocument &manifest)
{
    FileManager &fileManager = FileManager::getInstance();
    fileManager.createDirectory(SD_DATA_DIR);
    fileManager.createDirectory(SD_MANIFEST_DIR);
    String json;
    if (serializeJson(manifest, json) == 0) {
        ESP_LOGW(TAG, "Failed to serialize manifest for unit: %s", uid.c_str());
        return false;
    }
    // Written by the SD maintenance task (tmp + rename); the loop never waits for the card.
    if (!fileManager.writeFileDeferred(getUnitManifestPath(uid), json)) {
        ESP_LOGW(TAG, "Failed to queue manifest for unit: %s", uid.c_str());
        return false;
    }
    ESP_LOGI(TAG, "Saved manifest for unit: %s", uid.c_str());
    return true;
}

bool RequestManager::savePendingUnitManifest(const String &uid, const JsonDocument &manifest)
{
    FileManager &fileManager = FileManager::getInstance();
    fileManager.createDirectory(SD_DATA_DIR);
    fileManager.createDirectory(SD_PENDING_MANIFEST_DIR);
    String json;
    if (serializeJson(manifest, json) == 0) {
        ESP_LOGW(TAG, "Failed to serialize pending manifest for unit: %s", uid.c_str());
        return false;
    }
    // Written by the SD maintenance task (tmp + rename); the loop never waits for the card.
    if (!fileManager.writeFileDeferred(getPendingUnitManifestPath(uid), json)) {
        ESP_LOGW(TAG, "Failed to queue pending manifest for unit: %s", uid.c_str());
        return false;
    }
    ESP_LOGI(TAG, "Saved pending manifest for unit: %s", uid.c_str());
    return true;
}

bool RequestManager::loadUnitManifest(const String &uid, JsonDocument &manifest)
{
    File file;
    if (!FileManager::getInstance().openJsonForRead(getUnitManifestPath(uid).c_str(), file)) {
        return false; // neither the manifest nor its .tmp (crash between remove and rename)
    }

    DeserializationError error = deserializeJson(manifest, file);
    file.close();

    if (error) {
        ESP_LOGW(TAG, "Failed to parse cached manifest for %s: %s", uid.c_str(), error.c_str());
        return false;
    }

    return true;
}

bool RequestManager::deletePendingUnitManifest(const String &uid)
{
    // Queued behind any pending write of the same file (FIFO), so the order is preserved.
    return FileManager::getInstance().deleteFileDeferred(getPendingUnitManifestPath(uid));
}

bool RequestManager::loadPendingUnitManifest(const String &uid, JsonDocument &manifest)
{
    File file;
    if (!FileManager::getInstance().openJsonForRead(getPendingUnitManifestPath(uid).c_str(), file)) {
        return false;
    }

    DeserializationError error = deserializeJson(manifest, file);
    file.close();

    if (error) {
        ESP_LOGW(TAG, "Failed to parse pending manifest for %s: %s", uid.c_str(), error.c_str());
        return false;
    }

    return true;
}

String RequestManager::getUnitManifestPath(const String &uid) const
{
    return String(SD_MANIFEST_DIR) + "/" + uid + ".json";
}

String RequestManager::getPendingUnitManifestPath(const String &uid) const
{
    return String(SD_PENDING_MANIFEST_DIR) + "/" + uid + ".json";
}

String RequestManager::normalizeStoragePath(const String &path) const
{
    if (path.isEmpty()) {
        return String();
    }

    return path.startsWith("/") ? path : "/" + path;
}

void RequestManager::finalizeTrackedManifest(FigureDownloadTracker &tracker, bool success)
{
    if (!tracker.hasPendingManifest) {
        return;
    }

    if (!success) {
        deletePendingUnitManifest(tracker.uid);
        tracker.hasPendingManifest = false;
        tracker.pendingManifestJson = String();
        tracker.previousManifestJson = String();
        return;
    }

    JsonDocument pendingManifest(&psramJsonAllocator);
    bool hasPendingManifestData = false;

    if (!tracker.pendingManifestJson.isEmpty()) {
        DeserializationError pendingError = deserializeJson(pendingManifest, tracker.pendingManifestJson);
        if (pendingError) {
            ESP_LOGW(TAG, "Failed to parse in-memory pending manifest for %s: %s", tracker.uid.c_str(), pendingError.c_str());
        } else {
            hasPendingManifestData = true;
        }
    }

    if (!hasPendingManifestData && !loadPendingUnitManifest(tracker.uid, pendingManifest)) {
        ESP_LOGI(TAG, "Missing pending manifest while finalizing unit %s", tracker.uid.c_str());
        tracker.hasPendingManifest = false;
        tracker.pendingManifestJson = String();
        tracker.previousManifestJson = String();
        return;
    }

    JsonDocument previousManifest(&psramJsonAllocator);
    JsonDocument* previousManifestPtr = nullptr;
    if (!tracker.previousManifestJson.isEmpty()) {
        if (!deserializeJson(previousManifest, tracker.previousManifestJson)) {
            previousManifestPtr = &previousManifest;
        }
    }

    removeStaleAssets(tracker.uid, pendingManifest, previousManifestPtr);

    if (saveUnitManifest(tracker.uid, pendingManifest)) {
        deletePendingUnitManifest(tracker.uid);
        requestInventory(); // queued behind the writes above: the portal sees the final state
    }

    tracker.hasPendingManifest = false;
    tracker.pendingManifestJson = String();
    tracker.previousManifestJson = String();
}

RequestManager::Figure RequestManager::buildFigureFromManifest(const JsonDocument &manifest, bool existingFilesOnly) const
{
    Figure figure;
    JsonObjectConst figureObject = manifest["figure"].as<JsonObjectConst>();

    figure.id = jsonValueToString(figureObject["id"]);
    figure.name = figureObject["name"].as<String>();
    figure.description = figureObject["description"].as<String>();
    figure.type = figureObject["type"].as<String>();
    figure.manifestChecksum = manifest["manifest_checksum"].as<String>();

    FileManager &fileManager = FileManager::getInstance();

    JsonArrayConst contents = manifest["contents"].as<JsonArrayConst>();
    for (JsonVariantConst contentVariant : contents) {
        JsonObjectConst contentObject = contentVariant.as<JsonObjectConst>();
        Content content;
        content.id = jsonValueToString(contentObject["id"]);
        content.name = contentObject["name"].as<String>();
        content.description = contentObject["description"].as<String>();
        content.type = contentObject["type"].as<String>();
        content.sortOrder = contentObject["sort_order"] | 0;

        JsonArrayConst episodes = contentObject["episodes"].as<JsonArrayConst>();
        for (JsonVariantConst episodeVariant : episodes) {
            JsonObjectConst episodeObject = episodeVariant.as<JsonObjectConst>();
            Episode episode;
            episode.id = jsonValueToString(episodeObject["id"]);
            episode.name = episodeObject["name"].as<String>();
            episode.description = episodeObject["description"].as<String>();
            episode.sortOrder = episodeObject["sort_order"] | 0;

            JsonArrayConst tracks = episodeObject["tracks"].as<JsonArrayConst>();
            for (JsonVariantConst trackVariant : tracks) {
                JsonObjectConst trackObject = trackVariant.as<JsonObjectConst>();
                Track track;
                track.id = jsonValueToString(trackObject["id"]);
                track.name = trackObject["name"].as<String>();
                track.description = trackObject["description"].as<String>();
                track.audioUrl = trackObject["audio_url"].as<String>();
                track.duration = trackObject["duration"] | 0;
                track.sortOrder = trackObject["sort_order"] | 0;
                track.checksum = trackObject["checksum"].as<String>();
                track.localPath = normalizeStoragePath(trackObject["storage_path"].as<String>());

                if (existingFilesOnly && !fileManager.isPresent(track.localPath)) {
                    continue;
                }

                episode.tracks.push_back(track);
            }

            if (!episode.tracks.empty()) {
                content.episodes.push_back(episode);
            }
        }

        if (!content.episodes.empty()) {
            figure.contents.push_back(content);
        }
    }

    JsonArrayConst customTracks = manifest["custom_tracks"].as<JsonArrayConst>();
    for (JsonVariantConst customTrackVariant : customTracks) {
        JsonObjectConst customTrackObject = customTrackVariant.as<JsonObjectConst>();
        Track track;
        track.id = jsonValueToString(customTrackObject["id"]);
        track.name = customTrackObject["name"].as<String>();
        track.description = customTrackObject["description"].as<String>();
        track.audioUrl = customTrackObject["audio_url"].as<String>();
        track.duration = customTrackObject["duration"] | 0;
        track.sortOrder = customTrackObject["sort_order"] | 0;
        track.checksum = customTrackObject["checksum"].as<String>();
        track.localPath = normalizeStoragePath(customTrackObject["storage_path"].as<String>());

        if (existingFilesOnly && !fileManager.isPresent(track.localPath)) {
            continue;
        }

        figure.customTracks.push_back(track);
    }

    return figure;
}

std::vector<String> RequestManager::extractAssetPaths(const JsonDocument &manifest) const
{
    std::vector<String> paths;

    JsonArrayConst assetManifest = manifest["asset_manifest"].as<JsonArrayConst>();
    if (!assetManifest.isNull()) {
        for (JsonVariantConst assetVariant : assetManifest) {
            String normalizedPath = normalizeStoragePath(assetVariant["storage_path"].as<String>());
            if (!normalizedPath.isEmpty()) {
                paths.push_back(normalizedPath);
            }
        }
        return paths;
    }

    JsonArrayConst contents = manifest["contents"].as<JsonArrayConst>();
    for (JsonVariantConst contentVariant : contents) {
        JsonArrayConst episodes = contentVariant["episodes"].as<JsonArrayConst>();
        for (JsonVariantConst episodeVariant : episodes) {
            JsonArrayConst tracks = episodeVariant["tracks"].as<JsonArrayConst>();
            for (JsonVariantConst trackVariant : tracks) {
                String normalizedPath = normalizeStoragePath(trackVariant["storage_path"].as<String>());
                if (!normalizedPath.isEmpty()) {
                    paths.push_back(normalizedPath);
                }
            }
        }
    }

    JsonArrayConst customTracks = manifest["custom_tracks"].as<JsonArrayConst>();
    for (JsonVariantConst customTrackVariant : customTracks) {
        String normalizedPath = normalizeStoragePath(customTrackVariant["storage_path"].as<String>());
        if (!normalizedPath.isEmpty()) {
            paths.push_back(normalizedPath);
        }
    }

    return paths;
}

std::vector<String> RequestManager::assetsReferencedByOtherUnits(const String &currentUid) const
{
    std::vector<String> referenced;
    File manifestDir = SD_MMC.open(SD_MANIFEST_DIR);
    if (!manifestDir || !manifestDir.isDirectory()) {
        return referenced;
    }

    String currentManifestFile = currentUid + ".json";
    File entry = manifestDir.openNextFile();
    while (entry) {
        String entryName = String(entry.name());
        if (!entry.isDirectory() && entryName != currentManifestFile) {
            JsonDocument manifest(&psramJsonAllocator);
            DeserializationError error = deserializeJson(manifest, entry);
            entry.close();
            if (!error) {
                std::vector<String> assetPaths = extractAssetPaths(manifest);
                referenced.insert(referenced.end(), assetPaths.begin(), assetPaths.end());
            }
        } else {
            entry.close();
        }
        entry = manifestDir.openNextFile();
    }

    manifestDir.close();
    return referenced;
}

void RequestManager::removeStaleAssets(const String &uid, const JsonDocument &currentManifest, const JsonDocument *previousManifest)
{
    if (previousManifest == nullptr) {
        return;
    }

    std::vector<String> currentPaths = extractAssetPaths(currentManifest);
    std::vector<String> stale;
    for (const String &oldPath : extractAssetPaths(*previousManifest)) {
        if (std::find(currentPaths.begin(), currentPaths.end(), oldPath) == currentPaths.end()) {
            stale.push_back(oldPath);
        }
    }
    if (stale.empty()) {
        return;
    }

    // This runs on the loop task: scan the other units' manifests once (not once per asset),
    // rewrite the required-files list once, and leave the slow card erases to the SD
    // maintenance task.
    std::vector<String> referenced = assetsReferencedByOtherUnits(uid);
    FileManager &fileManager = FileManager::getInstance();
    bool requiredChanged = false;
    for (const String &oldPath : stale) {
        if (std::find(referenced.begin(), referenced.end(), oldPath) != referenced.end()) {
            continue;
        }
        requiredChanged = fileManager.removeRequiredFile(oldPath, false) || requiredChanged;
        if (fileManager.fileExists(oldPath)) {
            fileManager.deleteFileDeferred(oldPath);
        }
        ESP_LOGI(TAG, "stale asset queued for deletion: %s", oldPath.c_str());
    }
    if (requiredChanged) {
        fileManager.flushRequiredFiles();
    }
}

// ---- Stories inventory for the portal ------------------------------------------------------

void RequestManager::requestInventory()
{
    FileManager::getInstance().enqueueInventory();
}

// Runs on the SD maintenance task: reads every cached manifest, sizes the tracks, and queues one
// event per figure ({"seq","total","sd","figure"} without the outer braces; ReverbClient wraps it).
void RequestManager::buildInventory()
{
    FileManager &fm = FileManager::getInstance();
    // Final manifests (figures fully on the card) plus pending ones (figures still downloading).
    std::vector<String> uids;
    std::vector<bool> pendingOnly;
    for (const String &name : fm.listFiles(SD_MANIFEST_DIR)) {
        if (name.endsWith(".json")) {
            uids.push_back(name.substring(0, name.length() - 5));
            pendingOnly.push_back(false);
        }
    }
    for (const String &name : fm.listFiles(SD_PENDING_MANIFEST_DIR)) {
        if (!name.endsWith(".json")) continue;
        const String uid = name.substring(0, name.length() - 5);
        bool known = false;
        for (size_t i = 0; i < uids.size(); ++i) {
            if (uids[i] == uid) { known = true; pendingOnly[i] = true; break; } // being updated
        }
        if (!known) {
            uids.push_back(uid);
            pendingOnly.push_back(true);
        }
    }
    const uint32_t freeMb = fm.getSDCardFreeMB();
    const uint32_t totalMb = (uint32_t)(SD_MMC.totalBytes() >> 20);
    char sd[64];
    snprintf(sd, sizeof(sd), "\"sd\":{\"free_mb\":%u,\"total_mb\":%u}", (unsigned)freeMb, (unsigned)totalMb);
    if (uids.empty()) {
        ReverbClient::queueDeviceFiles(String("\"seq\":0,\"total\":0,") + sd + ",\"figure\":null");
        return;
    }
    for (size_t i = 0; i < uids.size(); ++i) {
        appendInventoryFigure(uids[i], pendingOnly[i], i + 1, uids.size(), sd);
    }
}

// One figure of the "client-device-files" inventory, as one or more fragments (ReverbClient wraps
// each into a frame). pending: describe the manifest being downloaded (the final one, if any, is
// what the figure played before the update).
//
// Wire format (agreed with the portal, docs/CONTENT-SYNC.md "Inventory"): every fragment of a
// figure carries the same seq/total; a figure whose tracks do not fit one frame is split into
// parts with top-level "part" (1-based) and "parts" (<= 32), each repeating the figure's
// uid/id/name/bytes/state/truncated with only its slice of "tracks". Single-part figures carry
// no part/parts (as before). "truncated": true marks a figure with more than 60 tracks (only
// the first 60 are listed). Every frame stays under 9,000 bytes (Reverb drops > 10,000).
void RequestManager::appendInventoryFigure(const String &uid, bool pending, size_t seq, size_t total, const char *sd)
{
    static const int INV_MAX_TRACKS = 60;
    static const size_t INV_TRACKS_BUDGET = 7800;   // bytes of track objects per frame (frame overhead + figure fields < 1,100)
    static const int INV_MAX_PARTS = 32;
    static const size_t INV_NAME_MAX = 120;          // a long title must not need a frame of its own
    FileManager &fm = FileManager::getInstance();
    JsonDocument manifest(&psramJsonAllocator);
    bool loaded = pending ? loadPendingUnitManifest(uid, manifest) : loadUnitManifest(uid, manifest);
    if (!loaded && pending) {
        loaded = loadUnitManifest(uid, manifest);
    }
    JsonDocument all(&psramJsonAllocator);       // every listed track, split below
    JsonArray tracks = all.to<JsonArray>();
    uint64_t bytes = 0;
    int count = 0, cachedCount = 0;
    bool truncated = false;
    auto addTrack = [&](JsonObjectConst t) {
        String path = normalizeStoragePath(t["storage_path"].as<String>());
        if (path.isEmpty()) return;
        if (count >= INV_MAX_TRACKS) { truncated = true; return; }
        JsonObject o = tracks.add<JsonObject>();
        o["id"] = jsonValueToString(t["id"]);
        String name = t["name"].as<String>();
        o["name"] = utf8Truncate(name, INV_NAME_MAX);
        o["path"] = path;
        size_t size = 0;
        bool cached = false;
        if (!fm.isDeletePending(path)) {
            File f = SD_MMC.open(path);
            if (f) {
                size = f.size();
                cached = !f.isDirectory();
                f.close();
            }
        }
        o["size"] = (uint32_t)size;
        o["cached"] = cached;
        bytes += size;
        count++;
        if (cached) cachedCount++;
    };
    if (loaded) {
        for (JsonVariantConst c : manifest["contents"].as<JsonArrayConst>())
            for (JsonVariantConst e : c["episodes"].as<JsonArrayConst>())
                for (JsonVariantConst t : e["tracks"].as<JsonArrayConst>())
                    addTrack(t.as<JsonObjectConst>());
        for (JsonVariantConst t : manifest["custom_tracks"].as<JsonArrayConst>())
            addTrack(t.as<JsonObjectConst>());
    }

    // Slice the tracks so each fragment's track list stays within the budget.
    std::vector<std::pair<size_t, size_t>> slices; // [first, end)
    size_t first = 0, used = 0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const size_t len = measureJson(tracks[i]) + 1;
        if (i > first && used + len > INV_TRACKS_BUDGET) {
            slices.push_back({first, i});
            first = i;
            used = 0;
        }
        used += len;
    }
    slices.push_back({first, tracks.size()}); // also the empty figure: one fragment, no tracks
    if ((int)slices.size() > INV_MAX_PARTS) {
        slices.resize(INV_MAX_PARTS);
        truncated = true;
    }

    // downloading: the manifest is not final yet (tracks still arriving); partial: final but
    // some tracks are missing (deleted from the portal page or a failed download); ready: all there.
    const char *state = pending ? "downloading" : (count > 0 && cachedCount == count ? "ready" : "partial");
    const size_t parts = slices.size();
    String figureName = loaded ? manifest["figure"]["name"].as<String>() : String("?");
    figureName = utf8Truncate(figureName, INV_NAME_MAX);
    for (size_t p = 0; p < parts; ++p) {
        JsonDocument out(&psramJsonAllocator);
        out["seq"] = (int)seq;
        out["total"] = (int)total;
        if (parts > 1) {
            out["part"] = (int)(p + 1);
            out["parts"] = (int)parts;
        }
        JsonObject fig = out["figure"].to<JsonObject>();
        fig["uid"] = uid;
        fig["id"] = loaded ? jsonValueToString(manifest["figure"]["id"]) : String();
        fig["name"] = figureName;
        fig["bytes"] = (uint32_t)bytes;
        fig["state"] = state;
        fig["truncated"] = truncated;
        JsonArray slice = fig["tracks"].to<JsonArray>();
        for (size_t i = slices[p].first; i < slices[p].second; ++i) {
            slice.add(tracks[i]);
        }
        String json;
        serializeJson(out, json);
        // "{...}" -> "..." so the sender can prepend device_id; sd is inserted here as text
        String inner = json.substring(1, json.length() - 1);
        ReverbClient::queueDeviceFiles(inner + "," + String(sd));
    }
}

bool RequestManager::forgetFigure(const String &uid)
{
    FileManager &fileManager = FileManager::getInstance();
    AudioController &audio = AudioController::getInstance();
    if (audio.hasPlaylist() && audio.getPlaylistFigureUid() == uid) {
        audio.stop();
        audio.clearPlaylist();
    }
    JsonDocument manifest(&psramJsonAllocator);
    std::vector<String> paths;
    if (loadUnitManifest(uid, manifest)) {
        paths = extractAssetPaths(manifest);
    }
    JsonDocument pending(&psramJsonAllocator);
    if (loadPendingUnitManifest(uid, pending)) { // still downloading: its assets too
        for (const String &p : extractAssetPaths(pending)) {
            if (std::find(paths.begin(), paths.end(), p) == paths.end()) paths.push_back(p);
        }
    }
    const std::vector<String> keep = assetsReferencedByOtherUnits(uid);
    int deleted = 0;
    for (const String &path : paths) {
        bool shared = false;
        for (const String &k : keep) {
            if (k == path) { shared = true; break; }
        }
        if (shared) continue;
        fileManager.cancelQueuedDownload(path);
        fileManager.removeRequiredFile(path, false);
        if (fileManager.fileExists(path)) {
            fileManager.deleteFileDeferred(path);
        }
        deleted++;
    }
    fileManager.flushRequiredFiles();
    fileManager.deleteFileDeferred(getUnitManifestPath(uid));
    deletePendingUnitManifest(uid);
    for (auto it = activeDownloads.begin(); it != activeDownloads.end();) {
        if (it->uid == uid) it = activeDownloads.erase(it); else ++it;
    }
    for (auto it = uidLru.begin(); it != uidLru.end(); ++it) {
        if (it->first == uid) {
            uidLru.erase(it);
            saveUidMappings();
            break;
        }
    }
    ESP_LOGI(TAG, "forgot figure %s (%d file(s) queued for deletion, %u shared kept)",
                  uid.c_str(), deleted, (unsigned)keep.size());
    return true;
}

// The portal answered 410 {"revoked": true}: the card no longer exists there. Its manifest,
// pending manifest, the assets no other cached card uses and their queued downloads go (the same
// clean-up as the hub page's delete-figure). Never on 404 or a network error.
void RequestManager::purgeRevokedUnit(const String &uid)
{
    FileManager &fm = FileManager::getInstance();
    const bool cached = fm.fileExists(getUnitManifestPath(uid)) || fm.fileExists(getPendingUnitManifestPath(uid));
    ESP_LOGW(TAG, "card %s was revoked on the portal - %s", uid.c_str(), cached ? "purging its cache" : "nothing cached");
    if (cached) {
        forgetFigure(uid);
        requestInventory();
    }
}

bool RequestManager::forgetTrack(const String &path)
{
    if (!path.startsWith("/assets/")) {
        return false;
    }
    FileManager &fileManager = FileManager::getInstance();
    AudioController &audio = AudioController::getInstance();
    if (audio.getCurrentTrack() == path) {
        audio.stop();
    }
    fileManager.removeRequiredFile(path, true);
    fileManager.deleteFileDeferred(path);
    ESP_LOGI(TAG, "forgot track %s", path.c_str());
    return true;
}

// ---- Content sync (docs/CONTENT-SYNC.md) ---------------------------------------------------

bool RequestManager::isFigureDownloading(const String &uid) const
{
    for (const auto &tracker : activeDownloads) {
        if (tracker.uid == uid && !tracker.completed) {
            return true;
        }
    }
    return false;
}

void RequestManager::refreshUnit(const String &uid)
{
    FileManager &fm = FileManager::getInstance();
    const bool cached = fm.fileExists(getUnitManifestPath(uid)) || fm.fileExists(getPendingUnitManifestPath(uid));
    if (!cached) {
        ESP_LOGI(TAG, "sync-figure %s: not cached here, ignored", uid.c_str());
        return;
    }
    if (!isNetworkReady()) {
        ESP_LOGI(TAG, "sync-figure %s: offline, the next re-check will do it", uid.c_str());
        return;
    }
    ESP_LOGI(TAG, "sync-figure %s: re-fetching the manifest", uid.c_str());
    // alreadyEmitted=true: a background refresh - no prompts, no playback announcement for a
    // figure that is not on the pins; a mounted figure merges the new list like a tap does.
    processOnlineFigureRequest(uid, true);
}

void RequestManager::recoverPendingManifests()
{
    FileManager &fm = FileManager::getInstance();
    fm.setDownloadCompleteCallback(staticFileDownloadCallback); // begin() repeats this later, harmless
    if (!fm.fileExists(SD_PENDING_MANIFEST_DIR)) {
        return;
    }
    int recovered = 0;
    for (const String &name : fm.listFiles(SD_PENDING_MANIFEST_DIR)) {
        if (!name.endsWith(".json")) continue;
        const String uid = name.substring(0, name.length() - 5);
        JsonDocument pending(&psramJsonAllocator);
        if (!loadPendingUnitManifest(uid, pending)) {
            deletePendingUnitManifest(uid); // unreadable: the next tap fetches a fresh one
            continue;
        }
        String pendingJson;
        serializeJson(pending, pendingJson);
        JsonDocument previous(&psramJsonAllocator);
        String previousJson;
        if (loadUnitManifest(uid, previous)) {
            serializeJson(previous, previousJson);
        }
        // The persisted download queue normally still holds the missing files; make sure.
        std::vector<String> trackPaths;
        for (JsonVariantConst asset : pending["asset_manifest"].as<JsonArrayConst>()) {
            const String path = normalizeStoragePath(asset["storage_path"].as<String>());
            if (path.isEmpty()) continue;
            trackPaths.push_back(path);
            const String url = asset["url"].as<String>();
            const String checksum = asset["checksum"].as<String>();
            if (checksum.isEmpty() && !fm.isPresent(path)) {
                Faults::raise(Fault::DOWNLOAD_FAILED, "nosha", false); // never fetched unverified
                trackPaths.pop_back();
                continue;
            }
            fm.addRequiredFile(path, url, checksum, false);
            if (!fm.isPresent(path) && url.length() > 0) {
                fm.scheduleDownload(url, path, checksum, false);
            }
        }
        fm.flushRequiredFiles();
        fm.flushDownloadQueue();
        Figure figureData = buildFigureFromManifest(pending);
        JsonObjectConst figure = pending["figure"].as<JsonObjectConst>();
        startTrackingFigure(uid, figure["name"].as<String>(), jsonValueToString(figure["id"]), trackPaths, figureData,
                            true, pendingJson, previousJson, /*alreadyEmitted=*/true);
        recovered++;
    }
    if (recovered > 0) {
        ESP_LOGI(TAG, "%d interrupted figure download(s) resumed", recovered);
    }
}

void RequestManager::serviceManifestRecheck()
{
    const unsigned long now = millis();
    if (nextRecheckAt == 0) {
        nextRecheckAt = now + RECHECK_FIRST_MS + (esp_random() % 60000);
        return;
    }
    if (!recheckQueue.empty()) {
        if (hasPendingRequests() || !isNetworkReady()) {
            return; // one request at a time, behind anything a tap needs
        }
        const String uid = recheckQueue.front();
        recheckQueue.erase(recheckQueue.begin());
        recheckUnit(uid);
        return;
    }
    if ((long)(now - nextRecheckAt) < 0 || !isNetworkReady()) {
        return;
    }
    nextRecheckAt = now + RECHECK_INTERVAL_MS;
    for (const String &name : FileManager::getInstance().listFiles(SD_MANIFEST_DIR)) {
        if (name.endsWith(".json")) {
            recheckQueue.push_back(name.substring(0, name.length() - 5));
        }
    }
    if (!recheckQueue.empty()) {
        ESP_LOGI(TAG, "checking %u cached figure(s) for content changes", (unsigned)recheckQueue.size());
    }
}

void RequestManager::recheckUnit(const String &uid)
{
    if (isFigureDownloading(uid)) {
        return; // its manifest was just fetched
    }
    JsonDocument manifest(&psramJsonAllocator);
    if (!loadUnitManifest(uid, manifest)) {
        return;
    }
    const String checksum = manifest["manifest_checksum"].as<String>();
    const String endpoint = "/units/" + uid;
    if (isRequestPending(endpoint)) {
        return;
    }
    requestAsync(endpoint, "GET", String(), [this, uid](JsonDocument &doc, int status) {
        if (status == 304) {
            ESP_LOGI(TAG, "%s unchanged", uid.c_str());
            return;
        }
        if (status == 410) {
            purgeRevokedUnit(uid);
            return;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "re-check of %s failed (%d)", uid.c_str(), status);
            char d[40];
            snprintf(d, sizeof(d), "%.24s http=%d", uid.c_str(), status);
            Faults::raise(Fault::CONTENT_SYNC_FAILED, d, false);
            return;
        }
        ESP_LOGI(TAG, "%s changed on the portal - updating the cache", uid.c_str());
        handleUnitManifest(uid, true, doc, status);
    }, checksum, Retry::BACKGROUND);
}
