#include <Arduino.h>
#include <esp_wifi.h>
#include "ConfigManager.h"
#include "WiFiProvisioning.h"
#include "BatteryManagement.h"
#include "FileManager.h"
#include "AudioController.h"
#include "LedController.h"
#include "NfcController.h"

// Use the singleton instance from the header
NfcController &nfcController = NfcController::getInstance();

// Global instances
ConfigManager &config = ConfigManager::getInstance();
WiFiProvisioningManager &wifiProv = WiFiProvisioningManager::getInstance();
BatteryManager &battery = BatteryManager::getInstance();
FileManager &fileManager = FileManager::getInstance();
AudioController &audioController = AudioController::getInstance();
LedController ledController;
// NfcController is already instantiated above

// +++ NFC Callback Functions +++

// This function will be called ONLY ONCE when a new card is detected
void afterNFCRead(const NFCData &nfcData)
{
    Serial.println("=== Hook: afterNFCRead ===");
    Serial.print("Card UID: ");
    Serial.println(nfcData.uidString);
    Serial.print("Timestamp: ");
    Serial.println(nfcData.timestamp);

    // Example: Play a sound and turn the LED green
    // audioController.play("/sounds/nfc_success.mp3");
    ledController.pulseRapid(0x00FF00, 3); // Green color

    Serial.println("==========================");
}

// This function will be called when the reed switch is deactivated
// AFTER a card was successfully read in that session.
void afterDetachNFC()
{
    Serial.println("=== Hook: afterDetachNFC ===");
    Serial.println("NFC session has ended.");

    // Example: Stop audio and turn the LED red
    audioController.stop();
    ledController.pulseRapid(0xFF0000, 3); // Red color

    Serial.println("==========================");
}

void setup()
{
    Serial.begin(115200);
    delay(1000); // Give serial time to initialize

    Serial.println("=== TilkieTalkie Board Tester ===");
    Serial.println("Initializing system...");

    // Enable peripheral power (IO17) - CRITICAL for SD card and other peripherals
    Serial.println("Enabling peripheral power...");
    pinMode(17, OUTPUT);
    digitalWrite(17, HIGH); // Enable power to peripherals
    delay(1000);            // Give peripherals time to power up
    Serial.println("Peripheral power enabled on IO17");

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
    if (!fileManager.begin())
    {
        Serial.println("WARNING: File Manager initialization failed!");
        Serial.println("SD card functionality will not be available.");
    }

    // Initialize audio controller
    Serial.println("Initializing Audio Controller...");
    if (!audioController.begin())
    {
        Serial.println("WARNING: Audio Controller initialization failed!");
        Serial.println("Audio functionality will not be available.");
    }

    // Initialize LED controller
    Serial.println("Initializing LED Controller...");
    ledController.begin();
    Serial.println("LED Controller initialized successfully!");

    // --- Initialize NFC Controller ---
    Serial.println("Initializing NFC Controller...");
    if (nfcController.begin())
    {
        Serial.println("NFC Controller initialized successfully!");

        // Set up the new NFC callbacks
        nfcController.setAfterNFCReadCallback(afterNFCRead);
        nfcController.setAfterDetachNFCCallback(afterDetachNFC);

        Serial.println("NFC callbacks configured.");
    }
    else
    {
        Serial.println("FATAL: NFC Controller initialization failed!");
        Serial.println("NFC functionality will not be available.");
        // Handle failure, maybe by pulsing an error color
        ledController.pulseLed(0xFF0000); // Pulse red for error
    }

    // Initialize other modules here
    // e.g., sensors, etc.

    // rapid pulse LED to indicate system is ready
    ledController.pulseRapid(0x00FF00, 3); // Rapid pulse green

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
    Serial.println("  sdtest  - Test file operations");
    Serial.println("  sdstress- Run SD card stress test");
    Serial.println("  sdspeed - Optimize SD card speed");
    Serial.println("  sdtree  - Check SD card file tree");
    Serial.println("  sdformat- Format SD card as FAT32");
    Serial.println("  files   - List files on SD card");
    Serial.println("  deletefile <path> - Delete file from SD card");
    Serial.println("  dlstats - Show download statistics");
    Serial.println("  dlqueue - Show download queue");
    Serial.println("  required- Show required files");
    Serial.println("  download <url> <path> - Download file from URL");
    Serial.println("  addfile <path> <url> - Add required file");
    Serial.println("  checkfiles - Check and download missing files");
    Serial.println("  cleanup - Clean up temporary files");
    Serial.println("Audio Commands:");
    Serial.println("  play <path> - Play MP3 file");
    Serial.println("  pause   - Pause current playback");
    Serial.println("  resume  - Resume paused playback");
    Serial.println("  stop    - Stop playback");
    Serial.println("  volup   - Volume up");
    Serial.println("  voldown - Volume down");
    Serial.println("  volume  - Show current volume");
    Serial.println("  track   - Show current track");
    Serial.println("LED Commands:");
    Serial.println("  ledon <hex> <intensity> - Turn LED on with hex color and intensity (0-255)");
    Serial.println("  ledoff  - Turn LED off");
    Serial.println("  pulse <hex> - Start pulsing LED with hex color");
    Serial.println("  rapid <hex> <count> - Rapid pulse LED for count times");
    Serial.println("NFC Commands:");
    Serial.println("  nfcstatus - Show NFC controller status");
    Serial.println("  nfcdata   - Show current NFC card data");
    Serial.println("  nfcreed   - Show reed switch status");
    Serial.println("  nfcdiag   - Run NFC diagnostics");
    Serial.println("Power Commands:");
    Serial.println("  power   - Show peripheral power status");
    Serial.println("  poweron - Enable peripheral power (IO17)");
    Serial.println("  poweroff- Disable peripheral power (IO17)");
    Serial.println("Type any command for help\n");
    audioController.play("/sounds/12.mp3"); // Play startup sound
}

void loop()
{
    // Update LED controller (handles pulse and pulseRapid animations)
    ledController.update();

    // Update NFC controller (handles reed switch monitoring and NFC reading)
    nfcController.update();

    // Handle serial commands
    if (Serial.available())
    {
        String command = Serial.readStringUntil('\n');
        command.trim();
        command.toLowerCase();

        // Skip empty commands
        if (command.length() == 0)
        {
            return;
        }

        // help command
        if (command == "help")
        {
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
            Serial.println("  sdtest  - Test file operations");
            Serial.println("  sdstress- Run SD card stress test");
            Serial.println("  sdspeed - Optimize SD card speed");
            Serial.println("  sdtree  - Check SD card file tree");
            Serial.println("  sdformat- Format SD card as FAT32");
            Serial.println("  files   - List files on SD card");
            Serial.println("  deletefile <path> - Delete file from SD card");
            Serial.println("  dlstats - Show download statistics");
            Serial.println("  dlqueue - Show download queue");
            Serial.println("  required- Show required files");
            Serial.println("  download <url> <path> - Download file from URL");
            Serial.println("  addfile <path> <url> - Add required file");
            Serial.println("  checkfiles - Check and download missing files");
            Serial.println("  cleanup - Clean up temporary files");
            Serial.println("Audio Commands:");
            Serial.println("  play <path> - Play MP3 file");
            Serial.println("  pause   - Pause current playback");
            Serial.println("  resume  - Resume paused playback");
            Serial.println("  stop    - Stop playback");
            Serial.println("  volup   - Volume up");
            Serial.println("  voldown - Volume down");
            Serial.println("  volume  - Show current volume");
            Serial.println("  track   - Show current track");
            Serial.println("LED Commands:");
            Serial.println("  ledon <hex> <intensity> - Turn LED on with hex color and intensity (0-255)");
            Serial.println("  ledoff  - Turn LED off");
            Serial.println("  pulse <hex> - Start pulsing LED with hex color");
            Serial.println("  rapid <hex> <count> - Rapid pulse LED for count times");
            Serial.println("NFC Commands:");
            Serial.println("  nfcstatus - Show NFC controller status");
            Serial.println("  nfcdata   - Show current NFC card data");
            Serial.println("  nfcreed   - Show reed switch status");
            Serial.println("  nfcdiag   - Run NFC diagnostics");
            Serial.println("Power Commands:");
            Serial.println("  power   - Show peripheral power status");
            Serial.println("  poweron - Enable peripheral power (IO17)");
            Serial.println("  poweroff- Disable peripheral power (IO17)");
            Serial.println("Type any command for help\n");
            Serial.flush();
            return; // Skip further processing
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
            while (!Serial.available())
            {
                delay(100);
            }
            String confirmation = Serial.readStringUntil('\n');
            confirmation.trim();
            confirmation.toLowerCase();

            if (confirmation == "yes")
            {
                config.factoryReset();
            }
            else
            {
                Serial.println("Factory reset cancelled.");
            }
        }
        // File Manager commands
        else if (command == "sdinfo")
        {
            Serial.println(fileManager.getSDCardInfo());
        }
        else if (command == "sdtest")
        {
            fileManager.testFileOperations();
        }
        else if (command == "sdstress")
        {
            fileManager.runSDCardStressTest();
        }
        else if (command == "sdspeed")
        {
            fileManager.optimizeSDCardSpeed();
        }
        else if (command == "sdtree")
        {
            fileManager.printFileTree();
        }
        else if (command == "sdformat")
        {
            fileManager.formatSDCard();
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
            int firstSpace = command.indexOf(' ');                  // Find first space (after "download")
            int secondSpace = command.indexOf(' ', firstSpace + 1); // Find second space

            if (firstSpace != -1 && secondSpace != -1)
            {
                String url = command.substring(firstSpace + 1, secondSpace);
                String path = command.substring(secondSpace + 1);

                Serial.println("Scheduling download: " + url + " -> " + path);
                if (fileManager.scheduleDownload(url, path))
                {
                    Serial.println("Download scheduled successfully");
                }
                else
                {
                    Serial.println("Failed to schedule download");
                }
            }
            else
            {
                Serial.println("Usage: download <url> <local_path>");
                Serial.println("Example: download http://example.com/audio.wav /audio/test.wav");
            }
        }
        else if (command.startsWith("addfile "))
        {
            // Parse addfile command: addfile <path> <url>
            int firstSpace = command.indexOf(' ');                  // Find first space (after "addfile")
            int secondSpace = command.indexOf(' ', firstSpace + 1); // Find second space

            if (firstSpace != -1 && secondSpace != -1)
            {
                String path = command.substring(firstSpace + 1, secondSpace);
                String url = command.substring(secondSpace + 1);

                Serial.println("Adding required file: " + path + " <- " + url);
                if (fileManager.addRequiredFile(path, url))
                {
                    Serial.println("Required file added successfully");
                }
                else
                {
                    Serial.println("Failed to add required file");
                }
            }
            else
            {
                Serial.println("Usage: addfile <local_path> <url>");
                Serial.println("Example: addfile /audio/sound.wav http://example.com/audio.wav");
            }
        }
        else if (command == "checkfiles")
        {
            Serial.println("Checking and downloading missing files...");
            fileManager.checkRequiredFiles();

            std::vector<String> missing = fileManager.getMissingFiles();
            if (missing.size() > 0)
            {
                Serial.printf("Found %d missing files:\n", missing.size());
                for (const auto &file : missing)
                {
                    Serial.println("  " + file);
                }
            }
            else
            {
                Serial.println("All required files are present");
            }
        }
        else if (command == "cleanup")
        {
            Serial.println("Cleaning up temporary files...");
            fileManager.cleanupTempFiles();
            Serial.println("Cleanup complete");
        }
        else if (command.startsWith("deletefile "))
        {
            // Parse deletefile command: deletefile <path>
            int firstSpace = command.indexOf(' ');

            if (firstSpace != -1)
            {
                String filePath = command.substring(firstSpace + 1);
                filePath.trim(); // Remove any extra whitespace

                if (filePath.isEmpty())
                {
                    Serial.println("Usage: deletefile <file_path>");
                    Serial.println("Example: deletefile /images/image.webp");
                }
                else
                {
                    Serial.println("Deleting file and removing from required list: " + filePath);
                    if (fileManager.deleteFileAndRemoveFromRequired(filePath))
                    {
                        Serial.println("File deleted successfully");
                    }
                    else
                    {
                        Serial.println("Failed to delete file (file may not exist)");
                    }
                }
            }
            else
            {
                Serial.println("Usage: deletefile <file_path>");
                Serial.println("Example: deletefile /images/image.webp");
                Serial.println("Note: This will also remove the file from required list to prevent re-download");
            }
        }
        // Audio commands
        else if (command.startsWith("play "))
        {
            // Parse play command: play <path>
            int firstSpace = command.indexOf(' ');

            if (firstSpace != -1)
            {
                String filePath = command.substring(firstSpace + 1);
                filePath.trim();

                if (filePath.isEmpty())
                {
                    Serial.println("Usage: play <file_path>");
                    Serial.println("Example: play /audio/song.mp3");
                }
                else
                {
                    Serial.println("Playing: " + filePath);
                    if (audioController.play(filePath))
                    {
                        Serial.println("Playback started successfully");
                    }
                    else
                    {
                        Serial.println("Failed to start playback");
                    }
                }
            }
            else
            {
                Serial.println("Usage: play <file_path>");
                Serial.println("Example: play /audio/song.mp3");
            }
        }
        else if (command == "pause")
        {
            if (audioController.pause())
            {
                Serial.println("Playback paused");
            }
            else
            {
                Serial.println("Nothing to pause or already paused");
            }
        }
        else if (command == "resume")
        {
            if (audioController.resume())
            {
                Serial.println("Playback resumed");
            }
            else
            {
                Serial.println("Nothing to resume or not paused");
            }
        }
        else if (command == "stop")
        {
            if (audioController.stop())
            {
                Serial.println("Playback stopped");
            }
            else
            {
                Serial.println("Nothing to stop or already stopped");
            }
        }
        else if (command == "volup")
        {
            if (audioController.volumeUp())
            {
                Serial.printf("Volume increased to %d%%\n", audioController.getCurrentVolume());
            }
            else
            {
                Serial.println("Volume already at maximum");
            }
        }
        else if (command == "voldown")
        {
            if (audioController.volumeDown())
            {
                Serial.printf("Volume decreased to %d%%\n", audioController.getCurrentVolume());
            }
            else
            {
                Serial.println("Volume already at minimum");
            }
        }
        else if (command == "volume")
        {
            Serial.printf("Current volume: %d%%\n", audioController.getCurrentVolume());
        }
        else if (command == "track")
        {
            String track = audioController.getCurrentTrack();
            if (track.isEmpty())
            {
                Serial.println("No track currently loaded");
            }
            else
            {
                Serial.println("Current track: " + track);
                Serial.println("Status: " + String(
                                                audioController.isPlaying() ? "Playing" : audioController.isPaused() ? "Paused"
                                                                                                                     : "Stopped"));
            }
        }
        // Power control commands
        else if (command == "power")
        {
            bool powerState = digitalRead(17);
            Serial.printf("Peripheral power (IO17): %s\n", powerState ? "ENABLED" : "DISABLED");
            Serial.printf("Pin state: %s\n", powerState ? "HIGH" : "LOW");
            if (!powerState)
            {
                Serial.println("WARNING: Peripherals (SD card, etc.) will not work with power disabled!");
                Serial.println("Use 'poweron' command to enable peripheral power.");
            }
        }
        else if (command == "poweron")
        {
            Serial.println("Enabling peripheral power...");
            digitalWrite(17, HIGH);
            delay(100);
            Serial.println("Peripheral power ENABLED");
            Serial.println("You may need to reinitialize modules (restart recommended)");
        }
        else if (command == "poweroff")
        {
            Serial.println("WARNING: This will disable power to SD card and other peripherals!");
            Serial.println("Type 'yes' to confirm or any other key to cancel:");
            while (!Serial.available())
            {
                delay(100);
            }
            String confirmation = Serial.readStringUntil('\n');
            confirmation.trim();
            confirmation.toLowerCase();

            if (confirmation == "yes")
            {
                digitalWrite(17, LOW);
                Serial.println("Peripheral power DISABLED");
            }
            else
            {
                Serial.println("Power-off cancelled.");
            }
        }
        // LED commands
        else if (command.startsWith("ledon"))
        {
            // Parse command: ledon <hex_color> <intensity>
            // Example: ledon FF0000 128 (red color with 128 intensity)
            String params = command.substring(5); // Remove "ledon"
            params.trim();

            int spaceIndex = params.indexOf(' ');
            if (spaceIndex > 0)
            {
                String hexStr = params.substring(0, spaceIndex);
                String intensityStr = params.substring(spaceIndex + 1);

                // Convert hex string to uint32_t
                uint32_t hexColor = strtol(hexStr.c_str(), NULL, 16);
                int intensity = intensityStr.toInt();

                ledController.simpleLed(hexColor, intensity);
                Serial.printf("LED set to color: 0x%06X, intensity: %d\n", hexColor, intensity);
            }
            else
            {
                Serial.println("Usage: ledon <hex_color> <intensity>");
                Serial.println("Example: ledon FF0000 128 (red color with 128 intensity)");
            }
        }
        else if (command == "ledoff")
        {
            ledController.turnOff();
            Serial.println("LED turned off");
        }
        else if (command.startsWith("pulse"))
        {
            // Parse command: pulse <hex_color>
            // Example: pulse 00FF00 (green pulsing)
            String params = command.substring(5); // Remove "pulse"
            params.trim();

            if (params.length() > 0)
            {
                uint32_t hexColor = strtol(params.c_str(), NULL, 16);
                ledController.pulseLed(hexColor);
                Serial.printf("LED pulsing started with color: 0x%06X\n", hexColor);
            }
            else
            {
                Serial.println("Usage: pulse <hex_color>");
                Serial.println("Example: pulse 00FF00 (green pulsing)");
            }
        }
        else if (command.startsWith("rapid"))
        {
            // Parse command: rapid <hex_color> <count>
            // Example: rapid 0000FF 5 (blue rapid pulse 5 times)
            String params = command.substring(5); // Remove "rapid"
            params.trim();

            int spaceIndex = params.indexOf(' ');
            if (spaceIndex > 0)
            {
                String hexStr = params.substring(0, spaceIndex);
                String countStr = params.substring(spaceIndex + 1);

                uint32_t hexColor = strtol(hexStr.c_str(), NULL, 16);
                int count = countStr.toInt();

                ledController.pulseRapid(hexColor, count);
                Serial.printf("LED rapid pulse started with color: 0x%06X, count: %d\n", hexColor, count);
            }
            else
            {
                Serial.println("Usage: rapid <hex_color> <count>");
                Serial.println("Example: rapid 0000FF 5 (blue rapid pulse 5 times)");
            }
        }
        // NFC commands
        else if (command == "nfcstatus")
        {
            Serial.println("\n--- NFC Controller Status ---");
            Serial.print("NFC Ready: ");
            Serial.println(nfcController.isNFCReady() ? "Yes" : "No");
            Serial.print("Reed Switch Active: ");
            Serial.println(nfcController.isReedSwitchActive() ? "Yes" : "No");
            Serial.print("Card Present: ");
            Serial.println(nfcController.isCardPresent() ? "Yes" : "No");
            Serial.println("-----------------------------\n");
        }
        else if (command == "nfcdata")
        {
            NFCData currentCard = nfcController.currentNFCData();
            Serial.println("\n--- Currently Docked NFC Card ---");
            if (currentCard.isValid)
            {
                Serial.print("UID: ");
                Serial.println(currentCard.uidString);
                Serial.print("UID Length: ");
                Serial.println(currentCard.uidLength);
                Serial.print("Timestamp: ");
                Serial.println(currentCard.timestamp);
            }
            else
            {
                Serial.println("No card is currently docked.");
            }
            Serial.println("----------------------------------\n");
        }
        else if (command == "nfcreed")
        {
            bool rawReedState = digitalRead(REED_SWITCH_PIN);
            Serial.println("\n--- Reed Switch Status ---");
            Serial.print("Raw Pin State (GPIO4): ");
            Serial.println(rawReedState ? "HIGH" : "LOW");
            Serial.print("Debounced Controller State: ");
            Serial.println(nfcController.isReedSwitchActive() ? "Active" : "Inactive");
            Serial.println("-------------------------\n");
        }
        else if (command == "nfcdiag")
        {
            nfcController.diagnostics();
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
            Serial.println("\nUnknown command. Type 'help' for a list of commands.");
        }
    }

    // Update battery management
    battery.update();

    // Update file manager
    fileManager.update();

    // Update audio controller
    audioController.update();
}