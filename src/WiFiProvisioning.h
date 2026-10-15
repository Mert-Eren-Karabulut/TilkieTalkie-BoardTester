#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <WiFiProv.h>
#include <esp_event.h>
#include <functional>
#include <WiFi.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>
#include <esp_wifi.h>
#include "ConfigManager.h"

class WiFiProvisioningManager {
private:
    ConfigManager& config;
    bool provisioningManagerInitialized;
    unsigned long lastReconnectAttempt;
    bool connectedOnce = false;
    int reconnectFailures = 0;        // consecutive background attempts without a link
    unsigned long offlineSince = 0;   // provisioned but no link since (0 = link up)
    std::function<bool()> idleCheck;  // the 15-minute restart only while the toy is idle
    
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
    void handleBackgroundReconnection();
    // Full radio restart (off, on, connect with the stored credentials): the escalation when
    // plain reconnects fail, or when the link is up but nothing gets through.
    void resetRadio(const char *why);
    void setIdleCheck(std::function<bool()> fn) { idleCheck = fn; }
    
    // Query underlying library state
    bool isProvisioned();
    bool isProvisioningManagerActive();
    
    // Callback for provisioning events
    void handleProvisioningEvent(arduino_event_t *sys_event);

    enum class ProvEvent { CRED_RECV, CRED_FAIL, CRED_SUCCESS, ENDED };
    // Runs on the WiFi event task: keep it short (queue work for the loop).
    void onProvisioningEvent(std::function<void(ProvEvent)> cb) { eventCb = cb; }
private:
    std::function<void(ProvEvent)> eventCb;
    // The IDF provisioning manager posts WIFI_PROV_EVENT on the default event loop; the Arduino
    // ARDUINO_EVENT_PROV_* translation only exists when the Arduino WiFiProv wrapper is used,
    // which this firmware does not. So we listen to the IDF events ourselves.
    static void provEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data);
    volatile bool provEndPending = false;   // WIFI_PROV_END seen; finish on the loop task
    bool provEventsRegistered = false;
};

#endif
