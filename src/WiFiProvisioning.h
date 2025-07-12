#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <WiFiProv.h>
#include <WiFi.h>
#include <wifi_provisioning/manager.h>
#include <esp_wifi.h>
#include "ConfigManager.h"

class WiFiProvisioningManager {
private:
    ConfigManager& config;
    bool isProvisioning;
    
    static WiFiProvisioningManager* instance;
    static void onSysProvEvent(arduino_event_t *sys_event);
    
public:
    WiFiProvisioningManager();
    
    // Singleton pattern
    static WiFiProvisioningManager& getInstance();
    
    void begin();
    void startProvisioning();
    void printQRCode();
    void handleCommand(const String& command);
    bool isConnected();
    void printStatus();
    void reset();
    
    // Callback for provisioning events
    void handleProvisioningEvent(arduino_event_t *sys_event);
};

#endif
