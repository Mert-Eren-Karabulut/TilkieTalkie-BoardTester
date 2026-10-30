#pragma once

#include <Arduino.h>
#include <functional>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include <ArduinoJson.h>

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

    // update() runs on the loop task: it hands the commands, chat messages and fault flags the
    // WebSocket task received over to their owners (audio, RequestManager, Faults) and builds the
    // device report. The socket itself (frames, keepalive pings, TLS reconnects, the channel auth
    // HTTPS) lives on its own task ("reverb", core 0), so a loop stall never drops the connection
    // and a slow handshake never stalls the loop.
    void update();
    void cleanup();
    bool isConnected();
    unsigned long lastConnectedAt() const { return _lastConnectedAt; } // millis(), 0 = never this boot
    String getConnectionStatus();
    void disconnect();
    void forceReconnect();
    bool sendMessage(const String &message);
    // Stories inventory: the SD maintenance task queues one JSON fragment per figure
    // ("seq":..,"total":..,"figure":{..},"sd":{..}); update() sends them as "client-device-files".
    static void queueDeviceFiles(const String &innerJson);
    // Reports every `seconds` (2..60) for the next two minutes (the portal asks while a page is open).
    void setReportInterval(uint32_t seconds);

    // Child-facing error code of the last problem (fruit names, see docs/VOICE-PROMPTS.md), sent in reports.
    static void setLastError(const char *code);
    static const char *lastError() { return _lastError; }

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
    void handleDeviceCommand(JsonVariantConst data);   // the parsed "data" of device.command.sent
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
    std::atomic<bool> _isConnected{false};
    std::atomic<bool> _isPrivateSubscribed{false};
    std::atomic<bool> _initialized{false};
    bool _wsStarted = false;
    bool _pendingSubscribe = false;
    // ---- the WebSocket task and the hand-offs between it and the loop ----
    TaskHandle_t _task = nullptr;
    SemaphoreHandle_t _wsLock = nullptr;        // recursive: the task's iteration, and the loop's disconnect/cleanup
    struct Handoff { String *type; String *value; bool hasValue; }; // a command (type) or a chat message (type == nullptr)
    QueueHandle_t _inbox = nullptr;             // task -> loop: commands and chat messages
    std::atomic<uint8_t> _deferredFaults{0};    // task -> loop: 1 WS_DISCONNECTED, 2 WS_AUTH_FAILED (code in _deferredAuthCode), 4 clear WS_AUTH_FAILED
    std::atomic<int> _deferredAuthCode{0};
    std::atomic<size_t> _reportLen{0};          // loop -> task: a report waiting in reportBuffer
    static char *reportBuffer;
    static void taskEntry(void *arg);
    void taskIteration();
    void ensureTask();
    unsigned long _subscribeRetryAt = 0;
    uint8_t _subscribeFailures = 0;
    unsigned long _reconnectDelayMs = 2000;
    std::function<String()> _tokenRefreshCb;
    // Report cadence: on change, every 5 s while playing (position), heartbeat every 10 s docked /
    // 30 s on battery, faster while the portal page asks (set-report-interval).
    unsigned long _lastReportAt = 0;
    unsigned long _lastPositionReportAt = 0;
    String _lastReportSignature;
    unsigned long _lastConnectedAt = 0;
    float _lastReportedPercent = -100.0f;
    int _lastReportedCurrentMa = 0;
    float _lastReportedVoltage = 0.0f;
    int _lastReportedRssi = 0;
    uint32_t _lastReportedSdFree = 0;
    std::atomic<bool> _reportDue{false};
    unsigned long _lastReportEvalAt = 0;     // the report state is gathered at most once a second
    unsigned long _lastOverflowLogAt = 0;
    unsigned long _fastReportMs = 0;
    unsigned long _fastReportUntil = 0;
    static QueueHandle_t _outbox;
    void drainOutbox();
    
    // Pre-allocated buffers to reduce heap fragmentation. The report frame lives in PSRAM
    // (allocated once in begin()): with fw, 8 active faults and an escaped detail it no longer
    // fits the old 1.5 KB internal buffer.
    static constexpr size_t JSON_BUFFER_LEN = 2560;
    static char *jsonBuffer;
    static char urlBuffer[128];
    static char headerBuffer[256];
    static char channelBuffer[64];
    static char tempBuffer[256];
    static char _lastError[16];

    static ReverbClient *instance;
};
