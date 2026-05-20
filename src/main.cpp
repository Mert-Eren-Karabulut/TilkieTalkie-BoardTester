#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <AsyncTCP.h>
#include <wifi_provisioning/manager.h>
#include "ConfigManager.h"
#include "WiFiProvisioning.h"
#include "BatteryManagement.h"
#include "FileManager.h"
#include "AudioController.h"
#include "LedController.h"
#include "NfcController.h"
#include "RequestManager.h"
#include "ReverbClient.h"
#include "Buttons.h"
#include "AsyncSpeedTest.h"
#include "SleepController.h"

static const char *TAG = "MAIN";

// Helper macros to ensure Serial output appears via ESP-IDF logging
#define LOG_PRINTF(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#define LOG_PRINT(tag, msg) ESP_LOGI(tag, "%s", msg)

// Bypass PSRAM test to prevent early boot issues
BYPASS_SPIRAM_TEST(true);

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
ButtonController &buttonController = ButtonController::getInstance();
SleepController &sleepController = SleepController::getInstance();

const bool DEBUG = true; // Set to false to disable debug prints

// Heap monitoring constants
const size_t CRITICAL_HEAP_THRESHOLD = 15000; // 15KB critical threshold
const size_t WARNING_HEAP_THRESHOLD = 25000;  // 25KB warning threshold

String getDeviceEfuseMacDecimal()
{
    char deviceId[21];
    snprintf(deviceId, sizeof(deviceId), "%llu", ESP.getEfuseMac());
    return String(deviceId);
}

String getDeviceEfuseMacHex()
{
    uint64_t mac = ESP.getEfuseMac();
    char macHex[18];
    snprintf(macHex, sizeof(macHex), "%02X:%02X:%02X:%02X:%02X:%02X",
             (uint8_t)(mac >> 40),
             (uint8_t)(mac >> 32),
             (uint8_t)(mac >> 24),
             (uint8_t)(mac >> 16),
             (uint8_t)(mac >> 8),
             (uint8_t)mac);
    return String(macHex);
}

// +++ Reverb WebSocket Callback Function +++
void handleChatMessage(const String &message)
{
    ESP_LOGI(TAG, "[REVERB] Message Received: %s", message.c_str());

    // Example action: Pulse the LED blue when a message comes in
    ledController.pulseRapid(0x0000FF, 2); // Blue color

    // You can add more complex logic here, e.g., parsing the message
    // if (message == "play_sound") {
    //   audioController.play("/sounds/notification.mp3");
    // }
}

// +++ NFC Callback Functions +++

// Callback function for when figure download is complete
void onFigureDownloadComplete(const String &uid, const String &figureName, bool success, const String &error, const RequestManager::Figure &figure)
{
    ESP_LOGI(TAG, "=== Figure Download Complete ===");
    ESP_LOGI(TAG, "UID: %s", uid.c_str());
    ESP_LOGI(TAG, "Figure: %s", figureName.c_str());
    ESP_LOGI(TAG, "Success: %s", success ? "YES" : "NO");

    if (success)
    {
        ESP_LOGI(TAG, "All tracks are ready! Checking if figure is still mounted...");

        // Check if the figure is still mounted on the device
        if (nfcController.isCardPresent() && nfcController.currentNFCData().uidString == uid)
        {
            ESP_LOGI(TAG, "Figure is still mounted! Starting automatic playback...");
            // Pulse LED green to indicate success
            // ledController.pulseRapid(0x00FF00, 2); // Green color, 2 rapid pulses

            // Create the playback order from manifest contents first, then append custom tracks.
            std::vector<String> playlist;
            for (const auto &content : figure.contents)
            {
                for (const auto &episode : content.episodes)
                {
                    for (const auto &track : episode.tracks)
                    {
                        playlist.push_back(track.localPath);
                        ESP_LOGI(TAG, "Added content track: %s (%s)", track.localPath.c_str(), track.name.c_str());
                    }
                }
            }

            for (const auto &track : figure.customTracks)
            {
                playlist.push_back(track.localPath);
                ESP_LOGI(TAG, "Added custom track: %s (%s)", track.localPath.c_str(), track.name.c_str());
            }

            if (!playlist.empty())
            {
                // Set the playlist and start playing
                audioController.setPlaylist(playlist, uid);
                audioController.play(); // Start playing the first track
                ESP_LOGI(TAG, "Started playing figure '%s' with %d tracks", figureName.c_str(), playlist.size());
            }
            else
            {
                ESP_LOGW(TAG, "No tracks found in figure structure!");
            }
        }
        else
        {
            ESP_LOGI(TAG, "Figure is no longer mounted. Not starting playback.");
        }

        ESP_LOGI(TAG, "Figure is ready for playback!");
    }
    else
    {
        ESP_LOGE(TAG, "Download failed: %s", error.c_str());

        // Pulse LED red to indicate failure
        ledController.pulseRapid(0xFF0000, 5); // Red color, 5 rapid pulses

        ESP_LOGW(TAG, "Some tracks may be missing. Check download status.");
    }

    ESP_LOGI(TAG, "================================");
}

// This function will be called ONLY ONCE when a new card is detected
void afterNFCRead(const NFCData &nfcData)
{
    ESP_LOGI(TAG, "=== Hook: afterNFCRead ===");
    ESP_LOGI(TAG, "Card UID: %s", nfcData.uidString.c_str());
    ESP_LOGI(TAG, "Timestamp: %lu", nfcData.timestamp);

    // Example: Play a sound and turn the LED green
    // audioController.play("/sounds/nfc_success.mp3");
    ledController.pulseRapid(0x00FF00, 3); // Green color
    // we need to check if the figure tracks are downloaded and they exist
    // we need to send get request with bearer token to the url :https://portal.tilkietalkie.com/api/units/{nfc_uid}
    requestManager.getCheckFigureTracks(nfcData.uidString);

    ESP_LOGI(TAG, "==========================");
}

// This function will be called when the reed switch is deactivated
// AFTER a card was successfully read in that session.
void afterDetachNFC()
{
    ESP_LOGI(TAG, "=== Hook: afterDetachNFC ===");
    ESP_LOGI(TAG, "NFC session has ended.");

    // Stop audio and clear the playlist
    audioController.stop();
    audioController.clearPlaylist();
    ledController.pulseRapid(0xFF0000, 3); // Red color

    ESP_LOGI(TAG, "Playlist cleared due to figure removal.");
    ESP_LOGI(TAG, "==========================");
}

void setup()
{
    if (DEBUG)
    {
        Serial.begin(115200);
        delay(1000); // Give serial time to initialize
    }
    ESP_LOGI(TAG, "=== TilkieTalkie Board Tester ===");
    ESP_LOGI(TAG, "Initializing system...");

    // Enable the shared 4V5 peripheral rail on GPIO4 before SD/NFC/audio init.
    ESP_LOGI(TAG, "Enabling peripheral power...");
    pinMode(4, OUTPUT);
    digitalWrite(4, HIGH); // Enable power to peripherals
    // Initialize file manager
    delay(500); // Give peripherals time to power up

    // // Initialize configuration (this will also initialize NVS)
    // ESP_LOGI(TAG, "Loading configuration...");
    // config.printAllSettings();

    // Initialize WiFi provisioning
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifiProv.begin();
    if (!fileManager.begin())
    {
        ESP_LOGW(TAG, "File Manager initialization failed!");
        ESP_LOGW(TAG, "SD card functionality will not be available.");
    }
    // Set up figure download complete callback (before WiFi connection)
    requestManager.setFigureDownloadCompleteCallback(onFigureDownloadComplete);

    // Initialize battery management
    battery.begin();

    // Initialize audio controller
    if (!audioController.begin())
    {
        ESP_LOGW(TAG, "Audio Controller initialization failed!");
        ESP_LOGW(TAG, "Audio functionality will not be available.");
    }

    // Initialize LED controller
    ESP_LOGI(TAG, "Initializing LED Controller...");
    ledController.begin();
    ESP_LOGI(TAG, "LED Controller initialized successfully!");

    // --- NEW: Initialize Reverb Client ---
    ESP_LOGI(TAG, "Initializing Reverb WebSocket Client...");

    // Wait for WiFi to connect before starting Reverb client
    unsigned long wifi_timeout = millis() + 5000; // 5 second timeout
    while (!WiFi.isConnected() && millis() < wifi_timeout)
    {
        delay(500);
        ESP_LOGI(TAG, ".");
    }
    ESP_LOGI(TAG, "");

    if (WiFi.isConnected())
    {
        ESP_LOGI(TAG, "WiFi is connected. Initializing network services...");

        // Initialize request manager (needs WiFi)
        if (!requestManager.begin())
        {
            ESP_LOGW(TAG, "Request Manager initialization failed!");
            ESP_LOGW(TAG, "API functionality may be limited.");
        }
        else
        {
            ESP_LOGI(TAG, "Request Manager initialized successfully");
        }

        // Use your production credentials from your Laravel .env file
        constexpr char HOST[] = "portal.tilkietalkie.com";
        constexpr uint16_t PORT = 443;
        constexpr char APP_KEY[] = "erko2001"; // Your REVERB_APP_KEY

        const String token = config.getJWTToken();

        // Generate a unique device ID from the ESP32's MAC address
        String deviceId = getDeviceEfuseMacDecimal();

        reverb.begin(HOST, PORT, APP_KEY, token.c_str(), deviceId.c_str());

        // Register the callback function to handle incoming messages
        reverb.onChatMessage(handleChatMessage);
    }
    else
    {
        ESP_LOGW(TAG, "WiFi connection timed out.");
        ESP_LOGW(TAG, "Network services (API, Reverb) will not be started.");
    }

    // --- Initialize NFC Controller ---
    ESP_LOGI(TAG, "Initializing NFC Controller...");

    // If network is available, wait for Reverb to connect before proceeding
    if (WiFi.isConnected())
    {
        ESP_LOGI(TAG, "Network available, waiting for Reverb to connect...");
        unsigned long reverbTimeout = millis() + 10000; // 10 second timeout
        while (!reverb.isConnected() && millis() < reverbTimeout)
        {
            reverb.update(); // Keep updating Reverb to help it connect
            delay(100);
        }

        if (reverb.isConnected())
        {
            ESP_LOGI(TAG, "Reverb connected successfully!");
        }
        else
        {
            ESP_LOGW(TAG, "Reverb connection timeout, proceeding anyway");
        }
    }

    if (nfcController.begin())
    {
        ESP_LOGI(TAG, "NFC Controller initialized successfully!");

        // Set up the new NFC callbacks
        nfcController.setAfterNFCReadCallback(afterNFCRead);
        nfcController.setAfterDetachNFCCallback(afterDetachNFC);

        ESP_LOGI(TAG, "NFC callbacks configured.");
    }
    else
    {
        ESP_LOGE(TAG, "FATAL: NFC Controller initialization failed!");
        ESP_LOGE(TAG, "NFC functionality will not be available.");
        // Handle failure, maybe by pulsing an error color
        ledController.pulseLed(0xFF0000); // Pulse red for error
    }

    // Initialize other modules here
    // e.g., sensors, etc.

    // Initialize Button Controller
    ESP_LOGI(TAG, "Initializing Button Controller...");
    buttonController.begin();

    // Set up button callbacks
    buttonController.onSingleClick([](ButtonController::ButtonId button)
                                   {
        // Reset sleep timer on any button activity
        sleepController.resetActivity();
        
        ESP_LOGI(TAG, "Single click on button %d", button + 1);
        // Example: Different actions for different buttons
        switch(button) {
            case ButtonController::BUTTON_1:
                // Button 1: Toggle playback
                ESP_LOGI(TAG, "Button 1: Toggle playback");
                ledController.pulseLed(0x0000FF); // Blue pulse
                if (audioController.isPlaying()) {
                    audioController.pause();
                } else if (audioController.isPaused()) {
                    audioController.resume();
                } else if (audioController.isStopped()) {
                    //check if there is a playlist set
                    if (audioController.hasPlaylist()) {
                        audioController.play(); // Start playing the first track
                    } else {
                        audioController.stop(); // Stop playback if no playlist is set
                    }
                }
                break;
            case ButtonController::BUTTON_2:
                // Button 2: Next track
                ESP_LOGI(TAG, "Button 2: Next track");
                ledController.pulseLed(0x00FF00); // Green pulse
                audioController.nextTrack();
                break;
            case ButtonController::BUTTON_3:
                // Button 3: Previous track
                ESP_LOGI(TAG, "Button 3: Previous track");
                ledController.pulseLed(0xFFFF00); // Yellow pulse
                audioController.prevTrack();
                break;
            case ButtonController::BUTTON_4:
                // Button 4: Menu/Settings
                ESP_LOGI(TAG, "Button 4: Menu/Settings");
                ledController.pulseLed(0xFF00FF); // Magenta pulse
                break;
        } });

    buttonController.onHoldStart([](ButtonController::ButtonId button, unsigned long duration)
                                 {
        // Reset sleep timer on button hold
        sleepController.resetActivity();
        
        ESP_LOGI(TAG, "Hold started on button %d (duration: %lu ms)", button + 1, duration);
        // Example: Volume control setup
        if (button == ButtonController::BUTTON_2 || button == ButtonController::BUTTON_4) {
            ESP_LOGI(TAG, "Starting volume %s", (button == ButtonController::BUTTON_2) ? "up" : "down");
        } });

    buttonController.onHoldContinuous([](ButtonController::ButtonId button, unsigned long duration)
                                      {
        // Example: Continuous volume adjustment
        if (button == ButtonController::BUTTON_2) {
            // Volume up - called every 100ms while holding
            ESP_LOGI(TAG, "Volume up (held for %lu ms)", duration);
            audioController.volumeUp(); // Increase volume by 1 step
        } else if (button == ButtonController::BUTTON_4) {
            // Volume down - called every 100ms while holding
            ESP_LOGI(TAG, "Volume down (held for %lu ms)", duration);
            audioController.volumeDown(); // Decrease by 1 step
        } });

    buttonController.onHoldEnd([](ButtonController::ButtonId button, unsigned long duration)
                               {
                                   ESP_LOGI(TAG, "Hold ended on button %d (total duration: %lu ms)", button + 1, duration);
                                   // Volume adjustment finished
                               });

    buttonController.onComboHold([]()
                                 {
                                     ESP_LOGW(TAG, "COMBO HOLD TRIGGERED (1+3) - RESTARTING DEVICE!");
                                     ledController.pulseRapid(0xFF0000, 5); // Rapid red pulse
                                     delay(2000);                           // Give time for LED animation
                                     ESP.restart();                         // Restart the device
                                 });

    buttonController.onComboHold2([]()
                                  {
                                      ESP_LOGW(TAG, "COMBO HOLD TRIGGERED (2+4) - RESETTING WIFI & RESTARTING!");
                                      ledController.pulseRapid(0xFF00FF, 5); // Rapid magenta pulse
                                      delay(1000);                           // Give time for LED animation
                                      wifiProv.reset();                      // Reset WiFi provisioning
                                      delay(1000);                           // Give time for reset
                                      ESP.restart();                         // Restart the device
                                  });

    ESP_LOGI(TAG, "Button Controller initialized successfully!");

    // Initialize Sleep Controller
    ESP_LOGI(TAG, "Initializing Sleep Controller...");
    sleepController.begin();

    // Set inactivity timeout to 5 minutes
    sleepController.setInactivityTimeout(300000); // 5 minutes
 
    // Optional: Set a sleep callback to be called before entering sleep
    sleepController.onSleep([]()
                            {
                                ESP_LOGI(TAG, "Device is about to enter deep sleep...");
                                ledController.pulseRapid(0xFFFF00, 3); // Yellow pulse before sleep
                                delay(1000);                           // Give time for LED animation
                                battery.prepareForDeepSleep();
                            });

    ESP_LOGI(TAG, "Sleep Controller initialized successfully!");

    // rapid pulse LED to indicate system is ready
    ledController.pulseRapid(0x00FF00, 3); // Rapid pulse green
    // audioController.play("/sounds/12.mp3"); // Play startup sound
}
static unsigned long lastFreeCall = 0;
static unsigned long lastStackCheck = 0;

void loop()
{
    // Stack monitoring every 10 seconds
    unsigned long now = millis();
    if (now - lastStackCheck > 10000)
    {
        lastStackCheck = now;
        UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
        if (stackHighWaterMark < 1000)
        { // Less than 1KB remaining
            ESP_LOGW(TAG, "WARNING: Low stack space remaining: %d bytes", stackHighWaterMark * sizeof(StackType_t));
        }
    }

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
            ESP_LOGI(TAG, "--- Terminal Commands ---");
            ESP_LOGI(TAG, "WiFi Commands:");
            ESP_LOGI(TAG, "  qr      - Print QR code for provisioning");
            ESP_LOGI(TAG, "  reset   - Reset WiFi provisioning");
            ESP_LOGI(TAG, "  stats   - Show WiFi connection status");
            ESP_LOGI(TAG, "Reverb Commands:");
            ESP_LOGI(TAG, "  reverbstatus - Show Reverb connection status");
            ESP_LOGI(TAG, "  reverbclean  - Clean up Reverb client to free memory");
            ESP_LOGI(TAG, "  reverbstart  - Start Reverb client (needs WiFi)");
            ESP_LOGI(TAG, "  testauth     - Test stored JWT token authorization with server");
            ESP_LOGI(TAG, "System Commands:");
            ESP_LOGI(TAG, "  restart - Restart the device");
            ESP_LOGI(TAG, "  config  - Show all configuration");
            ESP_LOGI(TAG, "  debug   - Show debug information");
            ESP_LOGI(TAG, "  efusemac - Show device efuse MAC in decimal and hex");
            ESP_LOGI(TAG, "  heap    - Show detailed heap information");
            ESP_LOGI(TAG, "  stack   - Show stack usage information");
            ESP_LOGI(TAG, "  factory - Factory reset (erase all data)");
            ESP_LOGI(TAG, "  speedtest - Test network download speed (AsyncTCP Module)");
            ESP_LOGI(TAG, "Battery Commands:");
            ESP_LOGI(TAG, "  battery - Show battery status");
            ESP_LOGI(TAG, "  battscan - Scan the battery I2C bus for responding devices");
            ESP_LOGI(TAG, "  batterycfg - Reapply 4V5 rail and charger defaults");
            ESP_LOGI(TAG, "  batteryclearflags - Clear latched BQ25792 charger/fault flags");
            ESP_LOGI(TAG, "  chargekick - Toggle BQ25792 EN_CHG off/on to restart charging");
            ESP_LOGI(TAG, "  gaugefets - Re-enable gauge FET control if it is disabled");
            ESP_LOGI(TAG, "  gauge1s - Clear BQ28 DA Configuration CC0 to force 1-cell mode");
            ESP_LOGI(TAG, "  gaugeprog - Force generic gauge provisioning");
            ESP_LOGI(TAG, "  gaugeresetlearn - Reset BQ28 learning state to a fresh relearn baseline");
            ESP_LOGI(TAG, "File Manager Commands:");
            ESP_LOGI(TAG, "  sdtree  - Check SD card file tree");
            ESP_LOGI(TAG, "  sdformat- Format SD card as FAT32");
            ESP_LOGI(TAG, "  deletefile <path> - Delete file from SD card");
            ESP_LOGI(TAG, "  delete  - Delete ALL required files from NVS and storage");
            ESP_LOGI(TAG, "  deletefig <uid> - Delete all files for a specific figure");
            ESP_LOGI(TAG, "  dlstats - Show download statistics");
            ESP_LOGI(TAG, "  dlqueue - Show download queue");
            ESP_LOGI(TAG, "  required- Show required files");
            ESP_LOGI(TAG, "  download <url> <path> - Download file from URL");
            ESP_LOGI(TAG, "  addfile <path> <url> - Add required file");
            ESP_LOGI(TAG, "  checkfiles - Check and download missing files");
            ESP_LOGI(TAG, "  cleanup - Clean up temporary files");
            ESP_LOGI(TAG, "Audio Commands:");
            ESP_LOGI(TAG, "  play <path> - Play mp3 file");
            ESP_LOGI(TAG, "  pause   - Pause current playback");
            ESP_LOGI(TAG, "  resume  - Resume paused playback");
            ESP_LOGI(TAG, "  stop    - Stop playback");
            ESP_LOGI(TAG, "  volup   - Volume up");
            ESP_LOGI(TAG, "  voldown - Volume down");
            ESP_LOGI(TAG, "  volume  - Show current volume");
            ESP_LOGI(TAG, "  track   - Show current track");
            ESP_LOGI(TAG, "LED Commands:");
            ESP_LOGI(TAG, "  ledon <hex> <intensity> - Turn LED on with hex color and intensity (0-255)");
            ESP_LOGI(TAG, "  ledoff  - Turn LED off");
            ESP_LOGI(TAG, "  pulse <hex> - Start pulsing LED with hex color");
            ESP_LOGI(TAG, "  rapid <hex> <count> - Rapid pulse LED for count times");
            ESP_LOGI(TAG, "NFC Commands:");
            ESP_LOGI(TAG, "  nfcstatus - Show NFC controller status");
            ESP_LOGI(TAG, "  nfcdata   - Show current NFC card data");
            ESP_LOGI(TAG, "  nfcreed   - Show reed switch status");
            ESP_LOGI(TAG, "  nfcdiag   - Run NFC diagnostics");
            ESP_LOGI(TAG, "Power Commands:");
            ESP_LOGI(TAG, "  power   - Show peripheral power status");
            ESP_LOGI(TAG, "  poweron - Enable peripheral power (IO17)");
            ESP_LOGI(TAG, "  poweroff- Disable peripheral power (IO17)");
            ESP_LOGI(TAG, "Sleep Commands:");
            ESP_LOGI(TAG, "  sleep   - Enter deep sleep immediately");
            ESP_LOGI(TAG, "  sleepafter <ms> - Schedule sleep after specified milliseconds");
            ESP_LOGI(TAG, "  cancelsleep - Cancel scheduled sleep");
            ESP_LOGI(TAG, "  sleepstatus - Show sleep controller status");
            ESP_LOGI(TAG, "Reverb Commands:");
            ESP_LOGI(TAG, "  send <message> - Send message to Reverb API for broadcast");
            ESP_LOGI(TAG, "  wsstatus - Show WebSocket connection status");
            ESP_LOGI(TAG, "  testauth - Test stored JWT token authorization with server");
            ESP_LOGI(TAG, "Type any command for help");
            return; // Skip further processing
        }
        // WebSocket commands
        if (command.startsWith("send "))
        {
            String message = command.substring(5); // Get the text after "send "
            message.trim();
            if (message.length() > 0)
            {
                ESP_LOGI(TAG, "Sending message: '%s'", message.c_str());
                if (reverb.sendMessage(message))
                {
                    ESP_LOGI(TAG, "Message sent to API for broadcast.");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to send message.");
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: send <your message>");
            }
        }

        // WiFi related commands
        if (command == "qr" || command == "reset" || command == "stats")
        {
            wifiProv.handleCommand(command);
        }
        // Reverb related commands
        else if (command == "reverbstatus")
        {
            ESP_LOGI(TAG, "Reverb Status: %s", reverb.isConnected() ? "Connected" : "Disconnected");
            ESP_LOGI(TAG, "WiFi Status: %s", WiFi.isConnected() ? "Connected" : "Disconnected");
            ESP_LOGI(TAG, "Free heap: %d bytes", ESP.getFreeHeap());
        }
        else if (command == "reverbclean")
        {
            ESP_LOGI(TAG, "Cleaning up Reverb client...");
            reverb.cleanup();
        }
        else if (command == "reverbstart")
        {
            if (WiFi.isConnected())
            {
                ESP_LOGI(TAG, "Starting Reverb client...");

                constexpr char HOST[] = "portal.tilkietalkie.com";
                constexpr uint16_t PORT = 443;
                constexpr char APP_KEY[] = "erko2001";

                const String token = config.getJWTToken();
                const String deviceId = getDeviceEfuseMacDecimal();

                reverb.begin(HOST, PORT, APP_KEY, token.c_str(), deviceId.c_str());
                reverb.onChatMessage(handleChatMessage);
            }
            else
            {
                ESP_LOGW(TAG, "Cannot start Reverb - WiFi not connected");
            }
        }
        else if (command == "efusemac" || command == "hubid")
        {
            const String deviceId = getDeviceEfuseMacDecimal();
            const String deviceMacHex = getDeviceEfuseMacHex();

            ESP_LOGI(TAG, "--- Device Identity ---");
            ESP_LOGI(TAG, "Hub UUID / efuse MAC (decimal): %s", deviceId.c_str());
            ESP_LOGI(TAG, "Base MAC (hex): %s", deviceMacHex.c_str());
            ESP_LOGI(TAG, "Backend token endpoint: /api/hubs/%s/token", deviceId.c_str());
            ESP_LOGI(TAG, "Create or bind a Hub with uuid='%s' to your user.", deviceId.c_str());
        }
        else if (command == "testauth")
        {
            ESP_LOGI(TAG, "--- Testing Authorization ---");
            String token = config.getJWTToken();

            if (token.length() == 0)
            {
                ESP_LOGE(TAG, "No JWT token stored in configuration");
                return;
            }

            if (!WiFi.isConnected())
            {
                ESP_LOGE(TAG, "WiFi not connected - cannot test authorization");
                return;
            }

            ESP_LOGI(TAG, "JWT Token found, testing with server...");
            ESP_LOGI(TAG, "Token length: %d characters", token.length());

            // Use the same method as ReverbClient for consistency
            WiFiClientSecure client;
            client.setInsecure(); // For testing only

            HTTPClient http;
            String url = "https://portal.tilkietalkie.com/api/user"; // Simple endpoint to test auth

            if (http.begin(client, url))
            {
                http.addHeader("Authorization", "Bearer " + token);
                http.addHeader("Accept", "application/json");

                ESP_LOGI(TAG, "Sending auth test request...");
                int httpCode = http.GET();

                if (httpCode == 200)
                {
                    ESP_LOGI(TAG, "Authorization successful! Token is valid.");
                    String response = http.getString();
                    ESP_LOGI(TAG, "Server response: %s", response.c_str());
                }
                else if (httpCode == 401)
                {
                    ESP_LOGE(TAG, "Authorization failed! Token is invalid or expired.");
                }
                else if (httpCode > 0)
                {
                    ESP_LOGW(TAG, "Unexpected response code: %d", httpCode);
                    String response = http.getString();
                    ESP_LOGW(TAG, "Response: %s", response.c_str());
                }
                else
                {
                    ESP_LOGE(TAG, "HTTP request failed with error: %d", httpCode);
                }

                http.end();
            }
            else
            {
                ESP_LOGE(TAG, "Failed to connect to server");
            }
        }
        else if (command == "factory" && DEBUG)
        {
            ESP_LOGW(TAG, "WARNING: Factory reset will erase ALL stored data!");
            ESP_LOGI(TAG, "Type 'yes' to confirm or any other key to cancel:");
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
                ESP_LOGI(TAG, "Factory reset cancelled.");
            }
        }
        // File Manager commands
        else if (command == "sdtree")
        {
            fileManager.printFileTree();
        }
        else if (command == "sdformat")
        {
            fileManager.formatSDCard();
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

                ESP_LOGI(TAG, "Scheduling download: %s -> %s", url.c_str(), path.c_str());
                if (fileManager.scheduleDownload(url, path))
                {
                    ESP_LOGI(TAG, "Download scheduled successfully");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to schedule download");
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: download <url> <local_path>");
                ESP_LOGI(TAG, "Example: download http://example.com/audio.mp3 /audio/test.mp3");
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

                ESP_LOGI(TAG, "Adding required file: %s <- %s", path.c_str(), url.c_str());
                if (fileManager.addRequiredFile(path, url))
                {
                    ESP_LOGI(TAG, "Required file added successfully");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to add required file");
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: addfile <local_path> <url>");
                ESP_LOGI(TAG, "Example: addfile /audio/sound.mp3 http://example.com/audio.mp3");
            }
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
                    ESP_LOGI(TAG, "Usage: deletefile <file_path>");
                    ESP_LOGI(TAG, "Example: deletefile /images/image.webp");
                }
                else
                {
                    ESP_LOGI(TAG, "Deleting file and removing from required list: %s", filePath.c_str());
                    if (fileManager.deleteFileAndRemoveFromRequired(filePath))
                    {
                        ESP_LOGI(TAG, "File deleted successfully");
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Failed to delete file (file may not exist)");
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: deletefile <file_path>");
                ESP_LOGI(TAG, "Example: deletefile /images/image.webp");
                ESP_LOGI(TAG, "Note: This will also remove the file from required list to prevent re-download");
            }
        }
        // Delete all required files command
        else if (command == "delete" && DEBUG)
        {
            ESP_LOGW(TAG, "WARNING: This will delete ALL required files from NVS and storage!");
            ESP_LOGI(TAG, "Are you sure? Type 'yes' to confirm:");

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
                        ESP_LOGI(TAG, "Confirmation received. Deleting all required files...");
                        fileManager.clearAllRequiredFiles();
                        ESP_LOGI(TAG, "All required files have been deleted from NVS and storage.");
                    }
                    else if (confirmation == "no" || confirmation.length() > 0)
                    {
                        ESP_LOGI(TAG, "Operation cancelled.");
                        break;
                    }
                }
                delay(100);
            }

            if (!confirmed && millis() >= confirmTimeout)
            {
                ESP_LOGI(TAG, "Confirmation timeout. Operation cancelled.");
            }
        }
        else if (command.startsWith("sdbench"))
        {
            fileManager.benchmarkSDCard();
        }
        // Delete figure-specific files command
        else if (command.startsWith("deletefig ") && DEBUG)
        {
            // Parse deletefig command: deletefig <figure_uid>
            int firstSpace = command.indexOf(' ');

            if (firstSpace != -1)
            {
                String figureUid = command.substring(firstSpace + 1);
                figureUid.trim(); // Remove any extra whitespace

                if (figureUid.isEmpty())
                {
                    ESP_LOGI(TAG, "Usage: deletefig <figure_uid>");
                    ESP_LOGI(TAG, "Example: deletefig c538b083-28c1-384b-ae6d-e58e1f38f1f7");
                    ESP_LOGI(TAG, "Note: This will delete all files associated with the figure");
                }
                else
                {
                    ESP_LOGI(TAG, "Looking up figure ID for UID: %s", figureUid.c_str());

                    // Try to get figure ID from UID mapping
                    String figureId = requestManager.getFigureIdFromUid(figureUid);

                    if (figureId.length() > 0)
                    {
                        ESP_LOGI(TAG, "Found figure ID: %s for UID: %s", figureId.c_str(), figureUid.c_str());
                        ESP_LOGW(TAG, "WARNING: This will delete all files for figure (UID: %s, ID: %s)", figureUid.c_str(), figureId.c_str());
                        ESP_LOGI(TAG, "Type 'yes' to confirm deletion, or 'no' to cancel:");

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
                                    ESP_LOGI(TAG, "Deleting all files for figure ID: %s", figureId.c_str());

                                    if (fileManager.deleteFigureFiles(figureId))
                                    {
                                        ESP_LOGI(TAG, "Successfully deleted all files for figure (UID: %s, ID: %s)", figureUid.c_str(), figureId.c_str());
                                    }
                                    else
                                    {
                                        ESP_LOGE(TAG, "Failed to delete files for figure (UID: %s, ID: %s)", figureUid.c_str(), figureId.c_str());
                                    }
                                }
                                else if (confirmation == "no" || confirmation.length() > 0)
                                {
                                    ESP_LOGI(TAG, "Operation cancelled.");
                                    break;
                                }
                            }
                            delay(100);
                        }

                        if (!confirmed && millis() >= confirmTimeout)
                        {
                            ESP_LOGI(TAG, "Confirmation timeout. Operation cancelled.");
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Figure ID not found for UID: %s", figureUid.c_str());
                        ESP_LOGI(TAG, "This could mean:");
                        ESP_LOGI(TAG, "1. The figure was never downloaded/tracked in this session");
                        ESP_LOGI(TAG, "2. The UID is incorrect");
                        ESP_LOGI(TAG, "3. You can manually delete by figure ID if you know it");

                        // List available figure directories as a hint
                        ESP_LOGI(TAG, "Available figure directories:");
                        std::vector<String> figureDirectories = fileManager.listFiles("/figures");
                        if (figureDirectories.empty())
                        {
                            ESP_LOGI(TAG, "  (No figure directories found)");
                        }
                        else
                        {
                            for (const String &figureDir : figureDirectories)
                            {
                                ESP_LOGI(TAG, "  - Figure ID: %s", figureDir.c_str());
                            }
                            ESP_LOGI(TAG, "You can use 'deletefig <figure_id>' if you know the correct figure ID.");
                        }
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: deletefig <figure_uid>");
                ESP_LOGI(TAG, "Example: deletefig c538b083-28c1-384b-ae6d-e58e1f38f1f7");
                ESP_LOGI(TAG, "Note: This will delete all files associated with the figure");
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
                    ESP_LOGI(TAG, "Usage: play <file_path>");
                    ESP_LOGI(TAG, "Example: play /audio/song.mp3");
                }
                else
                {
                    ESP_LOGI(TAG, "Playing: %s", filePath.c_str());
                    if (audioController.play(filePath))
                    {
                        ESP_LOGI(TAG, "Playback started successfully");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Failed to start playback");
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: play <file_path>");
                ESP_LOGI(TAG, "Example: play /audio/song.mp3");
            }
        }
        else if (command == "play")
        {
            // Play playlist or resume
            if (audioController.play())
            {
                if (audioController.hasPlaylist())
                {
                    ESP_LOGI(TAG, "Playing playlist track %d/%d",
                             audioController.getCurrentTrackIndex() + 1,
                             audioController.getPlaylistSize());
                }
                else
                {
                    ESP_LOGI(TAG, "Playback resumed");
                }
            }
            else
            {
                ESP_LOGW(TAG, "No playlist available or failed to start playback");
            }
        }
        else if (command == "pause")
        {
            if (audioController.pause())
            {
                ESP_LOGI(TAG, "Playback paused");
            }
            else
            {
                ESP_LOGW(TAG, "Nothing to pause or already paused");
            }
        }
        else if (command == "resume")
        {
            if (audioController.resume())
            {
                ESP_LOGI(TAG, "Playback resumed");
            }
            else
            {
                ESP_LOGW(TAG, "Nothing to resume or not paused");
            }
        }
        else if (command == "stop")
        {
            if (audioController.stop())
            {
                ESP_LOGI(TAG, "Playback stopped");
            }
            else
            {
                ESP_LOGW(TAG, "Nothing to stop or already stopped");
            }
        }
        else if (command == "next")
        {
            if (audioController.nextTrack())
            {
                ESP_LOGI(TAG, "Playing next track: %d/%d",
                         audioController.getCurrentTrackIndex() + 1,
                         audioController.getPlaylistSize());
            }
            else
            {
                ESP_LOGW(TAG, "No playlist available or reached end of playlist");
            }
        }
        else if (command == "prev")
        {
            if (audioController.prevTrack())
            {
                ESP_LOGI(TAG, "Playing previous track: %d/%d",
                         audioController.getCurrentTrackIndex() + 1,
                         audioController.getPlaylistSize());
            }
            else
            {
                ESP_LOGW(TAG, "No playlist available");
            }
        }
        else if (command == "playlist")
        {
            if (audioController.hasPlaylist())
            {
                ESP_LOGI(TAG, "Current playlist (Figure UID: %s):",
                         audioController.getPlaylistFigureUid().c_str());
                ESP_LOGI(TAG, "Current track: %d/%d",
                         audioController.getCurrentTrackIndex() + 1,
                         audioController.getPlaylistSize());

                // Print playlist tracks (limit to 10 for readability)
                int maxTracks = min(10, audioController.getPlaylistSize());
                for (int i = 0; i < maxTracks; i++)
                {
                    String indicator = (i == audioController.getCurrentTrackIndex()) ? " -> " : "    ";
                    ESP_LOGI(TAG, "%s%d. Track %d", indicator.c_str(), i + 1, i + 1);
                }

                if (audioController.getPlaylistSize() > 10)
                {
                    ESP_LOGI(TAG, "    ... and %d more tracks",
                             audioController.getPlaylistSize() - 10);
                }
            }
            else
            {
                ESP_LOGI(TAG, "No playlist loaded");
            }
        }
        else if (command == "volup")
        {
            if (audioController.volumeUp())
            {
                ESP_LOGI(TAG, "Volume increased to %d%%", audioController.getCurrentVolume());
            }
            else
            {
                ESP_LOGW(TAG, "Volume already at maximum");
            }
        }
        else if (command == "voldown")
        {
            if (audioController.volumeDown())
            {
                ESP_LOGI(TAG, "Volume decreased to %d%%", audioController.getCurrentVolume());
            }
            else
            {
                ESP_LOGW(TAG, "Volume already at minimum");
            }
        }
        else if (command == "volume")
        {
            ESP_LOGI(TAG, "Current volume: %d%%", audioController.getCurrentVolume());
        }
        else if (command == "track")
        {
            String track = audioController.getCurrentTrack();
            if (track.isEmpty())
            {
                ESP_LOGI(TAG, "No track currently loaded");
            }
            else
            {
                ESP_LOGI(TAG, "Current track: %s", track.c_str());
                ESP_LOGI(TAG, "Status: %s",
                         audioController.isPlaying() ? "Playing" : audioController.isPaused() ? "Paused"
                                                                                              : "Stopped");
            }
        }
        // Power control commands
        else if (command == "power")
        {
            bool powerState = digitalRead(4);
            ESP_LOGI(TAG, "Peripheral power (GPIO4): %s", powerState ? "ENABLED" : "DISABLED");
            ESP_LOGI(TAG, "Pin state: %s", powerState ? "HIGH" : "LOW");
            if (!powerState)
            {
                ESP_LOGW(TAG, "WARNING: Peripherals (SD card, etc.) will not work with power disabled!");
                ESP_LOGI(TAG, "Use 'poweron' command to enable peripheral power.");
            }
        }
        else if (command == "poweron")
        {
            ESP_LOGI(TAG, "Enabling peripheral power...");
            digitalWrite(4, HIGH);
            delay(100);
            ESP_LOGI(TAG, "Peripheral power ENABLED");
            ESP_LOGI(TAG, "You may need to reinitialize modules (restart recommended)");
        }
        else if (command == "poweroff" && DEBUG)
        {
            ESP_LOGW(TAG, "WARNING: This will disable power to SD card and other peripherals!");
            ESP_LOGI(TAG, "Type 'yes' to confirm or any other key to cancel:");
            while (!Serial.available())
            {
                delay(100);
            }
            String confirmation = Serial.readStringUntil('\n');
            confirmation.trim();
            confirmation.toLowerCase();

            if (confirmation == "yes")
            {
                digitalWrite(4, LOW);
                ESP_LOGI(TAG, "Peripheral power DISABLED");
            }
            else
            {
                ESP_LOGI(TAG, "Power-off cancelled.");
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
                ESP_LOGI(TAG, "LED set to color: 0x%06X, intensity: %d", hexColor, intensity);
            }
            else
            {
                ESP_LOGI(TAG, "Usage: ledon <hex_color> <intensity>");
                ESP_LOGI(TAG, "Example: ledon FF0000 128 (red color with 128 intensity)");
            }
        }
        else if (command == "ledoff")
        {
            ledController.turnOff();
            ESP_LOGI(TAG, "LED turned off");
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
                ESP_LOGI(TAG, "LED pulsing started with color: 0x%06X", hexColor);
            }
            else
            {
                ESP_LOGI(TAG, "Usage: pulse <hex_color>");
                ESP_LOGI(TAG, "Example: pulse 00FF00 (green pulsing)");
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
                ESP_LOGI(TAG, "LED rapid pulse started with color: 0x%06X, count: %d", hexColor, count);
            }
            else
            {
                ESP_LOGI(TAG, "Usage: rapid <hex_color> <count>");
                ESP_LOGI(TAG, "Example: rapid 0000FF 5 (blue rapid pulse 5 times)");
            }
        }
        // NFC commands
        else if (command == "nfcstatus")
        {
            ESP_LOGI(TAG, "--- NFC Controller Status ---");
            ESP_LOGI(TAG, "NFC Ready: %s", nfcController.isNFCReady() ? "Yes" : "No");
            ESP_LOGI(TAG, "Pogo Switch Active: %s", nfcController.isPogoSwitchActive() ? "Yes" : "No");
            ESP_LOGI(TAG, "Card Present: %s", nfcController.isCardPresent() ? "Yes" : "No");
            ESP_LOGI(TAG, "-----------------------------");
        }
        else if (command == "nfcdata")
        {
            NFCData currentCard = nfcController.currentNFCData();
            ESP_LOGI(TAG, "--- Currently Docked NFC Card ---");
            if (currentCard.isValid)
            {
                ESP_LOGI(TAG, "UID: %s", currentCard.uidString.c_str());
                ESP_LOGI(TAG, "UID Length: %d", currentCard.uidLength);
                ESP_LOGI(TAG, "Timestamp: %lu", currentCard.timestamp);
            }
            else
            {
                ESP_LOGI(TAG, "No card is currently docked.");
            }
            ESP_LOGI(TAG, "----------------------------------");
        }
        else if (command == "nfcpogo" || command == "nfcreed")
        {
            bool rawPogoState = digitalRead(POGO_SWITCH_PIN);
            ESP_LOGI(TAG, "--- Pogo Switch Status ---");
            ESP_LOGI(TAG, "Raw Pin State (GPIO6): %s", rawPogoState ? "HIGH" : "LOW");
            ESP_LOGI(TAG, "Debounced Controller State: %s", nfcController.isPogoSwitchActive() ? "Active" : "Inactive");
            ESP_LOGI(TAG, "-------------------------");
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
        else if (command == "battscan")
        {
            battery.printBusScan();
        }
        else if (command == "batterycfg")
        {
            bool success = battery.reconfigure(false);
            ESP_LOGI(TAG, "Battery bring-up reapply: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "batteryclearflags")
        {
            bool success = battery.clearChargerFaultHistory();
            ESP_LOGI(TAG, "Charger fault history clear: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "chargekick")
        {
            bool success = battery.restartChargeCycle();
            ESP_LOGI(TAG, "Charge cycle restart: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "gaugefets")
        {
            bool success = battery.restoreGaugeFetControl();
            ESP_LOGI(TAG, "Gauge FET control restore: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "gauge1s")
        {
            bool success = battery.setGaugeSingleCellMode();
            ESP_LOGI(TAG, "Gauge single-cell configuration: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "gaugeprog")
        {
            bool success = battery.forceGaugeProvisioning();
            ESP_LOGI(TAG, "Gauge provisioning: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "gaugeresetlearn")
        {
            bool success = battery.resetGaugeLearningState();
            ESP_LOGI(TAG, "Gauge learning reset: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        // System commands
        else if (command == "restart")
        {
            ESP_LOGW(TAG, "Restarting device...");
            delay(1000);
            ESP.restart();
        }
        else if (command == "config")
        {
            config.printAllSettings();
        }
        else if (command == "debug")
        {
            ESP_LOGI(TAG, "--- Debug Information ---");
            ESP_LOGI(TAG, "Free internal SRAM: %d bytes", ESP.getFreeHeap());
            ESP_LOGI(TAG, "Free PSRAM: %d bytes", ESP.getFreePsram());
            ESP_LOGI(TAG, "Largest free block: %d bytes", ESP.getMaxAllocHeap());
            ESP_LOGI(TAG, "Minimum free heap: %d bytes", ESP.getMinFreeHeap());
            ESP_LOGI(TAG, "Chip revision: %d", ESP.getChipRevision());
            ESP_LOGI(TAG, "SDK version: %s", ESP.getSdkVersion());
            ESP_LOGI(TAG, "WiFi mode: %d", WiFi.getMode());
            ESP_LOGI(TAG, "WiFi status: %d", WiFi.status());
            ESP_LOGI(TAG, "Battery: %s", battery.getBatteryStatusString().c_str());
            ESP_LOGI(TAG, "WiFi connected: %s", WiFi.isConnected() ? "Yes" : "No");
            ESP_LOGI(TAG, "Has WiFi credentials: %s", config.hasWiFiCredentials() ? "Yes" : "No");
            ESP_LOGI(TAG, "WiFi SSID length: %d", config.getWiFiSSID().length());
            ESP_LOGI(TAG, "WiFi Password length: %d", config.getWiFiPassword().length());

            // Check ESP32 WiFi provisioning library status
            bool provisioned = false;
            esp_err_t ret = wifi_prov_mgr_is_provisioned(&provisioned);
            ESP_LOGI(TAG, "ESP32 WiFi Library Provisioned: %s", provisioned ? "Yes" : "No");
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Provisioning check error: %s", esp_err_to_name(ret));
            }

            // Check actual WiFi config stored by ESP32
            wifi_config_t wifi_cfg;
            ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
            if (ret == ESP_OK)
            {
                String storedSSID = String((char *)wifi_cfg.sta.ssid);
                ESP_LOGI(TAG, "ESP32 Stored SSID: %s", storedSSID.length() > 0 ? storedSSID.c_str() : "(none)");
                ESP_LOGI(TAG, "ESP32 Stored Password Length: %d", strlen((char *)wifi_cfg.sta.password));
            }
            else
            {
                ESP_LOGE(TAG, "Failed to get WiFi config: %s", esp_err_to_name(ret));
            }

            ESP_LOGI(TAG, "Note: BLE is automatically managed by ESP32 provisioning library");
            config.printAllSettings();
        }
        // Add new heap command
        else if (command == "heap")
        {
            ESP_LOGI(TAG, "--- Detailed Heap Information ---");

            // Internal SRAM Heap Statistics
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Internal SRAM Heap:");
            ESP_LOGI(TAG, "  Total heap size: %d bytes", ESP.getHeapSize());
            ESP_LOGI(TAG, "  Free heap: %d bytes", ESP.getFreeHeap());
            ESP_LOGI(TAG, "  Largest free block: %d bytes", ESP.getMaxAllocHeap());
            ESP_LOGI(TAG, "  Minimum free heap since boot: %d bytes", ESP.getMinFreeHeap());

            // Calculate internal heap usage
            uint32_t usedHeap = ESP.getHeapSize() - ESP.getFreeHeap();
            float heapUsagePercent = (float)usedHeap / ESP.getHeapSize() * 100;
            ESP_LOGI(TAG, "  Used heap: %d bytes (%.1f%%)", usedHeap, heapUsagePercent);

            // Calculate internal heap fragmentation
            float heapFragmentation = 0;
            if (ESP.getFreeHeap() > 0)
            {
                heapFragmentation = (1.0 - (float)ESP.getMaxAllocHeap() / ESP.getFreeHeap()) * 100;
            }
            ESP_LOGI(TAG, "  Heap fragmentation: %.1f%%", heapFragmentation);

            // External PSRAM Statistics
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "External PSRAM:");
            ESP_LOGI(TAG, "  Total PSRAM size: %d bytes", ESP.getPsramSize());
            ESP_LOGI(TAG, "  Free PSRAM: %d bytes", ESP.getFreePsram());
            ESP_LOGI(TAG, "  Minimum free PSRAM since boot: %d bytes", ESP.getMinFreePsram());

            // Calculate PSRAM usage
            uint32_t usedPsram = ESP.getPsramSize() - ESP.getFreePsram();
            float psramUsagePercent = 0;
            if (ESP.getPsramSize() > 0)
            {
                psramUsagePercent = (float)usedPsram / ESP.getPsramSize() * 100;
            }
            ESP_LOGI(TAG, "  Used PSRAM: %d bytes (%.1f%%)", usedPsram, psramUsagePercent);

            // Overall memory status
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Combined Memory Status:");
            uint32_t totalFree = ESP.getFreeHeap() + ESP.getFreePsram();
            uint32_t totalSize = ESP.getHeapSize() + ESP.getPsramSize();
            ESP_LOGI(TAG, "  Total free memory: %d bytes", totalFree);
            ESP_LOGI(TAG, "  Total memory size: %d bytes", totalSize);

            // Memory health status
            ESP_LOGI(TAG, "");
            if (ESP.getFreeHeap() < CRITICAL_HEAP_THRESHOLD)
            {
                ESP_LOGE(TAG, "Internal Heap Status: CRITICAL - Very low memory");
            }
            else if (ESP.getFreeHeap() < WARNING_HEAP_THRESHOLD)
            {
                ESP_LOGW(TAG, "Internal Heap Status: WARNING - Low memory");
            }
            else
            {
                ESP_LOGI(TAG, "Internal Heap Status: OK - Memory levels normal");
            }

            if (ESP.getPsramSize() > 0)
            {
                if (ESP.getFreePsram() < 100000)
                { // Less than 100KB PSRAM free
                    ESP_LOGW(TAG, "PSRAM Status: WARNING - Low PSRAM");
                }
                else
                {
                    ESP_LOGI(TAG, "PSRAM Status: OK - PSRAM levels normal");
                }
            }
            else
            {
                ESP_LOGW(TAG, "PSRAM Status: No PSRAM detected or disabled");
            }

            ESP_LOGI(TAG, "----------------------------------");
        }
        // Stack monitoring command
        else if (command == "stack")
        {
            ESP_LOGI(TAG, "--- Stack Information ---");
            UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
            size_t stackRemaining = stackHighWaterMark * sizeof(StackType_t);
            ESP_LOGI(TAG, "Stack high water mark: %d words (%d bytes)",
                     stackHighWaterMark, stackRemaining);

            // Estimate stack usage (assuming 16KB total from build flags)
            size_t totalStack = 16384; // From CONFIG_ARDUINO_LOOP_STACK_SIZE
            size_t usedStack = totalStack - stackRemaining;
            float usagePercent = (float)usedStack / totalStack * 100;

            ESP_LOGI(TAG, "Estimated stack usage: %d/%d bytes (%.1f%%)",
                     usedStack, totalStack, usagePercent);

            if (stackRemaining < 1000)
            {
                ESP_LOGE(TAG, "Status: CRITICAL - Very low stack space");
            }
            else if (stackRemaining < 2000)
            {
                ESP_LOGW(TAG, "Status: WARNING - Low stack space");
            }
            else
            {
                ESP_LOGI(TAG, "Status: OK - Stack levels normal");
            }
            ESP_LOGI(TAG, "------------------------");
        }
        // AsyncTCP speed test command
        else if (command == "speedtest")
        {
            speedTest.start();
        }
        // Sleep commands
        else if (command == "sleep")
        {
            ESP_LOGI(TAG, "Entering deep sleep immediately...");
            sleepController.checkAndSleep();
        }
        else if (command.startsWith("sleepafter "))
        {
            String timeStr = command.substring(11);
            unsigned long timeMs = timeStr.toInt();
            if (timeMs > 0)
            {
                sleepController.scheduleSleep(timeMs);
                ESP_LOGI(TAG, "Sleep scheduled after %lu ms", timeMs);
            }
            else
            {
                ESP_LOGI(TAG, "Usage: sleepafter <milliseconds>");
                ESP_LOGI(TAG, "Example: sleepafter 30000 (sleep after 30 seconds)");
            }
        }
        else if (command == "cancelsleep")
        {
            sleepController.cancelSleep();
            ESP_LOGI(TAG, "Scheduled sleep cancelled");
        }
        else if (command == "sleepstatus")
        {
            ESP_LOGI(TAG, "--- Sleep Controller Status ---");
            ESP_LOGI(TAG, "Inactivity timeout: %lu ms", sleepController.getInactivityTimeout());
            ESP_LOGI(TAG, "Was woken from sleep: %s", sleepController.wasWokenFromSleep() ? "Yes" : "No");
            if (sleepController.wasWokenFromSleep())
            {
                sleepController.printWakeupReason();
            }

            // Check sleep conditions
            FileManager &fm = FileManager::getInstance();
            AudioController &ac = AudioController::getInstance();
            ESP_LOGI(TAG, "Pending downloads: %d", fm.getPendingDownloadsCount());
            ESP_LOGI(TAG, "Audio state: %d (0=STOPPED, 1=PLAYING, 2=PAUSED)", ac.getState());
            ESP_LOGI(TAG, "Can enter sleep: %s", (fm.getPendingDownloadsCount() == 0 && ac.getState() != 1) ? "Yes" : "No");
            ESP_LOGI(TAG, "-------------------------------");
        }
        // File Manager commands that were missing
        else if (command == "dlstats")
        {
            ESP_LOGI(TAG, "%s", fileManager.getDownloadStatsString().c_str());
        }
        else if (command == "dlqueue")
        {
            fileManager.printDownloadQueue();
        }
        else if (command == "required")
        {
            fileManager.printRequiredFiles();
        }
        else if (command == "checkfiles")
        {
            ESP_LOGI(TAG, "Checking required files and scheduling missing ones for download...");
            fileManager.checkRequiredFiles();
            ESP_LOGI(TAG, "Check complete. Use 'dlqueue' to see download queue.");
        }
        else if (command == "cleanup")
        {
            ESP_LOGI(TAG, "Cleaning up temporary files...");
            fileManager.cleanupTempFiles();
            ESP_LOGI(TAG, "Cleanup complete.");
        }
        else
        {
            ESP_LOGI(TAG, "Unknown command. Type 'help' for a list of commands.");
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
    if (WiFi.isConnected())
    {
        reverb.update();
    }

    // Update LED controller (handles pulse and pulseRapid animations)
    ledController.update();

    // Update Button controller (handles button state changes and callbacks)
    buttonController.update();

    // Update NFC controller (handles reed switch monitoring and NFC reading)
    nfcController.update();

    // Update Sleep controller (handles inactivity timeout and sleep scheduling)
    sleepController.update();

    delay(1);
}