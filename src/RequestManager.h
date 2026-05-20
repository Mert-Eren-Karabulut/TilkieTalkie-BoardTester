#ifndef REQUEST_MANAGER_H
#define REQUEST_MANAGER_H

#include <ConfigManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <FileManager.h>
#include <nvs_flash.h>
#include <nvs.h>
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
    static const char* SD_DATA_DIR;
    static const char* SD_MANIFEST_DIR;
    static const char* SD_PENDING_MANIFEST_DIR;
    
    // Private helper methods
    bool isNetworkReady();
    void setDefaultHeaders();
    JsonDocument parseResponse(const String& response);
    
    // HTTP request helper - consolidates common request patterns
    JsonDocument executeHttpRequest(const String& endpoint, const String& method, const String* payload = nullptr);

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

    // Track and Episode structures for playlist
    struct Track {
        String id;
        String name;
        String description;
        String audioUrl;
        String localPath;  // Local file path after download
        int duration = 0;
        int sortOrder = 0;
        String checksum;
    };
    
    struct Episode {
        String id;
        String name;
        String description;
        int sortOrder = 0;
        std::vector<Track> tracks;
    };

    struct Content {
        String id;
        String name;
        String description;
        String type;
        int sortOrder = 0;
        std::vector<Episode> episodes;
    };
    
    struct Figure {
        String id;
        String name;
        String description;
        String type;
        String manifestChecksum;
        std::vector<Content> contents;
        std::vector<Track> customTracks;
    };

    // Figure download callback system
    typedef void (*FigureDownloadCompleteCallback)(const String &uid, const String &figureName, bool success, const String &error, const Figure &figure);
    void setFigureDownloadCompleteCallback(FigureDownloadCompleteCallback callback);
    
    // Helper method to get figure ID from UID (for deletion purposes)
    String getFigureIdFromUid(const String &uid);

private:
    String lastError;
    int lastStatusCode;
    
    // NVS storage for UID to Figure ID mappings
    static const char* NVS_NAMESPACE;
    static const char* NVS_UID_MAPPING_KEY;
    nvs_handle_t nvsHandle;
    
    // Figure download tracking
    FigureDownloadCompleteCallback figureDownloadCompleteCallback;
    
    struct FigureDownloadTracker {
        String uid;
        String figureName;
        String figureId;
        int totalTracks = 0;
        int tracksReady = 0;   // tracks that existed or were successfully downloaded
        int tracksFailed = 0;  // tracks that failed to download
        std::vector<String> trackPaths;
        bool completed = false;
        Figure figureData;     // Store the complete figure structure
        bool hasPendingManifest = false;
        String previousManifestJson;
    };
    
    std::vector<FigureDownloadTracker> activeDownloads;
    
    // UID to Figure ID mapping for deletion purposes
    std::map<String, String> uidToFigureIdMap;
    
    // Helper methods for tracking
    void startTrackingFigure(const String &uid, const String &figureName, const String &figureId, const std::vector<String> &trackPaths, const Figure &figureData, bool hasPendingManifest = false, const String &previousManifestJson = String());
    void checkFigureDownloadStatus(const String &uid);
    void onTrackDownloadComplete(const String &path, bool success);
    void storeUidToFigureIdMapping(const String &uid, const String &figureId);
    static void staticFileDownloadCallback(const String& url, const String& path, bool success, const String& error);
    
    // NVS operations for UID mappings
    bool initializeNVS();
    bool saveUidMappings();
    bool loadUidMappings();
    
    // Offline mode methods
    bool saveUnitManifest(const String &uid, const JsonDocument &manifest);
    bool loadUnitManifest(const String &uid, JsonDocument &manifest);
    String getUnitManifestPath(const String &uid) const;
    bool savePendingUnitManifest(const String &uid, const JsonDocument &manifest);
    bool deletePendingUnitManifest(const String &uid);
    bool loadPendingUnitManifest(const String &uid, JsonDocument &manifest);
    String getPendingUnitManifestPath(const String &uid) const;
    String normalizeStoragePath(const String &path) const;
    void finalizeTrackedManifest(FigureDownloadTracker &tracker, bool success);
    Figure buildFigureFromManifest(const JsonDocument &manifest, bool existingFilesOnly = false) const;
    std::vector<String> extractAssetPaths(const JsonDocument &manifest) const;
    bool isAssetReferencedByOtherUnit(const String &assetPath, const String &currentUid) const;
    void removeStaleAssets(const String &uid, const JsonDocument &currentManifest, const JsonDocument *previousManifest);
    void processOnlineFigureRequest(const String &uid);
    void processOfflineFigureRequest(const String &uid);
};

#endif // REQUEST_MANAGER_H