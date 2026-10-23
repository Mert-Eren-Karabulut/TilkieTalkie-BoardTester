#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <esp_event.h>
#include <functional>
#include <atomic>
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

    // Portal reachability (docs/PROTOCOL-V2.md §6). After every connection the toy fetches
    // http://<host>[:port]/storage/ping.json over plain HTTP (no TLS, no redirects, 5 s) and
    // needs the body {"ping":"tilkie"}; anything else is a captive portal. host/port come from
    // the configured API base (a bench `endpoint set` keeps working).
    void setProbeTarget(const String &host, uint16_t port) { probeHost = host; probePort = port; }
    // Connected and the probe passed for this connection: network services may run.
    bool isInternetReady() const { return WiFi.status() == WL_CONNECTED && portalOk; }
    // Connected, but something else answered the probe (login page, redirect, DNS hijack):
    // treated as offline, re-probed every 2 minutes, no prompt, no radio reset.
    bool isCaptive() const { return WiFi.status() == WL_CONNECTED && captiveNow; }
    // Connected but the probe got no answer (no internet on this link, portal down): offline
    // the same way, re-probed every 2 minutes; during pairing such a network is accepted.
    bool hasNoInternet() const { return WiFi.status() == WL_CONNECTED && noInternetNow; }
    bool isOfflineOnLink() const { return isCaptive() || hasNoInternet(); }

    // The stored network rejected the password 3 times in a row (router password changed):
    // no restarts and no radio resets any more, a silent retry every 30 min. UxScenes speaks
    // the failure at most every 6 h and shows the offline LED only briefly (quietOffline).
    bool credentialsRejected() const { return authFailStreak.load() >= AUTH_FAIL_LIMIT; }
    bool quietOffline() const { return credentialsRejected() && millis() - lastBeginAt > 30000UL; }
    // False while the stored network is out of range (the toy is away from home, playing from
    // the card): no reconnect churn, no escalation, no "I cannot connect" prompt.
    bool isHomeNetworkVisible() const { return homeNetworkVisible; }
    
    // No proof of possession for BLE provisioning (no factory pop enrolled, and the build has
    // the development PIN is empty too): the toy cannot be provisioned and says so (112, red LED).
    bool provisioningImpossible() const { return noProofOfPossession; }
    // The PoP BLE provisioning uses: the printed TK- code of an enrolled unit, else the shared development PIN.
    String proofOfPossession();

    // ---- Factory tests (FactoryTest.cpp, docs/FT-PROTOCOL.md) ----
    // Hold: the background reconnection, the portal probe of the loop and the credential
    // mirroring stand still while a test uses the radio, so a test network never reaches
    // ConfigManager and nothing reconnects behind the test's back. Releasing it forces a fresh
    // portal probe of whatever link comes next.
    void setFactoryHold(bool on);
    bool factoryHold() const { return factoryHoldOn; }
    // One portal probe (the same check as after every connection) on the current link.
    // startFactoryProbe() is false when no probe target is configured or one is still running.
    bool startFactoryProbe();
    int factoryProbeResult() const;              // -1 running, 0 captive, 1 ours, 2 no answer
    String factoryProbeWhy() const;
    bool hasProbeTarget() const { return !probeHost.isEmpty(); }
    // BLE advertising under the normal device name with the normal PoP, for the `ble` test.
    // 1 = started by us (factoryBleStop() ends it), 2 = provisioning already advertising (left
    // alone), 0 = failed (err says why).
    int factoryBleStart(String &name, String &err);
    void factoryBleStop();

    // Query underlying library state
    bool isProvisioned();
    // The BLE controller memory went back to the heap this boot (a provisioned toy releases it
    // right after boot): the factory `ble` test cannot run until a clean and a reboot.
    bool isBtReleased() const { return btReleased; }
    bool isProvisioningManagerActive();
    
    // Arduino STA events (got IP, disconnected); provisioning events come from the IDF (below)
    void handleProvisioningEvent(arduino_event_t *sys_event);

    // CAPTIVE: the network the app sent wants a login page; the app was told "failed" (like a
    // wrong password), the credentials are forgotten and provisioning goes on.
    enum class ProvEvent { CRED_RECV, CRED_FAIL, CRED_SUCCESS, ENDED, CAPTIVE };
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

    // Captive-portal probe
    static void gotIpPreHandler(void *arg, esp_event_base_t base, int32_t id, void *data);
    void serviceProbe();                       // loop: probe each new connection, re-probe a captive one
    String probeHost;
    uint16_t probePort = 80;
    std::atomic<uint32_t> connectionGen{0};    // bumped on every GOT_IP (Arduino event task)
    uint32_t probedGen = 0;
    bool loopProbeRunning = false;
    volatile bool portalOk = false;
    volatile bool captiveNow = false;
    volatile bool noInternetNow = false;
    unsigned long nextProbeAt = 0;
    bool captiveRaisedThisLink = false;
    volatile bool credRecvSeen = false;        // the app sent credentials; the next GOT_IP is probed in line
    volatile bool captiveRejectPending = false; // set before the synthetic auth failure is posted
    volatile bool captiveCleanupPending = false;
    volatile unsigned long provStopAt = 0;     // success: stop the manager 30 s later (auto-stop is off)

    // Factory tests
    volatile bool factoryHoldOn = false;
    bool factoryBle = false;           // the `ble` test owns the provisioning manager
    bool factoryBleWifiStarted = false; // ... and started the WiFi driver for it
    volatile bool btReleased = false;   // BT controller memory went back to the heap this boot

    // Wrong password on a provisioned toy
    static const int AUTH_FAIL_LIMIT = 3;
    std::atomic<int> authFailStreak{0};
};

#endif
