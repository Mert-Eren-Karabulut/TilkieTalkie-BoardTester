#include <Arduino.h>
#include <esp_wifi.h>
#include "ConfigManager.h"
#include "WiFiProvisioning.h"
#include "BatteryManagement.h"
#include "FileManager.h"

// Global instances
ConfigManager &config = ConfigManager::getInstance();
WiFiProvisioningManager &wifiProv = WiFiProvisioningManager::getInstance();
BatteryManager &battery = BatteryManager::getInstance();
FileManager &fileManager = FileManager::getInstance();

void setup()
{
    Serial.begin(115200);
    delay(1000);  // Give serial time to initialize

    Serial.println("=== TilkieTalkie Board Tester ===");
    Serial.println("Initializing system...");

    // Initialize configuration (this will also initialize NVS)
    Serial.println("Loading configuration...");
    config.printAllSettings();

    // Initialize WiFi provisioning
    Serial.println("Initializing WiFi...");
    wifiProv.begin();

    // Initialize battery management
    Serial.println("Initializing Battery Management...");
    battery.begin();

    // Initialize file manager
    Serial.println("Initializing File Manager...");
    if (!fileManager.begin()) {
        Serial.println("WARNING: File Manager initialization failed!");
        Serial.println("SD card functionality will not be available.");
    }

    // Initialize other modules here
    // e.g., audio, sensors, etc.

    Serial.println("System initialization complete!");
    Serial.println("--- Terminal Commands ---");
    Serial.println("WiFi Commands:");
    Serial.println("  qr      - Print QR code for provisioning");
    Serial.println("  reset   - Reset WiFi provisioning");
    Serial.println("  stats   - Show WiFi connection status");
    Serial.println("  store   - Store current WiFi credentials");
    Serial.println("System Commands:");
    Serial.println("  restart - Restart the device");
    Serial.println("  config  - Show all configuration");
    Serial.println("  commit  - Force save configuration to flash");
    Serial.println("  debug   - Show debug information");
    Serial.println("  test    - Test NVS storage");
    Serial.println("  factory - Factory reset (erase all data)");
    Serial.println("Battery Commands:");
    Serial.println("  battery - Show battery status");
    Serial.println("  voltage - Show raw voltage reading");
    Serial.println("File Manager Commands:");
    Serial.println("  sdinfo  - Show SD card information");
    Serial.println("  files   - List files on SD card");
    Serial.println("  dlstats - Show download statistics");
    Serial.println("  dlqueue - Show download queue");
    Serial.println("  required- Show required files");
    Serial.println("  download <url> <path> - Download file from URL");
    Serial.println("  addfile <path> <url> - Add required file");
    Serial.println("  checkfiles - Check and download missing files");
    Serial.println("  cleanup - Clean up temporary files");
    Serial.println("Type any command for help\n");
}

void loop()
{
    // Handle serial commands
    if (Serial.available())
    {
        String command = Serial.readStringUntil('\n');
        command.trim();
        command.toLowerCase();

        // Skip empty commands
        if (command.length() == 0) {
            return;
        }

        // WiFi related commands
        if (command == "qr" || command == "reset" || command == "stats")
        {
            wifiProv.handleCommand(command);
        }
        else if (command == "store")
        {
            Serial.println("\nManually storing current WiFi credentials...");
            config.storeCurrentWiFiCredentials();
            config.commit();
        }
        else if (command == "test")
        {
            Serial.println("\nTesting NVS storage...");
            config.setWiFiCredentials("TestSSID", "TestPassword123");
            config.commit();
            Serial.println("Test credentials stored. Checking storage...");
            config.printAllSettings();
        }
        else if (command == "factory")
        {
            Serial.println("\nWARNING: Factory reset will erase ALL stored data!");
            Serial.println("Type 'yes' to confirm or any other key to cancel:");
            while (!Serial.available()) {
                delay(100);
            }
            String confirmation = Serial.readStringUntil('\n');
            confirmation.trim();
            confirmation.toLowerCase();
            
            if (confirmation == "yes") {
                config.factoryReset();
            } else {
                Serial.println("Factory reset cancelled.");
            }
        }
        // File Manager commands
        else if (command == "sdinfo")
        {
            Serial.println(fileManager.getSDCardInfo());
        }
        else if (command == "files")
        {
            fileManager.printFileList("/");
        }
        else if (command == "dlstats")
        {
            Serial.println(fileManager.getDownloadStatsString());
        }
        else if (command == "dlqueue")
        {
            fileManager.printDownloadQueue();
        }
        else if (command == "required")
        {
            fileManager.printRequiredFiles();
        }
        else if (command.startsWith("download "))
        {
            // Parse download command: download <url> <path>
            int firstSpace = command.indexOf(' ', 9);
            int secondSpace = command.indexOf(' ', firstSpace + 1);
            
            if (firstSpace != -1 && secondSpace != -1) {
                String url = command.substring(firstSpace + 1, secondSpace);
                String path = command.substring(secondSpace + 1);
                
                Serial.println("Scheduling download: " + url + " -> " + path);
                if (fileManager.scheduleDownload(url, path)) {
                    Serial.println("Download scheduled successfully");
                } else {
                    Serial.println("Failed to schedule download");
                }
            } else {
                Serial.println("Usage: download <url> <local_path>");
                Serial.println("Example: download http://example.com/audio.wav /audio/test.wav");
            }
        }
        else if (command.startsWith("addfile "))
        {
            // Parse addfile command: addfile <path> <url>
            int firstSpace = command.indexOf(' ', 8);
            int secondSpace = command.indexOf(' ', firstSpace + 1);
            
            if (firstSpace != -1 && secondSpace != -1) {
                String path = command.substring(firstSpace + 1, secondSpace);
                String url = command.substring(secondSpace + 1);
                
                Serial.println("Adding required file: " + path + " <- " + url);
                if (fileManager.addRequiredFile(path, url)) {
                    Serial.println("Required file added successfully");
                } else {
                    Serial.println("Failed to add required file");
                }
            } else {
                Serial.println("Usage: addfile <local_path> <url>");
                Serial.println("Example: addfile /audio/sound.wav http://example.com/audio.wav");
            }
        }
        else if (command == "checkfiles")
        {
            Serial.println("Checking and downloading missing files...");
            fileManager.checkRequiredFiles();
            
            std::vector<String> missing = fileManager.getMissingFiles();
            if (missing.size() > 0) {
                Serial.printf("Found %d missing files:\n", missing.size());
                for (const auto& file : missing) {
                    Serial.println("  " + file);
                }
            } else {
                Serial.println("All required files are present");
            }
        }
        else if (command == "cleanup")
        {
            Serial.println("Cleaning up temporary files...");
            fileManager.cleanupTempFiles();
            Serial.println("Cleanup complete");
        }
        // Battery commands
        else if (command == "battery")
        {
            battery.printBatteryInfo();
        }
        else if (command == "voltage")
        {
            Serial.println("\n--- Battery Voltage Details ---");
            Serial.println("Current voltage: " + String(battery.getBatteryVoltage(), 3) + "V");
            Serial.println("Battery percentage: " + String(battery.getBatteryPercentage(), 1) + "%");
            Serial.println("Charging status: " + String(battery.getChargingStatus() ? "Yes" : "No"));
            Serial.println("Battery status: " + battery.getBatteryStatusString());
            Serial.println("Raw ADC reading: " + String(analogRead(39)));
            Serial.println("Charging pin state: " + String(digitalRead(34)));
            Serial.println("------------------------------\n");
        }
        // System commands
        else if (command == "restart")
        {
            Serial.println("\nRestarting device...");
            delay(1000);
            ESP.restart();
        }
        else if (command == "config")
        {
            config.printAllSettings();
        }
        else if (command == "commit")
        {
            config.commit();
        }
        else if (command == "debug")
        {
            Serial.println("\n--- Debug Information ---");
            Serial.println("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
            Serial.println("Chip revision: " + String(ESP.getChipRevision()));
            Serial.println("SDK version: " + String(ESP.getSdkVersion()));
            Serial.println("WiFi mode: " + String(WiFi.getMode()));
            Serial.println("WiFi status: " + String(WiFi.status()));
            Serial.println("Battery: " + battery.getBatteryStatusString());
            config.printAllSettings();
        }
        else
        {
            Serial.println("\nAvailable commands:");
            Serial.println("WiFi: qr, reset, stats, store");
            Serial.println("System: restart, config, commit, debug, test, factory");
            Serial.println("Battery: battery, voltage");
            Serial.println("Files: sdinfo, files, dlstats, dlqueue, required, checkfiles, cleanup");
            Serial.println("       download <url> <path>, addfile <path> <url>");
        }
    }

    // Update battery management
    battery.update();
    
    // Update file manager
    fileManager.update();

    // Add your main application logic here
    // e.g., handle audio, sensors, communication, etc.

    delay(100);
}