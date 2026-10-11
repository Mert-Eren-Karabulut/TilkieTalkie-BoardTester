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
    // Called when /broadcasting/auth rejects the stored hub token; returns a fresh token or "".
    void onAuthTokenRefresh(std::function<String()> cb) { _tokenRefreshCb = cb; }
    void setAuthToken(const String &token) { _authToken = token; }

    // apiOrigin: scheme+host(+port) of the Laravel app for /broadcasting/auth and /api/chat
    // (production: https://portal.tilkietalkie.com; bench: http://192.168.x.x:8000).
    void begin(
        const char *host,
        uint16_t port,
        const char *appKey,
        const char *authToken,
        const char *deviceId,
        bool useTls = true,
        const char *apiOrigin = nullptr);

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
    void sendDeviceReport(bool force);
    void startWebSocket();
    int subscribeToPrivate();      // 0 = subscribe frame sent, otherwise the HTTP status (or -1)
    void serviceSubscription();    // runs the channel auth from update(), never inside the WS callback
    void bumpReconnectBackoff();
    void handleDeviceCommand(const char *payloadStr);
    void executeCommand(const String &type, const String &value, bool hasValue);
    static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
    void handleEvent(WStype_t type, uint8_t *payload, size_t length);

    // Member variables
    WebSocketsClient *_ws = nullptr;
    WiFiClient *_httpClient = nullptr; // WiFiClientSecure (https API origin) or plain WiFiClient (bench http)
    bool _httpSecure = true;
    String _host, _appKey, _authToken, _deviceId, _socketId;
    uint16_t _port;
    bool _useTls = true;
    String _apiOrigin;
    std::function<void(const String &)> _chatCb;
    bool _isConnected = false;
    bool _isPrivateSubscribed = false;
    bool _initialized = false;
    bool _wsStarted = false;
    bool _pendingSubscribe = false;
    unsigned long _subscribeRetryAt = 0;
    uint8_t _subscribeFailures = 0;
    unsigned long _reconnectDelayMs = 2000;
    std::function<String()> _tokenRefreshCb;
    // Report cadence: on change, every 5 s while playing (position), at least every 60 s.
    unsigned long _lastReportAt = 0;
    unsigned long _lastPositionReportAt = 0;
    String _lastReportSignature;
    bool _reportDue = false;
    
    // Pre-allocated static buffers to reduce heap fragmentation
    static char jsonBuffer[768];
    static char urlBuffer[128];
    static char headerBuffer[256];
    static char channelBuffer[64];
    static char tempBuffer[256];

    static ReverbClient *instance;
};
