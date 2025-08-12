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
        if (!_httpClient)
        {
            _httpClient = new WiFiClientSecure();
            ((WiFiClientSecure *)_httpClient)->setInsecure();
        }

        if (_ws == nullptr)
        {
            _ws = new WebSocketsClient();
        }

        // Configure WebSocket to work WITH the library's built-in reconnection
        instance = this;
        _ws->onEvent(webSocketEvent);
        _ws->setReconnectInterval(2000);      // Let library handle reconnection every 2 seconds
        _ws->enableHeartbeat(15000, 3000, 2); // 15s ping, 3s pong timeout, 2 retries

        // Pre-build WebSocket path to avoid String concatenation during runtime
        snprintf(urlBuffer, sizeof(urlBuffer), "/app/%s?protocol=7&client=esp32-client&version=1.0", _appKey.c_str());

        Serial.println("ReverbClient: Initialized, will connect when WiFi is available");

        // Start connection if WiFi is already available
        if (WiFi.isConnected())
        {
            _ws->beginSSL(_host.c_str(), _port, urlBuffer);
        }
    }

    void update()
    {
        // Handle WiFi state changes
        if (!WiFi.isConnected())
        {
            if (_isConnected || _wsStarted)
            {
                Serial.println("ReverbClient: WiFi disconnected, stopping WebSocket");
                if (_ws)
                {
                    _ws->disconnect();
                }
                _isConnected = false;
                _wsStarted = false;
            }
            return;
        }

        // WiFi is connected - start WebSocket if not already started
        if (!_wsStarted && _initialized)
        {
            Serial.println("ReverbClient: Starting WebSocket connection");
            _ws->beginSSL(_host.c_str(), _port, urlBuffer);
            _wsStarted = true;
        }

        // Let the library handle everything (reconnection, heartbeat, etc.)
        if (_ws)
        {
            _ws->loop();
        }

        // Send device reports when connected
        static unsigned long lastReportTime = 0;
        if (_isConnected && millis() - lastReportTime >= 5000)
        {
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
        if (!WiFi.isConnected())
        {
            return "WiFi Disconnected";
        }

        if (!_wsStarted)
        {
            return "WebSocket Not Started";
        }

        if (_ws && _ws->isConnected())
        {
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

        if (WiFi.isConnected() && _initialized)
        {
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

        if (_httpClient)
        {
            delete _httpClient;
            _httpClient = nullptr;
        }
    }

    bool sendMessage(const String &text)
    {
        if (!isConnected() || !_httpClient)
        {
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
            int httpCode = http.POST((uint8_t *)tempBuffer, strlen(tempBuffer));
            http.end();

            if (httpCode == 200)
            {
                Serial.println("ReverbClient: Message sent successfully");
                return true;
            }
            else
            {
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
    static char jsonBuffer[512] __attribute__((section(".data")));
    static char urlBuffer[128] __attribute__((section(".data")));
    static char headerBuffer[256] __attribute__((section(".data")));
    static char channelBuffer[64] __attribute__((section(".data")));
    static char tempBuffer[256] __attribute__((section(".data")));

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
            char *payloadStr = (char *)payload;
            payloadStr[length] = '\0';

            if (strstr(payloadStr, "pusher:connection_established"))
            {
                // Look for socket_id in the data field - handle both escaped and unescaped JSON
                char *socketStart = strstr(payloadStr, "socket_id");
                if (socketStart)
                {
                    // Find the actual ID value after the colon and quotes
                    socketStart = strchr(socketStart, ':');
                    if (socketStart)
                    {
                        socketStart++; // Skip the colon
                        // Skip whitespace and quotes
                        while (*socketStart && (*socketStart == ' ' || *socketStart == '"' || *socketStart == '\\'))
                        {
                            socketStart++;
                        }

                        // Find the end of the socket ID
                        char *socketEnd = socketStart;
                        while (*socketEnd && *socketEnd != '"' && *socketEnd != '\\' && *socketEnd != ',' && *socketEnd != '}')
                        {
                            socketEnd++;
                        }

                        if (socketEnd > socketStart)
                        {
                            char tempChar = *socketEnd;
                            *socketEnd = '\0';
                            _socketId = String(socketStart);
                            *socketEnd = tempChar; // Restore the character

                            Serial.printf("ReverbClient: Connected with socket ID: %s\n", _socketId.c_str());
                            if (subscribeToPrivate())
                            {
                                Serial.println("ReverbClient: Successfully subscribed to private channel");
                            }
                            else
                            {
                                Serial.println("ReverbClient: Failed to subscribe to private channel");
                            }
                        }
                    }
                }
            }
            else if (strstr(payloadStr, "pusher:ping"))
            {
                const char *pong = "{\"event\":\"pusher:pong\",\"data\":{}}";
                _ws->sendTXT(pong, strlen(pong));
            }
            else if (strstr(payloadStr, "device.status.updated"))
            {
                // Ignore device status updates (these are our own reports bounced back)
            }
            else if (strstr(payloadStr, "device.command.sent"))
            {
                handleDeviceCommand(payloadStr);
            }
            else if (strstr(payloadStr, "chat-message"))
            {
                Serial.println("ReverbClient: Chat message event detected!");

                if (!_chatCb)
                {
                    Serial.println("ReverbClient: ERROR - No callback registered for chat messages!");
                    break;
                }

                // The data field contains escaped JSON, so we need to find the text field within it
                // Look for "text":"<message>" pattern in the escaped JSON
                char *textStart = strstr(payloadStr, "\\\"text\\\":\\\"");
                if (textStart)
                {
                    textStart += 12; // Skip past "\"text\":\""
                    char *textEnd = strstr(textStart, "\\\"");
                    if (textEnd)
                    {
                        *textEnd = '\0';
                        String messageText = String(textStart);
                        *textEnd = '\\'; // Restore the backslash

                        Serial.printf("ReverbClient: Received chat message: %s\n", messageText.c_str());
                        _chatCb(messageText);
                    }
                    else
                    {
                        Serial.println("ReverbClient: Could not find end quote for text field");
                    }
                }
                else
                {
                    // Fallback: try unescaped version in case format changes
                    textStart = strstr(payloadStr, "\"text\":\"");
                    if (textStart)
                    {
                        textStart += 8; // Skip past "text":"
                        char *textEnd = strchr(textStart, '"');
                        if (textEnd)
                        {
                            *textEnd = '\0';
                            String messageText = String(textStart);
                            *textEnd = '"'; // Restore the quote

                            Serial.printf("ReverbClient: Received chat message (fallback): %s\n", messageText.c_str());
                            _chatCb(messageText);
                        }
                    }
                    else
                    {
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
        if (!isConnected())
            return;

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
        const char *audioStatus = "stopped";
        String currentTrack = "";
        if (audioState == 1)
        { // PLAYING state
            audioStatus = "playing";
            currentTrack = audio.getCurrentTrack();
        }
        else if (audioState == 2)
        { // PAUSED state
            audioStatus = "paused";
            currentTrack = audio.getCurrentTrack();
        }

        // Sanitize strings
        wifiSSID.replace("\"", "\\\"");
        currentTrack.replace("\"", "\\\"");

        // Get NFC card ID
        String nfcCardId = "";
        if (isReedActive && isCardPresent)
        {
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
                               nfcCardId.length() > 0 ? "\"" : "");

        if (written > 0 && written < sizeof(jsonBuffer) - 1)
        {
            _ws->sendTXT(jsonBuffer, written);
        }
    }

    bool subscribeToPrivate()
    {
        if (_socketId.length() == 0 || !_httpClient)
        {
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

            int httpCode = http.POST((uint8_t *)tempBuffer, strlen(tempBuffer));

            if (httpCode != 200)
            {
                http.end();
                return false;
            }

            String authResponse = http.getString();
            http.end();

            int authStart = authResponse.indexOf("\"auth\":\"") + 8;
            int authEnd = authResponse.indexOf("\"", authStart);
            if (authStart < 8 || authEnd < 0)
            {
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

    void handleDeviceCommand(const char *payloadStr)
    {
        // Parse the command data from the escaped JSON
        // Look for the data field which contains escaped JSON with device_id, timestamp, type, and value
        char *dataStart = strstr(payloadStr, "\\\"data\\\":\\\"");
        if (!dataStart)
        {
            // Fallback: try unescaped version
            dataStart = strstr(payloadStr, "\"data\":\"");
            if (!dataStart)
            {
                Serial.println("ReverbClient: Could not find data field in command");
                return;
            }
            dataStart += 8; // Skip past "data":"
        }
        else
        {
            dataStart += 12; // Skip past "\"data\":\""
        }

        // Find the end of the data field - look for the closing quote before the comma or closing brace
        // We need to find the last \" before \",\"channel\" or \"}
        char *dataEnd = nullptr;
        char *searchPos = dataStart;

        // Look for the pattern that indicates end of data field
        char *channelStart = strstr(dataStart, "\\\",\\\"channel\\\"");
        if (!channelStart)
        {
            channelStart = strstr(dataStart, "\",\"channel\"");
        }

        if (channelStart)
        {
            dataEnd = channelStart;
        }
        else
        {
            // Fallback: look for closing quote near end
            char *endBrace = strstr(dataStart, "\"}");
            if (endBrace)
            {
                dataEnd = endBrace;
            }
        }

        if (!dataEnd)
        {
            Serial.println("ReverbClient: Could not find end of data field");
            return;
        }

        // Create a temporary buffer for the data content
        size_t dataLen = dataEnd - dataStart;
        if (dataLen >= sizeof(tempBuffer) - 1)
        {
            Serial.println("ReverbClient: Command data too large to parse");
            return;
        }

        // Copy and null-terminate the data
        memcpy(tempBuffer, dataStart, dataLen);
        tempBuffer[dataLen] = '\0';

        // Unescape the JSON data
        String unescapedData = String(tempBuffer);
        unescapedData.replace("\\\"", "\"");
        unescapedData.replace("\\\\", "\\");

        Serial.printf("ReverbClient: Parsing command data: %s\n", unescapedData.c_str());

        // Extract command type
        String commandType = "";
        int typeStart = unescapedData.indexOf("\"type\":\"");
        if (typeStart >= 0)
        {
            typeStart += 8; // Skip past "type":"
            int typeEnd = unescapedData.indexOf("\"", typeStart);
            if (typeEnd > typeStart)
            {
                commandType = unescapedData.substring(typeStart, typeEnd);
            }
        }

        // Extract command value (can be null, string, or number)
        String commandValue = "";
        bool hasValue = false;
        int valueStart = unescapedData.indexOf("\"value\":");
        if (valueStart >= 0)
        {
            valueStart += 8; // Skip past "value":

            // Skip whitespace
            while (valueStart < unescapedData.length() &&
                   (unescapedData.charAt(valueStart) == ' ' || unescapedData.charAt(valueStart) == '\t'))
            {
                valueStart++;
            }

            if (valueStart < unescapedData.length())
            {
                char valueChar = unescapedData.charAt(valueStart);
                if (valueChar == 'n')
                {
                    // Check for null
                    if (unescapedData.substring(valueStart, valueStart + 4) == "null")
                    {
                        hasValue = false;
                    }
                }
                else if (valueChar == '"')
                {
                    // String value
                    valueStart++; // Skip opening quote
                    int valueEnd = unescapedData.indexOf("\"", valueStart);
                    if (valueEnd > valueStart)
                    {
                        commandValue = unescapedData.substring(valueStart, valueEnd);
                        hasValue = true;
                    }
                }
                else if (isdigit(valueChar) || valueChar == '-')
                {
                    // Numeric value
                    int valueEnd = valueStart;
                    while (valueEnd < unescapedData.length() &&
                           (isdigit(unescapedData.charAt(valueEnd)) ||
                            unescapedData.charAt(valueEnd) == '.' ||
                            unescapedData.charAt(valueEnd) == '-'))
                    {
                        valueEnd++;
                    }
                    commandValue = unescapedData.substring(valueStart, valueEnd);
                    hasValue = true;
                }
            }
        }

        if (commandType.length() == 0)
        {
            Serial.println("ReverbClient: Could not extract command type");
            return;
        }

        Serial.printf("ReverbClient: Executing command - Type: %s, Value: %s, HasValue: %s\n",
                      commandType.c_str(),
                      commandValue.c_str(),
                      hasValue ? "true" : "false");

        executeCommand(commandType, commandValue, hasValue);
    }

    void executeCommand(const String &type, const String &value, bool hasValue)
    {
        // Get instances of controllers we might need
        AudioController &audio = AudioController::getInstance();

        // Convert type to lowercase for case-insensitive comparison
        String lowerType = type;
        lowerType.toLowerCase();

        if (lowerType == "volup")
        {
            Serial.println("ReverbClient: Executing volume up command");
            if (audio.volumeUp())
            {
                Serial.printf("ReverbClient: Volume increased to %d\n", audio.getCurrentVolume());
            }
            else
            {
                Serial.println("ReverbClient: Failed to increase volume (may be at maximum)");
            }
        }
        else if (lowerType == "voldown")
        {
            Serial.println("ReverbClient: Executing volume down command");
            if (audio.volumeDown())
            {
                Serial.printf("ReverbClient: Volume decreased to %d\n", audio.getCurrentVolume());
            }
            else
            {
                Serial.println("ReverbClient: Failed to decrease volume (may be at minimum)");
            }
        }
        else if (lowerType == "play")
        {
            Serial.println("ReverbClient: Executing play command");
            if (hasValue && value.length() > 0)
            {
                Serial.printf("ReverbClient: Playing track: %s\n", value.c_str());
                if (audio.play(value))
                {
                    Serial.println("ReverbClient: Track playback started successfully");
                }
                else
                {
                    Serial.println("ReverbClient: Failed to start track playback");
                }
            }
            else
            {
                Serial.println("ReverbClient: Resuming playback");
                if (audio.resume())
                {
                    Serial.println("ReverbClient: Playback resumed successfully");
                }
                else
                {
                    Serial.println("ReverbClient: Failed to resume playback");
                }
            }
        }
        else if (lowerType == "stop-track")
        {
            Serial.println("ReverbClient: Executing stop command");
            if (audio.stop())
            {
                Serial.println("ReverbClient: Playback stopped successfully");
            }
            else
            {
                Serial.println("ReverbClient: Failed to stop playback");
            }
        }
        else if (lowerType == "next-track")
        {
            Serial.println("ReverbClient: Executing next-track command");
            if (audio.nextTrack())
            {
                Serial.println("ReverbClient: Skipped to next track successfully");
            }
            else
            {
                Serial.println("ReverbClient: Failed to skip to next track (may be at end of playlist)");
            }
        }
        else if (lowerType == "prev-track")
        {
            Serial.println("ReverbClient: Executing previous-track command");
            if (audio.prevTrack())
            {
                Serial.println("ReverbClient: Skipped to previous track successfully");
            }
            else
            {
                Serial.println("ReverbClient: Failed to skip to previous track (may be at beginning of playlist)");
            }
        }
        else if (lowerType == "pause-track")
        {
            Serial.println("ReverbClient: Executing pause-track command");
            if (audio.pause())
            {
                Serial.println("ReverbClient: Playback paused successfully");
            }
            else
            {
                Serial.println("ReverbClient: Failed to pause playback");
            }
        }
        else if (lowerType == "resume-track")
        {
            Serial.println("ReverbClient: Executing resume-track command");
            if (audio.resume())
            {
                Serial.println("ReverbClient: Playback resumed successfully");
            }
            else
            {
                Serial.println("ReverbClient: Failed to resume playback");
            }
        }
        else if (lowerType == "volset")
        {
            if (hasValue && value.length() > 0)
            {
                int volume = value.toInt();
                if (volume >= AudioController::MIN_VOLUME && volume <= AudioController::MAX_VOLUME)
                {
                    Serial.printf("ReverbClient: Setting volume to: %d\n", volume);
                    if (audio.setVolume(volume))
                    {
                        Serial.printf("ReverbClient: Volume set to %d successfully\n", audio.getCurrentVolume());
                    }
                    else
                    {
                        Serial.println("ReverbClient: Failed to set volume");
                    }
                }
                else
                {
                    Serial.printf("ReverbClient: Invalid volume value: %d (must be %d-%d)\n",
                                  volume, AudioController::MIN_VOLUME, AudioController::MAX_VOLUME);
                }
            }
            else
            {
                Serial.println("ReverbClient: SetVolume command missing value");
            }
        }
        else if (lowerType == "seek")
        {
            if (hasValue && value.length() > 0)
            {
                int position = value.toInt();
                Serial.printf("ReverbClient: Seeking to position: %d\n", position);
                // audio.seekTo(position);
            }
            else
            {
                Serial.println("ReverbClient: Seek command missing value");
            }
        }
        else if (lowerType == "reboot" || lowerType == "restart")
        {
            Serial.println("ReverbClient: Executing reboot command");
            // Add a small delay to allow the response to be sent
            delay(1000);
            ESP.restart();
        }
        else
        {
            Serial.printf("ReverbClient: Unknown command type: %s\n", type.c_str());
        }
    }
};

// Define static buffers
char ReverbClient::jsonBuffer[512];
char ReverbClient::urlBuffer[128];
char ReverbClient::headerBuffer[256];
char ReverbClient::channelBuffer[64];
char ReverbClient::tempBuffer[256];

ReverbClient *ReverbClient::instance = nullptr;
