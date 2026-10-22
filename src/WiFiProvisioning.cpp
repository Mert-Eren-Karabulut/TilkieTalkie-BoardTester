#include "WiFiProvisioning.h"
#include "Faults.h"
#include "OtaManager.h"
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_event.h>
#include <esp_log.h>
#include <WiFiClient.h>
#include <algorithm>
#include <ctype.h>

namespace {
const char *TAG = "wifi";
const unsigned long CONNECT_GRACE_MS = 10000; // a WiFi.begin() younger than this is still connecting
const unsigned long DHCP_GRACE_MS = 30000;    // associated without an IP for longer: treat as failed
const unsigned long CAPTIVE_REPROBE_MS = 120000;
const unsigned long REJECTED_RETRY_MS = 30UL * 60UL * 1000UL; // stored password rejected: one silent try per 30 min
const char *PROBE_PATH = "/storage/ping.json";
const char *PROBE_BODY = "{\"ping\":\"tilkie\"}";

// ---- Portal probe (docs/PROTOCOL-V2.md §6) -------------------------------------------------
// One probe at a time, on its own short-lived task: the loop never waits for it, and during
// provisioning the event-loop handler below waits on the semaphore without needing stack.
struct ProbeState {
    std::atomic<bool> busy{false};
    std::atomic<int8_t> result{-1};   // -1 running/none, else a ProbeResult
    char host[64] = {};
    uint16_t port = 80;
    char why[40] = {};
    SemaphoreHandle_t done = nullptr;
};
ProbeState g_probe;

// Three outcomes: our body (online); an answer that is not ours (a 3xx, HTML, any other body:
// someone on this network intercepts HTTP = captive portal); no answer at all (DNS failure,
// connect timeout, reset, silence for 5 s = the link has no internet right now).
enum ProbeResult : int8_t { PROBE_CAPTIVE = 0, PROBE_OK = 1, PROBE_NO_INTERNET = 2 };

ProbeResult runProbe(const char *host, uint16_t port, char *why, size_t whyLen)
{
    WiFiClient c;
    if (!c.connect(host, port, 5000)) {
        snprintf(why, whyLen, "connect failed");
        return PROBE_NO_INTERNET;
    }
    char req[200];
    if (port == 80) {
        snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: TilkieTalkie/1.0\r\nConnection: close\r\n\r\n", PROBE_PATH, host);
    } else {
        snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: TilkieTalkie/1.0\r\nConnection: close\r\n\r\n", PROBE_PATH, host, (unsigned)port);
    }
    c.print(req);
    // A real answer is ~250 bytes; a login page is bigger and fails the body check anyway.
    char buf[768];
    size_t n = 0;
    const unsigned long t0 = millis();
    while (millis() - t0 < 5000 && n < sizeof(buf) - 1) {
        int avail = c.available();
        if (avail > 0) {
            int got = c.read(reinterpret_cast<uint8_t *>(buf) + n, std::min((size_t)avail, sizeof(buf) - 1 - n));
            if (got > 0) n += got;
        } else if (!c.connected()) {
            break;
        } else {
            delay(10);
        }
    }
    c.stop();
    buf[n] = '\0';
    if (n == 0) {
        snprintf(why, whyLen, "no answer");
        return PROBE_NO_INTERNET;
    }
    // Something answered. Status line: anything but 200 (a 30x to a login page included;
    // redirects are never followed) is not ours.
    int status = 0;
    if (n > 12 && strncmp(buf, "HTTP/1.", 7) == 0) {
        status = atoi(buf + 9);
    }
    if (status != 200) {
        snprintf(why, whyLen, status ? "http=%d" : "not http", status);
        return PROBE_CAPTIVE;
    }
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) {
        snprintf(why, whyLen, "no body");
        return PROBE_CAPTIVE;
    }
    body += 4;
    while (*body && isspace((unsigned char)*body)) body++;
    char *end = body + strlen(body);
    while (end > body && isspace((unsigned char)end[-1])) *--end = '\0';
    if (strcmp(body, PROBE_BODY) != 0) {
        snprintf(why, whyLen, "body mismatch (%u bytes)", (unsigned)strlen(body));
        return PROBE_CAPTIVE;
    }
    snprintf(why, whyLen, "ok");
    return PROBE_OK;
}

void probeTask(void *)
{
    char why[40];
    const ProbeResult r = runProbe(g_probe.host, g_probe.port, why, sizeof(why));
    strlcpy(g_probe.why, why, sizeof(g_probe.why));
    g_probe.result = r;
    g_probe.busy = false;
    xSemaphoreGive(g_probe.done);
    vTaskDelete(nullptr);
}

bool startProbe(const String &host, uint16_t port)
{
    if (!g_probe.done) {
        g_probe.done = xSemaphoreCreateBinary();
        if (!g_probe.done) return false;
    }
    if (g_probe.busy.exchange(true)) {
        return false;
    }
    xSemaphoreTake(g_probe.done, 0); // drop a result nobody waited for
    strlcpy(g_probe.host, host.c_str(), sizeof(g_probe.host));
    g_probe.port = port;
    g_probe.why[0] = '\0';
    g_probe.result = -1;
    if (xTaskCreatePinnedToCore(probeTask, "probe", 6144, nullptr, 3, nullptr, 0) != pdPASS) {
        g_probe.busy = false;
        return false;
    }
    return true;
}

// Disconnect reasons that mean "this network does not accept our password".
bool isAuthReason(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_MIC_FAILURE;
}
}

WiFiProvisioningManager* WiFiProvisioningManager::instance = nullptr;

WiFiProvisioningManager::WiFiProvisioningManager() : config(ConfigManager::getInstance()), provisioningManagerInitialized(false), lastReconnectAttempt(0) {
}

WiFiProvisioningManager& WiFiProvisioningManager::getInstance() {
    if (instance == nullptr) {
        instance = new WiFiProvisioningManager();
    }
    return *instance;
}

void WiFiProvisioningManager::onSysProvEvent(arduino_event_t *sys_event) {
    WiFiProvisioningManager::getInstance().handleProvisioningEvent(sys_event);
}

void WiFiProvisioningManager::storeCredentialsIfChanged(const String &ssid, const String &password) {
    if (ssid.isEmpty()) {
        return;
    }
    if (ssid == config.getWiFiSSID() && password == config.getWiFiPassword()) {
        return; // unchanged: no NVS write (this runs on every connect)
    }
    config.setWiFiCredentials(ssid, password);
    ESP_LOGI(TAG, "WiFi credentials for '%s' stored in ConfigManager", ssid.c_str());
}

void WiFiProvisioningManager::begin() {
    // Ensure default event loop is created (required for provisioning manager)
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize WiFi first - required before provisioning manager
    WiFi.mode(WIFI_STA);

    // Small delay to ensure WiFi is properly initialized
    delay(100);

    WiFi.onEvent(onSysProvEvent);
    if (!provEventsRegistered) {
        esp_err_t er = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &provEventHandler, this);
        provEventsRegistered = (er == ESP_OK);
        if (er != ESP_OK) {
            ESP_LOGE(TAG, "cannot register WIFI_PROV_EVENT handler: %s", esp_err_to_name(er));
        }
        // Registered before wifi_prov_mgr_start_provisioning() registers the manager's own
        // GOT_IP handler: handlers of one event run in registration order, so the captive
        // probe below decides before the manager declares the provisioning a success.
        er = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &gotIpPreHandler, this);
        if (er != ESP_OK) {
            ESP_LOGE(TAG, "cannot register the GOT_IP probe handler: %s", esp_err_to_name(er));
        }
    }

    // Initialize the wifi provisioning manager
    wifi_prov_mgr_config_t prov_config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM // Auto-free BLE after provisioning
    };

    ret = wifi_prov_mgr_init(prov_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize provisioning manager: %s", esp_err_to_name(ret));
        return;
    }

    provisioningManagerInitialized = true;
    ESP_LOGI(TAG, "WiFi Provisioning Manager initialized successfully");
    ESP_LOGI(TAG, "Free heap after init: %u bytes", (unsigned)ESP.getFreeHeap());

    // Check if device is already provisioned using the library's method
    bool provisioned = false;
    ret = wifi_prov_mgr_is_provisioned(&provisioned);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Error checking provisioning status: %s", esp_err_to_name(ret));
        provisioned = false;
    }

    ESP_LOGI(TAG, "Provisioning status check: %s", provisioned ? "PROVISIONED" : "NOT PROVISIONED");

    if (provisioned) {
        ESP_LOGI(TAG, "Device is already provisioned - attempting WiFi connection...");
        // BLE is only needed to provision, and re-provisioning goes through reset() + restart,
        // so on a provisioned boot the controller's memory goes back to the scarce internal heap.
        {
            int before = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            esp_err_t rel = esp_bt_mem_release(ESP_BT_MODE_BTDM);
            ESP_LOGI(TAG, "BT controller memory released: %s (%+d bytes internal)", esp_err_to_name(rel),
                     (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - before);
        }

        // The driver's stored credentials are the reference; ConfigManager keeps a copy
        // (written only when it differs).
        wifi_config_t wifi_cfg;
        ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
        if (ret == ESP_OK) {
            String storedSSID = String((char*)wifi_cfg.sta.ssid);
            ESP_LOGI(TAG, "Found stored SSID: %s", storedSSID.c_str());
            storeCredentialsIfChanged(storedSSID, String((char*)wifi_cfg.sta.password));
        }

        // Connect in the background: the loop (serviceNetworkStart) starts the API client and
        // Reverb once there is an IP, and handleBackgroundReconnection() retries. Nothing waits
        // here any more - a wake-up must light the LEDs and play a cached story in seconds.
        WiFi.begin();
        lastBeginAt = millis();
        ESP_LOGI(TAG, "Connecting to WiFi in the background...");
        // The manager is only needed to provision; release it (and its memory) right away.
        wifi_prov_mgr_deinit();
        provisioningManagerInitialized = false;
    } else {
        // Device not provisioned - start provisioning
        ESP_LOGI(TAG, "Device not provisioned - starting BLE provisioning...");
        startProvisioning();
    }
}

void WiFiProvisioningManager::startProvisioning() {
    if (!provisioningManagerInitialized) {
        ESP_LOGW(TAG, "Provisioning manager not initialized");
        return;
    }

    ESP_LOGI(TAG, "Starting WiFi provisioning via BLE...");
    ESP_LOGI(TAG, "Free heap before provisioning: %u bytes", (unsigned)ESP.getFreeHeap());

    String deviceName = config.getDeviceName();
    const String pop = proofOfPossession();
    if (pop.isEmpty()) {
        // Without a per-unit pop anyone in BLE range could provision the toy with the PIN that
        // every unit shared: no provisioning at all (NOT_ENROLLED was raised at boot).
        ESP_LOGE(TAG, "no factory pop enrolled - BLE provisioning disabled (ft enroll)");
        noProofOfPossession = true;
        wifi_prov_mgr_deinit();
        provisioningManagerInitialized = false;
        return;
    }

    // The manager would stop itself 30 s after the first IP. A network that turns out to be a
    // captive portal must leave provisioning running, so the loop stops it after a success
    // that passed the probe (provStopAt) instead.
    wifi_prov_mgr_disable_auto_stop(1000);

    // Start provisioning with the library
    esp_err_t ret = wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_1,    // Use security
        pop.c_str(),             // Proof of possession (never logged)
        deviceName.c_str(),      // Device name (BLE advertisement name)
        nullptr                  // Service key (not used for BLE)
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "BLE provisioning started successfully!");
    ESP_LOGI(TAG, "Device is now discoverable as: %s", deviceName.c_str());
    printQRCode();
}

String WiFiProvisioningManager::proofOfPossession() {
    String pop = config.factoryPop();
    if (pop.length() > 0) {
        return pop;
    }
#ifdef TT_LEGACY_POP
    // Un-enrolled fleet (docs/PROTOCOL-V2.md §1): the old shared PIN until every unit has a pop.
    ESP_LOGW(TAG, "no factory pop enrolled - using the legacy provisioning PIN (TT_LEGACY_POP)");
    return config.getProvisioningPin();
#else
    return String();
#endif
}

void WiFiProvisioningManager::printQRCode() {
    if (!provisioningManagerInitialized) {
        ESP_LOGI(TAG, "Cannot print QR code - provisioning manager not active");
        return;
    }
    // No QR output on this device (the app finds it over BLE by name).
}

void WiFiProvisioningManager::handleCommand(const String& command) {
    if (command == "qr") {
        if (isConnected()) {
            ESP_LOGI(TAG, "WiFi is already connected. QR code is only needed for provisioning.");
            ESP_LOGI(TAG, "Use 'reset' command to clear WiFi settings and start provisioning.");
        } else if (provisioningManagerInitialized) {
            ESP_LOGI(TAG, "--- QR Code ---");
            printQRCode();
        } else {
            ESP_LOGI(TAG, "Provisioning not active. Use 'reset' to start provisioning.");
        }
    }
    else if (command == "reset") {
        ESP_LOGI(TAG, "Resetting WiFi provisioning...");
        reset();
    }
    else if (command == "stats") {
        printStatus();
    }
}

bool WiFiProvisioningManager::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool WiFiProvisioningManager::isProvisioned() {
    if (!provisioningManagerInitialized) {
        // If manager is deinitialized but we have WiFi, we're provisioned
        return WiFi.isConnected() || config.hasWiFiCredentials();
    }

    bool provisioned = false;
    esp_err_t ret = wifi_prov_mgr_is_provisioned(&provisioned);
    if (ret != ESP_OK) {
        return false;
    }
    return provisioned;
}

bool WiFiProvisioningManager::isProvisioningManagerActive() {
    return provisioningManagerInitialized;
}

void WiFiProvisioningManager::printStatus() {
    ESP_LOGI(TAG, "--- WiFi Provisioning Status ---");
    ESP_LOGI(TAG, "WiFi Connected: %s", isConnected() ? "Yes" : "No");

    if (isConnected()) {
        ESP_LOGI(TAG, "SSID: %s", WiFi.SSID().c_str());
        ESP_LOGI(TAG, "IP Address: %s", WiFi.localIP().toString().c_str());
        ESP_LOGI(TAG, "Signal Strength: %d dBm", (int)WiFi.RSSI());
    }

    ESP_LOGI(TAG, "Device Provisioned: %s", isProvisioned() ? "Yes" : "No");
    ESP_LOGI(TAG, "Provisioning Manager Active: %s", provisioningManagerInitialized ? "Yes" : "No");
    ESP_LOGI(TAG, "Home network visible: %s, reconnect failures %d, offline for %lu s",
             homeNetworkVisible ? "yes" : "no", reconnectFailures,
             offlineSince ? (millis() - offlineSince) / 1000 : 0UL);
    ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned)ESP.getFreeHeap());
}

void WiFiProvisioningManager::reset() {
    ESP_LOGW(TAG, "=== WiFi Provisioning Reset ===");
    ESP_LOGI(TAG, "Clearing WiFi credentials and resetting provisioning state...");

    // Stop and deinitialize manager if active
    if (provisioningManagerInitialized) {
        ESP_LOGI(TAG, "Stopping active provisioning...");
        wifi_prov_mgr_stop_provisioning();
        wifi_prov_mgr_deinit();
        provisioningManagerInitialized = false;
    }

    // Clear WiFi credentials using both methods for thoroughness
    ESP_LOGI(TAG, "Clearing WiFi provisioning data...");
    esp_err_t ret = wifi_prov_mgr_reset_provisioning();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Warning: Failed to reset provisioning: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Provisioning data cleared");
    }

    // Also clear WiFi configuration directly
    ESP_LOGI(TAG, "Clearing WiFi configuration...");
    wifi_config_t wifi_config = {};
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    // Clear from our config for consistency
    ESP_LOGI(TAG, "Clearing ConfigManager WiFi data...");
    config.clearWiFiCredentials();

    // Disconnect WiFi
    ESP_LOGI(TAG, "Disconnecting WiFi...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    ESP_LOGI(TAG, "All WiFi data cleared. Device will restart.");
    ESP_LOGI(TAG, "After restart, BLE will be auto-enabled for new provisioning.");
    delay(2000);
    ESP_LOGI(TAG, "Restarting device...");
    ESP.restart();
}

void WiFiProvisioningManager::handleBackgroundReconnection() {
    if (captiveCleanupPending) {
        // The app has been told "failed" (synthetic auth failure, see gotIpPreHandler). Make the
        // manager accept new credentials, drop the link and forget the captive network.
        captiveCleanupPending = false;
        esp_err_t r = wifi_prov_mgr_reset_sm_state_on_failure(); // also clears the driver's STA config
        esp_wifi_disconnect();
        config.clearWiFiCredentials();
        ESP_LOGW(TAG, "captive network forgotten, provisioning continues (%s)", esp_err_to_name(r));
    }
    if (provStopAt && (long)(millis() - provStopAt) >= 0) {
        provStopAt = 0;
        if (provisioningManagerInitialized) {
            wifi_prov_mgr_stop_provisioning(); // WIFI_PROV_END follows (provEndPending)
        }
    }
    if (provEndPending) {
        // Provisioning finished (the app got its "success"): release the manager and tell the UX.
        provEndPending = false;
        if (provisioningManagerInitialized) {
            wifi_prov_mgr_deinit();
            provisioningManagerInitialized = false;
        }
        ESP_LOGI(TAG, "Provisioning ended, manager released (free heap %u)", (unsigned)ESP.getFreeHeap());
        if (eventCb) eventCb(ProvEvent::ENDED);
    }
    // Only attempt reconnection if we're provisioned but not connected
    if (isConnected()) {
        connectedOnce = true;
        offlineSince = 0;
        reconnectFailures = 0;
        attemptedSinceOffline = false;
        homeNetworkVisible = true;
        serviceProbe();
        return;
    }
    portalOk = false;
    captiveNow = false;
    noInternetNow = false;
    if (provisioningManagerInitialized || !isProvisioned()) {
        return;
    }
    unsigned long currentTime = millis();
    if (!offlineSince) {
        offlineSince = currentTime;
        homeNetworkVisible = true; // until a scan says otherwise
    }
    // Is the home network around at all? A scan for the stored SSID decides. Out of range (the
    // toy is used away from home, playing from the card) nothing is retried every few seconds,
    // nothing is reset or restarted and nothing is said - the next look is in two minutes.
    // In range but not connecting: retry every 10 s (30 s once connected this boot), every 4th
    // real failure restarts the radio (a toy stayed offline 16 min next to a working router until
    // a reboot, 2026-10-15), and after 15 min the toy restarts itself while idle.
    if (scanInFlight) {
        int16_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            return;
        }
        scanInFlight = false;
        if (n >= 0) {
            homeNetworkVisible = n > 0;
            WiFi.scanDelete();
            if (homeNetworkVisible) {
                // In range and still no link after our previous attempt: that one failed.
                attemptReconnect(attemptedSinceOffline);
            } else {
                reconnectFailures = 0;
                ESP_LOGI(TAG, "home network not in range - staying offline quietly");
                Faults::raise(Fault::WIFI_OUT_OF_RANGE);
            }
        } else {
            attemptReconnect(false); // scan failed: just try
        }
        return;
    }
    // The driver may still be connecting: associated and waiting for DHCP (WL_IDLE_STATUS, given
    // DHCP_GRACE_MS so a router whose DHCP never answers does not park us here), or our last
    // begin() is recent. A scan now would abort that attempt, so the next look waits.
    if (WiFi.status() == WL_IDLE_STATUS) {
        if (!idleSeenAt) idleSeenAt = currentTime;
    } else {
        idleSeenAt = 0;
    }
    const bool connecting = (idleSeenAt && currentTime - idleSeenAt < DHCP_GRACE_MS) ||
                            (lastBeginAt && currentTime - lastBeginAt < CONNECT_GRACE_MS);
    unsigned long interval = homeNetworkVisible ? (connectedOnce ? 30000UL : 10000UL) : 120000UL;
    if (credentialsRejected()) {
        // The router no longer takes our password: retrying every 10 s, resetting the radio
        // and restarting the toy every 15 min (hello + "I cannot connect" all night on the dock)
        // changes nothing. One quiet attempt every 30 min until the password works again or
        // the user re-pairs.
        interval = REJECTED_RETRY_MS;
    }
    if (!connecting && currentTime - lastReconnectAttempt >= interval) {
        lastReconnectAttempt = currentTime;
        const String ssid = config.getWiFiSSID();
        int16_t r = ssid.isEmpty() ? WIFI_SCAN_FAILED : WiFi.scanNetworks(true, true, false, 120, 0, ssid.c_str());
        if (r == WIFI_SCAN_RUNNING) {
            scanInFlight = true; // the result decides (above)
        } else {
            attemptReconnect(false); // no SSID stored or the driver is busy: a plain attempt
        }
    }
    if (homeNetworkVisible && !credentialsRejected() && reconnectFailures >= 8 && currentTime - offlineSince > 15UL * 60UL * 1000UL &&
        idleCheck && idleCheck()) {
        ESP_LOGW(TAG, "home network in range but no link for 15 min - restarting while idle");
        delay(200);
        ESP.restart();
    }
}

void WiFiProvisioningManager::beginStored() {
    WiFi.begin(); // Use stored credentials
    lastBeginAt = millis();
    attemptedSinceOffline = true;
}

void WiFiProvisioningManager::attemptReconnect(bool countFailure) {
    if (countFailure && !credentialsRejected()) { // a radio reset does not fix a wrong password
        reconnectFailures++;
        if ((reconnectFailures % 4) == 0 && resetRadio("reconnects keep failing")) {
            return;
        }
    }
    ESP_LOGI(TAG, "Attempting WiFi reconnection with stored credentials (background)... (%d failed so far)", reconnectFailures);
    beginStored();
}

bool WiFiProvisioningManager::resetRadio(const char *why) {
    if (OtaManager::getInstance().inProgress()) {
        // Turning the radio off would cut the image download (or the prompt pack) mid-way.
        ESP_LOGW(TAG, "radio reset (%s) refused - firmware update in progress", why);
        return false;
    }
    ESP_LOGW(TAG, "radio reset (%s)", why);
    Faults::raise(Fault::WIFI_RADIO_RESET, why);
    WiFi.disconnect(true, false); // radio off, credentials kept
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_STA);
    const String ssid = config.getWiFiSSID();
    const String pass = config.getWiFiPassword();
    if (ssid.length() > 0) {
        // An empty password is an open network (no auth threshold).
        WiFi.begin(ssid.c_str(), pass.length() > 0 ? pass.c_str() : nullptr);
    } else {
        WiFi.begin();
    }
    lastReconnectAttempt = millis();
    lastBeginAt = lastReconnectAttempt;
    attemptedSinceOffline = true;
    return true;
}

// WiFi event task: keep it short.
void WiFiProvisioningManager::handleProvisioningEvent(arduino_event_t *sys_event) {
    switch (sys_event->event_id) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        {
            ESP_LOGI(TAG, "WiFi Connected! IP address: %s", IPAddress(sys_event->event_info.got_ip.ip_info.ip.addr).toString().c_str());
            connectionGen++;      // the loop probes the portal for this connection
            authFailStreak = 0;

            // Mirror the credentials that just worked (NVS written only if they changed).
            wifi_config_t wifi_cfg;
            esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
            if (ret == ESP_OK) {
                storeCredentialsIfChanged(String((char*)wifi_cfg.sta.ssid), String((char*)wifi_cfg.sta.password));
            } else {
                ESP_LOGW(TAG, "Failed to retrieve WiFi config: %s", esp_err_to_name(ret));
                // Fallback to current WiFi connection info
                storeCredentialsIfChanged(WiFi.SSID(), WiFi.psk());
            }

            ESP_LOGI(TAG, "Free heap after WiFi connection: %u bytes", (unsigned)ESP.getFreeHeap());
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        {
            const uint8_t reason = sys_event->event_info.wifi_sta_disconnected.reason;
            ESP_LOGW(TAG, "WiFi disconnected (reason %u). Will attempt to reconnect...", (unsigned)reason);
            portalOk = false;
            if (!provisioningManagerInitialized) { // during pairing the app reports a wrong password
                if (isAuthReason(reason)) {
                    const int n = ++authFailStreak;
                    if (n == AUTH_FAIL_LIMIT) {
                        ESP_LOGW(TAG, "stored network rejected the password %d times - quiet retries every 30 min", n);
                    }
                } else if (reason == WIFI_REASON_NO_AP_FOUND) {
                    authFailStreak = 0; // network gone: a different story (out of range)
                }
            }
            break;
        }

        default:
            break;
    }
}

// IDF provisioning events (WiFi event task): keep it short.
void WiFiProvisioningManager::provEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    WiFiProvisioningManager *self = static_cast<WiFiProvisioningManager *>(arg);
    if (!self || base != WIFI_PROV_EVENT) {
        return;
    }
    switch (id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started (IDF event)");
            break;
        case WIFI_PROV_CRED_RECV: {
            const wifi_sta_config_t *cfg = static_cast<const wifi_sta_config_t *>(data);
            ESP_LOGI(TAG, "Credentials received for '%s'%s", cfg ? (const char *)cfg->ssid : "?",
                     (cfg && cfg->password[0] == 0) ? " (open network)" : "");
            self->credRecvSeen = true;
            if (self->eventCb) self->eventCb(ProvEvent::CRED_RECV);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            const wifi_prov_sta_fail_reason_t *reason = static_cast<const wifi_prov_sta_fail_reason_t *>(data);
            if (self->captiveRejectPending) {
                // Our own synthetic failure for a captive network (gotIpPreHandler).
                self->captiveRejectPending = false;
                self->captiveCleanupPending = true;
                ESP_LOGW(TAG, "Provisioning failed: the network is a captive portal");
                if (self->eventCb) self->eventCb(ProvEvent::CAPTIVE);
                break;
            }
            ESP_LOGW(TAG, "Provisioning failed: %s",
                     (reason && *reason == WIFI_PROV_STA_AUTH_ERROR) ? "wrong password" : "network not found");
            if (self->eventCb) self->eventCb(ProvEvent::CRED_FAIL);
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            if (self->captiveRejectPending) {
                break; // the manager's view; the synthetic failure right behind it is what the app gets
            }
            ESP_LOGI(TAG, "Provisioning: credentials accepted, connected, portal reachable");
            // Auto-stop is off (see startProvisioning): give the app the same 30 s to read the result.
            self->provStopAt = (millis() + 30000UL) | 1UL;
            if (self->eventCb) self->eventCb(ProvEvent::CRED_SUCCESS);
            break;
        case WIFI_PROV_END:
            self->provEndPending = true; // deinit from the loop task, not from the event task
            break;
        case WIFI_PROV_DEINIT:
            self->provisioningManagerInitialized = false;
            break;
        default:
            break;
    }
}

// Default event loop, IP_EVENT_STA_GOT_IP, runs before the provisioning manager's handler.
// Only while provisioning, for the connection the app's credentials just produced: the
// manager must not report success for a network that is a captive portal. The probe runs on
// its own task; this handler only waits (up to 12 s, the event loop is otherwise idle while a
// phone provisions). Only on a captive answer (case b; not on "no answer") a synthetic "auth failed" disconnect is posted: the manager
// turns it into the same FAIL/AUTH_ERROR status the app shows for a wrong password.
void WiFiProvisioningManager::gotIpPreHandler(void *arg, esp_event_base_t, int32_t, void *)
{
    WiFiProvisioningManager *self = static_cast<WiFiProvisioningManager *>(arg);
    if (!self || !self->provisioningManagerInitialized || !self->credRecvSeen) {
        return;
    }
    self->credRecvSeen = false;
    if (self->probeHost.isEmpty()) {
        return;
    }
    bool started = false;
    for (int i = 0; i < 60 && !(started = startProbe(self->probeHost, self->probePort)); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100)); // a probe of the loop's is still running
    }
    // Only a positive "not ours" (case b) rejects the network. No answer (case c, or a probe
    // that could not run in time) is "no internet yet": the manager reports success as before,
    // the credentials stay, and the loop re-probes every 2 min.
    int8_t result = PROBE_NO_INTERNET;
    if (started && xSemaphoreTake(g_probe.done, pdMS_TO_TICKS(12000)) == pdTRUE) {
        result = g_probe.result.load();
    }
    ESP_LOGI(TAG, "provisioning: portal probe %s (%s)",
             result == PROBE_OK ? "passed" : result == PROBE_CAPTIVE ? "CAPTIVE" : "no internet yet - accepted",
             started ? g_probe.why : "not started");
    if (result != PROBE_CAPTIVE) {
        return;
    }
    self->captiveRejectPending = true;
    wifi_event_sta_disconnected_t ev = {};
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        memcpy(ev.ssid, cfg.sta.ssid, sizeof(ev.ssid));
        ev.ssid_len = strnlen((const char *)cfg.sta.ssid, sizeof(cfg.sta.ssid));
    }
    ev.reason = WIFI_REASON_AUTH_FAIL;
    if (esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ev, sizeof(ev), 0) != ESP_OK) {
        // Could not tell the manager: fall back to forgetting the network ourselves.
        self->captiveRejectPending = false;
        self->captiveCleanupPending = true;
        if (self->eventCb) self->eventCb(ProvEvent::CAPTIVE);
    }
}

// Loop task, while connected. Every new connection is probed once; a captive one is re-probed
// every 2 minutes. Network services wait for isInternetReady().
void WiFiProvisioningManager::serviceProbe()
{
    if (provisioningManagerInitialized || probeHost.isEmpty()) {
        if (probeHost.isEmpty()) portalOk = true; // nothing to check against
        return;                                   // provisioning probes in gotIpPreHandler
    }
    const unsigned long now = millis();
    if (loopProbeRunning) {
        if (g_probe.busy.load() || g_probe.result.load() < 0) {
            return;
        }
        loopProbeRunning = false;
        const int8_t r = g_probe.result.load();
        portalOk = r == PROBE_OK;
        captiveNow = r == PROBE_CAPTIVE;
        noInternetNow = r == PROBE_NO_INTERNET;
        if (portalOk) {
            ESP_LOGI(TAG, "portal probe passed - online");
            captiveRaisedThisLink = false;
        } else {
            nextProbeAt = now + CAPTIVE_REPROBE_MS;
            ESP_LOGW(TAG, "portal probe: %s (%s) - staying offline, again in 2 min",
                     captiveNow ? "captive portal" : "no internet", g_probe.why);
            if (!captiveRaisedThisLink) {
                captiveRaisedThisLink = true;
                // Provisioned toy: silent either way. No answer is the existing 106.
                Faults::raise(captiveNow ? Fault::CAPTIVE_PORTAL : Fault::PORTAL_UNREACHABLE, g_probe.why, false);
            }
        }
        return;
    }
    const uint32_t gen = connectionGen.load();
    const bool newLink = gen != probedGen;
    if (newLink) {
        portalOk = false;
        captiveNow = false;
        noInternetNow = false;
        captiveRaisedThisLink = false;
    }
    if (newLink || ((captiveNow || noInternetNow) && (long)(now - nextProbeAt) >= 0)) {
        if (startProbe(probeHost, probePort)) {
            probedGen = gen;
            loopProbeRunning = true;
        }
    }
}
