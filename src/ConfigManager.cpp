#include "ConfigManager.h"

// Static member definitions
ConfigManager* ConfigManager::instance = nullptr;
const char* ConfigManager::NAMESPACE = "config";
const char* ConfigManager::WIFI_SSID_KEY = "ssid";        // Shortened key
const char* ConfigManager::WIFI_PASSWORD_KEY = "pass";     // Shortened key
const char* ConfigManager::DEVICE_NAME_KEY = "device";     // Shortened key
const char* ConfigManager::PROVISIONING_PIN_KEY = "pin";   // Shortened key
const char* ConfigManager::JWT_TOKEN_KEY = "jwt";         // Shortened key

ConfigManager::ConfigManager() {
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    
    // Now initialize preferences
    if (!preferences.begin(NAMESPACE, false)) {
        Serial.println("ERROR: Failed to initialize preferences!");
        // Try to recover by clearing and retrying
        preferences.end();
        nvs_flash_erase();
        nvs_flash_init();
        if (!preferences.begin(NAMESPACE, false)) {
            Serial.println("FATAL: Unable to initialize NVS storage!");
        }
    }
}

ConfigManager::~ConfigManager() {
    preferences.end();
}

ConfigManager& ConfigManager::getInstance() {
    if (instance == nullptr) {
        instance = new ConfigManager();
    }
    return *instance;
}

String ConfigManager::getWiFiSSID() {
    if (!preferences.isKey(WIFI_SSID_KEY)) {
        return "";
    }
    return preferences.getString(WIFI_SSID_KEY, "");
}

String ConfigManager::getWiFiPassword() {
    if (!preferences.isKey(WIFI_PASSWORD_KEY)) {
        return "";
    }
    return preferences.getString(WIFI_PASSWORD_KEY, "");
}

void ConfigManager::setWiFiCredentials(const String& ssid, const String& password) {
    if (ssid.length() == 0 || password.length() == 0) {
        return;
    }
    
    // Clear old values first
    preferences.remove(WIFI_SSID_KEY);
    preferences.remove(WIFI_PASSWORD_KEY);
    
    // Store new values
    size_t ssidResult = preferences.putString(WIFI_SSID_KEY, ssid);
    size_t passResult = preferences.putString(WIFI_PASSWORD_KEY, password);
    
    if (ssidResult == 0 || passResult == 0) {
        Serial.println(F("ERROR: Failed to store WiFi credentials"));
    }
}

bool ConfigManager::hasWiFiCredentials() {
    // Check if keys exist before trying to read them to avoid NVS errors
    if (!preferences.isKey(WIFI_SSID_KEY) || !preferences.isKey(WIFI_PASSWORD_KEY)) {
        return false;
    }
    
    // Only read if keys exist
    String ssid = preferences.getString(WIFI_SSID_KEY, "");
    String password = preferences.getString(WIFI_PASSWORD_KEY, "");
    
    return ssid.length() > 0 && password.length() > 0;
}

void ConfigManager::clearWiFiCredentials() {
    preferences.remove(WIFI_SSID_KEY);
    preferences.remove(WIFI_PASSWORD_KEY);
}

String ConfigManager::getDeviceName() {
    String deviceName = preferences.getString(DEVICE_NAME_KEY, "");
    if (deviceName.length() == 0) {
        // Generate default device name and store it
        deviceName = String(F("TilkieTalkie_")) + String(ESP.getEfuseMac());
        preferences.putString(DEVICE_NAME_KEY, deviceName);
    }
    return deviceName;
}

void ConfigManager::setDeviceName(const String& name) {
    preferences.putString(DEVICE_NAME_KEY, name);
}

String ConfigManager::getProvisioningPin() {
    String pin = preferences.getString(PROVISIONING_PIN_KEY, "");
    if (pin.length() == 0) {
        // Use default PIN and store it
        pin = F("abcd1234");
        preferences.putString(PROVISIONING_PIN_KEY, pin);
    }
    return pin;
}

void ConfigManager::setProvisioningPin(const String& pin) {
    preferences.putString(PROVISIONING_PIN_KEY, pin);
}


void ConfigManager::storeCurrentWiFiCredentials() {
    if (WiFi.isConnected()) {
        setWiFiCredentials(WiFi.SSID(), WiFi.psk());
    }
}

void ConfigManager::resetAll() {
    preferences.clear();
}

void ConfigManager::printAllSettings() {
    Serial.println(F("=== Configuration Settings ==="));
    Serial.print(F("WiFi SSID: ")); Serial.println(getWiFiSSID());
    Serial.print(F("WiFi Password: ")); Serial.println(getWiFiPassword().length() > 0 ? F("***") : F("Not set"));
    Serial.print(F("Device Name: ")); Serial.println(getDeviceName());
    Serial.print(F("Provisioning PIN: ")); Serial.println(getProvisioningPin());
    Serial.print(F("JWT Token: ")); Serial.println(getJWTToken().length() > 0 ? F("Set") : F("Not set"));
    Serial.print(F("Config Valid: ")); Serial.println(isValid() ? F("Yes") : F("No"));
    Serial.print(F("Free NVS Space: ")); Serial.print(getFreeSpace()); Serial.println(F(" bytes"));
    
    // Essential NVS statistics only
    nvs_stats_t nvs_stats;
    if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
        Serial.print(F("NVS Stats: "));
        Serial.print(nvs_stats.used_entries);
        Serial.print(F(" used, "));
        Serial.print(nvs_stats.free_entries);
        Serial.println(F(" free"));
    }
    
    Serial.println(F("=============================="));
}

bool ConfigManager::isValid() {
    // Check if the configuration is in a valid state
    // At minimum, device name and provisioning pin should be set
    return getDeviceName().length() > 0 && getProvisioningPin().length() > 0;
}

void ConfigManager::commit() {
    // Silent commit - only essential error reporting
}

size_t ConfigManager::getFreeSpace() {
    return preferences.freeEntries();
}

void ConfigManager::factoryReset() {
    Serial.println("=== FACTORY RESET ===");
    Serial.println("WARNING: This will erase ALL stored data!");
    
    // Close preferences first
    preferences.end();
    
    // Erase entire NVS flash
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        Serial.println("ERROR: Failed to erase NVS flash");
        Serial.println("Error code: " + String(err));
    } else {
        Serial.println("SUCCESS: NVS flash erased");
    }
    
    // Reinitialize NVS
    err = nvs_flash_init();
    if (err != ESP_OK) {
        Serial.println("ERROR: Failed to reinitialize NVS");
        Serial.println("Error code: " + String(err));
    } else {
        Serial.println("SUCCESS: NVS reinitialized");
    }
    
    // Reopen preferences
    if (preferences.begin(NAMESPACE, false)) {
        Serial.println("SUCCESS: Preferences reopened");
    } else {
        Serial.println("ERROR: Failed to reopen preferences");
    }
    
    Serial.println("=== FACTORY RESET COMPLETE ===");
    Serial.println("Device will restart in 3 seconds...");
    delay(3000);
    ESP.restart();
}

String ConfigManager::getJWTToken() {
    String token = preferences.getString(JWT_TOKEN_KEY, "");
    return token;
}

void ConfigManager::setJWTToken(const String &token) {
    preferences.putString(JWT_TOKEN_KEY, token);
}
