#include "FileManager.h"
#include "BatteryManagement.h"
#include "PsramJson.h"
#include <SD_MMC.h>
#include <AsyncTCP.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <algorithm>

// Initialize static members
FileManager* FileManager::instance = nullptr;
const char* FileManager::SD_DATA_DIR = "/.filemanager";
const char* FileManager::SD_DOWNLOAD_QUEUE_FILE = "/.filemanager/download_queue.json";
const char* FileManager::SD_REQUIRED_FILES_FILE = "/.filemanager/required_files.json";
const char* FileManager::SD_DOWNLOAD_STATS_FILE = "/.filemanager/download_stats.json";

FileManager::FileManager() : 
    sdCardInitialized(false),
    downloadInProgress(false),
    downloadProgressCallback(nullptr),
    downloadCompleteCallback(nullptr),
    fileSystemEventCallback(nullptr),
    persistentBufferA(nullptr),
    ringBuffer(nullptr),
    buffersAllocated(false) {
    // Initialize download stats
    downloadStats.totalDownloads = 0;
    downloadStats.successfulDownloads = 0;
    downloadStats.failedDownloads = 0;
    downloadStats.totalBytesDownloaded = 0;
}

FileManager& FileManager::getInstance() {
    if (instance == nullptr) {
        instance = new FileManager();
    }
    return *instance;
}

bool FileManager::begin() {
    Serial.println("FileManager: Initializing...");
    
    // One 32 KB internal DMA bounce buffer (SDMMC cannot DMA from PSRAM) and a 4 MB PSRAM
    // ring between the network and the card. Allocated once, never freed.
    if (!buffersAllocated) {
        persistentBufferA = (uint8_t*)heap_caps_malloc(DOWNLOAD_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        ringBuffer = (uint8_t*)heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!persistentBufferA || !ringBuffer) {
            Serial.println("FileManager: CRITICAL: failed to allocate download buffers (32 KB internal + 4 MB PSRAM)");
            if (persistentBufferA) { free(persistentBufferA); persistentBufferA = nullptr; }
            if (ringBuffer) { free(ringBuffer); ringBuffer = nullptr; }
            buffersAllocated = false;
        } else {
            buffersAllocated = true;
            Serial.printf("FileManager: download buffers ready (%u KB internal bounce, %u KB PSRAM ring)\n",
                          (unsigned)(DOWNLOAD_BUFFER_SIZE / 1024), (unsigned)(RING_SIZE / 1024));
        }
    }
    
    // Initialize SD card first (needed for persistence)
    if (!initializeSDCard()) {
        Serial.println("FileManager: Failed to initialize SD card");
        return false;
    }
    
    // Create data directory for persistence files
    createDirectory(SD_DATA_DIR);
    
    // Load persistent data from SD card
    loadDownloadStats();
    loadDownloadQueue();
    loadRequiredFiles();
    
    Serial.println("FileManager: Initialization complete");
    return true;
}

void FileManager::end() {
    // Let queued background deletes finish before the card goes away (bounded).
    for (int i = 0; i < 100 && pendingDeletes.load() > 0; ++i) {
        delay(50);
    }
    // Save current state to SD card
    saveDownloadQueue();
    saveRequiredFiles();
    saveDownloadStats();
    for (int i = 0; i < 200 && pendingDeletes.load() > 0; ++i) {
        delay(50); // queued writes must land before the card goes away
    }

    // Unmount SD card
    SD_MMC.end();
    sdCardInitialized = false;
    
    // NOTE: We intentionally DO NOT free persistentBufferA/B here
    // They remain allocated for the lifetime of the device to prevent fragmentation
    
    Serial.println("FileManager: Shutdown complete (persistent buffers remain allocated)");
}

void FileManager::benchmarkSDCard() {
    Serial.println("\n=== SD Card Write Speed Benchmark ===");
    
    const size_t testSizes[] = {4096, 16384, 65536, 262144}; // 4KB, 16KB, 64KB, 256KB
    const int iterations = 10;
    
    // Test with Internal SRAM (DMA-aligned) - Sequential writes (simulating download)
    Serial.println("\n--- Internal SRAM (DMA-aligned) - Download Simulation ---");
    for (size_t testSize : testSizes) {
        uint8_t* testBuf = (uint8_t*)heap_caps_malloc(testSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!testBuf) {
            Serial.printf("Failed to allocate %d byte buffer from internal SRAM\n", testSize);
            continue;
        }
        
        for (size_t i = 0; i < testSize; i++) {
            testBuf[i] = random(256);
        }
        
        // Open file ONCE and write multiple times (like download does)
        File testFile = SD_MMC.open("/sd_benchmark_download.tmp", FILE_WRITE);
        if (!testFile) {
            Serial.println("Failed to create test file");
            free(testBuf);
            continue;
        }
        
        unsigned long totalTime = 0;
        unsigned long minTime = ULONG_MAX;
        unsigned long maxTime = 0;
        
        for (int i = 0; i < iterations; i++) {
            unsigned long start = micros();
            size_t written = testFile.write(testBuf, testSize);
            unsigned long elapsed = micros() - start;
            
            if (written != testSize) {
                Serial.printf("Write failed: %d/%d bytes\n", written, testSize);
                break;
            }
            
            totalTime += elapsed;
            minTime = min(minTime, elapsed);
            maxTime = max(maxTime, elapsed);
        }
        
        testFile.close();
        SD_MMC.remove("/sd_benchmark_download.tmp");
        free(testBuf);
        
        float avgTimeMs = totalTime / (float)iterations / 1000.0f;
        float minTimeMs = minTime / 1000.0f;
        float maxTimeMs = maxTime / 1000.0f;
        float speedKBps = (testSize / 1024.0f) / (avgTimeMs / 1000.0f);
        float speedMbps = speedKBps * 8.0f / 1024.0f;
        
        Serial.printf("%6d bytes: avg=%6.2f ms (min=%5.2f, max=%6.2f), %7.1f KB/s, %5.2f Mbps\n", 
                     testSize, avgTimeMs, minTimeMs, maxTimeMs, speedKBps, speedMbps);
    }
    
    // Test with PSRAM - Sequential writes
    Serial.println("\n--- PSRAM - Download Simulation ---");
    for (size_t testSize : testSizes) {
        uint8_t* testBuf = (uint8_t*)ps_malloc(testSize);
        if (!testBuf) {
            Serial.printf("Failed to allocate %d byte buffer from PSRAM\n", testSize);
            continue;
        }
        
        for (size_t i = 0; i < testSize; i++) {
            testBuf[i] = random(256);
        }
        
        File testFile = SD_MMC.open("/sd_benchmark_psram_download.tmp", FILE_WRITE);
        if (!testFile) {
            Serial.println("Failed to create test file");
            free(testBuf);
            continue;
        }
        
        unsigned long totalTime = 0;
        unsigned long minTime = ULONG_MAX;
        unsigned long maxTime = 0;
        
        for (int i = 0; i < iterations; i++) {
            unsigned long start = micros();
            size_t written = testFile.write(testBuf, testSize);
            unsigned long elapsed = micros() - start;
            
            if (written != testSize) {
                Serial.printf("Write failed: %d/%d bytes\n", written, testSize);
                break;
            }
            
            totalTime += elapsed;
            minTime = min(minTime, elapsed);
            maxTime = max(maxTime, elapsed);
        }
        
        testFile.close();
        SD_MMC.remove("/sd_benchmark_psram_download.tmp");
        free(testBuf);
        
        float avgTimeMs = totalTime / (float)iterations / 1000.0f;
        float minTimeMs = minTime / 1000.0f;
        float maxTimeMs = maxTime / 1000.0f;
        float speedKBps = (testSize / 1024.0f) / (avgTimeMs / 1000.0f);
        float speedMbps = speedKBps * 8.0f / 1024.0f;
        
        Serial.printf("%6d bytes: avg=%6.2f ms (min=%5.2f, max=%6.2f), %7.1f KB/s, %5.2f Mbps\n", 
                     testSize, avgTimeMs, minTimeMs, maxTimeMs, speedKBps, speedMbps);
    }
    
    Serial.println("\n=== Benchmark Complete ===\n");
    Serial.println("Summary: This simulates download behavior (open once, write many times).");
    Serial.println("Watch for increasing write times - this indicates FATFS overhead.\n");
}

bool FileManager::initializeSDCard() {
    Serial.println("FileManager: Initializing SD card...");
    
    Serial.printf("FileManager: Using SDMMC pins - CLK:%d, CMD:%d, D0:%d, D1:%d, D2:%d, D3:%d\n", 
                  SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    
    // A failed SDMMC begin can leave the peripheral in a bad state on this board,
    // especially while WiFi/BLE provisioning is active. Try the normal 4-bit path first,
    // then fall back to 1-bit/default-speed to distinguish marginal data lines from a dead card.
    Serial.println("FileManager: SD card initialization attempt 1/2 (4-bit)");
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    bool sdInitialized = SD_MMC.begin("/sd", false, false, BOARD_MAX_SDMMC_FREQ);
    
    if (!sdInitialized) {
        SD_MMC.end();
        Serial.println("FileManager: 4-bit SDMMC init failed, retrying in 1-bit mode at default speed");
        Serial.println("FileManager: SD card initialization attempt 2/2 (1-bit fallback)");
        sdInitialized = SD_MMC.begin("/sd", true, false, SDMMC_FREQ_DEFAULT);
    }

    if (!sdInitialized) {
        SD_MMC.end();
        Serial.println("FileManager: SD card initialization failed in both 4-bit and 1-bit modes");
        Serial.println("FileManager: Please check:");
        Serial.println("  1. SD card is properly inserted");
        Serial.println("  2. Wiring connections are correct");
        Serial.println("  3. SD card is formatted as FAT32");
        Serial.println("  4. CMD/D0 pull-ups are present and stable");
        Serial.println("  5. Power supply is adequate");
        return false;
    }
    
    // Check SD card type
    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("FileManager: No SD card attached");
        return false;
    }
    
    Serial.print("FileManager: SD card type: ");
    switch (cardType) {
        case CARD_MMC:
            Serial.println("MMC");
            break;
        case CARD_SD:
            Serial.println("SDSC");
            break;
        case CARD_SDHC:
            Serial.println("SDHC");
            break;
        default:
            Serial.println("Unknown");
            break;
    }
    
    // Print SD card size
    uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
    Serial.printf("FileManager: SD card size: %lluMB\n", cardSize);
    
    sdCardInitialized = true;
    ensureMaintenanceTask(); // background deletes/writes + the first free-space count
    
    // Create necessary directories
    createDirectory("/audio");
    createDirectory("/temp");
    createDirectory("/logs");
    createDirectory("/images");
    createDirectory("/figures");
    createDirectory("/assets");
    createDirectory("/assets/contents");
    createDirectory("/assets/custom_tracks");
    
    return true;
}

bool FileManager::checkConnectivity() {
    // The TCP connect to the asset host is the real connectivity test; a probe to a third
    // party used to block the loop for up to ~15 s per queued file whenever WAN was down.
    return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

bool FileManager::pingGoogle() {
    return checkConnectivity();
}

bool FileManager::isChargingRequired() {
    BatteryManager& battery = BatteryManager::getInstance();
    return battery.getChargingStatus();
}

void FileManager::update() {
    if (!sdCardInitialized) {
        return;
    }

    // Completion of an in-flight download is handled here, on the loop task (owner of the
    // queue, the files and the callbacks), never inside the AsyncTCP callbacks.
    if (downloadInProgress && asyncState.finishRequested) {
        finalizeAsyncDownload();
    }

    // Process download queue if conditions are met
    if (!downloadInProgress && downloadQueue.size() > 0) {
            processDownloadQueue();
    }
    
    // Check for missing required files periodically
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 300000 && WiFi.status() == WL_CONNECTED) { // Check every 5 minutes
        checkRequiredFiles();
        lastCheck = millis();
    }
}

bool FileManager::deleteFile(const String& path) {
    if (!sdCardInitialized) {
        return false;
    }
    
    bool success = SD_MMC.remove(path);
    
    if (fileSystemEventCallback) {
        fileSystemEventCallback("delete", path, success);
    }
    
    if (success) {
        Serial.printf("FileManager: File deleted successfully: %s\n", path.c_str());
    } else {
        Serial.printf("FileManager: Failed to delete file: %s\n", path.c_str());
    }
    
    return success;
}

bool FileManager::ensureMaintenanceTask() {
    if (!deleteQueue) {
        deleteQueue = xQueueCreate(32, sizeof(MaintJob*));
        if (!deleteQueue) {
            return false;
        }
    }
    if (!maintenanceTaskHandle) {
        if (xTaskCreatePinnedToCore(sdMaintenanceTask, "sd_maint", 6144, this, 2, &maintenanceTaskHandle, 0) != pdPASS) {
            maintenanceTaskHandle = nullptr;
            return false;
        }
    }
    return true;
}

bool FileManager::enqueueJob(uint8_t kind, const String& path, const char* data, size_t len) {
    if (!ensureMaintenanceTask()) {
        return false;
    }
    MaintJob* job = static_cast<MaintJob*>(calloc(1, sizeof(MaintJob)));
    if (!job) {
        return false;
    }
    job->kind = kind;
    job->path = strdup(path.c_str());
    if (data && len) {
        job->data = static_cast<char*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!job->data) job->data = static_cast<char*>(malloc(len));
        if (job->data) {
            memcpy(job->data, data, len);
            job->len = len;
        }
    }
    if (!job->path || (data && len && !job->data)) {
        free(job->path); free(job->data); free(job);
        return false;
    }
    pendingDeletes++;
    if (kind != 2) pendingWork++;
    if (xQueueSend(deleteQueue, &job, 0) != pdTRUE) {
        pendingDeletes--;
        if (kind != 2) pendingWork--;
        free(job->path); free(job->data); free(job);
        return false;
    }
    return true;
}

bool FileManager::deleteFileDeferred(const String& path) {
    if (!sdCardInitialized || path.isEmpty()) {
        return false;
    }
    if (!enqueueJob(0, path, nullptr, 0)) {
        Serial.printf("FileManager: delete queue full, deleting inline: %s\n", path.c_str());
        return deleteFile(path);
    }
    return true;
}

bool FileManager::writeFileDeferred(const String& path, const String& content) {
    if (!sdCardInitialized || path.isEmpty()) {
        return false;
    }
    if (!enqueueJob(1, path, content.c_str(), content.length())) {
        Serial.printf("FileManager: write queue full, writing inline: %s\n", path.c_str());
        return writeFileNow(path.c_str(), content.c_str(), content.length());
    }
    return true;
}

// tmp + rename so a power cut mid-write never leaves an empty or truncated file behind.
bool FileManager::writeFileNow(const char* path, const char* data, size_t len) {
    String tmp = String(path) + ".tmp";
    if (SD_MMC.exists(tmp)) {
        SD_MMC.remove(tmp);
    }
    File file = SD_MMC.open(tmp, FILE_WRITE);
    if (!file) {
        Serial.printf("FileManager: Failed to open %s for writing\n", tmp.c_str());
        return false;
    }
    size_t written = len ? file.write(reinterpret_cast<const uint8_t*>(data), len) : 0;
    file.flush();
    file.close();
    if (written != len) {
        SD_MMC.remove(tmp);
        Serial.printf("FileManager: short write %u/%u to %s\n", (unsigned)written, (unsigned)len, path);
        return false;
    }
    if (SD_MMC.exists(path)) {
        SD_MMC.remove(path); // FatFs rename does not overwrite
    }
    return SD_MMC.rename(tmp, path);
}

void FileManager::sdMaintenanceTask(void* parameter) {
    FileManager* self = static_cast<FileManager*>(parameter);
    // First free-space count of this boot. FatFS trusts the card's FSInfo record (a full FAT
    // scan of a 30 GB card took 5.5 s and, through the filesystem lock, stalled the boot).
    self->freeRefreshInFlight = true;
    self->refreshFreeSpaceCache();
    self->freeRefreshInFlight = false;
    MaintJob* job = nullptr;
    for (;;) {
        if (xQueueReceive(self->deleteQueue, &job, portMAX_DELAY) != pdTRUE || !job) {
            continue;
        }
        if (self->sdCardInitialized) {
            unsigned long t0 = millis();
            if (job->kind == 0) {
                bool ok = SD_MMC.remove(job->path);
                Serial.printf("FileManager: %s %s (%lu ms, background)\n", ok ? "deleted" : "failed to delete", job->path, millis() - t0);
            } else if (job->kind == 1) {
                bool ok = self->writeFileNow(job->path, job->data, job->len);
                unsigned long dt = millis() - t0;
                if (!ok || dt > 500) {
                    Serial.printf("FileManager: %s %s (%u bytes, %lu ms, background)\n", ok ? "wrote" : "failed to write", job->path, (unsigned)job->len, dt);
                }
            } else {
                self->refreshFreeSpaceCache();
                self->freeRefreshInFlight = false;
            }
        }
        const bool work = job->kind != 2;
        free(job->path);
        free(job->data);
        free(job);
        self->pendingDeletes--;
        if (work) self->pendingWork--;
    }
}

bool FileManager::deleteFileAndRemoveFromRequired(const String& path) {
    if (!sdCardInitialized) {
        Serial.println("FileManager: SD card not initialized");
        return false;
    }
    
    // Validate that this is a file path, not a directory
    if (path.endsWith("/") || path.isEmpty()) {
        Serial.printf("FileManager: Invalid file path (directories not allowed): %s\n", path.c_str());
        return false;
    }
    
    // Check if path points to a directory
    File fileOrDir = SD_MMC.open(path);
    if (fileOrDir) {
        if (fileOrDir.isDirectory()) {
            fileOrDir.close();
            Serial.printf("FileManager: Cannot delete directory with deleteFileAndRemoveFromRequired: %s\n", path.c_str());
            Serial.println("FileManager: Use removeDirectory() for directories");
            return false;
        }
        fileOrDir.close();
    }
    
    // Check if this file is in the required files list
    bool wasRequired = false;
    auto it = std::find_if(requiredFiles.begin(), requiredFiles.end(),
                          [&path](const FileEntry& entry) {
                              return entry.path == path;
                          });
    
    if (it != requiredFiles.end()) {
        wasRequired = true;
        Serial.printf("FileManager: File is marked as required, removing from required list: %s\n", path.c_str());
        requiredFiles.erase(it);
        saveRequiredFiles();
    }
    
    // Remove from download queue if it's currently queued
    bool wasInQueue = false;
    auto queueIt = downloadQueue.begin();
    while (queueIt != downloadQueue.end()) {
        if (queueIt->localPath == path) {
            Serial.printf("FileManager: Removing from download queue: %s\n", path.c_str());
            queueIt = downloadQueue.erase(queueIt);
            wasInQueue = true;
        } else {
            ++queueIt;
        }
    }
    
    if (wasInQueue) {
        saveDownloadQueue();
    }
    
    // Attempt to delete the file
    bool success = SD_MMC.remove(path);
    
    if (fileSystemEventCallback) {
        fileSystemEventCallback("delete_smart", path, success);
    }
    
    if (success) {
        Serial.printf("FileManager: File deleted successfully: %s\n", path.c_str());
        if (wasRequired) {
            Serial.printf("FileManager: File removed from required list to prevent re-download\n");
        }
        if (wasInQueue) {
            Serial.printf("FileManager: File removed from download queue\n");
        }
    } else {
        Serial.printf("FileManager: Failed to delete file: %s\n", path.c_str());
        
        // If deletion failed but we removed it from required list, add it back
        if (wasRequired) {
            // We need to reconstruct the FileEntry, but we don't have the URL
            // Log a warning that manual re-addition might be needed
            Serial.println("FileManager: WARNING - File was removed from required list but deletion failed");
            Serial.println("FileManager: You may need to manually re-add it with addfile command if needed");
        }
    }
    
    return success;
}

bool FileManager::createDirectory(const String& path) {
    if (!sdCardInitialized) {
        return false;
    }
    
    bool success = SD_MMC.mkdir(path);
    
    // Check if directory exists even if mkdir failed
    if (!success) {
        File dir = SD_MMC.open(path);
        success = (dir && dir.isDirectory());
        if (dir) dir.close();
    }
    
    if (fileSystemEventCallback) {
        fileSystemEventCallback("mkdir", path, success);
    }
    
    return success;
}

bool FileManager::fileExists(const String& path) {
    if (!sdCardInitialized) {
        return false;
    }

    return SD_MMC.exists(path);
}

std::vector<String> FileManager::listFiles(const String& directory) {
    std::vector<String> files;
    
    if (!sdCardInitialized) {
        return files;
    }
    
    File dir = SD_MMC.open(directory);
    if (!dir || !dir.isDirectory()) {
        Serial.printf("FileManager: Failed to open directory: %s\n", directory.c_str());
        return files;
    }
    
    File file = dir.openNextFile();
    while (file) {
        String fileName = file.name();
        if (file.isDirectory()) {
            fileName += "/";
        }
        files.push_back(fileName);
        file = dir.openNextFile();
    }
    
    dir.close();
    return files;
}

bool FileManager::scheduleDownload(const String& url, const String& localPath, const String& checksum) {
    // Check if already in queue
    for (const auto& task : downloadQueue) {
        if (task.url == url && task.localPath == localPath) {
            Serial.println("FileManager: Download already scheduled");
            return true;
        }
    }
    
    addToDownloadQueue(url, localPath, checksum);
    saveDownloadQueue();
    
    Serial.printf("FileManager: Download scheduled: %s -> %s\n", url.c_str(), localPath.c_str());
    return true;
}

void FileManager::addToDownloadQueue(const String& url, const String& localPath, const String& checksum) {
    DownloadTask task;
    task.url = url;
    task.localPath = localPath;
    task.checksum = checksum;
    task.retryCount = 0;
    task.hardFailures = 0;
    task.lastAttempt = 0;
    
    downloadQueue.push_back(task);
}

bool FileManager::downloadFileFromURL(const String& url, const String& localPath, const String& checksum, String& errorMsg) {
    if (downloadInProgress) {
        errorMsg = "Another download is in progress";
        return false;
    }
    if (!sdCardInitialized) {
        errorMsg = "SD card not initialized";
        return false;
    }

    // Media is fetched over plain HTTP by design (AsyncTCP has no TLS); integrity comes from
    // the sha256 in the manifest, which is verified while writing.
    String httpUrl = url;
    if (httpUrl.startsWith("https://")) {
        httpUrl = "http://" + httpUrl.substring(8);
    }

    String hostname, path;
    int port = 80;
    if (httpUrl.startsWith("http://")) {
        int hostStart = 7;
        int pathStart = httpUrl.indexOf('/', hostStart);
        if (pathStart == -1) {
            hostname = httpUrl.substring(hostStart);
            path = "/";
        } else {
            hostname = httpUrl.substring(hostStart, pathStart);
            path = httpUrl.substring(pathStart);
        }
        int portIndex = hostname.indexOf(':');
        if (portIndex != -1) {
            port = hostname.substring(portIndex + 1).toInt();
            hostname = hostname.substring(0, portIndex);
        }
    } else {
        errorMsg = "Invalid URL format (must start with http://)";
        return false;
    }

    downloadInProgress = true;
    Serial.printf("FileManager: Starting async download: %s -> %s\n", httpUrl.c_str(), localPath.c_str());

    if (!createDirectoryStructure(localPath)) {
        errorMsg = "Failed to create directory structure";
        downloadInProgress = false;
        return false;
    }

    asyncState.reset();
    asyncState.url = httpUrl;
    asyncState.localPath = localPath;
    asyncState.tempPath = localPath + ".tmp";
    asyncState.checksum = checksum;
    asyncState.requestPath = path;
    asyncState.startTime = millis();
    asyncState.lastDataTime = millis();
    if (cachedFreeAt == 0 || millis() - cachedFreeAt > 60000) {
        refreshFreeSpaceCache();
    }
    asyncState.freeBytesAtStart = cachedFreeBytes;

    if (!buffersAllocated || !persistentBufferA || !ringBuffer) {
        errorMsg = "Download buffer not available";
        downloadInProgress = false;
        return false;
    }

    if (SD_MMC.exists(asyncState.tempPath)) {
        SD_MMC.remove(asyncState.tempPath);
    }
    asyncState.file = SD_MMC.open(asyncState.tempPath, FILE_WRITE);
    if (!asyncState.file) {
        errorMsg = "Failed to create temporary file";
        downloadInProgress = false;
        return false;
    }

    asyncState.ringHead = 0;
    asyncState.ringTail = 0;

    mbedtls_sha256_init(&asyncState.sha);
    mbedtls_sha256_starts(&asyncState.sha, 0);
    asyncState.shaActive = true;

    asyncState.dataAvailable = xSemaphoreCreateBinary();
    if (!asyncState.dataAvailable) {
        errorMsg = "Failed to create semaphore";
        abortAsyncStart();
        return false;
    }

    // SD writer: core 0 (away from audio and the loop task), above loop priority so a
    // ready buffer is written as soon as the card accepts it.
    asyncState.writerExited = false;
    asyncState.writeTaskRunning = true;
    BaseType_t taskCreated = xTaskCreatePinnedToCore(sdWriteTask, "SDWriteTask", 6144, this, 5,
                                                     &asyncState.writeTaskHandle, 0);
    if (taskCreated != pdPASS) {
        errorMsg = "Failed to create SD write task";
        asyncState.writeTaskHandle = nullptr;
        asyncState.writeTaskRunning = false;
        abortAsyncStart();
        return false;
    }

    asyncState.client = new AsyncClient();
    if (!asyncState.client) {
        errorMsg = "Failed to create AsyncClient";
        abortAsyncStart();
        return false;
    }
    asyncState.client->onConnect(onAsyncConnect, this);
    asyncState.client->onData(onAsyncData, this);
    asyncState.client->onDisconnect(onAsyncDisconnect, this);
    asyncState.client->onError(onAsyncError, this);
    asyncState.client->onTimeout(onAsyncTimeout, this);
    asyncState.client->setNoDelay(true);
    asyncState.client->setRxTimeout(60);
    asyncState.client->setAckTimeout(5000);

    Serial.printf("FileManager: Connecting to %s:%d\n", hostname.c_str(), port);
    if (!asyncState.client->connect(hostname.c_str(), port)) {
        errorMsg = "Failed to start connection";
        abortAsyncStart();
        return false;
    }
    return true;
}

// Tear down a download that failed before the network side was live (owner task, no
// callbacks, no queue changes - the caller decides about retries).
void FileManager::abortAsyncStart() {
    if (asyncState.client) {
        asyncState.client->onConnect(nullptr, nullptr);
        asyncState.client->onData(nullptr, nullptr);
        asyncState.client->onDisconnect(nullptr, nullptr);
        asyncState.client->onError(nullptr, nullptr);
        asyncState.client->onTimeout(nullptr, nullptr);
        asyncState.client->close(true);
        delete asyncState.client;
        asyncState.client = nullptr;
    }
    if (asyncState.writeTaskHandle) {
        asyncState.discardRing = true;
        asyncState.writeTaskRunning = false;
        if (asyncState.dataAvailable) {
            xSemaphoreGive(asyncState.dataAvailable);
        }
        for (int i = 0; i < 300 && !asyncState.writerExited; ++i) {
            delay(10);
        }
        asyncState.writeTaskHandle = nullptr;
    }
    if (asyncState.dataAvailable) {
        vSemaphoreDelete(asyncState.dataAvailable);
        asyncState.dataAvailable = nullptr;
    }
    if (asyncState.shaActive) {
        mbedtls_sha256_free(&asyncState.sha);
        asyncState.shaActive = false;
    }
    if (asyncState.file) {
        asyncState.file.close();
    }
    if (SD_MMC.exists(asyncState.tempPath)) {
        SD_MMC.remove(asyncState.tempPath);
    }
    downloadInProgress = false;
    asyncState.reset();
}

void FileManager::processDownloadQueue() {
    if (downloadQueue.empty() || downloadInProgress) {
        return;
    }
    if (!checkConnectivity()) {
        return; // nothing to do without WiFi; the queue persists and sleep is no longer held off by it
    }

    for (auto it = downloadQueue.begin(); it != downloadQueue.end(); ++it) {
        DownloadTask& task = *it;

        // Exponential backoff per consecutive failure: 30 s, 60 s, ... capped at 10 min.
        if (task.lastAttempt > 0) {
            int exp = task.retryCount > 0 ? task.retryCount - 1 : 0;
            if (exp > 5) exp = 5;
            unsigned long wait = RETRY_DELAY_MS << exp;
            if (wait > MAX_RETRY_DELAY_MS) wait = MAX_RETRY_DELAY_MS;
            if (millis() - task.lastAttempt < wait) {
                continue;
            }
        }

        // Already present: integrity was verified when it was downloaded; trust it.
        if (fileExists(task.localPath)) {
            Serial.printf("FileManager: File already exists: %s\n", task.localPath.c_str());
            downloadQueue.erase(it);
            saveDownloadQueue();
            return;
        }

        if (task.retryCount >= MAX_RETRY_COUNT) {
            // Gave up: the next tap of the figure re-queues whatever its manifest still needs.
            Serial.printf("FileManager: giving up on %s after %d attempts\n", task.url.c_str(), task.retryCount);
            downloadQueue.erase(it);
            saveDownloadQueue();
            return;
        }

        task.lastAttempt = millis();
        task.retryCount++;
        Serial.printf("FileManager: Attempting download (attempt %d): %s\n", task.retryCount, task.url.c_str());

        String errorMsg;
        const String url = task.url, localPath = task.localPath, checksum = task.checksum;
        if (!downloadFileFromURL(url, localPath, checksum, errorMsg)) {
            Serial.printf("FileManager: Download failed to start: %s (%s)\n", url.c_str(), errorMsg.c_str());
            if (errorMsg.indexOf("directory") >= 0 || errorMsg.indexOf("Failed to create") >= 0 ||
                errorMsg.indexOf("open") >= 0) {
                // A local path that cannot be created never will be; retrying only keeps "syncing" on.
                Serial.println("FileManager: dropping the entry (local path error)");
                downloadQueue.erase(it);
            }
        }
        saveDownloadQueue();
        return; // one download at a time
    }
}

bool FileManager::addRequiredFile(const String& localPath, const String& url, const String& checksum, bool persist) {
    for (auto& file : requiredFiles) {
        if (file.path == localPath) {
            bool changed = false;

            if (file.url != url) {
                file.url = url;
                changed = true;
            }

            if (file.checksum != checksum) {
                file.checksum = checksum;
                changed = true;
            }

            if (changed) {
                if (persist) {
                    saveRequiredFiles();
                }
                Serial.printf("FileManager: Updated required file metadata: %s\n", localPath.c_str());
            }

            return true;
        }
    }
    
    FileEntry entry;
    entry.path = localPath;
    entry.url = url;
    entry.required = true;
    entry.checksum = checksum;
    
    requiredFiles.push_back(entry);
    if (persist) {
        saveRequiredFiles(); // callers adding a whole manifest pass persist=false and flush once
    }
    return true;
}

bool FileManager::removeRequiredFile(const String& localPath, bool persist) {
    auto it = std::remove_if(requiredFiles.begin(), requiredFiles.end(),
        [&localPath](const FileEntry& entry) {
            return entry.path == localPath;
        });

    if (it == requiredFiles.end()) {
        return false;
    }

    requiredFiles.erase(it, requiredFiles.end());
    if (persist) {
        saveRequiredFiles();
    }
    Serial.printf("FileManager: Removed required file: %s\n", localPath.c_str());
    return true;
}

void FileManager::checkRequiredFiles() {
    // Existence only. Files are hashed once, while they are downloaded; re-hashing the whole
    // library every 5 minutes on the loop task froze the device for seconds to minutes.
    for (const auto& file : requiredFiles) {
        if (!fileExists(file.path)) {
            Serial.printf("FileManager: Required file missing, scheduling download: %s\n", file.path.c_str());
            scheduleDownload(file.url, file.path, file.checksum);
        }
    }
}

std::vector<String> FileManager::getRequiredFilesByPattern(const String& pattern) {
    std::vector<String> matchingFiles;
    
    for (const auto& file : requiredFiles) {
        if (file.path.indexOf(pattern) >= 0) {
            matchingFiles.push_back(file.path);
        }
    }
    
    Serial.printf("FileManager: Found %d required files matching pattern: %s\n", 
                  matchingFiles.size(), pattern.c_str());
    return matchingFiles;
}

bool FileManager::createDirectoryStructure(const String& path) {
    String dir = getDirectoryFromPath(path);
    if (dir.isEmpty() || dir == "/") {
        return true;
    }
    return createDirectoryRecursive(dir);
}

bool FileManager::createDirectoryRecursive(const String& path) {
    if (path.isEmpty() || path == "/") {
        return true;
    }
    
    // Recursively create parent directory first
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        if (!createDirectoryRecursive(path.substring(0, lastSlash))) {
            return false;
        }
    }
    
    // Create this directory (returns false if already exists, which is OK)
    if (SD_MMC.mkdir(path)) {
        Serial.printf("FileManager: Created directory: %s\n", path.c_str());
        return true;
    }
    
    // Check if directory already exists
    File dirFile = SD_MMC.open(path);
    bool exists = (dirFile && dirFile.isDirectory());
    if (dirFile) dirFile.close();
    
    if (!exists) {
        Serial.printf("FileManager: Failed to create directory: %s\n", path.c_str());
    }
    return exists;
}

String FileManager::getDirectoryFromPath(const String& path) {
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash == -1) {
        return "";
    }
    return path.substring(0, lastSlash);
}

bool FileManager::verifyFileIntegrity(const String& filePath, const String& expectedChecksum) {
    if (expectedChecksum.isEmpty()) {
        return true; // No checksum to verify
    }
    
    String actualChecksum = calculateFileChecksum(filePath);
    return actualChecksum.equalsIgnoreCase(expectedChecksum);
}

String FileManager::calculateFileChecksum(const String& filePath) {
    File file = SD_MMC.open(filePath);
    if (!file) {
        return "";
    }

    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    mbedtls_sha256_starts(&sha256, 0);

    uint8_t buffer[512];
    uint8_t digest[32];

    while (file.available()) {
        size_t bytesRead = file.read(buffer, sizeof(buffer));
        if (bytesRead == 0) {
            break;
        }
        mbedtls_sha256_update(&sha256, buffer, bytesRead);
    }

    file.close();

    mbedtls_sha256_finish(&sha256, digest);

    mbedtls_sha256_free(&sha256);

    char checksum[65];
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(&checksum[index * 2], 3, "%02x", digest[index]);
    }
    checksum[64] = '\0';

    return String(checksum);
}

size_t FileManager::getSDCardTotalSpace() {
    if (!sdCardInitialized) {
        return 0;
    }
    return SD_MMC.cardSize();
}

size_t FileManager::getSDCardUsedSpace() {
    if (!sdCardInitialized) {
        return 0;
    }
    return SD_MMC.usedBytes();
}

void FileManager::refreshFreeSpaceCache() {
    if (!sdCardInitialized) {
        cachedFreeBytes = 0;
        return;
    }
    unsigned long t0 = millis();
    cachedFreeBytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    cachedFreeAt = millis();
    if (cachedFreeAt - t0 > 100) {
        Serial.printf("FileManager: free-space query took %lu ms\n", cachedFreeAt - t0);
    }
}

size_t FileManager::getSDCardFreeSpace() {
    if (!sdCardInitialized) {
        return 0;
    }
    // Never query the FAT while a download is writing (lock contention); otherwise refresh
    // at most once a minute, on the maintenance task. Callers only use this for the status
    // report and a sanity check.
    if (!downloadInProgress && (cachedFreeAt == 0 || millis() - cachedFreeAt > 60000)) {
        requestFreeSpaceRefresh();
    }
    uint64_t freeBytes = cachedFreeBytes;
    if (downloadInProgress && freeBytes > static_cast<uint64_t>(asyncState.totalDownloaded)) {
        freeBytes -= asyncState.totalDownloaded;
    }
    // size_t is 32-bit here; saturate instead of wrapping on cards over 4 GB.
    return freeBytes > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : static_cast<size_t>(freeBytes);
}

void FileManager::requestFreeSpaceRefresh() {
    if (freeRefreshInFlight) {
        return;
    }
    if (enqueueJob(2, "/", nullptr, 0)) {
        freeRefreshInFlight = true;
    }
}

uint32_t FileManager::getSDCardFreeMB() {
    if (!sdCardInitialized) {
        return 0;
    }
    if (!downloadInProgress && (cachedFreeAt == 0 || millis() - cachedFreeAt > 60000)) {
        requestFreeSpaceRefresh();
    }
    uint64_t freeBytes = cachedFreeBytes;
    if (downloadInProgress && freeBytes > static_cast<uint64_t>(asyncState.totalDownloaded)) {
        freeBytes -= asyncState.totalDownloaded;
    }
    return static_cast<uint32_t>(freeBytes >> 20);
}

String FileManager::getSDCardInfo() {
    if (!sdCardInitialized) {
        return "SD card not initialized";
    }
    
    String info = "SD Card Information:\n";
    info += "Type: ";
    
    uint8_t cardType = SD_MMC.cardType();
    switch (cardType) {
        case CARD_MMC:
            info += "MMC\n";
            break;
        case CARD_SD:
            info += "SDSC\n";
            break;
        case CARD_SDHC:
            info += "SDHC\n";
            break;
        default:
            info += "Unknown\n";
            break;
    }
    
    info += "Total space: " + formatBytes(getSDCardTotalSpace()) + "\n";
    info += "Used space: " + formatBytes(getSDCardUsedSpace()) + "\n";
    info += "Free space: " + formatBytes(getSDCardFreeSpace()) + "\n";
    
    return info;
}

String FileManager::formatBytes(size_t bytes) {
    if (bytes < 1024) {
        return String(bytes) + " B";
    } else if (bytes < 1024 * 1024) {
        return String(bytes / 1024.0, 2) + " KB";
    } else if (bytes < 1024 * 1024 * 1024) {
        return String(bytes / (1024.0 * 1024.0), 2) + " MB";
    } else {
        return String(bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
    }
}

void FileManager::printDownloadQueue() {
    Serial.printf("Download queue (%d items):\n", downloadQueue.size());
    
    for (size_t i = 0; i < downloadQueue.size(); i++) {
        const auto& task = downloadQueue[i];
        Serial.printf("  %d. %s -> %s\n", i + 1, task.url.c_str(), task.localPath.c_str());
        Serial.printf("      Attempts: %d/%d\n", task.retryCount, MAX_RETRY_COUNT);
        
        if (task.lastAttempt > 0) {
            unsigned long timeSince = millis() - task.lastAttempt;
            if (timeSince < RETRY_DELAY_MS) {
                Serial.printf("      Next retry in %lu seconds\n", (RETRY_DELAY_MS - timeSince) / 1000);
            }
        }
    }
}

void FileManager::printRequiredFiles() {
    Serial.printf("Required files (%d items):\n", requiredFiles.size());
    
    for (size_t i = 0; i < requiredFiles.size(); i++) {
        const auto& file = requiredFiles[i];
        bool exists = fileExists(file.path);
        Serial.printf("  %d. %s (exists: %s)\n", 
                      i + 1, file.path.c_str(), exists ? "yes" : "no");
    }
}

String FileManager::getDownloadStatsString() {
    String stats = "Download Statistics:\n";
    stats += "Total downloads: " + String(downloadStats.totalDownloads) + "\n";
    stats += "Successful: " + String(downloadStats.successfulDownloads) + "\n";
    stats += "Failed: " + String(downloadStats.failedDownloads) + "\n";
    stats += "Total bytes downloaded: " + formatBytes(downloadStats.totalBytesDownloaded) + "\n";
    
    if (downloadStats.totalDownloads > 0) {
        float successRate = (float)downloadStats.successfulDownloads / downloadStats.totalDownloads * 100;
        stats += "Success rate: " + String(successRate, 1) + "%\n";
    }
    
    return stats;
}

// SD card-based persistence operations
bool FileManager::saveDownloadQueue() {
    if (!sdCardInitialized) return false;
    
    JsonDocument doc(&psramJsonAllocator);
    JsonArray arr = doc.to<JsonArray>();
    
    for (const auto& task : downloadQueue) {
        JsonObject obj = arr.add<JsonObject>();
        obj["url"] = task.url;
        obj["path"] = task.localPath;
        obj["retries"] = task.retryCount;
        obj["hard"] = task.hardFailures;
        obj["lastAttempt"] = task.lastAttempt;
        obj["checksum"] = task.checksum;
    }
    return writeJsonAtomic(SD_DOWNLOAD_QUEUE_FILE, doc);
}

// Write-to-temp + rename so a power cut mid-write never leaves an empty/truncated list
// (an unparseable file used to be loaded as "no files", and the next save made that permanent).
bool FileManager::writeJsonAtomic(const char *path, const JsonDocument &doc) {
    String json;
    json.reserve(measureJson(doc) + 1);
    if (serializeJson(doc, json) == 0) {
        return false;
    }
    // Serialising is cheap; the SD write (up to seconds on a slow card) runs on the maintenance task.
    return writeFileDeferred(path, json);
}

bool FileManager::openJsonForRead(const char *path, File &file) {
    file = SD_MMC.open(path, FILE_READ);
    if (file) {
        return true;
    }
    // A crash between remove(path) and rename(tmp) leaves only the temp file: use it.
    String tmp = String(path) + ".tmp";
    file = SD_MMC.open(tmp, FILE_READ);
    return (bool)file;
}

bool FileManager::loadDownloadQueue() {
    if (!sdCardInitialized) return false;
    
    File file;
    if (!openJsonForRead(SD_DOWNLOAD_QUEUE_FILE, file)) {
        // No saved queue, start fresh
        return true;
    }
    
    JsonDocument doc(&psramJsonAllocator);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("FileManager: Failed to parse download queue: %s\n", error.c_str());
        return false;
    }
    
    downloadQueue.clear();
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        DownloadTask task;
        task.url = obj["url"].as<String>();
        task.localPath = obj["path"].as<String>();
        task.retryCount = obj["retries"] | 0;
        task.hardFailures = obj["hard"] | 0;
        task.lastAttempt = obj["lastAttempt"] | 0;
        task.checksum = obj["checksum"].as<String>();
        downloadQueue.push_back(task);
    }
    
    Serial.printf("FileManager: Loaded %d items from download queue\n", downloadQueue.size());
    return true;
}

bool FileManager::saveRequiredFiles() {
    if (!sdCardInitialized) return false;
    
    JsonDocument doc(&psramJsonAllocator);
    JsonArray arr = doc.to<JsonArray>();
    
    for (const auto& entry : requiredFiles) {
        JsonObject obj = arr.add<JsonObject>();
        obj["path"] = entry.path;
        obj["url"] = entry.url;
        obj["checksum"] = entry.checksum;
    }
    
    if (!writeJsonAtomic(SD_REQUIRED_FILES_FILE, doc)) {
        return false;
    }
    Serial.printf("FileManager: Saved %d required files to SD card\n", requiredFiles.size());
    return true;
}

bool FileManager::loadRequiredFiles() {
    if (!sdCardInitialized) return false;
    
    File file;
    if (!openJsonForRead(SD_REQUIRED_FILES_FILE, file)) {
        Serial.println("FileManager: No saved required files found");
        return true;
    }
    
    JsonDocument doc(&psramJsonAllocator);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("FileManager: Failed to parse required files: %s\n", error.c_str());
        return false;
    }
    
    requiredFiles.clear();
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        FileEntry entry;
        entry.path = obj["path"].as<String>();
        entry.url = obj["url"].as<String>();
        entry.checksum = obj["checksum"].as<String>();
        entry.required = true;
        requiredFiles.push_back(entry);
    }
    
    Serial.printf("FileManager: Loaded %d required files from SD card\n", requiredFiles.size());
    return true;
}

bool FileManager::saveDownloadStats() {
    if (!sdCardInitialized) return false;
    
    JsonDocument doc(&psramJsonAllocator);
    doc["totalDownloads"] = downloadStats.totalDownloads;
    doc["successfulDownloads"] = downloadStats.successfulDownloads;
    doc["failedDownloads"] = downloadStats.failedDownloads;
    doc["totalBytesDownloaded"] = downloadStats.totalBytesDownloaded;
    
    return writeJsonAtomic(SD_DOWNLOAD_STATS_FILE, doc);
}

bool FileManager::loadDownloadStats() {
    if (!sdCardInitialized) return false;
    
    File file = SD_MMC.open(SD_DOWNLOAD_STATS_FILE, FILE_READ);
    if (!file) {
        // No saved stats, start with defaults (already initialized in constructor)
        return true;
    }
    
    JsonDocument doc(&psramJsonAllocator);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("FileManager: Failed to parse download stats: %s\n", error.c_str());
        return false;
    }
    
    downloadStats.totalDownloads = doc["totalDownloads"] | 0;
    downloadStats.successfulDownloads = doc["successfulDownloads"] | 0;
    downloadStats.failedDownloads = doc["failedDownloads"] | 0;
    downloadStats.totalBytesDownloaded = doc["totalBytesDownloaded"] | 0UL;
    
    return true;
}

// Callback setters
void FileManager::setDownloadProgressCallback(DownloadProgressCallback callback) {
    downloadProgressCallback = callback;
}

void FileManager::setDownloadCompleteCallback(DownloadCompleteCallback callback) {
    downloadCompleteCallback = callback;
}

void FileManager::setFileSystemEventCallback(FileSystemEventCallback callback) {
    fileSystemEventCallback = callback;
}

// Additional utility methods
void FileManager::cancelAllDownloads() {
    downloadQueue.clear();
    saveDownloadQueue();
    Serial.println("FileManager: All downloads cancelled");
}

void FileManager::retryFailedDownloads() {
    for (auto& task : downloadQueue) {
        task.retryCount = 0;
        task.lastAttempt = 0;
    }
    saveDownloadQueue();
    Serial.println("FileManager: All downloads reset for retry");
}

int FileManager::getPendingDownloadsCount() {
    return downloadQueue.size();
}

void FileManager::cleanupTempFiles() {
    std::vector<String> tempFiles = listFiles("/temp");
    for (const auto& file : tempFiles) {
        String fullPath = "/temp/" + file;
        if (file.endsWith(".tmp") || file.endsWith(".partial")) {
            deleteFile(fullPath);
            Serial.printf("FileManager: Cleaned up temp file: %s\n", fullPath.c_str());
        }
    }
}

void FileManager::resetDownloadStats() {
    downloadStats.totalDownloads = 0;
    downloadStats.successfulDownloads = 0;
    downloadStats.failedDownloads = 0;
    downloadStats.totalBytesDownloaded = 0;
    saveDownloadStats();
    Serial.println("FileManager: Download statistics reset");
}

bool FileManager::removeDirectory(const String& path) {
    bool success = SD_MMC.rmdir(path);
    
    if (fileSystemEventCallback) {
        fileSystemEventCallback("rmdir", path, success);
    }
    
    if (success) {
        Serial.printf("FileManager: Directory removed successfully: %s\n", path.c_str());
    } else {
        Serial.printf("FileManager: Failed to remove directory: %s\n", path.c_str());
    }
    
    return success;
}

void FileManager::printFileTree() {
    Serial.println("=== SD Card File Tree ===");

    std::function<void(const String&, int)> printTree = [&](const String& dir, int depth) {
        File dirFile = SD_MMC.open(dir);
        if (!dirFile || !dirFile.isDirectory()) {
            if (dirFile) dirFile.close();
            return;
        }

        while (true) {
            File entry = dirFile.openNextFile();
            if (!entry) break;

            for (int i = 0; i < depth; ++i) Serial.print("  ");
            String name = entry.name();
            if (entry.isDirectory()) {
                Serial.printf("%s/\n", name.c_str());
                String subDir = dir;
                if (!subDir.endsWith("/")) subDir += "/";
                subDir += name;
                entry.close();
                printTree(subDir, depth + 1);
            } else {
                Serial.println(name);
                entry.close();
            }
        }
        dirFile.close();
    };

    printTree("/", 0);
    Serial.println("==========================");
}

void FileManager::formatSDCard() {
    Serial.println("=== Formatting SD Card ===");
    
    if (!sdCardInitialized) {
        Serial.println("SD card not initialized - run sddiag first");
        return;
    }
    Serial.println("Formatting...");

    // Remove all files and directories recursively
    std::function<void(const String&)> removeAll = [&](const String& dir) {
        File dirFile = SD_MMC.open(dir);
        if (!dirFile) return;
        File entry = dirFile.openNextFile();
        while (entry) {
            String entryName = entry.name();
            if (entry.isDirectory()) {
                entry.close();
                String subDir = dir;
                if (!subDir.endsWith("/")) subDir += "/";
                subDir += entryName;
                removeAll(subDir);
                SD_MMC.rmdir(subDir);
            } else {
                entry.close();
                String filePath = dir;
                if (!filePath.endsWith("/")) filePath += "/";
                filePath += entryName;
                SD_MMC.remove(filePath);
            }
            entry = dirFile.openNextFile();
        }
        dirFile.close();
    };

    removeAll("/");

    // Optionally recreate standard directories
    createDirectory("/audio");
    createDirectory("/temp");
    createDirectory("/logs");
    createDirectory("/images");

    Serial.println("Format complete. All files and directories removed.");
}

// Bulk deletion methods implementation
void FileManager::clearAllRequiredFiles() {
    Serial.println("FileManager: Clearing all required files from NVS and storage...");
    
    int filesDeleted = 0;
    int filesNotFound = 0;
    
    // Delete files based on required files list
    for (const auto& file : requiredFiles) {
        if (fileExists(file.path)) {
            if (deleteFile(file.path)) {
                filesDeleted++;
                Serial.printf("Deleted: %s\n", file.path.c_str());
            } else {
                Serial.printf("Failed to delete: %s\n", file.path.c_str());
            }
        } else {
            filesNotFound++;
            Serial.printf("File not found: %s\n", file.path.c_str());
        }
    }
    
    // Clear required files list from memory and NVS
    requiredFiles.clear();
    saveRequiredFiles();
    
    // Also clear download queue
    downloadQueue.clear();
    saveDownloadQueue();
    
    Serial.printf("FileManager: Cleared all required files. Deleted %d files, %d were already missing.\n", 
                 filesDeleted, filesNotFound);
}

bool FileManager::deleteFigureFiles(const String& figureId) {
    Serial.printf("FileManager: Deleting all files for figure ID: %s\n", figureId.c_str());
    
    String figureDir = "/figures/" + figureId;
    int filesDeleted = 0;
    int requiredFilesRemoved = 0;
    
    // Remove files from required files list that belong to this figure
    auto it = requiredFiles.begin();
    while (it != requiredFiles.end()) {
        if (it->path.startsWith(figureDir)) {
            Serial.printf("Removing from required list: %s\n", it->path.c_str());
            it = requiredFiles.erase(it);
            requiredFilesRemoved++;
        } else {
            ++it;
        }
    }
    
    // Remove from download queue as well
    auto queueIt = downloadQueue.begin();
    while (queueIt != downloadQueue.end()) {
        if (queueIt->localPath.startsWith(figureDir)) {
            Serial.printf("Removing from download queue: %s\n", queueIt->localPath.c_str());
            queueIt = downloadQueue.erase(queueIt);
        } else {
            ++queueIt;
        }
    }
    
    // Delete the entire figure directory from storage
    if (fileExists(figureDir)) {
        // List all files in the figure directory recursively and delete them
        std::vector<String> allFiles = listFiles(figureDir);
        for (const String& file : allFiles) {
            String fullPath = figureDir + "/" + file;
            if (deleteFile(fullPath)) {
                filesDeleted++;
                Serial.printf("Deleted file: %s\n", fullPath.c_str());
            }
        }
        
        // Try to remove the directory structure
        if (removeDirectory(figureDir)) {
            Serial.printf("Removed directory: %s\n", figureDir.c_str());
        }
    } else {
        Serial.printf("Figure directory does not exist: %s\n", figureDir.c_str());
    }
    
    // Save updated required files and download queue
    saveRequiredFiles();
    saveDownloadQueue();
    
    Serial.printf("FileManager: Figure deletion complete. Removed %d required file entries, deleted %d files.\n", 
                 requiredFilesRemoved, filesDeleted);
    
    return true;
}

// Sequential write benchmark with per-write timing. No network involved: isolates the
// card's own behaviour (e.g. periodic multi-second pauses) from the download pipeline.
void FileManager::benchmarkSequentialWrite(size_t chunkBytes, int count) {
    if (!sdCardInitialized || downloadInProgress || !persistentBufferA) {
        Serial.println("FileManager: bench unavailable (SD not ready or download in progress)");
        return;
    }
    if (chunkBytes == 0 || chunkBytes > DOWNLOAD_BUFFER_SIZE) {
        chunkBytes = DOWNLOAD_BUFFER_SIZE;
    }
    if (count <= 0) {
        count = 64;
    }
    const char* path = "/temp/bench.bin";
    createDirectory("/temp");
    if (SD_MMC.exists(path)) {
        SD_MMC.remove(path);
    }
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("FileManager: bench: cannot open file");
        return;
    }
    for (size_t i = 0; i < chunkBytes; ++i) {
        persistentBufferA[i] = (uint8_t)(i * 7 + 13);
    }
    unsigned long minUs = ~0UL, maxUs = 0, sumUs = 0;
    int slow = 0;
    unsigned long t0 = millis();
    for (int i = 0; i < count; ++i) {
        unsigned long ws = micros();
        size_t written = f.write(persistentBufferA, chunkBytes);
        unsigned long dt = micros() - ws;
        if (written != chunkBytes) {
            Serial.printf("FileManager: bench: short write at #%d (%u/%u)\n", i, (unsigned)written, (unsigned)chunkBytes);
            break;
        }
        if (dt < minUs) minUs = dt;
        if (dt > maxUs) maxUs = dt;
        sumUs += dt;
        if (dt > 300000) {
            slow++;
            Serial.printf("FileManager: bench: write #%d took %lu ms\n", i, dt / 1000);
        }
    }
    unsigned long fs = micros();
    f.flush();
    unsigned long flushUs = micros() - fs;
    f.close();
    unsigned long total = millis() - t0;
    SD_MMC.remove(path);
    Serial.printf("FileManager: bench: %d x %u KB: min %.1f ms, avg %.1f ms, max %.1f ms, >300ms: %d, flush %lu ms, total %lu ms = %.2f MB/s\n",
                  count, (unsigned)(chunkBytes / 1024), minUs / 1000.0, (sumUs / 1000.0) / count, maxUs / 1000.0,
                  slow, flushUs / 1000, total, total > 0 ? (count * (double)chunkBytes) / (total / 1000.0) / 1048576.0 : 0.0);
}

// ===== Async Download Callbacks =====
// These run on the async_tcp task (core 0, priority 23). They only parse headers, copy bytes
// into the double buffer and set flags. Everything slow or state-changing (joining the SD
// writer, closing/renaming files, hashing, queue and stats persistence, completion callbacks)
// runs in finalizeAsyncDownload() on the loop task. The AsyncClient is never deleted from
// inside its own callbacks (that was a use-after-free, and the semaphores it still used were
// deleted on a 200 ms timer: `assert failed: xQueueGenericSend queue.c:936`, reproducible on
// every multi-megabyte download).

void FileManager::requestAsyncFinish(bool success, const char *error, bool permanent) {
    if (asyncState.finishRequested) {
        return;
    }
    asyncState.finishSuccess = success;
    asyncState.permanentFailure = permanent;
    strncpy(asyncState.finishError, error ? error : "", sizeof(asyncState.finishError) - 1);
    asyncState.finishError[sizeof(asyncState.finishError) - 1] = '\0';
    asyncState.finishRequested = true;
}

void FileManager::onAsyncConnect(void* arg, AsyncClient* client) {
    FileManager* self = static_cast<FileManager*>(arg);
    if (self->asyncState.finishRequested) {
        return;
    }
    self->asyncState.connectTime = millis() - self->asyncState.startTime;
    Serial.printf("FileManager: Connected in %lu ms\n", self->asyncState.connectTime);

    String hostname = self->asyncState.url;
    if (hostname.startsWith("http://")) {
        hostname = hostname.substring(7);
        int slashPos = hostname.indexOf('/');
        if (slashPos != -1) {
            hostname = hostname.substring(0, slashPos);
        }
    }
    String request = "GET " + self->asyncState.requestPath + " HTTP/1.1\r\n";
    request += "Host: " + hostname + "\r\n";
    request += "Connection: close\r\n";
    request += "Accept-Encoding: identity\r\n";
    request += "User-Agent: TilkieTalkie/1.0 (ESP32-S3)\r\n";
    request += "\r\n";
    client->write(request.c_str(), request.length());
    self->asyncState.lastDataTime = millis();
}

void FileManager::onAsyncData(void* arg, AsyncClient* client, void* data, size_t len) {
    FileManager* self = static_cast<FileManager*>(arg);
    if (self->asyncState.finishRequested) {
        return; // finishing; ignore whatever else the server sends
    }
    unsigned long dataCallbackStart = millis();
    self->asyncState.lastDataTime = dataCallbackStart;

    auto bufferBodyData = [&](const uint8_t* chunk, size_t chunkLen) -> bool {
        if (!self->ringPush(chunk, chunkLen)) {
            self->requestAsyncFinish(false, "SD writer stalled (ring full)");
            return false;
        }
        return true;
    };

    if (!self->asyncState.headersParsed) {
        size_t headerCapacity = sizeof(self->asyncState.headerBuffer) - 1;
        size_t previousHeaderBytes = self->asyncState.headerBufferUsed;
        size_t bytesToAppend = len;
        if (self->asyncState.headerBufferUsed + bytesToAppend > headerCapacity) {
            bytesToAppend = headerCapacity - self->asyncState.headerBufferUsed;
        }
        if (bytesToAppend > 0) {
            memcpy(self->asyncState.headerBuffer + self->asyncState.headerBufferUsed, data, bytesToAppend);
            self->asyncState.headerBufferUsed += bytesToAppend;
            self->asyncState.headerBuffer[self->asyncState.headerBufferUsed] = '\0';
        }

        char* headerEnd = strstr(self->asyncState.headerBuffer, "\r\n\r\n");
        if (!headerEnd) {
            if (self->asyncState.headerBufferUsed == headerCapacity) {
                self->requestAsyncFinish(false, "HTTP headers too large or incomplete");
            }
            return;
        }

        // Status line: anything but 200 is a failure (4xx permanently; a 301/302 means the
        // server now insists on HTTPS or moved the file - the old code saved the HTML page).
        int status = 0;
        if (sscanf(self->asyncState.headerBuffer, "HTTP/%*d.%*d %d", &status) != 1) {
            self->requestAsyncFinish(false, "Malformed HTTP status line");
            return;
        }
        self->asyncState.httpStatus = status;
        if (status != 200) {
            char msg[48];
            snprintf(msg, sizeof(msg), "HTTP %d", status);
            self->requestAsyncFinish(false, msg, status >= 400 && status < 500);
            return;
        }
        if (strcasestr(self->asyncState.headerBuffer, "transfer-encoding: chunked")) {
            self->requestAsyncFinish(false, "Chunked transfer encoding not supported");
            return;
        }
        const char* contentLengthPtr = strcasestr(self->asyncState.headerBuffer, "content-length:");
        if (contentLengthPtr) {
            self->asyncState.contentLength = atoi(contentLengthPtr + 15);
            Serial.printf("FileManager: Content-Length: %d bytes\n", self->asyncState.contentLength);
            if (self->asyncState.contentLength > 0 &&
                static_cast<uint64_t>(self->asyncState.contentLength) + (1024 * 1024) > self->asyncState.freeBytesAtStart) {
                self->requestAsyncFinish(false, "Insufficient SD card space");
                return;
            }
        }

        self->asyncState.headersParsed = true;
        size_t headerSize = (headerEnd + 4) - self->asyncState.headerBuffer;
        size_t headerBytesFromCurrentChunk = headerSize > previousHeaderBytes ? (headerSize - previousHeaderBytes) : 0;
        if (headerBytesFromCurrentChunk > len) {
            self->requestAsyncFinish(false, "HTTP header accounting error");
            return;
        }
        size_t bodyBytesInCurrentChunk = len - headerBytesFromCurrentChunk;
        if (bodyBytesInCurrentChunk > 0) {
            bufferBodyData(static_cast<const uint8_t*>(data) + headerBytesFromCurrentChunk, bodyBytesInCurrentChunk);
        }
        return;
    }

    if (!bufferBodyData(static_cast<const uint8_t*>(data), len)) {
        return;
    }

    unsigned long callbackDuration = millis() - dataCallbackStart;
    if (callbackDuration > 500) {
        Serial.printf("FileManager: slow onData callback: %lu ms (%u bytes)\n", callbackDuration, (unsigned)len);
    }

    if (self->asyncState.contentLength > 0) {
        int progress = (int)((uint64_t)self->asyncState.totalDownloaded * 100 / self->asyncState.contentLength);
        if (progress >= self->asyncState.lastReportedProgress + 20) {
            self->asyncState.lastReportedProgress = progress;
            unsigned long elapsed = millis() - self->asyncState.startTime;
            float mbps = elapsed > 0 ? (self->asyncState.totalDownloaded * 8.0f) / (elapsed / 1000.0f) / 1e6f : 0;
            Serial.printf("FileManager: progress %d%% (%d/%d bytes) %.2f Mbps, %d SD writes\n",
                          progress, self->asyncState.totalDownloaded, self->asyncState.contentLength,
                          mbps, self->asyncState.writeCount);
            if (self->downloadProgressCallback) {
                self->downloadProgressCallback(self->asyncState.url, self->asyncState.localPath, progress,
                                               self->asyncState.totalDownloaded, self->asyncState.contentLength);
            }
        }
    }
}

void FileManager::onAsyncDisconnect(void* arg, AsyncClient* client) {
    FileManager* self = static_cast<FileManager*>(arg);
    // The server closed the connection ("Connection: close"): the data path is complete.
    // Whether it is a *successful* download (size, checksum) is decided by the owner.
    self->requestAsyncFinish(true, "");
}

void FileManager::onAsyncError(void* arg, AsyncClient* client, int8_t error) {
    FileManager* self = static_cast<FileManager*>(arg);
    char msg[48];
    snprintf(msg, sizeof(msg), "AsyncTCP error %d", (int)error);
    self->requestAsyncFinish(false, msg);
}

void FileManager::onAsyncTimeout(void* arg, AsyncClient* client, uint32_t time) {
    FileManager* self = static_cast<FileManager*>(arg);
    self->requestAsyncFinish(false, "Network timeout");
}

// Hand the full network buffer to the SD writer (called from onAsyncData). Waits for the
// previous write to finish: a few hundred ms is a normal SD card busy period, so a short
// timeout here used to abort perfectly good downloads. TCP flow control holds the sender.
// Producer side (async_tcp task): copy into the PSRAM ring. Only blocks when the ring is
// completely full, i.e. the card has been paused for RING_SIZE worth of network data.
bool FileManager::ringPush(const uint8_t* data, size_t len) {
    unsigned long waitedMs = 0;
    while (len > 0) {
        size_t space = ringFree();
        if (space == 0) {
            if (waitedMs >= SD_WRITER_STALL_MS) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            waitedMs += 20;
            continue;
        }
        size_t n = len < space ? len : space;
        size_t headIdx = asyncState.ringHead % RING_SIZE;
        size_t first = (RING_SIZE - headIdx) < n ? (RING_SIZE - headIdx) : n;
        memcpy(ringBuffer + headIdx, data, first);
        if (n > first) {
            memcpy(ringBuffer, data + first, n - first);
        }
        asyncState.ringHead += n;
        asyncState.totalDownloaded += n;
        data += n;
        len -= n;
        if (asyncState.dataAvailable) {
            xSemaphoreGive(asyncState.dataAvailable);
        }
    }
    return true;
}

// SD writer task (core 0): drains the PSRAM ring in DOWNLOAD_BUFFER_SIZE chunks through the
// internal bounce buffer, hashing as it goes. Exits only when told to AND signals it, so the
// owner can delete the semaphore safely.
void FileManager::sdWriteTask(void* parameter) {
    FileManager* self = static_cast<FileManager*>(parameter);
    for (;;) {
        size_t avail = self->ringAvailable();
        const bool flushTail = self->asyncState.producerDone && avail > 0;
        if (self->asyncState.discardRing) {
            break;
        }
        if (avail >= DOWNLOAD_BUFFER_SIZE || flushTail) {
            size_t n = avail < DOWNLOAD_BUFFER_SIZE ? avail : DOWNLOAD_BUFFER_SIZE;
            size_t tailIdx = self->asyncState.ringTail % RING_SIZE;
            size_t first = (RING_SIZE - tailIdx) < n ? (RING_SIZE - tailIdx) : n;
            memcpy(self->persistentBufferA, self->ringBuffer + tailIdx, first);
            if (n > first) {
                memcpy(self->persistentBufferA + first, self->ringBuffer, n - first);
            }
            unsigned long writeStart = micros();
            size_t written = self->asyncState.file.write(self->persistentBufferA, n);
            unsigned long writeTime = micros() - writeStart;
            if (self->asyncState.shaActive) {
                mbedtls_sha256_update(&self->asyncState.sha, self->persistentBufferA, written);
            }
            self->asyncState.totalWriteTime += (writeTime / 1000);
            self->asyncState.writeCount++;
            if (writeTime > 300000) {
                Serial.printf("FileManager: slow SD write #%d: %lu ms for %u bytes (ring backlog %u KB)\n",
                              self->asyncState.writeCount, writeTime / 1000, (unsigned)n,
                              (unsigned)(self->ringAvailable() / 1024));
            }
            if (written != n) {
                Serial.printf("FileManager: SD write #%d short: %u/%u bytes\n",
                              self->asyncState.writeCount, (unsigned)written, (unsigned)n);
                self->requestAsyncFinish(false, "SD write failed");
                self->asyncState.discardRing = true;
                break;
            }
            self->asyncState.ringTail += n;
            continue;
        }
        if (!self->asyncState.writeTaskRunning && avail == 0) {
            break;
        }
        if (!self->asyncState.writeTaskRunning && !self->asyncState.producerDone) {
            break; // stopped without a drain request
        }
        xSemaphoreTake(self->asyncState.dataAvailable, pdMS_TO_TICKS(50));
    }
    self->asyncState.writerExited = true;
    vTaskDelete(NULL);
}

// Runs on the loop task once the callbacks have requested completion.
void FileManager::finalizeAsyncDownload() {
    bool success = asyncState.finishSuccess;
    String errorMsg = asyncState.finishError;
    bool hardFailure = false;

    // 1. Network side: detach callbacks, close, delete (we are not inside a callback here).
    if (asyncState.client) {
        asyncState.client->onConnect(nullptr, nullptr);
        asyncState.client->onData(nullptr, nullptr);
        asyncState.client->onDisconnect(nullptr, nullptr);
        asyncState.client->onError(nullptr, nullptr);
        asyncState.client->onTimeout(nullptr, nullptr);
        asyncState.client->close(true);
        delay(20); // let the async_tcp task drain any event already queued for this client
        delete asyncState.client;
        asyncState.client = nullptr;
    }

    // 2. Let the writer drain the ring (success) or drop it (failure). The drain is NOT
    //    waited for here: this runs on the loop task and a 4 MB backlog on a slow card takes
    //    10+ s. We return and come back on later update() calls until the ring is empty.
    if (asyncState.writeTaskHandle) {
        if (success) {
            if (!asyncState.producerDone) {
                asyncState.producerDone = true;
                asyncState.drainStartedMs = millis();
                if (asyncState.dataAvailable) {
                    xSemaphoreGive(asyncState.dataAvailable);
                }
            }
            if (ringAvailable() > 0 && !asyncState.discardRing) {
                if (millis() - asyncState.drainStartedMs < 180000UL) {
                    return; // still draining; try again next loop
                }
                success = false;
                errorMsg = "SD writer did not drain the ring";
                asyncState.discardRing = true;
            }
        } else {
            asyncState.discardRing = true;
        }
        asyncState.writeTaskRunning = false;
        if (asyncState.dataAvailable) {
            xSemaphoreGive(asyncState.dataAvailable);
        }
        for (int i = 0; i < 500 && !asyncState.writerExited; ++i) {
            delay(10);
        }
        if (!asyncState.writerExited) {
            Serial.println("FileManager: SD writer did not exit; leaking its semaphore rather than crashing");
            asyncState.dataAvailable = nullptr;
        }
        asyncState.writeTaskHandle = nullptr;
    }
    if (asyncState.dataAvailable) {
        vSemaphoreDelete(asyncState.dataAvailable);
        asyncState.dataAvailable = nullptr;
    }
    // A short SD write inside the writer requests a failed finish after we started finalizing.
    if (success && asyncState.discardRing) {
        success = false;
        errorMsg = "SD write failed";
    }

    // 4. File: sync and close; verify size and checksum before it becomes visible.
    size_t bytesWritten = 0;
    if (asyncState.file) {
        asyncState.file.flush();
        bytesWritten = asyncState.file.position();
        asyncState.file.close();
    }
    String actualSha;
    if (asyncState.shaActive) {
        uint8_t digest[32];
        mbedtls_sha256_finish(&asyncState.sha, digest);
        mbedtls_sha256_free(&asyncState.sha);
        asyncState.shaActive = false;
        char hex[65];
        for (size_t i = 0; i < sizeof(digest); ++i) {
            snprintf(&hex[i * 2], 3, "%02x", digest[i]);
        }
        hex[64] = '\0';
        actualSha = hex;
    }
    if (success) {
        if (bytesWritten == 0) {
            success = false;
            errorMsg = "No data received";
        } else if (asyncState.contentLength > 0 && bytesWritten != static_cast<size_t>(asyncState.contentLength)) {
            success = false;
            hardFailure = true;
            errorMsg = "Size mismatch: wrote " + String(bytesWritten) + " expected " + String(asyncState.contentLength);
        } else if (!asyncState.checksum.isEmpty() && !actualSha.equalsIgnoreCase(asyncState.checksum)) {
            success = false;
            hardFailure = true;
            errorMsg = "Checksum mismatch";
        }
    }
    if (success) {
        if (SD_MMC.exists(asyncState.localPath)) {
            SD_MMC.remove(asyncState.localPath);
        }
        if (!SD_MMC.rename(asyncState.tempPath, asyncState.localPath)) {
            success = false;
            errorMsg = "Failed to rename temp file";
        }
    }
    if (!success && SD_MMC.exists(asyncState.tempPath)) {
        SD_MMC.remove(asyncState.tempPath);
    }

    // 5. Stats, queue and callbacks (owner task). Failure callbacks only when terminal.
    downloadStats.totalDownloads++;
    auto it = std::find_if(downloadQueue.begin(), downloadQueue.end(),
        [this](const DownloadTask& task) { return task.localPath == asyncState.localPath; });

    if (success) {
        downloadStats.successfulDownloads++;
        downloadStats.totalBytesDownloaded += bytesWritten;
        unsigned long totalTime = millis() - asyncState.startTime;
        float mbps = totalTime > 0 ? (bytesWritten * 8.0f) / (totalTime / 1000.0f) / 1e6f : 0;
        Serial.printf("FileManager: Download completed: %s (%u bytes, %.2f Mbps, %d SD writes, %lu ms in SD writes)%s\n",
                      asyncState.localPath.c_str(), (unsigned)bytesWritten, mbps, asyncState.writeCount,
                      asyncState.totalWriteTime, asyncState.checksum.isEmpty() ? "" : ", sha256 verified");
        if (it != downloadQueue.end()) {
            downloadQueue.erase(it);
            saveDownloadQueue();
        }
        if (cachedFreeBytes > bytesWritten) {
            cachedFreeBytes -= bytesWritten;
        }
        const String url = asyncState.url, path = asyncState.localPath;
        downloadInProgress = false;
        asyncState.reset();
        saveDownloadStats();
        if (downloadCompleteCallback) {
            downloadCompleteCallback(url, path, true, "");
        }
        return;
    }

    downloadStats.failedDownloads++;
    bool terminal = asyncState.permanentFailure;
    if (it != downloadQueue.end()) {
        if (hardFailure) {
            it->hardFailures++;
            if (it->hardFailures >= MAX_HARD_FAILURES) {
                terminal = true;
            }
        }
        if (terminal) {
            downloadQueue.erase(it);
        }
        saveDownloadQueue();
    } else {
        terminal = true; // ad-hoc download (not queued): nothing will retry it
    }
    Serial.printf("FileManager: Download failed: %s (%s)%s\n", asyncState.localPath.c_str(), errorMsg.c_str(),
                  terminal ? " - giving up" : " - will retry");
    const String url = asyncState.url, path = asyncState.localPath;
    downloadInProgress = false;
    asyncState.reset();
    saveDownloadStats();
    if (terminal && downloadCompleteCallback) {
        downloadCompleteCallback(url, path, false, errorMsg);
    }
}
