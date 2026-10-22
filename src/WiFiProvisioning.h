#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

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
    bool noProofOfPossession = false;
    int reconnectFailures = 0;        // consecutive background attempts without a link
    unsigned long offlineSince = 0;   // provisioned but no link since (0 = link up)
    std::function<bool()> idleCheck;  // the 15-minute restart only while the toy is idle
    bool homeNetworkVisible = true;   // last scan saw the stored SSID (assumed until a scan says otherwise)
    bool scanInFlight = false;
    unsigned long lastBeginAt = 0;    // last WiFi.begin() of ours (a connect may still be running)
    bool attemptedSinceOffline = false; // a WiFi.begin() of ours has had its chance in this outage
    unsigned long idleSeenAt = 0;     // WL_IDLE_STATUS (associated, no IP yet) first seen
    // countFailure: a scan saw the home network after our previous attempt and there is still no
    // link - a real failure. A plain retry (scan failed, no SSID stored) is not counted.
    void attemptReconnect(bool countFailure);
    void beginStored();
    // Credentials are mirrored into ConfigManager; NVS is written only when they differ.
    void storeCredentialsIfChanged(const String &ssid, const String &password);
    
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
    // plain reconnects fail, or when the link is up but nothing gets through. Refused (false)
    // while a firmware update is in progress.
    bool resetRadio(const char *why);
    void setIdleCheck(std::function<bool()> fn) { idleCheck = fn; }
    // False while the stored network is out of range (the toy is away from home, playing from
    // the card): no reconnect churn, no escalation, no "I cannot connect" prompt.
    bool isHomeNetworkVisible() const { return homeNetworkVisible; }
    
    // No proof of possession for BLE provisioning (no factory pop enrolled, and the build has
    // no TT_LEGACY_POP fallback): the toy cannot be provisioned and says so (112, red LED).
    bool provisioningImpossible() const { return noProofOfPossession; }
    // The PoP BLE provisioning uses: the factory pop, or the legacy PIN (TT_LEGACY_POP), or "".
    String proofOfPossession();

    // Query underlying library state
    bool isProvisioned();
    bool isProvisioningManagerActive();
    
    // Arduino STA events (got IP, disconnected); provisioning events come from the IDF (below)
    void handleProvisioningEvent(arduino_event_t *sys_event);

    enum class ProvEvent { CRED_RECV, CRED_FAIL, CRED_SUCCESS, ENDED };
    // Runs on the WiFi event task: keep it short (queue work for the loop).
    void onProvisioningEvent(std::function<void(ProvEvent)> cb) { eventCb = cb; }
private:
    std::function<void(ProvEvent)> eventCb;
    // The IDF provisioning manager posts WIFI_PROV_EVENT on the default event loop (the Arduino
    // ARDUINO_EVENT_PROV_* translation needs the WiFiProv wrapper, which this firmware does not
    // use), so we listen to the IDF events ourselves.
    static void provEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data);
    volatile bool provEndPending = false;   // WIFI_PROV_END seen; finish on the loop task
    bool provEventsRegistered = false;
};

#endif
