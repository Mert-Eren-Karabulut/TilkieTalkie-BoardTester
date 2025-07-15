#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <vector>
#include <map>

struct DownloadTask {
    String url;
    String localPath;
    int retryCount;
    int retryBatch; // Track which batch of retries we're on
    bool completed;
    unsigned long lastAttempt;
    unsigned long lastBatchAttempt; // Track when the last retry batch was attempted
    String checksum; // Optional for file integrity verification
};

struct FileEntry {
    String path;
    String url;
    bool required;
    String checksum; // Optional for file integrity verification
};

class FileManager {
private:
    // Singleton instance
    static FileManager* instance;
    
    // SD Card pin definitions
    static const int SD_CS_PIN = 15;
    static const int SD_MISO_PIN = 12;
    static const int SD_MOSI_PIN = 13;
    static const int SD_CLK_PIN = 14;
    
    // SD Card speed configuration (optimized for high-speed cards)
    // Default initialization tries 25MHz first, falls back to slower speeds if needed
    
    // Download configuration
    static const int MAX_RETRY_COUNT = 5;
    static const int MAX_RETRY_BATCHES = 10; // Maximum number of retry batches
    static const unsigned long RETRY_DELAY_MS = 10000; // 10 seconds between individual retries
    static const unsigned long RETRY_BATCH_DELAY_MS = 60000; // 1 minute between retry batches
    static const unsigned long CONNECTIVITY_TIMEOUT_MS = 10000; // 10 seconds
    static const size_t DOWNLOAD_BUFFER_SIZE = 8192; // 8KB buffer for downloads
    static const unsigned long DOWNLOAD_TIMEOUT_MS = 300000; // 5 minutes per download
    
    // NVS storage keys
    static const char* NVS_NAMESPACE;
    static const char* NVS_DOWNLOAD_QUEUE_KEY;
    static const char* NVS_FILE_LIST_KEY;
    static const char* NVS_DOWNLOAD_STATS_KEY;
    
    // Private members
    bool sdCardInitialized;
    bool downloadInProgress;
    std::vector<DownloadTask> downloadQueue;
    std::vector<FileEntry> requiredFiles;
    nvs_handle_t nvsHandle;
    
    // Download statistics
    struct DownloadStats {
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
    
    // NVS operations
    bool initializeNVS();
    bool saveDownloadQueue();
    bool loadDownloadQueue();
    bool saveRequiredFiles();
    bool loadRequiredFiles();
    bool saveDownloadStats();
    bool loadDownloadStats();
    
    // Download operations
    bool downloadFileFromURL(const String& url, const String& localPath, String& errorMsg);
    bool verifyFileIntegrity(const String& filePath, const String& expectedChecksum);
    void processDownloadQueue();
    void addToDownloadQueue(const String& url, const String& localPath, const String& checksum = "");
    
    // File operations
    bool createDirectoryStructure(const String& path);
    String getDirectoryFromPath(const String& path);
    size_t getFileSize(const String& path);
    
    public:
    // Singleton access
    static FileManager& getInstance();
    
    // Initialization and cleanup
    bool begin();
    void end();
    
    // Main update method (call regularly in loop)
    void update();
    
    // File management methods
    bool writeFile(const String& path, const String& content);
    bool writeFile(const String& path, const uint8_t* data, size_t length);
    String readFile(const String& path);
    bool readFile(const String& path, uint8_t* buffer, size_t& length);
    bool deleteFile(const String& path);
    bool deleteFileAndRemoveFromRequired(const String& path); // Smart delete that removes from required list
    bool renameFile(const String& oldPath, const String& newPath);
    bool createDirectory(const String& path);
    bool removeDirectory(const String& path);
    std::vector<String> listFiles(const String& directory = "/");
    bool copyFile(const String& sourcePath, const String& destPath);
    bool fileExists(const String& path);
    
    // Download management methods
    bool scheduleDownload(const String& url, const String& localPath, const String& checksum = "");
    bool downloadNow(const String& url, const String& localPath, String& errorMsg);
    void cancelAllDownloads();
    void retryFailedDownloads();
    int getPendingDownloadsCount();
    bool isDownloadInProgress() const { return downloadInProgress; }
    
    // Required files management
    bool addRequiredFile(const String& localPath, const String& url, const String& checksum = "");
    bool removeRequiredFile(const String& localPath);
    void checkRequiredFiles();
    std::vector<String> getMissingFiles();
    void downloadMissingFiles();
    
    // Status and info methods
    bool isSDCardAvailable() const { return sdCardInitialized; }
    size_t getSDCardTotalSpace();
    size_t getSDCardUsedSpace();
    size_t getSDCardFreeSpace();
    String getSDCardInfo();
    
    // Download statistics
    DownloadStats getDownloadStats() const { return downloadStats; }
    void resetDownloadStats();
    String getDownloadStatsString();
    
    // Utility methods
    void printFileList(const String& directory = "/");
    void printDownloadQueue();
    void printRequiredFiles();
    String formatBytes(size_t bytes);
    
    // Diagnostic methods
    bool testFileOperations();
    void runSDCardStressTest();
    void optimizeSDCardSpeed();
    
    // File integrity
    String calculateFileChecksum(const String& filePath);
    bool verifyFile(const String& filePath, const String& expectedChecksum);
    
    // Maintenance operations
    void cleanupTempFiles();
    void defragmentSDCard(); // Optional: implement if needed
    bool repairCorruptedFiles();
    
    // Event callbacks
    typedef void (*DownloadProgressCallback)(const String& url, const String& path, int progress, size_t downloaded, size_t total);
    typedef void (*DownloadCompleteCallback)(const String& url, const String& path, bool success, const String& error);
    typedef void (*FileSystemEventCallback)(const String& operation, const String& path, bool success);
    
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