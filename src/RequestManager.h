#ifndef REQUEST_MANAGER_H
#define REQUEST_MANAGER_H

#include <ConfigManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <FileManager.h>
#include <vector>
#include <map>
#include <memory>

class RequestManager
{
private:
    HTTPClient http;
    WiFiClient client; // Regular WiFi client for HTTP connections
    String baseUrl;
    String authToken;
    int timeout;
    
    // Response handling
    static constexpr size_t MAX_RESPONSE_SIZE = 16384; // 16KB max response
    
    // Private helper methods
    bool isWiFiConnected();
    bool checkNetworkConnectivity();
    void setDefaultHeaders();
    JsonDocument parseResponse(const String& response);
    String convertToHttp(const String& url);
    
    // Memory-efficient string building helper
    String buildUrl(const String& endpoint) const;

    // Private constructor for singleton
    RequestManager(const String &baseUrl = "http://your-laravel-api.com/api");

    // Delete copy constructor and assignment operator
    RequestManager(const RequestManager&) = delete;
    RequestManager& operator=(const RequestManager&) = delete;

public:
    // Singleton instance getter
    static RequestManager& getInstance(const String &baseUrl = "http://your-laravel-api.com/api");

    // Destructor
    ~RequestManager();

    // Initialization
    bool begin();

    // Configuration methods
    void setBaseUrl(const String& url);
    void setAuthToken(const String& token);
    void setTimeout(int timeoutMs);

    // HTTP Methods
    JsonDocument get(const String& endpoint);
    JsonDocument post(const String& endpoint, const JsonDocument& data);

    // Utility methods
    bool isConnected();
    String getLastError();
    int getLastStatusCode();
    String getJWTToken();
    bool validateToken(const String& token);
    void initConnection();

    void getCheckFigureTracks(const String &uid); // Method to fetch figure tracks

    // Track and Episode structures for playlist - using move semantics and reserved capacity
    struct Track {
        String id;
        String name;
        String description;
        String audioUrl;
        String localPath;  // Local file path after download
        int duration;
        
        // Move constructor and assignment operator for better memory management
        Track() = default;
        Track(Track&& other) noexcept = default;
        Track& operator=(Track&& other) noexcept = default;
        Track(const Track& other) = default;
        Track& operator=(const Track& other) = default;
    };
    
    struct Episode {
        String id;
        String name;
        String description;
        std::vector<Track> tracks;
        
        // Constructor with reserved capacity to prevent reallocations
        Episode() { tracks.reserve(10); } // Reserve space for typical episode size
        Episode(Episode&& other) noexcept = default;
        Episode& operator=(Episode&& other) noexcept = default;
        Episode(const Episode& other) = default;
        Episode& operator=(const Episode& other) = default;
    };
    
    struct Figure {
        String id;
        String name;
        String description;
        std::vector<Episode> episodes;
        
        // Constructor with reserved capacity to prevent reallocations
        Figure() { episodes.reserve(5); } // Reserve space for typical figure size
        Figure(Figure&& other) noexcept = default;
        Figure& operator=(Figure&& other) noexcept = default;
        Figure(const Figure& other) = default;
        Figure& operator=(const Figure& other) = default;
    };

    // Figure download callback system
    typedef void (*FigureDownloadCompleteCallback)(const String &uid, const String &figureName, bool success, const String &error, const Figure &figure);
    void setFigureDownloadCompleteCallback(FigureDownloadCompleteCallback callback);
    
    // Helper method to get figure ID from UID (for deletion purposes)
    String getFigureIdFromUid(const String &uid);
    
    // Memory cleanup methods
    void clearDownloadTrackers();
    void cleanupOldMappings(size_t maxMappings = 50);
    void cleanupCompletedTrackers();

private:
    String lastError;
    int lastStatusCode;
    
    // Figure download tracking
    FigureDownloadCompleteCallback figureDownloadCompleteCallback;
    
    struct FigureDownloadTracker {
        String uid;
        String figureName;
        String figureId;
        int totalTracks;
        int tracksReady; // tracks that existed or were successfully downloaded
        int tracksFailed; // tracks that failed to download
        std::vector<String> trackPaths;
        bool completed;
        Figure figureData; // Store the complete figure structure
        
        // Constructor with reserved capacity to prevent reallocations
        FigureDownloadTracker() { 
            trackPaths.reserve(20); // Reserve space for typical track count
            totalTracks = 0;
            tracksReady = 0;
            tracksFailed = 0;
            completed = false;
        }
        
        // Move semantics for better memory management
        FigureDownloadTracker(FigureDownloadTracker&& other) noexcept = default;
        FigureDownloadTracker& operator=(FigureDownloadTracker&& other) noexcept = default;
        FigureDownloadTracker(const FigureDownloadTracker& other) = default;
        FigureDownloadTracker& operator=(const FigureDownloadTracker& other) = default;
    };
    
    std::vector<FigureDownloadTracker> activeDownloads;
    
    // UID to Figure ID mapping for deletion purposes
    std::map<String, String> uidToFigureIdMap;
    
    // Helper methods for tracking
    void startTrackingFigure(const String &uid, const String &figureName, const String &figureId, const std::vector<String> &trackPaths, const Figure &figureData);
    void checkFigureDownloadStatus(const String &uid);
    void onTrackDownloadComplete(const String &path, bool success);
    void storeUidToFigureIdMapping(const String &uid, const String &figureId);
    static void staticFileDownloadCallback(const String& url, const String& path, bool success, const String& error);
};

#endif // REQUEST_MANAGER_H