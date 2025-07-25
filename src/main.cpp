#include <Arduino.h>
#include <esp_wifi.h>
#include "ConfigManager.h"
#include "WiFiProvisioning.h"
#include "BatteryManagement.h"
#include "FileManager.h"
#include "AudioController.h"
#include "LedController.h"
#include "NfcController.h"
#include "RequestManager.h"
#include "ReverbClient.h"

// Use the singleton instance from the header
NfcController &nfcController = NfcController::getInstance();

// Global instances
ConfigManager &config = ConfigManager::getInstance();
RequestManager &requestManager = RequestManager::getInstance("https://portal.tilkietalkie.com/api");
WiFiProvisioningManager &wifiProv = WiFiProvisioningManager::getInstance();
BatteryManager &battery = BatteryManager::getInstance();
FileManager &fileManager = FileManager::getInstance();
AudioController &audioController = AudioController::getInstance();
LedController ledController;
ReverbClient &reverb = ReverbClient::getInstance(); // Create an alias for easier access

// +++ Reverb WebSocket Callback Function +++
void handleChatMessage(const String &message)
{
    Serial.printf("\n[REVERB] Message Received: %s\n", message.c_str());

    // Example action: Pulse the LED blue when a message comes in
    ledController.pulseRapid(0x0000FF, 2); // Blue color

    // You can add more complex logic here, e.g., parsing the message
    // if (message == "play_sound") {
    //   audioController.play("/sounds/notification.mp3");
    // }
}

// +++ NFC Callback Functions +++

// Callback function for when figure download is complete
void onFigureDownloadComplete(const String &uid, const String &figureName, bool success, const String &error)
{
    Serial.println("=== Figure Download Complete ===");
    Serial.print("UID: ");
    Serial.println(uid);
    Serial.print("Figure: ");
    Serial.println(figureName);
    Serial.print("Success: ");
    Serial.println(success ? "YES" : "NO");
    
    if (success)
    {
        Serial.println("All tracks are ready! Starting automatic playback...");
        
        // Pulse LED green to indicate success
        ledController.pulseRapid(0x00FF00, 5); // Green color, 5 rapid pulses
        
        // TODO: Implement automatic track playback here
        // Example implementation:
        // String firstTrackPath = "/figures/" + figureId + "/1/1.mp3"; // Adjust path as needed
        // audioController.play(firstTrackPath);
        
        Serial.println("Figure is ready for playback!");
    }
    else
    {
        Serial.print("Download failed: ");
        Serial.println(error);
        
        // Pulse LED red to indicate failure
        ledController.pulseRapid(0xFF0000, 5); // Red color, 5 rapid pulses
        
        Serial.println("Some tracks may be missing. Check download status.");
    }
    
    Serial.println("================================");
}

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
    //we need to check if the figure tracks are downloaded and they exist
    //we need to send get request with bearer token to the url :https://portal.tilkietalkie.com/api/units/{nfc_uid}
    requestManager.getCheckFigureTracks(nfcData.uidString);

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

    // Initialize request manager
    if (!requestManager.begin())
    {
        Serial.println("WARNING: Request Manager initialization failed!");
        Serial.println("API functionality may be limited.");
    }
    
    // Set up figure download complete callback
    requestManager.setFigureDownloadCompleteCallback(onFigureDownloadComplete);

    // Initialize battery management
    battery.begin();

    // Initialize file manager
    if (!fileManager.begin())
    {
        Serial.println("WARNING: File Manager initialization failed!");
        Serial.println("SD card functionality will not be available.");
    }

    // Initialize audio controller
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

    // --- NEW: Initialize Reverb Client ---
    Serial.println("Initializing Reverb WebSocket Client...");

    // Wait for WiFi to connect before starting Reverb client
    unsigned long wifi_timeout = millis() + 10000; // 10 second timeout
    while (!WiFi.isConnected() && millis() < wifi_timeout)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.isConnected())
    {
        Serial.println("WiFi is connected. Starting Reverb client.");

        // Use your production credentials from your Laravel .env file
        constexpr char HOST[] = "portal.tilkietalkie.com";
        constexpr uint16_t PORT = 443;
        constexpr char APP_KEY[] = "erko2001"; // Your REVERB_APP_KEY

        const String token = config.getJWTToken();

        // Generate a unique device ID from the ESP32's MAC address
        String deviceId = String(ESP.getEfuseMac());

        reverb.begin(HOST, PORT, APP_KEY, token.c_str(), deviceId.c_str());

        // Register the callback function to handle incoming messages
        reverb.onChatMessage(handleChatMessage);
    }
    else
    {
        Serial.println("⚠️ WiFi connection timed out. Reverb client not started.");
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
    Serial.println("Note: BLE is automatically managed based on WiFi state");
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
    Serial.println("  charging- Show charging status for downloads");
    Serial.println("File Manager Commands:");
    Serial.println("  sdinfo  - Show SD card information");
    Serial.println("  sdtest  - Test file operations");
    Serial.println("  sdstress- Run SD card stress test");
    Serial.println("  sdspeed - Optimize SD card speed");
    Serial.println("  sdtree  - Check SD card file tree");
    Serial.println("  sdformat- Format SD card as FAT32");
    Serial.println("  files   - List files on SD card");
    Serial.println("  deletefile <path> - Delete file from SD card");
    Serial.println("  delete  - Delete ALL required files from NVS and storage");
    Serial.println("  deletefig <uid> - Delete all files for a specific figure");
    Serial.println("  dlstats - Show download statistics");
    Serial.println("  dlqueue - Show download queue");
    Serial.println("  required- Show required files");
    Serial.println("  download <url> <path> - Download file from URL");
    Serial.println("  addfile <path> <url> - Add required file");
    Serial.println("  checkfiles - Check and download missing files");
    Serial.println("  forcedownload - Force download processing (ignore charging)");
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
    Serial.println("Reverb Commands:");
    Serial.println("  send <message> - Send message to Reverb API for broadcast");
    Serial.println("  wsstatus - Show WebSocket connection status");
    Serial.println("  testauth - Test stored JWT token authorization with server");
    Serial.println("Type any command for help\n");
    // audioController.play("/sounds/12.mp3"); // Play startup sound
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
            Serial.println("Note: BLE is automatically managed by ESP32 provisioning library");
            Serial.println("Reverb Commands:");
            Serial.println("  reverbstatus - Show Reverb connection status");
            Serial.println("  reverbclean  - Clean up Reverb client to free memory");
            Serial.println("  reverbstart  - Start Reverb client (needs WiFi)");
            Serial.println("  testauth     - Test stored JWT token authorization with server");
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
            Serial.println("  charging- Show charging status for downloads");
            Serial.println("File Manager Commands:");
            Serial.println("  sdinfo  - Show SD card information");
            Serial.println("  sdtest  - Test file operations");
            Serial.println("  sdstress- Run SD card stress test");
            Serial.println("  sdspeed - Optimize SD card speed");
            Serial.println("  sdtree  - Check SD card file tree");
            Serial.println("  sdformat- Format SD card as FAT32");
            Serial.println("  files   - List files on SD card");
            Serial.println("  deletefile <path> - Delete file from SD card");
            Serial.println("  delete  - Delete ALL required files from NVS and storage");
            Serial.println("  deletefig <uid> - Delete all files for a specific figure");
            Serial.println("  dlstats - Show download statistics");
            Serial.println("  dlqueue - Show download queue");
            Serial.println("  required- Show required files");
            Serial.println("  download <url> <path> - Download file from URL");
            Serial.println("  addfile <path> <url> - Add required file");
            Serial.println("  checkfiles - Check and download missing files");
            Serial.println("  forcedownload - Force download processing (ignore charging)");
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
            Serial.println("Reverb Commands:");
            Serial.println("  send <message> - Send message to Reverb API for broadcast");
            Serial.println("  wsstatus - Show WebSocket connection status");
            Serial.println("  testauth - Test stored JWT token authorization with server");
            Serial.println("Type any command for help\n");
            Serial.flush();
            return; // Skip further processing
        }
        // WebSocket commands
        if (command.startsWith("send "))
        {
            String message = command.substring(5); // Get the text after "send "
            message.trim();
            if (message.length() > 0)
            {
                Serial.printf("Sending message: '%s'\n", message.c_str());
                if (reverb.sendMessage(message))
                {
                    Serial.println("Message sent to API for broadcast.");
                }
                else
                {
                    Serial.println("Failed to send message.");
                }
            }
            else
            {
                Serial.println("Usage: send <your message>");
            }
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
        // Reverb related commands
        else if (command == "reverbstatus")
        {
            Serial.print("Reverb Status: ");
            Serial.println(reverb.isConnected() ? "Connected" : "Disconnected");
            Serial.print("WiFi Status: ");
            Serial.println(WiFi.isConnected() ? "Connected" : "Disconnected");
            Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
        }
        else if (command == "reverbclean")
        {
            Serial.println("Cleaning up Reverb client...");
            reverb.cleanup();
        }
        else if (command == "reverbstart")
        {
            if (WiFi.isConnected()) {
                Serial.println("Starting Reverb client...");
                
                constexpr char HOST[] = "portal.tilkietalkie.com";
                constexpr uint16_t PORT = 443;
                constexpr char APP_KEY[] = "erko2001";
                
                const String token = config.getJWTToken();
                uint64_t chipid = ESP.getEfuseMac();
                char deviceId[14];
                snprintf(deviceId, sizeof(deviceId), "%llu", chipid);
                
                reverb.begin(HOST, PORT, APP_KEY, token.c_str(), deviceId);
                reverb.onChatMessage(handleChatMessage);
            } else {
                Serial.println("Cannot start Reverb - WiFi not connected");
            }
        }
        else if (command == "testauth")
        {
            Serial.println("\n--- Testing Authorization ---");
            String token = config.getJWTToken();
            
            if (token.length() == 0) {
                Serial.println("❌ No JWT token stored in configuration");
                return;
            }
            
            if (!WiFi.isConnected()) {
                Serial.println("❌ WiFi not connected - cannot test authorization");
                return;
            }
            
            Serial.println("🔑 JWT Token found, testing with server...");
            Serial.printf("Token length: %d characters\n", token.length());
            
            // Use the same method as ReverbClient for consistency
            WiFiClientSecure client;
            client.setInsecure(); // For testing only
            
            HTTPClient http;
            String url = "https://portal.tilkietalkie.com/api/user"; // Simple endpoint to test auth
            
            if (http.begin(client, url)) {
                http.addHeader("Authorization", "Bearer " + token);
                http.addHeader("Accept", "application/json");
                
                Serial.println("📡 Sending auth test request...");
                int httpCode = http.GET();
                
                if (httpCode == 200) {
                    Serial.println("✅ Authorization successful! Token is valid.");
                    String response = http.getString();
                    Serial.println("Server response: " + response);
                } else if (httpCode == 401) {
                    Serial.println("❌ Authorization failed! Token is invalid or expired.");
                } else if (httpCode > 0) {
                    Serial.printf("⚠️ Unexpected response code: %d\n", httpCode);
                    String response = http.getString();
                    Serial.println("Response: " + response);
                } else {
                    Serial.printf("❌ HTTP request failed with error: %d\n", httpCode);
                }
                
                http.end();
            } else {
                Serial.println("❌ Failed to connect to server");
            }
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
        else if (command == "forcedownload")
        {
            Serial.println("Forcing download processing (ignoring charging requirement)...");
            Serial.println("Note: This will attempt to download one file at a time for testing purposes.");
            fileManager.forceProcessDownloads();
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
        // Delete all required files command
        else if (command == "delete")
        {
            Serial.println("⚠️  WARNING: This will delete ALL required files from NVS and storage!");
            Serial.println("Are you sure? Type 'yes' to confirm:");
            
            // Wait for confirmation
            unsigned long confirmTimeout = millis() + 10000; // 10 second timeout
            bool confirmed = false;
            
            while (millis() < confirmTimeout && !confirmed)
            {
                if (Serial.available())
                {
                    String confirmation = Serial.readStringUntil('\n');
                    confirmation.trim();
                    confirmation.toLowerCase();
                    
                    if (confirmation == "yes")
                    {
                        confirmed = true;
                        Serial.println("Confirmation received. Deleting all required files...");
                        fileManager.clearAllRequiredFiles();
                        Serial.println("✅ All required files have been deleted from NVS and storage.");
                    }
                    else if (confirmation == "no" || confirmation.length() > 0)
                    {
                        Serial.println("❌ Operation cancelled.");
                        break;
                    }
                }
                delay(100);
            }
            
            if (!confirmed && millis() >= confirmTimeout)
            {
                Serial.println("❌ Confirmation timeout. Operation cancelled.");
            }
        }
        // Delete figure-specific files command
        else if (command.startsWith("deletefig "))
        {
            // Parse deletefig command: deletefig <figure_uid>
            int firstSpace = command.indexOf(' ');

            if (firstSpace != -1)
            {
                String figureUid = command.substring(firstSpace + 1);
                figureUid.trim(); // Remove any extra whitespace

                if (figureUid.isEmpty())
                {
                    Serial.println("Usage: deletefig <figure_uid>");
                    Serial.println("Example: deletefig c538b083-28c1-384b-ae6d-e58e1f38f1f7");
                    Serial.println("Note: This will delete all files associated with the figure");
                }
                else
                {
                    Serial.printf("🔍 Looking up figure ID for UID: %s\n", figureUid.c_str());
                    
                    // Try to get figure ID from UID mapping
                    String figureId = requestManager.getFigureIdFromUid(figureUid);
                    
                    if (figureId.length() > 0)
                    {
                        Serial.printf("Found figure ID: %s for UID: %s\n", figureId.c_str(), figureUid.c_str());
                        Serial.printf("⚠️  WARNING: This will delete all files for figure (UID: %s, ID: %s)\n", figureUid.c_str(), figureId.c_str());
                        Serial.println("Type 'yes' to confirm deletion, or 'no' to cancel:");
                        
                        // Wait for confirmation
                        unsigned long confirmTimeout = millis() + 10000; // 10 second timeout
                        bool confirmed = false;
                        
                        while (millis() < confirmTimeout && !confirmed)
                        {
                            if (Serial.available())
                            {
                                String confirmation = Serial.readStringUntil('\n');
                                confirmation.trim();
                                confirmation.toLowerCase();
                                
                                if (confirmation == "yes")
                                {
                                    confirmed = true;
                                    Serial.printf("Deleting all files for figure ID: %s\n", figureId.c_str());
                                    
                                    if (fileManager.deleteFigureFiles(figureId))
                                    {
                                        Serial.printf("✅ Successfully deleted all files for figure (UID: %s, ID: %s)\n", figureUid.c_str(), figureId.c_str());
                                    }
                                    else
                                    {
                                        Serial.printf("❌ Failed to delete files for figure (UID: %s, ID: %s)\n", figureUid.c_str(), figureId.c_str());
                                    }
                                }
                                else if (confirmation == "no" || confirmation.length() > 0)
                                {
                                    Serial.println("❌ Operation cancelled.");
                                    break;
                                }
                            }
                            delay(100);
                        }
                        
                        if (!confirmed && millis() >= confirmTimeout)
                        {
                            Serial.println("❌ Confirmation timeout. Operation cancelled.");
                        }
                    }
                    else
                    {
                        Serial.printf("❌ Figure ID not found for UID: %s\n", figureUid.c_str());
                        Serial.println("This could mean:");
                        Serial.println("1. The figure was never downloaded/tracked in this session");
                        Serial.println("2. The UID is incorrect");
                        Serial.println("3. You can manually delete by figure ID if you know it");
                        
                        // List available figure directories as a hint
                        Serial.println("\nAvailable figure directories:");
                        std::vector<String> figureDirectories = fileManager.listFiles("/figures");
                        if (figureDirectories.empty())
                        {
                            Serial.println("  (No figure directories found)");
                        }
                        else
                        {
                            for (const String& figureDir : figureDirectories)
                            {
                                Serial.printf("  - Figure ID: %s\n", figureDir.c_str());
                            }
                            Serial.println("\nYou can use 'deletefig <figure_id>' if you know the correct figure ID.");
                        }
                    }
                }
            }
            else
            {
                Serial.println("Usage: deletefig <figure_uid>");
                Serial.println("Example: deletefig c538b083-28c1-384b-ae6d-e58e1f38f1f7");
                Serial.println("Note: This will delete all files associated with the figure");
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
        else if (command == "charging")
        {
            Serial.println("\n--- Charging Status for Downloads ---");
            bool chargingStatus = battery.getChargingStatus();
            Serial.println("Charging status: " + String(chargingStatus ? "CHARGING" : "NOT CHARGING"));
            Serial.println("Charging required for downloads: " + String(chargingStatus ? "Met" : "NOT MET"));
            Serial.println("Download queue size: " + String(fileManager.getPendingDownloadsCount()));
            Serial.println("Download in progress: " + String(fileManager.isDownloadInProgress() ? "Yes" : "No"));
            Serial.println("SD Card initialized: " + String(fileManager.isSDCardAvailable() ? "Yes" : "No"));
            Serial.println("WiFi connected: " + String(WiFi.isConnected() ? "Yes" : "No"));
            
            if (!chargingStatus) {
                Serial.println("\n⚠️  ISSUE: Device is not charging!");
                Serial.println("Downloads will not start until device is connected to power.");
                Serial.println("Use 'forcedownload' command to bypass charging requirement for testing.");
            }
            
            Serial.println("Note: Downloads only start when device is charging");
            Serial.println("---------------------------------------\n");
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
            Serial.println("WiFi connected: " + String(WiFi.isConnected()));
            Serial.println("Has WiFi credentials: " + String(config.hasWiFiCredentials()));
            Serial.println("WiFi SSID length: " + String(config.getWiFiSSID().length()));
            Serial.println("WiFi Password length: " + String(config.getWiFiPassword().length()));
            Serial.println("Note: BLE is automatically managed by ESP32 provisioning library");
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
    
    // Handle WiFi background reconnection (only when credentials exist but not connected)
    wifiProv.handleBackgroundReconnection();

    // Update Reverb client only if WiFi is connected
    if (WiFi.isConnected()) {
        reverb.update();
    }
    
    // Update LED controller (handles pulse and pulseRapid animations)
    ledController.update();

    // Update NFC controller (handles reed switch monitoring and NFC reading)
    nfcController.update();

}