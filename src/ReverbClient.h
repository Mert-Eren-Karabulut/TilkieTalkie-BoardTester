#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h> // Required for the insecure client
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h> // links2004/WebSockets v2.6.1

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

        // Check available memory and fragmentation before starting
        size_t freeHeap = ESP.getFreeHeap();
        size_t largestBlock = ESP.getMaxAllocHeap();
        
        // Require sufficient memory for SSL WebSocket connections
        if (freeHeap < 80000 || largestBlock < 40000) { 
            return;
        }
        
        // If fragmentation is too high, attempt cleanup
        float fragmentation = (1.0f - (float)largestBlock / (float)freeHeap) * 100.0f;
        if (fragmentation > 70.0f) {
            String temp = String("cleanup");
            temp.reserve(1000);
            temp = "";
            delay(100);
        }

        _host = host;
        _port = port;
        _appKey = appKey;
        _authToken = authToken;
        _deviceId = deviceId;

        // Initialize shared SSL client for HTTP requests to reduce memory allocations  
        if (!_sslClient) {
            _sslClient = new WiFiClientSecure();
            _sslClient->setInsecure();  // Configure once for all HTTP requests
        }

        // Initialize the WebSocketsClient pointer only if we have enough memory
        if (_ws == nullptr)
        {
            _ws = new WebSocketsClient();
        }

        instance = this;
        _ws->onEvent(webSocketEvent);
        _ws->setReconnectInterval(30000); // Increased from 5000 to reduce reconnection frequency
        // Reduce heartbeat frequency to minimize memory allocations
        _ws->enableHeartbeat(30000, 5000, 2); // ping every 30s, 5s timeout, 2 failures max

        // Pre-build WebSocket path to avoid String concatenation
        snprintf(urlBuffer, sizeof(urlBuffer), "/app/%s?protocol=7&client=esp32-client&version=1.0", _appKey.c_str());

        // Use the standard beginSSL method (WebSocket library manages its own SSL client)
        _ws->beginSSL(_host.c_str(), _port, urlBuffer);
    }

    void update()
    {
        if (!WiFi.isConnected())
        {
            // Don't update WebSocket if WiFi is not connected
            return;
        }
        
                // Handle automatic reconnection with fixed 3-second delay
        if (!_isConnected && _ws && _connectionRetries > 0 && _connectionRetries <= MAX_CONNECTION_RETRIES)
        {
            if (millis() - _lastConnectionAttempt >= RETRY_DELAY)
            {
                // Check memory before reconnection attempt
                size_t freeHeap = ESP.getFreeHeap();
                size_t largestBlock = ESP.getMaxAllocHeap();
                if (freeHeap < 80000 || largestBlock < 40000) {
                    _lastConnectionAttempt = millis(); // Delay next attempt
                    return;
                }
                
                // Attempt reconnection using existing connection settings
                if (_ws) {
                    _ws->disconnect(); // Ensure clean state
                    delay(1000); // Brief delay for cleanup
                    _ws->beginSSL(_host.c_str(), _port, urlBuffer);
                    _lastConnectionAttempt = millis();
                }
            }
        }
        
        if (_ws) {
            _ws->loop();
        }

        // --- Reduce device report frequency to minimize memory allocations ---
        static unsigned long lastReportTime = 0;
        if (_isConnected && millis() - lastReportTime >= 10000) // Increased from 5000 to 10000ms
        {
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
        
        // Clean up shared SSL client to free memory
        if (_sslClient) {
            delete _sslClient;
            _sslClient = nullptr;
        }
        
        Serial.println(F("ReverbClient cleaned up - memory freed"));
    }

    bool sendMessage(const String &text)
    {
        if (!WiFi.isConnected() || !_sslClient) {
            return false;
        }

        HTTPClient http;
        snprintf(urlBuffer, sizeof(urlBuffer), "https://%s/api/chat/device/%s", _host.c_str(), _deviceId.c_str());
        
        if (http.begin(*_sslClient, urlBuffer))
        {
            snprintf(headerBuffer, sizeof(headerBuffer), "Bearer %s", _authToken.c_str());
            http.addHeader("Authorization", headerBuffer);
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Accept", "application/json");

            StaticJsonDocument<128> doc;
            doc["text"] = text;
            size_t payloadLength = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
            
            if (payloadLength == 0) {
                http.end();
                return false;
            }

            Serial.printf("Sending message to API: %s\n", text.c_str());
            int httpCode = http.POST((uint8_t*)jsonBuffer, payloadLength);
            http.end();

            return (httpCode == 200);
        }
        return false;
    }

private:
    ReverbClient() = default;

    // Pre-allocated static buffers to reduce heap fragmentation - conservative sizes
    static char jsonBuffer[768];            // For device reports and auth payloads
    static char urlBuffer[200];             // For URL construction
    static char headerBuffer[400];          // For header values
    static char channelBuffer[100];         // For channel names

    WebSocketsClient *_ws = nullptr;
    WiFiClientSecure *_sslClient = nullptr;  // Shared SSL client to reduce allocations
    String _host, _appKey, _authToken, _deviceId, _socketId;
    uint16_t _port;
    std::function<void(const String &)> _chatCb;
    bool _isConnected = false;
    
    // Connection retry management
    unsigned long _lastConnectionAttempt = 0;
    uint8_t _connectionRetries = 0;
    static const uint8_t MAX_CONNECTION_RETRIES = 10;  // More retries since WebSocket is critical
    static const unsigned long RETRY_DELAY = 3000; // Fixed 3 second delay between retries

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
            
            // Implement fixed delay reconnections
            _connectionRetries++;
            if (_connectionRetries <= MAX_CONNECTION_RETRIES) {
                _lastConnectionAttempt = millis();
            }
            break;

        case WStype_CONNECTED:
            _isConnected = true;
            _connectionRetries = 0; // Reset retry counter on successful connection
            _lastConnectionAttempt = 0;
            break;

        case WStype_TEXT:
        {
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, payload, length);
            if (error)
            {
                Serial.print(F("⚠️ JSON parsing failed: "));
                Serial.println(error.c_str());
                return;
            }

            const char *eventName = doc["event"];
            if (!eventName)
                return;

            if (strcmp(eventName, "pusher:connection_established") == 0)
            {
                const char *socketData = doc["data"];
                StaticJsonDocument<256> socketDoc;
                deserializeJson(socketDoc, socketData);
                _socketId = socketDoc["socket_id"].as<String>();
                subscribeToPrivate();
            }
            else if (strcmp(eventName, "pusher:ping") == 0)
            {
                // Respond to ping with pong to keep connection alive
                StaticJsonDocument<64> pongDoc;
                pongDoc["event"] = "pusher:pong";
                pongDoc["data"] = JsonObject();
                size_t pongLength = serializeJson(pongDoc, jsonBuffer, sizeof(jsonBuffer));
                if (pongLength > 0) {
                    _ws->sendTXT(jsonBuffer, pongLength);
                }
            }
            else if (strcmp(eventName, "chat-message") == 0 && _chatCb)
            {
                const char *chatDataStr = doc["data"];
                StaticJsonDocument<256> chatDoc;
                deserializeJson(chatDoc, chatDataStr);

                const char *text = chatDoc["text"];
                _chatCb(String(text));
            }
            break;
        }
        default:
            break;
        }
    }
    
    /**
     * @brief Constructs and sends a JSON object with the device's current state.
     * This is a "client event" sent over the WebSocket to the private device channel.
     * Optimized to minimize memory allocations during runtime.  
     */
    void sendDeviceReport()
    {
        if (!isConnected()) return;

        // Check available memory before creating device report
        size_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 30000) {
            return;
        }

        // Use static JSON document to avoid heap allocation
        StaticJsonDocument<768> reportFrame;

        // Pre-build channel name to avoid string concatenation
        snprintf(channelBuffer, sizeof(channelBuffer), "device.%s", _deviceId.c_str());

        // Set the event name and channel for the Pusher client event
        reportFrame["event"] = "device-report";
        reportFrame["channel"] = channelBuffer;

        // Create the nested "data" and "device_report" objects
        JsonObject dataObj = reportFrame.createNestedObject("data");
        JsonObject reportObj = dataObj.createNestedObject("device_report");

        // --- Populate device_report ---
        reportObj["device_id"] = _deviceId;

        // --- Battery Info ---
        JsonObject batteryObj = reportObj.createNestedObject("battery");
        BatteryManager &battery = BatteryManager::getInstance();
        batteryObj["status"] = battery.getChargingStatus() ? "charging" : "discharging";
        batteryObj["percent"] = battery.getBatteryPercentage();
        batteryObj["voltage"] = battery.getBatteryVoltage();

        // --- Files Info ---
        JsonObject filesObj = reportObj.createNestedObject("files");
        FileManager &fileManager = FileManager::getInstance();
        filesObj["status"] = fileManager.getPendingDownloadsCount() > 0 ? "syncing" : "in_sync";
        filesObj["sd_remaining"] = fileManager.getSDCardFreeSpace();
        filesObj["wifi_ssid"] = ConfigManager::getInstance().getWiFiSSID();
        filesObj["wifi_rssi"] = WiFi.RSSI();

        // --- Audio Info ---
        JsonObject audioObj = reportObj.createNestedObject("audio");
        AudioController &audio = AudioController::getInstance();
        switch (audio.getState())
        {
        case AudioController::PLAYING:
            audioObj["current_track_status"] = "playing";
            audioObj["current_track_id"] = audio.getCurrentTrack();
            audioObj["current_track_seconds"] = audio.getCurrentTrackSeconds();
            break;
        case AudioController::PAUSED:
            audioObj["current_track_status"] = "paused";
            audioObj["current_track_id"] = audio.getCurrentTrack();
            audioObj["current_track_seconds"] = audio.getCurrentTrackSeconds();
            break;
        case AudioController::STOPPED:
            audioObj["current_track_status"] = "stopped";
            audioObj["current_track_id"] = nullptr;
            audioObj["current_track_seconds"] = nullptr;
            break;
        }

        // --- NFC Info ---
        JsonObject nfcObj = reportObj.createNestedObject("nfc");
        NfcController &nfc = NfcController::getInstance();
        bool reedActive = nfc.isReedSwitchActive();
        nfcObj["switch_status"] = reedActive ? "present" : "empty";
        if (reedActive && nfc.isCardPresent()) {
            nfcObj["docked_card_id"] = nfc.currentNFCData().uidString;
        } else {
            nfcObj["docked_card_id"] = nullptr;
        }

        // Serialize directly to static buffer to avoid String allocation
        size_t jsonLength = serializeJson(reportFrame, jsonBuffer, sizeof(jsonBuffer));
        if (jsonLength > 0 && jsonLength < sizeof(jsonBuffer) - 1) {
            _ws->sendTXT(jsonBuffer, jsonLength);
        }
    }

    bool subscribeToPrivate()
    {
        if (_socketId.length() == 0 || !_sslClient) {
            return false;
        }

        HTTPClient http;
        snprintf(urlBuffer, sizeof(urlBuffer), "https://%s/broadcasting/auth", _host.c_str());

        if (http.begin(*_sslClient, urlBuffer))
        {
            http.addHeader("Content-Type", "application/json");
            snprintf(headerBuffer, sizeof(headerBuffer), "Bearer %s", _authToken.c_str());
            http.addHeader("Authorization", headerBuffer);
            http.addHeader("Accept", "application/json");
            http.addHeader("X-Client-Source", "esp32");
            
            StaticJsonDocument<128> authPayloadDoc;
            authPayloadDoc["socket_id"] = _socketId;
            snprintf(channelBuffer, sizeof(channelBuffer), "device.%s", _deviceId.c_str());
            authPayloadDoc["channel_name"] = channelBuffer;

            size_t authPayloadLength = serializeJson(authPayloadDoc, jsonBuffer, sizeof(jsonBuffer));
            if (authPayloadLength == 0) {
                http.end();
                return false;
            }

            int httpCode = http.POST((uint8_t*)jsonBuffer, authPayloadLength);
            if (httpCode != 200)
            {
                http.end();
                return false;
            }

            String authResponse = http.getString();
            StaticJsonDocument<512> authRespDoc;
            DeserializationError error = deserializeJson(authRespDoc, authResponse);
            http.end();
            if (error)
            {
                return false;
            }

            StaticJsonDocument<512> subMsgDoc;
            subMsgDoc["event"] = "pusher:subscribe";
            JsonObject data = subMsgDoc.createNestedObject("data");
            data["auth"] = authRespDoc["auth"].as<String>();
            data["channel"] = channelBuffer; // Reuse the pre-built channel name
            data["channel_data"] = authRespDoc["channel_data"];

            size_t subFrameLength = serializeJson(subMsgDoc, jsonBuffer, sizeof(jsonBuffer));
            if (subFrameLength > 0) {
                _ws->sendTXT(jsonBuffer, subFrameLength);
                return true;
            }
        }
        return false;
    }
};

// Define static buffers with conservative sizes to minimize memory footprint
char ReverbClient::jsonBuffer[768];      // Reduced from 1024 - sufficient for device reports
char ReverbClient::urlBuffer[200];       // Reduced from 256 - URLs shouldn't be too long  
char ReverbClient::headerBuffer[400];    // Reduced from 512 - headers are typically shorter
char ReverbClient::channelBuffer[100];   // Reduced from 128 - device IDs are shorter

ReverbClient *ReverbClient::instance = nullptr;