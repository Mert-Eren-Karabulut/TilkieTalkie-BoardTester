#include "RequestManager.h"
#include "ReverbClient.h"

// Initialize static members
const char* RequestManager::NVS_NAMESPACE = "requestmgr";
const char* RequestManager::NVS_UID_MAPPING_KEY = "uid_mappings";

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

    FileManager &fileManager = FileManager::getInstance();
    
    // Try to find figure data in response
    JsonObject figure = doc["figure"].as<JsonObject>();
    if (figure.isNull()) {
        figure = doc["data"].isNull() ? doc["unit"].as<JsonObject>() : doc["data"].as<JsonObject>();
    }
    
    if (figure.isNull()) {
        if (figureDownloadCompleteCallback) {
            Figure emptyFigure;
            figureDownloadCompleteCallback(uid, "null", false, "No figure data found", emptyFigure);
        }
        return;
    }
    
    String figureId = String(figure["id"].as<int>());
    String figureName = figure["name"].as<String>();
    JsonArray episodes = figure["episodes"].as<JsonArray>();
    
    storeUidToFigureIdMapping(uid, figureId);
    
    Figure figureData;
    figureData.id = figureId;
    figureData.name = figureName;
    figureData.description = figure["description"].as<String>();
    
    std::vector<String> trackPaths;
    int tracksToDownload = 0;
    int tracksAlreadyExist = 0;
    
    for (JsonVariant episodeVar : episodes) {
        JsonObject episodeObj = episodeVar.as<JsonObject>();
        Episode episode;
        episode.id = String(episodeObj["id"].as<int>());
        episode.name = episodeObj["name"].as<String>();
        episode.description = episodeObj["description"].as<String>();
        
        JsonArray tracks = episodeObj["tracks"].as<JsonArray>();
        
        for (JsonVariant trackVar : tracks) {
            JsonObject trackObj = trackVar.as<JsonObject>();
            Track track;
            track.id = String(trackObj["id"].as<int>());
            track.name = trackObj["name"].as<String>();
            track.description = trackObj["description"].as<String>();
            track.audioUrl = trackObj["audio_url"].as<String>();
            track.duration = trackObj["duration"].as<int>();
            track.localPath = "/figures/" + figureId + "/" + episode.id + "/" + track.id + ".mp3";
            
            trackPaths.push_back(track.localPath);
            
            if (track.audioUrl.length() > 0) {
                fileManager.addRequiredFile(track.localPath, track.audioUrl);
                
                if (!fileManager.fileExists(track.localPath)) {
                    fileManager.scheduleDownload(track.audioUrl, track.localPath);
                    tracksToDownload++;
                } else {
                    tracksAlreadyExist++;
                }
            }
            
            episode.tracks.push_back(track);
        }
        
        figureData.episodes.push_back(episode);
    }
    
    startTrackingFigure(uid, figureName, figureId, trackPaths, figureData);
    
    if (tracksToDownload > 0) {
        Serial.printf("Starting %d downloads for: %s\n", tracksToDownload, figureName.c_str());
    }
    if (tracksAlreadyExist > 0) {
        Serial.printf("%d tracks cached for: %s\n", tracksAlreadyExist, figureName.c_str());
    }
}

void RequestManager::processOfflineFigureRequest(const String &uid)
{
    Serial.println(F("RequestManager: Processing offline figure request"));
    
    String figureId = getFigureIdFromUid(uid);
    if (figureId.isEmpty()) {
        if (figureDownloadCompleteCallback) {
            Figure emptyFigure;
            figureDownloadCompleteCallback(uid, "Unknown", false, "No offline data available", emptyFigure);
        }
        return;
    }
    
    Figure figureData = constructFigureFromLocalFiles(uid, figureId);
    
    if (figureData.episodes.empty()) {
        if (figureDownloadCompleteCallback) {
            figureDownloadCompleteCallback(uid, figureData.name.isEmpty() ? "Unknown" : figureData.name, 
                                         false, "No local tracks available", figureData);
        }
        return;
    }
    
    std::vector<String> trackPaths;
    for (const auto& episode : figureData.episodes) {
        for (const auto& track : episode.tracks) {
            trackPaths.push_back(track.localPath);
        }
    }
    
    startTrackingFigure(uid, figureData.name, figureId, trackPaths, figureData);
}

void RequestManager::setFigureDownloadCompleteCallback(FigureDownloadCompleteCallback callback)
{
    this->figureDownloadCompleteCallback = callback;
}

void RequestManager::startTrackingFigure(const String &uid, const String &figureName, const String &figureId, 
                                          const std::vector<String> &trackPaths, const Figure &figureData)
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
    
    FileManager &fileManager = FileManager::getInstance();
    for (const String &path : trackPaths) {
        if (fileManager.fileExists(path)) {
            tracker.tracksReady++;
        }
    }
    
    bool allReady = (tracker.tracksReady >= tracker.totalTracks && tracker.totalTracks > 0);
    
    if (allReady) {
        tracker.completed = true;
        if (figureDownloadCompleteCallback) {
            figureDownloadCompleteCallback(uid, figureName, true, "", figureData);
        }
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
                
                if (figureDownloadCompleteCallback) {
                    figureDownloadCompleteCallback(uid, tracker.figureName, success, 
                        success ? "" : "Some tracks failed", tracker.figureData);
                }
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

RequestManager::Figure RequestManager::constructFigureFromLocalFiles(const String &uid, const String &figureId)
{
    Figure figure;
    figure.id = figureId;
    figure.name = "Local Figure";
    figure.description = "Offline figure data";
    
    std::vector<String> figurePaths = getRequiredFilesForFigure(figureId);
    if (figurePaths.empty()) return figure;
    
    std::map<String, std::vector<String>> episodeTrackMap;
    FileManager &fileManager = FileManager::getInstance();
    String prefix = "/figures/" + figureId + "/";
    
    for (const String& path : figurePaths) {
        if (!fileManager.fileExists(path)) continue;
        if (!path.startsWith(prefix)) continue;
        
        int episodeEnd = path.indexOf('/', prefix.length());
        if (episodeEnd > (int)prefix.length()) {
            String episodeId = path.substring(prefix.length(), episodeEnd);
            episodeTrackMap[episodeId].push_back(path);
        }
    }
    
    for (const auto& episodePair : episodeTrackMap) {
        Episode episode;
        episode.id = episodePair.first;
        episode.name = "Episode " + episodePair.first;
        
        for (const String& trackPath : episodePair.second) {
            String filename = trackPath.substring(trackPath.lastIndexOf('/') + 1);
            String trackId = filename.substring(0, filename.lastIndexOf('.'));
            
            Track track;
            track.id = trackId;
            track.name = "Track " + trackId;
            track.localPath = trackPath;
            
            episode.tracks.push_back(track);
        }
        
        if (!episode.tracks.empty()) {
            figure.episodes.push_back(episode);
        }
    }
    
    return figure;
}

std::vector<String> RequestManager::getRequiredFilesForFigure(const String &figureId)
{
    FileManager &fileManager = FileManager::getInstance();
    return fileManager.getRequiredFilesByPattern("/figures/" + figureId + "/");
}
