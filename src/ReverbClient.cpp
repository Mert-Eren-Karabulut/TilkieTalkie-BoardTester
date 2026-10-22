#include "ReverbClient.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>

// Include device components for status reporting
#include "BatteryManagement.h"
#include "FileManager.h"
#include "AudioController.h"
#include "NfcController.h"
#include "ConfigManager.h"
#include "CaBundle.h"
#include "OtaManager.h"
#include "PromptPackManager.h"
#include "Faults.h"
#include "RequestManager.h"
#include "PsramJson.h"
#include "TimeSync.h"
#include "Version.h"
#include <esp_heap_caps.h>
#include <esp_log.h>

static const char *TAG = "reverb";

// JSON string escaping for values put into hand-built frames: quotes, backslashes and control
// characters (track paths and names come from the portal, card ids from the reader).
static String jsonEscape(const String &in)
{
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        const char c = in[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((uint8_t)c < 0x20) {
                char u[8];
                snprintf(u, sizeof(u), "\\u%04x", (unsigned)(uint8_t)c);
                out += u;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// Define static members
char *ReverbClient::jsonBuffer = nullptr;
char ReverbClient::urlBuffer[128];
char ReverbClient::headerBuffer[256];
char ReverbClient::channelBuffer[64];
char ReverbClient::tempBuffer[256];
char ReverbClient::_lastError[16] = "";
QueueHandle_t ReverbClient::_outbox = nullptr;

void ReverbClient::setLastError(const char *code)
{
    strlcpy(_lastError, code ? code : "", sizeof(_lastError));
}
ReverbClient *ReverbClient::instance = nullptr;

ReverbClient &ReverbClient::getInstance()
{
    static ReverbClient inst;
    return inst;
}

void ReverbClient::onChatMessage(std::function<void(const String &)> cb)
{
    _chatCb = cb;
    ESP_LOGI(TAG, "Chat message callback registered");
}

void ReverbClient::begin(const char *host, uint16_t port, const char *appKey, const char *authToken,
                         const char *deviceId, bool useTls, const char *apiOrigin)
{

    _host = host;
    _port = port;
    _appKey = appKey;
    _authToken = authToken;
    _deviceId = deviceId;
    _useTls = useTls;
    _apiOrigin = (apiOrigin && apiOrigin[0]) ? String(apiOrigin) : String(_useTls ? "https://" : "http://") + _host;
    _initialized = true;
    if (!jsonBuffer)
    {
        jsonBuffer = static_cast<char *>(heap_caps_malloc(JSON_BUFFER_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!jsonBuffer)
        {
            jsonBuffer = static_cast<char *>(malloc(JSON_BUFFER_LEN));
        }
    }

    // HTTP client for /broadcasting/auth and chat: TLS or plain depending on the API origin.
    if (!_httpClient)
    {
        _httpSecure = _apiOrigin.startsWith("https://");
        if (_httpSecure)
        {
            WiFiClientSecure *secure = new WiFiClientSecure();
            secure->setCACertBundle(caBundleStart(), caBundleSize()); // verify the portal's certificate
            _httpClient = secure;
        }
        else
        {
            _httpClient = new WiFiClient();
        }
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

    ESP_LOGI(TAG, "Initialized (%s://%s:%u, api %s), will connect when WiFi is available",
                  _useTls ? "wss" : "ws", _host.c_str(), _port, _apiOrigin.c_str());

    // Start connection if WiFi is already available
    if (WiFi.isConnected())
    {
        startWebSocket();
    }
}

// The WebSocket path lives in its own buffer: urlBuffer is reused by the HTTP helpers, and a
// reconnect after a WiFi drop used to pass the /broadcasting/auth URL as the WS path.
void ReverbClient::startWebSocket()
{
    static char wsPathBuffer[128];
    snprintf(wsPathBuffer, sizeof(wsPathBuffer), "/app/%s?protocol=7&client=esp32-client&version=1.0", _appKey.c_str());
    if (_useTls)
    {
        _ws->beginSslWithBundle(_host.c_str(), _port, wsPathBuffer, caBundleStart(), caBundleSize());
    }
    else
    {
        _ws->begin(_host.c_str(), _port, wsPathBuffer);
    }
}

void ReverbClient::update()
{
    // Handle WiFi state changes
    if (!WiFi.isConnected())
    {
        if (_isConnected || _wsStarted)
        {
            ESP_LOGI(TAG, "WiFi disconnected, stopping WebSocket");
            if (_ws)
            {
                _ws->disconnect();
            }
            _isConnected = false;
            _isPrivateSubscribed = false;
            _pendingSubscribe = false;
            _wsStarted = false;
        }
        return;
    }

    // WiFi is connected - start WebSocket if not already started
    if (!_wsStarted && _initialized)
    {
        ESP_LOGI(TAG, "Starting WebSocket connection");
        startWebSocket();
        _wsStarted = true;
    }

    // Let the library handle everything (reconnection, heartbeat, etc.)
    if (_ws)
    {
        _ws->loop();
        drainOutbox();
    }

    serviceSubscription();

    if (isConnected())
    {
        _lastConnectedAt = millis();
        sendDeviceReport(false);
    }
}

void ReverbClient::serviceSubscription()
{
    if (!_pendingSubscribe || !_isConnected || _socketId.isEmpty() || millis() < _subscribeRetryAt)
    {
        return;
    }

    int code = subscribeToPrivate();
    if (code == 0)
    {
        _pendingSubscribe = false; // confirmed by pusher_internal:subscription_succeeded
        return;
    }

    _subscribeFailures++;
    unsigned long delayMs = 5000UL << (_subscribeFailures > 4 ? 3 : _subscribeFailures - 1); // 5, 10, 20, 40 s
    if (delayMs > 60000UL)
    {
        delayMs = 60000UL;
    }
    _subscribeRetryAt = millis() + delayMs;
    ESP_LOGW(TAG, "channel auth failed (HTTP %d), retry in %lu s", code, delayMs / 1000);
    if (code == 401 || code == 403)
    {
        char d[16];
        snprintf(d, sizeof(d), "http=%d", code);
        Faults::raise(Fault::WS_AUTH_FAILED, d);
    }

    // 401/403 means the token is bad; a backend that answers 419/5xx twice in a row to the same
    // token is treated the same way (a revoked token produced 500s before the backend fix).
    if ((code == 401 || code == 403 || _subscribeFailures >= 2) && _tokenRefreshCb)
    {
        String fresh = _tokenRefreshCb();
        if (fresh.length() > 0)
        {
            _authToken = fresh;
            ESP_LOGI(TAG, "hub token refreshed for channel auth");
        }
    }
}

void ReverbClient::bumpReconnectBackoff()
{
    _reconnectDelayMs = _reconnectDelayMs >= 30000UL ? 60000UL : _reconnectDelayMs * 2;
    if (_ws)
    {
        _ws->setReconnectInterval(_reconnectDelayMs);
    }
    ESP_LOGI(TAG, "next reconnect attempt in %lu s", _reconnectDelayMs / 1000);
}

bool ReverbClient::isConnected()
{
    return WiFi.isConnected() && _ws && _ws->isConnected() && _isPrivateSubscribed;
}

String ReverbClient::getConnectionStatus()
{
    if (!WiFi.isConnected())
    {
        return "WiFi Disconnected";
    }

    if (!_wsStarted)
    {
        return "WebSocket Not Started";
    }

    if (_ws && _ws->isConnected() && _isPrivateSubscribed)
    {
        return "Fully Connected";
    }

    return "WebSocket Connecting...";
}

void ReverbClient::disconnect()
{
    ESP_LOGI(TAG, "Manual disconnect requested");
    if (_ws)
    {
        _ws->disconnect();
    }
    _isConnected = false;
    _wsStarted = false;
}

void ReverbClient::forceReconnect()
{
    ESP_LOGI(TAG, "Force reconnection requested");
    disconnect();

    if (WiFi.isConnected() && _initialized)
    {
        ESP_LOGI(TAG, "Restarting WebSocket connection");
        startWebSocket();
        _wsStarted = true;
    }
}

void ReverbClient::cleanup()
{
    ESP_LOGI(TAG, "Cleaning up resources");

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
        if (_httpSecure) { delete static_cast<WiFiClientSecure *>(_httpClient); } else { delete _httpClient; }
        _httpClient = nullptr;
    }
}

bool ReverbClient::sendMessage(const String &text)
{
    if (!isConnected() || !_httpClient)
    {
        ESP_LOGW(TAG, "Cannot send message - Connection status: %s",
                      getConnectionStatus().c_str());
        return false;
    }

    HTTPClient http;
    snprintf(urlBuffer, sizeof(urlBuffer), "%s/api/chat/device/%s", _apiOrigin.c_str(), _deviceId.c_str());

    if (http.begin(*_httpClient, urlBuffer))
    {
        snprintf(headerBuffer, sizeof(headerBuffer), "Bearer %s", _authToken.c_str());
        http.addHeader("Authorization", headerBuffer);
        http.addHeader("Content-Type", "application/json");

        // Build minimal JSON payload manually
        const String escapedText = jsonEscape(text);
        snprintf(tempBuffer, sizeof(tempBuffer), "{\"text\":\"%s\"}", escapedText.c_str());

        ESP_LOGI(TAG, "Sending message: %s", text.c_str());
        int httpCode = http.POST((uint8_t *)tempBuffer, strlen(tempBuffer));
        http.end();

        if (httpCode == 200)
        {
            ESP_LOGI(TAG, "Message sent successfully");
            return true;
        }
        else
        {
            ESP_LOGW(TAG, "Failed to send message, HTTP code: %d", httpCode);
            return false;
        }
    }
    ESP_LOGW(TAG, "Failed to initialize HTTP request");
    return false;
}

void ReverbClient::webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    if (instance)
        instance->handleEvent(type, payload, length);
}

void ReverbClient::handleEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket disconnected");
        if (_isConnected)
        {
            Faults::raise(Fault::WS_DISCONNECTED, nullptr, false); // a real drop, not a failed reconnect; counted by the dedupe
        }
        _isConnected = false;
        _isPrivateSubscribed = false;
        _pendingSubscribe = false;
        _socketId = "";
        bumpReconnectBackoff();
        break;

    case WStype_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected to: %s", payload);
        _isConnected = true;
        break;

    case WStype_ERROR:
        ESP_LOGI(TAG, "WebSocket error: %.*s", length, payload);
        _isConnected = false;
        _isPrivateSubscribed = false;
        _pendingSubscribe = false;
        _socketId = "";
        bumpReconnectBackoff();
        break;

    case WStype_TEXT:
    {
        // Pusher frames: {"event":"...","channel":"...","data":"<JSON as a string>"} (or data as an
        // object). Parsed with ArduinoJson into PSRAM: escaped quotes and \uXXXX (Turkish letters)
        // survive, and routing is on the event name, not on substrings of the whole frame.
        JsonDocument frame(&psramJsonAllocator);
        const DeserializationError err = deserializeJson(frame, (const char *)payload, length);
        if (err)
        {
            ESP_LOGW(TAG, "unparsable frame (%s, %u bytes)", err.c_str(), (unsigned)length);
            break;
        }
        const char *event = frame["event"] | "";
        JsonDocument data(&psramJsonAllocator);
        JsonVariantConst rawData = frame["data"];
        if (rawData.is<const char *>())
        {
            if (deserializeJson(data, rawData.as<const char *>()))
            {
                data.clear(); // a plain string (or empty): no fields
            }
        }
        else
        {
            data.set(rawData);
        }

        if (strcmp(event, "pusher:connection_established") == 0)
        {
            const char *socketId = data["socket_id"] | "";
            if (socketId[0])
            {
                _socketId = socketId;
                ESP_LOGI(TAG, "Connected with socket ID: %s", _socketId.c_str());
                // The channel auth is an HTTP round trip: done from update(), not here.
                _isPrivateSubscribed = false;
                _pendingSubscribe = true;
                _subscribeRetryAt = 0;
                _subscribeFailures = 0;
                _reconnectDelayMs = 2000;
                _ws->setReconnectInterval(_reconnectDelayMs);
            }
            else
            {
                ESP_LOGW(TAG, "connection_established without a socket_id");
            }
        }
        else if (strcmp(event, "pusher_internal:subscription_succeeded") == 0)
        {
            ESP_LOGI(TAG, "Successfully subscribed to private channel");
            _isPrivateSubscribed = true;
            _reportDue = true;
        }
        else if (strcmp(event, "pusher:error") == 0)
        {
            ESP_LOGI(TAG, "server error: %.*s", (int)length, payload);
        }
        else if (strcmp(event, "pusher:ping") == 0)
        {
            const char *pong = "{\"event\":\"pusher:pong\",\"data\":{}}";
            _ws->sendTXT(pong, strlen(pong));
        }
        else if (strcmp(event, "pusher:pong") == 0 || strcmp(event, "device.status.updated") == 0 ||
                 strcmp(event, "client-device-report") == 0 || strcmp(event, "device.files.updated") == 0 ||
                 strcmp(event, "client-device-files") == 0 || strcmp(event, "hub.firmware.updated") == 0 ||
                 strcmp(event, "hub.events.updated") == 0)
        {
            // Our own events relayed back, or broadcasts meant for the portal pages.
        }
        else if (strcmp(event, "device.command.sent") == 0)
        {
            handleDeviceCommand(data.as<JsonVariantConst>());
        }
        else if (strcmp(event, "chat-message") == 0)
        {
            const char *text = data["text"] | (const char *)nullptr;
            if (!text)
            {
                ESP_LOGW(TAG, "chat message without text");
            }
            else if (!_chatCb)
            {
                ESP_LOGE(TAG, "ERROR - No callback registered for chat messages!");
            }
            else
            {
                ESP_LOGI(TAG, "Received chat message: %s", text);
                _chatCb(String(text));
            }
        }
        else
        {
            ESP_LOGD(TAG, "event '%s' ignored", event);
        }
        break;
    }
    default:
        break;
    }
}

void ReverbClient::sendDeviceReport(bool force)
{
    if (!isConnected() || !jsonBuffer)
        return;

    // Gathering the state and building the signature costs a few String allocations and
    // calls into every subsystem: do it at most once a second unless a report is forced.
    const unsigned long evalNow = millis();
    if (!force && !_reportDue && evalNow - _lastReportEvalAt < 1000UL)
        return;
    _lastReportEvalAt = evalNow;

    BatteryManager &battery = BatteryManager::getInstance();
    FileManager &fileManager = FileManager::getInstance();
    AudioController &audio = AudioController::getInstance();
    NfcController &nfc = NfcController::getInstance();

    bool isCharging = battery.getChargingStatus();
    float batteryPercent = battery.getBatteryPercentage();
    float batteryVoltage = battery.getBatteryVoltage();
    const int pendingDownloads = fileManager.getPendingDownloadsCount();
    bool isFileSyncing = pendingDownloads > 0;
    const int downloadProgress = fileManager.getDownloadProgress(); // -1 when nothing is in flight
    String downloadName = fileManager.getActiveDownloadPath();
    downloadName = jsonEscape(downloadName.substring(downloadName.lastIndexOf('/') + 1));
    uint32_t sdFreeSpace = fileManager.getSDCardFreeMB(); // reported as MB
    int wifiRSSI = WiFi.RSSI();
    int audioState = audio.getState();
    bool isPogoActive = nfc.isPogoSwitchActive();
    bool isCardPresent = nfc.isCardPresent();

    const char *audioStatus = "stopped";
    String currentTrack = "";
    int currentSecond = 0;
    int currentVolume = audio.getCurrentVolume();
    int volumeCeiling = audio.getVolumeCeiling();
    if (audioState == 1)
    {
        audioStatus = "playing";
        currentTrack = audio.getCurrentTrack();
        currentSecond = (int)audio.getCurrentTrackSeconds();
    }
    else if (audioState == 2)
    {
        audioStatus = "paused";
        currentTrack = audio.getCurrentTrack();
        currentSecond = (int)audio.getCurrentTrackSeconds();
    }
    currentTrack = jsonEscape(currentTrack);

    String nfcCardId = "";
    if (isPogoActive && isCardPresent)
    {
        nfcCardId = jsonEscape(nfc.currentNFCData().uidString);
    }

    // A report goes out when something meaningful changed (with hysteresis on the analog values,
    // so a cell voltage hovering on a boundary does not cause a report every poll), every 5 s
    // while playing (position), and as a heartbeat (10 s docked, 30 s on battery).
    PromptPackManager &ppm = PromptPackManager::getInstance();
    const String promptPack = ppm.installedChecksum().substring(0, 12);
    const String faultsJson = Faults::reportJson();
    // The running firmware ("fw") and the prompt pack ("prompts") let the portal clear a stale
    // ota_status after a USB flash (it used to learn the version from the OTA check only).
    const String fwVersion = jsonEscape(String(firmwareVersion()));
    strlcpy(_lastError, Faults::fruit(), sizeof(_lastError)); // older portal code reads the fruit
    char signature[220];
    snprintf(signature, sizeof(signature), "%d|%d|%d|%d|%s|%s|%d|%d|%d|%s|%s|%s",
             isCharging ? 1 : 0, isFileSyncing ? 1 : 0, pendingDownloads, downloadProgress / 10,
             audioStatus, currentTrack.c_str(),
             currentVolume, volumeCeiling, isPogoActive ? 1 : 0, nfcCardId.c_str(),
             ppm.stateName(), OtaManager::getInstance().stateName());
    String signatureStr = String(signature) + "|" + faultsJson;

    unsigned long now = millis();
    bool changed = (_lastReportSignature != signatureStr) ||
                   fabsf(batteryPercent - _lastReportedPercent) >= 2.0f ||
                   fabsf(batteryVoltage - _lastReportedVoltage) >= 0.10f ||
                   abs(wifiRSSI - _lastReportedRssi) >= 10 ||
                   sdFreeSpace != _lastReportedSdFree;
    // Heartbeat: docked 10 s (power is free, the owner is likely looking at the portal), on
    // battery 30 s; while the portal page is watching (set-report-interval): its rate.
    const bool fast = _fastReportUntil != 0 && (long)(_fastReportUntil - now) > 0;
    const unsigned long heartbeatMs = fast ? _fastReportMs : (battery.isVbusPresent() ? 10000UL : 30000UL);
    bool heartbeat = (now - _lastReportAt) >= heartbeatMs;
    const unsigned long positionMs = (fast && _fastReportMs < 5000UL) ? _fastReportMs : 5000UL;
    bool position = (audioState == 1) && (now - _lastPositionReportAt) >= positionMs;
    if (!force && !_reportDue && !changed && !heartbeat && !position)
    {
        return;
    }

    snprintf(channelBuffer, sizeof(channelBuffer), "private-device.%s", _deviceId.c_str());

    int written = snprintf(jsonBuffer, JSON_BUFFER_LEN,
                           "{"
                           "\"event\":\"client-device-report\","
                           "\"channel\":\"%s\","
                           "\"data\":{"
                           "\"device_report\":{"
                           "\"device_id\":\"%s\","
                           "\"last_error\":\"%s\","
                           "\"fw\":{\"v\":\"%s\",\"code\":%u},"
                           "\"ota\":{\"state\":\"%s\",\"progress\":%d},"
                           "\"prompts\":{\"pack\":\"%s\",\"state\":\"%s\"},"
                           "\"faults\":{%s},"
                           "\"battery\":{"
                           "\"status\":\"%s\","
                           "\"percent\":%.1f,"
                           "\"voltage\":%.2f"
                           "},"
                           "\"files\":{"
                           "\"status\":\"%s\","
                           "\"sd_remaining\":%u,"
                           "\"wifi_rssi\":%d,"
                           "\"pending\":%d,"
                           "\"downloading\":\"%s\","
                           "\"progress\":%d"
                           "},"
                           "\"audio\":{"
                           "\"current_track_status\":\"%s\""
                           "%s%s%s"
                           ",\"current_track_second\":%d"
                           ",\"current_volume\":%d"
                           ",\"volume_ceiling\":%d"
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
                           _lastError,
                           fwVersion.c_str(),
                           (unsigned)firmwareVersionCode(),
                           OtaManager::getInstance().stateName(),
                           OtaManager::getInstance().progress(),
                           promptPack.c_str(),
                           ppm.stateName(),
                           faultsJson.c_str(),
                           isCharging ? "charging" : "discharging",
                           batteryPercent,
                           batteryVoltage,
                           isFileSyncing ? "syncing" : "in_sync",
                           sdFreeSpace,
                           wifiRSSI,
                           pendingDownloads,
                           downloadName.c_str(),
                           downloadProgress,
                           audioStatus,
                           currentTrack.length() > 0 ? ",\"current_track_id\":\"" : "",
                           currentTrack.c_str(),
                           currentTrack.length() > 0 ? "\"" : "",
                           currentSecond,
                           currentVolume,
                           volumeCeiling,
                           isPogoActive ? "present" : "empty",
                           nfcCardId.length() > 0 ? ",\"docked_card_id\":\"" : "",
                           nfcCardId.c_str(),
                           nfcCardId.length() > 0 ? "\"" : "");

    const bool overflow = written <= 0 || written >= (int)JSON_BUFFER_LEN - 1;
    if (overflow)
    {
        if (_lastOverflowLogAt == 0 || now - _lastOverflowLogAt >= 60000UL)
        {
            _lastOverflowLogAt = now;
            ESP_LOGW(TAG, "device report too large (%d bytes), skipped", written);
        }
    }
    // An oversized report is treated like a sent one for the cadence: it is retried at the next
    // heartbeat or change, not on every loop pass.
    if (overflow || _ws->sendTXT(jsonBuffer, written))
    {
        _lastReportAt = now;
        _lastPositionReportAt = now;
        _lastReportSignature = signatureStr;
        _lastReportedPercent = batteryPercent;
        _lastReportedVoltage = batteryVoltage;
        _lastReportedRssi = wifiRSSI;
        _lastReportedSdFree = sdFreeSpace;
        _reportDue = false;
    }
}

int ReverbClient::subscribeToPrivate()
{
    if (_socketId.length() == 0 || !_httpClient)
    {
        return -1;
    }

    HTTPClient http;
    snprintf(urlBuffer, sizeof(urlBuffer), "%s/broadcasting/auth", _apiOrigin.c_str());

    if (http.begin(*_httpClient, urlBuffer))
    {
        http.addHeader("Content-Type", "application/json");
        snprintf(headerBuffer, sizeof(headerBuffer), "Bearer %s", _authToken.c_str());
        http.addHeader("Authorization", headerBuffer);

        snprintf(channelBuffer, sizeof(channelBuffer), "private-device.%s", _deviceId.c_str());
        snprintf(tempBuffer, sizeof(tempBuffer),
                 "{\"socket_id\":\"%s\",\"channel_name\":\"%s\"}",
                 _socketId.c_str(), channelBuffer);

        int httpCode = http.POST((uint8_t *)tempBuffer, strlen(tempBuffer));

        if (httpCode != 200)
        {
            http.end();
            return httpCode > 0 ? httpCode : -1;
        }

        String authResponse = http.getString();
        http.end();

        int authStart = authResponse.indexOf("\"auth\":\"") + 8;
        int authEnd = authResponse.indexOf("\"", authStart);
        if (authStart < 8 || authEnd < 0)
        {
            return -1;
        }
        String authValue = authResponse.substring(authStart, authEnd);

        if (!jsonBuffer)
        {
            return -1;
        }
        snprintf(jsonBuffer, JSON_BUFFER_LEN,
                 "{\"event\":\"pusher:subscribe\",\"data\":{\"auth\":\"%s\",\"channel\":\"%s\"}}",
                 authValue.c_str(), channelBuffer);

        _ws->sendTXT(jsonBuffer, strlen(jsonBuffer));
        return 0;
    }
    return -1;
}

void ReverbClient::handleDeviceCommand(JsonVariantConst data)
{
    // {"device_id","timestamp","issued_at","type","value"} from the portal's DeviceCommandSent.
    if (!data.is<JsonObjectConst>())
    {
        ESP_LOGW(TAG, "command without a data object, ignored");
        return;
    }
    const char *deviceId = data["device_id"] | (const char *)nullptr;
    if (deviceId && _deviceId != deviceId)
    {
        ESP_LOGI(TAG, "command addressed to another device, ignored");
        return;
    }
    const char *type = data["type"] | "";
    if (!type[0])
    {
        ESP_LOGW(TAG, "command without a type, ignored");
        return;
    }
    // A command that sat in a stopped queue worker must not fire minutes later (a "play" at
    // night, a stale ota-update): dropped when older than 5 min, once the clock is set.
    JsonVariantConst issuedAt = data["issued_at"];
    if (!issuedAt.isNull() && TimeSync::valid())
    {
        const double issued = issuedAt.is<const char *>() ? atof(issuedAt.as<const char *>()) : issuedAt.as<double>();
        const double age = (double)TimeSync::now() - issued;
        if (issued > 0 && age > 300.0)
        {
            ESP_LOGW(TAG, "command '%s' issued %.0f s ago - dropped (older than 5 min)", type, age);
            return;
        }
    }

    // value: null, a string or a number (kept as its JSON text, e.g. 42 or 0.5).
    String commandValue;
    bool hasValue = false;
    JsonVariantConst value = data["value"];
    if (value.is<const char *>())
    {
        commandValue = value.as<const char *>();
        hasValue = true;
    }
    else if (value.is<bool>())
    {
        commandValue = value.as<bool>() ? "true" : "false";
        hasValue = true;
    }
    else if (!value.isNull())
    {
        serializeJson(value, commandValue);
        hasValue = true;
    }

    ESP_LOGI(TAG, "Executing command - Type: %s, Value: %s, HasValue: %s", type, commandValue.c_str(),
                  hasValue ? "true" : "false");
    executeCommand(String(type), commandValue, hasValue);
}

void ReverbClient::executeCommand(const String &type, const String &value, bool hasValue)
{
    // Get instances of controllers we might need
    AudioController &audio = AudioController::getInstance();

    // Convert type to lowercase for case-insensitive comparison
    String lowerType = type;
    lowerType.toLowerCase();

    if (lowerType == "volup")
    {
        ESP_LOGI(TAG, "Executing volume up command");
        if (audio.volumeUp())
        {
            ESP_LOGI(TAG, "Volume increased to %d", audio.getCurrentVolume());
        }
        else
        {
            ESP_LOGW(TAG, "Failed to increase volume (may be at maximum)");
        }
    }
    else if (lowerType == "voldown")
    {
        ESP_LOGI(TAG, "Executing volume down command");
        if (audio.volumeDown())
        {
            ESP_LOGI(TAG, "Volume decreased to %d", audio.getCurrentVolume());
        }
        else
        {
            ESP_LOGW(TAG, "Failed to decrease volume (may be at minimum)");
        }
    }
    else if (lowerType == "play")
    {
        ESP_LOGI(TAG, "Executing play command");
        if (hasValue && value.length() > 0)
        {
            if (!value.startsWith("/assets/"))
            {
                ESP_LOGW(TAG, "play refused - only files under /assets/ can be started remotely");
            }
            else if (audio.play(value))
            {
                ESP_LOGI(TAG, "Track playback started successfully");
            }
            else
            {
                ESP_LOGW(TAG, "Failed to start track playback");
            }
        }
        else
        {
            ESP_LOGI(TAG, "Resuming playback");
            if (audio.resume())
            {
                ESP_LOGI(TAG, "Playback resumed successfully");
            }
            else
            {
                ESP_LOGW(TAG, "Failed to resume playback");
            }
        }
    }
    else if (lowerType == "stop-track")
    {
        ESP_LOGI(TAG, "Executing stop command");
        if (audio.stop())
        {
            ESP_LOGI(TAG, "Playback stopped successfully");
        }
        else
        {
            ESP_LOGW(TAG, "Failed to stop playback");
        }
    }
    else if (lowerType == "next-track")
    {
        ESP_LOGI(TAG, "Executing next-track command");
        if (audio.nextTrack())
        {
            ESP_LOGI(TAG, "Skipped to next track successfully");
        }
        else
        {
            ESP_LOGW(TAG, "Failed to skip to next track (may be at end of playlist)");
        }
    }
    else if (lowerType == "prev-track")
    {
        ESP_LOGI(TAG, "Executing previous-track command");
        if (audio.prevTrack())
        {
            ESP_LOGI(TAG, "Skipped to previous track successfully");
        }
        else
        {
            ESP_LOGW(TAG, "Failed to skip to previous track (may be at beginning of playlist)");
        }
    }
    else if (lowerType == "pause-track")
    {
        ESP_LOGI(TAG, "Executing pause-track command");
        if (audio.pause())
        {
            ESP_LOGI(TAG, "Playback paused successfully");
        }
        else
        {
            ESP_LOGW(TAG, "Failed to pause playback");
        }
    }
    else if (lowerType == "resume-track")
    {
        ESP_LOGI(TAG, "Executing resume-track command");
        if (audio.resume())
        {
            ESP_LOGI(TAG, "Playback resumed successfully");
        }
        else
        {
            ESP_LOGW(TAG, "Failed to resume playback");
        }
    }
    else if (lowerType == "volset")
    {
        if (hasValue && value.length() > 0)
        {
            int volume = value.toInt();
            if (volume >= AudioController::MIN_VOLUME && volume <= AudioController::MAX_VOLUME)
            {
                ESP_LOGI(TAG, "Setting volume to: %d", volume);
                if (audio.setVolume(volume))
                {
                    ESP_LOGI(TAG, "Volume set to %d successfully", audio.getCurrentVolume());
                }
                else
                {
                    ESP_LOGW(TAG, "Failed to set volume");
                }
            }
            else
            {
                ESP_LOGW(TAG, "Invalid volume value: %d (must be %d-%d)",
                              volume, AudioController::MIN_VOLUME, AudioController::MAX_VOLUME);
            }
        }
        else
        {
            ESP_LOGI(TAG, "SetVolume command missing value");
        }
    }
    else if(lowerType == "set-max-volume")
    {
        if (hasValue && value.length() > 0)
        {
            int maxVolume = value.toInt();
            if (maxVolume >= AudioController::MIN_VOLUME && maxVolume <= AudioController::MAX_VOLUME)
            {
                ESP_LOGI(TAG, "Setting max volume to: %d", maxVolume);
                audio.setVolumeCeiling(maxVolume);
                ESP_LOGI(TAG, "Max volume set to %d successfully", maxVolume);
            }
            else
            {
                ESP_LOGW(TAG, "Invalid max volume value: %d (must be %d-%d)",
                              maxVolume, AudioController::MIN_VOLUME, AudioController::MAX_VOLUME);
            }
        }
        else
        {
            ESP_LOGI(TAG, "Set-Max-Volume command missing value");
        }
    }
    else if (lowerType == "play-track")
    {
        if (hasValue && value.length() > 0)
        {
            ESP_LOGI(TAG, "Playing track: %s", value.c_str());
            if (audio.playTrack(value))
            {
                ESP_LOGI(TAG, "Track playback started successfully");
            }
            else
            {
                ESP_LOGW(TAG, "Failed to start track playback");
            }
        }
        else
        {
            ESP_LOGI(TAG, "Play command missing value");
        }
    }
    else if (lowerType == "seek-in-track")
    {
        if (hasValue && value.length() > 0)
        {
            int seconds = value.toInt();
            ESP_LOGI(TAG, "Seeking to seconds: %d", seconds);
            audio.seekTo(seconds);
        }
        else
        {
            ESP_LOGI(TAG, "Seek command missing value");
        }
    }
    else if (lowerType == "ota-update")
    {
        uint32_t releaseId = (hasValue && value.length() > 0) ? (uint32_t)value.toInt() : 0;
        ESP_LOGI(TAG, "OTA update requested (release %u)", (unsigned)releaseId);
        OtaManager::getInstance().checkNow(releaseId, "command");
    }
    else if (lowerType == "list-files")
    {
        ESP_LOGI(TAG, "list-files");
        RequestManager::getInstance().requestInventory();
    }
    else if (lowerType == "delete-figure")
    {
        if (!hasValue || value.isEmpty())
        {
            ESP_LOGI(TAG, "delete-figure needs a figure uid");
        }
        else
        {
            RequestManager::getInstance().forgetFigure(value);
            RequestManager::getInstance().requestInventory();
        }
    }
    else if (lowerType == "delete-track")
    {
        if (!hasValue || !value.startsWith("/assets/"))
        {
            ESP_LOGW(TAG, "delete-track refused - only files under /assets/");
        }
        else
        {
            RequestManager::getInstance().forgetTrack(value);
            RequestManager::getInstance().requestInventory();
        }
    }
    else if (lowerType == "sync-figure")
    {
        if (!hasValue || value.isEmpty())
        {
            ESP_LOGI(TAG, "sync-figure needs a figure uid");
        }
        else
        {
            RequestManager::getInstance().refreshUnit(value);
        }
    }
    else if (lowerType == "set-report-interval")
    {
        setReportInterval(hasValue ? (uint32_t)value.toInt() : 5);
    }
    else if (lowerType == "reboot" || lowerType == "restart")
    {
        ESP_LOGI(TAG, "Executing reboot command");
        // Add a small delay to allow the response to be sent
        delay(1000);
        ESP.restart();
    }
    else
    {
        ESP_LOGW(TAG, "Unknown command type: %s", type.c_str());
    }
}

void ReverbClient::setReportInterval(uint32_t seconds)
{
    if (seconds < 2) seconds = 2;
    if (seconds > 60) seconds = 60;
    _fastReportMs = seconds * 1000UL;
    _fastReportUntil = millis() + 120000UL;
    if (_fastReportUntil == 0) _fastReportUntil = 1;
    _reportDue = true;
    ESP_LOGI(TAG, "reports every %u s for the next 2 minutes", (unsigned)seconds);
}

void ReverbClient::queueDeviceFiles(const String &innerJson)
{
    if (!_outbox)
    {
        _outbox = xQueueCreate(24, sizeof(char *)); // a large figure is several parts
        if (!_outbox) return;
    }
    const size_t len = innerJson.length() + 1;
    char *copy = static_cast<char *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) copy = static_cast<char *>(malloc(len));
    if (!copy) return;
    memcpy(copy, innerJson.c_str(), len);
    // The maintenance task may wait for the loop to send (one frame per loop pass) while the
    // channel is up; with the channel down nothing drains, so it does not wait at all.
    const bool live = instance && instance->_isPrivateSubscribed;
    if (xQueueSend(_outbox, &copy, live ? pdMS_TO_TICKS(1000) : 0) != pdTRUE)
    {
        free(copy);
        ESP_LOGW(TAG, "outbox full, inventory part dropped");
    }
}

void ReverbClient::drainOutbox()
{
    if (!_outbox || !isConnected())
    {
        return;
    }
    char *item = nullptr;
    if (xQueueReceive(_outbox, &item, 0) != pdTRUE || !item)
    {
        return;
    }
    String frame;
    frame.reserve(strlen(item) + 160);
    frame += "{\"event\":\"client-device-files\",\"channel\":\"private-device.";
    frame += _deviceId;
    frame += "\",\"data\":{\"device_files\":{\"device_id\":\"";
    frame += _deviceId;
    frame += "\",";
    frame += item;
    frame += "}}}";
    free(item);
    if (!_ws->sendTXT((uint8_t *)frame.c_str(), frame.length()))
    {
        ESP_LOGW(TAG, "failed to send device files event");
    }
}
