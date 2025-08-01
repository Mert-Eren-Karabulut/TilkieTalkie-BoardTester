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
    }

    void begin(
        const char *host,
        uint16_t port,
        const char *appKey,
        const char *authToken,
        const char *deviceId)
    {
        if (!WiFi.isConnected())
        {
            return;
        }

        // WebSocket is CRITICAL - only check for absolute minimum memory
        size_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 25000) {
            Serial.printf("ReverbClient: CRITICAL memory shortage - Free: %d\n", freeHeap);
            return;
        }

        _host = host;
        _port = port;
        _appKey = appKey;
        _authToken = authToken;
        _deviceId = deviceId;

        // Pre-allocate all objects at once to minimize fragmentation
        if (!_httpClient) {
            _httpClient = new WiFiClientSecure();
            ((WiFiClientSecure*)_httpClient)->setInsecure();
        }

        if (_ws == nullptr) {
            _ws = new WebSocketsClient();
        }

        // Configure WebSocket with optimized settings
        instance = this;
        _ws->onEvent(webSocketEvent);
        _ws->setReconnectInterval(10000);
        _ws->enableHeartbeat(15000, 2000, 1);
        
        // Pre-build WebSocket path to avoid String concatenation during runtime
        snprintf(urlBuffer, sizeof(urlBuffer), "/app/%s?protocol=7&client=esp32-client&version=1.0", _appKey.c_str());

        // Immediate connection attempt - WebSocket is critical
        Serial.println("ReverbClient: Initiating critical WebSocket connection...");
        _ws->beginSSL(_host.c_str(), _port, urlBuffer);
    }

    void update()
    {
        if (!WiFi.isConnected())
        {
            return;
        }
        
        // ALWAYS handle WebSocket loop - this is critical
        if (_ws) {
            _ws->loop();
        }
        
        // Handle reconnection more aggressively since WebSocket is critical
        if (!_isConnected && _ws && _connectionRetries <= MAX_CONNECTION_RETRIES)
        {
            if (millis() - _lastConnectionAttempt >= RETRY_DELAY)
            {
                Serial.printf("ReverbClient: Attempting critical reconnection (attempt %d/%d)\n", 
                             _connectionRetries + 1, MAX_CONNECTION_RETRIES);
                
                if (_ws) {
                    _ws->disconnect();
                    delay(200);
                    _ws->beginSSL(_host.c_str(), _port, urlBuffer);
                    _lastConnectionAttempt = millis();
                    _connectionRetries++;
                }
            }
        }

        // Adaptive device report frequency based on memory pressure
        static unsigned long lastReportTime = 0;
        static unsigned long reportInterval = 1000;
        
        if (_isConnected && millis() - lastReportTime >= reportInterval)
        {
            size_t freeHeap = ESP.getFreeHeap();
            lastReportTime = millis();
            sendDeviceReport();
        }
    }

    bool isConnected()
    {
        return WiFi.isConnected() && _isConnected;
    }

    void disconnect()
    {
        if (_ws)
        {
            _ws->disconnect();
            _isConnected = false;
        }
    }

    void cleanup()
    {
        if (_ws)
        {
            _ws->disconnect();
            delete _ws;
            _ws = nullptr;
            _isConnected = false;
        }
        
        if (_httpClient) {
            delete _httpClient;
            _httpClient = nullptr;
        }
    }

    bool sendMessage(const String &text)
    {
        if (!WiFi.isConnected() || !_httpClient) {
            Serial.println("ReverbClient: Cannot send message - WiFi not connected or no HTTP client");
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
            
            int httpCode = http.POST((uint8_t*)tempBuffer, strlen(tempBuffer));
            http.end();

            return (httpCode == 200);
        }
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
    
    // Connection retry management
    unsigned long _lastConnectionAttempt = 0;
    uint8_t _connectionRetries = 0;
    static const uint8_t MAX_CONNECTION_RETRIES = 5;
    static const unsigned long RETRY_DELAY = 2000;

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
            _isConnected = false;
            _socketId = "";
            _connectionRetries++;
            if (_connectionRetries <= MAX_CONNECTION_RETRIES) {
                _lastConnectionAttempt = millis();
            }
            break;

        case WStype_CONNECTED:
            _isConnected = true;
            _connectionRetries = 0;
            _lastConnectionAttempt = 0;
            break;

        case WStype_TEXT:
        {
            // Parse JSON minimally using string search
            char* payloadStr = (char*)payload;
            payloadStr[length] = '\0';
            
            if (strstr(payloadStr, "pusher:connection_established")) {
                char* socketStart = strstr(payloadStr, "socket_id\":\"") + 12;
                if (socketStart > payloadStr + 11) {
                    char* socketEnd = strchr(socketStart, '"');
                    if (socketEnd) {
                        *socketEnd = '\0';
                        _socketId = String(socketStart);
                        *socketEnd = '"';
                        subscribeToPrivate();
                    }
                }
            }
            else if (strstr(payloadStr, "pusher:ping")) {
                const char* pong = "{\"event\":\"pusher:pong\",\"data\":{}}";
                _ws->sendTXT(pong, strlen(pong));
            }
            else if (strstr(payloadStr, "chat-message")) {
                char* textStart = strstr(payloadStr, "\"text\":\"") + 8;
                if (textStart > payloadStr + 7 && _chatCb) {
                    char* textEnd = strchr(textStart, '"');
                    if (textEnd) {
                        *textEnd = '\0';
                        _chatCb(String(textStart));
                        *textEnd = '"';
                    }
                }
            }
            break;
        }
        case WStype_ERROR:
            Serial.printf("ReverbClient: WebSocket error: %.*s\n", length, payload);
            break;
        default:
            break;
        }
    }
    
    void sendDeviceReport()
    {
        if (!isConnected()) return;

        // Skip only if memory is critically low
        size_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 20000) {
            Serial.printf("ReverbClient: Skipping device report due to critical memory: %d bytes\n", freeHeap);
            return;
        }

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
