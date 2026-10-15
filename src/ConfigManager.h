#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Preferences.h>
#include <Arduino.h>
#include <nvs_flash.h>
#include <esp_err.h>
#include <WiFi.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class ConfigManager
{
private:
    Preferences preferences;
    static ConfigManager *instance;
    // Preferences is used from the loop task and the request worker (token refresh): every
    // access goes through this recursive mutex.
    SemaphoreHandle_t lock = nullptr;
    bool opened = false;        // preferences.begin() succeeded
    bool openFailed = false;    // begin() failed at construction (reported once via takeOpenFailure)
    bool ensureOpen();          // lazily retries begin() (caller holds the lock)

    // Configuration keys
    static const char *NAMESPACE;
    static const char *SETTINGS_NAMESPACE;  // Separate namespace for user settings
    static const char *WIFI_SSID_KEY;
    static const char *WIFI_PASSWORD_KEY;
    static const char *DEVICE_NAME_KEY;
    static const char *PROVISIONING_PIN_KEY;
    static const char *JWT_TOKEN_KEY;

public:
    ConfigManager();
    ~ConfigManager();

    // Singleton pattern
    static ConfigManager &getInstance();

    // WiFi related
    String getWiFiSSID();
    String getWiFiPassword();
    void setWiFiCredentials(const String &ssid, const String &password);
    bool hasWiFiCredentials();
    void clearWiFiCredentials();
    void storeCurrentWiFiCredentials();  // Store currently connected WiFi credentials

    // Device settings
    String getDeviceName();
    void setDeviceName(const String &name);
    String getProvisioningPin();
    void setProvisioningPin(const String &pin);

    // JWT Token management
    String getJWTToken();
    void setJWTToken(const String &token);

    // General methods
    void resetAll();
    void printAllSettings();
    bool isValid();  // Check if config is in a valid state
    // No-op: Preferences commits on every put. Kept for the callers that mark a save point.
    void commit();
    // True once if the NVS namespace could not be opened at construction (before Faults is up):
    // the caller raises CONFIG_CORRUPT.
    bool takeOpenFailure();
    size_t getFreeSpace();  // Get available NVS space
    void factoryReset();  // Complete factory reset including NVS erase
    
    // Simple settings storage methods
    void storeInt(const String& keyname, int value);
    void storeString(const String& keyname, const String& value);
    int getInt(const String& keyname, int defaultValue = 0);
    String getString(const String& keyname, const String& defaultValue = "");
    void deleteSetting(const String& keyname);
    void deleteAllSettings();

    // Add more configuration variables as needed
    // Example: audio settings, sensor calibration, etc.
};

#endif
