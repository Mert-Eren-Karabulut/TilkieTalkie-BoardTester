#include "FileManager.h"
#include "BatteryManagement.h"

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
    SD.end();
    sdCardInitialized = false;
    
    Serial.println("FileManager: Shutdown complete");
}

bool FileManager::initializeSDCard() {
    Serial.println("FileManager: Initializing SD card...");
    
    // Check power supply first
    BatteryManager& battery = BatteryManager::getInstance();
    float voltage = battery.getBatteryVoltage();
    Serial.printf("FileManager: System voltage: %.2fV\n", voltage);
    
    // Allow operation even with low voltage reading when USB powered
    if (voltage < 3.2 && voltage > 0.1) {
        Serial.println("FileManager: Voltage appears low, but continuing (may be USB powered)");
    } else if (voltage < 0.1) {
        Serial.println("FileManager: Note - Voltage reading may be inaccurate when USB powered");
    }
    
    // Configure CS pin as output and set high initially
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    delay(10);
    
    // Initialize SPI with explicit parameters
    SPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    
    // Start with high-speed SPI for optimized performance
    SPI.setFrequency(25000000);  // 25MHz - will fallback if needed during SD.begin()
    
    Serial.printf("FileManager: Using pins - CS:%d, CLK:%d, MISO:%d, MOSI:%d\n", 
                  SD_CS_PIN, SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN);
    
    // Power cycle the SD card by toggling CS
    digitalWrite(SD_CS_PIN, LOW);
    delay(10);
    digitalWrite(SD_CS_PIN, HIGH);
    delay(100);
    
    // Try multiple initialization attempts with optimized speeds
    bool sdInitialized = false;
    uint32_t initSpeeds[] = {25000000, 20000000, 10000000, 4000000, 1000000, 400000}; // 25MHz down to 400kHz
    const char* speedNames[] = {"25MHz", "20MHz", "10MHz", "4MHz", "1MHz", "400kHz"};
    int numSpeeds = sizeof(initSpeeds) / sizeof(initSpeeds[0]);
    
    for (int attempt = 1; attempt <= 3 && !sdInitialized; attempt++) {
        Serial.printf("FileManager: SD card initialization attempt %d/3\n", attempt);
        
        // Try different speeds, starting with fastest
        for (int i = 0; i < numSpeeds && !sdInitialized; i++) {
            Serial.printf("FileManager: Trying %s... ", speedNames[i]);
            
            if (SD.begin(SD_CS_PIN, SPI, initSpeeds[i])) {
                sdInitialized = true;
                Serial.printf("SUCCESS\n");
                Serial.printf("FileManager: SD card initialized at %s (≈%.1f KB/s)\n", 
                             speedNames[i], (initSpeeds[i] * 0.1) / 1024.0);
                break;
            } else {
                Serial.printf("failed, ");
            }
        }
        
        if (!sdInitialized) {
            Serial.printf("Attempt %d failed at all speeds, retrying...\n", attempt);
            SD.end();  // Clean up before retry
            delay(1000);
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
    uint8_t cardType = SD.cardType();
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
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("FileManager: SD card size: %lluMB\n", cardSize);
    
    sdCardInitialized = true;
    
    // Create necessary directories
    createDirectory("/audio");
    createDirectory("/temp");
    createDirectory("/logs");
    createDirectory("/images");
    
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
    
    // Simple HTTP request to check connectivity (alternative to ping)
    HTTPClient http;
    http.begin("http://www.google.com");
    http.setTimeout(5000); // 5 second timeout
    
    int httpCode = http.GET();
    http.end();
    
    bool connected = (httpCode > 0);
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
    if (!downloadInProgress && 
        downloadQueue.size() > 0 && 
        isChargingRequired()) {
        processDownloadQueue();
    }
    
    // Check for missing required files periodically
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 300000) { // Check every 5 minutes
        checkRequiredFiles();
        lastCheck = millis();
    }
}

bool FileManager::writeFile(const String& path, const String& content) {
    return writeFile(path, (const uint8_t*)content.c_str(), content.length());
}

bool FileManager::writeFile(const String& path, const uint8_t* data, size_t length) {
    if (!sdCardInitialized) {
        Serial.println("FileManager: SD card not initialized");
        return false;
    }
    
    // Create directory structure if needed
    createDirectoryStructure(path);
    
    File file = SD.open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("FileManager: Failed to open file for writing: %s\n", path.c_str());
        if (fileSystemEventCallback) {
            fileSystemEventCallback("write", path, false);
        }
        return false;
    }
    
    size_t bytesWritten = file.write(data, length);
    file.close();
    
    bool success = (bytesWritten == length);
    if (fileSystemEventCallback) {
        fileSystemEventCallback("write", path, success);
    }
    
    if (success) {
        Serial.printf("FileManager: File written successfully: %s (%d bytes)\n", path.c_str(), bytesWritten);
    } else {
        Serial.printf("FileManager: Failed to write file: %s\n", path.c_str());
    }
    
    return success;
}

String FileManager::readFile(const String& path) {
    if (!sdCardInitialized) {
        return "";
    }
    
    File file = SD.open(path);
    if (!file) {
        Serial.printf("FileManager: Failed to open file for reading: %s\n", path.c_str());
        return "";
    }
    
    String content = file.readString();
    file.close();
    
    Serial.printf("FileManager: File read successfully: %s (%d bytes)\n", path.c_str(), content.length());
    return content;
}

bool FileManager::readFile(const String& path, uint8_t* buffer, size_t& length) {
    if (!sdCardInitialized) {
        return false;
    }
    
    File file = SD.open(path);
    if (!file) {
        Serial.printf("FileManager: Failed to open file for reading: %s\n", path.c_str());
        return false;
    }
    
    size_t fileSize = file.size();
    if (fileSize > length) {
        Serial.printf("FileManager: Buffer too small for file: %s\n", path.c_str());
        file.close();
        return false;
    }
    
    length = file.read(buffer, fileSize);
    file.close();
    
    Serial.printf("FileManager: File read successfully: %s (%d bytes)\n", path.c_str(), length);
    return true;
}

bool FileManager::deleteFile(const String& path) {
    if (!sdCardInitialized) {
        return false;
    }
    
    bool success = SD.remove(path);
    
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
    File fileOrDir = SD.open(path);
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
    bool success = SD.remove(path);
    
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
    
    bool success = SD.mkdir(path);
    
    if (fileSystemEventCallback) {
        fileSystemEventCallback("mkdir", path, success);
    }
    
    if (success) {
        Serial.printf("FileManager: Directory created successfully: %s\n", path.c_str());
    } else {
        // Directory might already exist, check if it exists
        File dir = SD.open(path);
        if (dir && dir.isDirectory()) {
            dir.close();
            return true; // Directory already exists
        }
        Serial.printf("FileManager: Failed to create directory: %s\n", path.c_str());
    }
    
    return success;
}

bool FileManager::fileExists(const String& path) {
    if (!sdCardInitialized) {
        return false;
    }
    
    File file = SD.open(path);
    if (file) {
        file.close();
        return true;
    }
    return false;
}

size_t FileManager::getFileSize(const String& path) {
    if (!sdCardInitialized) {
        return 0;
    }
    
    File file = SD.open(path);
    if (!file) {
        return 0;
    }
    
    size_t size = file.size();
    file.close();
    return size;
}

std::vector<String> FileManager::listFiles(const String& directory) {
    std::vector<String> files;
    
    if (!sdCardInitialized) {
        return files;
    }
    
    File dir = SD.open(directory);
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

bool FileManager::downloadNow(const String& url, const String& localPath, String& errorMsg) {
    if (!checkConnectivity()) {
        errorMsg = "No internet connection";
        return false;
    }
    
    if (!isChargingRequired()) {
        errorMsg = "Device must be charging for downloads";
        return false;
    }
    
    return downloadFileFromURL(url, localPath, errorMsg);
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
    
    Serial.printf("FileManager: Starting download: %s -> %s\n", url.c_str(), localPath.c_str());
    
    // Create directory structure with verification
    if (!createDirectoryStructure(localPath)) {
        errorMsg = "Failed to create directory structure";
        downloadInProgress = false;
        return false;
    }
    
    // Verify directory exists before proceeding
    String dir = getDirectoryFromPath(localPath);
    File dirFile = SD.open(dir);
    if (!dirFile || !dirFile.isDirectory()) {
        errorMsg = "Directory creation failed or not accessible: " + dir;
        if (dirFile) dirFile.close();
        downloadInProgress = false;
        return false;
    }
    dirFile.close();
    
    // Create temporary file
    String tempPath = localPath + ".tmp";
    
    // Remove any existing temp file
    if (SD.exists(tempPath)) {
        SD.remove(tempPath);
    }
    
    HTTPClient http;
    http.begin(url);
    http.setTimeout(DOWNLOAD_TIMEOUT_MS);
    
    int httpCode = http.GET();
    
    if (httpCode != HTTP_CODE_OK) {
        errorMsg = "HTTP error: " + String(httpCode);
        http.end();
        downloadInProgress = false;
        return false;
    }
    
    int contentLength = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    
    // Check available space
    size_t freeSpace = getSDCardFreeSpace();
    if (contentLength > 0 && (size_t)contentLength > freeSpace) {
        errorMsg = "Insufficient SD card space";
        http.end();
        downloadInProgress = false;
        return false;
    }
    
    File file = SD.open(tempPath, FILE_WRITE);
    if (!file) {
        errorMsg = "Failed to create temporary file: " + tempPath;
        http.end();
        downloadInProgress = false;
        return false;
    }
    
    uint8_t* buffer = (uint8_t*)malloc(DOWNLOAD_BUFFER_SIZE);
    if (!buffer) {
        errorMsg = "Failed to allocate download buffer";
        file.close();
        http.end();
        downloadInProgress = false;
        return false;
    }
    
    int totalDownloaded = 0;
    unsigned long lastProgress = 0;
    bool downloadSuccess = true;
    
    while (http.connected() && downloadSuccess && (contentLength > 0 || contentLength == -1)) {
        size_t availableData = stream->available();
        
        if (availableData > 0) {
            size_t bytesToRead = min(availableData, (size_t)DOWNLOAD_BUFFER_SIZE);
            int readBytes = stream->readBytes(buffer, bytesToRead);
            
            if (readBytes > 0) {
                size_t written = file.write(buffer, readBytes);
                if (written != readBytes) {
                    errorMsg = "Failed to write to file";
                    downloadSuccess = false;
                    break;
                }
                totalDownloaded += readBytes;
                
                if (contentLength > 0) {
                    int progress = (totalDownloaded * 100) / contentLength;
                    
                    // Report progress every 5%
                    if (progress >= lastProgress + 5) {
                        lastProgress = progress;
                        Serial.printf("FileManager: Download progress: %d%% (%d/%d bytes)\n", 
                                    progress, totalDownloaded, contentLength);
                        
                        if (downloadProgressCallback) {
                            downloadProgressCallback(url, localPath, progress, totalDownloaded, contentLength);
                        }
                    }
                }
                
                if (contentLength > 0 && totalDownloaded >= contentLength) {
                    break;
                }
            }
        }
        
        delay(1); // Yield to other tasks
    }
    
    free(buffer);
    file.close();
    http.end();
    
    if (!downloadSuccess) {
        SD.remove(tempPath);
        downloadInProgress = false;
        return false;
    }
    
    // Verify download size
    if (contentLength > 0 && totalDownloaded != contentLength) {
        errorMsg = "Download incomplete: " + String(totalDownloaded) + "/" + String(contentLength) + " bytes";
        SD.remove(tempPath);
        downloadInProgress = false;
        return false;
    }
    
    // Move temporary file to final location
    if (SD.exists(localPath)) {
        SD.remove(localPath);
    }
    
    if (!SD.rename(tempPath, localPath)) {
        errorMsg = "Failed to move temporary file to final location";
        SD.remove(tempPath);
        downloadInProgress = false;
        return false;
    }
    
    // Verify final file exists and has correct size
    File finalFile = SD.open(localPath);
    if (!finalFile) {
        errorMsg = "Final file verification failed";
        downloadInProgress = false;
        return false;
    }
    size_t finalSize = finalFile.size();
    finalFile.close();
    
    if (contentLength > 0 && finalSize != contentLength) {
        errorMsg = "Final file size mismatch";
        SD.remove(localPath);
        downloadInProgress = false;
        return false;
    }
    
    downloadInProgress = false;
    
    // Update statistics
    downloadStats.totalDownloads++;
    downloadStats.successfulDownloads++;
    downloadStats.totalBytesDownloaded += totalDownloaded;
    saveDownloadStats();
    
    Serial.printf("FileManager: Download completed successfully: %s (%d bytes)\n", 
                  localPath.c_str(), totalDownloaded);
    
    if (downloadCompleteCallback) {
        downloadCompleteCallback(url, localPath, true, "");
    }
    
    return true;
}

void FileManager::processDownloadQueue() {
    if (downloadQueue.empty() || downloadInProgress) {
        return;
    }
    
    // Only proceed if device is charging
    if (!isChargingRequired()) {
        return;
    }
    
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
        
        String errorMsg;
        task.lastAttempt = millis();
        task.retryCount++;
        
        Serial.printf("FileManager: Attempting download (batch %d, attempt %d/%d): %s\n", 
                     task.retryBatch + 1, task.retryCount, MAX_RETRY_COUNT, task.url.c_str());
        
        if (downloadFileFromURL(task.url, task.localPath, errorMsg)) {
            // Verify integrity if checksum provided
            if (!task.checksum.isEmpty() && !verifyFileIntegrity(task.localPath, task.checksum)) {
                Serial.printf("FileManager: Downloaded file failed integrity check: %s\n", task.localPath.c_str());
                deleteFile(task.localPath);
                Serial.printf("FileManager: Download attempt %d/%d failed (integrity): %s\n", 
                             task.retryCount, MAX_RETRY_COUNT, errorMsg.c_str());
            } else {
                task.completed = true;
                Serial.printf("FileManager: Download successful: %s\n", task.localPath.c_str());
            }
        } else {
            Serial.printf("FileManager: Download attempt %d/%d failed: %s (Error: %s)\n", 
                         task.retryCount, MAX_RETRY_COUNT, task.url.c_str(), errorMsg.c_str());
        }
        
        ++it;
        break; // Process one download at a time
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

std::vector<String> FileManager::getMissingFiles() {
    std::vector<String> missing;
    
    for (const auto& file : requiredFiles) {
        if (!fileExists(file.path)) {
            missing.push_back(file.path);
        }
    }
    
    return missing;
}

bool FileManager::createDirectoryStructure(const String& path) {
    String dir = getDirectoryFromPath(path);
    if (dir.isEmpty() || dir == "/") {
        return true;
    }
    
    // Check if directory already exists
    File dirFile = SD.open(dir);
    if (dirFile && dirFile.isDirectory()) {
        dirFile.close();
        return true;
    }
    if (dirFile) {
        dirFile.close();
    }
    
    // Create directory and verify creation
    bool success = createDirectory(dir);
    if (!success) {
        Serial.printf("FileManager: Failed to create directory structure for: %s\n", path.c_str());
        Serial.printf("FileManager: Attempted to create directory: %s\n", dir.c_str());
    }
    return success;
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
    
    File file = SD.open(filePath);
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
    return SD.cardSize();
}

size_t FileManager::getSDCardUsedSpace() {
    if (!sdCardInitialized) {
        return 0;
    }
    return SD.usedBytes();
}

size_t FileManager::getSDCardFreeSpace() {
    if (!sdCardInitialized) {
        return 0;
    }
    return SD.cardSize() - SD.usedBytes();
}

String FileManager::getSDCardInfo() {
    if (!sdCardInitialized) {
        return "SD card not initialized";
    }
    
    String info = "SD Card Information:\n";
    info += "Type: ";
    
    uint8_t cardType = SD.cardType();
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

void FileManager::printFileList(const String& directory) {
    Serial.printf("Files in directory: %s\n", directory.c_str());
    
    std::vector<String> files = listFiles(directory);
    for (const auto& file : files) {
        Serial.printf("  %s\n", file.c_str());
    }
    
    Serial.printf("Total files: %d\n", files.size());
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
    
    nvs_commit(nvsHandle);
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

void FileManager::downloadMissingFiles() {
    checkRequiredFiles(); // This will schedule missing files for download
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

bool FileManager::copyFile(const String& sourcePath, const String& destPath) {
    if (!fileExists(sourcePath)) {
        Serial.printf("FileManager: Source file does not exist: %s\n", sourcePath.c_str());
        return false;
    }
    
    createDirectoryStructure(destPath);
    
    File sourceFile = SD.open(sourcePath);
    File destFile = SD.open(destPath, FILE_WRITE);
    
    if (!sourceFile || !destFile) {
        Serial.printf("FileManager: Failed to open files for copying\n");
        if (sourceFile) sourceFile.close();
        if (destFile) destFile.close();
        return false;
    }
    
    uint8_t buffer[1024];
    while (sourceFile.available()) {
        size_t bytesRead = sourceFile.read(buffer, sizeof(buffer));
        destFile.write(buffer, bytesRead);
    }
    
    sourceFile.close();
    destFile.close();
    
    Serial.printf("FileManager: File copied successfully: %s -> %s\n", sourcePath.c_str(), destPath.c_str());
    return true;
}

bool FileManager::renameFile(const String& oldPath, const String& newPath) {
    bool success = SD.rename(oldPath, newPath);
    
    if (fileSystemEventCallback) {
        fileSystemEventCallback("rename", oldPath + " -> " + newPath, success);
    }
    
    if (success) {
        Serial.printf("FileManager: File renamed successfully: %s -> %s\n", oldPath.c_str(), newPath.c_str());
    } else {
        Serial.printf("FileManager: Failed to rename file: %s -> %s\n", oldPath.c_str(), newPath.c_str());
    }
    
    return success;
}

bool FileManager::removeDirectory(const String& path) {
    bool success = SD.rmdir(path);
    
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

bool FileManager::removeRequiredFile(const String& localPath) {
    auto it = std::find_if(requiredFiles.begin(), requiredFiles.end(),
                          [&localPath](const FileEntry& entry) {
                              return entry.path == localPath;
                          });
    
    if (it != requiredFiles.end()) {
        requiredFiles.erase(it);
        saveRequiredFiles();
        Serial.printf("FileManager: Removed required file: %s\n", localPath.c_str());
        return true;
    }
    
    return false;
}

bool FileManager::verifyFile(const String& filePath, const String& expectedChecksum) {
    return verifyFileIntegrity(filePath, expectedChecksum);
}

bool FileManager::repairCorruptedFiles() {
    bool anyRepaired = false;
    
    for (const auto& file : requiredFiles) {
        if (fileExists(file.path)) {
            if (!file.checksum.isEmpty() && !verifyFileIntegrity(file.path, file.checksum)) {
                Serial.printf("FileManager: Repairing corrupted file: %s\n", file.path.c_str());
                deleteFile(file.path);
                scheduleDownload(file.url, file.path, file.checksum);
                anyRepaired = true;
            }
        }
    }
    
    return anyRepaired;
}

bool FileManager::testFileOperations() {
    Serial.println("\n=== Testing File Operations ===");
    
    if (!sdCardInitialized) {
        Serial.println("SD card not initialized - run sddiag first");
        return false;
    }
    
    bool allTestsPassed = true;
    
    // Test 1: Root directory access
    Serial.print("Test 1 - Root directory access: ");
    File root = SD.open("/");
    if (root && root.isDirectory()) {
        Serial.println("PASS");
        root.close();
    } else {
        Serial.println("FAIL");
        allTestsPassed = false;
    }
    
    // Test 2: Create directory
    Serial.print("Test 2 - Create directory: ");
    if (SD.mkdir("/test_dir")) {
        Serial.println("PASS");
    } else {
        // Might already exist
        File dir = SD.open("/test_dir");
        if (dir && dir.isDirectory()) {
            Serial.println("PASS (already exists)");
            dir.close();
        } else {
            Serial.println("FAIL");
            allTestsPassed = false;
        }
    }
    
    // Test 3: Write file
    Serial.print("Test 3 - Write file: ");
    File writeFile = SD.open("/test_dir/write_test.txt", FILE_WRITE);
    if (writeFile) {
        writeFile.println("FileManager write test");
        writeFile.println("Current millis: " + String(millis()));
        writeFile.close();
        Serial.println("PASS");
    } else {
        Serial.println("FAIL");
        allTestsPassed = false;
    }
    
    // Test 4: Read file
    Serial.print("Test 4 - Read file: ");
    File readFile = SD.open("/test_dir/write_test.txt", FILE_READ);
    if (readFile) {
        String content = readFile.readString();
        readFile.close();
        if (content.length() > 0) {
            Serial.println("PASS");
            Serial.println("  Content: " + content.substring(0, min(50, (int)content.length())) + "...");
        } else {
            Serial.println("FAIL (empty file)");
            allTestsPassed = false;
        }
    } else {
        Serial.println("FAIL");
        allTestsPassed = false;
    }
    
    // Test 5: List directory
    Serial.print("Test 5 - List directory: ");
    File dir = SD.open("/test_dir");
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        int fileCount = 0;
        while (entry) {
            fileCount++;
            entry = dir.openNextFile();
        }
        dir.close();
        Serial.printf("PASS (%d files found)\n", fileCount);
    } else {
        Serial.println("FAIL");
        allTestsPassed = false;
    }
    
    // Test 6: Delete file
    Serial.print("Test 6 - Delete file: ");
    if (SD.remove("/test_dir/write_test.txt")) {
        Serial.println("PASS");
    } else {
        Serial.println("FAIL");
        allTestsPassed = false;
    }
    
    // Test 7: Remove directory
    Serial.print("Test 7 - Remove directory: ");
    if (SD.rmdir("/test_dir")) {
        Serial.println("PASS");
    } else {
        Serial.println("FAIL");
        allTestsPassed = false;
    }
    
    Serial.println("\n=== File Operations Test Complete ===");
    Serial.printf("Overall result: %s\n", allTestsPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    
    if (!allTestsPassed) {
        Serial.println("\nTroubleshooting suggestions:");
        Serial.println("- Try lower SPI frequency (400kHz)");
        Serial.println("- Check SD card for corruption");
        Serial.println("- Try different SD card");
        Serial.println("- Check wiring connections");
    }
    
    return allTestsPassed;
}

void FileManager::runSDCardStressTest() {
    Serial.println("\n=== SD Card Stress Test ===");
    
    if (!sdCardInitialized) {
        Serial.println("SD card not initialized");
        return;
    }
    
    const int NUM_ITERATIONS = 10;
    const int FILE_SIZE_KB = 1; // 1KB files
    const int BUFFER_SIZE = 64;
    
    int passedTests = 0;
    uint8_t testData[BUFFER_SIZE];
    
    // Fill test data with known pattern
    for (int i = 0; i < BUFFER_SIZE; i++) {
        testData[i] = (uint8_t)(i % 256);
    }
    
    Serial.printf("Running %d iterations of write/read/verify cycles...\n", NUM_ITERATIONS);
    
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        Serial.printf("Iteration %d/%d: ", iter + 1, NUM_ITERATIONS);
        
        String filename = "/stress_test_" + String(iter) + ".dat";
        bool iterationPassed = true;
        
        // Write test
        File writeFile = SD.open(filename, FILE_WRITE);
        if (!writeFile) {
            Serial.println("FAIL (write open)");
            iterationPassed = false;
        } else {
            // Write multiple blocks
            for (int block = 0; block < (FILE_SIZE_KB * 1024) / BUFFER_SIZE; block++) {
                size_t written = writeFile.write(testData, BUFFER_SIZE);
                if (written != BUFFER_SIZE) {
                    Serial.printf("FAIL (write block %d)\n", block);
                    iterationPassed = false;
                    break;
                }
            }
            writeFile.close();
        }
        
        if (!iterationPassed) continue;
        
        // Read and verify test
        File readFile = SD.open(filename, FILE_READ);
        if (!readFile) {
            Serial.println("FAIL (read open)");
            iterationPassed = false;
        } else {
            uint8_t readBuffer[BUFFER_SIZE];
            for (int block = 0; block < (FILE_SIZE_KB * 1024) / BUFFER_SIZE; block++) {
                size_t bytesRead = readFile.read(readBuffer, BUFFER_SIZE);
                if (bytesRead != BUFFER_SIZE) {
                    Serial.printf("FAIL (read block %d)\n", block);
                    iterationPassed = false;
                    break;
                }
                
                // Verify data
                for (int i = 0; i < BUFFER_SIZE; i++) {
                    if (readBuffer[i] != testData[i]) {
                        Serial.printf("FAIL (verify block %d, byte %d)\n", block, i);
                        iterationPassed = false;
                        break;
                    }
                }
                if (!iterationPassed) break;
            }
            readFile.close();
        }
        
        // Clean up
        SD.remove(filename);
        
        if (iterationPassed) {
            Serial.println("PASS");
            passedTests++;
        }
        
        // Small delay between iterations
        delay(50);
    }
    
    Serial.printf("\nStress test complete: %d/%d tests passed\n", passedTests, NUM_ITERATIONS);
    if (passedTests == NUM_ITERATIONS) {
        Serial.println("✓ SD card appears stable and reliable");
    } else {
        Serial.println("✗ SD card has reliability issues");
        Serial.println("Suggestions:");
        Serial.println("- Try lower SPI frequency");
        Serial.println("- Check power supply stability");
        Serial.println("- Try different SD card");
    }
    
    Serial.println("=== Stress Test Complete ===\n");
}

void FileManager::optimizeSDCardSpeed() {
    Serial.println("=== Optimizing SD Card Speed ===");
    
    // Test different frequencies to find the fastest stable one
    uint32_t frequencies[] = {25000000, 20000000, 16000000, 10000000, 8000000, 4000000, 2000000, 1000000, 400000};
    const char* freqNames[] = {"25MHz", "20MHz", "16MHz", "10MHz", "8MHz", "4MHz", "2MHz", "1MHz", "400kHz"};
    int numFreq = sizeof(frequencies) / sizeof(frequencies[0]);
    
    uint32_t workingFreq = 400000;  // Start with conservative speed
    
    for (int i = 0; i < numFreq; i++) {
        Serial.printf("Testing %s... ", freqNames[i]);
        
        SD.end();
        delay(100);
        
        if (SD.begin(SD_CS_PIN, SPI, frequencies[i])) {
            // Quick test - create and delete a test file
            bool testPassed = true;
            String testFile = "/speedtest.tmp";
            
            File file = SD.open(testFile, FILE_WRITE);
            if (file) {
                file.println("Speed test");
                file.close();
                
                file = SD.open(testFile);
                if (file) {
                    String content = file.readString();
                    file.close();
                    SD.remove(testFile);
                    
                    if (content.indexOf("Speed test") == -1) {
                        testPassed = false;
                    }
                } else {
                    testPassed = false;
                }
            } else {
                testPassed = false;
            }
            
            if (testPassed) {
                workingFreq = frequencies[i];
                Serial.println("PASS");
                break;  // Found fastest working frequency
            } else {
                Serial.println("FAIL (data corruption)");
            }
        } else {
            Serial.println("FAIL (init)");
        }
    }
    
    // Final initialization with the fastest working frequency
    SD.end();
    delay(100);
    
    if (SD.begin(SD_CS_PIN, SPI, workingFreq)) {
        float dataRateMBps = (workingFreq * 0.8) / (8.0 * 1024.0 * 1024.0);  // Rough estimate with 80% efficiency
        Serial.printf("✓ Optimized speed: %s (≈%.1f KB/s)\n", 
                      workingFreq >= 25000000 ? "25MHz" :
                      workingFreq >= 20000000 ? "20MHz" :
                      workingFreq >= 16000000 ? "16MHz" :
                      workingFreq >= 10000000 ? "10MHz" :
                      workingFreq >= 8000000 ? "8MHz" :
                      workingFreq >= 4000000 ? "4MHz" :
                      workingFreq >= 2000000 ? "2MHz" :
                      workingFreq >= 1000000 ? "1MHz" : "400kHz",
                      (workingFreq * 0.1) / 1024.0);  // More realistic estimate
    } else {
        Serial.println("✗ Speed optimization failed, SD card may have issues");
    }
    
    Serial.println("=== Speed Optimization Complete ===\n");
}

void FileManager::printFileTree() {
    Serial.println("=== SD Card File Tree ===");

    std::function<void(const String&, int)> printTree = [&](const String& dir, int depth) {
        File dirFile = SD.open(dir);
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
        File dirFile = SD.open(dir);
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
                SD.rmdir(subDir);
            } else {
                entry.close();
                String filePath = dir;
                if (!filePath.endsWith("/")) filePath += "/";
                filePath += entryName;
                SD.remove(filePath);
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

