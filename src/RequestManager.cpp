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
    this->baseUrl = baseUrl;
    this->timeout = 10000; // Default 10 seconds timeout
    this->lastStatusCode = 0;
    this->lastError = "";
    this->figureDownloadCompleteCallback = nullptr;
}

// Destructor
RequestManager::~RequestManager()
{
    http.end();
}

// Initialization
bool RequestManager::begin()
{
    Serial.println("Initializing RequestManager...");
    initSecureConnection();

    // Set up FileManager callback to track download completion
    FileManager &fileManager = FileManager::getInstance();
    fileManager.setDownloadCompleteCallback(staticFileDownloadCallback);

    if (authToken.length() > 0)
    {
        Serial.println("RequestManager initialized successfully with authentication token");
        return true;
    }
    else
    {
        Serial.println("RequestManager initialized but no authentication token available");
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
    return WiFi.status() == WL_CONNECTED;
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
    http.begin(url);
    http.setTimeout(timeout);
    setDefaultHeaders();

    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        return parseResponse(response);
    }
    else
    {
        lastError = "HTTP GET failed with code: " + String(httpResponseCode);
        http.end();
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
    http.begin(url);
    http.setTimeout(timeout);
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
    // send get request to BASE_URL + "/{ESP.getEfuseMac()}/token"
    String url = baseUrl + "/hubs/" + String(ESP.getEfuseMac()) + "/token";
    http.begin(url);
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
            Serial.println("JWT token obtained successfully: " + doc["token"].as<String>());
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
        Serial.println("HTTP Error: " + lastError);
        return "";
    }
}

bool RequestManager::validateToken(String token)
{
    // send get request to BASE_URL + "/validate-token"
    String url = baseUrl + "/hubs/" + String(ESP.getEfuseMac()) + "/validate-token";
    
    http.begin(url);
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
    //we need to send get request with bearer token to the url :https://portal.tilkietalkie.com/api/units/{nfc_uid}
    Serial.println("Fetching figure tracks for UID: " + uid);
    // response will be like below
//     {
//     "id": 1,
//     "nfc_id": "c538b083-28c1-384b-ae6d-e58e1f38f1f7",
//     "created_at": "2025-07-16T17:35:19.000000Z",
//     "updated_at": "2025-07-16T17:35:19.000000Z",
//     "figure": {
//         "id": 1,
//         "thumbnail": "/testData/placeholder.png",
//         "name": "autem voluptas cupiditate",
//         "mass_assign_key": "ID-1983",
//         "description": "Reprehenderit qui ut natus in et maxime ipsam. Et omnis laboriosam error commodi ut. Ullam eligendi consectetur repudiandae minima dolor dolorem dignissimos.",
//         "thumbnail_url": "https://portal.tilkietalkie.com/storage//testData/placeholder.png",
//         "episodes": [
//             {
//                 "id": 2,
//                 "thumbnail": "/testData/placeholder.png",
//                 "name": "Episode: Dicta voluptatem.",
//                 "description": "Impedit numquam aut dolor iusto natus et. Doloribus temporibus quis dolorum at nesciunt.",
//                 "created_at": "2025-07-16T17:35:19.000000Z",
//                 "thumbnail_url": "https://portal.tilkietalkie.com/storage//testData/placeholder.png",
//                 "tracks": [
//                     {
//                         "id": 6,
//                         "name": "aut quis ratione est",
//                         "description": "Qui id qui repellat assumenda voluptas voluptatem.",
//                         "audio_file": "/testData/track.mp3",
//                         "duration": 141,
//                         "created_at": "2025-07-16T17:35:19.000000Z",
//                         "audio_url": "https://portal.tilkietalkie.com/storage//testData/track.mp3"
//                     },
//                     {
//                         "id": 7,
//                         "name": "minima esse voluptatem veritatis",
//                         "description": "Quia ut cupiditate est laborum.",
//                         "audio_file": "/testData/track.mp3",
//                         "duration": 135,
//                         "created_at": "2025-07-16T17:35:19.000000Z",
//                         "audio_url": "https://portal.tilkietalkie.com/storage//testData/track.mp3"
//                     },
//                     {
//                         "id": 8,
//                         "name": "harum odit beatae qui",
//                         "description": "Molestias excepturi sit laudantium voluptas totam ad.",
//                         "audio_file": "/testData/track.mp3",
//                         "duration": 462,
//                         "created_at": "2025-07-16T17:35:19.000000Z",
//                         "audio_url": "https://portal.tilkietalkie.com/storage//testData/track.mp3"
//                     },
//                     {
//                         "id": 9,
//                         "name": "et eaque voluptatibus temporibus",
//                         "description": "Nulla quis consectetur aut voluptatem dolor et.",
//                         "audio_file": "/testData/track.mp3",
//                         "duration": 494,
//                         "created_at": "2025-07-16T17:35:19.000000Z",
//                         "audio_url": "https://portal.tilkietalkie.com/storage//testData/track.mp3"
//                     },
//                     {
//                         "id": 10,
//                         "name": "ducimus cupiditate voluptatem ullam",
//                         "description": "Similique sint et provident.",
//                         "audio_file": "/testData/track.mp3",
//                         "duration": 230,
//                         "created_at": "2025-07-16T17:35:19.000000Z",
//                         "audio_url": "https://portal.tilkietalkie.com/storage//testData/track.mp3"
//                     }
//                 ]
//             }
//         ]
//     }
// }
    String url = baseUrl + "/units/" + uid;
    http.begin(url);
    http.setTimeout(timeout);
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
            Serial.println("Error fetching figure tracks: " + doc["message"].as<String>());
            return;
        }

        Serial.println("Figure tracks fetched successfully");
        // now we check if this figures tracks are already downloaded
        //file structure is /figures/{figure_id}/{episode_id}/{track_id}.mp3
        //there can be multiple episodes and tracks for each figure
        //first get instance of file manager
        FileManager &fileManager = FileManager::getInstance();
        JsonObject figure = doc["figure"].as<JsonObject>();
        String figureId = String(figure["id"].as<int>());
        String figureName = figure["name"].as<String>();
        JsonArray episodes = figure["episodes"].as<JsonArray>();
        
        // Collect all track paths for this figure
        std::vector<String> allTrackPaths;
        int totalTracks = 0;
        int tracksAlreadyExist = 0;
        
        for (JsonVariant episode : episodes)
        {
            String episodeId = String(episode["id"].as<int>());
            JsonArray tracks = episode["tracks"].as<JsonArray>();

            for (JsonVariant track : tracks)
            {
                String trackId = String(track["id"].as<int>());
                String trackName = track["name"].as<String>();
                String audioUrl = track["audio_url"].as<String>();

                // Check if the file already exists
                String filePath = "/figures/" + figureId + "/" + episodeId + "/" + trackId + ".mp3";
                allTrackPaths.push_back(filePath);
                totalTracks++;
                
                if (!fileManager.fileExists(filePath))
                {
                    Serial.println("Downloading track: " + trackName);
                    fileManager.addRequiredFile(filePath, audioUrl);
                    // Immediately schedule the download
                    fileManager.scheduleDownload(audioUrl, filePath);
                }
                else
                {
                    Serial.println("Track already exists: " + trackName);
                    tracksAlreadyExist++;
                }
            }
        }
        
        // Start tracking this figure's download progress
        if (totalTracks > 0)
        {
            // Store UID to figure ID mapping for future reference
            storeUidToFigureIdMapping(uid, figureId);
            
            startTrackingFigure(uid, figureName, figureId, allTrackPaths);
            
            // If all tracks already exist, immediately call the callback
            if (tracksAlreadyExist == totalTracks)
            {
                Serial.println("All tracks already exist for figure: " + figureName);
                if (figureDownloadCompleteCallback)
                {
                    figureDownloadCompleteCallback(uid, figureName, true, "All tracks already available");
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
                Serial.printf("Figure '%s': %d tracks already exist, %d need to be downloaded\n", 
                             figureName.c_str(), tracksAlreadyExist, totalTracks - tracksAlreadyExist);
                
                // Trigger download processing for newly added files
                Serial.println("Checking required files and starting downloads...");
                fileManager.checkRequiredFiles();
            }
        }
        else
        {
            Serial.println("No tracks found for figure: " + figureName);
            if (figureDownloadCompleteCallback)
            {
                figureDownloadCompleteCallback(uid, figureName, false, "No tracks found for this figure");
            }
        }
    }
    else
    {
        lastError = "HTTP GET failed with code: " + String(httpResponseCode);
        http.end();
        Serial.println("HTTP Error: " + lastError);
    }
}

// Figure download callback system implementation
void RequestManager::setFigureDownloadCompleteCallback(FigureDownloadCompleteCallback callback)
{
    this->figureDownloadCompleteCallback = callback;
}

void RequestManager::startTrackingFigure(const String &uid, const String &figureName, const String &figureId, const std::vector<String> &trackPaths)
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
                    figureDownloadCompleteCallback(tracker.uid, tracker.figureName, true, "All tracks downloaded successfully");
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
                        figureDownloadCompleteCallback(tracker.uid, tracker.figureName, true, "All tracks downloaded successfully");
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
                        figureDownloadCompleteCallback(tracker.uid, tracker.figureName, false, errorMsg);
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
