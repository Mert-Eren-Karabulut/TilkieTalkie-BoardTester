#include "RequestManager.h"

// Define static buffer
char RequestManager::responseBuffer[4096];

// Singleton instance getter
RequestManager &RequestManager::getInstance(const String &baseUrl)
{
    static RequestManager instance(baseUrl);
    return instance;
}

// Constructor
RequestManager::RequestManager(const String &baseUrl)
{
    this->baseUrl = baseUrl;
    this->timeout = 10000; // Default 10 seconds timeout
    this->lastStatusCode = 0;
    this->lastError = "";
    this->figureDownloadCompleteCallback = nullptr;
    this->connectionEstablished = false;
    this->lastUsedHost = "";
}

// Destructor
RequestManager::~RequestManager()
{
    closeConnection();
    http.end();
}

// Initialization
bool RequestManager::begin()
{
    Serial.println(F("RequestManager: Initializing..."));
    
    // Pre-configure secure client for reuse
    secureClient.setInsecure(); // For development - use proper certificates in production
    
    // Set up FileManager callback to track download completion
    FileManager &fileManager = FileManager::getInstance();
    fileManager.setDownloadCompleteCallback(staticFileDownloadCallback);

    // Try to load stored JWT token and initialize secure connection
    initSecureConnection();

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
    this->baseUrl = url;
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

void RequestManager::setDefaultHeaders()
{
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");

    if (authToken.length() > 0)
    {
        http.addHeader("Authorization", "Bearer " + authToken);
    }
}

JsonDocument RequestManager::parseResponse(String response)
{
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error)
    {
        lastError = "JSON parsing error: " + String(error.c_str());
        JsonDocument errorDoc;
        errorDoc["error"] = true;
        errorDoc["message"] = lastError;
        return errorDoc;
    }

    return doc;
}

// HTTP Methods
JsonDocument RequestManager::get(const String &endpoint)
{
    JsonDocument emptyDoc;

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
        Serial.println("No auth token available, attempting to authenticate...");
        initSecureConnection();
        if (authToken.length() == 0)
        {
            lastError = "Authentication failed - no token available";
            emptyDoc["error"] = true;
            emptyDoc["message"] = lastError;
            return emptyDoc;
        }
    }

    String url = baseUrl + endpoint;
    
    // For now, let's revert to the simpler approach to fix the immediate issue
    // The connection pooling was causing HTTP header failures
    http.end(); // Clean up any previous connections
    
    // Begin connection with retry logic
    bool connected = false;
    for (int retry = 0; retry < 3 && !connected; retry++)
    {
        if (retry > 0)
        {
            Serial.printf("RequestManager: Retrying connection attempt %d/3\n", retry + 1);
            delay(1000); // Wait before retry
        }
        
        connected = http.begin(secureClient, url);
        
        if (!connected)
        {
            Serial.printf("RequestManager: Failed to begin HTTP connection to %s (attempt %d)\n", url.c_str(), retry + 1);
        }
    }
    
    if (!connected)
    {
        lastError = "Failed to establish HTTP connection after 3 attempts";
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }

    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    setDefaultHeaders();

    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end(); // Clean up connection after use
        return parseResponse(response);
    }
    else
    {
        // More detailed error reporting
        String errorDetail = "";
        switch (httpResponseCode)
        {
            case -1: errorDetail = "Connection refused"; break;
            case -2: errorDetail = "Send header failed"; break;
            case -3: errorDetail = "Send payload failed"; break;
            case -4: errorDetail = "Not connected"; break;
            case -5: errorDetail = "Connection lost"; break;
            case -6: errorDetail = "No stream"; break;
            case -7: errorDetail = "No HTTP server"; break;
            case -8: errorDetail = "Too less RAM"; break;
            case -9: errorDetail = "Encoding error"; break;
            case -10: errorDetail = "Stream write error"; break;
            case -11: errorDetail = "Read timeout"; break;
            default: errorDetail = "Unknown error"; break;
        }
        
        lastError = "HTTP GET failed with code: " + String(httpResponseCode) + " (" + errorDetail + ")";
        Serial.printf("RequestManager: %s to URL: %s\n", lastError.c_str(), url.c_str());
        
        http.end(); // Clean up connection on error
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }
}

JsonDocument RequestManager::post(const String &endpoint, const JsonDocument &data)
{
    JsonDocument emptyDoc;

    if (!isWiFiConnected())
    {
        lastError = "WiFi not connected";
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }

    // Check if we have an auth token, if not try to get one
    if (authToken.length() == 0)
    {
        Serial.println("No auth token available, attempting to authenticate...");
        initSecureConnection();
        if (authToken.length() == 0)
        {
            lastError = "Authentication failed - no token available";
            emptyDoc["error"] = true;
            emptyDoc["message"] = lastError;
            return emptyDoc;
        }
    }

    String url = baseUrl + endpoint;
    
    // End any previous connection to ensure clean state
    http.end();
    
    // Begin connection with retry logic
    bool connected = false;
    for (int retry = 0; retry < 3 && !connected; retry++)
    {
        if (retry > 0)
        {
            Serial.printf("RequestManager: POST retrying connection attempt %d/3\n", retry + 1);
            delay(1000); // Wait before retry
        }
        
        connected = http.begin(secureClient, url);
        if (!connected)
        {
            Serial.printf("RequestManager: Failed to begin HTTP POST connection to %s (attempt %d)\n", url.c_str(), retry + 1);
        }
    }
    
    if (!connected)
    {
        lastError = "Failed to establish HTTP POST connection after 3 attempts";
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }
    
    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    setDefaultHeaders();

    String jsonString;
    serializeJson(data, jsonString);

    int httpResponseCode = http.POST(jsonString);
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        return parseResponse(response);
    }
    else
    {
        lastError = "HTTP POST failed with code: " + String(httpResponseCode);
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
    String url = baseUrl + "/hubs/" + String(macAddress) + "/token";
    
    // End any previous connection to ensure clean state
    http.end();
    
    // Begin connection with retry logic
    bool connected = false;
    for (int retry = 0; retry < 3 && !connected; retry++)
    {
        if (retry > 0)
        {
            delay(1000);
        }
        
        connected = http.begin(secureClient, url);
    }
    
    if (!connected)
    {
        lastError = F("Failed to establish JWT token connection");
        return "";
    }
    
    http.setTimeout(timeout);
    setDefaultHeaders();

    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    // response is formatted as below
    //  return response()->json([
    //              'token'   => $token,
    //              'status'  => 'success',
    //              'message' => 'Token generated successfully.'
    //          ]);
    // or
    //      return response()->json([
    //          'token'   => null,
    //          'status'  => 'error',
    //          'message' => 'No user is bound to this hub.'], 404);
    //  }

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        
        JsonDocument doc = parseResponse(response);
        // check if status is success
        if (doc["status"] == "success")
        {
            return doc["token"].as<String>();
        }
        else
        {
            lastError = doc["message"].as<String>();
            return "";
        }
    }
    else
    {
        lastError = "HTTP GET failed with code: " + String(httpResponseCode);
        http.end();
        return "";
    }
}

bool RequestManager::validateToken(String token)
{
    // send get request to BASE_URL + "/validate-token"
    String url = baseUrl + "/hubs/" + String(ESP.getEfuseMac()) + "/validate-token";
    
    // End any previous connection to ensure clean state
    http.end();
    
    // Begin connection with retry logic
    bool connected = false;
    for (int retry = 0; retry < 3 && !connected; retry++)
    {
        if (retry > 0)
        {
            Serial.printf("RequestManager: Token validation retrying connection attempt %d/3\n", retry + 1);
            delay(1000);
        }
        
        connected = http.begin(secureClient, url);
        if (!connected)
        {
            Serial.printf("RequestManager: Failed to begin token validation connection to %s (attempt %d)\n", url.c_str(), retry + 1);
        }
    }
    
    if (!connected)
    {
        lastError = "Failed to establish token validation connection after 3 attempts";
        return false;
    }
    
    http.setTimeout(timeout);
    setDefaultHeaders();

    // add Authorization header
    http.addHeader("Authorization", "Bearer " + token);

    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    if (httpResponseCode == 200)
    {
        http.end();
        return true;
    }
    else
    {
        lastError = "Token validation failed with code: " + String(httpResponseCode);
        http.end();
        return false;
    }
}

void RequestManager::initSecureConnection()
{
    ConfigManager &config = ConfigManager::getInstance();
    String storedToken = config.getJWTToken();

    bool needNewToken = true;

    if (storedToken.length() > 0)
    {
        if (validateToken(storedToken))
        {
            Serial.println("Using stored JWT token: " + storedToken);
            needNewToken = false;
        }
        else
        {
            config.setJWTToken("");
        }
    }

    if (needNewToken)
    {
        storedToken = getJWTToken();
        if (storedToken.length() > 0)
        {
            Serial.println("New JWT token obtained: " + storedToken);
            config.setJWTToken(storedToken);
        }
        else
        {
            setAuthToken("");
            return;
        }
    }

    setAuthToken(storedToken);
}

void RequestManager::getCheckFigureTracks(const String &uid)
{
    // Check network connectivity first
    if (!checkNetworkConnectivity()) {
        return;
    }
    
    String url = baseUrl + "/units/" + uid;
    
    // End any previous connection and wait a bit to ensure cleanup
    http.end();
    delay(100); // Small delay to ensure proper cleanup
    
    // Begin connection with retry logic
    bool connected = false;
    for (int retry = 0; retry < 3 && !connected; retry++)
    {
        if (retry > 0)
        {
            delay(2000); // Longer delay between retries for network stability
        }
        
        connected = http.begin(secureClient, url);
    }
    
    if (!connected)
    {
        lastError = F("Failed to establish figure tracks connection");
        return;
    }
    
    http.setTimeout(timeout);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    setDefaultHeaders();

    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        
        JsonDocument doc = parseResponse(response);
        if (doc["error"].as<bool>())
        {
            return;
        }

        // now we check if this figures tracks are already downloaded
        //file structure is /figures/{figure_id}/{episode_id}/{track_id}.mp3
        //there can be multiple episodes and tracks for each figure
        //first get instance of file manager
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
                figureDownloadCompleteCallback(uid, F("null"), false, F("No figure data found"), emptyFigure);
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
        
        // Parse episodes and tracks
        for (JsonVariant episodeVar : episodes)
        {
            JsonObject episodeObj = episodeVar.as<JsonObject>();
            Episode episode;
            episode.id = String(episodeObj["id"].as<int>());
            episode.name = episodeObj["name"].as<String>();
            episode.description = episodeObj["description"].as<String>();
            
            JsonArray tracks = episodeObj["tracks"].as<JsonArray>();
            for (JsonVariant trackVar : tracks)
            {
                JsonObject trackObj = trackVar.as<JsonObject>();
                Track track;
                track.id = String(trackObj["id"].as<int>());
                track.name = trackObj["name"].as<String>();
                track.description = trackObj["description"].as<String>();
                track.audioUrl = trackObj["audio_url"].as<String>();
                track.duration = trackObj["duration"].as<int>();
                
                // Generate local path
                track.localPath = "/figures/" + figureId + "/" + episode.id + "/" + track.id + ".mp3";
                
                episode.tracks.push_back(track);
            }
            
            figureData.episodes.push_back(episode);
        }
        
        // Collect all track paths for this figure
        std::vector<String> allTrackPaths;
        int totalTracks = 0;
        int tracksAlreadyExist = 0;
        
        for (const auto& episode : figureData.episodes)
        {
            for (const auto& track : episode.tracks)
            {
                allTrackPaths.push_back(track.localPath);
                totalTracks++;
                
                if (!fileManager.fileExists(track.localPath))
                {
                    fileManager.addRequiredFile(track.localPath, track.audioUrl);
                    // Immediately schedule the download
                    fileManager.scheduleDownload(track.audioUrl, track.localPath);
                }
                else
                {
                    tracksAlreadyExist++;
                }
            }
        }
        
        // Start tracking this figure's download progress
        if (totalTracks > 0)
        {
            // Store UID to figure ID mapping for future reference
            storeUidToFigureIdMapping(uid, figureId);
            
            startTrackingFigure(uid, figureName, figureId, allTrackPaths, figureData);
            
            // If all tracks already exist, immediately call the callback
            if (tracksAlreadyExist == totalTracks)
            {
                if (figureDownloadCompleteCallback)
                {
                    figureDownloadCompleteCallback(uid, figureName, true, F("All tracks available"), figureData);
                }
                
                // Remove from tracking since we're done
                for (auto it = activeDownloads.begin(); it != activeDownloads.end(); ++it)
                {
                    if (it->uid == uid)
                    {
                        activeDownloads.erase(it);
                        break;
                    }
                }
            }
            else
            {
                // Trigger download processing for newly added files
                fileManager.checkRequiredFiles();
            }
        }
        else
        {
            if (figureDownloadCompleteCallback)
            {
                Figure emptyFigure; // Create empty figure for error case
                figureDownloadCompleteCallback(uid, figureName, false, F("No tracks found"), emptyFigure);
            }
        }
    }
    else
    {
        lastError = "HTTP GET failed with code: " + String(httpResponseCode);
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
    // Remove any existing tracker for this UID
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
    tracker.figureData = figureData; // Store the complete figure structure
    
    // Count tracks that already exist
    FileManager &fileManager = FileManager::getInstance();
    for (const String &path : trackPaths)
    {
        if (fileManager.fileExists(path))
        {
            tracker.tracksReady++;
        }
    }
    
    activeDownloads.push_back(tracker);
    
    Serial.printf("Started tracking figure '%s' (UID: %s): %d total tracks, %d already ready\n", 
                 figureName.c_str(), uid.c_str(), tracker.totalTracks, tracker.tracksReady);
}

void RequestManager::checkFigureDownloadStatus(const String &uid)
{
    for (auto &tracker : activeDownloads)
    {
        if (tracker.uid == uid && !tracker.completed)
        {
            FileManager &fileManager = FileManager::getInstance();
            int currentReady = 0;
            int currentFailed = 0;
            
            // Recount to get current status
            for (const String &path : tracker.trackPaths)
            {
                if (fileManager.fileExists(path))
                {
                    currentReady++;
                }
                else
                {
                    // Check if this path is still in download queue or has failed permanently
                    // For simplicity, we'll assume if it doesn't exist and we've been called,
                    // it might have failed. The FileManager will handle retries.
                }
            }
            
            tracker.tracksReady = currentReady;
            
            // Check if all tracks are ready
            if (tracker.tracksReady == tracker.totalTracks)
            {
                tracker.completed = true;
                Serial.printf("Figure '%s' download completed successfully! All %d tracks are ready.\n", 
                             tracker.figureName.c_str(), tracker.totalTracks);
                
                if (figureDownloadCompleteCallback)
                {
                    figureDownloadCompleteCallback(tracker.uid, tracker.figureName, true, "All tracks downloaded successfully", tracker.figureData);
                }
                
                // Remove completed tracker
                for (auto it = activeDownloads.begin(); it != activeDownloads.end(); ++it)
                {
                    if (it->uid == uid)
                    {
                        activeDownloads.erase(it);
                        break;
                    }
                }
                return;
            }
            
            Serial.printf("Figure '%s' download progress: %d/%d tracks ready\n", 
                         tracker.figureName.c_str(), tracker.tracksReady, tracker.totalTracks);
            break;
        }
    }
}

void RequestManager::onTrackDownloadComplete(const String &path, bool success)
{
    // Find which figure this track belongs to
    for (auto &tracker : activeDownloads)
    {
        if (tracker.completed) continue;
        
        // Check if this path belongs to this figure
        bool pathBelongsToFigure = false;
        for (const String &trackPath : tracker.trackPaths)
        {
            if (trackPath == path)
            {
                pathBelongsToFigure = true;
                break;
            }
        }
        
        if (pathBelongsToFigure)
        {
            if (success)
            {
                tracker.tracksReady++;
                Serial.printf("Track downloaded successfully for figure '%s': %s (%d/%d ready)\n", 
                             tracker.figureName.c_str(), path.c_str(), tracker.tracksReady, tracker.totalTracks);
            }
            else
            {
                tracker.tracksFailed++;
                Serial.printf("Track download failed for figure '%s': %s (%d failed)\n", 
                             tracker.figureName.c_str(), path.c_str(), tracker.tracksFailed);
            }
            
            // Check if all tracks are accounted for (ready + failed = total)
            if (tracker.tracksReady + tracker.tracksFailed >= tracker.totalTracks)
            {
                tracker.completed = true;
                
                if (tracker.tracksReady == tracker.totalTracks)
                {
                    // All tracks successful
                    Serial.printf("Figure '%s' download completed successfully! All %d tracks are ready.\n", 
                                 tracker.figureName.c_str(), tracker.totalTracks);
                    
                    if (figureDownloadCompleteCallback)
                    {
                        figureDownloadCompleteCallback(tracker.uid, tracker.figureName, true, "All tracks downloaded successfully", tracker.figureData);
                    }
                }
                else
                {
                    // Some tracks failed
                    String errorMsg = String("Download completed with errors: ") + 
                                    String(tracker.tracksReady) + "/" + String(tracker.totalTracks) + " tracks ready, " +
                                    String(tracker.tracksFailed) + " failed";
                    
                    Serial.printf("Figure '%s' download completed with errors: %d/%d tracks ready, %d failed\n", 
                                 tracker.figureName.c_str(), tracker.tracksReady, tracker.totalTracks, tracker.tracksFailed);
                    
                    if (figureDownloadCompleteCallback)
                    {
                        figureDownloadCompleteCallback(tracker.uid, tracker.figureName, false, errorMsg, tracker.figureData);
                    }
                }
                
                // Remove completed tracker
                for (auto it = activeDownloads.begin(); it != activeDownloads.end(); ++it)
                {
                    if (it->uid == tracker.uid)
                    {
                        activeDownloads.erase(it);
                        break;
                    }
                }
                return;
            }
            break;
        }
    }
}

// Static callback that forwards to instance method
void RequestManager::staticFileDownloadCallback(const String& url, const String& path, bool success, const String& error)
{
    // Get the singleton instance and forward the call
    RequestManager &instance = RequestManager::getInstance("https://portal.tilkietalkie.com/api");
    instance.onTrackDownloadComplete(path, success);
}

// Store UID to Figure ID mapping for deletion purposes
void RequestManager::storeUidToFigureIdMapping(const String &uid, const String &figureId)
{
    uidToFigureIdMap[uid] = figureId;
    Serial.printf("Stored UID->FigureID mapping: %s -> %s\n", uid.c_str(), figureId.c_str());
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
    Serial.printf("No figure ID found for UID: %s\n", uid.c_str());
    return "";
}

bool RequestManager::ensureConnection(const String& host) {
    // Check if we can reuse existing connection
    if (connectionEstablished && lastUsedHost == host) {
        return true;
    }
    
    // Close previous connection if different host
    if (connectionEstablished && lastUsedHost != host) {
        closeConnection();
    }
    
    // Establish new connection
    String url = String("https://") + host;
    
    // Begin connection with retry logic
    bool connected = false;
    for (int retry = 0; retry < 3 && !connected; retry++) {
        if (retry > 0) {
            Serial.printf("RequestManager: Retrying connection attempt %d/3\n", retry + 1);
            delay(1000);
        }
        
        connected = http.begin(secureClient, url);
        
        if (!connected) {
            Serial.printf("RequestManager: Failed to begin HTTP connection to %s (attempt %d)\n", host.c_str(), retry + 1);
        }
    }
    
    if (connected) {
        connectionEstablished = true;
        lastUsedHost = host;
        Serial.printf("RequestManager: Connection established to %s (reusable)\n", host.c_str());
    } else {
        Serial.printf("RequestManager: Failed to establish connection to %s after 3 attempts\n", host.c_str());
    }
    
    return connected;
}

void RequestManager::closeConnection() {
    if (connectionEstablished) {
        http.end();
        connectionEstablished = false;
        lastUsedHost = "";
        Serial.println("RequestManager: Connection closed");
    }
}
