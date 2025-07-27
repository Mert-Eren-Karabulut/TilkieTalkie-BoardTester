#ifndef REQUEST_MANAGER_H
#define REQUEST_MANAGER_H

#include <ConfigManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FileManager.h>
#include <vector>
#include <map>

class RequestManager
{
private:
    HTTPClient http;
    String baseUrl;
    String authToken;
    int timeout;

    // Private helper methods
    bool isWiFiConnected();
    bool checkNetworkConnectivity();
    void setDefaultHeaders();
    JsonDocument parseResponse(String response);

    // Private constructor for singleton
    RequestManager(const String &baseUrl = "https://your-laravel-api.com/api");

    // Delete copy constructor and assignment operator
    RequestManager(const RequestManager&) = delete;
    RequestManager& operator=(const RequestManager&) = delete;

public:
    // Singleton instance getter
    static RequestManager& getInstance(const String &baseUrl = "https://your-laravel-api.com/api");

    // Destructor
    ~RequestManager();

    // Initialization
    bool begin();

    // Configuration methods
    void setBaseUrl(const String& url);
    void setAuthToken(const String& token);
    void setTimeout(int timeoutMs);

    // HTTP Methods
    // HTTP Methods
    JsonDocument get(const String& endpoint);
    JsonDocument post(const String& endpoint, const JsonDocument& data);

    // Utility methods
    bool isConnected();
    String getLastError();
    int getLastStatusCode();
    String getJWTToken();
    bool validateToken(String token);
    void initSecureConnection();

    void getCheckFigureTracks(const String &uid); // New method to fetch figure tracks

    // Track and Episode structures for playlist
    struct Track {
        String id;
        String name;
        String description;
        String audioUrl;
        String localPath;  // Local file path after download
        int duration;
    };
    
    struct Episode {
        String id;
        String name;
        String description;
        std::vector<Track> tracks;
    };
    
    struct Figure {
        String id;
        String name;
        String description;
        std::vector<Episode> episodes;
    };

    // Figure download callback system
    typedef void (*FigureDownloadCompleteCallback)(const String &uid, const String &figureName, bool success, const String &error, const Figure &figure);
    void setFigureDownloadCompleteCallback(FigureDownloadCompleteCallback callback);
    
    // Helper method to get figure ID from UID (for deletion purposes)
    String getFigureIdFromUid(const String &uid);

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