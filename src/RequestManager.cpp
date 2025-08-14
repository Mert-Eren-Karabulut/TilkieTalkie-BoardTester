#include "RequestManager.h"

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
{
    this->baseUrl = convertToHttp(baseUrl);
    this->timeout = 15000; // 15 seconds timeout for robustness
    this->lastStatusCode = 0;
    this->lastError = "";
    this->figureDownloadCompleteCallback = nullptr;
    this->nvsHandle = 0;
    
    // Pre-reserve memory for containers to prevent frequent reallocations
    activeDownloads.reserve(5);
}

// Destructor
RequestManager::~RequestManager()
{
    http.end();
    
    // Save UID mappings before cleanup
    saveUidMappings();
    
    // Close NVS handle
    if (nvsHandle != 0) {
        nvs_close(nvsHandle);
        nvsHandle = 0;
    }
    
    // Clean up all tracking data
    clearDownloadTrackers();
    uidToFigureIdMap.clear();
}

// Initialization
bool RequestManager::begin()
{
    Serial.println(F("RequestManager: Initializing..."));
    
    // Initialize NVS for UID mappings
    if (!initializeNVS()) {
        Serial.println(F("RequestManager: Failed to initialize NVS"));
        return false;
    }
    
    // Load stored UID mappings
    loadUidMappings();
    
    // Set up FileManager callback to track download completion
    FileManager &fileManager = FileManager::getInstance();
    fileManager.setDownloadCompleteCallback(staticFileDownloadCallback);

    // Try to load stored JWT token and initialize connection
    initConnection();

    if (authToken.length() > 0)
    {
        Serial.println(F("RequestManager: Initialized with token"));
        return true;
    }
    else
    {
        Serial.println(F("RequestManager: No token available"));
        return false;
    }
}

// Configuration methods
void RequestManager::setBaseUrl(const String &url)
{
    this->baseUrl = convertToHttp(url);
}

void RequestManager::setAuthToken(const String &token)
{
    this->authToken = token;
}

void RequestManager::setTimeout(int timeoutMs)
{
    this->timeout = timeoutMs;
}

// Private helper methods
bool RequestManager::isWiFiConnected()
{
    bool connected = WiFi.status() == WL_CONNECTED;
    if (!connected)
    {
        Serial.printf("RequestManager: WiFi not connected (status: %d)\n", WiFi.status());
    }
    return connected;
}

bool RequestManager::checkNetworkConnectivity()
{
    if (!isWiFiConnected())
    {
        return false;
    }
    
    // Additional network diagnostics
    IPAddress localIP = WiFi.localIP();
    if (localIP[0] == 0)
    {
        return false;
    }
    
    return true;
}

// Convert HTTPS URLs to HTTP for memory efficiency
String RequestManager::convertToHttp(const String& url)
{
    String convertedUrl = url;
    if (convertedUrl.startsWith("https://"))
    {
        convertedUrl.replace("https://", "http://");
        Serial.println(F("RequestManager: Converted HTTPS to HTTP for memory efficiency"));
    }
    return convertedUrl;
}

// Memory-efficient string building helper
String RequestManager::buildUrl(const String& endpoint) const
{
    String url;
    url.reserve(baseUrl.length() + endpoint.length() + 1);
    url = baseUrl;
    url += endpoint;
    return url;
}

// Memory cleanup methods
void RequestManager::clearDownloadTrackers()
{
    activeDownloads.clear();
    activeDownloads.shrink_to_fit(); // Free unused capacity
}

void RequestManager::cleanupCompletedTrackers()
{
    auto it = activeDownloads.begin();
    while (it != activeDownloads.end())
    {
        if (it->completed)
        {
            Serial.printf("RequestManager: Removing completed tracker for figure: %s\n", it->figureName.c_str());
            it = activeDownloads.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void RequestManager::setDefaultHeaders()
{
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "TilkieTalkie/1.0");

    if (authToken.length() > 0)
    {
        http.addHeader("Authorization", "Bearer " + authToken);
    }
}

JsonDocument RequestManager::parseResponse(const String& response)
{
    // Check response size
    if (response.length() > MAX_RESPONSE_SIZE)
    {
        Serial.printf("RequestManager: Response too large (%d bytes), truncating\n", response.length());
        // Could truncate here if needed, but for now we'll try to parse as-is
    }
    
    // Calculate JSON document size with more conservative approach
    size_t jsonSize = min(response.length() * 2, (size_t)MAX_RESPONSE_SIZE);
    if (jsonSize < 512) jsonSize = 512;    // Minimum size
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, response);

    if (error)
    {
        lastError = "JSON parsing error: ";
        lastError += error.c_str();
        
        Serial.println("RequestManager: " + lastError);
        Serial.printf("Response length: %d bytes\n", response.length());
        Serial.println("Response preview: " + response.substring(0, min(200, (int)response.length())));
        
        // Create minimal error document
        DynamicJsonDocument errorDoc(256);
        errorDoc["error"] = true;
        errorDoc["message"] = lastError;
        return errorDoc;
    }

    return doc;
}

// HTTP Methods
JsonDocument RequestManager::get(const String &endpoint)
{
    DynamicJsonDocument emptyDoc(256);

    if (!checkNetworkConnectivity())
    {
        lastError = "Network connectivity check failed";
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }

    // Check if we have an auth token, if not try to get one
    if (authToken.length() == 0)
    {
        Serial.println(F("No auth token available, attempting to authenticate..."));
        initConnection();
        if (authToken.length() == 0)
        {
            lastError = "Authentication failed - no token available";
            emptyDoc["error"] = true;
            emptyDoc["message"] = lastError;
            return emptyDoc;
        }
    }

    String url = buildUrl(endpoint);
    
    // Clean up any previous connections
    http.end();
    
    // Begin HTTP connection (not HTTPS)
    if (!http.begin(client, url))
    {
        lastError = "Failed to establish HTTP connection to " + url;
        Serial.println("RequestManager: " + lastError);
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }

    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    setDefaultHeaders();

    Serial.print(F("RequestManager: Sending GET request to "));
    Serial.println(url);
    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        
        Serial.printf("RequestManager: GET response code: %d, size: %d bytes\n", httpResponseCode, response.length());
        return parseResponse(response);
    }
    else
    {
        // Improved error reporting
        String errorDetail;
        switch (httpResponseCode)
        {
            case HTTPC_ERROR_CONNECTION_REFUSED: errorDetail = "Connection refused"; break;
            case HTTPC_ERROR_SEND_HEADER_FAILED: errorDetail = "Send header failed"; break;
            case HTTPC_ERROR_SEND_PAYLOAD_FAILED: errorDetail = "Send payload failed"; break;
            case HTTPC_ERROR_NOT_CONNECTED: errorDetail = "Not connected"; break;
            case HTTPC_ERROR_CONNECTION_LOST: errorDetail = "Connection lost"; break;
            case HTTPC_ERROR_NO_STREAM: errorDetail = "No stream"; break;
            case HTTPC_ERROR_NO_HTTP_SERVER: errorDetail = "No HTTP server"; break;
            case HTTPC_ERROR_TOO_LESS_RAM: errorDetail = "Too less RAM"; break;
            case HTTPC_ERROR_ENCODING: errorDetail = "Encoding error"; break;
            case HTTPC_ERROR_STREAM_WRITE: errorDetail = "Stream write error"; break;
            case HTTPC_ERROR_READ_TIMEOUT: errorDetail = "Read timeout"; break;
            default: errorDetail = "Unknown error"; break;
        }
        
        lastError = "HTTP GET failed with code: " + String(httpResponseCode) + " (" + errorDetail + ")";
        Serial.println("RequestManager: " + lastError + " to URL: " + url);
        
        http.end();
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }
}

JsonDocument RequestManager::post(const String &endpoint, const JsonDocument &data)
{
    DynamicJsonDocument emptyDoc(256);

    if (!checkNetworkConnectivity())
    {
        lastError = "Network connectivity check failed";
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }

    // Check if we have an auth token, if not try to get one
    if (authToken.length() == 0)
    {
        Serial.println(F("No auth token available, attempting to authenticate..."));
        initConnection();
        if (authToken.length() == 0)
        {
            lastError = "Authentication failed - no token available";
            emptyDoc["error"] = true;
            emptyDoc["message"] = lastError;
            return emptyDoc;
        }
    }

    String url = buildUrl(endpoint);
    
    // Clean up any previous connections
    http.end();
    
    // Begin HTTP connection
    if (!http.begin(client, url))
    {
        lastError = "Failed to establish HTTP POST connection to " + url;
        Serial.println("RequestManager: " + lastError);
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }
    
    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    setDefaultHeaders();

    String jsonString;
    jsonString.reserve(512); // Pre-reserve space for JSON serialization
    serializeJson(data, jsonString);

    Serial.print(F("RequestManager: Sending POST request to "));
    Serial.println(url);
    int httpResponseCode = http.POST(jsonString);
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        
        Serial.printf("RequestManager: POST response code: %d, size: %d bytes\n", httpResponseCode, response.length());
        return parseResponse(response);
    }
    else
    {
        lastError = "HTTP POST failed with code: " + String(httpResponseCode);
        Serial.println("RequestManager: " + lastError);
        
        http.end();
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }
}

// Utility methods
bool RequestManager::isConnected()
{
    return isWiFiConnected();
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
    
    // Build endpoint URL
    String endpoint = "/hubs/" + String(macAddress, 10) + "/token";
    String url = buildUrl(endpoint);
    
    // Clean up any previous connections
    http.end();
    
    // Begin HTTP connection
    if (!http.begin(client, url))
    {
        lastError = "Failed to establish JWT token connection";
        Serial.println("RequestManager: " + lastError);
        return String();
    }
    
    http.setTimeout(timeout);
    setDefaultHeaders();

    Serial.println(F("RequestManager: Requesting JWT token"));
    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        
        JsonDocument doc = parseResponse(response);
        // check if status is success
        if (doc["status"] == "success")
        {
            String token = doc["token"].as<String>();
            Serial.println(F("RequestManager: JWT token obtained successfully"));
            return token;
        }
        else
        {
            lastError = doc["message"].as<String>();
            Serial.println("RequestManager: JWT token request failed: " + lastError);
            return String();
        }
    }
    else
    {
        lastError = "HTTP GET failed with code: " + String(httpResponseCode);
        Serial.println("RequestManager: " + lastError);
        
        http.end();
        return String();
    }
}

bool RequestManager::validateToken(const String& token)
{
    uint64_t macAddress = ESP.getEfuseMac();
    
    // Build endpoint URL
    String endpoint = "/hubs/" + String(macAddress, 10) + "/validate-token";
    String url = buildUrl(endpoint);
    
    // Clean up any previous connections
    http.end();
    
    // Begin HTTP connection
    if (!http.begin(client, url))
    {
        lastError = "Failed to establish token validation connection";
        Serial.println("RequestManager: " + lastError);
        return false;
    }
    
    http.setTimeout(timeout);
    setDefaultHeaders();

    // add Authorization header
    String authHeader = "Bearer " + token;
    http.addHeader("Authorization", authHeader);

    Serial.println(F("RequestManager: Validating token"));
    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    http.end();
    
    if (httpResponseCode == 200)
    {
        Serial.println(F("RequestManager: Token validation successful"));
        return true;
    }
    else
    {
        lastError = "Token validation failed with code: " + String(httpResponseCode);
        Serial.println("RequestManager: " + lastError);
        return false;
    }
}

void RequestManager::initConnection()
{
    ConfigManager &config = ConfigManager::getInstance();
    String storedToken = config.getJWTToken();

    bool needNewToken = true;

    if (storedToken.length() > 0)
    {
        if (validateToken(storedToken))
        {
            Serial.println(F("RequestManager: Using stored JWT token"));
            needNewToken = false;
        }
        else
        {
            config.setJWTToken(String()); // Clear invalid token
        }
    }

    if (needNewToken)
    {
        storedToken = getJWTToken();
        if (storedToken.length() > 0)
        {
            Serial.println(F("RequestManager: New JWT token obtained"));
            config.setJWTToken(storedToken);
        }
        else
        {
            setAuthToken(String());
            return;
        }
    }

    setAuthToken(storedToken);
}

void RequestManager::getCheckFigureTracks(const String &uid)
{
    Serial.println(F("RequestManager: Processing figure tracks request"));
    Serial.print(F("UID: "));
    Serial.println(uid);
    
    // Check if we have WiFi connectivity
    bool isOnline = checkNetworkConnectivity();
    Serial.print(F("RequestManager: Device is "));
    Serial.println(isOnline ? F("online") : F("offline"));
    
    if (isOnline) {
        // Online mode - fetch from server and update local storage
        processOnlineFigureRequest(uid);
    } else {
        // Offline mode - check if we have local data for this UID
        processOfflineFigureRequest(uid);
    }
}

void RequestManager::processOnlineFigureRequest(const String &uid)
{
    String endpoint = "/units/" + uid;
    String url = buildUrl(endpoint);
    
    // Clean up any previous connections
    http.end();
    
    // Begin HTTP connection
    if (!http.begin(client, url))
    {
        lastError = "Failed to establish figure tracks connection";
        Serial.println("RequestManager: " + lastError);
        return;
    }
    
    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    setDefaultHeaders();

    Serial.println(F("RequestManager: Fetching figure tracks from server"));
    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        
        JsonDocument doc = parseResponse(response);
        if (doc["error"].as<bool>())
        {
            Serial.println(F("RequestManager: API returned error"));
            return;
        }

        // Get file manager instance
        FileManager &fileManager = FileManager::getInstance();
        
        JsonObject figure = doc["figure"].as<JsonObject>();
        if (figure.isNull()) {
            // Try alternative keys that might contain the figure data
            if (!doc["data"].isNull()) {
                figure = doc["data"].as<JsonObject>();
            } else if (!doc["unit"].isNull()) {
                figure = doc["unit"].as<JsonObject>();
            }
        }
        
        if (figure.isNull()) {
            if (figureDownloadCompleteCallback)
            {
                Figure emptyFigure;
                figureDownloadCompleteCallback(uid, "null", false, "No figure data found", emptyFigure);
            }
            return;
        }
        
        String figureId = String(figure["id"].as<int>());
        String figureName = figure["name"].as<String>();
        JsonArray episodes = figure["episodes"].as<JsonArray>();
        
        // Store UID to Figure ID mapping in NVS immediately
        storeUidToFigureIdMapping(uid, figureId);
        
        // Parse the complete figure structure for the callback
        Figure figureData;
        figureData.id = figureId;
        figureData.name = figureName;
        figureData.description = figure["description"].as<String>();
        
        // Pre-reserve capacity based on episodes count
        if (episodes.size() > 0) {
            figureData.episodes.reserve(episodes.size());
        }
        
        // Parse episodes and tracks
        std::vector<String> trackPaths;
        int tracksToDownload = 0;
        int tracksAlreadyExist = 0;
        
        for (JsonVariant episodeVar : episodes)
        {
            JsonObject episodeObj = episodeVar.as<JsonObject>();
            Episode episode;
            episode.id = String(episodeObj["id"].as<int>());
            episode.name = episodeObj["name"].as<String>();
            episode.description = episodeObj["description"].as<String>();
            
            JsonArray tracks = episodeObj["tracks"].as<JsonArray>();
            if (tracks.size() > 0) {
                episode.tracks.reserve(tracks.size());
            }
            
            for (JsonVariant trackVar : tracks)
            {
                JsonObject trackObj = trackVar.as<JsonObject>();
                Track track;
                track.id = String(trackObj["id"].as<int>());
                track.name = trackObj["name"].as<String>();
                track.description = trackObj["description"].as<String>();
                track.audioUrl = trackObj["audio_url"].as<String>();
                track.duration = trackObj["duration"].as<int>();
                
                // Create local path for the track using the original file structure
                track.localPath = "/figures/" + figureId + "/" + episode.id + "/" + track.id + ".wav";
                trackPaths.push_back(track.localPath);
                
                // Always add to required files list (regardless of whether file exists)
                if (track.audioUrl.length() > 0)
                {
                    // Add to required files list first
                    fileManager.addRequiredFile(track.localPath, track.audioUrl);
                    
                    // Then check if we need to download
                    if (!fileManager.fileExists(track.localPath))
                    {
                        Serial.print(F("Downloading: "));
                        Serial.println(track.name);
                        fileManager.scheduleDownload(track.audioUrl, track.localPath);
                        tracksToDownload++;
                    }
                    else
                    {
                        Serial.print(F("Cached: "));
                        Serial.println(track.name);
                        tracksAlreadyExist++;
                    }
                }
                
                episode.tracks.push_back(std::move(track));
            }
            
            figureData.episodes.push_back(std::move(episode));
        }
        
        // Start tracking the download progress
        startTrackingFigure(uid, figureName, figureId, trackPaths, figureData);
        
        // More informative download summary
        if (tracksToDownload > 0)
        {
            Serial.printf("Starting %d track downloads for: %s\n", tracksToDownload, figureName.c_str());
        }
        if (tracksAlreadyExist > 0)
        {
            Serial.printf("%d tracks already cached for: %s\n", tracksAlreadyExist, figureName.c_str());
        }
    }
    else
    {
        lastError = "HTTP GET failed with code: " + String(httpResponseCode);
        Serial.println("RequestManager: " + lastError);
        http.end();
    }
}

void RequestManager::processOfflineFigureRequest(const String &uid)
{
    Serial.println(F("RequestManager: Processing offline figure request"));
    
    // Check if we have a mapping for this UID
    String figureId = getFigureIdFromUid(uid);
    if (figureId.isEmpty()) {
        Serial.println(F("RequestManager: No offline data found for this UID"));
        if (figureDownloadCompleteCallback) {
            Figure emptyFigure;
            figureDownloadCompleteCallback(uid, "Unknown", false, "No offline data available for this figure", emptyFigure);
        }
        return;
    }
    
    Serial.print(F("RequestManager: Found offline mapping for UID "));
    Serial.print(uid);
    Serial.print(F(" -> Figure ID "));
    Serial.println(figureId);
    
    // Construct figure from local files
    Figure figureData = constructFigureFromLocalFiles(uid, figureId);
    
    if (figureData.episodes.empty()) {
        Serial.println(F("RequestManager: No local tracks found for figure"));
        if (figureDownloadCompleteCallback) {
            figureDownloadCompleteCallback(uid, figureData.name.isEmpty() ? "Unknown" : figureData.name, 
                                         false, "No local tracks available", figureData);
        }
        return;
    }
    
    // Create track paths for tracking
    std::vector<String> trackPaths;
    for (const auto& episode : figureData.episodes) {
        for (const auto& track : episode.tracks) {
            trackPaths.push_back(track.localPath);
        }
    }
    
    // Start tracking (all tracks should be ready since we're offline)
    startTrackingFigure(uid, figureData.name, figureId, trackPaths, figureData);
    
    Serial.print(F("RequestManager: Offline figure ready with "));
    Serial.print(trackPaths.size());
    Serial.print(F(" tracks: "));
    Serial.println(figureData.name);
}

// Figure download callback system implementation
void RequestManager::setFigureDownloadCompleteCallback(FigureDownloadCompleteCallback callback)
{
    this->figureDownloadCompleteCallback = callback;
}

void RequestManager::startTrackingFigure(const String &uid, const String &figureName, const String &figureId, const std::vector<String> &trackPaths, const Figure &figureData)
{
    // Clean up completed trackers periodically to prevent memory bloat
    cleanupCompletedTrackers();
    
    // Remove any existing tracker for this UID to prevent duplicates
    for (auto it = activeDownloads.begin(); it != activeDownloads.end(); ++it)
    {
        if (it->uid == uid)
        {
            activeDownloads.erase(it);
            break;
        }
    }
    
    // Create new tracker
    FigureDownloadTracker tracker;
    tracker.uid = uid;
    tracker.figureName = figureName;
    tracker.figureId = figureId;
    tracker.totalTracks = trackPaths.size();
    tracker.tracksReady = 0;
    tracker.tracksFailed = 0;
    tracker.trackPaths = trackPaths;
    tracker.completed = false;
    tracker.figureData = figureData;
    
    // Count tracks that already exist
    FileManager &fileManager = FileManager::getInstance();
    for (const String &path : trackPaths)
    {
        if (fileManager.fileExists(path))
        {
            tracker.tracksReady++;
        }
    }
    
    Serial.printf("Tracking figure '%s': %d tracks (%d ready)\n", 
                  figureName.c_str(), tracker.totalTracks, tracker.tracksReady);
    
    // Check if all tracks are already ready before adding to activeDownloads
    bool allTracksReady = (tracker.tracksReady >= tracker.totalTracks && tracker.totalTracks > 0);
    
    if (allTracksReady)
    {
        Serial.println(F("All tracks already exist, triggering immediate callback"));
        tracker.completed = true;
        
        if (figureDownloadCompleteCallback)
        {
            figureDownloadCompleteCallback(uid, figureName, true, "", figureData);
        }
    }
    
    activeDownloads.push_back(std::move(tracker));
}

void RequestManager::checkFigureDownloadStatus(const String &uid)
{
    for (auto &tracker : activeDownloads)
    {
        if (tracker.uid == uid && !tracker.completed)
        {
            if (tracker.tracksReady + tracker.tracksFailed >= tracker.totalTracks)
            {
                tracker.completed = true;
                bool success = (tracker.tracksReady > 0) && (tracker.tracksFailed == 0);
                
                if (figureDownloadCompleteCallback)
                {
                    String error = success ? "" : "Some tracks failed to download";
                    figureDownloadCompleteCallback(uid, tracker.figureName, success, error, tracker.figureData);
                }
                
                Serial.printf("Figure download completed: %s (%d/%d tracks)\n", 
                              tracker.figureName.c_str(), tracker.tracksReady, tracker.totalTracks);
                break;
            }
        }
    }
}

void RequestManager::onTrackDownloadComplete(const String &path, bool success)
{
    // Find which figure this track belongs to
    for (auto &tracker : activeDownloads)
    {
        if (!tracker.completed)
        {
            for (const String &trackPath : tracker.trackPaths)
            {
                if (trackPath == path)
                {
                    if (success)
                    {
                        tracker.tracksReady++;
                    }
                    else
                    {
                        tracker.tracksFailed++;
                    }
                    
                    // Check if this figure is now complete
                    checkFigureDownloadStatus(tracker.uid);
                    return;
                }
            }
        }
    }
}

// Static callback that forwards to instance method
void RequestManager::staticFileDownloadCallback(const String& url, const String& path, bool success, const String& error)
{
    // Get the singleton instance and forward the call
    RequestManager &instance = RequestManager::getInstance("http://portal.tilkietalkie.com/api");
    instance.onTrackDownloadComplete(path, success);
}

// Store UID to Figure ID mapping for deletion purposes
void RequestManager::storeUidToFigureIdMapping(const String &uid, const String &figureId)
{
    uidToFigureIdMap[uid] = figureId;
    Serial.print(F("Stored UID->FigureID mapping: "));
    Serial.print(uid);
    Serial.print(F(" -> "));
    Serial.println(figureId);
    
    // Save to NVS immediately for persistence
    saveUidMappings();
}

// Get figure ID from UID for deletion purposes
String RequestManager::getFigureIdFromUid(const String &uid)
{
    auto it = uidToFigureIdMap.find(uid);
    if (it != uidToFigureIdMap.end())
    {
        return it->second;
    }
    
    // If not found in memory, could potentially query the server or search filesystem
    Serial.print(F("No figure ID found for UID: "));
    Serial.println(uid);
    return String();
}

// NVS operations for UID mappings
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
        Serial.printf("RequestManager: Failed to open NVS handle: %s\n", esp_err_to_name(err));
        return false;
    }
    
    Serial.println(F("RequestManager: NVS initialized successfully"));
    return true;
}

bool RequestManager::saveUidMappings()
{
    if (nvsHandle == 0) {
        Serial.println(F("RequestManager: NVS not initialized"));
        return false;
    }
    
    // Create JSON document to store mappings
    DynamicJsonDocument doc(2048);
    JsonObject mappings = doc.to<JsonObject>();
    
    for (const auto& pair : uidToFigureIdMap) {
        mappings[pair.first] = pair.second;
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    esp_err_t err = nvs_set_str(nvsHandle, NVS_UID_MAPPING_KEY, jsonString.c_str());
    if (err != ESP_OK) {
        Serial.printf("RequestManager: Failed to save UID mappings: %s\n", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_commit(nvsHandle);
    if (err != ESP_OK) {
        Serial.printf("RequestManager: Failed to commit UID mappings: %s\n", esp_err_to_name(err));
        return false;
    }
    
    Serial.print(F("RequestManager: Saved "));
    Serial.print(uidToFigureIdMap.size());
    Serial.println(F(" UID mappings to NVS"));
    return true;
}

bool RequestManager::loadUidMappings()
{
    if (nvsHandle == 0) {
        Serial.println(F("RequestManager: NVS not initialized"));
        return false;
    }
    
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(nvsHandle, NVS_UID_MAPPING_KEY, NULL, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println(F("RequestManager: No UID mappings found in NVS"));
        return true; // Not an error, just no data
    }
    
    if (err != ESP_OK) {
        Serial.printf("RequestManager: Failed to get UID mappings size: %s\n", esp_err_to_name(err));
        return false;
    }
    
    char* jsonString = (char*)malloc(required_size);
    if (jsonString == NULL) {
        Serial.println(F("RequestManager: Failed to allocate memory for UID mappings"));
        return false;
    }
    
    err = nvs_get_str(nvsHandle, NVS_UID_MAPPING_KEY, jsonString, &required_size);
    if (err != ESP_OK) {
        Serial.printf("RequestManager: Failed to load UID mappings: %s\n", esp_err_to_name(err));
        free(jsonString);
        return false;
    }
    
    // Parse JSON
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, jsonString);
    free(jsonString);
    
    if (error) {
        Serial.printf("RequestManager: Failed to parse UID mappings JSON: %s\n", error.c_str());
        return false;
    }
    
    // Load mappings into memory
    uidToFigureIdMap.clear();
    JsonObject mappings = doc.as<JsonObject>();
    for (JsonPair pair : mappings) {
        uidToFigureIdMap[String(pair.key().c_str())] = String(pair.value().as<String>());
    }
    
    Serial.print(F("RequestManager: Loaded "));
    Serial.print(uidToFigureIdMap.size());
    Serial.println(F(" UID mappings from NVS"));
    return true;
}

RequestManager::Figure RequestManager::constructFigureFromLocalFiles(const String &uid, const String &figureId)
{
    Figure figure;
    figure.id = figureId;
    figure.name = "Local Figure"; // Default name since we don't have metadata
    figure.description = "Offline figure data";
    
    // Get all required files for this figure
    std::vector<String> figurePaths = getRequiredFilesForFigure(figureId);
    
    if (figurePaths.empty()) {
        Serial.println(F("RequestManager: No local files found for figure"));
        return figure;
    }
    
    // Group files by episode (parse path structure: /figures/{figureId}/{episodeId}/{trackId}.wav)
    std::map<String, std::vector<String>> episodeTrackMap;
    FileManager &fileManager = FileManager::getInstance();
    
    // Pre-calculate prefix length to avoid repeated String operations
    const int prefixLen = 9 + figureId.length() + 1; // "/figures/" + figureId + "/"
    
    for (const String& path : figurePaths) {
        // Only process files that actually exist
        if (!fileManager.fileExists(path)) {
            continue;
        }
        
        // Parse path to extract episode ID
        // Expected format: /figures/{figureId}/{episodeId}/{trackId}.wav
        if (path.length() > prefixLen && path.startsWith("/figures/" + figureId + "/")) {
            // Find the episode ID (after the figureId)
            int episodeEnd = path.indexOf('/', prefixLen);
            
            if (episodeEnd > prefixLen) {
                String episodeId = path.substring(prefixLen, episodeEnd);
                episodeTrackMap[episodeId].push_back(path);
            }
        }
    }
    
    // Build episodes and tracks
    for (const auto& episodePair : episodeTrackMap) {
        Episode episode;
        episode.id = episodePair.first;
        episode.name = "Episode " + episodePair.first;
        episode.description = "Local episode";
        
        for (const String& trackPath : episodePair.second) {
            // Extract track ID from filename
            String filename = trackPath.substring(trackPath.lastIndexOf('/') + 1);
            String trackId = filename.substring(0, filename.lastIndexOf('.'));
            
            Track track;
            track.id = trackId;
            track.name = "Track " + trackId;
            track.description = "Local track";
            track.localPath = trackPath;
            track.audioUrl = ""; // Not needed for offline
            track.duration = 0; // Unknown for offline
            
            episode.tracks.push_back(std::move(track));
        }
        
        if (!episode.tracks.empty()) {
            figure.episodes.push_back(std::move(episode));
        }
    }
    
    Serial.print(F("RequestManager: Constructed figure with "));
    Serial.print(figure.episodes.size());
    Serial.println(F(" episodes"));
    return figure;
}

std::vector<String> RequestManager::getRequiredFilesForFigure(const String &figureId)
{
    FileManager &fileManager = FileManager::getInstance();
    String figurePrefix = "/figures/" + figureId + "/";
    
    // Use FileManager's pattern matching to get required files for this figure
    std::vector<String> figurePaths = fileManager.getRequiredFilesByPattern(figurePrefix);
    
    Serial.print(F("RequestManager: Found "));
    Serial.print(figurePaths.size());
    Serial.print(F(" required files for figure "));
    Serial.println(figureId);
    return figurePaths;
}
