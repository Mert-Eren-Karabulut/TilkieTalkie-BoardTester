#include "WiFiProvisioning.h"
#include "Faults.h"
#include "OtaManager.h"
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_event.h>
#include <esp_log.h>

namespace {
const char *TAG = "wifi";
const unsigned long CONNECT_GRACE_MS = 10000; // a WiFi.begin() younger than this is still connecting
const unsigned long DHCP_GRACE_MS = 30000;    // associated without an IP for longer: treat as failed
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
    String pin = config.getProvisioningPin();

    // Start provisioning with the library
    esp_err_t ret = wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_1,    // Use security
        pin.c_str(),             // Proof of possession
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
        return;
    }
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
    const unsigned long interval = homeNetworkVisible ? (connectedOnce ? 30000UL : 10000UL) : 120000UL;
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
    if (homeNetworkVisible && reconnectFailures >= 8 && currentTime - offlineSince > 15UL * 60UL * 1000UL &&
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
    if (countFailure) {
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
        WiFi.begin(ssid.c_str(), pass.c_str());
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
            ESP_LOGW(TAG, "WiFi disconnected. Will attempt to reconnect...");
            break;

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
            ESP_LOGI(TAG, "Credentials received for '%s'", cfg ? (const char *)cfg->ssid : "?");
            if (self->eventCb) self->eventCb(ProvEvent::CRED_RECV);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            const wifi_prov_sta_fail_reason_t *reason = static_cast<const wifi_prov_sta_fail_reason_t *>(data);
            ESP_LOGW(TAG, "Provisioning failed: %s",
                     (reason && *reason == WIFI_PROV_STA_AUTH_ERROR) ? "wrong password" : "network not found");
            if (self->eventCb) self->eventCb(ProvEvent::CRED_FAIL);
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning: credentials accepted, connected");
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
