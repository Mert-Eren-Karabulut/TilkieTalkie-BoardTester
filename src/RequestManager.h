#ifndef REQUEST_MANAGER_H
#define REQUEST_MANAGER_H

#include <ConfigManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <FileManager.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <vector>
#include <map>
#include <memory>
#include <deque>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class RequestManager
{
private:
    HTTPClient http;
    WiFiClient plainClient;          // http:// base (bench)
    WiFiClientSecure secureClient;   // https:// base (production), verified against the CA bundle
    WiFiClient *client = &plainClient; // selected by setBaseUrl()
    String baseUrl;
    String authToken;
    int timeout;
    
    static const char* SD_DATA_DIR;
    static const char* SD_MANIFEST_DIR;
    static const char* SD_PENDING_MANIFEST_DIR;
    
    // Private helper methods
    void setDefaultHeaders();
    JsonDocument parseResponse(const String& response);
    
    // Synchronous request on the calling task (token fetch only).
    JsonDocument executeHttpRequest(const String& endpoint, const String& method, const String* payload = nullptr);
    // One HTTP exchange (serialized by httpLock). Returns the HTTP status (>0), an HTTPClient
    // error (<0) or 0 when the request was not attempted; error carries the reason.
    int performHttp(const String& endpoint, const String& method, const String* payload, String& body, String& error,
                    const String& ifNoneMatch = String());
    JsonDocument responseToDocument(int code, const String& body, const String& error);

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

    // Asynchronous requests: the HTTP exchange (DNS, TLS, 8 s timeout) runs on a short-lived
    // worker task; the callback runs from update() on the loop task with the parsed body or
    // {"error":true,"message":...}. Returns false when there is no network (no callback).
    using AsyncCallback = std::function<void(JsonDocument &doc, int status)>;
    // ifNoneMatch: a manifest checksum sent as If-None-Match; the portal answers 304 (no body) when unchanged.
    bool requestAsync(const String &endpoint, const String &method, const String &payload, AsyncCallback cb,
                      const String &ifNoneMatch = String());
    bool getAsync(const String &endpoint, AsyncCallback cb) { return requestAsync(endpoint, "GET", String(), cb); }
    bool postAsync(const String &endpoint, const JsonDocument &data, AsyncCallback cb);
    bool hasPendingRequests();                      // queued, in flight or not yet dispatched
    bool isRequestPending(const String &endpoint);  // the same endpoint is queued or in flight

    // Utility methods
    bool isConnected();
    bool isNetworkReady();   // WiFi up with an IP (no portal check)
    String getJWTToken();
    bool validateToken(const String& token);
    void initConnection();
    void update();

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
    // isUpdate=false: first playable state for this tap (play it). isUpdate=true: more tracks
    // became available for a figure that was already announced (merge, do not restart).
    typedef void (*FigureDownloadCompleteCallback)(const String &uid, const String &figureName, bool success, const String &error, const Figure &figure, bool isUpdate);
    void setFigureDownloadCompleteCallback(FigureDownloadCompleteCallback callback);
    // Progress events for the UX layer (loop task).
    enum class FigureEvent { DOWNLOADS_STARTED };  // nothing playable yet, stories being downloaded
    void setFigureEventCallback(std::function<void(const String &uid, FigureEvent)> cb) { figureEventCallback = cb; }
    
    // Stories on the card for the portal: one "client-device-files" event per figure, built on the
    // SD maintenance task (manifests + file sizes), sent by ReverbClient from its outbox.
    void requestInventory();                 // loop task: queue the job
    void buildInventory();                   // maintenance task
    bool forgetFigure(const String &uid);    // delete a figure's cached stories + manifest (loop task)
    bool forgetTrack(const String &path);    // delete one cached track (loop task)

    // Content sync (docs/CONTENT-SYNC.md): a cached unit is re-fetched on tap, on the portal's
    // "sync-figure" command, and all cached units are re-checked (If-None-Match -> 304) a few
    // minutes after boot and every 12 h. Changes are applied in the background: new or replaced
    // tracks are downloaded, tracks no longer in the manifest are deleted.
    void refreshUnit(const String &uid);     // "sync-figure": re-fetch one cached unit now (ignored when not cached)
    bool isFigureDownloading(const String &uid) const; // a download tracker is open for this unit
    // After a reboot: units whose download was interrupted (pending manifest on the card) get
    // their tracker back, so the resumed downloads complete the manifest instead of leaving it
    // invisible (no final manifest = "not cached" for the next offline tap and for the inventory).
    void recoverPendingManifests();

    // Helper method to get figure ID from UID (for deletion purposes)
    String getFigureIdFromUid(const String &uid);

private:
    // Faults raised by HTTP exchanges are queued here and raised from update() on the loop task
    // (Faults is not thread-safe; performHttp also runs on the request worker).
    struct DeferredFault { uint16_t code; char detail[40]; bool speak; };
    std::vector<DeferredFault> deferredFaults;   // guarded by jobLock
    void deferFault(uint16_t code, const char *detail, bool speak);
    void raiseDeferredFaults();

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
        String pendingManifestJson;
        String previousManifestJson;
        bool emittedAny = false;  // a playable state was already announced for this tap
    };

    struct PendingFigureCompletion {
        String uid;
        String figureName;
        bool success = false;
        String error;
        Figure figureData;
        bool isUpdate = false;
    };
    
    std::vector<FigureDownloadTracker> activeDownloads;
    std::function<void(const String &, FigureEvent)> figureEventCallback;
    std::vector<PendingFigureCompletion> pendingCompletions;
    
    // UID to Figure ID mapping for deletion purposes
    std::map<String, String> uidToFigureIdMap;
    
    // Helper methods for tracking
    void startTrackingFigure(const String &uid, const String &figureName, const String &figureId, const std::vector<String> &trackPaths, const Figure &figureData, bool hasPendingManifest = false, const String &pendingManifestJson = String(), const String &previousManifestJson = String(), bool alreadyEmitted = false);
    void checkFigureDownloadStatus(const String &uid);
    void onTrackDownloadComplete(const String &path, bool success);
    void queueFigureDownloadCompletion(const String &uid, const String &figureName, bool success, const String &error, const Figure &figureData, bool isUpdate = false);
    bool emitFromCache(const String &uid);                 // cache-first: announce whatever is already on the card
    Figure filterExisting(const Figure &figure) const;      // keep only tracks whose files exist
    bool firstTrackExists(const Figure &figure) const;      // first track in play order is on the card
    void emitTrackerState(FigureDownloadTracker &tracker);  // announce the currently playable subset
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
    std::vector<String> assetsReferencedByOtherUnits(const String &currentUid) const; // one scan of the other manifests
    void removeStaleAssets(const String &uid, const JsonDocument &currentManifest, const JsonDocument *previousManifest);
    void processOnlineFigureRequest(const String &uid, bool alreadyEmitted);
    void processOfflineFigureRequest(const String &uid);
    void handleUnitManifest(const String &uid, bool alreadyEmitted, JsonDocument &doc); // loop task

    // Request worker
    struct AsyncJob {
        String endpoint;
        String method;
        String payload;
        String ifNoneMatch;
        AsyncCallback cb;
        String body;
        String error;
        int status = 0;
    };
    std::deque<AsyncJob*> jobQueue;     // waiting for the worker
    std::deque<AsyncJob*> jobDone;      // finished, callback not yet run
    String activeEndpoint;              // what the worker is doing right now
    unsigned long jobStartedAt = 0;
    bool workerRunning = false;
    SemaphoreHandle_t jobLock = nullptr;   // guards the deques + worker state
    SemaphoreHandle_t httpLock = nullptr;  // recursive: one HTTP exchange at a time
    static void workerEntry(void *arg);
    void workerLoop();
    void dispatchCompletedJobs();

    // Periodic re-check of every cached unit (one request at a time, 304 when unchanged)
    void serviceManifestRecheck();
    void recheckUnit(const String &uid);
    std::vector<String> recheckQueue;
    unsigned long nextRecheckAt = 0;
    void appendInventoryFigure(const String &uid, bool pending, size_t seq, size_t total, const char *sd);
};

#endif // REQUEST_MANAGER_H