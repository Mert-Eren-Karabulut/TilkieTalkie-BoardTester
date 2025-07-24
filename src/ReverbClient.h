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
            Serial.println(F("⚠️ WiFi not connected! Call begin() only after your WiFi class connects."));
            return;
        }

        // Check available memory before starting
        size_t freeHeap = ESP.getFreeHeap();
        Serial.printf("Free heap before ReverbClient begin: %d bytes\n", freeHeap);
        if (freeHeap < 50000)
        { // Require at least 50KB of free memory
            Serial.println(F("⚠️ Insufficient memory for WebSocket connection! Try disabling BLE first."));
            return;
        }

        _host = host;
        _port = port;
        _appKey = appKey;
        _authToken = authToken;
        _deviceId = deviceId;

        // Initialize the WebSocketsClient pointer only if we have enough memory
        if (_ws == nullptr)
        {
            _ws = new WebSocketsClient();
        }

        instance = this;
        _ws->onEvent(webSocketEvent);
        _ws->setReconnectInterval(5000);
        // Add these settings for better connection stability
        _ws->enableHeartbeat(15000, 3000, 2); // ping interval, pong timeout, disconnect after failures

        String path = String("/app/") + _appKey + "?protocol=7&client=esp32-client&version=1.0";

        // --- FIX: Use the standard beginSSL method ---
        // The library will manage its own secure client internally.
        _ws->beginSSL(_host.c_str(), _port, path.c_str());
        Serial.println(F("🔌 ReverbClient: connecting to WebSocket server..."));
        Serial.printf("Free heap after ReverbClient begin: %d bytes\n", ESP.getFreeHeap());
    }

    void update()
    {
        if (!WiFi.isConnected() || !_ws)
        {
            // Don't update WebSocket if WiFi is not connected or if client is not initialized
            return;
        }
        
        _ws->loop();

        // --- NEW: Periodically send device status report ---
        static unsigned long lastReportTime = 0;
        if (_isConnected && millis() - lastReportTime >= 5000)
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
            Serial.println(F("ReverbClient disconnected"));
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
            Serial.println(F("ReverbClient cleaned up - memory freed"));
            Serial.printf("Free heap after cleanup: %d bytes\n", ESP.getFreeHeap());
        }
    }

    bool sendMessage(const String &text)
    {
        if (!WiFi.isConnected())
        {
            Serial.println(F("⚠️ Cannot send message, WiFi is disconnected."));
            return false;
        }

        // Use an insecure client for the HTTP request to save memory
        WiFiClientSecure insecureClient;
        insecureClient.setInsecure();

        HTTPClient http;
        String url = "https://" + _host + "/api/chat/device/" + _deviceId;
        if (http.begin(insecureClient, url))
        {
            http.addHeader("Authorization", "Bearer " + _authToken);
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Accept", "application/json");

            StaticJsonDocument<128> doc;
            doc["text"] = text;
            String payload;
            serializeJson(doc, payload);

            Serial.printf("Sending message to API: %s\n", text.c_str());
            int httpCode = http.POST(payload);
            http.end();

            if (httpCode == 200)
            {
                Serial.println(F("✅ Message sent successfully."));
                return true;
            }
            else
            {
                Serial.printf("⚠️ Failed to send message, HTTP Error: %d\n", httpCode);
                return false;
            }
        }
        else
        {
            Serial.println(F("⚠️ HTTP client connection failed!"));
            return false;
        }
    }

private:
    ReverbClient() = default;

    WebSocketsClient *_ws = nullptr;
    String _host, _appKey, _authToken, _deviceId, _socketId;
    uint16_t _port;
    std::function<void(const String &)> _chatCb;
    bool _isConnected = false;

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
            Serial.println(F("⚠️ WebSocket Disconnected!"));
            _isConnected = false;
            _socketId = ""; // Clear socket ID so we can re-establish connection
            break;

        case WStype_CONNECTED:
            Serial.println(F("🔗 WebSocket Connected to server. Waiting for handshake..."));
            _isConnected = true;
            // Note: Don't call subscribeToPrivate() here - wait for pusher:connection_established
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

                Serial.printf("🤝 Handshake complete. Socket ID: %s. Subscribing to private channel...\n", _socketId.c_str());
                subscribeToPrivate();
            }
            else if (strcmp(eventName, "pusher:ping") == 0)
            {
                // Respond to ping with pong to keep connection alive
                StaticJsonDocument<64> pongDoc;
                pongDoc["event"] = "pusher:pong";
                pongDoc["data"] = JsonObject();
                String pongMessage;
                serializeJson(pongDoc, pongMessage);
                _ws->sendTXT(pongMessage);
                Serial.println(F("🏓 Responded to ping with pong"));
            }
            else if (strcmp(eventName, "chat-message") == 0 && _chatCb)
            {
                const char *chatDataStr = doc["data"];
                StaticJsonDocument<256> chatDoc;
                deserializeJson(chatDoc, chatDataStr);

                const char *text = chatDoc["text"];
                const char *senderName = chatDoc["user"]["name"];

                Serial.printf("📨 Message from '%s': %s\n", senderName, text);
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
     * Note: To implement `current_track_seconds`, the AudioController class
     * needs a method to return the elapsed time of the current track.
     */
    void sendDeviceReport()
    {
        if (!isConnected()) return;

        // This JSON document holds the entire payload for the client event
        JsonDocument reportFrame;

        // Set the event name and channel for the Pusher client event
        reportFrame["event"] = "device-report";
        reportFrame["channel"] = "device." + _deviceId;

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

        // Serialize and send the final JSON payload
        String reportPayload;
        serializeJson(reportFrame, reportPayload);
        _ws->sendTXT(reportPayload);
        Serial.println(F("✅ Device report sent successfully."));
    }

    bool subscribeToPrivate()
    {
        if (_socketId.length() == 0)
        {
            Serial.println(F("⚠️ Cannot subscribe, socket_id is missing."));
            return false;
        }

        // Use an insecure client for the HTTP request to save memory
        WiFiClientSecure insecureClient;
        insecureClient.setInsecure();

        HTTPClient http;
        String url = "https://" + _host + "/broadcasting/auth";

        if (http.begin(insecureClient, url))
        {
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Authorization", "Bearer " + _authToken);
            http.addHeader("Accept", "application/json");
            http.addHeader("X-Client-Source", "esp32");
            StaticJsonDocument<128> authPayloadDoc;
            authPayloadDoc["socket_id"] = _socketId;
            authPayloadDoc["channel_name"] = "device." + _deviceId;

            String authPayload;
            serializeJson(authPayloadDoc, authPayload);

            int httpCode = http.POST(authPayload);
            if (httpCode != 200)
            {
                Serial.printf("⚠️ Private channel auth failed! HTTP Code: %d\n", httpCode);
                if (httpCode > 0)
                {
                    String response = http.getString();
                    Serial.println("Response: " + response);
                }
                http.end();
                return false;
            }

            StaticJsonDocument<512> authRespDoc;
            DeserializationError error = deserializeJson(authRespDoc, http.getString());
            http.end();
            if (error)
            {
                Serial.println(F("⚠️ Failed to parse auth response."));
                return false;
            }

            StaticJsonDocument<512> subMsgDoc;
            subMsgDoc["event"] = "pusher:subscribe";
            JsonObject data = subMsgDoc.createNestedObject("data");
            data["auth"] = authRespDoc["auth"].as<String>();
            data["channel"] = "device." + _deviceId;
            data["channel_data"] = authRespDoc["channel_data"];

            String subFrame;
            serializeJson(subMsgDoc, subFrame);
            _ws->sendTXT(subFrame);

            Serial.println("✅ Successfully subscribed to device." + _deviceId);
            return true;
        }
        else
        {
            Serial.println(F("⚠️ Auth HTTP client connection failed!"));
            return false;
        }
    }
};

ReverbClient *ReverbClient::instance = nullptr;