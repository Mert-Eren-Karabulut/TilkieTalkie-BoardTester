#include "FileManager.h"
#include "Faults.h"
#include "PsramJson.h"
#include "RequestManager.h"
#include <SD_MMC.h>
#include <AsyncTCP.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_random.h>
#include <functional>
#include <mbedtls/sha256.h>
#include <algorithm>

static const char *TAG = "files";

// Fault details are short (the portal stores ~40 chars): keep the tail of a path, which
// carries the file name.
static String faultDetail(const String &path)
{
    return path.length() <= 39 ? path : path.substring(path.length() - 39);
}

// Initialize static members
FileManager* FileManager::instance = nullptr;
const char* FileManager::SD_DATA_DIR = "/.filemanager";
const char* FileManager::SD_DOWNLOAD_QUEUE_FILE = "/.filemanager/download_queue.json";
const char* FileManager::SD_REQUIRED_FILES_FILE = "/.filemanager/required_files.json";
const char* FileManager::SD_DOWNLOAD_STATS_FILE = "/.filemanager/download_stats.json";

FileManager::FileManager() : 
    sdCardInitialized(false),
    downloadInProgress(false),
    persistentBufferA(nullptr),
    ringBuffer(nullptr),
    buffersAllocated(false),
    downloadCompleteCallback(nullptr) {
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
    ESP_LOGI(TAG, "Initializing...");
    
    // One 32 KB internal DMA bounce buffer (SDMMC cannot DMA from PSRAM) and a 4 MB PSRAM
    // ring between the network and the card. Allocated once, never freed.
    if (!buffersAllocated) {
        persistentBufferA = (uint8_t*)heap_caps_malloc(DOWNLOAD_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        ringBuffer = (uint8_t*)heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!persistentBufferA || !ringBuffer) {
            ESP_LOGE(TAG, "CRITICAL: failed to allocate download buffers (32 KB internal + 4 MB PSRAM)");
            if (persistentBufferA) { free(persistentBufferA); persistentBufferA = nullptr; }
            if (ringBuffer) { free(ringBuffer); ringBuffer = nullptr; }
            buffersAllocated = false;
        } else {
            buffersAllocated = true;
            ESP_LOGI(TAG, "download buffers ready (%u KB internal bounce, %u KB PSRAM ring)",
                          (unsigned)(DOWNLOAD_BUFFER_SIZE / 1024), (unsigned)(RING_SIZE / 1024));
        }
    }
    
    // Initialize SD card first (needed for persistence)
    if (!initializeSDCard()) {
        ESP_LOGW(TAG, "Failed to initialize SD card");
        return false;
    }
    
    // Create data directory for persistence files
    createDirectory(SD_DATA_DIR);

    // A card moved from another toy must not resume that toy's queue or play its cache.
    checkDeviceMarker();
    if (SD_MMC.exists("/.trash")) {
        enqueueJob(5, "/.trash", nullptr, 0); // a wipe interrupted by a reboot (or just done)
    }
    
    // Load persistent data from SD card
    loadDownloadStats();
    loadDownloadQueue();
    loadRequiredFiles();
    // Orphaned partial downloads are swept before the first download starts (the sweep's flag
    // holds downloads off); the queue is loaded first so its partial files stay for Range resume.
    cleanupTempFiles();
    
    ESP_LOGI(TAG, "Initialization complete");
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
    
    ESP_LOGI(TAG, "Shutdown complete (persistent buffers remain allocated)");
}

void FileManager::benchmarkSDCard() {
    ESP_LOGI(TAG, "=== SD Card Write Speed Benchmark ===");
    
    const size_t testSizes[] = {4096, 16384, 65536, 262144}; // 4KB, 16KB, 64KB, 256KB
    const int iterations = 10;
    
    // Test with Internal SRAM (DMA-aligned) - Sequential writes (simulating download)
    ESP_LOGI(TAG, "--- Internal SRAM (DMA-aligned) - Download Simulation ---");
    for (size_t testSize : testSizes) {
        uint8_t* testBuf = (uint8_t*)heap_caps_malloc(testSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!testBuf) {
            ESP_LOGW(TAG, "Failed to allocate %d byte buffer from internal SRAM", testSize);
            continue;
        }
        
        for (size_t i = 0; i < testSize; i++) {
            testBuf[i] = random(256);
        }
        
        // Open file ONCE and write multiple times (like download does)
        File testFile = SD_MMC.open("/sd_benchmark_download.tmp", FILE_WRITE);
        if (!testFile) {
            ESP_LOGW(TAG, "Failed to create test file");
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
                ESP_LOGW(TAG, "Write failed: %d/%d bytes", written, testSize);
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
        
        ESP_LOGI(TAG, "%6d bytes: avg=%6.2f ms (min=%5.2f, max=%6.2f), %7.1f KB/s, %5.2f Mbps", 
                     testSize, avgTimeMs, minTimeMs, maxTimeMs, speedKBps, speedMbps);
    }
    
    // Test with PSRAM - Sequential writes
    ESP_LOGI(TAG, "--- PSRAM - Download Simulation ---");
    for (size_t testSize : testSizes) {
        uint8_t* testBuf = (uint8_t*)ps_malloc(testSize);
        if (!testBuf) {
            ESP_LOGW(TAG, "Failed to allocate %d byte buffer from PSRAM", testSize);
            continue;
        }
        
        for (size_t i = 0; i < testSize; i++) {
            testBuf[i] = random(256);
        }
        
        File testFile = SD_MMC.open("/sd_benchmark_psram_download.tmp", FILE_WRITE);
        if (!testFile) {
            ESP_LOGW(TAG, "Failed to create test file");
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
                ESP_LOGW(TAG, "Write failed: %d/%d bytes", written, testSize);
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
        
        ESP_LOGI(TAG, "%6d bytes: avg=%6.2f ms (min=%5.2f, max=%6.2f), %7.1f KB/s, %5.2f Mbps", 
                     testSize, avgTimeMs, minTimeMs, maxTimeMs, speedKBps, speedMbps);
    }
    
    ESP_LOGI(TAG, "=== Benchmark Complete ===");
    ESP_LOGI(TAG, "Summary: This simulates download behavior (open once, write many times).");
    ESP_LOGI(TAG, "Watch for increasing write times - this indicates FATFS overhead.");
}

bool FileManager::initializeSDCard() {
    ESP_LOGI(TAG, "Initializing SD card...");
    
    ESP_LOGI(TAG, "Using SDMMC pins - CLK:%d, CMD:%d, D0:%d, D1:%d, D2:%d, D3:%d", 
                  SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    
    // A failed SDMMC begin can leave the peripheral in a bad state on this board,
    // especially while WiFi/BLE provisioning is active. Try the normal 4-bit path first,
    // then fall back to 1-bit/default-speed to distinguish marginal data lines from a dead card.
    ESP_LOGI(TAG, "SD card initialization attempt 1/2 (4-bit)");
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    bool sdInitialized = SD_MMC.begin("/sd", false, false, BOARD_MAX_SDMMC_FREQ);
    
    if (!sdInitialized) {
        SD_MMC.end();
        ESP_LOGW(TAG, "4-bit SDMMC init failed, retrying in 1-bit mode at default speed");
        ESP_LOGI(TAG, "SD card initialization attempt 2/2 (1-bit fallback)");
        sdInitialized = SD_MMC.begin("/sd", true, false, SDMMC_FREQ_DEFAULT);
    }

    if (!sdInitialized) {
        SD_MMC.end();
        ESP_LOGW(TAG, "SD card initialization failed in both 4-bit and 1-bit modes");
        ESP_LOGI(TAG, "Please check:");
        ESP_LOGI(TAG, "  1. SD card is properly inserted");
        ESP_LOGI(TAG, "  2. Wiring connections are correct");
        ESP_LOGI(TAG, "  3. SD card is formatted as FAT32");
        ESP_LOGI(TAG, "  4. CMD/D0 pull-ups are present and stable");
        ESP_LOGI(TAG, "  5. Power supply is adequate");
        return false;
    }
    
    // Check SD card type
    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        ESP_LOGI(TAG, "No SD card attached");
        return false;
    }
    
    const char *typeName = "Unknown";
    switch (cardType) {
        case CARD_MMC: typeName = "MMC"; break;
        case CARD_SD: typeName = "SDSC"; break;
        case CARD_SDHC: typeName = "SDHC"; break;
        default: break;
    }
    ESP_LOGI(TAG, "SD card type: %s", typeName);
    
    // Print SD card size
    uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
    ESP_LOGI(TAG, "SD card size: %lluMB", cardSize);
    
    sdCardInitialized = true;
    createDirectory("/assets"); // story assets; sub-directories are created per download
    ensureMaintenanceTask(); // background deletes/writes + the first free-space count
    
    return true;
}

bool FileManager::checkConnectivity() {
    // The TCP connect to the asset host is the real connectivity test; a probe to a third
    // party used to block the loop for up to ~15 s per queued file whenever WAN was down.
    // Behind a captive portal a download would fetch a login page: offline until the probe passes.
    return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0) && RequestManager::portalReachable();
}

void FileManager::update() {
    if (!sdCardInitialized) {
        return;
    }

    // Completion of an in-flight download is handled here, on the loop task (owner of the
    // queue, the files and the callbacks), never inside the AsyncTCP callbacks.
    if (downloadInProgress && asyncState.finishState.load(std::memory_order_acquire) == FINISH_READY) {
        finalizeAsyncDownload();
    }

    // Process download queue if conditions are met
    if (!downloadInProgress && downloadQueue.size() > 0) {
            processDownloadQueue();
    }

    // Batched queue persistence (retry bookkeeping): at most every QUEUE_SAVE_INTERVAL_MS,
    // at once when the queue has drained.
    if (queueDirty && (downloadQueue.empty() || millis() - lastQueueSave >= QUEUE_SAVE_INTERVAL_MS)) {
        saveDownloadQueue();
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
    
    if (success) {
        ESP_LOGI(TAG, "File deleted successfully: %s", path.c_str());
    } else {
        ESP_LOGW(TAG, "Failed to delete file: %s", path.c_str());
    }
    
    return success;
}

bool FileManager::ensureMaintenanceTask() {
    if (!pendingLock) {
        pendingLock = xSemaphoreCreateMutex();
    }
    if (!deleteQueue) {
        deleteQueue = xQueueCreate(32, sizeof(MaintJob*));
        if (!deleteQueue) {
            return false;
        }
    }
    if (!maintenanceTaskHandle) {
        if (xTaskCreatePinnedToCore(sdMaintenanceTask, "sd_maint", 8192, this, 2, &maintenanceTaskHandle, 0) != pdPASS) {
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
    const bool work = (kind == 0 || kind == 1);
    pendingDeletes++;
    if (work) pendingWork++;
    if (xQueueSend(deleteQueue, &job, 0) != pdTRUE) {
        pendingDeletes--;
        if (work) pendingWork--;
        free(job->path); free(job->data); free(job);
        return false;
    }
    if (kind == 0 && pendingLock) {
        xSemaphoreTake(pendingLock, portMAX_DELAY);
        pendingDeletePaths.push_back(path);
        xSemaphoreGive(pendingLock);
    }
    return true;
}

bool FileManager::deleteFileDeferred(const String& path) {
    if (!sdCardInitialized || path.isEmpty()) {
        return false;
    }
    if (!enqueueJob(0, path, nullptr, 0)) {
        ESP_LOGW(TAG, "delete queue full, deleting inline: %s", path.c_str());
        return deleteFile(path);
    }
    return true;
}

bool FileManager::writeFileDeferred(const String& path, const String& content) {
    if (!sdCardInitialized || path.isEmpty()) {
        return false;
    }
    if (!enqueueJob(1, path, content.c_str(), content.length())) {
        ESP_LOGW(TAG, "write queue full, writing inline: %s", path.c_str());
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
        ESP_LOGW(TAG, "Failed to open %s for writing", tmp.c_str());
        return false;
    }
    size_t written = len ? file.write(reinterpret_cast<const uint8_t*>(data), len) : 0;
    file.flush();
    file.close();
    if (written != len) {
        SD_MMC.remove(tmp);
        ESP_LOGW(TAG, "short write %u/%u to %s", (unsigned)written, (unsigned)len, path);
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
                ESP_LOGI(TAG, "%s %s (%lu ms, background)", ok ? "deleted" : "failed to delete", job->path, millis() - t0);
                if (self->pendingLock) {
                    xSemaphoreTake(self->pendingLock, portMAX_DELAY);
                    for (auto it = self->pendingDeletePaths.begin(); it != self->pendingDeletePaths.end(); ++it) {
                        if (*it == job->path) { self->pendingDeletePaths.erase(it); break; }
                    }
                    xSemaphoreGive(self->pendingLock);
                }
            } else if (job->kind == 3) {
                RequestManager::getInstance().buildInventory(); // stories on the card -> ReverbClient outbox
                self->inventoryQueued = false;
            } else if (job->kind == 4) {
                self->sweepOrphanedTempFiles();
            } else if (job->kind == 5) {
                self->sweepTrash();
            } else if (job->kind == 1) {
                bool ok = self->writeFileNow(job->path, job->data, job->len);
                unsigned long dt = millis() - t0;
                if (!ok || dt > 500) {
                    ESP_LOGI(TAG, "%s %s (%u bytes, %lu ms, background)", ok ? "wrote" : "failed to write", job->path, (unsigned)job->len, dt);
                }
            } else {
                self->refreshFreeSpaceCache();
                self->freeRefreshInFlight = false;
            }
        }
        if (job->kind == 4) {
            self->tmpSweepPending = false; // downloads may start again
        }
        const bool work = (job->kind == 0 || job->kind == 1);
        free(job->path);
        free(job->data);
        free(job);
        self->pendingDeletes--;
        if (work) self->pendingWork--;
    }
}

bool FileManager::deleteFileAndRemoveFromRequired(const String& path) {
    if (!sdCardInitialized) {
        ESP_LOGW(TAG, "SD card not initialized");
        return false;
    }
    
    // Validate that this is a file path, not a directory
    if (path.endsWith("/") || path.isEmpty()) {
        ESP_LOGW(TAG, "Invalid file path (directories not allowed): %s", path.c_str());
        return false;
    }
    
    // Check if path points to a directory
    File fileOrDir = SD_MMC.open(path);
    if (fileOrDir) {
        if (fileOrDir.isDirectory()) {
            fileOrDir.close();
            ESP_LOGW(TAG, "Cannot delete directory with deleteFileAndRemoveFromRequired: %s", path.c_str());
            ESP_LOGI(TAG, "Use removeDirectory() for directories");
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
        ESP_LOGI(TAG, "File is marked as required, removing from required list: %s", path.c_str());
        requiredFiles.erase(it);
        saveRequiredFiles();
    }
    
    // Remove from download queue if it's currently queued
    bool wasInQueue = false;
    auto queueIt = downloadQueue.begin();
    while (queueIt != downloadQueue.end()) {
        if (queueIt->localPath == path) {
            ESP_LOGI(TAG, "Removing from download queue: %s", path.c_str());
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
    
    if (success) {
        ESP_LOGI(TAG, "File deleted successfully: %s", path.c_str());
        if (wasRequired) {
            ESP_LOGI(TAG, "File removed from required list to prevent re-download");
        }
        if (wasInQueue) {
            ESP_LOGI(TAG, "File removed from download queue");
        }
    } else {
        ESP_LOGW(TAG, "Failed to delete file: %s", path.c_str());
        
        // If deletion failed but we removed it from required list, add it back
        if (wasRequired) {
            // We need to reconstruct the FileEntry, but we don't have the URL
            // Log a warning that manual re-addition might be needed
            ESP_LOGW(TAG, "WARNING - File was removed from required list but deletion failed");
            ESP_LOGI(TAG, "You may need to manually re-add it with addfile command if needed");
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
    
    return success;
}

bool FileManager::fileExists(const String& path) {
    if (!sdCardInitialized) {
        return false;
    }

    return SD_MMC.exists(path);
}

bool FileManager::isPresent(const String& path) {
    return fileExists(path) && !isDeletePending(path);
}

std::vector<String> FileManager::listFiles(const String& directory) {
    std::vector<String> files;
    
    if (!sdCardInitialized) {
        return files;
    }
    
    File dir = SD_MMC.open(directory);
    if (!dir || !dir.isDirectory()) {
        ESP_LOGW(TAG, "Failed to open directory: %s", directory.c_str());
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

bool FileManager::scheduleDownload(const String& url, const String& localPath, const String& checksum, bool persist) {
    // Check if already in queue
    for (const auto& task : downloadQueue) {
        if (task.url == url && task.localPath == localPath) {
            ESP_LOGI(TAG, "Download already scheduled");
            return true;
        }
    }
    
    addToDownloadQueue(url, localPath, checksum);
    if (persist) {
        saveDownloadQueue();
    } else {
        queueDirty = true; // the caller flushes once for the whole batch
    }
    
    ESP_LOGI(TAG, "Download scheduled: %s -> %s", url.c_str(), localPath.c_str());
    return true;
}

void FileManager::flushDownloadQueue() {
    if (queueDirty) {
        saveDownloadQueue();
    }
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

bool FileManager::downloadFileFromURL(const String& url, const String& localPath, const String& checksum, String& errorMsg,
                                      bool& pathError) {
    pathError = false;
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
    ESP_LOGI(TAG, "Starting async download: %s -> %s", httpUrl.c_str(), localPath.c_str());

    if (!createDirectoryStructure(localPath)) {
        errorMsg = "Failed to create directory structure";
        pathError = true;
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
    if (cachedFreeAt.load() == 0 || millis() - cachedFreeAt.load() > 60000) {
        refreshFreeSpaceCache();
    }
    asyncState.freeBytesAtStart = cachedFreeBytes.load();

    if (!buffersAllocated || !persistentBufferA || !ringBuffer) {
        errorMsg = "Download buffer not available";
        downloadInProgress = false;
        return false;
    }

    // An interrupted download left its .tmp: continue it with a Range request instead of
    // starting over (a 3 MB track on a 0.3 MB/s card is 10 s of writes). Only with a checksum:
    // the whole file is verified at the end, prefix included (the SD writer hashes it first).
    size_t existing = 0;
    if (!checksum.isEmpty() && SD_MMC.exists(asyncState.tempPath)) {
        File partial = SD_MMC.open(asyncState.tempPath, FILE_READ);
        if (partial) {
            existing = partial.isDirectory() ? 0 : partial.size();
            partial.close();
        }
    }
    if (existing > 0) {
        asyncState.resumeFrom = existing;
        ESP_LOGI(TAG, "resuming %s at %u bytes", localPath.c_str(), (unsigned)existing);
        // the SD writer opens the file (append, or truncate on a 200) once the answer is known
    } else {
        if (SD_MMC.exists(asyncState.tempPath)) {
            SD_MMC.remove(asyncState.tempPath);
        }
        asyncState.file = SD_MMC.open(asyncState.tempPath, FILE_WRITE);
        if (!asyncState.file) {
            errorMsg = "Failed to create temporary file";
            pathError = true;
            downloadInProgress = false;
            return false;
        }
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

    ESP_LOGI(TAG, "Connecting to %s:%d", hostname.c_str(), port);
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
    if (asyncState.resumeFrom == 0 && SD_MMC.exists(asyncState.tempPath)) {
        SD_MMC.remove(asyncState.tempPath); // a resumable partial file stays for the next attempt
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
    if (tmpSweepPending.load()) {
        return; // the boot sweep of orphaned *.tmp must not race a new temp file
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

        // A delete of the same path still queued on the maintenance task would erase the new
        // file after it lands: wait for it.
        if (isDeletePending(task.localPath)) {
            continue;
        }

        // Already present: integrity was verified when it was downloaded; trust it.
        if (fileExists(task.localPath)) {
            ESP_LOGI(TAG, "File already exists: %s", task.localPath.c_str());
            downloadQueue.erase(it);
            saveDownloadQueue();
            return;
        }

        if (task.checksum.isEmpty() && task.localPath.startsWith("/assets/")) {
            // A story asset without a checksum (queued by an older firmware) is never fetched
            // unverified over plain HTTP; the next manifest with a checksum queues it again.
            ESP_LOGW(TAG, "dropping %s: no checksum", task.localPath.c_str());
            giveUp(it, "nosha");
            return;
        }

        if (task.retryCount >= MAX_RETRY_COUNT) {
            // Gave up: the next tap of the figure re-queues whatever its manifest still needs.
            ESP_LOGW(TAG, "giving up on %s after %d attempts", task.url.c_str(), task.retryCount);
            giveUp(it, "Gave up after repeated attempts");
            return;
        }

        task.lastAttempt = millis();
        task.retryCount++;
        ESP_LOGI(TAG, "Attempting download (attempt %d): %s", task.retryCount, task.url.c_str());

        String errorMsg;
        bool pathError = false;
        const String url = task.url, localPath = task.localPath, checksum = task.checksum;
        if (!downloadFileFromURL(url, localPath, checksum, errorMsg, pathError)) {
            ESP_LOGW(TAG, "Download failed to start: %s (%s)", url.c_str(), errorMsg.c_str());
            // Only a local path that cannot be created is terminal (a directory at once; a temp
            // file after MAX_HARD_FAILURES, since a busy card can refuse one open). A semaphore,
            // task, client or connection that could not be created is transient: retried.
            if (pathError) {
                Faults::raise(Fault::SD_WRITE_FAILED, faultDetail(localPath).c_str(), false);
                if (errorMsg.indexOf("directory") >= 0 || ++task.hardFailures >= MAX_HARD_FAILURES) {
                    ESP_LOGW(TAG, "dropping the entry (local path error)");
                    giveUp(it, errorMsg.c_str());
                    return;
                }
            }
        }
        queueDirty = true; // retry bookkeeping: flushed by update()
        return; // one download at a time
    }
}

// Terminal failure of a queued download: drop it, persist at once, report it and complete the
// trackers waiting for it (a figure tracker or the playlist would otherwise wait forever).
void FileManager::giveUp(std::vector<DownloadTask>::iterator it, const char *reason) {
    const String url = it->url, path = it->localPath;
    const String error = reason ? reason : "";
    downloadQueue.erase(it);
    saveDownloadQueue();
    if (SD_MMC.exists(path + ".tmp")) {
        deleteFileDeferred(path + ".tmp"); // nothing will resume it
    }
    Faults::raise(Fault::DOWNLOAD_FAILED, faultDetail(path).c_str(), false);
    if (downloadCompleteCallback) {
        downloadCompleteCallback(url, path, false, error); // last: it may change the queue
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
                ESP_LOGI(TAG, "Updated required file metadata: %s", localPath.c_str());
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
    ESP_LOGI(TAG, "Removed required file: %s", localPath.c_str());
    return true;
}

void FileManager::checkRequiredFiles() {
    // Existence only. Files are hashed once, while they are downloaded; re-hashing the whole
    // library every 5 minutes on the loop task froze the device for seconds to minutes.
    for (const auto& file : requiredFiles) {
        if (!fileExists(file.path)) {
            ESP_LOGI(TAG, "Required file missing, scheduling download: %s", file.path.c_str());
            scheduleDownload(file.url, file.path, file.checksum, false);
        }
    }
    flushDownloadQueue(); // one write for the whole pass
}

std::vector<String> FileManager::getRequiredFilesByPattern(const String& pattern) {
    std::vector<String> matchingFiles;
    
    for (const auto& file : requiredFiles) {
        if (file.path.indexOf(pattern) >= 0) {
            matchingFiles.push_back(file.path);
        }
    }
    
    ESP_LOGI(TAG, "Found %d required files matching pattern: %s", 
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
        ESP_LOGI(TAG, "Created directory: %s", path.c_str());
        return true;
    }
    
    // Check if directory already exists
    File dirFile = SD_MMC.open(path);
    bool exists = (dirFile && dirFile.isDirectory());
    if (dirFile) dirFile.close();
    
    if (!exists) {
        ESP_LOGW(TAG, "Failed to create directory: %s", path.c_str());
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
    const unsigned long t1 = millis();
    cachedFreeAt = t1;
    if (t1 - t0 > 100) {
        ESP_LOGI(TAG, "free-space query took %lu ms", t1 - t0);
    }
}

size_t FileManager::getSDCardFreeSpace() {
    if (!sdCardInitialized) {
        return 0;
    }
    // Never query the FAT while a download is writing (lock contention); otherwise refresh
    // at most once a minute, on the maintenance task. Callers only use this for the status
    // report and a sanity check.
    if (!downloadInProgress && (cachedFreeAt.load() == 0 || millis() - cachedFreeAt.load() > 60000)) {
        requestFreeSpaceRefresh();
    }
    uint64_t freeBytes = cachedFreeBytes.load();
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

bool FileManager::isDeletePending(const String& path) {
    if (!pendingLock) {
        return false;
    }
    bool pending = false;
    xSemaphoreTake(pendingLock, portMAX_DELAY);
    for (const String& p : pendingDeletePaths) {
        if (p == path) { pending = true; break; }
    }
    xSemaphoreGive(pendingLock);
    return pending;
}

void FileManager::enqueueInventory() {
    if (inventoryQueued) {
        return;
    }
    if (enqueueJob(3, "/", nullptr, 0)) {
        inventoryQueued = true;
    }
}

uint32_t FileManager::getSDCardFreeMB() {
    if (!sdCardInitialized) {
        return 0;
    }
    if (!downloadInProgress && (cachedFreeAt.load() == 0 || millis() - cachedFreeAt.load() > 60000)) {
        requestFreeSpaceRefresh();
    }
    uint64_t freeBytes = cachedFreeBytes.load();
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
    ESP_LOGI(TAG, "Download queue (%d items):", downloadQueue.size());
    
    for (size_t i = 0; i < downloadQueue.size(); i++) {
        const auto& task = downloadQueue[i];
        ESP_LOGI(TAG, "  %d. %s -> %s", i + 1, task.url.c_str(), task.localPath.c_str());
        ESP_LOGI(TAG, "      Attempts: %d/%d", task.retryCount, MAX_RETRY_COUNT);
        
        if (task.lastAttempt > 0) {
            unsigned long timeSince = millis() - task.lastAttempt;
            if (timeSince < RETRY_DELAY_MS) {
                ESP_LOGI(TAG, "      Next retry in %lu seconds", (RETRY_DELAY_MS - timeSince) / 1000);
            }
        }
    }
}

void FileManager::printRequiredFiles() {
    ESP_LOGI(TAG, "Required files (%d items):", requiredFiles.size());
    
    for (size_t i = 0; i < requiredFiles.size(); i++) {
        const auto& file = requiredFiles[i];
        bool exists = fileExists(file.path);
        ESP_LOGI(TAG, "  %d. %s (exists: %s)", 
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
        obj["checksum"] = task.checksum;
    }
    // lastAttempt is a millis() of this boot: meaningless after a reboot, not persisted.
    queueDirty = false;
    lastQueueSave = millis();
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
        ESP_LOGW(TAG, "Failed to parse download queue: %s", error.c_str());
        quarantineCorrupt(SD_DOWNLOAD_QUEUE_FILE);
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
        task.lastAttempt = 0; // a previous boot's millis(): the backoff restarts with this boot
        task.checksum = obj["checksum"].as<String>();
        downloadQueue.push_back(task);
    }
    
    ESP_LOGI(TAG, "Loaded %d items from download queue", downloadQueue.size());
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
    ESP_LOGI(TAG, "Saved %d required files to SD card", requiredFiles.size());
    return true;
}

bool FileManager::loadRequiredFiles() {
    if (!sdCardInitialized) return false;
    
    File file;
    if (!openJsonForRead(SD_REQUIRED_FILES_FILE, file)) {
        ESP_LOGI(TAG, "No saved required files found");
        return true;
    }
    
    JsonDocument doc(&psramJsonAllocator);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        ESP_LOGW(TAG, "Failed to parse required files: %s", error.c_str());
        quarantineCorrupt(SD_REQUIRED_FILES_FILE);
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
    
    ESP_LOGI(TAG, "Loaded %d required files from SD card", requiredFiles.size());
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
    
    File file;
    if (!openJsonForRead(SD_DOWNLOAD_STATS_FILE, file)) {
        // No saved stats, start with defaults (already initialized in constructor)
        return true;
    }
    
    JsonDocument doc(&psramJsonAllocator);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        ESP_LOGW(TAG, "Failed to parse download stats: %s", error.c_str());
        return false;
    }
    
    downloadStats.totalDownloads = doc["totalDownloads"] | 0;
    downloadStats.successfulDownloads = doc["successfulDownloads"] | 0;
    downloadStats.failedDownloads = doc["failedDownloads"] | 0;
    downloadStats.totalBytesDownloaded = doc["totalBytesDownloaded"] | 0UL;
    
    return true;
}

// An unparseable list is kept as "<file>.bad" (the previous one is replaced) for a post-mortem,
// so the next save starts a fresh file instead of overwriting the evidence.
void FileManager::quarantineCorrupt(const char *path) {
    String src = path;
    if (!SD_MMC.exists(src)) {
        src += ".tmp"; // openJsonForRead fell back to the temp file
        if (!SD_MMC.exists(src)) {
            return;
        }
    }
    const String bad = String(path) + ".bad";
    if (SD_MMC.exists(bad)) {
        SD_MMC.remove(bad);
    }
    const bool moved = SD_MMC.rename(src, bad);
    ESP_LOGW(TAG, "%s unreadable, %s", src.c_str(), moved ? "kept as .bad" : "could not be moved");
    if (!moved) {
        SD_MMC.remove(src);
    }
    const char *name = strrchr(path, '/');
    Faults::raise(Fault::SD_FILE_CORRUPT, name ? name + 1 : path, false);
}

void FileManager::setDownloadCompleteCallback(DownloadCompleteCallback callback) {
    downloadCompleteCallback = callback;
}

// Additional utility methods
void FileManager::cancelAllDownloads() {
    downloadQueue.clear();
    saveDownloadQueue();
    ESP_LOGI(TAG, "All downloads cancelled");
}

int FileManager::getPendingDownloadsCount() {
    return downloadQueue.size();
}

int FileManager::getDownloadProgress() const {
    if (!downloadInProgress || asyncState.contentLength <= 0) {
        return downloadInProgress ? 0 : -1;
    }
    uint64_t pct = (uint64_t)asyncState.totalDownloaded * 100ULL / (uint64_t)asyncState.contentLength;
    return pct > 100 ? 100 : (int)pct;
}

String FileManager::requiredFileChecksum(const String &path) const {
    for (const auto &file : requiredFiles) {
        if (file.path == path) {
            return file.checksum;
        }
    }
    return String();
}

bool FileManager::cancelQueuedDownload(const String &path) {
    bool removed = false;
    for (auto it = downloadQueue.begin(); it != downloadQueue.end();) {
        if (it->localPath == path && !(downloadInProgress && asyncState.localPath == path)) {
            it = downloadQueue.erase(it);
            removed = true;
            if (SD_MMC.exists(path + ".tmp")) {
                deleteFileDeferred(path + ".tmp"); // a partial file nothing will resume
            }
        } else {
            ++it;
        }
    }
    if (removed) {
        saveDownloadQueue();
    }
    return removed;
}

void FileManager::cleanupTempFiles() {
    if (!sdCardInitialized || tmpSweepPending.load()) {
        return;
    }
    tmpSweepPending = true; // holds new downloads off until the sweep is done
    // Partial files of queued downloads (with a checksum) are resumed, not swept. Read by the
    // maintenance task only after it dequeues the job.
    tmpKeep.clear();
    for (const DownloadTask &t : downloadQueue) {
        if (!t.checksum.isEmpty()) {
            tmpKeep.push_back(t.localPath + ".tmp");
        }
    }
    if (!enqueueJob(4, "/assets", nullptr, 0)) {
        tmpSweepPending = false;
        ESP_LOGW(TAG, "temp-file sweep could not be queued");
    }
}

// Maintenance task. A "*.tmp" under /assets is a download cut by a reset or power loss (a
// finished download is renamed); none is being written while tmpSweepPending is set. Those of
// queued downloads are kept (tmpKeep): the next attempt resumes them with a Range request.
void FileManager::sweepOrphanedTempFiles() {
    int removed = 0;
    unsigned long t0 = millis();
    sweepTempDir("/assets", 0, removed);
    if (removed > 0) {
        ESP_LOGI(TAG, "removed %d orphaned temp file(s) (%lu ms)", removed, millis() - t0);
    }
}

void FileManager::sweepTempDir(const String &dir, int depth, int &removed) {
    if (depth > 6) {
        return;
    }
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return;
    }
    std::vector<String> subdirs;
    std::vector<String> temps;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        String full = dir;
        if (!full.endsWith("/")) full += "/";
        full += e.name();
        if (e.isDirectory()) {
            subdirs.push_back(full);
        } else if (full.endsWith(".tmp") && std::find(tmpKeep.begin(), tmpKeep.end(), full) == tmpKeep.end()) {
            temps.push_back(full);
        }
        e.close();
    }
    d.close();
    for (const String &t : temps) {
        if (SD_MMC.remove(t)) {
            removed++;
            ESP_LOGI(TAG, "Cleaned up temp file: %s", t.c_str());
        }
    }
    for (const String &sub : subdirs) {
        sweepTempDir(sub, depth + 1, removed);
    }
}

// ---- Card identity and cache wipe -----------------------------------------------------------

static const char *DEVICE_MARKER = "/.device";
static const char *TRASH_DIR = "/.trash";

void FileManager::checkDeviceMarker() {
    char uuid[24];
    snprintf(uuid, sizeof(uuid), "%llu", ESP.getEfuseMac());
    String previous;
    File f = SD_MMC.open(DEVICE_MARKER, FILE_READ);
    if (f) {
        previous = f.readString().substring(0, 32);
        f.close();
        previous.trim();
    }
    if (previous == uuid) {
        return;
    }
    if (previous.length() > 0) {
        // Its manifests would play cache-first and its queue would resume here; the portal
        // decides afresh what this toy gets when a figure is tapped.
        ESP_LOGW(TAG, "card belongs to toy %s - wiping its story cache", previous.c_str());
        wipeCardCache("card from another toy");
        cardWasForeign = true;
        char d[40];
        snprintf(d, sizeof(d), "prev=%.30s", previous.c_str());
        Faults::raise(Fault::CARD_FOREIGN, d);
    } else {
        // New card, or a card prepared before the marker existed: it is ours from now on.
        ESP_LOGI(TAG, "card marker written (%s)", uuid);
    }
    if (!writeFileNow(DEVICE_MARKER, uuid, strlen(uuid))) {
        ESP_LOGW(TAG, "cannot write %s", DEVICE_MARKER);
    }
}

bool FileManager::wipeCardCache(const char *why) {
    if (!sdCardInitialized) {
        return false;
    }
    ESP_LOGW(TAG, "wiping the card cache (%s)", why ? why : "?");
    // Queued manifest writes must land first, or they would recreate a manifest after the wipe.
    for (int i = 0; i < 100 && pendingWork.load() > 0; ++i) {
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
        delay(50);
    }
    downloadQueue.clear();
    requiredFiles.clear();
    queueDirty = false;
    const char *lists[] = {SD_DOWNLOAD_QUEUE_FILE, SD_REQUIRED_FILES_FILE};
    for (const char *path : lists) {
        if (SD_MMC.exists(path)) SD_MMC.remove(path);
        const String tmp = String(path) + ".tmp";
        if (SD_MMC.exists(tmp)) SD_MMC.remove(tmp);
    }
    createDirectory(TRASH_DIR);
    const char *dirs[] = {"/assets", "/.requestmanager"};
    int moved = 0;
    for (const char *dir : dirs) {
        if (!SD_MMC.exists(dir)) continue;
        char target[48];
        snprintf(target, sizeof(target), "%s/%08lx%s", TRASH_DIR, (unsigned long)esp_random(), dir + 1);
        if (SD_MMC.rename(dir, target)) {
            moved++;
        } else {
            ESP_LOGW(TAG, "cannot move %s to the trash", dir);
        }
    }
    createDirectory("/assets");
    createDirectory("/.requestmanager");
    createDirectory("/.requestmanager/manifests");
    createDirectory("/.requestmanager/pending_manifests");
    if (!enqueueJob(5, TRASH_DIR, nullptr, 0)) {
        ESP_LOGW(TAG, "trash sweep not queued (next boot does it)");
    }
    ESP_LOGW(TAG, "card cache wiped: %d folder(s) moved to %s", moved, TRASH_DIR);
    return true;
}

// Maintenance task. Deletes /.trash bottom-up, a few entries at a time (a directory listing is
// never held in memory as a whole: a story folder may hold thousands of files).
void FileManager::sweepTrash() {
    const unsigned long t0 = millis();
    int removed = 0;
    std::function<void(const String &, int)> removeTree = [&](const String &dir, int depth) {
        if (depth > 8) return;
        for (;;) {
            File d = SD_MMC.open(dir);
            if (!d || !d.isDirectory()) {
                if (d) d.close();
                return;
            }
            std::vector<String> files, subdirs;
            for (File e = d.openNextFile(); e && files.size() + subdirs.size() < 32; e = d.openNextFile()) {
                String full = dir + "/" + e.name();
                if (e.isDirectory()) subdirs.push_back(full); else files.push_back(full);
                e.close();
            }
            d.close();
            if (files.empty() && subdirs.empty()) break;
            const int before = removed;
            bool subGone = false;
            for (const String &f : files) {
                if (SD_MMC.remove(f)) removed++;
            }
            for (const String &sub : subdirs) {
                removeTree(sub, depth + 1);
                subGone = subGone || !SD_MMC.exists(sub);
            }
            if (removed == before && !subGone) {
                break; // nothing could be removed this round: give up instead of spinning
            }
        }
        SD_MMC.rmdir(dir);
    };
    removeTree(TRASH_DIR, 0);
    ESP_LOGI(TAG, "trash erased: %d file(s) in %lu s", removed, (millis() - t0) / 1000);
}

void FileManager::resetDownloadStats() {
    downloadStats.totalDownloads = 0;
    downloadStats.successfulDownloads = 0;
    downloadStats.failedDownloads = 0;
    downloadStats.totalBytesDownloaded = 0;
    saveDownloadStats();
    ESP_LOGI(TAG, "Download statistics reset");
}

bool FileManager::removeDirectory(const String& path) {
    bool success = SD_MMC.rmdir(path);
    
    if (success) {
        ESP_LOGI(TAG, "Directory removed successfully: %s", path.c_str());
    } else {
        ESP_LOGW(TAG, "Failed to remove directory: %s", path.c_str());
    }
    
    return success;
}

void FileManager::printFileTree() {
    ESP_LOGI(TAG, "=== SD Card File Tree ===");

    std::function<void(const String&, int)> printTree = [&](const String& dir, int depth) {
        File dirFile = SD_MMC.open(dir);
        if (!dirFile || !dirFile.isDirectory()) {
            if (dirFile) dirFile.close();
            return;
        }

        while (true) {
            File entry = dirFile.openNextFile();
            if (!entry) break;

            String indent;
            for (int i = 0; i < depth; ++i) indent += "  ";
            String name = entry.name();
            if (entry.isDirectory()) {
                ESP_LOGI(TAG, "%s%s/", indent.c_str(), name.c_str());
                String subDir = dir;
                if (!subDir.endsWith("/")) subDir += "/";
                subDir += name;
                entry.close();
                printTree(subDir, depth + 1);
            } else {
                ESP_LOGI(TAG, "%s%s", indent.c_str(), name.c_str());
                entry.close();
            }
        }
        dirFile.close();
    };

    printTree("/", 0);
    ESP_LOGI(TAG, "==========================");
}

void FileManager::formatSDCard() {
    ESP_LOGI(TAG, "=== Formatting SD Card ===");
    
    if (!sdCardInitialized) {
        ESP_LOGW(TAG, "SD card not initialized - run sddiag first");
        return;
    }
    ESP_LOGI(TAG, "Formatting...");

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

    createDirectory("/assets");

    ESP_LOGI(TAG, "Format complete. All files and directories removed.");
}

// Bulk deletion methods implementation
void FileManager::clearAllRequiredFiles() {
    ESP_LOGI(TAG, "Clearing all required files from NVS and storage...");
    
    int filesDeleted = 0;
    int filesNotFound = 0;
    
    // Delete files based on required files list
    for (const auto& file : requiredFiles) {
        if (fileExists(file.path)) {
            if (deleteFile(file.path)) {
                filesDeleted++;
                ESP_LOGI(TAG, "Deleted: %s", file.path.c_str());
            } else {
                ESP_LOGW(TAG, "Failed to delete: %s", file.path.c_str());
            }
        } else {
            filesNotFound++;
            ESP_LOGI(TAG, "File not found: %s", file.path.c_str());
        }
    }
    
    // Clear required files list from memory and NVS
    requiredFiles.clear();
    saveRequiredFiles();
    
    // Also clear download queue
    downloadQueue.clear();
    saveDownloadQueue();
    
    ESP_LOGI(TAG, "Cleared all required files. Deleted %d files, %d were already missing.", 
                 filesDeleted, filesNotFound);
}

// Sequential write benchmark with per-write timing. No network involved: isolates the
// card's own behaviour (e.g. periodic multi-second pauses) from the download pipeline.
void FileManager::benchmarkSequentialWrite(size_t chunkBytes, int count) {
    if (!sdCardInitialized || downloadInProgress || !persistentBufferA) {
        ESP_LOGI(TAG, "bench unavailable (SD not ready or download in progress)");
        return;
    }
    if (chunkBytes == 0 || chunkBytes > DOWNLOAD_BUFFER_SIZE) {
        chunkBytes = DOWNLOAD_BUFFER_SIZE;
    }
    if (count <= 0) {
        count = 64;
    }
    createDirectory(SD_DATA_DIR);
    const char* path = "/.filemanager/bench.bin";
    if (SD_MMC.exists(path)) {
        SD_MMC.remove(path);
    }
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        ESP_LOGW(TAG, "bench: cannot open file");
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
            ESP_LOGW(TAG, "bench: short write at #%d (%u/%u)", i, (unsigned)written, (unsigned)chunkBytes);
            break;
        }
        if (dt < minUs) minUs = dt;
        if (dt > maxUs) maxUs = dt;
        sumUs += dt;
        if (dt > 300000) {
            slow++;
            ESP_LOGI(TAG, "bench: write #%d took %lu ms", i, dt / 1000);
        }
    }
    unsigned long fs = micros();
    f.flush();
    unsigned long flushUs = micros() - fs;
    f.close();
    unsigned long total = millis() - t0;
    SD_MMC.remove(path);
    ESP_LOGI(TAG, "bench: %d x %u KB: min %.1f ms, avg %.1f ms, max %.1f ms, >300ms: %d, flush %lu ms, total %lu ms = %.2f MB/s",
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

// Called from the async_tcp task and from the SD writer: the first caller wins the
// compare-exchange; the loop task only reads the result after FINISH_READY is published.
void FileManager::requestAsyncFinish(bool success, const char *error, bool permanent, uint8_t kind) {
    uint8_t expected = FINISH_NONE;
    if (!asyncState.finishState.compare_exchange_strong(expected, FINISH_CLAIMED, std::memory_order_acq_rel)) {
        return;
    }
    asyncState.finishSuccess = success;
    asyncState.permanentFailure = permanent;
    asyncState.finishKind = kind;
    strncpy(asyncState.finishError, error ? error : "", sizeof(asyncState.finishError) - 1);
    asyncState.finishError[sizeof(asyncState.finishError) - 1] = '\0';
    asyncState.finishState.store(FINISH_READY, std::memory_order_release);
}

void FileManager::onAsyncConnect(void* arg, AsyncClient* client) {
    FileManager* self = static_cast<FileManager*>(arg);
    if (self->asyncState.finishState.load(std::memory_order_acquire) != FINISH_NONE) {
        return;
    }
    self->asyncState.connectTime = millis() - self->asyncState.startTime;
    ESP_LOGI(TAG, "Connected in %lu ms", self->asyncState.connectTime);

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
    if (self->asyncState.resumeFrom > 0) {
        request += "Range: bytes=" + String((unsigned long)self->asyncState.resumeFrom) + "-\r\n";
    }
    request += "User-Agent: TilkieTalkie/1.0 (ESP32-S3)\r\n";
    request += "\r\n";
    client->write(request.c_str(), request.length());
}

void FileManager::onAsyncData(void* arg, AsyncClient* client, void* data, size_t len) {
    FileManager* self = static_cast<FileManager*>(arg);
    if (self->asyncState.finishState.load(std::memory_order_acquire) != FINISH_NONE) {
        return; // finishing; ignore whatever else the server sends
    }
    unsigned long dataCallbackStart = millis();

    auto bufferBodyData = [&](const uint8_t* chunk, size_t chunkLen) -> bool {
        if (!self->ringPush(chunk, chunkLen)) {
            self->requestAsyncFinish(false, "SD writer stalled (ring full)", false, FK_STALLED);
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
        const bool resuming = self->asyncState.resumeFrom > 0;
        if (resuming && status == 416) {
            // The partial file is not a prefix of what the server has (file replaced): start over.
            self->requestAsyncFinish(false, "HTTP 416 on resume", false, FK_RESTART);
            return;
        }
        if (resuming && status == 200) {
            // Range ignored: the whole file comes; the writer truncates the partial one first.
            self->asyncState.restartFromZero = true; // published to the writer by the ring index (release)
            ESP_LOGI(TAG, "server sent the whole file (200) - restarting from zero");
        } else if (resuming && status == 206) {
            const char* cr = strcasestr(self->asyncState.headerBuffer, "content-range:");
            unsigned long long first = 0, last = 0, total = 0;
            if (!cr || sscanf(cr + 14, " bytes %llu-%llu/%llu", &first, &last, &total) != 3 ||
                first != self->asyncState.resumeFrom || last + 1 != total) {
                self->requestAsyncFinish(false, "unexpected Content-Range on resume", false, FK_RESTART);
                return;
            }
            self->asyncState.rangeTotal = (int64_t)total;
        } else if (status != 200) {
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
            ESP_LOGI(TAG, "Content-Length: %d bytes", self->asyncState.contentLength);
            if (self->asyncState.contentLength > 0 &&
                static_cast<uint64_t>(self->asyncState.contentLength) + (1024 * 1024) > self->asyncState.freeBytesAtStart) {
                self->requestAsyncFinish(false, "Insufficient SD card space", false, FK_NO_SPACE);
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
        ESP_LOGI(TAG, "slow onData callback: %lu ms (%u bytes)", callbackDuration, (unsigned)len);
    }

    if (self->asyncState.contentLength > 0) {
        int progress = (int)((uint64_t)self->asyncState.totalDownloaded * 100 / self->asyncState.contentLength);
        if (progress >= self->asyncState.lastReportedProgress + 20) {
            self->asyncState.lastReportedProgress = progress;
            unsigned long elapsed = millis() - self->asyncState.startTime;
            float mbps = elapsed > 0 ? (self->asyncState.totalDownloaded * 8.0f) / (elapsed / 1000.0f) / 1e6f : 0;
            ESP_LOGI(TAG, "progress %d%% (%u/%d bytes) %.2f Mbps, %d SD writes",
                          progress, (unsigned)self->asyncState.totalDownloaded, self->asyncState.contentLength,
                          mbps, self->asyncState.writeCount);
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
    self->requestAsyncFinish(false, "Network timeout", false, FK_STALLED);
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
        const size_t head = asyncState.ringHead.load(std::memory_order_relaxed); // only this task writes it
        size_t headIdx = head % RING_SIZE;
        size_t first = (RING_SIZE - headIdx) < n ? (RING_SIZE - headIdx) : n;
        memcpy(ringBuffer + headIdx, data, first);
        if (n > first) {
            memcpy(ringBuffer, data + first, n - first);
        }
        asyncState.ringHead.store(head + n, std::memory_order_release); // bytes visible before the index
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
// Resume: the hash must cover the bytes already on the card. Read through the bounce buffer
// before any new byte is written (the ring absorbs the network meanwhile).
static bool hashPrefix(File &f, size_t len, uint8_t *buf, size_t bufLen, mbedtls_sha256_context *sha, volatile bool *stop)
{
    while (len > 0) {
        if (*stop) return false;
        const size_t want = len < bufLen ? len : bufLen;
        const int n = f.read(buf, want);
        if (n <= 0) return false;
        mbedtls_sha256_update(sha, buf, (size_t)n);
        len -= (size_t)n;
    }
    return true;
}

// SD writer: the temp file of a resumed download is opened here, once the server's answer is
// known: append after the prefix (206) or truncate (200, restartFromZero).
bool FileManager::writerOpenFile() {
    if (asyncState.file) {
        return true;
    }
    if (asyncState.restartFromZero) {
        mbedtls_sha256_free(&asyncState.sha); // the prefix hash is void
        mbedtls_sha256_init(&asyncState.sha);
        mbedtls_sha256_starts(&asyncState.sha, 0);
        asyncState.file = SD_MMC.open(asyncState.tempPath, FILE_WRITE);
    } else {
        asyncState.file = SD_MMC.open(asyncState.tempPath, FILE_APPEND);
    }
    if (!asyncState.file) {
        asyncState.fileOpenFailed = true;
        return false;
    }
    return true;
}

void FileManager::sdWriteTask(void* parameter) {
    FileManager* self = static_cast<FileManager*>(parameter);
    if (self->asyncState.resumeFrom > 0) {
        File prefix = SD_MMC.open(self->asyncState.tempPath, FILE_READ);
        const bool ok = prefix && hashPrefix(prefix, self->asyncState.resumeFrom, self->persistentBufferA, DOWNLOAD_BUFFER_SIZE,
                                             &self->asyncState.sha, &self->asyncState.discardRing);
        if (prefix) prefix.close();
        if (!ok && !self->asyncState.discardRing) {
            self->requestAsyncFinish(false, "partial file unreadable", false, FK_RESTART);
            self->asyncState.discardRing = true;
        }
    }
    for (;;) {
        size_t avail = self->ringAvailable();
        const bool flushTail = self->asyncState.producerDone && avail > 0;
        if (self->asyncState.discardRing) {
            break;
        }
        if ((avail >= DOWNLOAD_BUFFER_SIZE || flushTail) && !self->writerOpenFile()) {
            self->requestAsyncFinish(false, "temp file could not be opened", false, FK_SD_WRITE);
            self->asyncState.discardRing = true;
            break;
        }
        if (avail >= DOWNLOAD_BUFFER_SIZE || flushTail) {
            size_t n = avail < DOWNLOAD_BUFFER_SIZE ? avail : DOWNLOAD_BUFFER_SIZE;
            const size_t tail = self->asyncState.ringTail.load(std::memory_order_relaxed); // only this task writes it
            size_t tailIdx = tail % RING_SIZE;
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
            if (writeTime / 1000 > self->asyncState.maxWriteMs) {
                self->asyncState.maxWriteMs = writeTime / 1000; // SD_SLOW is raised by the loop task
            }
            self->asyncState.writeCount++;
            if (writeTime > 300000) {
                ESP_LOGI(TAG, "slow SD write #%d: %lu ms for %u bytes (ring backlog %u KB)",
                              self->asyncState.writeCount, writeTime / 1000, (unsigned)n,
                              (unsigned)(self->ringAvailable() / 1024));
            }
            if (written != n) {
                ESP_LOGW(TAG, "SD write #%d short: %u/%u bytes",
                              self->asyncState.writeCount, (unsigned)written, (unsigned)n);
                self->requestAsyncFinish(false, "SD write failed", false, FK_SD_WRITE);
                self->asyncState.discardRing = true;
                break;
            }
            self->asyncState.ringTail.store(tail + n, std::memory_order_release); // slot free for the producer
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
            ESP_LOGW(TAG, "SD writer did not exit; leaking its semaphore rather than crashing");
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
        bytesWritten = asyncState.file.size(); // the whole file: a resumed prefix included
        asyncState.file.close();
    }
    // What the complete file must measure: Content-Range's total on a resume (206), else
    // Content-Length (a 200, also the restart of a resume).
    const bool resumed = asyncState.resumeFrom > 0 && !asyncState.restartFromZero;
    const int64_t expectedSize = resumed ? asyncState.rangeTotal : (int64_t)asyncState.contentLength;
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
        } else if (expectedSize > 0 && (int64_t)bytesWritten < expectedSize) {
            // The server closed early (proxy timeout, link drop): transient, retried with backoff
            // (and resumed from here).
            success = false;
            errorMsg = "Incomplete: wrote " + String(bytesWritten) + " expected " + String((long)expectedSize);
        } else if (expectedSize > 0 && (int64_t)bytesWritten > expectedSize) {
            success = false;
            hardFailure = true; // more bytes than announced: the server is broken, not the link
            errorMsg = "Size mismatch: wrote " + String(bytesWritten) + " expected " + String((long)expectedSize);
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
            Faults::raise(Fault::SD_WRITE_FAILED, faultDetail(asyncState.localPath).c_str(), false);
        }
    }
    // Faults are raised here, on the loop task (Faults is not thread-safe); the callbacks and
    // the SD writer only record what happened.
    if (asyncState.maxWriteMs > SD_SLOW_WRITE_MS) {
        char d[24];
        snprintf(d, sizeof(d), "write=%lums", asyncState.maxWriteMs);
        Faults::raise(Fault::SD_SLOW, d, false);
    }
    if (!success) {
        switch (asyncState.finishKind) {
        case FK_STALLED: Faults::raise(Fault::DOWNLOAD_STALLED, faultDetail(asyncState.localPath).c_str(), false); break;
        case FK_NO_SPACE: Faults::raise(Fault::SD_FULL, faultDetail(asyncState.localPath).c_str()); break;
        case FK_SD_WRITE: Faults::raise(Fault::SD_WRITE_FAILED, faultDetail(asyncState.localPath).c_str(), false); break;
        default: break;
        }
    }
    // A transient failure keeps the partial file for a Range resume (it is a valid prefix: the
    // writer only appends whole chunks); broken data, a refused resume, a full card or a
    // write error delete it. Terminal give-ups delete it below.
    const bool restartPending = asyncState.finishKind == FK_RESTART;
    const bool keepPartial = !success && !hardFailure && !asyncState.permanentFailure && !restartPending &&
                             !asyncState.checksum.isEmpty() && !asyncState.fileOpenFailed &&
                             asyncState.finishKind != FK_SD_WRITE && asyncState.finishKind != FK_NO_SPACE;
    if (!success && !keepPartial && SD_MMC.exists(asyncState.tempPath)) {
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
        ESP_LOGI(TAG, "Download completed: %s (%u bytes, %.2f Mbps, %d SD writes, %lu ms in SD writes)%s",
                      asyncState.localPath.c_str(), (unsigned)bytesWritten, mbps, asyncState.writeCount,
                      asyncState.totalWriteTime, asyncState.checksum.isEmpty() ? "" : ", sha256 verified");
        if (it != downloadQueue.end()) {
            downloadQueue.erase(it);
            saveDownloadQueue();
        }
        uint64_t freeNow = cachedFreeBytes.load();
        while (freeNow > bytesWritten &&
               !cachedFreeBytes.compare_exchange_weak(freeNow, freeNow - bytesWritten)) {
            // the maintenance task refreshed it meanwhile: retry against the new value
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
        if (restartPending && !terminal) {
            // The partial file was the problem, not the link: the retry from zero is immediate
            // and does not count as an attempt.
            if (it->retryCount > 0) it->retryCount--;
            it->lastAttempt = 0;
        }
        if (terminal) {
            downloadQueue.erase(it);
            saveDownloadQueue();
        } else {
            queueDirty = true; // retry bookkeeping: flushed by update()
        }
    } else {
        terminal = true; // ad-hoc download (not queued): nothing will retry it
    }
    if (terminal && keepPartial && SD_MMC.exists(asyncState.tempPath)) {
        SD_MMC.remove(asyncState.tempPath); // nothing will resume it
    }
    ESP_LOGW(TAG, "Download failed: %s (%s)%s", asyncState.localPath.c_str(), errorMsg.c_str(),
                  terminal ? " - giving up" : " - will retry");
    const String url = asyncState.url, path = asyncState.localPath;
    downloadInProgress = false;
    asyncState.reset();
    saveDownloadStats();
    if (terminal) {
        Faults::raise(Fault::DOWNLOAD_FAILED, faultDetail(path).c_str(), false);
    }
    if (terminal && downloadCompleteCallback) {
        downloadCompleteCallback(url, path, false, errorMsg);
    }
}
