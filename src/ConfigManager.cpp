#include "ConfigManager.h"
#include <esp_log.h>

static const char *TAG = "cfg";

namespace {
struct CfgLock {
    SemaphoreHandle_t m;
    explicit CfgLock(SemaphoreHandle_t h) : m(h) { if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY); }
    ~CfgLock() { if (m) xSemaphoreGiveRecursive(m); }
};
}

// Static member definitions
ConfigManager* ConfigManager::instance = nullptr;
const char* ConfigManager::NAMESPACE = "config";
const char* ConfigManager::SETTINGS_NAMESPACE = "settings";
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
    lock = xSemaphoreCreateRecursiveMutex();
    
    // Now initialize preferences. A failure is NOT answered by erasing the whole NVS (that
    // would also drop the WiFi credentials, the fault ring and the OTA state): it is logged,
    // reported as CONFIG_CORRUPT once Faults is up, and begin() is retried on the next access.
    opened = preferences.begin(NAMESPACE, false);
    if (!opened) {
        ESP_LOGE(TAG, "ERROR: Failed to initialize preferences!");
        openFailed = true;
    }
}

bool ConfigManager::ensureOpen() {
    if (!opened) {
        opened = preferences.begin(NAMESPACE, false);
    }
    return opened;
}

bool ConfigManager::takeOpenFailure() {
    CfgLock g(lock);
    const bool failed = openFailed;
    openFailed = false;
    return failed;
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
    CfgLock g(lock);
    if (!ensureOpen() || !preferences.isKey(WIFI_SSID_KEY)) {
        return "";
    }
    return preferences.getString(WIFI_SSID_KEY, "");
}

String ConfigManager::getWiFiPassword() {
    CfgLock g(lock);
    if (!ensureOpen() || !preferences.isKey(WIFI_PASSWORD_KEY)) {
        return "";
    }
    return preferences.getString(WIFI_PASSWORD_KEY, "");
}

void ConfigManager::setWiFiCredentials(const String& ssid, const String& password) {
    if (ssid.length() == 0 || password.length() == 0) {
        return;
    }
    CfgLock g(lock);
    if (!ensureOpen()) {
        ESP_LOGE(TAG, "ERROR: Failed to store WiFi credentials");
        return;
    }
    // Overwrite in place (no remove first: a reset between remove and put used to lose the
    // credentials), and only when they changed (flash wear).
    if (preferences.isKey(WIFI_SSID_KEY) && preferences.isKey(WIFI_PASSWORD_KEY) &&
        preferences.getString(WIFI_SSID_KEY, "") == ssid && preferences.getString(WIFI_PASSWORD_KEY, "") == password) {
        return;
    }
    size_t ssidResult = preferences.putString(WIFI_SSID_KEY, ssid);
    size_t passResult = preferences.putString(WIFI_PASSWORD_KEY, password);
    
    if (ssidResult == 0 || passResult == 0) {
        ESP_LOGE(TAG, "ERROR: Failed to store WiFi credentials");
    }
}

bool ConfigManager::hasWiFiCredentials() {
    CfgLock g(lock);
    // Check if keys exist before trying to read them to avoid NVS errors
    if (!ensureOpen() || !preferences.isKey(WIFI_SSID_KEY) || !preferences.isKey(WIFI_PASSWORD_KEY)) {
        return false;
    }
    
    // Only read if keys exist
    String ssid = preferences.getString(WIFI_SSID_KEY, "");
    String password = preferences.getString(WIFI_PASSWORD_KEY, "");
    
    return ssid.length() > 0 && password.length() > 0;
}

void ConfigManager::clearWiFiCredentials() {
    CfgLock g(lock);
    if (!ensureOpen()) return;
    preferences.remove(WIFI_SSID_KEY);
    preferences.remove(WIFI_PASSWORD_KEY);
}

// Getters never write: the default is derived again on every read until something is set.
String ConfigManager::getDeviceName() {
    String deviceName;
    {
        CfgLock g(lock);
        if (ensureOpen() && preferences.isKey(DEVICE_NAME_KEY)) {
            deviceName = preferences.getString(DEVICE_NAME_KEY, "");
        }
    }
    if (deviceName.length() == 0) {
        deviceName = String(F("TilkieTalkie_")) + String(ESP.getEfuseMac());
    }
    return deviceName;
}

void ConfigManager::setDeviceName(const String& name) {
    CfgLock g(lock);
    if (ensureOpen()) preferences.putString(DEVICE_NAME_KEY, name);
}

String ConfigManager::getProvisioningPin() {
    String pin;
    {
        CfgLock g(lock);
        if (ensureOpen() && preferences.isKey(PROVISIONING_PIN_KEY)) {
            pin = preferences.getString(PROVISIONING_PIN_KEY, "");
        }
    }
    if (pin.length() == 0) {
        pin = F("abcd1234");
    }
    return pin;
}

void ConfigManager::setProvisioningPin(const String& pin) {
    CfgLock g(lock);
    if (ensureOpen()) preferences.putString(PROVISIONING_PIN_KEY, pin);
}


void ConfigManager::storeCurrentWiFiCredentials() {
    if (WiFi.isConnected()) {
        setWiFiCredentials(WiFi.SSID(), WiFi.psk());
    }
}

void ConfigManager::resetAll() {
    CfgLock g(lock);
    if (ensureOpen()) preferences.clear();
}

void ConfigManager::printAllSettings() {
    CfgLock g(lock);
    ESP_LOGI(TAG, "=== Configuration Settings ===");
    ESP_LOGI(TAG, "WiFi SSID: %s", getWiFiSSID().c_str());
    ESP_LOGI(TAG, "WiFi Password: %s", getWiFiPassword().length() > 0 ? "***" : "Not set");
    ESP_LOGI(TAG, "Device Name: %s", getDeviceName().c_str());
    ESP_LOGI(TAG, "Provisioning PIN: %s", getProvisioningPin().c_str());
    ESP_LOGI(TAG, "JWT Token: %s", getJWTToken().length() > 0 ? "Set" : "Not set");
    ESP_LOGI(TAG, "Config Valid: %s", isValid() ? "Yes" : "No");
    ESP_LOGI(TAG, "Free NVS Space: %u bytes", (unsigned)getFreeSpace());
    
    // Settings namespace info
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, true)) {
        ESP_LOGI(TAG, "Settings Namespace Entries: %u", (unsigned)settingsPrefs.freeEntries());
        settingsPrefs.end();
    }
    
    // Essential NVS statistics only
    nvs_stats_t nvs_stats;
    if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
        ESP_LOGI(TAG, "NVS Stats: %u used, %u free", (unsigned)nvs_stats.used_entries, (unsigned)nvs_stats.free_entries);
    }
    
    ESP_LOGI(TAG, "==============================");
}

bool ConfigManager::isValid() {
    // Check if the configuration is in a valid state
    // At minimum, device name and provisioning pin should be set
    return getDeviceName().length() > 0 && getProvisioningPin().length() > 0;
}

void ConfigManager::commit() {
    // Intentionally empty: Preferences::put* commits to flash before returning.
}

size_t ConfigManager::getFreeSpace() {
    CfgLock g(lock);
    return ensureOpen() ? preferences.freeEntries() : 0;
}

void ConfigManager::factoryReset() {
    ESP_LOGI(TAG, "=== FACTORY RESET ===");
    ESP_LOGW(TAG, "WARNING: This will erase ALL stored data!");
    CfgLock g(lock);
    
    // Close preferences first
    preferences.end();
    opened = false;
    
    // Erase entire NVS flash
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ERROR: Failed to erase NVS flash");
        ESP_LOGE(TAG, "Error code: %d", (int)err);
    } else {
        ESP_LOGI(TAG, "SUCCESS: NVS flash erased");
    }
    
    // Reinitialize NVS
    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ERROR: Failed to reinitialize NVS");
        ESP_LOGE(TAG, "Error code: %d", (int)err);
    } else {
        ESP_LOGI(TAG, "SUCCESS: NVS reinitialized");
    }
    
    // Reopen preferences
    opened = preferences.begin(NAMESPACE, false);
    if (opened) {
        ESP_LOGI(TAG, "SUCCESS: Preferences reopened");
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to reopen preferences");
    }
    
    ESP_LOGI(TAG, "=== FACTORY RESET COMPLETE ===");
    ESP_LOGI(TAG, "Device will restart in 3 seconds...");
    delay(3000);
    ESP.restart();
}

String ConfigManager::getJWTToken() {
    CfgLock g(lock);
    if (!ensureOpen() || !preferences.isKey(JWT_TOKEN_KEY)) {
        return String();
    }
    return preferences.getString(JWT_TOKEN_KEY, "");
}

void ConfigManager::setJWTToken(const String &token) {
    CfgLock g(lock);
    if (ensureOpen()) preferences.putString(JWT_TOKEN_KEY, token);
}

// Simple settings storage methods
void ConfigManager::storeInt(const String& keyname, int value) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        size_t result = settingsPrefs.putInt(keyname.c_str(), value);
        if (result == 0) {
            ESP_LOGE(TAG, "ERROR: Failed to store int setting: %s", keyname.c_str());
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for storing int");
    }
}

void ConfigManager::storeString(const String& keyname, const String& value) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        size_t result = settingsPrefs.putString(keyname.c_str(), value);
        if (result == 0) {
            ESP_LOGE(TAG, "ERROR: Failed to store string setting: %s", keyname.c_str());
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for storing string");
    }
}

int ConfigManager::getInt(const String& keyname, int defaultValue) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    int value = defaultValue;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, true)) {  // Read-only mode
        if (settingsPrefs.isKey(keyname.c_str())) {
            value = settingsPrefs.getInt(keyname.c_str(), defaultValue);
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for reading int");
    }
    return value;
}

String ConfigManager::getString(const String& keyname, const String& defaultValue) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    String value = defaultValue;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, true)) {  // Read-only mode
        if (settingsPrefs.isKey(keyname.c_str())) {
            value = settingsPrefs.getString(keyname.c_str(), defaultValue);
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for reading string");
    }
    return value;
}

void ConfigManager::deleteSetting(const String& keyname) {
    CfgLock g(lock);
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        // Deleting a key that was never written is normal (OTA bookkeeping clears all its keys);
        // Preferences::remove() logs an error for that, so check first.
        if (settingsPrefs.isKey(keyname.c_str()) && !settingsPrefs.remove(keyname.c_str())) {
            ESP_LOGW(TAG, "Failed to delete setting: %s", keyname.c_str());
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for deleting setting");
    }
}

void ConfigManager::deleteAllSettings() {
    CfgLock g(lock);
    Preferences settingsPrefs;
    if (settingsPrefs.begin(SETTINGS_NAMESPACE, false)) {
        bool result = settingsPrefs.clear();
        if (result) {
            ESP_LOGI(TAG, "SUCCESS: All settings cleared");
        } else {
            ESP_LOGE(TAG, "ERROR: Failed to clear all settings");
        }
        settingsPrefs.end();
    } else {
        ESP_LOGE(TAG, "ERROR: Failed to open settings namespace for clearing all settings");
    }
}
