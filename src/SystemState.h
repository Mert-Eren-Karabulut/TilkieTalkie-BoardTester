#pragma once

// State shared between main.cpp (setup/loop/hooks) and the modules split out of it
// (ChargerSupervisor, DeadCellRecovery, Bringup, UxScenes, Console). Everything here is
// defined in main.cpp and only touched from the loop task.

#include <Arduino.h>
#include "ConfigManager.h"
#include "WiFiProvisioning.h"
#include "BatteryManagement.h"
#include "FileManager.h"
#include "AudioController.h"
#include "LedController.h"
#include "NfcController.h"
#include "RequestManager.h"
#include "ReverbClient.h"
#include "Buttons.h"
#include "SleepController.h"

// --- Singletons (aliases defined in main.cpp) ---
extern NfcController &nfcController;
extern ConfigManager &config;
extern RequestManager &requestManager;
extern WiFiProvisioningManager &wifiProv;
extern BatteryManager &battery;
extern FileManager &fileManager;
extern AudioController &audioController;
extern LedController ledController;
extern ReverbClient &reverb;
extern ButtonController &buttonController;
extern SleepController &sleepController;

// --- Deferred system bring-up ---
// On boot the device starts in a minimal battery-monitor mode and only brings up the heavy
// subsystems (SD, WiFi, audio, NFC, buttons) once the cell can support them without
// brown-out looping. A depleted board charges in monitor mode instead of crashing.
enum SystemPhase
{
    PHASE_MONITOR, // minimal: battery + bring-up + LED only, waiting for a healthy cell
    PHASE_RUNNING  // full system started
};
extern SystemPhase gSystemPhase;
extern unsigned long gFullSystemStartedAt; // millis() when startFullSystem() ran
extern unsigned long gFigureLoadingSince;  // tap time until something plays / fails
extern bool gSystemError;                  // NFC reader failed to come up (steady red scene)
extern int gOtaProgress;                   // 0-100 while an update is being written, else -1
// time_valid: TimeSync::valid() (TimeSync.h). SNTP starts once WiFi has an IP; until the clock
// is set, fault events carry only their age and command issued_at is not checked.

// Gauge learning-cycle discharge (bench `learndis`): the charger is in HIZ on purpose, so the
// charger supervisor, the charge bar and the low-battery cutoff stand down. Always false in a
// production build.
extern bool gLearnDischarge;

// Factory `ft test pack`: the charger input is in HIZ for a few seconds on purpose (load step
// for the pack-resistance measurement); the charger supervisor and the low-battery cutoff
// stand down meanwhile.
extern bool gFactoryLoadStep;

// `gaugebus release`: the battery bus belongs to an external tool (bqStudio/EV2400) until
// `gaugebus attach`. Persisted so a wake from deep sleep does not grab the bus back.
constexpr const char *GAUGEBUS_NVS_KEY = "gaugebus_rel";
extern bool gGaugeBusReleased;

constexpr uint32_t SYSTEM_MONITOR_LED = 0x0000FF; // blue breathing while warming up / charging

// --- Backend endpoints (production by default; a bench build can override them in NVS) ---
struct Endpoints
{
    String apiBase;   // e.g. https://portal.tilkietalkie.com/api
    String apiOrigin; // apiBase without the trailing /api
    String reverbHost;
    uint16_t reverbPort;
    bool reverbTls;
    String appKey;
};
Endpoints loadEndpoints();

// --- Hooks main.cpp provides to the console ---
void startFullSystem();
// Factory session (FactoryTest.cpp): startFullSystem() leaves WiFi/BLE alone while a session is
// open; `ft end` starts what it skipped (no-op otherwise).
void startDeferredWifi();
void startReverb();
String getDeviceEfuseMacDecimal();
String getDeviceEfuseMacHex();

// --- Resets (docs/PROTOCOL-V2.md §7) ---
// Customer reset: forget WiFi, delete the device token, wipe the card cache (stories,
// manifests, queue, uid mappings; not the prompt pack, not /.device), clear the fault ring and
// the volume/brightness settings, ask the portal to unbind (best effort, 3 s), restart into
// provisioning. all=true (`ft reset --all`) also erases the factory namespace. Loop task only;
// does not return.
void runCustomerReset(bool all, const char *why, bool speak);
// From a callback that must not block (the Reverb `factory-reset` command): runs from the loop.
void requestCustomerReset(const char *why);
