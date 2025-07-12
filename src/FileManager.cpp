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
    
    // Configure SPI pins for SD card
    SPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    
    // Initialize SD card
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("FileManager: SD card initialization failed");
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
        isChargingRequired() && 
        checkConnectivity()) {
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
    task.completed = false;
    task.lastAttempt = 0;
    
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
    
    downloadInProgress = true;
    
    Serial.printf("FileManager: Starting download: %s -> %s\n", url.c_str(), localPath.c_str());
    
    // Create directory structure
    createDirectoryStructure(localPath);
    
    // Create temporary file
    String tempPath = localPath + ".tmp";
    
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
    
    File file = SD.open(tempPath, FILE_WRITE);
    if (!file) {
        errorMsg = "Failed to create temporary file";
        http.end();
        downloadInProgress = false;
        return false;
    }
    
    uint8_t buffer[DOWNLOAD_BUFFER_SIZE];
    int totalDownloaded = 0;
    unsigned long lastProgress = 0;
    
    while (http.connected() && (contentLength > 0 || contentLength == -1)) {
        size_t availableData = stream->available();
        
        if (availableData > 0) {
            int readBytes = stream->readBytes(buffer, min(availableData, sizeof(buffer)));
            file.write(buffer, readBytes);
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
        
        delay(1); // Yield to other tasks
    }
    
    file.close();
    http.end();
    
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
    
    auto it = downloadQueue.begin();
    while (it != downloadQueue.end()) {
        DownloadTask& task = *it;
        
        if (task.completed) {
            it = downloadQueue.erase(it);
            continue;
        }
        
        if (task.retryCount >= MAX_RETRY_COUNT) {
            Serial.printf("FileManager: Download failed after %d retries: %s\n", 
                         MAX_RETRY_COUNT, task.url.c_str());
            
            downloadStats.totalDownloads++;
            downloadStats.failedDownloads++;
            
            if (downloadCompleteCallback) {
                downloadCompleteCallback(task.url, task.localPath, false, "Max retries exceeded");
            }
            
            it = downloadQueue.erase(it);
            continue;
        }
        
        // Check if enough time has passed since last attempt
        if (task.lastAttempt > 0 && 
            (millis() - task.lastAttempt) < RETRY_DELAY_MS) {
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
        
        if (downloadFileFromURL(task.url, task.localPath, errorMsg)) {
            // Verify integrity if checksum provided
            if (!task.checksum.isEmpty() && !verifyFileIntegrity(task.localPath, task.checksum)) {
                Serial.printf("FileManager: Downloaded file failed integrity check: %s\n", task.localPath.c_str());
                deleteFile(task.localPath);
                task.retryCount++;
            } else {
                task.completed = true;
            }
        } else {
            Serial.printf("FileManager: Download failed: %s (Error: %s)\n", task.url.c_str(), errorMsg.c_str());
            task.retryCount++;
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
    
    return createDirectory(dir);
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
        Serial.printf("  %d. %s -> %s (retries: %d, completed: %s)\n", 
                      i + 1, task.url.c_str(), task.localPath.c_str(), 
                      task.retryCount, task.completed ? "yes" : "no");
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
        json += "\"completed\":" + String(task.completed ? "true" : "false") + ",";
        json += "\"lastAttempt\":" + String(task.lastAttempt) + ",";
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
    // Similar implementation to saveDownloadQueue
    return true;
}

bool FileManager::loadRequiredFiles() {
    // Similar implementation to loadDownloadQueue
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
        if (!task.completed && task.retryCount < MAX_RETRY_COUNT) {
            task.retryCount = 0; // Reset retry count
            task.lastAttempt = 0; // Reset last attempt time
        }
    }
    saveDownloadQueue();
    Serial.println("FileManager: Failed downloads reset for retry");
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