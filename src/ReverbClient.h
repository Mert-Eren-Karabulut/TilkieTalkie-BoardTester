#pragma once

#include <Arduino.h>
#include <functional>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>

class ReverbClient
{
public:
    static ReverbClient &getInstance();

    void onChatMessage(std::function<void(const String &)> cb);

    void begin(
        const char *host,
        uint16_t port,
        const char *appKey,
        const char *authToken,
        const char *deviceId);

    void update();
    void cleanup();
    bool isConnected();
    String getConnectionStatus();
    void disconnect();
    void forceReconnect();
    bool sendMessage(const String &message);

private:
    // Private constructor for singleton
    ReverbClient() = default;
    ~ReverbClient() = default;
    ReverbClient(const ReverbClient&) = delete;
    ReverbClient& operator=(const ReverbClient&) = delete;

    // Private methods - declarations only
    void sendDeviceReport();
    bool subscribeToPrivate();
    void handleDeviceCommand(const char *payloadStr);
    void executeCommand(const String &type, const String &value, bool hasValue);
    static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
    void handleEvent(WStype_t type, uint8_t *payload, size_t length);

    // Member variables
    WebSocketsClient *_ws = nullptr;
    WiFiClientSecure *_httpClient = nullptr;
    String _host, _appKey, _authToken, _deviceId, _socketId;
    uint16_t _port;
    std::function<void(const String &)> _chatCb;
    bool _isConnected = false;
    bool _initialized = false;
    bool _wsStarted = false;
    
    // Pre-allocated static buffers to reduce heap fragmentation
    static char jsonBuffer[512];
    static char urlBuffer[128];
    static char headerBuffer[256];
    static char channelBuffer[64];
    static char tempBuffer[256];

    static ReverbClient *instance;
};
