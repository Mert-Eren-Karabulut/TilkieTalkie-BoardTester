#include "RequestManager.h"
#include "ReverbClient.h"
#include "CaBundle.h"
#include "PsramJson.h"

// Initialize static members
const char* RequestManager::NVS_NAMESPACE = "requestmgr";
const char* RequestManager::NVS_UID_MAPPING_KEY = "uid_mappings";
const char* RequestManager::SD_DATA_DIR = "/.requestmanager";
const char* RequestManager::SD_MANIFEST_DIR = "/.requestmanager/manifests";
const char* RequestManager::SD_PENDING_MANIFEST_DIR = "/.requestmanager/pending_manifests";

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
    , lastStatusCode(0)
    , figureDownloadCompleteCallback(nullptr)
    , nvsHandle(0)
{
    httpLock = xSemaphoreCreateRecursiveMutex();
    jobLock = xSemaphoreCreateRecursiveMutex();
    setBaseUrl(baseUrl);
    // Convert HTTPS to HTTP for memory efficiency
    this->baseUrl = baseUrl;
    if (this->baseUrl.startsWith("https://")) {
        this->baseUrl.replace("https://", "http://");
    }
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
    uidToFigureIdMap.clear();
}

// Initialization
bool RequestManager::begin()
{
    Serial.println(F("RequestManager: Initializing..."));
    
    if (!initializeNVS()) {
        Serial.println(F("RequestManager: Failed to initialize NVS"));
        return false;
    }
    
    loadUidMappings();
    
    FileManager &fileManager = FileManager::getInstance();
    fileManager.createDirectory(SD_DATA_DIR);
    fileManager.createDirectory(SD_MANIFEST_DIR);
    fileManager.createDirectory(SD_PENDING_MANIFEST_DIR);
    fileManager.setDownloadCompleteCallback(staticFileDownloadCallback);

    initConnection();

    Serial.println(authToken.length() > 0 ? F("RequestManager: Initialized with token") 
                                           : F("RequestManager: No token available"));
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
    Serial.printf("RequestManager: API base %s (%s)\n", baseUrl.c_str(),
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
    dispatchCompletedJobs();

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
        lastError = String("JSON parsing error: ") + error.c_str();
        Serial.println("RequestManager: " + lastError);
        
        JsonDocument errorDoc(&psramJsonAllocator);
        errorDoc["error"] = true;
        errorDoc["message"] = lastError;
        return errorDoc;
    }

    return doc;
}

// One HTTP exchange. Serialized by httpLock: the synchronous API runs it on the caller (boot,
// token refresh), the asynchronous API on the request worker task - never two at once.
int RequestManager::performHttp(const String& endpoint, const String& method, const String* payload, String& body, String& error)
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

    int code = (method == "POST" && payload) ? http.POST(*payload) : http.GET();

    if (code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_SEND_HEADER_FAILED ||
        code == HTTPC_ERROR_SEND_PAYLOAD_FAILED || code == HTTPC_ERROR_NOT_CONNECTED ||
        code == HTTPC_ERROR_CONNECTION_LOST) {
        // A kept-alive connection the server has since closed (nginx drops idle ones after
        // 65 s) fails at the first write. Drop it and try once more on a fresh connection.
        http.end();
        client->stop();
        Serial.printf("RequestManager: %s %s failed (%d) on a reused connection - retrying\n", method.c_str(), endpoint.c_str(), code);
        if (!http.begin(*client, url)) {
            error = "Failed to connect to " + url;
            return 0;
        }
        http.setTimeout(timeout);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        setDefaultHeaders();
        code = (method == "POST" && payload) ? http.POST(*payload) : http.GET();
    }

    if (code == 401 && !tokenEndpoint) {
        // Token revoked server-side (e.g. another hub of the same user fetched a new one):
        // re-acquire once and retry, instead of staying dead until the next reboot.
        static bool retrying = false;
        http.end();
        if (!retrying) {
            retrying = true;
            Serial.println("RequestManager: 401 - refreshing hub token and retrying once");
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
        return code;
    }

    if (code > 0) {
        // 204/304 and Content-Length: 0 carry no body. Asking HTTPClient for one anyway makes it
        // wait for the server to close the keep-alive connection: 65 s behind nginx.
        if (code != 204 && code != 304 && http.getSize() != 0) {
            body = http.getString();
        }
        Serial.printf("RequestManager: %s %s -> %d (%u bytes)\n", method.c_str(), endpoint.c_str(), code, (unsigned)body.length());
        if (body.length() > 0 && body.length() < 600 && !endpoint.endsWith("/token")) {
            Serial.println("RequestManager: body: " + body); // small bodies only (never the token)
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
    Serial.println("RequestManager: " + error);
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
    lastError = error.isEmpty() ? String("HTTP ") + String(code) : error;
    JsonDocument errorDoc(&psramJsonAllocator);
    errorDoc["error"] = true;
    errorDoc["message"] = lastError;
    return errorDoc;
}

// Synchronous request on the calling task (blocks it for up to the HTTP timeout).
JsonDocument RequestManager::executeHttpRequest(const String& endpoint, const String& method, const String* payload)
{
    String body, error;
    int code = performHttp(endpoint, method, payload, body, error);
    lastStatusCode = code;
    return responseToDocument(code, body, error);
}

// ---- Request worker -----------------------------------------------------------------------
// Requests that nothing waits for (figure manifests, OTA checks, status posts) run on a
// worker task that lives only while there is work; the loop task keeps the LEDs, audio and
// the WebSocket alive meanwhile. Callbacks run on the loop task from update().

bool RequestManager::requestAsync(const String &endpoint, const String &method, const String &payload, AsyncCallback cb)
{
    if (!isNetworkReady()) {
        return false;
    }
    AsyncJob *job = new AsyncJob();
    job->endpoint = endpoint;
    job->method = method;
    job->payload = payload;
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
        Serial.println("RequestManager: request worker could not start - running on the caller");
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
        job->status = performHttp(job->endpoint, job->method, job->method == "POST" ? &job->payload : nullptr, job->body, job->error);
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
        lastStatusCode = job->status;
        if (job->cb) {
            job->cb(doc, job->status); // may queue further requests
        }
        delete job;
    }
    static unsigned long lastStuckLog = 0;
    if (jobStartedAt && millis() - jobStartedAt > 30000 && millis() - lastStuckLog > 30000) {
        lastStuckLog = millis();
        Serial.printf("RequestManager: request %s busy for %lu s\n", activeEndpoint.c_str(), (millis() - jobStartedAt) / 1000);
    }
}

// HTTP Methods - now simplified using executeHttpRequest
JsonDocument RequestManager::get(const String &endpoint)
{
    return executeHttpRequest(endpoint, "GET");
}

JsonDocument RequestManager::post(const String &endpoint, const JsonDocument &data)
{
    String jsonString;
    serializeJson(data, jsonString);
    return executeHttpRequest(endpoint, "POST", &jsonString);
}

// Utility methods
bool RequestManager::isConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

String RequestManager::getLastError()
{
    return lastError;
}

int RequestManager::getLastStatusCode()
{
    return lastStatusCode;
}

String RequestManager::getJWTToken()
{
    LockGuard guard(httpLock);
    uint64_t macAddress = ESP.getEfuseMac();
    
    String endpoint = "/hubs/" + String(macAddress, 10) + "/token";
    //print endpoint being accessed
    Serial.println("RequestManager: Requesting JWT token from endpoint: " + endpoint);
    
    // Temporarily clear auth token so executeHttpRequest doesn't try to init connection
    String savedToken = authToken;
    authToken = "";
    
    JsonDocument doc = executeHttpRequest(endpoint, "GET");
    
    authToken = savedToken;
    
    if (doc["status"] == "success") {
        Serial.println(F("RequestManager: JWT token obtained"));
        return doc["token"].as<String>();
    }
    
    lastError = doc["message"].as<String>();
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

    int httpResponseCode = http.GET();
    http.end();
    
    return httpResponseCode == 200;
}

void RequestManager::initConnection()
{
    LockGuard guard(httpLock);
    ConfigManager &config = ConfigManager::getInstance();
    String storedToken = config.getJWTToken();

    if (storedToken.length() > 0 && validateToken(storedToken)) {
        Serial.println(F("RequestManager: Using stored JWT token"));
        setAuthToken(storedToken);
        return;
    }
    
    config.setJWTToken(String()); // Clear invalid token
    
    String newToken = getJWTToken();
    if (newToken.length() > 0) {
        Serial.println(F("RequestManager: New JWT token obtained"));
        config.setJWTToken(newToken);
        setAuthToken(newToken);
    } else {
        setAuthToken(String());
    }
}

void RequestManager::getCheckFigureTracks(const String &uid)
{
    Serial.println(F("RequestManager: Processing figure tracks request"));

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
        Serial.println("RequestManager: manifest request already pending for this figure");
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
        Serial.printf("RequestManager: manifest fetch failed (%s)\n", doc["message"].as<const char*>() ? doc["message"].as<const char*>() : "?");
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
        fileManager.addRequiredFile(storagePath, url, checksum, false); // flushed once below
        Serial.printf("RequestManager: asset %s (%s) %s\n", storagePath.c_str(), asset["asset_type"].as<const char*>() ? asset["asset_type"].as<const char*>() : "?",
                      fileManager.fileExists(storagePath) ? "present" : "missing");
        // Existence only: integrity was verified (sha256) when the file was downloaded.
        // Re-hashing every cached track here used to block the loop for tens of seconds.
        if (!fileManager.fileExists(storagePath)) {
            if (url.length() > 0) {
                fileManager.scheduleDownload(url, storagePath, checksum);
                assetsToDownload++;
            }
        } else {
            assetsAlreadyCached++;
        }
    }

    fileManager.flushRequiredFiles(); // one SD write for the whole manifest instead of one per asset

    if (unchanged && assetsToDownload == 0) {
        Serial.printf("RequestManager: manifest for %s unchanged, all %d assets present\n", figureName.c_str(), assetsAlreadyCached);
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
            Serial.println("RequestManager: failed to cache pending manifest (continuing without stale-asset cleanup)");
        }
    }

    Figure figureData = buildFigureFromManifest(doc);
    startTrackingFigure(uid, figureName, figureId, trackPaths, figureData, hasPending, pendingManifestJson, previousManifestJson, alreadyEmitted);

    if (assetsToDownload > 0) {
        Serial.printf("RequestManager: %d asset(s) to download for %s (%d already cached)\n", assetsToDownload, figureName.c_str(), assetsAlreadyCached);
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
    Serial.printf("RequestManager: cache hit for %s (%s) - playing what is on the card\n",
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
                if (fileManager.fileExists(track.localPath)) {
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
        if (fileManager.fileExists(track.localPath)) {
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
                return fileManager.fileExists(track.localPath);
            }
        }
    }
    if (!figure.customTracks.empty()) {
        return fileManager.fileExists(figure.customTracks.front().localPath);
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
    Serial.println(F("RequestManager: offline and nothing cached for this figure"));
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
        if (fileManager.fileExists(path)) {
            tracker.tracksReady++;
        }
    }
    
    bool allReady = (tracker.tracksReady >= tracker.totalTracks && tracker.totalTracks > 0);
    Serial.printf("RequestManager: tracking %s: %d asset(s), %d present, pending manifest %s, announced %s\n",
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
        Serial.printf("RequestManager: WARNING backend reports no assets for %s (%s); cached content kept\n",
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
                        Serial.printf("RequestManager: %s: %d track(s) could not be downloaded, playing the rest\n",
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
    RequestManager &instance = RequestManager::getInstance("http://portal.tilkietalkie.com/api");
    instance.onTrackDownloadComplete(path, success);
}

void RequestManager::storeUidToFigureIdMapping(const String &uid, const String &figureId)
{
    auto it = uidToFigureIdMap.find(uid);
    if (it != uidToFigureIdMap.end() && it->second == figureId) {
        return; // unchanged: no NVS write on every tap
    }
    uidToFigureIdMap[uid] = figureId;
    saveUidMappings();
}

String RequestManager::getFigureIdFromUid(const String &uid)
{
    auto it = uidToFigureIdMap.find(uid);
    return (it != uidToFigureIdMap.end()) ? it->second : String();
}

// NVS operations
bool RequestManager::initializeNVS()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvsHandle);
    if (err != ESP_OK) {
        Serial.printf("RequestManager: Failed to open NVS: %s\n", esp_err_to_name(err));
        return false;
    }
    
    return true;
}

bool RequestManager::saveUidMappings()
{
    if (nvsHandle == 0) return false;
    
    JsonDocument doc(&psramJsonAllocator);
    JsonObject mappings = doc.to<JsonObject>();
    
    for (const auto& pair : uidToFigureIdMap) {
        mappings[pair.first] = pair.second;
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    esp_err_t err = nvs_set_str(nvsHandle, NVS_UID_MAPPING_KEY, jsonString.c_str());
    if (err != ESP_OK) return false;
    
    return nvs_commit(nvsHandle) == ESP_OK;
}

bool RequestManager::loadUidMappings()
{
    if (nvsHandle == 0) return false;
    
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(nvsHandle, NVS_UID_MAPPING_KEY, NULL, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    
    char* jsonString = (char*)heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM);
    if (!jsonString) return false;
    
    err = nvs_get_str(nvsHandle, NVS_UID_MAPPING_KEY, jsonString, &required_size);
    if (err != ESP_OK) {
        heap_caps_free(jsonString);
        return false;
    }
    
    JsonDocument doc(&psramJsonAllocator);
    DeserializationError error = deserializeJson(doc, jsonString);
    heap_caps_free(jsonString);
    
    if (error) return false;
    
    uidToFigureIdMap.clear();
    for (JsonPair pair : doc.as<JsonObject>()) {
        uidToFigureIdMap[String(pair.key().c_str())] = pair.value().as<String>();
    }
    
    Serial.printf("RequestManager: Loaded %d UID mappings\n", uidToFigureIdMap.size());
    return true;
}

bool RequestManager::saveUnitManifest(const String &uid, const JsonDocument &manifest)
{
    FileManager &fileManager = FileManager::getInstance();
    fileManager.createDirectory(SD_DATA_DIR);
    fileManager.createDirectory(SD_MANIFEST_DIR);
    String json;
    if (serializeJson(manifest, json) == 0) {
        Serial.printf("RequestManager: Failed to serialize manifest for unit: %s\n", uid.c_str());
        return false;
    }
    // Written by the SD maintenance task (tmp + rename); the loop never waits for the card.
    if (!fileManager.writeFileDeferred(getUnitManifestPath(uid), json)) {
        Serial.printf("RequestManager: Failed to queue manifest for unit: %s\n", uid.c_str());
        return false;
    }
    Serial.printf("RequestManager: Saved manifest for unit: %s\n", uid.c_str());
    return true;
}

bool RequestManager::savePendingUnitManifest(const String &uid, const JsonDocument &manifest)
{
    FileManager &fileManager = FileManager::getInstance();
    fileManager.createDirectory(SD_DATA_DIR);
    fileManager.createDirectory(SD_PENDING_MANIFEST_DIR);
    String json;
    if (serializeJson(manifest, json) == 0) {
        Serial.printf("RequestManager: Failed to serialize pending manifest for unit: %s\n", uid.c_str());
        return false;
    }
    // Written by the SD maintenance task (tmp + rename); the loop never waits for the card.
    if (!fileManager.writeFileDeferred(getPendingUnitManifestPath(uid), json)) {
        Serial.printf("RequestManager: Failed to queue pending manifest for unit: %s\n", uid.c_str());
        return false;
    }
    Serial.printf("RequestManager: Saved pending manifest for unit: %s\n", uid.c_str());
    return true;
}

bool RequestManager::loadUnitManifest(const String &uid, JsonDocument &manifest)
{
    File file = SD_MMC.open(getUnitManifestPath(uid), FILE_READ);
    if (!file) {
        return false;
    }

    DeserializationError error = deserializeJson(manifest, file);
    file.close();

    if (error) {
        Serial.printf("RequestManager: Failed to parse cached manifest for %s: %s\n", uid.c_str(), error.c_str());
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
    File file = SD_MMC.open(getPendingUnitManifestPath(uid), FILE_READ);
    if (!file) {
        return false;
    }

    DeserializationError error = deserializeJson(manifest, file);
    file.close();

    if (error) {
        Serial.printf("RequestManager: Failed to parse pending manifest for %s: %s\n", uid.c_str(), error.c_str());
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
            Serial.printf("RequestManager: Failed to parse in-memory pending manifest for %s: %s\n", tracker.uid.c_str(), pendingError.c_str());
        } else {
            hasPendingManifestData = true;
        }
    }

    if (!hasPendingManifestData && !loadPendingUnitManifest(tracker.uid, pendingManifest)) {
        Serial.printf("RequestManager: Missing pending manifest while finalizing unit %s\n", tracker.uid.c_str());
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

                if (existingFilesOnly && !fileManager.fileExists(track.localPath)) {
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

        if (existingFilesOnly && !fileManager.fileExists(track.localPath)) {
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
        Serial.printf("RequestManager: stale asset queued for deletion: %s\n", oldPath.c_str());
    }
    if (requiredChanged) {
        fileManager.flushRequiredFiles();
    }
}
