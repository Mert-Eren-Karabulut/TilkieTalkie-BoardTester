#include "WiFiProvisioning.h"
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_event.h>

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

void WiFiProvisioningManager::begin() {
    // Ensure default event loop is created (required for provisioning manager)
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        Serial.printf("Failed to create default event loop: %s\n", esp_err_to_name(ret));
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
            Serial.printf("WiFiProvisioning: cannot register WIFI_PROV_EVENT handler: %s\n", esp_err_to_name(er));
        }
    }
    
    // Initialize the wifi provisioning manager
    wifi_prov_mgr_config_t prov_config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM // Auto-free BLE after provisioning
    };
    
    ret = wifi_prov_mgr_init(prov_config);
    if (ret != ESP_OK) {
        Serial.printf("Failed to initialize provisioning manager: %s\n", esp_err_to_name(ret));
        return;
    }
    
    provisioningManagerInitialized = true;
    Serial.println("WiFi Provisioning Manager initialized successfully");
    Serial.printf("Free heap after init: %d bytes\n", ESP.getFreeHeap());
    
    // Check if device is already provisioned using the library's method
    bool provisioned = false;
    ret = wifi_prov_mgr_is_provisioned(&provisioned);
    
    if (ret != ESP_OK) {
        Serial.printf("Error checking provisioning status: %s\n", esp_err_to_name(ret));
        provisioned = false;
    }
    
    Serial.printf("Provisioning status check: %s\n", provisioned ? "PROVISIONED" : "NOT PROVISIONED");
    
    if (provisioned) {
        Serial.println("Device is already provisioned - attempting WiFi connection...");
        // BLE is only needed to provision, and re-provisioning goes through reset() + restart,
        // so on a provisioned boot the controller's memory goes back to the scarce internal heap.
        {
            int before = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            esp_err_t rel = esp_bt_mem_release(ESP_BT_MODE_BTDM);
            Serial.printf("BT controller memory released: %s (%+d bytes internal)\n", esp_err_to_name(rel),
                          (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - before);
        }
        
        // First, try to get stored credentials from the provisioning manager
        wifi_config_t wifi_cfg;
        ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
        if (ret == ESP_OK) {
            String storedSSID = String((char*)wifi_cfg.sta.ssid);
            Serial.printf("Found stored SSID: %s\n", storedSSID.c_str());
            
            // Sync these credentials with our ConfigManager for consistency
            String storedPassword = String((char*)wifi_cfg.sta.password);
            if (storedSSID.length() > 0) {
                config.setWiFiCredentials(storedSSID, storedPassword);
                config.commit();
                Serial.println("Synced WiFi credentials with ConfigManager");
            }
        }
        
        // BLE is automatically disabled by the library when device is provisioned
        // Connect in the background: the loop (serviceNetworkStart) starts the API client and
        // Reverb once there is an IP, and handleBackgroundReconnection() retries. Nothing waits
        // here any more - a wake-up must light the LEDs and play a cached story in seconds.
        WiFi.begin();
        Serial.println("Connecting to WiFi in the background...");
        // The manager is only needed to provision; release it (and its memory) right away.
        wifi_prov_mgr_deinit();
        provisioningManagerInitialized = false;
    } else {
        // Device not provisioned - start provisioning
        Serial.println("Device not provisioned - starting BLE provisioning...");
        startProvisioning();
    }
}

void WiFiProvisioningManager::startProvisioning() {
    if (!provisioningManagerInitialized) {
        Serial.println("Provisioning manager not initialized");
        return;
    }
    
    Serial.println("Starting WiFi provisioning via BLE...");
    Serial.printf("Free heap before provisioning: %d bytes\n", ESP.getFreeHeap());
    
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
        Serial.printf("Failed to start provisioning: %s\n", esp_err_to_name(ret));
        return;
    }
    
    Serial.println("BLE provisioning started successfully!");
    Serial.println("Device is now discoverable as: " + deviceName);
    printQRCode();
}

void WiFiProvisioningManager::printQRCode() {
    if (!provisioningManagerInitialized) {
        Serial.println("Cannot print QR code - provisioning manager not active");
        return;
    }
    
    String deviceName = config.getDeviceName();
    String pin = config.getProvisioningPin();
    // WiFiProv.printQR(deviceName.c_str(), pin.c_str(), "ble");
}

void WiFiProvisioningManager::handleCommand(const String& command) {
    if (command == "qr") {
        if (isConnected()) {
            Serial.println("\nWiFi is already connected. QR code is only needed for provisioning.");
            Serial.println("Use 'reset' command to clear WiFi settings and start provisioning.");
        } else if (provisioningManagerInitialized) {
            Serial.println("\n--- QR Code ---");
            printQRCode();
        } else {
            Serial.println("Provisioning not active. Use 'reset' to start provisioning.");
        }
    }
    else if (command == "reset") {
        Serial.println("\nResetting WiFi provisioning...");
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
    Serial.println("\n--- WiFi Provisioning Status ---");
    Serial.printf("WiFi Connected: %s\n", isConnected() ? "Yes" : "No");
    
    if (isConnected()) {
        Serial.print("SSID: ");
        Serial.println(WiFi.SSID());
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
        Serial.print("Signal Strength: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
    }
    
    Serial.printf("Device Provisioned: %s\n", isProvisioned() ? "Yes" : "No");
    Serial.printf("Provisioning Manager Active: %s\n", provisioningManagerInitialized ? "Yes" : "No");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    
    Serial.println("--- Library State Management ---");
    Serial.println("• BLE automatically managed by ESP32 provisioning library");
    Serial.println("• BLE auto-enabled when provisioning starts");
    Serial.println("• BLE auto-disabled after successful provisioning");
    Serial.println("• Use 'reset' command to clear credentials and reprovision");
    Serial.println("• Manager auto-deinitialized after connection to save memory");
}

void WiFiProvisioningManager::reset() {
    Serial.println("=== WiFi Provisioning Reset ===");
    Serial.println("Clearing WiFi credentials and resetting provisioning state...");
    
    // Stop and deinitialize manager if active
    if (provisioningManagerInitialized) {
        Serial.println("Stopping active provisioning...");
        wifi_prov_mgr_stop_provisioning();
        wifi_prov_mgr_deinit();
        provisioningManagerInitialized = false;
    }
    
    // Clear WiFi credentials using both methods for thoroughness
    Serial.println("Clearing WiFi provisioning data...");
    esp_err_t ret = wifi_prov_mgr_reset_provisioning();
    if (ret != ESP_OK) {
        Serial.printf("Warning: Failed to reset provisioning: %s\n", esp_err_to_name(ret));
    } else {
        Serial.println("✅ Provisioning data cleared");
    }
    
    // Also clear WiFi configuration directly
    Serial.println("Clearing WiFi configuration...");
    wifi_config_t wifi_config = {};
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    
    // Clear from our config for consistency
    Serial.println("Clearing ConfigManager WiFi data...");
    config.clearWiFiCredentials();
    config.commit();
    
    // Disconnect WiFi
    Serial.println("Disconnecting WiFi...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    
    Serial.println("All WiFi data cleared. Device will restart.");
    Serial.println("After restart, BLE will be auto-enabled for new provisioning.");
    delay(2000);
    Serial.println("Restarting device...");
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
        Serial.printf("🏁 Provisioning ended, manager released (free heap %u)\n", ESP.getFreeHeap());
        if (eventCb) eventCb(ProvEvent::ENDED);
    }
    // Only attempt reconnection if we're provisioned but not connected
    if (isConnected() || provisioningManagerInitialized || !isProvisioned()) {
        return;
    }
    
    // Retry without blocking: every 10 s until the first connection of this boot, then every
    // 30 s. The WiFi driver reports the result through the events; nothing is waited for here.
    static bool connectedOnce = false;
    unsigned long currentTime = millis();
    const unsigned long interval = connectedOnce ? 30000UL : 10000UL;
    if (currentTime - lastReconnectAttempt >= interval) {
        lastReconnectAttempt = currentTime;
        Serial.println("Attempting WiFi reconnection with stored credentials (background)...");
        WiFi.begin(); // Use stored credentials
    }
    if (WiFi.isConnected()) {
        connectedOnce = true;
    }
}

void WiFiProvisioningManager::handleProvisioningEvent(arduino_event_t *sys_event) {
    switch (sys_event->event_id) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        {
            Serial.print("\n✅ WiFi Connected! IP address: ");
            Serial.println(IPAddress(sys_event->event_info.got_ip.ip_info.ip.addr));
            
            // Get and store credentials immediately after successful connection
            Serial.println("Retrieving and storing WiFi credentials...");
            wifi_config_t wifi_cfg;
            esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
            if (ret == ESP_OK) {
                String connectedSSID = String((char*)wifi_cfg.sta.ssid);
                String connectedPassword = String((char*)wifi_cfg.sta.password);
                
                Serial.printf("Connected SSID: %s\n", connectedSSID.c_str());
                Serial.printf("Password length: %d characters\n", connectedPassword.length());
                
                // Store in our config for consistency and backup
                config.setWiFiCredentials(connectedSSID, connectedPassword);
                config.commit();
                Serial.println("✅ WiFi credentials stored in ConfigManager");
            } else {
                Serial.printf("❌ Failed to retrieve WiFi config: %s\n", esp_err_to_name(ret));
                // Fallback to current WiFi connection info
                config.storeCurrentWiFiCredentials();
                config.commit();
                Serial.println("✅ Stored current WiFi connection info as fallback");
            }
            
            Serial.printf("Free heap after WiFi connection: %d bytes\n", ESP.getFreeHeap());
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("\n⚠️ WiFi disconnected. Will attempt to reconnect...");
            break;

        case ARDUINO_EVENT_PROV_START:
            Serial.println("\n🔵 Provisioning started");
            Serial.println("📱 Use the ESP BLE Provisioning app to connect");
            Serial.printf("📶 Device name: %s\n", config.getDeviceName().c_str());
            Serial.printf("🔐 PIN: %s\n", config.getProvisioningPin().c_str());
            break;

        case ARDUINO_EVENT_PROV_CRED_RECV:
        {
            Serial.println("\n📨 WiFi credentials received via provisioning");
            String ssid = (const char *)sys_event->event_info.prov_cred_recv.ssid;
            String password = (const char *)sys_event->event_info.prov_cred_recv.password;
            Serial.printf("📡 SSID: %s\n", ssid.c_str());
            Serial.printf("🔑 Password: %s\n", String(password.length() > 0 ? "***" : "(none)").c_str());
            
            // Store in our config immediately (as backup)
            config.setWiFiCredentials(ssid, password);
            config.commit();
            Serial.println("✅ Credentials backed up to ConfigManager");
            if (eventCb) eventCb(ProvEvent::CRED_RECV);
            break;
        }

        case ARDUINO_EVENT_PROV_CRED_FAIL:
            Serial.println("\n❌ Provisioning failed - invalid credentials");
            Serial.println("Please check WiFi network name and password");
            if (eventCb) eventCb(ProvEvent::CRED_FAIL);
            break;

        case ARDUINO_EVENT_PROV_CRED_SUCCESS:
            Serial.println("\n✅ Provisioning successful - credentials accepted");
            Serial.println("Device will now connect to WiFi...");
            if (eventCb) eventCb(ProvEvent::CRED_SUCCESS);
            break;

        case ARDUINO_EVENT_PROV_END:
            Serial.println("\n🏁 Provisioning ended");
            
            // The library automatically handles BLE cleanup and manager deinitialization
            // We just need to update our local state
            if (provisioningManagerInitialized) {
                provisioningManagerInitialized = false;
                Serial.println("📱 BLE automatically disabled by library");
                Serial.println("💾 Provisioning manager auto-deinitialized to save memory");
                Serial.printf("🆓 Free heap after cleanup: %d bytes\n", ESP.getFreeHeap());
                Serial.println("🌐 System ready for HTTPS/WebSocket connections");
            }
            if (eventCb) eventCb(ProvEvent::ENDED);
            break;
            
        default:
            break;
    }
}

void WiFiProvisioningManager::provEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    WiFiProvisioningManager *self = static_cast<WiFiProvisioningManager *>(arg);
    if (!self || base != WIFI_PROV_EVENT) {
        return;
    }
    switch (id) {
        case WIFI_PROV_START:
            Serial.println("🔵 Provisioning started (IDF event)");
            break;
        case WIFI_PROV_CRED_RECV: {
            const wifi_sta_config_t *cfg = static_cast<const wifi_sta_config_t *>(data);
            Serial.printf("📨 Credentials received for '%s'\n", cfg ? (const char *)cfg->ssid : "?");
            if (self->eventCb) self->eventCb(ProvEvent::CRED_RECV);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            const wifi_prov_sta_fail_reason_t *reason = static_cast<const wifi_prov_sta_fail_reason_t *>(data);
            Serial.printf("❌ Provisioning failed: %s\n",
                          (reason && *reason == WIFI_PROV_STA_AUTH_ERROR) ? "wrong password" : "network not found");
            if (self->eventCb) self->eventCb(ProvEvent::CRED_FAIL);
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            Serial.println("✅ Provisioning: credentials accepted, connected");
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
