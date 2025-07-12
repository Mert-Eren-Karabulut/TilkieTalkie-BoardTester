/*
 * FileManager Example Usage
 * 
 * This example demonstrates how to use the FileManager class
 * for SD card operations and audio file downloads.
 */

#include <Arduino.h>
#include "FileManager.h"
#include "BatteryManagement.h"
#include "WiFiProvisioning.h"

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("=== FileManager Example ===");
    
    // Initialize FileManager
    FileManager& fileManager = FileManager::getInstance();
    if (!fileManager.begin()) {
        Serial.println("Failed to initialize FileManager!");
        return;
    }
    
    // Initialize WiFi and Battery (required for downloads)
    WiFiProvisioningManager& wifiProv = WiFiProvisioningManager::getInstance();
    BatteryManager& battery = BatteryManager::getInstance();
    
    wifiProv.begin();
    battery.begin();
    
    // Example 1: Basic file operations
    Serial.println("\n--- Example 1: Basic File Operations ---");
    
    // Write a test file
    String testContent = "Hello from ESP32 FileManager!\nTimestamp: " + String(millis());
    if (fileManager.writeFile("/test.txt", testContent)) {
        Serial.println("✓ Test file written successfully");
    }
    
    // Read the file back
    String readContent = fileManager.readFile("/test.txt");
    if (readContent.length() > 0) {
        Serial.println("✓ Test file read successfully:");
        Serial.println("Content: " + readContent);
    }
    
    // List files
    Serial.println("\n✓ Files on SD card:");
    fileManager.printFileList("/");
    
    // Example 2: Directory operations
    Serial.println("\n--- Example 2: Directory Operations ---");
    
    if (fileManager.createDirectory("/audio/sounds")) {
        Serial.println("✓ Audio directory created");
    }
    
    if (fileManager.createDirectory("/logs/system")) {
        Serial.println("✓ Logs directory created");
    }
    
    // Example 3: Adding required files
    Serial.println("\n--- Example 3: Required Files Management ---");
    
    // Add some example audio files that should be downloaded
    fileManager.addRequiredFile(
        "/audio/welcome.wav", 
        "https://example.com/audio/welcome.wav"
    );
    
    fileManager.addRequiredFile(
        "/audio/beep.wav", 
        "https://example.com/audio/beep.wav"
    );
    
    fileManager.addRequiredFile(
        "/audio/goodbye.wav", 
        "https://example.com/audio/goodbye.wav"
    );
    
    Serial.println("✓ Required files added");
    fileManager.printRequiredFiles();
    
    // Example 4: Check for missing files
    Serial.println("\n--- Example 4: Missing Files Check ---");
    
    std::vector<String> missingFiles = fileManager.getMissingFiles();
    if (missingFiles.size() > 0) {
        Serial.printf("Found %d missing files:\n", missingFiles.size());
        for (const auto& file : missingFiles) {
            Serial.println("  - " + file);
        }
        
        Serial.println("These files will be downloaded when:");
        Serial.println("  1. Device is charging");
        Serial.println("  2. WiFi is connected");
        Serial.println("  3. Internet is available");
    } else {
        Serial.println("✓ All required files are present");
    }
    
    // Example 5: Manual download (if conditions are met)
    Serial.println("\n--- Example 5: Manual Download ---");
    
    if (battery.getChargingStatus()) {
        Serial.println("✓ Device is charging - downloads allowed");
        
        // Example download (replace with actual URL)
        String errorMsg;
        if (fileManager.downloadNow(
            "https://httpbin.org/bytes/1024", 
            "/test_download.bin", 
            errorMsg)) {
            Serial.println("✓ Test download completed successfully");
        } else {
            Serial.println("✗ Download failed: " + errorMsg);
        }
    } else {
        Serial.println("⚡ Device not charging - downloads disabled");
        Serial.println("Connect charger to enable downloads");
    }
    
    // Example 6: SD Card information
    Serial.println("\n--- Example 6: SD Card Information ---");
    Serial.println(fileManager.getSDCardInfo());
    
    // Example 7: Download statistics
    Serial.println("\n--- Example 7: Download Statistics ---");
    Serial.println(fileManager.getDownloadStatsString());
    
    Serial.println("\n=== Example Complete ===");
    Serial.println("The FileManager will continue running in the background.");
    Serial.println("It will automatically check for missing files and download them");
    Serial.println("when the device is charging and connected to WiFi.");
}

void loop() {
    // Update FileManager (this handles background downloads)
    FileManager& fileManager = FileManager::getInstance();
    fileManager.update();
    
    // Update other managers
    BatteryManager& battery = BatteryManager::getInstance();
    battery.update();
    
    // Print status every 30 seconds
    static unsigned long lastStatus = 0;
    if (millis() - lastStatus > 30000) {
        lastStatus = millis();
        
        Serial.println("\n--- Status Update ---");
        Serial.printf("Pending downloads: %d\n", fileManager.getPendingDownloadsCount());
        Serial.printf("Download in progress: %s\n", fileManager.isDownloadInProgress() ? "Yes" : "No");
        Serial.printf("SD card available: %s\n", fileManager.isSDCardAvailable() ? "Yes" : "No");
        Serial.printf("Battery charging: %s\n", battery.getChargingStatus() ? "Yes" : "No");
        Serial.printf("Free space: %s\n", fileManager.formatBytes(fileManager.getSDCardFreeSpace()).c_str());
    }
    
    delay(1000);
}

/*
 * Expected behavior:
 * 
 * 1. On startup, the FileManager initializes the SD card
 * 2. Basic file operations are demonstrated
 * 3. Required files are added to the system
 * 4. Missing files are identified
 * 5. If charging, a test download is attempted
 * 6. In the loop, the FileManager automatically:
 *    - Checks for missing required files
 *    - Downloads them when conditions are met (charging + WiFi + internet)
 *    - Retries failed downloads with exponential backoff
 *    - Maintains persistent download queue across reboots
 * 
 * Console commands available in main.cpp:
 * - sdinfo: Show SD card information
 * - files: List files on SD card
 * - dlstats: Show download statistics
 * - dlqueue: Show pending downloads
 * - required: Show required files
 * - download <url> <path>: Schedule a download
 * - addfile <path> <url>: Add a required file
 * - checkfiles: Check and download missing files
 * - cleanup: Clean temporary files
 */
