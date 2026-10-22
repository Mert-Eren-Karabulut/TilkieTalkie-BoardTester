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
const char* RequestManager::SD_DATA_DIR = "/.requestmanager";
const char* RequestManager::SD_MANIFEST_DIR = "/.requestmanager/manifests";
const char* RequestManager::SD_PENDING_MANIFEST_DIR = "/.requestmanager/pending_manifests";
static const unsigned long RECHECK_FIRST_MS = 3UL * 60UL * 1000UL;        // after boot (plus up to 60 s jitter)
static const unsigned long RECHECK_INTERVAL_MS = 12UL * 60UL * 60UL * 1000UL; // the portal also pushes sync-figure

namespace {
struct LockGuard {
    SemaphoreHandle_t m;
    explicit LockGuard(SemaphoreHandle_t h) : m(h) { if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY); }
    ~LockGuard() { if (m) xSemaphoreGiveRecursive(m); }
};

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
    if (WiFi.status() != WL_CONNECTED) {
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
                                const String& ifNoneMatch)
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
            ConfigManager::getInstance().setJWTToken(String());
            initConnection();
            int retried = authToken.length() > 0 ? performHttp(endpoint, method, payload, body, error) : 0;
            retrying = false;
            if (authToken.length() > 0) {
                return retried;
            }
        }
        error = "Unauthorized";
        deferFault((uint16_t)Fault::TOKEN_REJECTED, endpoint.substring(0, 39).c_str(), true);
        return code;
    }

    if (code > 0) {
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
                                  const String &ifNoneMatch)
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

bool RequestManager::postAsync(const String &endpoint, const JsonDocument &data, AsyncCallback cb)
{
    String jsonString;
    serializeJson(data, jsonString);
    return requestAsync(endpoint, "POST", jsonString, cb);
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

void RequestManager::workerLoop()
{
    for (;;) {
        AsyncJob *job = nullptr;
        {
            LockGuard guard(jobLock);
            if (jobQueue.empty()) {
                workerRunning = false;
                activeEndpoint = String();
                break;
            }
            job = jobQueue.front();
            jobQueue.pop_front();
            activeEndpoint = job->endpoint;
            jobStartedAt = millis();
        }
        job->status = performHttp(job->endpoint, job->method, job->method == "POST" ? &job->payload : nullptr, job->body, job->error,
                                  job->ifNoneMatch);
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

String RequestManager::getJWTToken()
{
    LockGuard guard(httpLock);
    uint64_t macAddress = ESP.getEfuseMac();
    
    String endpoint = "/hubs/" + String(macAddress, 10) + "/token";
    //print endpoint being accessed
    ESP_LOGI(TAG, "Requesting JWT token from endpoint: %s", endpoint.c_str());
    
    // Temporarily clear auth token so executeHttpRequest doesn't try to init connection
    String savedToken = authToken;
    authToken = "";
    
    JsonDocument doc = executeHttpRequest(endpoint, "GET");
    
    authToken = savedToken;
    
    if (doc["status"] == "success") {
        ESP_LOGI(TAG, "JWT token obtained");
        return doc["token"].as<String>();
    }
    
    ESP_LOGW(TAG, "JWT token request failed: %s", doc["message"].as<const char*>() ? doc["message"].as<const char*>() : "?");
    return String();
}

bool RequestManager::validateToken(const String& token)
{
    LockGuard guard(httpLock);
    uint64_t macAddress = ESP.getEfuseMac();
    String endpoint = "/hubs/" + String(macAddress, 10) + "/validate-token";
    
    http.end();
    String url = baseUrl + endpoint;
    
    if (!http.begin(*client, url)) {
        return false;
    }
    
    http.setTimeout(timeout);
    setDefaultHeaders();
    http.addHeader("Authorization", "Bearer " + token);
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset(); // only the loop task is subscribed
    int httpResponseCode = http.GET();
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset(); // only the loop task is subscribed
    http.end();
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

    if (storedToken.length() > 0 && validateToken(storedToken)) {
        ESP_LOGI(TAG, "Using stored JWT token");
        setAuthToken(storedToken);
        return;
    }
    
    config.setJWTToken(String()); // Clear invalid token
    
    String newToken = getJWTToken();
    if (newToken.length() > 0) {
        ESP_LOGI(TAG, "New JWT token obtained");
        config.setJWTToken(newToken);
        setAuthToken(newToken);
    } else {
        setAuthToken(String());
    }
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
    bool started = getAsync(endpoint, [this, uid, alreadyEmitted](JsonDocument &doc, int) {
        handleUnitManifest(uid, alreadyEmitted, doc);
    });
    if (!started) {
        JsonDocument doc(&psramJsonAllocator);
        doc["error"] = true;
        doc["message"] = "Network not ready";
        handleUnitManifest(uid, alreadyEmitted, doc);
    }
}

void RequestManager::handleUnitManifest(const String &uid, bool alreadyEmitted, JsonDocument &doc)
{
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

// One "client-device-files" fragment. pending: describe the manifest being downloaded (the
// final one, if any, is what the figure played before the update).
void RequestManager::appendInventoryFigure(const String &uid, bool pending, size_t seq, size_t total, const char *sd)
{
    FileManager &fm = FileManager::getInstance();
    JsonDocument manifest(&psramJsonAllocator);
    bool loaded = pending ? loadPendingUnitManifest(uid, manifest) : loadUnitManifest(uid, manifest);
    if (!loaded && pending) {
        loaded = loadUnitManifest(uid, manifest);
    }
    JsonDocument out(&psramJsonAllocator);
    out["seq"] = (int)seq;
    out["total"] = (int)total;
    JsonObject fig = out["figure"].to<JsonObject>();
    fig["uid"] = uid;
    fig["id"] = loaded ? jsonValueToString(manifest["figure"]["id"]) : String();
    fig["name"] = loaded ? manifest["figure"]["name"].as<String>() : String("?");
    JsonArray tracks = fig["tracks"].to<JsonArray>();
    uint64_t bytes = 0;
    int count = 0, cachedCount = 0;
    auto addTrack = [&](JsonObjectConst t) {
        if (count >= 60) return;
        String path = normalizeStoragePath(t["storage_path"].as<String>());
        if (path.isEmpty()) return;
        JsonObject o = tracks.add<JsonObject>();
        o["id"] = jsonValueToString(t["id"]);
        o["name"] = t["name"].as<String>();
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
    fig["bytes"] = (uint32_t)bytes;
    // downloading: the manifest is not final yet (tracks still arriving); partial: final but
    // some tracks are missing (deleted from the portal page or a failed download); ready: all there.
    fig["state"] = pending ? "downloading" : (count > 0 && cachedCount == count ? "ready" : "partial");
    String json;
    serializeJson(out, json);
    // "{...}" -> "..." so the sender can prepend device_id; sd is inserted here as text
    String inner = json.substring(1, json.length() - 1);
    ReverbClient::queueDeviceFiles(inner + "," + String(sd));
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
        if (status != 200) {
            ESP_LOGW(TAG, "re-check of %s failed (%d)", uid.c_str(), status);
            char d[40];
            snprintf(d, sizeof(d), "%.24s http=%d", uid.c_str(), status);
            Faults::raise(Fault::CONTENT_SYNC_FAILED, d, false);
            return;
        }
        ESP_LOGI(TAG, "%s changed on the portal - updating the cache", uid.c_str());
        handleUnitManifest(uid, true, doc);
    }, checksum);
}
