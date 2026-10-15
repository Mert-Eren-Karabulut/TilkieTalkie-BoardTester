#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <Arduino.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <Client.h>
#include <vector>
#include <map>
#include <esp_vfs_fat.h>  // For explicit VFS unmount to fix ESP_ERR_INVALID_STATE
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>

struct DownloadTask
{
    String url;
    String localPath;
    int retryCount;            // consecutive failed attempts (drives the backoff; reset on success)
    int hardFailures;          // size/checksum mismatches (terminal after MAX_HARD_FAILURES)
    unsigned long lastAttempt; // Last attempt timestamp for retry delay
    String checksum;           // Optional for file integrity verification
};

struct FileEntry
{
    String path;
    String url;
    bool required;
    String checksum; // Optional for file integrity verification
};

class FileManager
{
private:
    // Singleton instance
    static FileManager *instance;

    // SD Card pin definitions (4-bit SDMMC mode)
    static const int SD_CLK_PIN = 48; // SD_CLK
    static const int SD_CMD_PIN = 38; // SD_CMD
    static const int SD_D0_PIN = 42;  // SD_D0
    static const int SD_D1_PIN = 47;  // SD_D1
    static const int SD_D2_PIN = 39;  // SD_D2
    static const int SD_D3_PIN = 40;  // SD_D3

    // Download configuration
    static const int MAX_RETRY_COUNT = 10;                      // used for the backoff exponent cap / legacy queue entries
    static const int MAX_HARD_FAILURES = 3;                     // size/checksum mismatches before a file is dropped
    static const unsigned long RETRY_DELAY_MS = 30000;          // base retry delay, doubled per consecutive failure
    static const unsigned long MAX_RETRY_DELAY_MS = 600000;     // 10 minute backoff cap
    static const unsigned long SD_WRITER_STALL_MS = 3000;       // network side waits this long for the SD writer
    static const size_t DOWNLOAD_BUFFER_SIZE = 32768;           // 32KB internal SRAM (DMA-aligned) - 2x32KB = 64KB total
    static const unsigned long DOWNLOAD_TIMEOUT_MS = 300000;    // 5 minutes per download

    // SD card persistence paths (instead of NVS to avoid bloat)
    static const char* SD_DATA_DIR;
    static const char* SD_DOWNLOAD_QUEUE_FILE;
    static const char* SD_REQUIRED_FILES_FILE;
    static const char* SD_DOWNLOAD_STATS_FILE;

    // Async download state.
    // Data flow: AsyncTCP onData (core 0) -> PSRAM ring (RING_SIZE) -> SD writer task copies
    // 32 KB chunks into an internal DMA bounce buffer -> SD. The ring absorbs the card's
    // multi-second write pauses (observed ~3 s every ~1.8 MB) without blocking the network.
    static const size_t RING_SIZE = 4 * 1024 * 1024;
    struct AsyncDownloadState {
        AsyncClient* client;
        File file;
        String url;
        String localPath;
        String tempPath;
        String checksum;         // expected sha256 (hex) from the manifest, may be empty
        String requestPath;      // HTTP request path (built before connect())
        int httpStatus;
        uint64_t freeBytesAtStart;
        volatile bool finishRequested;   // set by callbacks; finalizeAsyncDownload() runs on the loop task
        volatile bool finishSuccess;
        volatile bool permanentFailure;
        char finishError[96];
        volatile bool writerExited;
        volatile bool producerDone;      // no more network data: writer flushes the partial tail
        unsigned long drainStartedMs;    // finalize: when the ring drain began (bounded wait, non-blocking)
        volatile bool discardRing;       // failure: writer stops without draining
        mbedtls_sha256_context sha;      // hashed by the SD writer as it writes
        bool shaActive;
        int contentLength;
        volatile size_t totalDownloaded;
        bool headersParsed;
        unsigned long startTime;
        unsigned long connectTime;
        unsigned long lastDataTime;
        unsigned long totalWriteTime;
        int writeCount;
        int lastReportedProgress;
        char headerBuffer[1024];
        size_t headerBufferUsed;
        // Ring indices: monotonically increasing byte counters (single producer / single consumer).
        volatile size_t ringHead;
        volatile size_t ringTail;
        TaskHandle_t writeTaskHandle;
        SemaphoreHandle_t dataAvailable;
        volatile bool writeTaskRunning;

        void reset() {
            client = nullptr;
            contentLength = -1;
            totalDownloaded = 0;
            headersParsed = false;
            startTime = 0;
            connectTime = 0;
            lastDataTime = 0;
            totalWriteTime = 0;
            writeCount = 0;
            lastReportedProgress = 0;
            headerBuffer[0] = '\0';
            headerBufferUsed = 0;
            ringHead = 0;
            ringTail = 0;
            writeTaskHandle = nullptr;
            dataAvailable = nullptr;
            writeTaskRunning = false;
            url = "";
            localPath = "";
            tempPath = "";
            checksum = "";
            requestPath = "";
            httpStatus = 0;
            freeBytesAtStart = 0;
            finishRequested = false;
            finishSuccess = false;
            permanentFailure = false;
            finishError[0] = '\0';
            writerExited = false;
            producerDone = false;
            discardRing = false;
            drainStartedMs = 0;
            shaActive = false;
        }
    };

    // Private members
    bool sdCardInitialized;
    bool downloadInProgress;
    AsyncDownloadState asyncState;
    std::vector<DownloadTask> downloadQueue;
    std::vector<FileEntry> requiredFiles;
    
    // Cached free-space figure: f_getfree can scan the whole FAT (seconds on a 30 GB card)
    // while holding the filesystem lock, so it is refreshed only when idle and at most once a minute.
    uint64_t cachedFreeBytes = 0;
    unsigned long cachedFreeAt = 0;
    volatile bool freeRefreshInFlight = false;
    void requestFreeSpaceRefresh();   // runs the FAT free-cluster count on the maintenance task
    void refreshFreeSpaceCache();

    // Allocated once at startup: 32 KB internal DMA bounce buffer for SD writes and the
    // PSRAM ring that decouples the network from the card.
    uint8_t* persistentBufferA;   // internal, DMA-capable bounce buffer (DOWNLOAD_BUFFER_SIZE)
    uint8_t* ringBuffer;          // PSRAM, RING_SIZE
    bool buffersAllocated;

    // Download statistics
    struct DownloadStats
    {
        int totalDownloads;
        int successfulDownloads;
        int failedDownloads;
        unsigned long totalBytesDownloaded;
    } downloadStats;

    // Constructor (private for singleton)
    FileManager();

    // Helper methods
    bool initializeSDCard();
    bool checkConnectivity();
    bool pingGoogle();
    bool isChargingRequired();

    
    
    // SD-based persistence operations
    bool saveDownloadQueue();
    bool loadDownloadQueue();
    bool saveRequiredFiles();
    bool loadRequiredFiles();
    bool saveDownloadStats();
    bool loadDownloadStats();

    // Download operations
    bool downloadFileFromURL(const String &url, const String &localPath, const String &checksum, String &errorMsg);
    void abortAsyncStart();
    void requestAsyncFinish(bool success, const char *error, bool permanent = false);
    void finalizeAsyncDownload();
    bool writeJsonAtomic(const char *path, const JsonDocument &doc);
    bool openJsonForRead(const char *path, File &file);
    bool verifyFileIntegrity(const String &filePath, const String &expectedChecksum);
    void processDownloadQueue();
    void addToDownloadQueue(const String &url, const String &localPath, const String &checksum = "");
    
    // Async download callbacks
    static void onAsyncConnect(void* arg, AsyncClient* client);
    static void onAsyncData(void* arg, AsyncClient* client, void* data, size_t len);
    static void onAsyncDisconnect(void* arg, AsyncClient* client);
    static void onAsyncError(void* arg, AsyncClient* client, int8_t error);
    static void onAsyncTimeout(void* arg, AsyncClient* client, uint32_t time);
    
    // Async download helpers
    static void sdWriteTask(void* parameter);

    // Deferred deletes: SD_MMC.remove() takes 0.3-2 s per file on a slow card, so stale assets are
    // erased by a low-priority task on core 0 instead of stalling the loop task.
    // The queue carries MaintJob pointers: deletes, atomic file writes (tmp + rename) and the
    // free-space count. FIFO, so a write followed by a delete of the same path stays ordered.
    struct MaintJob {
        uint8_t kind;        // 0 delete, 1 write, 2 free-space refresh
        char* path;
        char* data;          // write: content (malloc'd)
        size_t len;
    };
    QueueHandle_t deleteQueue = nullptr;
    TaskHandle_t maintenanceTaskHandle = nullptr;
    std::atomic<int> pendingDeletes{0};   // all queued jobs (name kept for the callers)
    std::atomic<int> pendingWork{0};      // deletes + writes only (the free-space count must not hold off sleep)
    std::vector<String> pendingDeletePaths; // guarded by pendingLock
    SemaphoreHandle_t pendingLock = nullptr;
    volatile bool inventoryQueued = false;
    static void sdMaintenanceTask(void* parameter);
    bool ensureMaintenanceTask();
    bool enqueueJob(uint8_t kind, const String& path, const char* data, size_t len);
    bool writeFileNow(const char* path, const char* data, size_t len); // tmp + rename, any task
    bool ringPush(const uint8_t* data, size_t len);
    size_t ringAvailable() const { return asyncState.ringHead - asyncState.ringTail; }
    size_t ringFree() const { return RING_SIZE - ringAvailable() - 1; }


    // File operations
    bool createDirectoryStructure(const String &path);
    bool createDirectoryRecursive(const String &path);
    String getDirectoryFromPath(const String &path);
    size_t getFileSize(const String &path);

public:
    // Singleton access
    static FileManager &getInstance();

    // Initialization and cleanup
    bool begin();
    void end();

    // Main update method (call regularly in loop)
    void update();

    // File management methods
    bool deleteFile(const String &path);
    bool deleteFileDeferred(const String &path);                  // erased later by the SD maintenance task
    bool writeFileDeferred(const String &path, const String &content); // written later (tmp + rename) by the SD maintenance task
    bool isDeletePending(const String &path);                       // queued for deletion, not yet gone
    void enqueueInventory();                                         // build the stories inventory on the maintenance task
    bool hasPendingMaintenance() const { return pendingWork.load() > 0; }
    bool flushRequiredFiles() { return saveRequiredFiles(); }
    bool deleteFileAndRemoveFromRequired(const String &path); // Smart delete that removes from required list
    bool createDirectory(const String &path);
    bool removeDirectory(const String &path);
    std::vector<String> listFiles(const String &directory = "/");
    bool fileExists(const String &path);
    void printFileTree();
    void formatSDCard(); // Format SD card as FAT32
    // SD Card diagnostics
    void benchmarkSDCard();
    void benchmarkSequentialWrite(size_t chunkBytes, int count); // per-write timing, no network involved

    // Download management methods
    bool scheduleDownload(const String &url, const String &localPath, const String &checksum = "");
    void cancelAllDownloads();
    void retryFailedDownloads();
    int getPendingDownloadsCount();
    bool isDownloadInProgress() const { return downloadInProgress; }
    bool hasActiveDownload() const { return downloadInProgress; } // a transfer is in flight (not merely queued)

    // Required files management
    bool addRequiredFile(const String &localPath, const String &url, const String &checksum = "", bool persist = true);
    bool removeRequiredFile(const String &localPath, bool persist = true); // persist=false: caller calls flushRequiredFiles()
    void checkRequiredFiles();
    std::vector<String> getRequiredFilesByPattern(const String &pattern); // Get required files matching pattern

    // Bulk deletion methods
    void clearAllRequiredFiles();                   // Clear all required files from NVS and storage
    bool deleteFigureFiles(const String &figureId); // Delete all files for a specific figure

    // Status and info methods
    bool isSDCardAvailable() const { return sdCardInitialized; }
    size_t getSDCardTotalSpace();
    size_t getSDCardUsedSpace();
    size_t getSDCardFreeSpace();
    uint32_t getSDCardFreeMB();   // same cache, in MB (the byte value saturates at 4 GB)
    String getSDCardInfo();

    // Download statistics
    DownloadStats getDownloadStats() const { return downloadStats; }
    void resetDownloadStats();
    String getDownloadStatsString();

    // Utility methods
    void printDownloadQueue();
    void printRequiredFiles();
    String formatBytes(size_t bytes);

    // File integrity
    String calculateFileChecksum(const String &filePath);

    // Maintenance operations
    void cleanupTempFiles();

    // Event callbacks
    typedef void (*DownloadProgressCallback)(const String &url, const String &path, int progress, size_t downloaded, size_t total);
    typedef void (*DownloadCompleteCallback)(const String &url, const String &path, bool success, const String &error);
    typedef void (*FileSystemEventCallback)(const String &operation, const String &path, bool success);

    void setDownloadProgressCallback(DownloadProgressCallback callback);
    void setDownloadCompleteCallback(DownloadCompleteCallback callback);
    void setFileSystemEventCallback(FileSystemEventCallback callback);

private:
    // Callbacks
    DownloadProgressCallback downloadProgressCallback;
    DownloadCompleteCallback downloadCompleteCallback;
    FileSystemEventCallback fileSystemEventCallback;
};

#endif // FILEMANAGER_H