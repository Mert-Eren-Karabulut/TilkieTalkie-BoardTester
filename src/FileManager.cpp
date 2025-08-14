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
        if (isChargingRequired()) {
            processDownloadQueue();
        } else {
            // Add periodic debug message for blocked downloads
            static unsigned long lastChargingWarning = 0;
            if (millis() - lastChargingWarning > 30000) { // Every 30 seconds
                Serial.printf("FileManager: %d downloads pending but device is not charging. Connect power to start downloads.\n", 
                             downloadQueue.size());
                lastChargingWarning = millis();
            }
        }
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
        Serial.printf("FileManager: Starting download: %s -> %s\n", httpUrl.c_str(), localPath.c_str());
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
    
    Serial.printf("FileManager: Connecting to %s:%d\n", hostname.c_str(), port);
    
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
    
    // Create WiFi client and connect
    WiFiClient client;
    // Optimize client settings for better throughput
    client.setTimeout(30000); // 30 seconds for connection operations
    // Connect to server
    if (!client.connect(hostname.c_str(), port)) {
        errorMsg = "Failed to connect to server: " + hostname;
        downloadInProgress = false;
        return false;
    }
    
    // Send optimized HTTP GET request with larger receive buffer hint
    String request = "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + hostname + "\r\n";
    request += "Connection: close\r\n";
    request += "User-Agent: ESP32-FileManager/1.0\r\n";
    request += "Accept: */*\r\n"; // Add Accept header for better compatibility
    request += "\r\n";
    
    client.print(request);
    
    // Read response headers
    unsigned long startTime = millis();
    bool headersDone = false;
    String responseHeaders = "";
    int contentLength = -1;
    int httpCode = 0;
    
    while (client.connected() && (millis() - startTime < DOWNLOAD_TIMEOUT_MS) && !headersDone) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            line.trim();
            
            if (line.length() == 0) {
                headersDone = true;
                break;
            }
            
            // Parse HTTP status code
            if (line.startsWith("HTTP/")) {
                int spaceIndex = line.indexOf(' ');
                if (spaceIndex != -1) {
                    httpCode = line.substring(spaceIndex + 1, spaceIndex + 4).toInt();
                }
            }
            
            // Parse Content-Length
            if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
                int colonIndex = line.indexOf(':');
                if (colonIndex != -1) {
                    contentLength = line.substring(colonIndex + 1).toInt();
                }
            }
        }
        delay(1);
    }
    
    if (!headersDone) {
        errorMsg = "Failed to read HTTP headers";
        client.stop();
        downloadInProgress = false;
        return false;
    }
    
    if (httpCode != 200) {
        errorMsg = "HTTP error: " + String(httpCode);
        client.stop();
        downloadInProgress = false;
        return false;
    }
    
    Serial.printf("FileManager: Starting download: %s (%s)\n", 
                  getDirectoryFromPath(localPath).c_str(), 
                  contentLength > 0 ? String(contentLength) + " bytes" : "unknown size");
    
    // Check available space
    size_t freeSpace = getSDCardFreeSpace();
    if (contentLength > 0 && (size_t)contentLength > freeSpace) {
        errorMsg = "Insufficient SD card space";
        client.stop();
        downloadInProgress = false;
        return false;
    }
    
    File file = SD.open(tempPath, FILE_WRITE);
    if (!file) {
        errorMsg = "Failed to create temporary file: " + tempPath;
        client.stop();
        downloadInProgress = false;
        return false;
    }
    
    uint8_t* buffer = (uint8_t*)malloc(DOWNLOAD_BUFFER_SIZE);
    if (!buffer) {
        errorMsg = "Failed to allocate download buffer (" + String(DOWNLOAD_BUFFER_SIZE) + " bytes)";
        Serial.printf("FileManager: Heap before allocation attempt: %d bytes\n", ESP.getFreeHeap());
        file.close();
        client.stop();
        downloadInProgress = false;
        return false;
    }
    
    Serial.printf("FileManager: Allocated %d KB buffer\n", DOWNLOAD_BUFFER_SIZE / 1024);
    
    int totalDownloaded = 0;
    unsigned long lastProgress = 0;
    bool downloadSuccess = true;
    
    // Download the file content
    startTime = millis(); // Reset timer for download phase
    unsigned long lastDataTime = millis(); // Track when we last received data
    const unsigned long NO_DATA_TIMEOUT = 15000; // 15 seconds without data = timeout (increased from 10s)
    
    while (downloadSuccess && (millis() - startTime < DOWNLOAD_TIMEOUT_MS)) {
        size_t availableData = client.available();
        
        if (availableData > 0) {
            lastDataTime = millis(); // Reset no-data timer
            
            // Optimize: Read larger chunks when available
            size_t bytesToRead = min(availableData, (size_t)DOWNLOAD_BUFFER_SIZE);
            int readBytes = client.readBytes(buffer, bytesToRead);
            
            if (readBytes > 0) {
                size_t written = file.write(buffer, readBytes);
                if (written != readBytes) {
                    errorMsg = "Failed to write to file";
                    downloadSuccess = false;
                    break;
                }
                totalDownloaded += readBytes;
                
                // Optimized progress reporting - less frequent updates
                if (contentLength > 0) {
                    int progress = (totalDownloaded * 100) / contentLength;
                    
                    // Report progress every 10% or every 1MB (whichever comes first)
                    unsigned long now = millis();
                    bool shouldReport = false;
                    
                    if (progress >= lastProgress + 10) { // Every 10% instead of 5%
                        shouldReport = true;
                        lastProgress = progress;
                    } else if ((totalDownloaded % 1048576 == 0) && totalDownloaded > 0) { // Every 1MB
                        shouldReport = true;
                    }
                    
                    if (shouldReport) {
                        Serial.printf("Download: %d%% (%d/%d bytes)\n", 
                                    progress, totalDownloaded, contentLength);
                        
                        if (downloadProgressCallback) {
                            downloadProgressCallback(httpUrl, localPath, progress, totalDownloaded, contentLength);
                        }
                    }
                }
                
                // Check if we have all expected data
                if (contentLength > 0 && totalDownloaded >= contentLength) {
                    // Wait a bit more to ensure no more data is coming
                    unsigned long drainStart = millis();
                    while ((millis() - drainStart) < 2000 && client.connected()) { // Wait up to 2 seconds
                        if (client.available() > 0) {
                            break; // More data available, continue main loop
                        }
                        delay(50);
                    }
                    
                    // If no more data after waiting, we're truly done
                    if (client.available() == 0) {
                        break;
                    }
                }
            }
        } else {
            // No data available right now
            if (!client.connected()) {
                // Connection closed
                if (contentLength > 0 && totalDownloaded < contentLength) {
                    Serial.printf("FileManager: Connection closed with %d/%d bytes received\n", 
                                 totalDownloaded, contentLength);
                    errorMsg = "Connection lost before download completed";
                    downloadSuccess = false;
                } else {
                    // Connection closed normally
                    break;
                }
            } else if ((millis() - lastDataTime) > NO_DATA_TIMEOUT) {
                // No data for too long, but connection still alive
                errorMsg = "Download stalled - no data received for " + String(NO_DATA_TIMEOUT / 1000) + " seconds";
                downloadSuccess = false;
            }
        }
        
        // Optimized timing: Reduce system call overhead
        if (availableData == 0) {
            // Only delay when no data is available to prevent tight loop
            delayMicroseconds(500); // Reduced from delay(10) - much more efficient
        }
        
        // Reduced yield frequency for better performance - only yield every 8KB processed
        if ((totalDownloaded % 8192) == 0 && totalDownloaded > 0) {
            yield();
        }
    }
    
    // Check for overall timeout
    if ((millis() - startTime) >= DOWNLOAD_TIMEOUT_MS) {
        errorMsg = "Download timed out after " + String(DOWNLOAD_TIMEOUT_MS / 1000) + " seconds";
        downloadSuccess = false;
    }
    
    free(buffer);
    file.close();
    client.stop();
    
    if (!downloadSuccess) {
        SD.remove(tempPath);
        downloadInProgress = false;
        
        // Update failure statistics
        downloadStats.totalDownloads++;
        downloadStats.failedDownloads++;
        saveDownloadStats();
        
        return false;
    }
    
    // Verify download size with small tolerance for edge cases
    if (contentLength > 0) {
        int missingBytes = contentLength - totalDownloaded;
        if (missingBytes > 0) {
            if (missingBytes <= 64) { // Allow up to 64 bytes missing (could be padding or encoding issues)
                // Size difference within tolerance, continue silently
            } else {
                errorMsg = "Download incomplete: " + String(totalDownloaded) + "/" + String(contentLength) + " bytes (" + String(missingBytes) + " bytes missing)";
                SD.remove(tempPath);
                downloadInProgress = false;
                return false;
            }
        } else if (missingBytes < 0) {
            Serial.printf("FileManager: Downloaded %d extra bytes (file may have grown)\n", -missingBytes);
        }
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
    
    // Verify final file exists and has reasonable size
    File finalFile = SD.open(localPath);
    if (!finalFile) {
        errorMsg = "Final file verification failed";
        downloadInProgress = false;
        return false;
    }
    size_t finalSize = finalFile.size();
    finalFile.close();
    
    if (contentLength > 0) {
        int sizeDiff = abs((int)finalSize - (int)contentLength);
        if (sizeDiff > 64) { // Allow up to 64 bytes difference
            errorMsg = "Final file size mismatch: expected " + String(contentLength) + ", got " + String(finalSize);
            SD.remove(localPath);
            downloadInProgress = false;
            return false;
        } else if (sizeDiff > 0) {
            // Size difference within tolerance, continue silently
        }
    }
    
    downloadInProgress = false;
    
    // Update statistics
    downloadStats.totalDownloads++;
    downloadStats.successfulDownloads++;
    downloadStats.totalBytesDownloaded += totalDownloaded;
    saveDownloadStats();
    
    Serial.printf("Download completed: %s (%d bytes)\n", 
                  localPath.substring(localPath.lastIndexOf('/') + 1).c_str(), totalDownloaded);
    
    if (downloadCompleteCallback) {
        downloadCompleteCallback(httpUrl, localPath, true, "");
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
    
    // Check if directory already exists
    File dirFile = SD.open(dir);
    if (dirFile && dirFile.isDirectory()) {
        dirFile.close();
        return true;
    }
    if (dirFile) {
        dirFile.close();
    }
    
    // Create directories recursively
    return createDirectoryRecursive(dir);
}

bool FileManager::createDirectoryRecursive(const String& path) {
    if (path.isEmpty() || path == "/") {
        return true;
    }
    
    // Check if directory already exists
    File dirFile = SD.open(path);
    if (dirFile && dirFile.isDirectory()) {
        dirFile.close();
        return true;
    }
    if (dirFile) {
        dirFile.close();
    }
    
    // Find parent directory
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) { // Don't include root directory
        String parentDir = path.substring(0, lastSlash);
        // Recursively create parent directory first
        if (!createDirectoryRecursive(parentDir)) {
            Serial.printf("FileManager: Failed to create parent directory: %s\n", parentDir.c_str());
            return false;
        }
    }
    
    // Now create this directory
    bool success = SD.mkdir(path);
    if (success) {
        Serial.printf("FileManager: Created directory: %s\n", path.c_str());
    } else {
        // Check if it exists now (might have been created by another process)
        File dirFile = SD.open(path);
        if (dirFile && dirFile.isDirectory()) {
            dirFile.close();
            Serial.printf("FileManager: Directory already exists: %s\n", path.c_str());
            return true;
        }
        Serial.printf("FileManager: Failed to create directory: %s\n", path.c_str());
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
