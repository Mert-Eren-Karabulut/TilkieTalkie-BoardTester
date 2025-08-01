#include "RequestManager.h"

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
    
    // Pre-reserve memory for containers to prevent frequent reallocations
    activeDownloads.reserve(5);
}

// Destructor
RequestManager::~RequestManager()
{
    http.end();
    
    // Clean up all tracking data
    clearDownloadTrackers();
    uidToFigureIdMap.clear();
}

// Initialization
bool RequestManager::begin()
{
    Serial.println(F("RequestManager: Initializing..."));
    
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

void RequestManager::cleanupOldMappings(size_t maxMappings)
{
    if (uidToFigureIdMap.size() > maxMappings)
    {
        // Keep only the most recent mappings (simple cleanup strategy)
        auto it = uidToFigureIdMap.begin();
        std::advance(it, uidToFigureIdMap.size() - maxMappings);
        uidToFigureIdMap.erase(uidToFigureIdMap.begin(), it);
        
        Serial.printf("RequestManager: Cleaned up old UID mappings, kept %zu entries\n", maxMappings);
    }
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
    // Check network connectivity first
    if (!checkNetworkConnectivity()) {
        Serial.println(F("RequestManager: Network connectivity check failed"));
        return;
    }
    
    // Cleanup old mappings periodically to prevent memory bloat
    cleanupOldMappings();
    
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

    Serial.println(F("RequestManager: Fetching figure tracks"));
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
                track.localPath = "/figures/" + figureId + "/" + episode.id + "/" + track.id + ".mp3";
                trackPaths.push_back(track.localPath);
                
                // Only schedule download if file doesn't already exist
                if (track.audioUrl.length() > 0)
                {
                    if (!fileManager.fileExists(track.localPath))
                    {
                        Serial.print(F("RequestManager: Starting download: "));
                        Serial.println(track.name);
                        fileManager.scheduleDownload(track.audioUrl, track.localPath);
                        tracksToDownload++;
                    }
                    else
                    {
                        Serial.print(F("RequestManager: File already exists, skipping download: "));
                        Serial.println(track.name);
                        tracksAlreadyExist++;
                    }
                }
                
                episode.tracks.push_back(std::move(track));
            }
            
            figureData.episodes.push_back(std::move(episode));
        }
        
        // Store UID to Figure ID mapping
        storeUidToFigureIdMapping(uid, figureId);
        
        // Start tracking the download progress
        startTrackingFigure(uid, figureName, figureId, trackPaths, figureData);
        
        // More informative download summary
        if (tracksToDownload > 0)
        {
            Serial.print(F("RequestManager: Started downloading "));
            Serial.print(tracksToDownload);
            Serial.print(F(" new tracks for figure: "));
            Serial.println(figureName);
        }
        if (tracksAlreadyExist > 0)
        {
            Serial.print(F("RequestManager: "));
            Serial.print(tracksAlreadyExist);
            Serial.print(F(" tracks already exist for figure: "));
            Serial.println(figureName);
        }
    }
    else
    {
        lastError = "HTTP GET failed with code: " + String(httpResponseCode);
        Serial.println("RequestManager: " + lastError);
        http.end();
    }
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
    
    Serial.print(F("Started tracking figure '"));
    Serial.print(figureName);
    Serial.print(F("' (UID: "));
    Serial.print(uid);
    Serial.print(F("): "));
    Serial.print(tracker.totalTracks);
    Serial.print(F(" total tracks, "));
    Serial.print(tracker.tracksReady);
    Serial.println(F(" already ready"));
    
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
                
                Serial.print(F("Figure download completed for: "));
                Serial.print(tracker.figureName);
                Serial.print(F(" - Success: "));
                Serial.print(success ? "Yes" : "No");
                Serial.print(F(" ("));
                Serial.print(tracker.tracksReady);
                Serial.print(F("/"));
                Serial.print(tracker.totalTracks);
                Serial.println(F(" tracks ready)"));
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
