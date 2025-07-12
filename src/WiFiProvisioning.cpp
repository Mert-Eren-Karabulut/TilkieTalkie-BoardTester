#include "WiFiProvisioning.h"

WiFiProvisioningManager* WiFiProvisioningManager::instance = nullptr;

WiFiProvisioningManager::WiFiProvisioningManager() : config(ConfigManager::getInstance()), isProvisioning(false) {
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
    WiFi.onEvent(onSysProvEvent);
    
    // Try to connect with stored credentials from our config
    if (config.hasWiFiCredentials()) {
        Serial.println("Attempting to connect with stored credentials...");
        WiFi.begin(config.getWiFiSSID().c_str(), config.getWiFiPassword().c_str());
        
        // Wait for connection attempt
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            Serial.print(".");
            attempts++;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\nConnected to WiFi!");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            return;
        } else {
            Serial.println("\nConnection failed with stored credentials.");
        }
    }
    
    // Start provisioning if no stored credentials or connection failed
    Serial.println("No valid WiFi credentials found - starting provisioning...");
    startProvisioning();
}

void WiFiProvisioningManager::startProvisioning() {
    Serial.println("Starting WiFi provisioning...");
    isProvisioning = true;
    
    uint8_t uuid[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
                        0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};
    
    String deviceName = config.getDeviceName();
    String pin = config.getProvisioningPin();
    
    WiFiProv.beginProvision(
        WIFI_PROV_SCHEME_BLE, 
        WIFI_PROV_SCHEME_HANDLER_FREE_BLE, 
        WIFI_PROV_SECURITY_1, 
        pin.c_str(), 
        deviceName.c_str(), 
        NULL, 
        uuid, 
        false  // Don't force reset - only reset when explicitly requested
    );
    
    printQRCode();
}

void WiFiProvisioningManager::printQRCode() {
    String deviceName = config.getDeviceName();
    String pin = config.getProvisioningPin();
    WiFiProv.printQR(deviceName.c_str(), pin.c_str(), "ble");
}

void WiFiProvisioningManager::handleCommand(const String& command) {
    if (command == "qr") {
        if (isConnected()) {
            Serial.println("\nWiFi is already connected. QR code is only needed for provisioning.");
            Serial.println("Use 'reset' command to clear WiFi settings and start provisioning.");
        } else {
            Serial.println("\n--- QR Code ---");
            printQRCode();
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

void WiFiProvisioningManager::printStatus() {
    Serial.println("\n--- WiFi Status ---");
    if (isConnected()) {
        Serial.println("Status: Connected");
        Serial.print("SSID: ");
        Serial.println(WiFi.SSID());
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
        Serial.print("Signal Strength: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
    } else {
        Serial.println("Status: Disconnected");
        if (isProvisioning) {
            Serial.println("Provisioning mode active");
        }
    }
}

void WiFiProvisioningManager::reset() {
    Serial.println("=== WiFi Provisioning Reset ===");
    Serial.println("Clearing WiFi credentials and provisioning data...");
    
    // Clear stored WiFi credentials from our config
    esp_wifi_restore();
    config.clearWiFiCredentials();
    config.commit();  // Ensure changes are saved
    
    // Stop current WiFi provisioning if active
    if (isProvisioning) {
        isProvisioning = false;
    }
    
    // Disconnect WiFi and clear all network settings
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    
    // Clear ESP32's internal WiFi credentials BEFORE restart
    Serial.println("Clearing ESP32's internal WiFi credentials...");
    esp_wifi_restore();
    
    // Clear ESP32 provisioning manager's stored data BEFORE restart
    Serial.println("Resetting ESP32 provisioning manager...");
    wifi_prov_mgr_reset_provisioning();
    
    Serial.println("All WiFi data cleared. Device will restart and require new provisioning.");
    delay(2000);  // Give more time for cleanup
    Serial.println("Restarting device...");
    ESP.restart();
}

void WiFiProvisioningManager::handleProvisioningEvent(arduino_event_t *sys_event) {
    switch (sys_event->event_id) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.print("\nConnected IP address : ");
            Serial.println(IPAddress(sys_event->event_info.got_ip.ip_info.ip.addr));
            isProvisioning = false;
            
            // Store current WiFi credentials as fallback
            Serial.println("Storing current WiFi credentials after successful connection...");
            config.storeCurrentWiFiCredentials();
            config.commit();  // Ensure everything is saved
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("\nDisconnected. Connecting to the AP again...");
            break;
        case ARDUINO_EVENT_PROV_START:
            Serial.println("\nProvisioning started\nGive Credentials of your access point using smartphone app");
            break;
        case ARDUINO_EVENT_PROV_CRED_RECV:
            {
                Serial.println("\n=== PROVISIONING CREDENTIALS RECEIVED ===");
                String ssid = (const char *)sys_event->event_info.prov_cred_recv.ssid;
                String password = (const char *)sys_event->event_info.prov_cred_recv.password;
                Serial.print("Received SSID: ");
                Serial.println(ssid);
                Serial.print("Received Password: ");
                Serial.println(password);
                Serial.print("SSID Length: ");
                Serial.println(ssid.length());
                Serial.print("Password Length: ");
                Serial.println(password.length());
                
                // Store credentials in our config
                Serial.println("Calling setWiFiCredentials...");
                config.setWiFiCredentials(ssid, password);
                Serial.println("Calling commit...");
                config.commit();  // Ensure credentials are saved immediately
                Serial.println("=== PROVISIONING CREDENTIALS STORED ===");
                break;
            }
        case ARDUINO_EVENT_PROV_CRED_FAIL:
            Serial.println("\nProvisioning failed!");
            break;
        case ARDUINO_EVENT_PROV_CRED_SUCCESS:
            Serial.println("\nProvisioning Successful");
            config.commit();  // Ensure the flag is saved
            break;
        case ARDUINO_EVENT_PROV_END:
            Serial.println("\nProvisioning Ends");
            isProvisioning = false;
            break;
        default:
            break;
    }
}
