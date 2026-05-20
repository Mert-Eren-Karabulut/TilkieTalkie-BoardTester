#include "RequestManager.h"
#include "ReverbClient.h"

// Initialize static members
const char* RequestManager::NVS_NAMESPACE = "requestmgr";
const char* RequestManager::NVS_UID_MAPPING_KEY = "uid_mappings";
const char* RequestManager::SD_DATA_DIR = "/.requestmanager";
const char* RequestManager::SD_MANIFEST_DIR = "/.requestmanager/manifests";
const char* RequestManager::SD_PENDING_MANIFEST_DIR = "/.requestmanager/pending_manifests";

namespace {
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
    : timeout(15000)
    , lastStatusCode(0)
    , figureDownloadCompleteCallback(nullptr)
    , nvsHandle(0)
{
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
    this->baseUrl = url;
    if (this->baseUrl.startsWith("https://")) {
        this->baseUrl.replace("https://", "http://");
    }
}

void RequestManager::setAuthToken(const String &token)
{
    this->authToken = token;
}

void RequestManager::setTimeout(int timeoutMs)
{
    this->timeout = timeoutMs;
}

void RequestManager::update()
{
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
        completion.figureData
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
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
        lastError = String("JSON parsing error: ") + error.c_str();
        Serial.println("RequestManager: " + lastError);
        
        JsonDocument errorDoc;
        errorDoc["error"] = true;
        errorDoc["message"] = lastError;
        return errorDoc;
    }

    return doc;
}

// Consolidated HTTP request execution
JsonDocument RequestManager::executeHttpRequest(const String& endpoint, const String& method, const String* payload)
{
    JsonDocument emptyDoc;

    if (!isNetworkReady()) {
        lastError = "Network not ready";
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }

    // Ensure we have auth token for protected endpoints
    if (authToken.length() == 0 && !endpoint.endsWith("/token") && !endpoint.endsWith("/validate-token")) {
        initConnection();
        if (authToken.length() == 0) {
            lastError = "Authentication failed";
            emptyDoc["error"] = true;
            emptyDoc["message"] = lastError;
            return emptyDoc;
        }
    }

    String url = baseUrl + endpoint;
    http.end();
    
    if (!http.begin(client, url)) {
        lastError = "Failed to connect to " + url;
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }

    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    setDefaultHeaders();

    int httpResponseCode;
    if (method == "POST" && payload) {
        httpResponseCode = http.POST(*payload);
    } else {
        httpResponseCode = http.GET();
    }
    
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.printf("RequestManager: %s %s -> %d\n", method.c_str(), endpoint.c_str(), httpResponseCode);
        Serial.println("RequestManager: Response body: " + response);
        http.end();
        return parseResponse(response);
    }

    // Error handling
    static const char* errorMessages[] = {
        "Connection refused", "Send header failed", "Send payload failed",
        "Not connected", "Connection lost", "No stream", "No HTTP server",
        "Too less RAM", "Encoding error", "Stream write error", "Read timeout"
    };
    
    const char* errorDetail = "Unknown error";
    int errorIndex = -httpResponseCode - 1;
    if (errorIndex >= 0 && errorIndex < 11) {
        errorDetail = errorMessages[errorIndex];
    }
    
    lastError = String("HTTP ") + method + " failed: " + String(httpResponseCode) + " (" + errorDetail + ")";
    Serial.println("RequestManager: " + lastError);
    
    http.end();
    emptyDoc["error"] = true;
    emptyDoc["message"] = lastError;
    return emptyDoc;
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
    uint64_t macAddress = ESP.getEfuseMac();
    String endpoint = "/hubs/" + String(macAddress, 10) + "/validate-token";
    
    http.end();
    String url = baseUrl + endpoint;
    
    if (!http.begin(client, url)) {
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
    
    if (isNetworkReady()) {
        processOnlineFigureRequest(uid);
    } else {
        processOfflineFigureRequest(uid);
    }
}

void RequestManager::processOnlineFigureRequest(const String &uid)
{
    JsonDocument doc = get("/units/" + uid);
    
    if (doc["error"].as<bool>()) {
        Serial.println(F("RequestManager: API returned error"));
        return;
    }

    JsonObject figure = doc["figure"].as<JsonObject>();
    if (figure.isNull()) {
        if (figureDownloadCompleteCallback) {
            Figure emptyFigure;
            figureDownloadCompleteCallback(uid, "null", false, "No figure data found", emptyFigure);
        }
        return;
    }

    FileManager &fileManager = FileManager::getInstance();
    String figureId = jsonValueToString(figure["id"]);
    String figureName = figure["name"].as<String>();
    JsonArray assetManifest = doc["asset_manifest"].as<JsonArray>();
    if (assetManifest.isNull()) {
        if (figureDownloadCompleteCallback) {
            Figure emptyFigure;
            figureDownloadCompleteCallback(uid, figureName, false, "Manifest is missing asset_manifest", emptyFigure);
        }
        return;
    }
    
    storeUidToFigureIdMapping(uid, figureId);

    JsonDocument previousManifest;
    String previousManifestJson;
    if (loadUnitManifest(uid, previousManifest)) {
        serializeJson(previousManifest, previousManifestJson);
    }

    String pendingManifestJson;
    serializeJson(doc, pendingManifestJson);

    if (!savePendingUnitManifest(uid, doc)) {
        if (figureDownloadCompleteCallback) {
            Figure emptyFigure;
            figureDownloadCompleteCallback(uid, figureName, false, "Failed to cache pending manifest", emptyFigure);
        }
        return;
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
        fileManager.addRequiredFile(storagePath, url, checksum);

        bool needsDownload = !fileManager.fileExists(storagePath);
        if (!needsDownload && !checksum.isEmpty()) {
            String currentChecksum = fileManager.calculateFileChecksum(storagePath);
            if (currentChecksum != checksum) {
                Serial.printf("RequestManager: Checksum mismatch, refreshing asset: %s\n", storagePath.c_str());
                fileManager.deleteFile(storagePath);
                needsDownload = true;
            }
        }

        if (needsDownload && url.length() > 0) {
            fileManager.scheduleDownload(url, storagePath, checksum);
            assetsToDownload++;
        } else if (!needsDownload) {
            assetsAlreadyCached++;
        }
    }

    Figure figureData = buildFigureFromManifest(doc);
    
    startTrackingFigure(uid, figureName, figureId, trackPaths, figureData, true, pendingManifestJson, previousManifestJson);
    
    if (assetsToDownload > 0) {
        Serial.printf("RequestManager: Starting %d asset downloads for %s\n", assetsToDownload, figureName.c_str());
    }
    if (assetsAlreadyCached > 0) {
        Serial.printf("RequestManager: %d assets already cached for %s\n", assetsAlreadyCached, figureName.c_str());
    }
}

void RequestManager::processOfflineFigureRequest(const String &uid)
{
    Serial.println(F("RequestManager: Processing offline figure request"));

    JsonDocument manifest;
    if (!loadUnitManifest(uid, manifest)) {
        if (figureDownloadCompleteCallback) {
            Figure emptyFigure;
            figureDownloadCompleteCallback(uid, "Unknown", false, "No offline data available", emptyFigure);
        }
        return;
    }

    Figure figureData = buildFigureFromManifest(manifest, true);

    std::vector<String> trackPaths;
    for (const auto& content : figureData.contents) {
        for (const auto& episode : content.episodes) {
            for (const auto& track : episode.tracks) {
                trackPaths.push_back(track.localPath);
            }
        }
    }

    for (const auto& customTrack : figureData.customTracks) {
        trackPaths.push_back(customTrack.localPath);
    }

    if (trackPaths.empty()) {
        if (figureDownloadCompleteCallback) {
            figureDownloadCompleteCallback(uid, figureData.name.isEmpty() ? "Unknown" : figureData.name, 
                                         false, "No local tracks available", figureData);
        }
        return;
    }

    String figureId = figureData.id.isEmpty() ? getFigureIdFromUid(uid) : figureData.id;
    startTrackingFigure(uid, figureData.name, figureId, trackPaths, figureData);
}

void RequestManager::setFigureDownloadCompleteCallback(FigureDownloadCompleteCallback callback)
{
    this->figureDownloadCompleteCallback = callback;
}

void RequestManager::queueFigureDownloadCompletion(const String &uid, const String &figureName, bool success, const String &error, const Figure &figureData)
{
    PendingFigureCompletion completion;
    completion.uid = uid;
    completion.figureName = figureName;
    completion.success = success;
    completion.error = error;
    completion.figureData = figureData;
    pendingCompletions.push_back(completion);
}

void RequestManager::startTrackingFigure(const String &uid, const String &figureName, const String &figureId, 
                                          const std::vector<String> &trackPaths, const Figure &figureData,
                                          bool hasPendingManifest, const String &pendingManifestJson,
                                          const String &previousManifestJson)
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
    
    FileManager &fileManager = FileManager::getInstance();
    for (const String &path : trackPaths) {
        if (fileManager.fileExists(path)) {
            tracker.tracksReady++;
        }
    }
    
    bool allReady = (tracker.tracksReady >= tracker.totalTracks && tracker.totalTracks > 0);
    
    if (allReady) {
        tracker.completed = true;
        finalizeTrackedManifest(tracker, true);
        queueFigureDownloadCompletion(uid, figureName, true, "", figureData);
    }
    
    activeDownloads.push_back(tracker);
}

void RequestManager::checkFigureDownloadStatus(const String &uid)
{
    for (auto &tracker : activeDownloads) {
        if (tracker.uid == uid && !tracker.completed) {
            if (tracker.tracksReady + tracker.tracksFailed >= tracker.totalTracks) {
                tracker.completed = true;
                bool success = (tracker.tracksReady > 0) && (tracker.tracksFailed == 0);
                finalizeTrackedManifest(tracker, success);

                queueFigureDownloadCompletion(uid, tracker.figureName, success,
                    success ? "" : "Some tracks failed", tracker.figureData);
            }
            break;
        }
    }
}

void RequestManager::onTrackDownloadComplete(const String &path, bool success)
{
    for (auto &tracker : activeDownloads) {
        if (!tracker.completed) {
            for (const String &trackPath : tracker.trackPaths) {
                if (trackPath == path) {
                    if (success) {
                        tracker.tracksReady++;
                    } else {
                        tracker.tracksFailed++;
                    }
                    checkFigureDownloadStatus(tracker.uid);
                    return;
                }
            }
        }
    }
}

void RequestManager::staticFileDownloadCallback(const String& url, const String& path, bool success, const String& error)
{
    RequestManager &instance = RequestManager::getInstance("http://portal.tilkietalkie.com/api");
    instance.onTrackDownloadComplete(path, success);
}

void RequestManager::storeUidToFigureIdMapping(const String &uid, const String &figureId)
{
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
    
    JsonDocument doc;
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
    
    JsonDocument doc;
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

    String manifestPath = getUnitManifestPath(uid);
    if (SD_MMC.exists(manifestPath)) {
        SD_MMC.remove(manifestPath);
    }

    File file = SD_MMC.open(manifestPath, FILE_WRITE);
    if (!file) {
        Serial.printf("RequestManager: Failed to open manifest for writing: %s\n", uid.c_str());
        return false;
    }

    if (serializeJson(manifest, file) == 0) {
        file.close();
        Serial.printf("RequestManager: Failed to serialize manifest for unit: %s\n", uid.c_str());
        return false;
    }

    file.close();
    Serial.printf("RequestManager: Saved manifest for unit: %s\n", uid.c_str());
    return true;
}

bool RequestManager::savePendingUnitManifest(const String &uid, const JsonDocument &manifest)
{
    FileManager &fileManager = FileManager::getInstance();
    fileManager.createDirectory(SD_DATA_DIR);
    fileManager.createDirectory(SD_PENDING_MANIFEST_DIR);

    String manifestPath = getPendingUnitManifestPath(uid);
    if (SD_MMC.exists(manifestPath)) {
        SD_MMC.remove(manifestPath);
    }

    File file = SD_MMC.open(manifestPath, FILE_WRITE);
    if (!file) {
        Serial.printf("RequestManager: Failed to open pending manifest for writing: %s\n", uid.c_str());
        return false;
    }

    if (serializeJson(manifest, file) == 0) {
        file.close();
        Serial.printf("RequestManager: Failed to serialize pending manifest for unit: %s\n", uid.c_str());
        return false;
    }

    file.close();
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
    String pendingManifestPath = getPendingUnitManifestPath(uid);
    if (!SD_MMC.exists(pendingManifestPath)) {
        return true;
    }

    bool deleted = SD_MMC.remove(pendingManifestPath);
    if (!deleted) {
        Serial.printf("RequestManager: Failed to delete pending manifest for unit: %s\n", uid.c_str());
    }

    return deleted;
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

    JsonDocument pendingManifest;
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

    JsonDocument previousManifest;
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

bool RequestManager::isAssetReferencedByOtherUnit(const String &assetPath, const String &currentUid) const
{
    File manifestDir = SD_MMC.open(SD_MANIFEST_DIR);
    if (!manifestDir || !manifestDir.isDirectory()) {
        return false;
    }

    String currentManifestFile = currentUid + ".json";
    File entry = manifestDir.openNextFile();
    while (entry) {
        String entryName = String(entry.name());
        if (!entry.isDirectory() && entryName != currentManifestFile) {
            JsonDocument manifest;
            DeserializationError error = deserializeJson(manifest, entry);
            entry.close();

            if (!error) {
                std::vector<String> assetPaths = extractAssetPaths(manifest);
                if (std::find(assetPaths.begin(), assetPaths.end(), assetPath) != assetPaths.end()) {
                    manifestDir.close();
                    return true;
                }
            }
        } else {
            entry.close();
        }

        entry = manifestDir.openNextFile();
    }

    manifestDir.close();
    return false;
}

void RequestManager::removeStaleAssets(const String &uid, const JsonDocument &currentManifest, const JsonDocument *previousManifest)
{
    if (previousManifest == nullptr) {
        return;
    }

    FileManager &fileManager = FileManager::getInstance();
    std::vector<String> currentPaths = extractAssetPaths(currentManifest);
    std::vector<String> previousPaths = extractAssetPaths(*previousManifest);

    for (const String &oldPath : previousPaths) {
        if (std::find(currentPaths.begin(), currentPaths.end(), oldPath) != currentPaths.end()) {
            continue;
        }

        if (isAssetReferencedByOtherUnit(oldPath, uid)) {
            continue;
        }

        fileManager.removeRequiredFile(oldPath);
        if (fileManager.fileExists(oldPath)) {
            fileManager.deleteFile(oldPath);
        }
        Serial.printf("RequestManager: Removed stale asset: %s\n", oldPath.c_str());
    }
}
