#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Preferences.h>
#include <Arduino.h>
#include <nvs_flash.h>
#include <esp_err.h>
#include <WiFi.h>
#include <nvs.h>

class ConfigManager
{
private:
    Preferences preferences;
    static ConfigManager *instance;

    // Configuration keys
    static const char *NAMESPACE;
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
    void commit();   // Force commit all changes to flash
    size_t getFreeSpace();  // Get available NVS space
    void factoryReset();  // Complete factory reset including NVS erase

    // Add more configuration variables as needed
    // Example: audio settings, sensor calibration, etc.
};

#endif
