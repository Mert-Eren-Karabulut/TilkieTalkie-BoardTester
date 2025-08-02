#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

// Include headers for all necessary device components to fetch status data
#include "BatteryManagement.h"
#include "FileManager.h"
#include "AudioController.h"
#include "NfcController.h"
#include "ConfigManager.h"

class ReverbClient
{
public:
    static ReverbClient &getInstance()
    {
        static ReverbClient inst;
        return inst;
    }

    void onChatMessage(std::function<void(const String &)> cb)
    {
        _chatCb = cb;
        Serial.println("ReverbClient: Chat message callback registered");
    }

    void begin(
        const char *host,
        uint16_t port,
        const char *appKey,
        const char *authToken,
        const char *deviceId)
    {
        _host = host;
        _port = port;
        _appKey = appKey;
        _authToken = authToken;
        _deviceId = deviceId;
        _initialized = true;

        // Pre-allocate all objects at once to minimize fragmentation
        if (!_httpClient) {
            _httpClient = new WiFiClientSecure();
            ((WiFiClientSecure*)_httpClient)->setInsecure();
        }

        if (_ws == nullptr) {
            _ws = new WebSocketsClient();
        }

        // Configure WebSocket to work WITH the library's built-in reconnection
        instance = this;
        _ws->onEvent(webSocketEvent);
        _ws->setReconnectInterval(2000); // Let library handle reconnection every 2 seconds
        _ws->enableHeartbeat(15000, 3000, 2); // 15s ping, 3s pong timeout, 2 retries
        
        // Pre-build WebSocket path to avoid String concatenation during runtime
        snprintf(urlBuffer, sizeof(urlBuffer), "/app/%s?protocol=7&client=esp32-client&version=1.0", _appKey.c_str());

        Serial.println("ReverbClient: Initialized, will connect when WiFi is available");
        
        // Start connection if WiFi is already available
        if (WiFi.isConnected()) {
            _ws->beginSSL(_host.c_str(), _port, urlBuffer);
        }
    }

    void update()
    {
        // Handle WiFi state changes
        if (!WiFi.isConnected()) {
            if (_isConnected || _wsStarted) {
                Serial.println("ReverbClient: WiFi disconnected, stopping WebSocket");
                if (_ws) {
                    _ws->disconnect();
                }
                _isConnected = false;
                _wsStarted = false;
            }
            return;
        }
        
        // WiFi is connected - start WebSocket if not already started
        if (!_wsStarted && _initialized) {
            Serial.println("ReverbClient: Starting WebSocket connection");
            _ws->beginSSL(_host.c_str(), _port, urlBuffer);
            _wsStarted = true;
        }
        
        // Let the library handle everything (reconnection, heartbeat, etc.)
        if (_ws) {
            _ws->loop();
        }

        // Send device reports when connected
        static unsigned long lastReportTime = 0;
        if (_isConnected && millis() - lastReportTime >= 5000) {
            lastReportTime = millis();
            sendDeviceReport();
        }
    }

    bool isConnected()
    {
        return WiFi.isConnected() && _ws && _ws->isConnected();
    }
    
    // Get simple connection status
    String getConnectionStatus()
    {
        if (!WiFi.isConnected()) {
            return "WiFi Disconnected";
        }
        
        if (!_wsStarted) {
            return "WebSocket Not Started";
        }
        
        if (_ws && _ws->isConnected()) {
            return "Fully Connected";
        }
        
        return "WebSocket Connecting...";
    }

    void disconnect()
    {
        Serial.println("ReverbClient: Manual disconnect requested");
        if (_ws)
        {
            _ws->disconnect();
        }
        _isConnected = false;
        _wsStarted = false;
    }
    
    // Force immediate reconnection attempt
    void forceReconnect()
    {
        Serial.println("ReverbClient: Force reconnection requested");
        disconnect();
        
        if (WiFi.isConnected() && _initialized) {
            Serial.println("ReverbClient: Restarting WebSocket connection");
            _ws->beginSSL(_host.c_str(), _port, urlBuffer);
            _wsStarted = true;
        }
    }

    void cleanup()
    {
        Serial.println("ReverbClient: Cleaning up resources");
        
        _initialized = false;
        _isConnected = false;
        _wsStarted = false;
        
        if (_ws)
        {
            _ws->disconnect();
            delete _ws;
            _ws = nullptr;
        }
        
        if (_httpClient) {
            delete _httpClient;
            _httpClient = nullptr;
        }
    }

    bool sendMessage(const String &text)
    {
        if (!isConnected() || !_httpClient) {
            Serial.printf("ReverbClient: Cannot send message - Connection status: %s\n", 
                         getConnectionStatus().c_str());
            return false;
        }

        HTTPClient http;
        snprintf(urlBuffer, sizeof(urlBuffer), "https://%s/api/chat/device/%s", _host.c_str(), _deviceId.c_str());
        
        if (http.begin(*_httpClient, urlBuffer))
        {
            snprintf(headerBuffer, sizeof(headerBuffer), "Bearer %s", _authToken.c_str());
            http.addHeader("Authorization", headerBuffer);
            http.addHeader("Content-Type", "application/json");

            // Build minimal JSON payload manually
            String escapedText = text;
            escapedText.replace("\"", "\\\"");
            snprintf(tempBuffer, sizeof(tempBuffer), "{\"text\":\"%s\"}", escapedText.c_str());
            
            Serial.printf("ReverbClient: Sending message: %s\n", text.c_str());
            int httpCode = http.POST((uint8_t*)tempBuffer, strlen(tempBuffer));
            http.end();

            if (httpCode == 200) {
                Serial.println("ReverbClient: Message sent successfully");
                return true;
            } else {
                Serial.printf("ReverbClient: Failed to send message, HTTP code: %d\n", httpCode);
                return false;
            }
        }
        Serial.println("ReverbClient: Failed to initialize HTTP request");
        return false;
    }

private:
    ReverbClient() = default;

    // Pre-allocated static buffers to reduce heap fragmentation
    static char jsonBuffer[512];
    static char urlBuffer[128];
    static char headerBuffer[256];
    static char channelBuffer[64];
    static char tempBuffer[256];

    WebSocketsClient *_ws = nullptr;
    WiFiClientSecure *_httpClient = nullptr;
    String _host, _appKey, _authToken, _deviceId, _socketId;
    uint16_t _port;
    std::function<void(const String &)> _chatCb;
    bool _isConnected = false;
    bool _initialized = false;
    bool _wsStarted = false;

    static ReverbClient *instance;

    static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
    {
        if (instance)
            instance->handleEvent(type, payload, length);
    }

    void handleEvent(WStype_t type, uint8_t *payload, size_t length)
    {
        switch (type)
        {
        case WStype_DISCONNECTED:
            Serial.println("ReverbClient: WebSocket disconnected");
            _isConnected = false;
            _socketId = "";
            break;

        case WStype_CONNECTED:
            Serial.printf("ReverbClient: WebSocket connected to: %s\n", payload);
            _isConnected = true;
            break;

        case WStype_ERROR:
            Serial.printf("ReverbClient: WebSocket error: %.*s\n", length, payload);
            _isConnected = false;
            _socketId = "";
            break;

        case WStype_TEXT:
        {
            // Parse JSON minimally using string search
            char* payloadStr = (char*)payload;
            payloadStr[length] = '\0';
            
            if (strstr(payloadStr, "pusher:connection_established")) {
                // Look for socket_id in the data field - handle both escaped and unescaped JSON
                char* socketStart = strstr(payloadStr, "socket_id");
                if (socketStart) {
                    // Find the actual ID value after the colon and quotes
                    socketStart = strchr(socketStart, ':');
                    if (socketStart) {
                        socketStart++; // Skip the colon
                        // Skip whitespace and quotes
                        while (*socketStart && (*socketStart == ' ' || *socketStart == '"' || *socketStart == '\\')) {
                            socketStart++;
                        }
                        
                        // Find the end of the socket ID
                        char* socketEnd = socketStart;
                        while (*socketEnd && *socketEnd != '"' && *socketEnd != '\\' && *socketEnd != ',' && *socketEnd != '}') {
                            socketEnd++;
                        }
                        
                        if (socketEnd > socketStart) {
                            char tempChar = *socketEnd;
                            *socketEnd = '\0';
                            _socketId = String(socketStart);
                            *socketEnd = tempChar; // Restore the character
                            
                            Serial.printf("ReverbClient: Connected with socket ID: %s\n", _socketId.c_str());
                            if (subscribeToPrivate()) {
                                Serial.println("ReverbClient: Successfully subscribed to private channel");
                            } else {
                                Serial.println("ReverbClient: Failed to subscribe to private channel");
                            }
                        }
                    }
                }
            }
            else if (strstr(payloadStr, "pusher:ping")) {
                const char* pong = "{\"event\":\"pusher:pong\",\"data\":{}}";
                _ws->sendTXT(pong, strlen(pong));
            }
            else if (strstr(payloadStr, "device.status.updated")) {
                // Ignore device status updates (these are our own reports bounced back)
            }
            else if (strstr(payloadStr, "device.command.sent")) {
                Serial.printf("ReverbClient: Command sent event detected: %.*s\n", length, payload);
                // We can handle commands here if needed, but for now just log it
            }
            else if (strstr(payloadStr, "chat-message")) {
                Serial.println("ReverbClient: Chat message event detected!");
                
                if (!_chatCb) {
                    Serial.println("ReverbClient: ERROR - No callback registered for chat messages!");
                    break;
                }
                
                // The data field contains escaped JSON, so we need to find the text field within it
                // Look for "text":"<message>" pattern in the escaped JSON
                char* textStart = strstr(payloadStr, "\\\"text\\\":\\\"");
                if (textStart) {
                    textStart += 12; // Skip past "\"text\":\""
                    char* textEnd = strstr(textStart, "\\\"");
                    if (textEnd) {
                        *textEnd = '\0';
                        String messageText = String(textStart);
                        *textEnd = '\\'; // Restore the backslash
                        
                        Serial.printf("ReverbClient: Received chat message: %s\n", messageText.c_str());
                        _chatCb(messageText);
                    } else {
                        Serial.println("ReverbClient: Could not find end quote for text field");
                    }
                } else {
                    // Fallback: try unescaped version in case format changes
                    textStart = strstr(payloadStr, "\"text\":\"");
                    if (textStart) {
                        textStart += 8; // Skip past "text":"
                        char* textEnd = strchr(textStart, '"');
                        if (textEnd) {
                            *textEnd = '\0';
                            String messageText = String(textStart);
                            *textEnd = '"'; // Restore the quote
                            
                            Serial.printf("ReverbClient: Received chat message (fallback): %s\n", messageText.c_str());
                            _chatCb(messageText);
                        }
                    } else {
                        Serial.printf("ReverbClient: Could not parse chat message. Full payload: %.*s\n", length, payload);
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
    
    void sendDeviceReport()
    {
        if (!isConnected()) return;

        // Get and cache all values to minimize object access
        BatteryManager &battery = BatteryManager::getInstance();
        FileManager &fileManager = FileManager::getInstance();
        AudioController &audio = AudioController::getInstance();
        NfcController &nfc = NfcController::getInstance();
        ConfigManager &config = ConfigManager::getInstance();

        bool isCharging = battery.getChargingStatus();
        float batteryPercent = battery.getBatteryPercentage();
        float batteryVoltage = battery.getBatteryVoltage();
        bool isFileSyncing = fileManager.getPendingDownloadsCount() > 0;
        uint32_t sdFreeSpace = fileManager.getSDCardFreeSpace();
        String wifiSSID = config.getWiFiSSID();
        int wifiRSSI = WiFi.RSSI();
        int audioState = audio.getState(); // Use int instead of enum
        bool isReedActive = nfc.isReedSwitchActive();
        bool isCardPresent = nfc.isCardPresent();

        // Pre-build channel name
        snprintf(channelBuffer, sizeof(channelBuffer), "device.%s", _deviceId.c_str());

        // Get audio status
        const char* audioStatus = "stopped";
        String currentTrack = "";
        if (audioState == 1) { // PLAYING state
            audioStatus = "playing";
            currentTrack = audio.getCurrentTrack();
        } else if (audioState == 2) { // PAUSED state
            audioStatus = "paused";
            currentTrack = audio.getCurrentTrack();
        }

        // Sanitize strings
        wifiSSID.replace("\"", "\\\"");
        currentTrack.replace("\"", "\\\"");

        // Get NFC card ID
        String nfcCardId = "";
        if (isReedActive && isCardPresent) {
            nfcCardId = nfc.currentNFCData().uidString;
        }

        // Build optimized JSON
        int written = snprintf(jsonBuffer, sizeof(jsonBuffer),
            "{"
            "\"event\":\"device-report\","
            "\"channel\":\"%s\","
            "\"data\":{"
                "\"device_report\":{"
                    "\"device_id\":\"%s\","
                    "\"battery\":{"
                        "\"status\":\"%s\","
                        "\"percent\":%.1f,"
                        "\"voltage\":%.2f"
                    "},"
                    "\"files\":{"
                        "\"status\":\"%s\","
                        "\"sd_remaining\":%u,"
                        "\"wifi_ssid\":\"%s\","
                        "\"wifi_rssi\":%d"
                    "},"
                    "\"audio\":{"
                        "\"current_track_status\":\"%s\""
                        "%s%s%s"
                    "},"
                    "\"nfc\":{"
                        "\"switch_status\":\"%s\""
                        "%s%s%s"
                    "}"
                "}"
            "}"
            "}",
            channelBuffer,
            _deviceId.c_str(),
            isCharging ? "charging" : "discharging",
            batteryPercent,
            batteryVoltage,
            isFileSyncing ? "syncing" : "in_sync",
            sdFreeSpace,
            wifiSSID.c_str(),
            wifiRSSI,
            audioStatus,
            currentTrack.length() > 0 ? ",\"current_track_id\":\"" : "",
            currentTrack.c_str(),
            currentTrack.length() > 0 ? "\"" : "",
            isReedActive ? "present" : "empty",
            nfcCardId.length() > 0 ? ",\"docked_card_id\":\"" : "",
            nfcCardId.c_str(),
            nfcCardId.length() > 0 ? "\"" : ""
        );

        if (written > 0 && written < sizeof(jsonBuffer) - 1) {
            _ws->sendTXT(jsonBuffer, written);
        }
    }

    bool subscribeToPrivate()
    {
        if (_socketId.length() == 0 || !_httpClient) {
            return false;
        }

        HTTPClient http;
        snprintf(urlBuffer, sizeof(urlBuffer), "https://%s/broadcasting/auth", _host.c_str());

        if (http.begin(*_httpClient, urlBuffer))
        {
            http.addHeader("Content-Type", "application/json");
            snprintf(headerBuffer, sizeof(headerBuffer), "Bearer %s", _authToken.c_str());
            http.addHeader("Authorization", headerBuffer);
            http.addHeader("X-Client-Source", "esp32");
            
            snprintf(channelBuffer, sizeof(channelBuffer), "device.%s", _deviceId.c_str());
            snprintf(tempBuffer, sizeof(tempBuffer), 
                "{\"socket_id\":\"%s\",\"channel_name\":\"%s\"}", 
                _socketId.c_str(), channelBuffer);

            int httpCode = http.POST((uint8_t*)tempBuffer, strlen(tempBuffer));
            
            if (httpCode != 200)
            {
                http.end();
                return false;
            }

            String authResponse = http.getString();
            http.end();

            int authStart = authResponse.indexOf("\"auth\":\"") + 8;
            int authEnd = authResponse.indexOf("\"", authStart);
            if (authStart < 8 || authEnd < 0) {
                return false;
            }
            String authValue = authResponse.substring(authStart, authEnd);

            snprintf(jsonBuffer, sizeof(jsonBuffer),
                "{\"event\":\"pusher:subscribe\",\"data\":{\"auth\":\"%s\",\"channel\":\"%s\"}}",
                authValue.c_str(), channelBuffer);

            _ws->sendTXT(jsonBuffer, strlen(jsonBuffer));
            return true;
        }
        return false;
    }
};

// Define static buffers
char ReverbClient::jsonBuffer[512];
char ReverbClient::urlBuffer[128];
char ReverbClient::headerBuffer[256];
char ReverbClient::channelBuffer[64];
char ReverbClient::tempBuffer[256];

ReverbClient *ReverbClient::instance = nullptr;
