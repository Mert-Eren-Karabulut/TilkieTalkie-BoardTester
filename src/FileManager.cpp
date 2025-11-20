#include "FileManager.h"
#include "BatteryManagement.h"
#include <SD_MMC.h>
#include <AsyncTCP.h>
#include <esp_heap_caps.h>

// Initialize static members
FileManager* FileManager::instance = nullptr;
const char* FileManager::NVS_NAMESPACE = "filemanager";
const char* FileManager::NVS_DOWNLOAD_QUEUE_KEY = "dl_queue";
const char* FileManager::NVS_FILE_LIST_KEY = "file_list";
const char* FileManager::NVS_DOWNLOAD_STATS_KEY = "dl_stats";

FileManager::FileManager() : 
    sdCardInitialized(false),
    downloadInProgress(false),
    downloadProgressCallback(nullptr),
    downloadCompleteCallback(nullptr),
    fileSystemEventCallback(nullptr) {
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
    
    // Initialize NVS
    if (!initializeNVS()) {
        Serial.println("FileManager: Failed to initialize NVS");
        return false;
    }
    
    // Load persistent data
    loadDownloadStats();
    loadDownloadQueue();
    loadRequiredFiles();
    
    // Initialize SD card
    if (!initializeSDCard()) {
        Serial.println("FileManager: Failed to initialize SD card");
        return false;
    }
    
    Serial.println("FileManager: Initialization complete");
    return true;
}

void FileManager::end() {
    // Save current state
    saveDownloadQueue();
    saveRequiredFiles();
    saveDownloadStats();
    
    // Close NVS
    if (nvsHandle != 0) {
        nvs_close(nvsHandle);
    }
    
    // Unmount SD card
    SD_MMC.end();
    sdCardInitialized = false;
    
    Serial.println("FileManager: Shutdown complete");
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
    
    // Try multiple initialization attempts
    bool sdInitialized = false;
    
    for (int attempt = 1; attempt <= 3 && !sdInitialized; attempt++) {
        Serial.printf("FileManager: SD card initialization attempt %d/3\n", attempt);
        
        // Initialize SD_MMC with 4-bit mode at 40MHz (SDMMC_FREQ_HIGHSPEED)
        // Parameters: mountpoint, mode1bit (false for 4-bit), format_if_mount_failed, frequency
        SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
        sdInitialized = SD_MMC.begin("/sd", false, false, BOARD_MAX_SDMMC_FREQ);
        
        if (!sdInitialized) {
            Serial.printf("FileManager: Attempt %d failed, retrying...\n", attempt);
            SD_MMC.end();
            delay(500);
        }
    }
    
    if (!sdInitialized) {
        Serial.println("FileManager: SD card initialization failed after 3 attempts");
        Serial.println("FileManager: Please check:");
        Serial.println("  1. SD card is properly inserted");
        Serial.println("  2. Wiring connections are correct");
        Serial.println("  3. SD card is formatted as FAT32");
        Serial.println("  4. Power supply is adequate");
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
    
    // Create necessary directories
    createDirectory("/audio");
    createDirectory("/temp");
    createDirectory("/logs");
    createDirectory("/images");
    createDirectory("/figures");
    
    return true;
}

bool FileManager::initializeNVS() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvsHandle);
    if (err != ESP_OK) {
        Serial.printf("FileManager: Error opening NVS handle: %s\n", esp_err_to_name(err));
        return false;
    }
    
    return true;
}

bool FileManager::checkConnectivity() {
    // Check WiFi connection
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("FileManager: WiFi not connected");
        return false;
    }
    
    // Check internet connectivity by pinging Google's DNS
    return pingGoogle();
}

bool FileManager::pingGoogle() {
    Serial.println("FileManager: Checking internet connectivity...");
    
    // Use WiFiClient for memory efficiency instead of HTTPClient
    WiFiClient client;
    client.setTimeout(5000); // 5 second timeout
    
    if (!client.connect("www.google.com", 80)) {
        Serial.printf("FileManager: Internet connectivity: FAILED (connection failed)\n");
        return false;
    }
    
    // Send simple HTTP HEAD request (lighter than GET)
    String request = "HEAD / HTTP/1.1\r\n";
    request += "Host: www.google.com\r\n";
    request += "Connection: close\r\n";
    request += "\r\n";
    
    client.print(request);
    
    // Wait for response with timeout
    unsigned long startTime = millis();
    bool responseReceived = false;
    
    while (client.connected() && (millis() - startTime < 5000)) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (line.startsWith("HTTP/")) {
                responseReceived = true;
                break;
            }
        }
        delay(1);
    }
    
    client.stop();
    
    bool connected = responseReceived;
    Serial.printf("FileManager: Internet connectivity: %s\n", connected ? "OK" : "FAILED");
    
    return connected;
}

bool FileManager::isChargingRequired() {
    BatteryManager& battery = BatteryManager::getInstance();
    return battery.getChargingStatus();
}

void FileManager::update() {
    if (!sdCardInitialized) {
        return;
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
    
    File file = SD_MMC.open(path);
    if (file) {
        file.close();
        return true;
    }
    return false;
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
    task.retryBatch = 0;
    task.completed = false;
    task.lastAttempt = 0;
    task.lastBatchAttempt = 0;
    
    downloadQueue.push_back(task);
}

bool FileManager::downloadFileFromURL(const String& url, const String& localPath, String& errorMsg) {
    if (downloadInProgress) {
        errorMsg = "Another download is in progress";
        return false;
    }
    
    if (!sdCardInitialized) {
        errorMsg = "SD card not initialized";
        return false;
    }
    
    downloadInProgress = true;
    
    // Convert HTTPS to HTTP to reduce memory usage
    String httpUrl = url;
    if (httpUrl.startsWith("https://")) {
        httpUrl.replace("https://", "http://");
    }
    
    // Parse URL to extract hostname and path
    String hostname, path;
    int port = 80;
    
    if (httpUrl.startsWith("http://")) {
        int hostStart = 7; // Length of "http://"
        int pathStart = httpUrl.indexOf('/', hostStart);
        
        if (pathStart == -1) {
            hostname = httpUrl.substring(hostStart);
            path = "/";
        } else {
            hostname = httpUrl.substring(hostStart, pathStart);
            path = httpUrl.substring(pathStart);
        }
        
        // Check for port in hostname
        int portIndex = hostname.indexOf(':');
        if (portIndex != -1) {
            port = hostname.substring(portIndex + 1).toInt();
            hostname = hostname.substring(0, portIndex);
        }
    } else {
        errorMsg = "Invalid URL format (must start with http://)";
        downloadInProgress = false;
        return false;
    }
    
    Serial.printf("FileManager: Starting async download: %s -> %s\n", httpUrl.c_str(), localPath.c_str());
    Serial.printf("FileManager: Connecting to %s:%d\n", hostname.c_str(), port);
    
    // Create directory structure
    if (!createDirectoryStructure(localPath)) {
        errorMsg = "Failed to create directory structure";
        downloadInProgress = false;
        return false;
    }
    
    // Initialize async state
    asyncState.reset();
    asyncState.url = httpUrl;
    asyncState.localPath = localPath;
    asyncState.tempPath = localPath + ".tmp";
    asyncState.startTime = millis();
    asyncState.lastDataTime = millis();
    
    // Remove any existing temp file
    if (SD_MMC.exists(asyncState.tempPath)) {
        SD_MMC.remove(asyncState.tempPath);
    }
    
    // Open temp file for writing
    asyncState.file = SD_MMC.open(asyncState.tempPath, FILE_WRITE);
    if (!asyncState.file) {
        errorMsg = "Failed to create temporary file";
        downloadInProgress = false;
        return false;
    }
    
    // Allocate double buffers from internal SRAM with DMA alignment
    asyncState.bufferSize = DOWNLOAD_BUFFER_SIZE;
    asyncState.bufferA = (uint8_t*)heap_caps_malloc(asyncState.bufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    asyncState.bufferB = (uint8_t*)heap_caps_malloc(asyncState.bufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    
    if (!asyncState.bufferA || !asyncState.bufferB) {
        errorMsg = "Failed to allocate double buffers from internal SRAM";
        Serial.printf("FileManager: ❌ Failed to allocate 2x%dKB buffers from internal SRAM\n", DOWNLOAD_BUFFER_SIZE / 1024);
        if (asyncState.bufferA) free(asyncState.bufferA);
        if (asyncState.bufferB) free(asyncState.bufferB);
        asyncState.file.close();
        downloadInProgress = false;
        return false;
    }
    Serial.printf("FileManager: ✓ Allocated 2x%dKB DMA-aligned double buffers from internal SRAM\n", asyncState.bufferSize / 1024);
    
    // Create semaphores for buffer coordination
    asyncState.bufferSwapSemaphore = xSemaphoreCreateBinary();
    asyncState.writeCompleteSemaphore = xSemaphoreCreateBinary();
    
    if (!asyncState.bufferSwapSemaphore || !asyncState.writeCompleteSemaphore) {
        errorMsg = "Failed to create buffer semaphores";
        Serial.println("FileManager: ❌ Failed to create semaphores");
        if (asyncState.bufferSwapSemaphore) vSemaphoreDelete(asyncState.bufferSwapSemaphore);
        if (asyncState.writeCompleteSemaphore) vSemaphoreDelete(asyncState.writeCompleteSemaphore);
        free(asyncState.bufferA);
        free(asyncState.bufferB);
        asyncState.file.close();
        downloadInProgress = false;
        return false;
    }
    
    // Initialize as available (write task starts ready)
    xSemaphoreGive(asyncState.writeCompleteSemaphore);
    
    // Create SD write task on Core 1 (separate from WiFi/AsyncTCP on Core 0)
    asyncState.writeTaskRunning = true;
    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        sdWriteTask,
        "SDWriteTask",
        4096,  // Stack size
        this,  // Parameter
        1,     // Priority (lower than network)
        &asyncState.writeTaskHandle,
        1      // Core 1
    );
    
    if (taskCreated != pdPASS) {
        errorMsg = "Failed to create SD write task";
        Serial.println("FileManager: ❌ Failed to create write task");
        vSemaphoreDelete(asyncState.bufferSwapSemaphore);
        free(asyncState.bufferA);
        free(asyncState.bufferB);
        asyncState.file.close();
        downloadInProgress = false;
        return false;
    }
    
    Serial.println("FileManager: ✓ SD write task created on Core 0");
    
    // Create AsyncClient
    asyncState.client = new AsyncClient();
    if (!asyncState.client) {
        errorMsg = "Failed to create AsyncClient";
        vSemaphoreDelete(asyncState.bufferSwapSemaphore);
        free(asyncState.bufferA);
        free(asyncState.bufferB);
        asyncState.file.close();
        downloadInProgress = false;
        return false;
    }
    
    // Set up callbacks
    asyncState.client->onConnect(onAsyncConnect, this);
    asyncState.client->onData(onAsyncData, this);
    asyncState.client->onDisconnect(onAsyncDisconnect, this);
    asyncState.client->onError(onAsyncError, this);
    asyncState.client->onTimeout(onAsyncTimeout, this);
    
    // TCP optimizations
    asyncState.client->setNoDelay(true);
    asyncState.client->setRxTimeout(60);
    asyncState.client->setAckTimeout(1000);
    
    // Connect to server
    if (!asyncState.client->connect(hostname.c_str(), port)) {
        errorMsg = "Failed to connect to server";
        cleanupAsyncDownload(false, errorMsg);
        return false;
    }
    
    // Store path for request (need to keep it for callback)
    // We'll send the HTTP request in the onConnect callback
    asyncState.checksum = path; // Temporary storage for path
    
    return true; // Actual success/failure will be determined in callbacks
}

void FileManager::processDownloadQueue() {
    if (downloadQueue.empty() || downloadInProgress) {
        return;
    }
    
    // First pass: Remove completed tasks and permanently failed tasks
    auto it = downloadQueue.begin();
    while (it != downloadQueue.end()) {
        DownloadTask& task = *it;
        
        if (task.completed) {
            it = downloadQueue.erase(it);
            continue;
        }
        
        // Check if we've exceeded the maximum retry batches
        if (task.retryBatch >= MAX_RETRY_BATCHES) {
            Serial.printf("FileManager: Download permanently failed after %d retry batches: %s\n", 
                         MAX_RETRY_BATCHES, task.url.c_str());
            
            downloadStats.totalDownloads++;
            downloadStats.failedDownloads++;
            
            if (downloadCompleteCallback) {
                downloadCompleteCallback(task.url, task.localPath, false, "Max retry batches exceeded");
            }
            
            it = downloadQueue.erase(it);
            continue;
        }
        
        ++it;
    }
    
    // Second pass: Find the first task that's ready to be processed
    bool taskProcessed = false;
    it = downloadQueue.begin();
    while (it != downloadQueue.end() && !taskProcessed) {
        DownloadTask& task = *it;
        
        // Check if we need to wait between retry batches
        if (task.retryCount >= MAX_RETRY_COUNT) {
            // We've exhausted this batch of retries, need to wait before starting next batch
            if (task.lastBatchAttempt == 0) {
                task.lastBatchAttempt = millis();
                Serial.printf("FileManager: Retry batch %d failed for %s, waiting %d seconds before next batch\n", 
                             task.retryBatch + 1, task.url.c_str(), RETRY_BATCH_DELAY_MS / 1000);
            }
            
            if ((millis() - task.lastBatchAttempt) >= RETRY_BATCH_DELAY_MS) {
                // Start new retry batch
                task.retryBatch++;
                task.retryCount = 0;
                task.lastBatchAttempt = 0;
                task.lastAttempt = 0;
                Serial.printf("FileManager: Starting retry batch %d for %s\n", 
                             task.retryBatch + 1, task.url.c_str());
            } else {
                ++it;
                continue;
            }
        }
        
        // Check if enough time has passed since last individual retry
        if (task.lastAttempt > 0 && 
            (millis() - task.lastAttempt) < RETRY_DELAY_MS) {
            ++it;
            continue;
        }
        
        // Check connectivity before attempting download
        if (!checkConnectivity()) {
            // Don't increment retry count for connectivity issues, just wait
            task.lastAttempt = millis();
            ++it;
            continue;
        }
        
        // Check if file already exists and is valid
        if (fileExists(task.localPath)) {
            if (task.checksum.isEmpty() || verifyFileIntegrity(task.localPath, task.checksum)) {
                Serial.printf("FileManager: File already exists and is valid: %s\n", task.localPath.c_str());
                task.completed = true;
                ++it;
                continue;
            } else {
                Serial.printf("FileManager: Existing file failed integrity check, re-downloading: %s\n", task.localPath.c_str());
                deleteFile(task.localPath);
            }
        }
        
        // Process this task
        String errorMsg;
        task.lastAttempt = millis();
        task.retryCount++;
        
        Serial.printf("FileManager: Attempting download (batch %d, attempt %d/%d): %s\n", 
                     task.retryBatch + 1, task.retryCount, MAX_RETRY_COUNT, task.url.c_str());
        
        bool downloadSuccess = downloadFileFromURL(task.url, task.localPath, errorMsg);
        
        if (downloadSuccess) {
            // Verify integrity if checksum provided
            if (!task.checksum.isEmpty() && !verifyFileIntegrity(task.localPath, task.checksum)) {
                Serial.printf("FileManager: Downloaded file failed integrity check: %s\n", task.localPath.c_str());
                deleteFile(task.localPath);
                Serial.printf("FileManager: Download attempt %d/%d failed (integrity): %s\n", 
                             task.retryCount, MAX_RETRY_COUNT, errorMsg.c_str());
                downloadSuccess = false;
            } else {
                task.completed = true;
                Serial.printf("FileManager: Download successful: %s\n", task.localPath.c_str());
            }
        } else {
            Serial.printf("FileManager: Download attempt %d/%d failed: %s (Error: %s)\n", 
                         task.retryCount, MAX_RETRY_COUNT, task.url.c_str(), errorMsg.c_str());
        }
        
        // If download failed and hasn't reached max retries, move to end of queue for fair processing
        if (!downloadSuccess && task.retryCount < MAX_RETRY_COUNT) {
            // Move failed task to end of queue to give other tasks a chance
            DownloadTask failedTask = task;
            downloadQueue.erase(it);
            downloadQueue.push_back(failedTask);
            Serial.printf("FileManager: Moved failed download to end of queue: %s\n", failedTask.localPath.c_str());
        }
        
        taskProcessed = true; // Process only one download per call
    }
    
    saveDownloadQueue();
}

bool FileManager::addRequiredFile(const String& localPath, const String& url, const String& checksum) {
    // Check if already in list
    for (const auto& file : requiredFiles) {
        if (file.path == localPath) {
            Serial.printf("FileManager: File already in required list: %s\n", localPath.c_str());
            return true;
        }
    }
    
    FileEntry entry;
    entry.path = localPath;
    entry.url = url;
    entry.required = true;
    entry.checksum = checksum;
    
    requiredFiles.push_back(entry);
    saveRequiredFiles();
    
    Serial.printf("FileManager: Added required file: %s\n", localPath.c_str());
    return true;
}

void FileManager::checkRequiredFiles() {
    for (const auto& file : requiredFiles) {
        if (!fileExists(file.path)) {
            Serial.printf("FileManager: Required file missing, scheduling download: %s\n", file.path.c_str());
            scheduleDownload(file.url, file.path, file.checksum);
        } else if (!file.checksum.isEmpty() && !verifyFileIntegrity(file.path, file.checksum)) {
            Serial.printf("FileManager: Required file failed integrity check, re-downloading: %s\n", file.path.c_str());
            deleteFile(file.path);
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
    // Simple CRC32 checksum implementation
    // For production, consider using a more robust hash like SHA256
    
    File file = SD_MMC.open(filePath);
    if (!file) {
        return "";
    }
    
    uint32_t crc = 0xFFFFFFFF;
    uint8_t buffer[256];
    
    while (file.available()) {
        size_t bytesRead = file.read(buffer, sizeof(buffer));
        for (size_t i = 0; i < bytesRead; i++) {
            crc ^= buffer[i];
            for (int j = 0; j < 8; j++) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320;
                } else {
                    crc >>= 1;
                }
            }
        }
    }
    
    file.close();
    crc ^= 0xFFFFFFFF;
    
    return String(crc, HEX);
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

size_t FileManager::getSDCardFreeSpace() {
    if (!sdCardInitialized) {
        return 0;
    }
    return SD_MMC.cardSize() - SD_MMC.usedBytes();
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
        Serial.printf("      Batch: %d/%d, Attempt: %d/%d, Completed: %s\n", 
                      task.retryBatch + 1, MAX_RETRY_BATCHES,
                      task.retryCount, MAX_RETRY_COUNT,
                      task.completed ? "yes" : "no");
        
        if (!task.completed && task.retryCount >= MAX_RETRY_COUNT && task.lastBatchAttempt > 0) {
            unsigned long timeLeft = RETRY_BATCH_DELAY_MS - (millis() - task.lastBatchAttempt);
            if (timeLeft > 0) {
                Serial.printf("      Waiting %lu seconds before next batch\n", timeLeft / 1000);
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

// NVS operations implementation
bool FileManager::saveDownloadQueue() {
    // For simplicity, we'll save as a JSON string
    // In production, consider using a more efficient binary format
    
    String json = "[";
    for (size_t i = 0; i < downloadQueue.size(); i++) {
        if (i > 0) json += ",";
        const auto& task = downloadQueue[i];
        json += "{\"url\":\"" + task.url + "\",";
        json += "\"path\":\"" + task.localPath + "\",";
        json += "\"retries\":" + String(task.retryCount) + ",";
        json += "\"retryBatch\":" + String(task.retryBatch) + ",";
        json += "\"completed\":" + String(task.completed ? "true" : "false") + ",";
        json += "\"lastAttempt\":" + String(task.lastAttempt) + ",";
        json += "\"lastBatchAttempt\":" + String(task.lastBatchAttempt) + ",";
        json += "\"checksum\":\"" + task.checksum + "\"}";
    }
    json += "]";
    
    esp_err_t err = nvs_set_str(nvsHandle, NVS_DOWNLOAD_QUEUE_KEY, json.c_str());
    if (err != ESP_OK) {
        Serial.printf("FileManager: Failed to save download queue: %s\n", esp_err_to_name(err));
        return false;
    }
    
    nvs_commit(nvsHandle);
    return true;
}

bool FileManager::loadDownloadQueue() {
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(nvsHandle, NVS_DOWNLOAD_QUEUE_KEY, NULL, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // No saved queue, start fresh
        return true;
    }
    
    if (err != ESP_OK) {
        Serial.printf("FileManager: Failed to get download queue size: %s\n", esp_err_to_name(err));
        return false;
    }
    
    char* json_str = (char*)malloc(required_size);
    err = nvs_get_str(nvsHandle, NVS_DOWNLOAD_QUEUE_KEY, json_str, &required_size);
    
    if (err != ESP_OK) {
        Serial.printf("FileManager: Failed to load download queue: %s\n", esp_err_to_name(err));
        free(json_str);
        return false;
    }
    
    // Parse JSON and populate download queue
    // This is a simplified parser - in production, use ArduinoJson
    downloadQueue.clear();
    
    free(json_str);
    return true;
}

bool FileManager::saveRequiredFiles() {
    // Save required files as JSON string, similar to download queue
    String json = "[";
    for (size_t i = 0; i < requiredFiles.size(); i++) {
        if (i > 0) json += ",";
        const auto& file = requiredFiles[i];
        json += "{\"path\":\"" + file.path + "\",";
        json += "\"url\":\"" + file.url + "\",";
        json += "\"required\":" + String(file.required ? "true" : "false") + ",";
        json += "\"checksum\":\"" + file.checksum + "\"}";
    }
    json += "]";
    
    esp_err_t err = nvs_set_str(nvsHandle, NVS_FILE_LIST_KEY, json.c_str());
    if (err != ESP_OK) {
        Serial.printf("FileManager: Failed to save required files: %s\n", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_commit(nvsHandle);
    if (err != ESP_OK) {
        Serial.printf("FileManager: Failed to commit NVS: %s\n", esp_err_to_name(err));
        return false;
    }
    
    Serial.printf("FileManager: Saved %d required files to NVS\n", requiredFiles.size());
    return true;
}

bool FileManager::loadRequiredFiles() {
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(nvsHandle, NVS_FILE_LIST_KEY, NULL, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // No saved required files, start fresh
        Serial.println("FileManager: No saved required files found");
        return true;
    }
    
    if (err != ESP_OK) {
        Serial.printf("FileManager: Failed to get required files size: %s\n", esp_err_to_name(err));
        return false;
    }
    
    char* json_str = (char*)malloc(required_size);
    if (!json_str) {
        Serial.println("FileManager: Failed to allocate memory for required files");
        return false;
    }
    
    err = nvs_get_str(nvsHandle, NVS_FILE_LIST_KEY, json_str, &required_size);
    
    if (err != ESP_OK) {
        Serial.printf("FileManager: Failed to load required files: %s\n", esp_err_to_name(err));
        free(json_str);
        return false;
    }
    
    // Parse JSON and populate required files
    // This is a simplified parser - in production, use ArduinoJson
    requiredFiles.clear();
    
    String json(json_str);
    free(json_str);
    
    // Simple JSON parsing for required files
    int startPos = 0;
    int braceCount = 0;
    bool inString = false;
    char prevChar = 0;
    
    for (int i = 0; i < json.length(); i++) {
        char c = json.charAt(i);
        
        if (c == '"' && prevChar != '\\') {
            inString = !inString;
        } else if (!inString) {
            if (c == '{') {
                if (braceCount == 0) startPos = i;
                braceCount++;
            } else if (c == '}') {
                braceCount--;
                if (braceCount == 0) {
                    // Found complete object, parse it
                    String objStr = json.substring(startPos, i + 1);
                    
                    // Extract fields using simple string operations
                    FileEntry entry;
                    
                    // Extract path
                    int pathStart = objStr.indexOf("\"path\":\"") + 8;
                    int pathEnd = objStr.indexOf("\"", pathStart);
                    if (pathStart > 7 && pathEnd > pathStart) {
                        entry.path = objStr.substring(pathStart, pathEnd);
                    }
                    
                    // Extract url
                    int urlStart = objStr.indexOf("\"url\":\"") + 7;
                    int urlEnd = objStr.indexOf("\"", urlStart);
                    if (urlStart > 6 && urlEnd > urlStart) {
                        entry.url = objStr.substring(urlStart, urlEnd);
                    }
                    
                    // Extract checksum
                    int checksumStart = objStr.indexOf("\"checksum\":\"") + 12;
                    int checksumEnd = objStr.indexOf("\"", checksumStart);
                    if (checksumStart > 11 && checksumEnd > checksumStart) {
                        entry.checksum = objStr.substring(checksumStart, checksumEnd);
                    }
                    
                    // Set required to true (we only store required files)
                    entry.required = true;
                    
                    if (!entry.path.isEmpty() && !entry.url.isEmpty()) {
                        requiredFiles.push_back(entry);
                    }
                }
            }
        }
        prevChar = c;
    }
    
    Serial.printf("FileManager: Loaded %d required files from NVS\n", requiredFiles.size());
    return true;
}

bool FileManager::saveDownloadStats() {
    esp_err_t err = nvs_set_blob(nvsHandle, NVS_DOWNLOAD_STATS_KEY, &downloadStats, sizeof(downloadStats));
    if (err != ESP_OK) {
        Serial.printf("FileManager: Failed to save download stats: %s\n", esp_err_to_name(err));
        return false;
    }
    
    nvs_commit(nvsHandle);
    return true;
}

bool FileManager::loadDownloadStats() {
    size_t required_size = sizeof(downloadStats);
    esp_err_t err = nvs_get_blob(nvsHandle, NVS_DOWNLOAD_STATS_KEY, &downloadStats, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // No saved stats, start with zeros (already initialized in constructor)
        return true;
    }
    
    if (err != ESP_OK) {
        Serial.printf("FileManager: Failed to load download stats: %s\n", esp_err_to_name(err));
        return false;
    }
    
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
        if (!task.completed) {
            task.retryCount = 0; // Reset retry count within current batch
            task.retryBatch = 0; // Reset to first batch
            task.lastAttempt = 0; // Reset last attempt time
            task.lastBatchAttempt = 0; // Reset batch attempt time
        }
    }
    saveDownloadQueue();
    Serial.println("FileManager: All failed downloads reset for retry");
}

int FileManager::getPendingDownloadsCount() {
    int count = 0;
    for (const auto& task : downloadQueue) {
        if (!task.completed) {
            count++;
        }
    }
    return count;
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

// ===== Async Download Callbacks =====

void FileManager::onAsyncConnect(void* arg, AsyncClient* client) {
    FileManager* self = static_cast<FileManager*>(arg);
    
    self->asyncState.connectTime = millis() - self->asyncState.startTime;
    Serial.printf("FileManager: ⚡ Connected in %lu ms\n", self->asyncState.connectTime);
    
    // Build HTTP request - path is temporarily stored in checksum field
    String request = "GET " + self->asyncState.checksum + " HTTP/1.1\r\n";
    
    // Extract hostname from URL for Host header
    String hostname = self->asyncState.url;
    if (hostname.startsWith("http://")) {
        hostname = hostname.substring(7);
        int slashPos = hostname.indexOf('/');
        if (slashPos != -1) {
            hostname = hostname.substring(0, slashPos);
        }
    }
    
    request += "Host: " + hostname + "\r\n";
    request += "Connection: close\r\n";
    request += "User-Agent: ESP32-FileManager-Async/1.0\r\n";
    request += "\r\n";
    
    // Send HTTP request
    client->write(request.c_str(), request.length());
    
    // Clear checksum field (was temporary storage for path)
    self->asyncState.checksum = "";
    self->asyncState.lastDataTime = millis();
}

void FileManager::onAsyncData(void* arg, AsyncClient* client, void* data, size_t len) {
    FileManager* self = static_cast<FileManager*>(arg);
    
    unsigned long dataCallbackStart = millis();
    self->asyncState.lastDataTime = dataCallbackStart;
    
    if (!self->asyncState.headersParsed) {
        // Parse headers
        unsigned long headerParseStart = millis();
        const char* buf = static_cast<const char*>(data);
        
        // Look for Content-Length
        const char* contentLengthPtr = strstr(buf, "Content-Length: ");
        if (!contentLengthPtr) {
            contentLengthPtr = strstr(buf, "content-length: ");
        }
        if (contentLengthPtr) {
            self->asyncState.contentLength = atoi(contentLengthPtr + 16);
            Serial.printf("FileManager: Content-Length: %d bytes\n", self->asyncState.contentLength);
            
            // Check available space
            size_t freeSpace = self->getSDCardFreeSpace();
            if (self->asyncState.contentLength > 0 && (size_t)self->asyncState.contentLength > freeSpace) {
                Serial.println("FileManager: Insufficient SD card space");
                self->cleanupAsyncDownload(false, "Insufficient SD card space");
                return;
            }
            
            // Note: Pre-allocation was tested but caused write slowdown due to FATFS overhead
            // Sequential writes without pre-allocation are actually faster with our buffering strategy
        }
        
        // Find end of headers
        const char* headerEnd = strstr(buf, "\r\n\r\n");
        if (headerEnd) {
            self->asyncState.headersParsed = true;
            
            // Calculate data bytes after headers in this packet
            size_t headerSize = (headerEnd + 4) - buf;
            if (len > headerSize) {
                size_t dataLen = len - headerSize;
                const uint8_t* bodyData = (const uint8_t*)data + headerSize;
                
                // Buffer the data
                if (self->asyncState.bufferAUsed + dataLen <= self->asyncState.bufferSize) {
                    memcpy(self->asyncState.bufferA + self->asyncState.bufferAUsed, bodyData, dataLen);
                    self->asyncState.bufferAUsed += dataLen;
                    self->asyncState.totalDownloaded += dataLen;
                } else {
                    // Swap buffers first
                    self->swapBuffers();
                    
                    // Now buffer the data
                    memcpy(self->asyncState.bufferA, bodyData, dataLen);
                    self->asyncState.bufferAUsed = dataLen;
                    self->asyncState.totalDownloaded += dataLen;
                }
            }
            
            self->asyncState.headerParseTime = millis() - headerParseStart;
            Serial.printf("FileManager: ⚡ Headers parsed in %lu ms, starting data transfer\n", 
                         self->asyncState.headerParseTime);
        }
        return;
    }
    
    // Data phase - buffer incoming data in bufferA
    const uint8_t* incomingData = static_cast<const uint8_t*>(data);
    size_t remaining = len;
    
    while (remaining > 0) {
        size_t spaceLeft = self->asyncState.bufferSize - self->asyncState.bufferAUsed;
        
        if (spaceLeft == 0) {
            // BufferA full, request swap with bufferB
            self->swapBuffers();
            spaceLeft = self->asyncState.bufferSize;
        }
        
        size_t toCopy = min(remaining, spaceLeft);
        memcpy(self->asyncState.bufferA + self->asyncState.bufferAUsed, 
               incomingData + (len - remaining), toCopy);
        self->asyncState.bufferAUsed += toCopy;
        remaining -= toCopy;
    }
    
    self->asyncState.totalDownloaded += len;
    
    // Calculate callback processing time
    unsigned long callbackDuration = millis() - dataCallbackStart;
    if (callbackDuration > 100) {
        Serial.printf("⚠️  Slow onData callback: %lu ms (received %d bytes)\n", 
                     callbackDuration, len);
    }
    
    // Report progress every 20%
    if (self->asyncState.contentLength > 0) {
        int progress = (self->asyncState.totalDownloaded * 100) / self->asyncState.contentLength;
        if (progress >= self->asyncState.lastReportedProgress + 20) {
            self->asyncState.lastReportedProgress = progress;
            
            unsigned long elapsed = millis() - self->asyncState.startTime;
            float currentSpeed = elapsed > 0 ? (self->asyncState.totalDownloaded * 8.0f) / (elapsed / 1000.0f) / 1000.0f : 0;
            
            Serial.printf("📥 Progress: %d%% (%d/%d bytes) - %.2f Mbps - %d SD writes\n", 
                         progress, self->asyncState.totalDownloaded, self->asyncState.contentLength, 
                         currentSpeed / 1000.0f, self->asyncState.writeCount);
            
            if (self->downloadProgressCallback) {
                self->downloadProgressCallback(self->asyncState.url, self->asyncState.localPath, 
                                             progress, self->asyncState.totalDownloaded, 
                                             self->asyncState.contentLength);
            }
        }
    }
}

void FileManager::onAsyncDisconnect(void* arg, AsyncClient* client) {
    FileManager* self = static_cast<FileManager*>(arg);
    
    // Final buffer swap to ensure all data is written
    if (self->asyncState.bufferAUsed > 0) {
        self->swapBuffers();
    }
    
    // Stop write task
    if (self->asyncState.writeTaskHandle) {
        self->asyncState.writeTaskRunning = false;
        xSemaphoreGive(self->asyncState.bufferSwapSemaphore);  // Wake up task to exit
        vTaskDelay(pdMS_TO_TICKS(100));  // Give task time to finish
    }
    
    // Check if download was successful
    bool success = true;
    String errorMsg = "";
    
    // Allow size difference up to buffer size (data might be in flight)
    if (self->asyncState.contentLength > 0) {
        int sizeDiff = abs(self->asyncState.contentLength - self->asyncState.totalDownloaded);
        if (sizeDiff > (int)self->asyncState.bufferSize) {
            success = false;
            errorMsg = "Download size mismatch: got " + String(self->asyncState.totalDownloaded) + 
                      " expected " + String(self->asyncState.contentLength) + 
                      " (diff: " + String(sizeDiff) + " bytes)";
            Serial.printf("FileManager: %s\n", errorMsg.c_str());
        } else if (sizeDiff > 0) {
            Serial.printf("FileManager: Small size difference: %d bytes (within tolerance)\n", sizeDiff);
        }
    }
    
    if (success && self->asyncState.totalDownloaded > 0) {
        // Print performance summary
        unsigned long totalTime = millis() - self->asyncState.startTime;
        float totalSec = totalTime / 1000.0f;
        float avgSpeed = totalSec > 0 ? (self->asyncState.totalDownloaded * 8.0f) / (totalSec * 1000.0f) : 0;
        
        Serial.println("\n📊 Download Performance Summary:");
        Serial.printf("   ⏱️  Total: %.2f sec (%.2f Mbps avg)\n", totalSec, avgSpeed / 1000.0f);
        Serial.printf("   📦 Downloaded: %.2f MB (%d bytes)\n",
                     self->asyncState.totalDownloaded / 1048576.0f,
                     self->asyncState.totalDownloaded);
        Serial.printf("   💾 SD writes: %d writes, %.1f sec total (%.1f%% of time)\n",
                     self->asyncState.writeCount,
                     self->asyncState.totalWriteTime / 1000.0f,
                     (float)self->asyncState.totalWriteTime / totalTime * 100);
        Serial.printf("   🌐 Network: %.1f sec (%.1f%% of time)\n",
                     (totalTime - self->asyncState.totalWriteTime) / 1000.0f,
                     (float)(totalTime - self->asyncState.totalWriteTime) / totalTime * 100);
        Serial.println();
        
        // Close temp file
        self->asyncState.file.close();
        
        // Move temp file to final location
        if (SD_MMC.exists(self->asyncState.localPath)) {
            SD_MMC.remove(self->asyncState.localPath);
        }
        
        if (!SD_MMC.rename(self->asyncState.tempPath, self->asyncState.localPath)) {
            success = false;
            errorMsg = "Failed to rename temp file";
        } else if (!SD_MMC.exists(self->asyncState.localPath)) {
            success = false;
            errorMsg = "File verification failed";
        }
    } else if (self->asyncState.totalDownloaded == 0) {
        success = false;
        errorMsg = "No data received";
    }
    
    self->cleanupAsyncDownload(success, errorMsg);
}

void FileManager::onAsyncError(void* arg, AsyncClient* client, int8_t error) {
    FileManager* self = static_cast<FileManager*>(arg);
    
    String errorMsg = "AsyncTCP error: " + String(error);
    Serial.println("FileManager: " + errorMsg);
    
    self->cleanupAsyncDownload(false, errorMsg);
}

void FileManager::onAsyncTimeout(void* arg, AsyncClient* client, uint32_t time) {
    FileManager* self = static_cast<FileManager*>(arg);
    
    Serial.printf("FileManager: Timeout after %u ms\n", time);
    self->cleanupAsyncDownload(false, "Download timeout");
}

// Buffer swap function - called from onAsyncData when bufferA is full
void FileManager::swapBuffers() {
    unsigned long swapStart = millis();
    
    // Wait for previous write to complete (should be instant if write task is fast)
    if (xSemaphoreTake(asyncState.writeCompleteSemaphore, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("⚠️  Write task blocked - timeout waiting for previous write!");
        return;
    }
    
    // Now safe to swap - write task is idle
    asyncState.swapRequested = true;
    
    // Swap the pointers immediately (write task is waiting)
    uint8_t* temp = asyncState.bufferB;
    asyncState.bufferB = asyncState.bufferA;
    asyncState.bufferA = temp;
    
    size_t bytesToWrite = asyncState.bufferAUsed;  // Old bufferA, now in bufferB
    asyncState.bufferBUsed = bytesToWrite;
    asyncState.bufferAUsed = 0;  // Reset for new network data
    
    asyncState.swapRequested = false;
    
    // Signal write task to start writing bufferB
    xSemaphoreGive(asyncState.bufferSwapSemaphore);
    
    unsigned long swapTime = millis() - swapStart;
    if (swapTime > 5) {
        Serial.printf("⚠️  Buffer swap delayed: %lu ms (write task busy)\n", swapTime);
    }
}

// FreeRTOS task that runs on Core 0 and writes buffers to SD card
void FileManager::sdWriteTask(void* parameter) {
    FileManager* self = static_cast<FileManager*>(parameter);
    
    Serial.println("[WriteTask] Started on Core 0");
    
    while (self->asyncState.writeTaskRunning) {
        // Wait for buffer swap signal from network task
        if (xSemaphoreTake(self->asyncState.bufferSwapSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
            
            if (!self->asyncState.writeTaskRunning) break;  // Exit signal
            
            // Pointers already swapped by swapBuffers(), just write bufferB
            size_t bytesToWrite = self->asyncState.bufferBUsed;
            
            if (bytesToWrite > 0) {
                unsigned long writeStart = micros();
                size_t written = self->asyncState.file.write(self->asyncState.bufferB, bytesToWrite);
                unsigned long writeTime = micros() - writeStart;
                
                self->asyncState.totalWriteTime += (writeTime / 1000);  // Convert to ms
                self->asyncState.writeCount++;
                
                // Only log every 50 writes to reduce spam
                if (self->asyncState.writeCount % 50 == 0) {
                    float writeTimeMs = writeTime / 1000.0f;
                    float speedMbps = (bytesToWrite / 1024.0f) / (writeTimeMs / 1000.0f) * 8.0f / 1024.0f;
                    Serial.printf("[SD Write #%d] %.2f ms (%.1f Mbps) | %lu bytes total\n",
                                 self->asyncState.writeCount, writeTimeMs, speedMbps,
                                 self->asyncState.file.position());
                }
                
                if (written != bytesToWrite) {
                    Serial.printf("❌ SD Write #%d failed: %d/%d bytes\n", 
                                 self->asyncState.writeCount, written, bytesToWrite);
                }
                
                self->asyncState.bufferBUsed = 0;
            }
            
            // Signal that write is complete - network can proceed
            xSemaphoreGive(self->asyncState.writeCompleteSemaphore);
        }
    }
    
    Serial.println("[WriteTask] Exiting");
    vTaskDelete(NULL);
}

void FileManager::cleanupAsyncDownload(bool success, const String& errorMsg) {
    // Stop write task if running
    if (asyncState.writeTaskHandle) {
        asyncState.writeTaskRunning = false;
        xSemaphoreGive(asyncState.bufferSwapSemaphore);  // Wake task to exit
        vTaskDelay(pdMS_TO_TICKS(200));  // Wait for task to finish
        asyncState.writeTaskHandle = nullptr;
    }
    
    // Delete semaphores
    if (asyncState.bufferSwapSemaphore) {
        vSemaphoreDelete(asyncState.bufferSwapSemaphore);
        asyncState.bufferSwapSemaphore = nullptr;
    }
    if (asyncState.writeCompleteSemaphore) {
        vSemaphoreDelete(asyncState.writeCompleteSemaphore);
        asyncState.writeCompleteSemaphore = nullptr;
    }
    
    // Close file
    if (asyncState.file) {
        asyncState.file.close();
    }
    
    // Free buffers
    if (asyncState.bufferA) {
        free(asyncState.bufferA);
        asyncState.bufferA = nullptr;
    }
    if (asyncState.bufferB) {
        free(asyncState.bufferB);
        asyncState.bufferB = nullptr;
    }
    
    // Clean up client
    if (asyncState.client) {
        asyncState.client->close(true);
        delete asyncState.client;
        asyncState.client = nullptr;
    }
    
    // Remove temp file on failure
    if (!success && SD_MMC.exists(asyncState.tempPath)) {
        SD_MMC.remove(asyncState.tempPath);
    }
    
    // Update statistics
    downloadStats.totalDownloads++;
    if (success) {
        downloadStats.successfulDownloads++;
        downloadStats.totalBytesDownloaded += asyncState.totalDownloaded;
        
        Serial.printf("Download completed: %s (%d bytes)\n", 
                     asyncState.localPath.substring(asyncState.localPath.lastIndexOf('/') + 1).c_str(),
                     asyncState.totalDownloaded);
        
        if (downloadCompleteCallback) {
            downloadCompleteCallback(asyncState.url, asyncState.localPath, true, "");
        }
    } else {
        downloadStats.failedDownloads++;
        
        Serial.printf("Download failed: %s\n", errorMsg.c_str());
        
        if (downloadCompleteCallback) {
            downloadCompleteCallback(asyncState.url, asyncState.localPath, false, errorMsg);
        }
    }
    
    saveDownloadStats();
    
    // Reset state
    downloadInProgress = false;
    asyncState.reset();
}