#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_private/brownout.h> // esp_brownout_disable() for the dead-cell recovery hold
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <sys/time.h> // gettimeofday: system time survives deep sleep (sleeptest elapsed)
#include <esp_log.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <AsyncTCP.h>
#include <wifi_provisioning/manager.h>
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
#include "AsyncSpeedTest.h"
#include "SleepController.h"
#include "rtc_wake_stub_vbus.h"
#include "Diag.h"
#include "OtaManager.h"
#include "Version.h"

// The espidf CMake build compiles every file in src/ regardless of build_src_filter,
// so the sleep-probe build (env:sleepprobe, -DSLEEP_PROBE_BUILD) excludes this whole
// application via this guard and provides its own setup()/loop() in sleepprobe_main.cpp.
#ifndef SLEEP_PROBE_BUILD

static const char *TAG = "MAIN";

// Helper macros to ensure Serial output appears via ESP-IDF logging
#define LOG_PRINTF(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#define LOG_PRINT(tag, msg) ESP_LOGI(tag, "%s", msg)

// Bypass PSRAM test to prevent early boot issues
BYPASS_SPIRAM_TEST(true);

// Use the singleton instance from the header
NfcController &nfcController = NfcController::getInstance();

// Global instances
ConfigManager &config = ConfigManager::getInstance();
RequestManager &requestManager = RequestManager::getInstance("https://portal.tilkietalkie.com/api");
WiFiProvisioningManager &wifiProv = WiFiProvisioningManager::getInstance();
BatteryManager &battery = BatteryManager::getInstance();
FileManager &fileManager = FileManager::getInstance();
AudioController &audioController = AudioController::getInstance();
LedController ledController;
ReverbClient &reverb = ReverbClient::getInstance(); // Create an alias for easier access
ButtonController &buttonController = ButtonController::getInstance();
SleepController &sleepController = SleepController::getInstance();

const bool DEBUG = true; // Set to false to disable debug prints

// Gauge learning-cycle discharge state. When active (learndis), the board runs off the
// battery (charger HIZ) to drain it. loop() enforces a hard floor so the discharge
// never drives the LFP cell into the CUV protection trip.
static bool gLearnDischarge = false;
static const float LEARN_DISCHARGE_FLOOR_V = 2.90f; // stop discharge at/below this (loaded)

// Low-battery cutoff: when running on battery (not charging), force deep sleep before the
// LFP cell can over-discharge. Sits well above the gauge CUV (~2.5V) so the firmware, not
// the protection IC, is what stops the drain. Debounced to ignore momentary load sag.
static const float LOW_BATTERY_CUTOFF_V = 3.00f;
static const unsigned long LOW_BATTERY_CUTOFF_DEBOUNCE_MS = 5000;

// Deep-discharge recovery thresholds. Battery-only below SHIP_MODE_CUTOFF_V the cell is
// disconnected outright (BQ25792 ship mode, ~µA drain, wake = USB plug-in — QON is not
// wired on this board). On USB below DEAD_CELL_RECOVERY_ENTER_V the boot charges with
// the ESP parked in deep sleep (see runDeadCellRecoveryHold), re-checking each minute.
static const float SHIP_MODE_CUTOFF_V = 2.85f;
static const float DEAD_CELL_RECOVERY_ENTER_V = 2.85f;
// A rested near-empty cell can read above SYSTEM_START_V for a single sample and then
// collapse under WiFi load, so the full system starts only on a clearly-healthy voltage
// immediately, or on SYSTEM_START_V sustained for SYSTEM_START_HOLD_MS.
static const float SYSTEM_START_IMMEDIATE_V = 3.20f;
static const unsigned long SYSTEM_START_HOLD_MS = 10000;

// --- First-boot bring-up: auto gauge provisioning + current-sense calibration ---
// Runs once per board (gated by an NVS flag). LED indicates the phase (see FACTORY_BRINGUP.md).
enum BringupPhase
{
    BRINGUP_INACTIVE,    // already done (NVS flag set) or finished -> normal operation
    BRINGUP_PROVISION,   // write the LiFePO4 gauge config (1S, chem, capacity, thresholds)
    BRINGUP_WAIT_CHARGE, // wait for a steady charge current (technician: ensure USB power)
    BRINGUP_CALIBRATE,   // calibrate current sense against the charger IBAT ADC
    BRINGUP_COMPLETE,    // mark done in NVS, raise charge current, show success
    BRINGUP_FAILED       // hardware fault (e.g. gauge not detected)
};
static BringupPhase gBringupPhase = BRINGUP_INACTIVE;
static const char *BRINGUP_NVS_KEY = "bringup_v1";
// Bring-up LED phase colors (documented in FACTORY_BRINGUP.md):
static const uint32_t BRINGUP_LED_PROVISION = 0x0000FF; // solid BLUE   - provisioning
static const uint32_t BRINGUP_LED_WAIT = 0xFF6000;      // pulsing AMBER - waiting for charge
static const uint32_t BRINGUP_LED_CALIBRATE = 0x00FFFF; // solid CYAN   - calibrating
static const uint32_t BRINGUP_LED_SUCCESS = 0x00FF00;   // solid GREEN  - complete
static const uint32_t BRINGUP_LED_FAIL = 0xFF0000;      // pulsing RED  - failed

// --- Deferred system bring-up ---
// On boot the device starts in a minimal battery-monitor mode and only brings up the heavy
// subsystems (SD, WiFi, audio, NFC, buttons) once the cell can support them without
// brown-out looping. A depleted board charges in monitor mode instead of crashing.
enum SystemPhase
{
    PHASE_MONITOR, // minimal: battery + bring-up + LED only, waiting for a healthy cell
    PHASE_RUNNING  // full system started
};
static SystemPhase gSystemPhase = PHASE_MONITOR;
static unsigned long gFullSystemStartedAt = 0;  // millis() when startFullSystem() ran
static unsigned long gFigureLoadingSince = 0;   // tap time until something plays / fails
static bool gSystemError = false;               // SD or audio failed to come up
static int gOtaProgress = -1;                   // 0-100 while an update is being written
static const float SYSTEM_START_V = 3.10f;            // cell must reach this to start full system
static const uint32_t SYSTEM_MONITOR_LED = 0x0000FF;  // blue breathing while warming up / charging
void startFullSystem();

// --- Gauge-based sleep-current measurement (`sleeptest`) ---
// The cell path cannot be measured with a series DMM (the burden resistance corrupts
// the gauge's impedance tracking), so the gauge itself is the ammeter: it stays awake
// through deep sleep and coulomb-counts everything the cell sources. A test records
// RemainingCapacity + wall time into RTC memory, deep-sleeps for N minutes on a timer
// (VBUS wake stub suppressed so it cannot hijack the timer), and the wake boot prints
// delta-mAh -> average mA. Variant flags isolate suspects against the baseline
// production sleep. Battery only (charging makes the coulomb delta meaningless);
// prefer >= 30 minute runs — RemCap has 1mAh resolution and IT applies corrections.
struct SleepTestState
{
    uint32_t magic;
    uint64_t startEpochUs;
    uint16_t startRemainingMAh;
    uint16_t startMillivolts;
    uint8_t railOn;
    uint8_t nfcAwake;
    uint8_t holdPads;
};
static RTC_DATA_ATTR SleepTestState gSleepTest = {};
static const uint32_t SLEEP_TEST_MAGIC = 0x51EE7E57; // "SLEEpTESt"

static uint64_t epochMicros()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

// Prints the result of a pending sleep test on the wake boot. Called right after
// battery.begin() so the gauge readings are fresh.
static void reportSleepTestIfAny()
{
    if (gSleepTest.magic != SLEEP_TEST_MAGIC)
    {
        return;
    }
    gSleepTest.magic = 0;

    const float hours = (epochMicros() - gSleepTest.startEpochUs) / 3600.0e6f;
    if (!battery.hasTelemetry() || hours <= 0.0f)
    {
        ESP_LOGE(TAG, "SLEEPTEST: woke without usable telemetry — result discarded.");
        return;
    }
    const int endRemainingMAh = battery.getRemainingCapacityMilliAmpHours();
    const int deltaMAh = (int)gSleepTest.startRemainingMAh - endRemainingMAh;
    const int endMillivolts = (int)(battery.getBatteryVoltage() * 1000.0f);

    ESP_LOGW(TAG, "================= SLEEP TEST RESULT =================");
    ESP_LOGW(TAG, "Variant: rail=%s, NFC=%s, pad-hold=%s",
             gSleepTest.railOn ? "ON" : "off",
             gSleepTest.nfcAwake ? "AWAKE" : "powered-down",
             gSleepTest.holdPads ? "ARMED" : "off");
    ESP_LOGW(TAG, "Slept %.1f min: RemCap %u -> %d mAh (delta %d mAh), %u -> %d mV",
             hours * 60.0f, gSleepTest.startRemainingMAh, endRemainingMAh,
             deltaMAh, gSleepTest.startMillivolts, endMillivolts);
    ESP_LOGW(TAG, "AVERAGE SLEEP CURRENT ~= %.1f mA", deltaMAh / hours);
    ESP_LOGW(TAG, "(1mAh resolution + IT corrections: compare variants relatively, >=30min runs)");
    ESP_LOGW(TAG, "=====================================================");
}

// Runs one sleep-current measurement. Replicates the production battery-sleep
// preparation, with knobs to leave individual suspects powered for A/B isolation.
static void runSleepTest(uint32_t minutes, bool railOn, bool nfcAwake, bool holdPads)
{
    if (battery.isVbusPresent())
    {
        ESP_LOGE(TAG, "SLEEPTEST: unplug USB first — a charging cell makes the delta meaningless.");
        return;
    }
    if (!battery.hasTelemetry())
    {
        ESP_LOGE(TAG, "SLEEPTEST: no battery telemetry available.");
        return;
    }

    gSleepTest.magic = SLEEP_TEST_MAGIC;
    gSleepTest.startEpochUs = epochMicros();
    gSleepTest.startRemainingMAh = battery.getRemainingCapacityMilliAmpHours();
    gSleepTest.startMillivolts = (uint16_t)(battery.getBatteryVoltage() * 1000.0f);
    gSleepTest.railOn = railOn;
    gSleepTest.nfcAwake = nfcAwake;
    gSleepTest.holdPads = holdPads;

    ESP_LOGW(TAG, "SLEEPTEST: %lu min, rail=%s, NFC=%s, pad-hold=%s — start RemCap %umAh @ %umV.",
             (unsigned long)minutes, railOn ? "ON" : "off", nfcAwake ? "AWAKE" : "powered-down",
             holdPads ? "ARMED" : "off", gSleepTest.startRemainingMAh, gSleepTest.startMillivolts);
    ESP_LOGW(TAG, "SLEEPTEST: result prints on the wake boot. A button press ends the run early");
    ESP_LOGW(TAG, "(still valid — real elapsed time is recorded).");

    if (!nfcAwake)
    {
        // Same PN532 power-down + bus parking as the production sleep path.
        nfcController.powerDown();
        pinMode(NFC_SDA_PIN, OUTPUT);
        digitalWrite(NFC_SDA_PIN, HIGH);
        gpio_hold_en((gpio_num_t)NFC_SDA_PIN);
        pinMode(NFC_SCL_PIN, OUTPUT);
        digitalWrite(NFC_SCL_PIN, HIGH);
        gpio_hold_en((gpio_num_t)NFC_SCL_PIN);
    }
    battery.prepareForDeepSleep();
    if (railOn)
    {
        // Keep the 4V5 rail energized through the sleep (GPIO4 is already high).
        gpio_hold_en(GPIO_NUM_4);
    }
    else
    {
        sleepController.disablePeripheralPower(); // drives GPIO4 low + latches it
    }
    if (holdPads)
    {
        gpio_deep_sleep_hold_en(); // the global digital-pad hold under test (~13mA suspect)
    }

    // Timer wake for the measurement window; buttons/pogo stay armed as an abort path.
    // The VBUS wake stub must NOT run here — it would re-arm a 20s timer and hijack
    // the measurement window.
    esp_set_deep_sleep_wake_stub(NULL);
    esp_sleep_enable_timer_wakeup((uint64_t)minutes * 60ULL * 1000000ULL);
    const uint64_t wakeMask = (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | (1ULL << 21) | (1ULL << 6);
    esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_HIGH);
    Serial.flush();
    delay(100);
    esp_deep_sleep_start(); // does not return; reportSleepTestIfAny() runs on the wake boot
}

// Docked user feedback: while USB is present, show the 5-segment charge bar on the
// LEDs (one segment per 20% SOC, the filling segment breathing while current flows).
// Runs in both monitor and full-run phases. Deliberately stands down for every LED
// owner with higher claim: first-boot bring-up (owns the LED legend), the learning
// discharge (uses the LEDs as its load), and transient pulseRapid event feedback
// (plays over the bar, which resumes by itself). Read-only towards the charging
// state machine — supervisor, PD negotiation, recovery and bring-up are untouched.
static void updateChargingUx()
{
    static bool wasShowingCharge = false;
    const bool showCharge = battery.isVbusPresent() &&
                            gBringupPhase == BRINGUP_INACTIVE &&
                            !gLearnDischarge &&
                            battery.hasTelemetry();
    if (showCharge)
    {
        // Spike filter: a glitched gauge read once flashed the bar full for a frame.
        // The displayed percent only follows a >20-point jump if it persists for 2.5s
        // (several 1s gauge refresh cycles); real SOC re-seeds still get through.
        static int shownPercent = -1;
        static unsigned long jumpSince = 0;
        const int fresh = (int)battery.getBatteryPercentage();
        if (shownPercent < 0 || abs(fresh - shownPercent) <= 20)
        {
            shownPercent = fresh;
            jumpSince = 0;
        }
        else if (jumpSince == 0)
        {
            jumpSince = millis();
        }
        else if (millis() - jumpSince > 2500)
        {
            shownPercent = fresh;
            jumpSince = 0;
        }
        ledController.showChargeLevel((uint8_t)shownPercent, battery.getChargingStatus());
        wasShowingCharge = true;
    }
    else if (wasShowingCharge)
    {
        wasShowingCharge = false;
        ledController.stopChargeDisplay();
        if (gSystemPhase == PHASE_MONITOR)
        {
            ledController.pulseLed(SYSTEM_MONITOR_LED); // restore monitor breathing
        }
        else
        {
            // running phase: deriveLedScene() picks the next scene on the following loop
        }
    }
}

// Drives the first-boot bring-up state machine. Called from loop() until complete.
void updateBringup()
{
    if (gBringupPhase == BRINGUP_INACTIVE)
    {
        return;
    }

    static unsigned long lastTick = 0;
    static int calAttempts = 0;
    static int provisionAttempts = 0;
    unsigned long now = millis();

    switch (gBringupPhase)
    {
    case BRINGUP_PROVISION:
        ledController.setMaxBrightness(255);
        ledController.simpleLed(BRINGUP_LED_PROVISION, 255); // solid blue
        // The gauge boots in ROM mode (0x0B) for several seconds (longer after a deep
        // discharge) and only answers at 0x55 once the cell has charged a little. While it
        // is not yet alive we WAIT INDEFINITELY — this is a charging/recovery situation
        // (USB present), not a fault. The full system is independently gated on a healthy
        // cell, so a truly dead gauge never boots the system regardless. No false failure.
        if (!battery.isGaugeAlive())
        {
            if (now - lastTick > 2000)
            {
                lastTick = now;
                ESP_LOGW(TAG, "BRINGUP: waiting for gauge to come up (charging / recovery)...");
            }
            break;
        }
        // Gauge answers at 0x55. If it is blank (e.g. begin() ran while it was still in ROM
        // mode, or a genuinely fresh gauge), provision it now — safe here because we are in
        // monitor mode (minimal load), so the provisioning DEVICE_RESET won't brown out.
        if (!battery.isGaugeProvisioned())
        {
            if (++provisionAttempts > 5)
            {
                ESP_LOGE(TAG, "BRINGUP: gauge alive but provisioning won't stick — hardware fault.");
                gBringupPhase = BRINGUP_FAILED;
                ledController.pulseLed(BRINGUP_LED_FAIL);
                lastTick = now;
                break;
            }
            ESP_LOGW(TAG, "BRINGUP: gauge unprovisioned — provisioning now (attempt %d)...", provisionAttempts);
            battery.forceGaugeProvisioning();
            battery.update();
            lastTick = now;
            break; // re-check next tick (gauge may briefly re-enter ROM mode after the reset)
        }
        // Gauge is alive and provisioned. Ensure CUV latches until charge so the cell can't
        // over-discharge by CUV auto-recovery (non-destructive DF byte write, no reset).
        battery.ensureGaugeCuvLatch();
        if (battery.isCurrentSenseCalibrated())
        {
            ESP_LOGI(TAG, "BRINGUP: gauge already provisioned + current-calibrated — finishing.");
            gBringupPhase = BRINGUP_COMPLETE;
            break;
        }
        // Fresh board: current calibration is still needed. It resets the gauge, so it runs
        // here in monitor mode (minimal load, before WiFi/BLE) and only while charging.
        ESP_LOGI(TAG, "BRINGUP: gauge provisioned; current calibration required.");
        battery.setChargeCurrent(300); // safe while the gauge is still uncalibrated
        calAttempts = 0;
        lastTick = now;
        gBringupPhase = BRINGUP_WAIT_CHARGE;
        ledController.pulseLed(BRINGUP_LED_WAIT); // amber pulse = needs charge current
        ESP_LOGW(TAG, "BRINGUP: waiting for charge current — ensure USB-C power, cell not full.");
        break;

    case BRINGUP_WAIT_CHARGE:
        if (now - lastTick < 1500)
        {
            break;
        }
        lastTick = now;
        ledController.pulseLed(BRINGUP_LED_WAIT); // re-assert amber
        if (battery.getChargingStatus())
        {
            gBringupPhase = BRINGUP_CALIBRATE;
        }
        else
        {
            ESP_LOGW(TAG, "BRINGUP: waiting for charge — connect USB-C power.");
        }
        break;

    case BRINGUP_CALIBRATE:
        ledController.simpleLed(BRINGUP_LED_CALIBRATE, 255); // solid cyan
        ESP_LOGI(TAG, "BRINGUP: calibrating current sense vs charger IBAT...");
        if (battery.calibrateCurrentSense())
        {
            gBringupPhase = BRINGUP_COMPLETE;
        }
        else if (++calAttempts >= 5)
        {
            ESP_LOGE(TAG, "BRINGUP: current calibration failed repeatedly.");
            gBringupPhase = BRINGUP_FAILED;
            ledController.pulseLed(BRINGUP_LED_FAIL);
            lastTick = now;
        }
        else
        {
            ESP_LOGW(TAG, "BRINGUP: calibration not ready (current not steady), waiting...");
            gBringupPhase = BRINGUP_WAIT_CHARGE;
            ledController.pulseLed(BRINGUP_LED_WAIT);
            lastTick = now;
        }
        break;

    case BRINGUP_COMPLETE:
        config.storeInt(BRINGUP_NVS_KEY, 1);
        battery.setChargeCurrent(1000); // calibrated now -> safe to raise
        sleepController.setInactivityTimeout(30000); // restore normal sleep
        sleepController.resetActivity();
        ledController.setMaxBrightness(255);
        ledController.simpleLed(BRINGUP_LED_SUCCESS, 255); // solid green
        ESP_LOGI(TAG, "BRINGUP: COMPLETE — gauge provisioned + current calibrated. Starting system.");
        gBringupPhase = BRINGUP_INACTIVE;
        break;

    case BRINGUP_FAILED:
        // Reason was logged once at the transition. Just keep the red LED pulsing; don't
        // spam the log. Full system is still allowed to start (bring-up is terminal).
        if (now - lastTick > 5000)
        {
            lastTick = now;
            ledController.pulseLed(BRINGUP_LED_FAIL);
        }
        break;

    default:
        break;
    }
}

// Heap monitoring constants
const size_t CRITICAL_HEAP_THRESHOLD = 15000; // 15KB critical threshold
const size_t WARNING_HEAP_THRESHOLD = 25000;  // 25KB warning threshold

String getDeviceEfuseMacDecimal();
void handleChatMessage(const String &message);

// --- Backend endpoints (production by default; overridable in NVS for bench testing against a
// local portal: `endpoint set <api_base> <reverb_host> <reverb_port> <tls 0|1> [app_key]`) ---
struct Endpoints
{
    String apiBase;    // e.g. https://portal.tilkietalkie.com/api
    String apiOrigin;  // apiBase without the trailing /api
    String reverbHost;
    uint16_t reverbPort;
    bool reverbTls;
    String appKey;
};
static Endpoints loadEndpoints()
{
    Endpoints ep;
    ep.apiBase = config.getString("ep_api", "https://portal.tilkietalkie.com/api");
    ep.reverbHost = config.getString("ep_rvhost", "portal.tilkietalkie.com");
    int port = config.getInt("ep_rvport", 0);
    ep.reverbPort = port > 0 ? (uint16_t)port : 443;
    ep.reverbTls = config.getInt("ep_rvtls", 1) != 0;
    ep.appKey = config.getString("ep_rvkey", "erko2001");
    ep.apiOrigin = ep.apiBase;
    if (ep.apiOrigin.endsWith("/api"))
    {
        ep.apiOrigin = ep.apiOrigin.substring(0, ep.apiOrigin.length() - 4);
    }
    return ep;
}
static void printEndpoints()
{
    Endpoints ep = loadEndpoints();
    ESP_LOGI(TAG, "API base: %s", ep.apiBase.c_str());
    ESP_LOGI(TAG, "Reverb:   %s://%s:%u (app key %s)", ep.reverbTls ? "wss" : "ws", ep.reverbHost.c_str(), ep.reverbPort, ep.appKey.c_str());
    ESP_LOGI(TAG, "%s", config.getString("ep_api", "").isEmpty() ? "(production defaults)" : "(bench override from NVS - 'endpoint clear' to revert)");
}
// The Arduino core marks a freshly booted OTA image valid inside initArduino() unless this
// weak hook says otherwise; OtaManager confirms the image itself after 60 s of uptime so that a
// crash before then still lets the bootloader fall back to the previous firmware.
extern "C" bool verifyRollbackLater()
{
    return true;
}

// ReverbClient calls this when /broadcasting/auth rejects the stored hub token (revoked
// server-side); the fresh token is persisted and shared with the API client.
static String refreshHubTokenForReverb()
{
    String fresh = requestManager.getJWTToken();
    if (fresh.length() > 0)
    {
        config.setJWTToken(fresh);
        requestManager.setAuthToken(fresh);
    }
    return fresh;
}

static void startReverb()
{
    Endpoints ep = loadEndpoints();
    const String token = config.getJWTToken();
    String deviceId = getDeviceEfuseMacDecimal();
    reverb.begin(ep.reverbHost.c_str(), ep.reverbPort, ep.appKey.c_str(), token.c_str(), deviceId.c_str(),
                 ep.reverbTls, ep.apiOrigin.c_str());
    reverb.onChatMessage(handleChatMessage);
    reverb.onAuthTokenRefresh(refreshHubTokenForReverb);
}

String getDeviceEfuseMacDecimal()
{
    char deviceId[21];
    snprintf(deviceId, sizeof(deviceId), "%llu", ESP.getEfuseMac());
    return String(deviceId);
}

String getDeviceEfuseMacHex()
{
    uint64_t mac = ESP.getEfuseMac();
    char macHex[18];
    snprintf(macHex, sizeof(macHex), "%02X:%02X:%02X:%02X:%02X:%02X",
             (uint8_t)(mac >> 40),
             (uint8_t)(mac >> 32),
             (uint8_t)(mac >> 24),
             (uint8_t)(mac >> 16),
             (uint8_t)(mac >> 8),
             (uint8_t)mac);
    return String(macHex);
}

// +++ Reverb WebSocket Callback Function +++
void handleChatMessage(const String &message)
{
    ESP_LOGI(TAG, "[REVERB] Message Received: %s", message.c_str());

    // Example action: Pulse the LED blue when a message comes in
    ledController.flash(LedController::FX_MESSAGE);

    // You can add more complex logic here, e.g., parsing the message
    // if (message == "play_sound") {
    //   audioController.play("/sounds/notification.mp3");
    // }
}

// +++ NFC Callback Functions +++

// Callback function for when figure download is complete
void onFigureDownloadComplete(const String &uid, const String &figureName, bool success, const String &error, const RequestManager::Figure &figure, bool isUpdate)
{
    const bool mounted = nfcController.isCardPresent() && nfcController.currentNFCData().uidString == uid;

    if (!success)
    {
        ESP_LOGE(TAG, "Figure %s (%s): %s", figureName.c_str(), uid.c_str(), error.c_str());
        gFigureLoadingSince = 0;
        if (mounted)
        {
            ledController.flash(LedController::FX_NO_CONTENT);
        }
        return;
    }

    // Play order: manifest contents (sorted by the backend), then custom tracks.
    std::vector<String> playlist;
    for (const auto &content : figure.contents)
    {
        for (const auto &episode : content.episodes)
        {
            for (const auto &track : episode.tracks)
            {
                playlist.push_back(track.localPath);
            }
        }
    }
    for (const auto &track : figure.customTracks)
    {
        playlist.push_back(track.localPath);
    }

    ESP_LOGI(TAG, "Figure %s (%s): %u playable track(s)%s%s", figureName.c_str(), uid.c_str(),
             (unsigned)playlist.size(), isUpdate ? " [update]" : "", mounted ? "" : " (figure no longer mounted)");

    if (!mounted || playlist.empty())
    {
        return;
    }

    gFigureLoadingSince = 0;
    const bool samePlaylist = audioController.hasPlaylist() && audioController.getPlaylistFigureUid() == uid;
    if (isUpdate && samePlaylist)
    {
        // Same UID, but is the content we announced still this figure's content? The cache-first
        // path may have started tracks from an older manifest (figure re-bound, content replaced);
        // once the backend's answer arrives, content that is not in it must not keep playing.
        const String current = audioController.getCurrentTrack();
        bool keepPlaying = false;
        if (!current.isEmpty())
        {
            for (const auto &path : playlist)
            {
                if (path == current)
                {
                    keepPlaying = true;
                    break;
                }
            }
        }
        else
        {
            for (const auto &path : playlist) // nothing playing: same figure if any track overlaps
            {
                if (audioController.playlistContains(path))
                {
                    keepPlaying = true;
                    break;
                }
            }
        }
        if (keepPlaying)
        {
            audioController.mergePlaylist(playlist, uid); // never interrupts the current track
            return;
        }
        ESP_LOGW(TAG, "Figure %s (%s): current content is not in the new manifest - restarting with the new content",
                 figureName.c_str(), uid.c_str());
    }
    else if (samePlaylist)
    {
        return; // already playing this figure (e.g. a second tap event)
    }
    audioController.setPlaylist(playlist, uid);
    audioController.play();
    ESP_LOGI(TAG, "Started playing figure '%s'", figureName.c_str());
}

// This function will be called ONLY ONCE when a new card is detected
void afterNFCRead(const NFCData &nfcData)
{
    // A figure wobbling on the pogo pins (or a child tapping it repeatedly) must not fire a
    // manifest request per bounce: the same UID within 1.5 s is one tap.
    static String lastTapUid;
    static unsigned long lastTapAt = 0;
    if (nfcData.uidString == lastTapUid && millis() - lastTapAt < 1500)
    {
        ESP_LOGI(TAG, "Tap of %s ignored (debounce)", nfcData.uidString.c_str());
        return;
    }
    lastTapUid = nfcData.uidString;
    lastTapAt = millis();
    ESP_LOGI(TAG, "=== Hook: afterNFCRead ===");
    ESP_LOGI(TAG, "Card UID: %s", nfcData.uidString.c_str());
    ESP_LOGI(TAG, "Timestamp: %lu", nfcData.timestamp);

    // Example: Play a sound and turn the LED green
    // audioController.play("/sounds/nfc_success.mp3");
    ledController.flash(LedController::FX_ACK);
    gFigureLoadingSince = millis(); // amber chase until the first track plays
    // we need to check if the figure tracks are downloaded and they exist
    // we need to send get request with bearer token to the url :https://portal.tilkietalkie.com/api/units/{nfc_uid}
    requestManager.getCheckFigureTracks(nfcData.uidString);

    ESP_LOGI(TAG, "==========================");
}

// This function will be called when the reed switch is deactivated
// AFTER a card was successfully read in that session.
void afterDetachNFC()
{
    ESP_LOGI(TAG, "=== Hook: afterDetachNFC ===");
    ESP_LOGI(TAG, "NFC session has ended.");

    // Stop audio and clear the playlist
    audioController.stop();
    audioController.clearPlaylist();
    gFigureLoadingSince = 0; // removing the figure is normal: no red, the scene just returns to idle

    ESP_LOGI(TAG, "Playlist cleared due to figure removal.");
    ESP_LOGI(TAG, "==========================");
}

// Dead-cell recovery. ROOT CAUSE of the recovery bootloop (2026-07-08, nailed by the
// observation that the device is rock-stable while FLASHING — i.e. whenever CE floats
// and charging is OFF): attempting to charge a sub-CUV pack makes the BQ25792 fault
// periodically (VBAT OVP into the near-open cell) and restart its converter; during
// those ms-long faults nothing feeds SYS, and the awake ESP's ~50mA drains the SYS
// capacitors (~1.7V/ms) below U34's UVLO -> hard POR. No awake-firmware trick survives
// an actual rail collapse (BOD-off didn't). But the LOAD is ours: asleep, total SYS
// draw is ~2mA and the same converter faults barely dent SYS. So the recovery keeps
// charging ENABLED and parks the ESP in deep sleep, with CE latched low through the
// sleep via the global digital-pad hold — its flash/PSRAM standby cost is irrelevant
// here because this path only ever runs on USB power. Wakes each minute; every wake is
// a fresh boot in which battery.begin() releases and re-asserts CE seamlessly, then
// this gate either sleeps again or lets the normal boot continue. The charger STAT LED
// (red) staying lit through the sleep gaps is the visible proof of progress.
// NOTE: no setCpuFrequencyMhz() here — EVER, anywhere in this firmware: octal PSRAM at
// 80MHz uses boot-time MSPI timing calibration; dynamic frequency switching crashes it.
static void runDeadCellRecoveryHold()
{
    if (!battery.isVbusPresent())
    {
        return;
    }
    const float v = battery.getBatteryVoltage();
    if (v >= DEAD_CELL_RECOVERY_ENTER_V)
    {
        return;
    }

    esp_brownout_disable(); // don't let a survivable dip reset us before we reach sleep

    // Stop the fault clock FIRST: the SYS-collapsing charger faults only occur while
    // charge is ENABLED, and begin() enabled it ~1.2s ago — the observed fault arrives
    // ~1.2-1.8s after enable, which is why every previous hold variant lost the race to
    // sleep (the absent NFC module's ~0.6s of I2C retry timeouts sealed it last time —
    // no PN532 power-down here, its ~1mA is irrelevant on USB). With charging paused
    // the rail is provably rock-stable (the flashing observation), so the rest of this
    // function is under no time pressure. Charging is re-enabled as the very last
    // instruction before deep sleep: the first fault can only ever land on a sleeping
    // ESP (~2mA SYS load), which rides it through.
    battery.setChargeEnabled(false);

    ESP_LOGW(TAG, "DEAD-CELL RECOVERY: %.3fV on USB — charging in deep sleep (STAT LED red), re-check in 60s.", v);
    battery.setChargeCurrent(300); // gentle: a weak/high-ESR cell must not overshoot VBAT OVP

    battery.prepareForDeepSleep(); // asserts CE low + gpio_hold_en on GPIO44
    gpio_deep_sleep_hold_en();     // digital pads (incl. GPIO44/CE) stay held through this sleep
    // Clear any VBUS wake stub left in RTC memory by a previous battery sleep: the pad
    // hold above freezes GPIO1/2, the stub's bit-banged I2C would fail, and it would
    // then re-sleep forever instead of letting these recovery re-check boots happen.
    esp_set_deep_sleep_wake_stub(NULL);
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    Serial.flush();
    delay(50);
    battery.setChargeEnabled(true); // last action awake — fault clock restarts as we sleep
    esp_deep_sleep_start();         // does not return; wakes into a fresh boot -> re-evaluates here
}

// Keeps the charger healthy whenever USB is present, in EVERY phase, and implements the
// battery-side half of dynamic charging (the input side is silicon: ICO probes the
// source's real capability, IINDPM/VINDPM throttle charge current when input headroom
// runs out). Battery datasheet (4000mAh LFP): 0.2C=800mA recommended, 0.5C=2000mA
// "Rapid", charge window 0-60°C. Tiers: 300mA weak-cell OVP guard below 3.05V, 0.5C
// above 3.15V, derated to 0.2C near the temperature edges, suspended outside the
// window. A PD source's declared current budget (CH224Q reg 0x50) is applied as the
// input limit; non-PD sources rely on ICO. Also kicks the charger out of latched fault
// states ("Not charging" with a starving cell). Skipped while first-boot bring-up owns
// the charge current (300mA until current-sense calibration) and during the learning
// discharge (HIZ intentional there).
static void superviseChargerOnUsb()
{
    constexpr uint16_t kFastChargeCurrentMa = 1000;
    static uint8_t pdBudgetChecksLeft = 0;
    static unsigned long lastPdBudgetCheck = 0;
    static unsigned long lastChargeKick = 0;
    static uint16_t requestedChargeCurrentMa = 0;
    static bool wasVbusPresent = false;
    static bool chargeSuspendedByTemp = false;
    static bool tempDerated = false;

    if (!battery.isVbusPresent())
    {
        if (wasVbusPresent)
        {
            // Session ended: drop any PD-budget input limit and re-enable ICO so the
            // next (possibly weaker, non-PD) source starts from default + adaptation.
            wasVbusPresent = false;
            battery.setInputCurrentLimit(0);
            battery.setIcoEnabled(true);
        }
        return;
    }
    if (!wasVbusPresent)
    {
        // New VBUS session: give the CH224Q a few polls to finish its handshake.
        wasVbusPresent = true;
        pdBudgetChecksLeft = 5;
        lastPdBudgetCheck = 0;
    }

    if (gBringupPhase != BRINGUP_INACTIVE || gLearnDischarge || !battery.hasTelemetry())
    {
        return;
    }

    const float v = battery.getBatteryVoltage();
    const float t = battery.getTemperatureC();

    // Hard temperature window (datasheet: charge 0..60°C), 2°C re-entry hysteresis.
    if (chargeSuspendedByTemp)
    {
        if (t >= 2.0f && t <= 58.0f)
        {
            chargeSuspendedByTemp = false;
            battery.setChargeEnabled(true);
            ESP_LOGW(TAG, "CHARGER: %.1fC back inside the charge window — charging re-enabled.", t);
        }
        else
        {
            return; // stay suspended; no tier management or kicking
        }
    }
    else if (t < 0.0f || t > 60.0f)
    {
        chargeSuspendedByTemp = true;
        battery.setChargeEnabled(false);
        ESP_LOGW(TAG, "CHARGER: %.1fC outside the 0-60C charge window — charging suspended.", t);
        return;
    }

    // Derate to the 0.2C recommended rate near the temperature edges (hysteresis so a
    // hovering reading doesn't flap the tier).
    if (tempDerated)
    {
        if (t >= 7.0f && t <= 43.0f)
        {
            tempDerated = false;
        }
    }
    else if (t < 5.0f || t > 45.0f)
    {
        tempDerated = true;
    }

    // Battery-side ceiling with hysteresis on the voltage boundary (3.05/3.15 keeps
    // the previous tier in between).
    uint16_t wanted = requestedChargeCurrentMa;
    if (v < 3.05f)
    {
        wanted = 300;  // weak/high-ESR cell: the BAT node must not overshoot VBAT OVP
    }
    else if (v > 3.15f)
    {
        // Cell allows 0.5C (2 A) but the 1.0 mm-pitch single-contact cell connector (CN33)
        // and the ~100 mOhm cell-to-PACK path do not: the charger was in CV at 1.4 A anyway.
        wanted = kFastChargeCurrentMa;
    }
    if (wanted == 0)
    {
        wanted = 300;
    }
    if (tempDerated && wanted > 800)
    {
        wanted = 800;  // datasheet 0.2C recommended rate near the temperature edges
    }
    if (wanted != requestedChargeCurrentMa && battery.setChargeCurrent(wanted))
    {
        ESP_LOGI(TAG, "CHARGER: charge current -> %umA (%.3fV, %.1fC)", wanted, v, t);
        requestedChargeCurrentMa = wanted;
    }

    // Apply a PD source's declared current budget as the input limit (deterministic,
    // straight from the source's contract). Non-PD or unreadable: default + ICO.
    if (pdBudgetChecksLeft > 0 && millis() - lastPdBudgetCheck > 2000)
    {
        lastPdBudgetCheck = millis();
        pdBudgetChecksLeft--;
        uint8_t pdStatus = 0;
        uint16_t grantedMa = 0;
        if (battery.readUsbPdStatus(pdStatus, grantedMa) && (pdStatus & 0x08) && grantedMa >= 500)
        {
            const uint16_t limit = grantedMa < 3000 ? grantedMa : 3000;
            // The PD contract IS the input budget: ICO must not "re-detect" it (with
            // the charger's D+/D- unconnected it guesses a legacy ~1.5A and overwrites
            // IINDPM — observed under-running a valid 3A contract).
            if (battery.setInputCurrentLimit(limit) && battery.setIcoEnabled(false))
            {
                ESP_LOGI(TAG, "CHARGER: PD source grants %umA — input limit %umA, ICO off.",
                         grantedMa, limit);
            }
            pdBudgetChecksLeft = 0;
        }
    }

    // Never "kick" after Charge Done: toggling EN_CHG there starts a fresh CC/CV cycle every
    // 10 s, so the cell never rests at full (which the gauge needs to learn capacity).
    if (!battery.getChargingStatus() && !battery.isChargeDone() && millis() - lastChargeKick > 10000)
    {
        lastChargeKick = millis();
        ESP_LOGW(TAG, "CHARGER: USB present but not charging — clearing faults + kicking charger.");
        battery.clearChargerFaultHistory();
        battery.restartChargeCycle();
    }
}

void setup()
{
    if (DEBUG)
    {
        Serial.setTxBufferSize(4096); // absorb boot-time bursts so lines are not dropped
        Serial.begin(115200);
        Serial.setTxTimeoutMs(30); // a host that is not draining the USB console must not stall the loop for long
        delay(1000); // Give serial time to initialize
    }
    ESP_LOGI(TAG, "=== TilkieTalkie Board Tester ===");
    ESP_LOGI(TAG, "Initializing system...");
    Diag::logBootInfo(); // reset reason, heap, stored core dump

    // Initialize battery management FIRST — its I2C bus is independent of the 4V5 rail.
    // This restores the gauge protection FETs so the cell is connected and can buffer
    // the inrush/transient current from WiFi/BLE/Reverb. With the FETs open the cell is
    // isolated, the board runs unbuffered off USB, and it browns out during bring-up.
    battery.begin();

    // If this boot is the timer wake of a sleep-current measurement, print its result.
    reportSleepTestIfAny();

    // Deeply discharged cell on USB: charge at minimal load instead of boot-looping.
    // Does not return until the cell recovers (restarts) or USB is pulled (ship mode).
    runDeadCellRecoveryHold();

    // Enable the shared 4V5 peripheral rail on GPIO4 before SD/NFC/audio/LED init.
    ESP_LOGI(TAG, "Enabling peripheral power...");
    pinMode(4, OUTPUT);
    digitalWrite(4, HIGH); // Enable power to peripherals
    delay(500);            // Give peripherals time to power up

    // First-boot bring-up: if this board has never been provisioned + current-calibrated,
    // run the auto bring-up (LED-guided) and hold the charge current at a safe 300mA until
    // the gauge current sense is calibrated (an uncalibrated gauge over-reads and trips OCC).
    const bool bringupDone = config.getInt(BRINGUP_NVS_KEY, 0) != 0;
    if (!bringupDone)
    {
        battery.setChargeCurrent(300);
        gBringupPhase = BRINGUP_PROVISION;
        ESP_LOGW(TAG, "FIRST BOOT: gauge bring-up required — running auto provision+calibration.");
        ESP_LOGW(TAG, "Watch the LED (blue=provision, amber=needs charge, cyan=calibrate, green=done).");
    }

    // --- Minimal subsystems only: battery (above) + LED + sleep. The heavy subsystems
    // (SD, WiFi, audio, NFC, buttons) are deferred to startFullSystem() until the cell can
    // support them, so a depleted board stays in battery-monitor mode and charges instead
    // of brown-out looping at WiFi bring-up. ---
    ESP_LOGI(TAG, "Initializing LED Controller...");
    ledController.begin();
    OtaManager::getInstance().begin();

    ESP_LOGI(TAG, "Initializing Sleep Controller...");
    sleepController.begin();
    // Inactivity timeout (disabled during first-boot bring-up so it stays awake to
    // provision + calibrate and survive gauge resets).
    sleepController.setInactivityTimeout(gBringupPhase == BRINGUP_INACTIVE ? (5 * 60 * 1000) : 0xFFFFFFFF);
    sleepController.onSleep([]()
                            {
                                ESP_LOGI(TAG, "Device is about to enter deep sleep...");
                                ledController.setScene(LedController::SCENE_SLEEPING); // fade out
                                delay(900);
                                // The PN532 sits on the always-on 3.3V rail with RSTPD_N pulled
                                // high in hardware, so GPIO4 can't de-power it. Without this
                                // command it idles ~40mA through deep sleep (5-day depletion
                                // incident) — software power-down is the only off switch.
                                nfcController.powerDown();
                                // Park the NFC I2C lines high and latch them through deep sleep.
                                // The pads get isolated in sleep and the pullups are internal-only,
                                // so the bus would float — which the powered PN532 reads as endless
                                // phantom traffic (its wake source is I2C activity), defeating the
                                // power-down. Idle-high bus keeps it asleep. Released in
                                // NfcController::begin()/powerDown().
                                pinMode(NFC_SDA_PIN, OUTPUT);
                                digitalWrite(NFC_SDA_PIN, HIGH);
                                gpio_hold_en((gpio_num_t)NFC_SDA_PIN);
                                pinMode(NFC_SCL_PIN, OUTPUT);
                                digitalWrite(NFC_SCL_PIN, HIGH);
                                gpio_hold_en((gpio_num_t)NFC_SCL_PIN);
                                battery.prepareForDeepSleep();
                                // Keep charging through deep sleep when docked: CE (GPIO44) is a
                                // digital pad, so it only stays low across deep sleep if the global
                                // pad hold is armed — without it CE floats up the 2MΩ pull-up and
                                // the charger STOPS the moment the ESP sleeps (observed: STAT LED
                                // going dark on sleep entry). The hold's flash/PSRAM standby cost
                                // (~13mA) comes out of VBUS here, not the cell. On battery we skip
                                // it: CE is irrelevant without an adapter, and the lower sleep
                                // floor matters.
                                if (battery.isVbusPresent())
                                {
                                    gpio_deep_sleep_hold_en();
                                }
                                else
                                {
                                    // Battery sleep: no pad hold (see above) — instead arm the
                                    // VBUS wake stub, which polls the charger from RTC memory
                                    // every ~20s (~µA average) and only boots when USB appears,
                                    // so a docked sleeping toy starts charging within seconds.
                                    // Never armed together with the hold: frozen pads would
                                    // break the stub's bit-banged I2C.
                                    wakeStubVbusArm();
                                }
                            });

    // Decide whether to start the full system now. Defer (stay in battery-monitor mode) if
    // either (a) the cell can't yet support the heavy load, or (b) first-boot bring-up is
    // still pending — bring-up's calibration resets the gauge (briefly opening the FETs),
    // which must happen at minimal load, before WiFi/BLE come up.
    battery.update();
    if (gBringupPhase != BRINGUP_INACTIVE)
    {
        gSystemPhase = PHASE_MONITOR;
        ledController.pulseLed(SYSTEM_MONITOR_LED);
        ESP_LOGW(TAG, "Bring-up pending — deferring full system until it completes.");
    }
    else if (battery.hasTelemetry() && battery.getBatteryVoltage() >= SYSTEM_START_IMMEDIATE_V)
    {
        // Clearly healthy cell (on or above the LFP plateau): start right away. Anything
        // lower must prove itself in monitor mode first (sustained-voltage gate in loop).
        startFullSystem();
    }
    else
    {
        gSystemPhase = PHASE_MONITOR;
        ledController.pulseLed(SYSTEM_MONITOR_LED); // blue breathing = warming up / charging
        ESP_LOGW(TAG, "Battery %.3fV: below the %.2fV instant-start — monitoring (full system once >=%.2fV holds for %lus).",
                 battery.hasTelemetry() ? battery.getBatteryVoltage() : 0.0f,
                 SYSTEM_START_IMMEDIATE_V, SYSTEM_START_V, SYSTEM_START_HOLD_MS / 1000UL);
    }
}

// Brings up the heavy subsystems (SD, WiFi, audio, Reverb, NFC, buttons). Called once the
// battery can support them — immediately from setup() on a healthy cell, or later from
// loop() once a depleted cell has charged back above SYSTEM_START_V.
void startFullSystem()
{
    if (gSystemPhase == PHASE_RUNNING)
    {
        return; // already started
    }
    gSystemPhase = PHASE_RUNNING;
    gFullSystemStartedAt = millis();
    ESP_LOGI(TAG, "Battery ready (%.3fV) — starting full system.",
             battery.hasTelemetry() ? battery.getBatteryVoltage() : 0.0f);

    if (!fileManager.begin())
    {
        ESP_LOGW(TAG, "File Manager initialization failed!");
        ESP_LOGW(TAG, "SD card functionality will not be available.");
    }
    // Set up figure download complete callback (before WiFi connection)
    requestManager.setFigureDownloadCompleteCallback(onFigureDownloadComplete);

    // Initialize WiFi provisioning (the battery is verified able to buffer RF inrush).
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifiProv.begin();

    // Initialize audio controller
    if (!audioController.begin())
    {
        ESP_LOGW(TAG, "Audio Controller initialization failed!");
        ESP_LOGW(TAG, "Audio functionality will not be available.");
    }

    // --- Initialize Reverb Client ---
    ESP_LOGI(TAG, "Initializing Reverb WebSocket Client...");

    // Wait for WiFi to connect before starting Reverb client
    unsigned long wifi_timeout = millis() + 5000; // 5 second timeout
    while (!WiFi.isConnected() && millis() < wifi_timeout)
    {
        delay(500);
        ESP_LOGI(TAG, ".");
    }
    ESP_LOGI(TAG, "");

    if (WiFi.isConnected())
    {
        ESP_LOGI(TAG, "WiFi is connected. Initializing network services...");

        // Apply the configured API base (production unless overridden for the bench).
        requestManager.setBaseUrl(loadEndpoints().apiBase);

        // Initialize request manager (needs WiFi)
        if (!requestManager.begin())
        {
            ESP_LOGW(TAG, "Request Manager initialization failed!");
            ESP_LOGW(TAG, "API functionality may be limited.");
        }
        else
        {
            ESP_LOGI(TAG, "Request Manager initialized successfully");
        }

        startReverb();
    }
    else
    {
        ESP_LOGW(TAG, "WiFi connection timed out.");
        ESP_LOGW(TAG, "Network services (API, Reverb) will not be started.");
    }

    // --- Initialize NFC Controller ---
    ESP_LOGI(TAG, "Initializing NFC Controller...");

    // If network is available, wait for Reverb to connect before proceeding
    if (WiFi.isConnected())
    {
        ESP_LOGI(TAG, "Network available, waiting for Reverb to connect...");
        unsigned long reverbTimeout = millis() + 10000; // 10 second timeout
        while (!reverb.isConnected() && millis() < reverbTimeout)
        {
            reverb.update(); // Keep updating Reverb to help it connect
            delay(100);
        }

        if (reverb.isConnected())
        {
            ESP_LOGI(TAG, "Reverb connected successfully!");
            OtaManager::getInstance().markBootHealthy();
        }
        else
        {
            ESP_LOGW(TAG, "Reverb connection timeout, proceeding anyway");
        }
    }

    if (nfcController.begin())
    {
        ESP_LOGI(TAG, "NFC Controller initialized successfully!");

        // Set up the new NFC callbacks
        nfcController.setAfterNFCReadCallback(afterNFCRead);
        nfcController.setAfterDetachNFCCallback(afterDetachNFC);

        ESP_LOGI(TAG, "NFC callbacks configured.");
    }
    else
    {
        ESP_LOGE(TAG, "FATAL: NFC Controller initialization failed!");
        ESP_LOGE(TAG, "NFC functionality will not be available.");
        // Handle failure, maybe by pulsing an error color
        gSystemError = true; // deriveLedScene() shows the steady red error scene
    }

    // Initialize other modules here
    // e.g., sensors, etc.

    // Initialize Button Controller
    ESP_LOGI(TAG, "Initializing Button Controller...");
    buttonController.begin();

    // Set up button callbacks
    buttonController.onSingleClick([](ButtonController::ButtonId button)
                                   {
        // Reset sleep timer on any button activity
        sleepController.resetActivity();
        
        ESP_LOGI(TAG, "Single click on button %d", button + 1);
        switch(button) {
            case ButtonController::BUTTON_1:
                    ESP_LOGI(TAG, "Button 1: Volume up");
                    audioController.volumeUp(); ledController.flash(LedController::FX_VOLUME, audioController.getCurrentVolume());
                break;
            case ButtonController::BUTTON_2:
                ESP_LOGI(TAG, "Button 2: Next track");
                audioController.nextTrack(); ledController.flash(LedController::FX_NEXT);
                break;
            case ButtonController::BUTTON_3:
                ESP_LOGI(TAG, "Button 3: Previous track");
                audioController.prevTrack(); ledController.flash(LedController::FX_PREV);
                break;
            case ButtonController::BUTTON_4:
                    ESP_LOGI(TAG, "Button 4: Volume down");
                    audioController.volumeDown(); ledController.flash(LedController::FX_VOLUME, audioController.getCurrentVolume());
                break;
        } });

    buttonController.onHoldStart([](ButtonController::ButtonId button, unsigned long duration)
                                 {
        // Reset sleep timer on button hold
        sleepController.resetActivity();
        
        ESP_LOGI(TAG, "Hold started on button %d (duration: %lu ms)", button + 1, duration);
            if (button == ButtonController::BUTTON_1 || button == ButtonController::BUTTON_4) {
                ESP_LOGI(TAG, "Starting volume %s", (button == ButtonController::BUTTON_1) ? "up" : "down");
        } });

    buttonController.onHoldContinuous([](ButtonController::ButtonId button, unsigned long duration)
                                      {
            if (button == ButtonController::BUTTON_1) {
            ESP_LOGI(TAG, "Volume up (held for %lu ms)", duration);
                audioController.volumeUp(); ledController.flash(LedController::FX_VOLUME, audioController.getCurrentVolume());
        } else if (button == ButtonController::BUTTON_4) {
            ESP_LOGI(TAG, "Volume down (held for %lu ms)", duration);
                audioController.volumeDown(); ledController.flash(LedController::FX_VOLUME, audioController.getCurrentVolume());
        } });

    buttonController.onHoldEnd([](ButtonController::ButtonId button, unsigned long duration)
                               {
                                   ESP_LOGI(TAG, "Hold ended on button %d (total duration: %lu ms)", button + 1, duration);
                               });

    buttonController.onComboHold([]()
                                 {
                                         ESP_LOGW(TAG, "COMBO HOLD TRIGGERED (1+4) - RESTARTING DEVICE!");
                                     ledController.flash(LedController::FX_FAIL);
                                     delay(2000);                           // Give time for LED animation
                                     ESP.restart();                         // Restart the device
                                 });

    buttonController.onComboHold2([]()
                                  {
                                          ESP_LOGW(TAG, "COMBO HOLD TRIGGERED (2+3) - RESETTING WIFI & RESTARTING!");
                                      ledController.flash(LedController::FX_RESET);
                                      delay(1000);                           // Give time for LED animation
                                      wifiProv.reset();                      // Reset WiFi provisioning
                                      delay(1000);                           // Give time for reset
                                      ESP.restart();                         // Restart the device
                                  });

    ESP_LOGI(TAG, "Button Controller initialized successfully!");

    // rapid pulse LED to indicate system is ready
    ledController.flash(LedController::FX_SUCCESS);
    battery.startTask(); // ~190 ms of bit-banged I2C per poll leaves the loop
    OtaManager::getInstance().setIdleCheck([]() {
        // Flash writes stall every task for tens of ms: never while a story plays or a file is
        // being written, and not on a nearly empty cell.
        return !audioController.isPlaying() && !audioController.isPaused() && !nfcController.isCardPresent() &&
               !fileManager.hasActiveDownload() &&
               (battery.getChargingStatus() || battery.getBatteryPercentage() >= 40.0f);
    });
    ESP_LOGI(TAG, "Full system started.");
    // audioController.play("/sounds/12.mp3"); // Play startup sound
}
static unsigned long lastFreeCall = 0;
static unsigned long lastStackCheck = 0;

// Single owner of the persistent LED scene while the full system runs. Derived from state on
// every loop iteration, so no code path can leave a stale animation behind; event feedback
// uses ledController.flash() and plays on top.
static void deriveLedScene()
{
    if (gSystemPhase != PHASE_RUNNING)
    {
        return; // monitor / bring-up drive the LED directly
    }
    if (ledController.isChargeDisplayActive())
    {
        return; // docked SOC bar (chargeux section owns it)
    }
    if (gOtaProgress >= 0)
    {
        ledController.setScene(LedController::SCENE_OTA, gOtaProgress);
        return;
    }
    if (gSystemError)
    {
        ledController.setScene(LedController::SCENE_ERROR);
        return;
    }

    const bool lowBattery = battery.hasTelemetry() && !battery.getChargingStatus() && battery.getBatteryPercentage() < 15.0f;
    ledController.setLowBattery(lowBattery);

    if (audioController.isPlaying())
    {
        ledController.setScene(LedController::SCENE_PLAYING);
    }
    else if (audioController.isPaused())
    {
        ledController.setScene(LedController::SCENE_PAUSED);
    }
    else if (gFigureLoadingSince != 0 && millis() - gFigureLoadingSince < 30000UL)
    {
        ledController.setScene(LedController::SCENE_LOADING);
    }
    else if (fileManager.hasActiveDownload())
    {
        ledController.setScene(LedController::SCENE_DOWNLOADING);
    }
    else if (!WiFi.isConnected())
    {
        if (wifiProv.isProvisioningManagerActive())
            ledController.setScene(LedController::SCENE_PROVISIONING);
        else if (millis() - gFullSystemStartedAt < 60000UL)
            ledController.setScene(LedController::SCENE_WIFI_CONNECTING);
        else
            ledController.setScene(LedController::SCENE_OFFLINE);
    }
    else if (lowBattery)
    {
        ledController.setScene(LedController::SCENE_LOW_BATTERY);
    }
    else
    {
        ledController.setScene(LedController::SCENE_OFF);
    }
}

void loop()
{
    Diag::periodicReport(10000); // loop section timing + heap every 10 s

    // Stack monitoring every 10 seconds
    unsigned long now = millis();
    if (now - lastStackCheck > 10000)
    {
        lastStackCheck = now;
        UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
        if (stackHighWaterMark < 1000)
        { // Less than 1KB remaining
            ESP_LOGW(TAG, "WARNING: Low stack space remaining: %d bytes", stackHighWaterMark * sizeof(StackType_t));
        }
    }

    // Handle serial commands
    if (Serial.available())
    {
        String command = Serial.readStringUntil('\n');
        command.trim();
        // Lowercase only the command word: URLs, paths and hex arguments are case sensitive
        // (a lowercased URL used to turn 10MB.zip into 10mb.zip).
        {
            int sp = command.indexOf(' ');
            String word = sp > 0 ? command.substring(0, sp) : command;
            word.toLowerCase();
            command = sp > 0 ? word + command.substring(sp) : word;
        }

        // Skip empty commands
        if (command.length() == 0)
        {
            return;
        }

        // help command
        if (command == "help")
        {
            ESP_LOGI(TAG, "--- Terminal Commands ---");
            ESP_LOGI(TAG, "WiFi Commands:");
            ESP_LOGI(TAG, "  qr      - Print QR code for provisioning");
            ESP_LOGI(TAG, "  reset   - Reset WiFi provisioning");
            ESP_LOGI(TAG, "  stats   - Show WiFi connection status");
            ESP_LOGI(TAG, "Reverb Commands:");
            ESP_LOGI(TAG, "  reverbstatus - Show Reverb connection status");
            ESP_LOGI(TAG, "  reverbclean  - Clean up Reverb client to free memory");
            ESP_LOGI(TAG, "  reverbstart  - Start Reverb client (needs WiFi)");
            ESP_LOGI(TAG, "  testauth     - Test stored JWT token authorization with server");
            ESP_LOGI(TAG, "System Commands:");
            ESP_LOGI(TAG, "  restart - Restart the device");
            ESP_LOGI(TAG, "  endpoint [set <api_base> <reverb_host> <port> <tls> [key] | clear] - backend endpoints (bench)");
            ESP_LOGI(TAG, "  config  - Show all configuration");
            ESP_LOGI(TAG, "  debug   - Show debug information");
            ESP_LOGI(TAG, "  efusemac - Show device efuse MAC in decimal and hex");
            ESP_LOGI(TAG, "  heap    - Show detailed heap information");
            ESP_LOGI(TAG, "  stack   - Show stack usage information");
            ESP_LOGI(TAG, "  timing  - Show loop section timing (max/worst/avg per update())");
            ESP_LOGI(TAG, "  tasks   - Show FreeRTOS task CPU usage and stacks");
            ESP_LOGI(TAG, "  coredump - Show summary of a stored crash core dump");
            ESP_LOGI(TAG, "  coredumperase - Erase the stored core dump");
            ESP_LOGI(TAG, "  factory - Factory reset (erase all data)");
            ESP_LOGI(TAG, "  speedtest - Test network download speed (AsyncTCP Module)");
            ESP_LOGI(TAG, "Battery Commands:");
            ESP_LOGI(TAG, "  battery - Show battery status");
            ESP_LOGI(TAG, "  dfread <hexaddr> [len] / dfwrite8 <hexaddr> <hexval> - gauge data flash (bench)");
            ESP_LOGI(TAG, "  battscan - Scan the battery I2C bus for responding devices");
            ESP_LOGI(TAG, "  batterycfg - Reapply 4V5 rail and charger defaults");
            ESP_LOGI(TAG, "  batteryclearflags - Clear latched BQ25792 charger/fault flags");
            ESP_LOGI(TAG, "  chargekick - Toggle BQ25792 EN_CHG off/on to restart charging");
            ESP_LOGI(TAG, "  gaugefets - Re-enable gauge FET control if it is disabled");
            ESP_LOGI(TAG, "  gauge1s - Clear BQ28 DA Configuration CC0 to force 1-cell mode");
            ESP_LOGI(TAG, "  gaugeprog - Force generic gauge provisioning");
            ESP_LOGI(TAG, "  gaugeresetlearn - Reset BQ28 learning state to a fresh relearn baseline");
            ESP_LOGI(TAG, "  learndis - Learning: DISCHARGE phase (charger HIZ on, no sleep)");
            ESP_LOGI(TAG, "  learnrest - Learning: REST phase (deep sleep for OCV reading)");
            ESP_LOGI(TAG, "  learnchg - Learning: CHARGE phase (exit HIZ, fast charge)");
            ESP_LOGI(TAG, "  sleeptest <min> [railon] [nfcawake] [hold] - measure sleep current via gauge");
            ESP_LOGI(TAG, "            (battery only; result prints on the wake boot after <min> minutes)");
            ESP_LOGI(TAG, "  pdstatus - decode CH224Q handshake (BC/QC/PD) + actual VBUS voltage");
            ESP_LOGI(TAG, "  pdvolt <5|9|12|15|20> - request a different USB-PD voltage gear");
            ESP_LOGI(TAG, "File Manager Commands:");
            ESP_LOGI(TAG, "  sdtree  - Check SD card file tree");
            ESP_LOGI(TAG, "  sdformat- Format SD card as FAT32");
            ESP_LOGI(TAG, "  deletefile <path> - Delete file from SD card");
            ESP_LOGI(TAG, "  delete  - Delete ALL required files from NVS and storage");
            ESP_LOGI(TAG, "  deletefig <uid> - Delete all files for a specific figure");
            ESP_LOGI(TAG, "  dlstats - Show download statistics");
            ESP_LOGI(TAG, "  dlqueue - Show download queue");
            ESP_LOGI(TAG, "  required- Show required files");
            ESP_LOGI(TAG, "  download <url> <path> - Download file from URL");
            ESP_LOGI(TAG, "  addfile <path> <url> - Add required file");
            ESP_LOGI(TAG, "  checkfiles - Check and download missing files");
            ESP_LOGI(TAG, "  cleanup - Clean up temporary files");
            ESP_LOGI(TAG, "Audio Commands:");
            ESP_LOGI(TAG, "  play <path> - Play mp3 file");
            ESP_LOGI(TAG, "  pause   - Pause current playback");
            ESP_LOGI(TAG, "  resume  - Resume paused playback");
            ESP_LOGI(TAG, "  stop    - Stop playback");
            ESP_LOGI(TAG, "  volup   - Volume up");
            ESP_LOGI(TAG, "  voldown - Volume down");
            ESP_LOGI(TAG, "  volume  - Show current volume");
            ESP_LOGI(TAG, "  track   - Show current track");
            ESP_LOGI(TAG, "LED Commands:");
            ESP_LOGI(TAG, "  ledon <hex> <intensity> - Turn LED on with hex color and intensity (0-255)");
            ESP_LOGI(TAG, "  ledoff  - Turn LED off");
            ESP_LOGI(TAG, "  pulse <hex> - Start pulsing LED with hex color");
            ESP_LOGI(TAG, "  rapid <hex> <count> - Rapid pulse LED for count times");
            ESP_LOGI(TAG, "NFC Commands:");
            ESP_LOGI(TAG, "  tap <uid> / untap - simulate mounting/removing a figure (bench)");
            ESP_LOGI(TAG, "  nfcstatus - Show NFC controller status");
            ESP_LOGI(TAG, "  nfcdata   - Show current NFC card data");
            ESP_LOGI(TAG, "  nfcreed   - Show reed switch status");
            ESP_LOGI(TAG, "  nfcdiag   - Run NFC diagnostics");
            ESP_LOGI(TAG, "  nfcsleep  - PN532 software power-down (bench: watch gauge current drop)");
            ESP_LOGI(TAG, "Power Commands:");
            ESP_LOGI(TAG, "  power   - Show peripheral power status");
            ESP_LOGI(TAG, "  poweron - Enable peripheral power (GPIO4 / 4V5 rail)");
            ESP_LOGI(TAG, "  poweroff- Disable peripheral power (GPIO4 / 4V5 rail)");
            ESP_LOGI(TAG, "Sleep Commands:");
            ESP_LOGI(TAG, "  sleep   - Enter deep sleep immediately");
            ESP_LOGI(TAG, "  sleepafter <ms> - Schedule sleep after specified milliseconds");
            ESP_LOGI(TAG, "  cancelsleep - Cancel scheduled sleep");
            ESP_LOGI(TAG, "  sleepstatus - Show sleep controller status");
            ESP_LOGI(TAG, "Reverb Commands:");
            ESP_LOGI(TAG, "  send <message> - Send message to Reverb API for broadcast");
            ESP_LOGI(TAG, "  wsstatus - Show WebSocket connection status");
            ESP_LOGI(TAG, "  testauth - Test stored JWT token authorization with server");
            ESP_LOGI(TAG, "Type any command for help");
            return; // Skip further processing
        }
        // WebSocket commands
        if (command.startsWith("send "))
        {
            String message = command.substring(5); // Get the text after "send "
            message.trim();
            if (message.length() > 0)
            {
                ESP_LOGI(TAG, "Sending message: '%s'", message.c_str());
                if (reverb.sendMessage(message))
                {
                    ESP_LOGI(TAG, "Message sent to API for broadcast.");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to send message.");
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: send <your message>");
            }
        }

        // WiFi related commands
        if (command == "qr" || command == "reset" || command == "stats")
        {
            wifiProv.handleCommand(command);
        }
        // Reverb related commands
        else if (command == "reverbstatus")
        {
            ESP_LOGI(TAG, "Reverb Status: %s", reverb.isConnected() ? "Connected" : "Disconnected");
            ESP_LOGI(TAG, "WiFi Status: %s", WiFi.isConnected() ? "Connected" : "Disconnected");
            ESP_LOGI(TAG, "Free heap: %d bytes", ESP.getFreeHeap());
        }
        else if (command == "reverbclean")
        {
            ESP_LOGI(TAG, "Cleaning up Reverb client...");
            reverb.cleanup();
        }
        else if (command == "reverbstart")
        {
            if (WiFi.isConnected())
            {
                ESP_LOGI(TAG, "Starting Reverb client...");

                startReverb();
            }
            else
            {
                ESP_LOGW(TAG, "Cannot start Reverb - WiFi not connected");
            }
        }
        else if (command == "efusemac" || command == "hubid")
        {
            const String deviceId = getDeviceEfuseMacDecimal();
            const String deviceMacHex = getDeviceEfuseMacHex();

            ESP_LOGI(TAG, "--- Device Identity ---");
            ESP_LOGI(TAG, "Hub UUID / efuse MAC (decimal): %s", deviceId.c_str());
            ESP_LOGI(TAG, "Base MAC (hex): %s", deviceMacHex.c_str());
            ESP_LOGI(TAG, "Backend token endpoint: /api/hubs/%s/token", deviceId.c_str());
            ESP_LOGI(TAG, "Create or bind a Hub with uuid='%s' to your user.", deviceId.c_str());
        }
        else if (command == "testauth")
        {
            ESP_LOGI(TAG, "--- Testing Authorization ---");
            String token = config.getJWTToken();

            if (token.length() == 0)
            {
                ESP_LOGE(TAG, "No JWT token stored in configuration");
                return;
            }

            if (!WiFi.isConnected())
            {
                ESP_LOGE(TAG, "WiFi not connected - cannot test authorization");
                return;
            }

            ESP_LOGI(TAG, "JWT Token found, testing with server...");
            ESP_LOGI(TAG, "Token length: %d characters", token.length());

            // Use the same method as ReverbClient for consistency
            WiFiClientSecure client;
            client.setInsecure(); // For testing only

            HTTPClient http;
            String url = "https://portal.tilkietalkie.com/api/user"; // Simple endpoint to test auth

            if (http.begin(client, url))
            {
                http.addHeader("Authorization", "Bearer " + token);
                http.addHeader("Accept", "application/json");

                ESP_LOGI(TAG, "Sending auth test request...");
                int httpCode = http.GET();

                if (httpCode == 200)
                {
                    ESP_LOGI(TAG, "Authorization successful! Token is valid.");
                    String response = http.getString();
                    ESP_LOGI(TAG, "Server response: %s", response.c_str());
                }
                else if (httpCode == 401)
                {
                    ESP_LOGE(TAG, "Authorization failed! Token is invalid or expired.");
                }
                else if (httpCode > 0)
                {
                    ESP_LOGW(TAG, "Unexpected response code: %d", httpCode);
                    String response = http.getString();
                    ESP_LOGW(TAG, "Response: %s", response.c_str());
                }
                else
                {
                    ESP_LOGE(TAG, "HTTP request failed with error: %d", httpCode);
                }

                http.end();
            }
            else
            {
                ESP_LOGE(TAG, "Failed to connect to server");
            }
        }
        else if (command == "factory" && DEBUG)
        {
            ESP_LOGW(TAG, "WARNING: Factory reset will erase ALL stored data!");
            ESP_LOGI(TAG, "Type 'yes' to confirm or any other key to cancel:");
            while (!Serial.available())
            {
                delay(100);
            }
            String confirmation = Serial.readStringUntil('\n');
            confirmation.trim();
            confirmation.toLowerCase();

            if (confirmation == "yes")
            {
                config.factoryReset();
            }
            else
            {
                ESP_LOGI(TAG, "Factory reset cancelled.");
            }
        }
        // File Manager commands
        else if (command == "sdtree")
        {
            fileManager.printFileTree();
        }
        else if (command.startsWith("cat ") && DEBUG)
        {
            // cat <path> - print the first 4 KB of a text file on the SD card (bench)
            String path = command.substring(4);
            path.trim();
            File f = SD_MMC.open(path, FILE_READ);
            if (!f)
            {
                ESP_LOGE(TAG, "cat: cannot open %s", path.c_str());
            }
            else
            {
                size_t shown = 0;
                while (f.available() && shown < 4096)
                {
                    String line = f.readStringUntil('\n');
                    shown += line.length() + 1;
                    Serial.println(line);
                }
                if (f.available()) Serial.println("... (truncated)");
                f.close();
            }
        }
        else if (command.startsWith("sdwritebench"))
        {
            // sdwritebench [chunkKB] [count]
            String args = command.substring(12);
            args.trim();
            int chunkKb = 32, count = 64;
            if (args.length() > 0)
            {
                int sp = args.indexOf(' ');
                chunkKb = (sp > 0 ? args.substring(0, sp) : args).toInt();
                if (sp > 0) count = args.substring(sp + 1).toInt();
            }
            fileManager.benchmarkSequentialWrite((size_t)chunkKb * 1024, count);
        }
        else if (command == "sdformat")
        {
            fileManager.formatSDCard();
        }
        else if (command.startsWith("download "))
        {
            // Parse download command: download <url> <path>
            int firstSpace = command.indexOf(' ');                  // Find first space (after "download")
            int secondSpace = command.indexOf(' ', firstSpace + 1); // Find second space

            if (firstSpace != -1 && secondSpace != -1)
            {
                String url = command.substring(firstSpace + 1, secondSpace);
                String path = command.substring(secondSpace + 1);

                ESP_LOGI(TAG, "Scheduling download: %s -> %s", url.c_str(), path.c_str());
                if (fileManager.scheduleDownload(url, path))
                {
                    ESP_LOGI(TAG, "Download scheduled successfully");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to schedule download");
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: download <url> <local_path>");
                ESP_LOGI(TAG, "Example: download http://example.com/audio.mp3 /audio/test.mp3");
            }
        }
        else if (command.startsWith("addfile "))
        {
            // Parse addfile command: addfile <path> <url>
            int firstSpace = command.indexOf(' ');                  // Find first space (after "addfile")
            int secondSpace = command.indexOf(' ', firstSpace + 1); // Find second space

            if (firstSpace != -1 && secondSpace != -1)
            {
                String path = command.substring(firstSpace + 1, secondSpace);
                String url = command.substring(secondSpace + 1);

                ESP_LOGI(TAG, "Adding required file: %s <- %s", path.c_str(), url.c_str());
                if (fileManager.addRequiredFile(path, url))
                {
                    ESP_LOGI(TAG, "Required file added successfully");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to add required file");
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: addfile <local_path> <url>");
                ESP_LOGI(TAG, "Example: addfile /audio/sound.mp3 http://example.com/audio.mp3");
            }
        }
        else if (command.startsWith("deletefile "))
        {
            // Parse deletefile command: deletefile <path>
            int firstSpace = command.indexOf(' ');

            if (firstSpace != -1)
            {
                String filePath = command.substring(firstSpace + 1);
                filePath.trim(); // Remove any extra whitespace

                if (filePath.isEmpty())
                {
                    ESP_LOGI(TAG, "Usage: deletefile <file_path>");
                    ESP_LOGI(TAG, "Example: deletefile /images/image.webp");
                }
                else
                {
                    ESP_LOGI(TAG, "Deleting file and removing from required list: %s", filePath.c_str());
                    if (fileManager.deleteFileAndRemoveFromRequired(filePath))
                    {
                        ESP_LOGI(TAG, "File deleted successfully");
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Failed to delete file (file may not exist)");
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: deletefile <file_path>");
                ESP_LOGI(TAG, "Example: deletefile /images/image.webp");
                ESP_LOGI(TAG, "Note: This will also remove the file from required list to prevent re-download");
            }
        }
        // Delete all required files command
        else if (command == "delete" && DEBUG)
        {
            ESP_LOGW(TAG, "WARNING: This will delete ALL required files from NVS and storage!");
            ESP_LOGI(TAG, "Are you sure? Type 'yes' to confirm:");

            // Wait for confirmation
            unsigned long confirmTimeout = millis() + 10000; // 10 second timeout
            bool confirmed = false;

            while (millis() < confirmTimeout && !confirmed)
            {
                if (Serial.available())
                {
                    String confirmation = Serial.readStringUntil('\n');
                    confirmation.trim();
                    confirmation.toLowerCase();

                    if (confirmation == "yes")
                    {
                        confirmed = true;
                        ESP_LOGI(TAG, "Confirmation received. Deleting all required files...");
                        fileManager.clearAllRequiredFiles();
                        ESP_LOGI(TAG, "All required files have been deleted from NVS and storage.");
                    }
                    else if (confirmation == "no" || confirmation.length() > 0)
                    {
                        ESP_LOGI(TAG, "Operation cancelled.");
                        break;
                    }
                }
                delay(100);
            }

            if (!confirmed && millis() >= confirmTimeout)
            {
                ESP_LOGI(TAG, "Confirmation timeout. Operation cancelled.");
            }
        }
        else if (command.startsWith("sdbench"))
        {
            fileManager.benchmarkSDCard();
        }
        // Delete figure-specific files command
        else if (command.startsWith("deletefig ") && DEBUG)
        {
            // Parse deletefig command: deletefig <figure_uid>
            int firstSpace = command.indexOf(' ');

            if (firstSpace != -1)
            {
                String figureUid = command.substring(firstSpace + 1);
                figureUid.trim(); // Remove any extra whitespace

                if (figureUid.isEmpty())
                {
                    ESP_LOGI(TAG, "Usage: deletefig <figure_uid>");
                    ESP_LOGI(TAG, "Example: deletefig c538b083-28c1-384b-ae6d-e58e1f38f1f7");
                    ESP_LOGI(TAG, "Note: This will delete all files associated with the figure");
                }
                else
                {
                    ESP_LOGI(TAG, "Looking up figure ID for UID: %s", figureUid.c_str());

                    // Try to get figure ID from UID mapping
                    String figureId = requestManager.getFigureIdFromUid(figureUid);

                    if (figureId.length() > 0)
                    {
                        ESP_LOGI(TAG, "Found figure ID: %s for UID: %s", figureId.c_str(), figureUid.c_str());
                        ESP_LOGW(TAG, "WARNING: This will delete all files for figure (UID: %s, ID: %s)", figureUid.c_str(), figureId.c_str());
                        ESP_LOGI(TAG, "Type 'yes' to confirm deletion, or 'no' to cancel:");

                        // Wait for confirmation
                        unsigned long confirmTimeout = millis() + 10000; // 10 second timeout
                        bool confirmed = false;

                        while (millis() < confirmTimeout && !confirmed)
                        {
                            if (Serial.available())
                            {
                                String confirmation = Serial.readStringUntil('\n');
                                confirmation.trim();
                                confirmation.toLowerCase();

                                if (confirmation == "yes")
                                {
                                    confirmed = true;
                                    ESP_LOGI(TAG, "Deleting all files for figure ID: %s", figureId.c_str());

                                    if (fileManager.deleteFigureFiles(figureId))
                                    {
                                        ESP_LOGI(TAG, "Successfully deleted all files for figure (UID: %s, ID: %s)", figureUid.c_str(), figureId.c_str());
                                    }
                                    else
                                    {
                                        ESP_LOGE(TAG, "Failed to delete files for figure (UID: %s, ID: %s)", figureUid.c_str(), figureId.c_str());
                                    }
                                }
                                else if (confirmation == "no" || confirmation.length() > 0)
                                {
                                    ESP_LOGI(TAG, "Operation cancelled.");
                                    break;
                                }
                            }
                            delay(100);
                        }

                        if (!confirmed && millis() >= confirmTimeout)
                        {
                            ESP_LOGI(TAG, "Confirmation timeout. Operation cancelled.");
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Figure ID not found for UID: %s", figureUid.c_str());
                        ESP_LOGI(TAG, "This could mean:");
                        ESP_LOGI(TAG, "1. The figure was never downloaded/tracked in this session");
                        ESP_LOGI(TAG, "2. The UID is incorrect");
                        ESP_LOGI(TAG, "3. You can manually delete by figure ID if you know it");

                        // List available figure directories as a hint
                        ESP_LOGI(TAG, "Available figure directories:");
                        std::vector<String> figureDirectories = fileManager.listFiles("/figures");
                        if (figureDirectories.empty())
                        {
                            ESP_LOGI(TAG, "  (No figure directories found)");
                        }
                        else
                        {
                            for (const String &figureDir : figureDirectories)
                            {
                                ESP_LOGI(TAG, "  - Figure ID: %s", figureDir.c_str());
                            }
                            ESP_LOGI(TAG, "You can use 'deletefig <figure_id>' if you know the correct figure ID.");
                        }
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: deletefig <figure_uid>");
                ESP_LOGI(TAG, "Example: deletefig c538b083-28c1-384b-ae6d-e58e1f38f1f7");
                ESP_LOGI(TAG, "Note: This will delete all files associated with the figure");
            }
        }
        // Audio commands
        else if (command.startsWith("play "))
        {
            // Parse play command: play <path>
            int firstSpace = command.indexOf(' ');

            if (firstSpace != -1)
            {
                String filePath = command.substring(firstSpace + 1);
                filePath.trim();

                if (filePath.isEmpty())
                {
                    ESP_LOGI(TAG, "Usage: play <file_path>");
                    ESP_LOGI(TAG, "Example: play /audio/song.mp3");
                }
                else
                {
                    ESP_LOGI(TAG, "Playing: %s", filePath.c_str());
                    if (audioController.play(filePath))
                    {
                        ESP_LOGI(TAG, "Playback started successfully");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Failed to start playback");
                    }
                }
            }
            else
            {
                ESP_LOGI(TAG, "Usage: play <file_path>");
                ESP_LOGI(TAG, "Example: play /audio/song.mp3");
            }
        }
        else if (command == "play")
        {
            // Play playlist or resume
            if (audioController.play())
            {
                if (audioController.hasPlaylist())
                {
                    ESP_LOGI(TAG, "Playing playlist track %d/%d",
                             audioController.getCurrentTrackIndex() + 1,
                             audioController.getPlaylistSize());
                }
                else
                {
                    ESP_LOGI(TAG, "Playback resumed");
                }
            }
            else
            {
                ESP_LOGW(TAG, "No playlist available or failed to start playback");
            }
        }
        else if (command == "pause")
        {
            if (audioController.pause())
            {
                ESP_LOGI(TAG, "Playback paused");
            }
            else
            {
                ESP_LOGW(TAG, "Nothing to pause or already paused");
            }
        }
        else if (command == "resume")
        {
            if (audioController.resume())
            {
                ESP_LOGI(TAG, "Playback resumed");
            }
            else
            {
                ESP_LOGW(TAG, "Nothing to resume or not paused");
            }
        }
        else if (command == "stop")
        {
            if (audioController.stop())
            {
                ESP_LOGI(TAG, "Playback stopped");
            }
            else
            {
                ESP_LOGW(TAG, "Nothing to stop or already stopped");
            }
        }
        else if (command == "next")
        {
            if (audioController.nextTrack())
            {
                ESP_LOGI(TAG, "Playing next track: %d/%d",
                         audioController.getCurrentTrackIndex() + 1,
                         audioController.getPlaylistSize());
            }
            else
            {
                ESP_LOGW(TAG, "No playlist available or reached end of playlist");
            }
        }
        else if (command == "prev")
        {
            if (audioController.prevTrack())
            {
                ESP_LOGI(TAG, "Playing previous track: %d/%d",
                         audioController.getCurrentTrackIndex() + 1,
                         audioController.getPlaylistSize());
            }
            else
            {
                ESP_LOGW(TAG, "No playlist available");
            }
        }
        else if (command == "playlist")
        {
            if (audioController.hasPlaylist())
            {
                ESP_LOGI(TAG, "Current playlist (Figure UID: %s):",
                         audioController.getPlaylistFigureUid().c_str());
                ESP_LOGI(TAG, "Current track: %d/%d",
                         audioController.getCurrentTrackIndex() + 1,
                         audioController.getPlaylistSize());

                // Print playlist tracks (limit to 10 for readability)
                int maxTracks = min(10, audioController.getPlaylistSize());
                for (int i = 0; i < maxTracks; i++)
                {
                    String indicator = (i == audioController.getCurrentTrackIndex()) ? " -> " : "    ";
                    ESP_LOGI(TAG, "%s%d. Track %d", indicator.c_str(), i + 1, i + 1);
                }

                if (audioController.getPlaylistSize() > 10)
                {
                    ESP_LOGI(TAG, "    ... and %d more tracks",
                             audioController.getPlaylistSize() - 10);
                }
            }
            else
            {
                ESP_LOGI(TAG, "No playlist loaded");
            }
        }
        else if (command == "volup")
        {
            if (audioController.volumeUp())
            {
                ESP_LOGI(TAG, "Volume increased to %d%%", audioController.getCurrentVolume());
            }
            else
            {
                ESP_LOGW(TAG, "Volume already at maximum");
            }
        }
        else if (command == "voldown")
        {
            if (audioController.volumeDown())
            {
                ESP_LOGI(TAG, "Volume decreased to %d%%", audioController.getCurrentVolume());
            }
            else
            {
                ESP_LOGW(TAG, "Volume already at minimum");
            }
        }
        else if (command.startsWith("volume "))
        {
            int requested = command.substring(7).toInt();
            audioController.setVolume(requested);
            ESP_LOGI(TAG, "Volume now %d%% (ceiling %d%%)", audioController.getCurrentVolume(), audioController.getVolumeCeiling());
        }
        else if (command == "volume")
        {
            ESP_LOGI(TAG, "Current volume: %d%% (ceiling %d%%)", audioController.getCurrentVolume(), audioController.getVolumeCeiling());
        }
        else if (command == "audio")
        {
            audioController.printStatus();
        }
        else if (command == "crash")
        {
            ESP_LOGE(TAG, "crash requested from the console (panic)");
            delay(50);
            abort();
        }
        else if (command == "version")
        {
            ESP_LOGI(TAG, "Firmware %s (code %u), hardware %s, IDF %s", firmwareVersion(), (unsigned)firmwareVersionCode(),
                     TT_HARDWARE_REV, esp_app_get_description()->idf_ver);
        }
        else if (command.startsWith("diag"))
        {
            String arg = command.substring(4);
            arg.trim();
            if (arg == "on") Diag::setVerbose(true);
            else if (arg == "off") Diag::setVerbose(false);
            ESP_LOGI(TAG, "Periodic diagnostics: %s", Diag::verbose() ? "verbose (section table every 10 s)" : "quiet (one line a minute)");
        }
        else if (command.startsWith("ota"))
        {
            // ota | ota check [release_id] | ota url <url> <sha256> <size> [version]
            String arg = command.substring(3);
            arg.trim();
            OtaManager &ota = OtaManager::getInstance();
            if (arg.isEmpty())
            {
                ota.printStatus();
            }
            else if (arg.startsWith("check"))
            {
                String id = arg.substring(5);
                id.trim();
                ota.checkNow(id.isEmpty() ? 0 : (uint32_t)id.toInt(), "console");
            }
            else if (arg.startsWith("url "))
            {
                String rest = arg.substring(4);
                String parts[4];
                int n = 0;
                while (n < 4 && rest.length() > 0)
                {
                    int sp = rest.indexOf(' ');
                    parts[n++] = sp > 0 ? rest.substring(0, sp) : rest;
                    rest = sp > 0 ? rest.substring(sp + 1) : "";
                    rest.trim();
                }
                if (n < 3)
                {
                    ESP_LOGW(TAG, "usage: ota url <url> <sha256> <size> [version]");
                }
                else
                {
                    OtaManager::Job job;
                    job.url = parts[0];
                    job.sha256 = parts[1];
                    job.size = (size_t)parts[2].toInt();
                    job.version = n > 3 ? parts[3] : String("bench");
                    job.releaseId = 0;
                    ota.startJob(job);
                }
            }
        }
        else if (command.startsWith("amp"))
        {
            // amp on|off|auto - speaker amplifier override for bench diagnostics
            String arg = command.substring(3);
            arg.trim();
            if (arg == "on")
                audioController.setAmpMode(AudioController::AMP_FORCE_ON);
            else if (arg == "off")
                audioController.setAmpMode(AudioController::AMP_FORCE_OFF);
            else if (arg == "auto")
                audioController.setAmpMode(AudioController::AMP_AUTO);
            audioController.printStatus();
        }
        else if (command == "track")
        {
            String track = audioController.getCurrentTrack();
            if (track.isEmpty())
            {
                ESP_LOGI(TAG, "No track currently loaded");
            }
            else
            {
                ESP_LOGI(TAG, "Current track: %s", track.c_str());
                ESP_LOGI(TAG, "Status: %s",
                         audioController.isPlaying() ? "Playing" : audioController.isPaused() ? "Paused"
                                                                                              : "Stopped");
            }
        }
        // Power control commands
        else if (command == "power")
        {
            bool powerState = digitalRead(4);
            ESP_LOGI(TAG, "Peripheral power (GPIO4): %s", powerState ? "ENABLED" : "DISABLED");
            ESP_LOGI(TAG, "Pin state: %s", powerState ? "HIGH" : "LOW");
            if (!powerState)
            {
                ESP_LOGW(TAG, "WARNING: Peripherals (SD card, etc.) will not work with power disabled!");
                ESP_LOGI(TAG, "Use 'poweron' command to enable peripheral power.");
            }
        }
        else if (command == "poweron")
        {
            ESP_LOGI(TAG, "Enabling peripheral power...");
            digitalWrite(4, HIGH);
            delay(100);
            ESP_LOGI(TAG, "Peripheral power ENABLED");
            ESP_LOGI(TAG, "You may need to reinitialize modules (restart recommended)");
        }
        else if (command == "poweroff" && DEBUG)
        {
            ESP_LOGW(TAG, "WARNING: This will disable power to SD card and other peripherals!");
            ESP_LOGI(TAG, "Type 'yes' to confirm or any other key to cancel:");
            while (!Serial.available())
            {
                delay(100);
            }
            String confirmation = Serial.readStringUntil('\n');
            confirmation.trim();
            confirmation.toLowerCase();

            if (confirmation == "yes")
            {
                digitalWrite(4, LOW);
                ESP_LOGI(TAG, "Peripheral power DISABLED");
            }
            else
            {
                ESP_LOGI(TAG, "Power-off cancelled.");
            }
        }
        // LED commands
        else if (command.startsWith("ledon"))
        {
            // Parse command: ledon <hex_color> <intensity>
            // Example: ledon FF0000 128 (red color with 128 intensity)
            String params = command.substring(5); // Remove "ledon"
            params.trim();

            int spaceIndex = params.indexOf(' ');
            if (spaceIndex > 0)
            {
                String hexStr = params.substring(0, spaceIndex);
                String intensityStr = params.substring(spaceIndex + 1);

                // Convert hex string to uint32_t
                uint32_t hexColor = strtol(hexStr.c_str(), NULL, 16);
                int intensity = intensityStr.toInt();

                ledController.simpleLed(hexColor, intensity);
                ESP_LOGI(TAG, "LED set to color: 0x%06X, intensity: %d", hexColor, intensity);
            }
            else
            {
                ESP_LOGI(TAG, "Usage: ledon <hex_color> <intensity>");
                ESP_LOGI(TAG, "Example: ledon FF0000 128 (red color with 128 intensity)");
            }
        }
        else if (command == "ledoff")
        {
            ledController.turnOff();
            ESP_LOGI(TAG, "LED turned off");
        }
        else if (command.startsWith("pulse"))
        {
            // Parse command: pulse <hex_color>
            // Example: pulse 00FF00 (green pulsing)
            String params = command.substring(5); // Remove "pulse"
            params.trim();

            if (params.length() > 0)
            {
                uint32_t hexColor = strtol(params.c_str(), NULL, 16);
                ledController.pulseLed(hexColor);
                ESP_LOGI(TAG, "LED pulsing started with color: 0x%06X", hexColor);
            }
            else
            {
                ESP_LOGI(TAG, "Usage: pulse <hex_color>");
                ESP_LOGI(TAG, "Example: pulse 00FF00 (green pulsing)");
            }
        }
        else if (command.startsWith("rapid"))
        {
            // Parse command: rapid <hex_color> <count>
            // Example: rapid 0000FF 5 (blue rapid pulse 5 times)
            String params = command.substring(5); // Remove "rapid"
            params.trim();

            int spaceIndex = params.indexOf(' ');
            if (spaceIndex > 0)
            {
                String hexStr = params.substring(0, spaceIndex);
                String countStr = params.substring(spaceIndex + 1);

                uint32_t hexColor = strtol(hexStr.c_str(), NULL, 16);
                int count = countStr.toInt();

                ledController.pulseRapid(hexColor, count);
                ESP_LOGI(TAG, "LED rapid pulse started with color: 0x%06X, count: %d", hexColor, count);
            }
            else
            {
                ESP_LOGI(TAG, "Usage: rapid <hex_color> <count>");
                ESP_LOGI(TAG, "Example: rapid 0000FF 5 (blue rapid pulse 5 times)");
            }
        }
        // NFC commands
        else if (command.startsWith("tap ") && DEBUG)
        {
            // tap <uid> - simulate mounting a figure (bench), e.g. tap 04-1F-76-D2-56-68-80
            String uid = command.substring(4);
            uid.trim();
            uid.toUpperCase();
            if (uid.isEmpty())
            {
                ESP_LOGI(TAG, "Usage: tap <nfc-uid>");
            }
            else
            {
                sleepController.resetActivity();
                nfcController.simulateCard(uid);
            }
        }
        else if (command == "untap" && DEBUG)
        {
            nfcController.clearSimulation();
        }
        else if (command == "nfcstatus")
        {
            ESP_LOGI(TAG, "--- NFC Controller Status ---");
            ESP_LOGI(TAG, "NFC Ready: %s", nfcController.isNFCReady() ? "Yes" : "No");
            ESP_LOGI(TAG, "Pogo Switch Active: %s", nfcController.isPogoSwitchActive() ? "Yes" : "No");
            ESP_LOGI(TAG, "Card Present: %s", nfcController.isCardPresent() ? "Yes" : "No");
            ESP_LOGI(TAG, "-----------------------------");
        }
        else if (command == "nfcdata")
        {
            NFCData currentCard = nfcController.currentNFCData();
            ESP_LOGI(TAG, "--- Currently Docked NFC Card ---");
            if (currentCard.isValid)
            {
                ESP_LOGI(TAG, "UID: %s", currentCard.uidString.c_str());
                ESP_LOGI(TAG, "UID Length: %d", currentCard.uidLength);
                ESP_LOGI(TAG, "Timestamp: %lu", currentCard.timestamp);
            }
            else
            {
                ESP_LOGI(TAG, "No card is currently docked.");
            }
            ESP_LOGI(TAG, "----------------------------------");
        }
        else if (command == "nfcpogo" || command == "nfcreed")
        {
            bool rawPogoState = digitalRead(POGO_SWITCH_PIN);
            ESP_LOGI(TAG, "--- Pogo Switch Status ---");
            ESP_LOGI(TAG, "Raw Pin State (GPIO6): %s", rawPogoState ? "HIGH" : "LOW");
            ESP_LOGI(TAG, "Debounced Controller State: %s", nfcController.isPogoSwitchActive() ? "Active" : "Inactive");
            ESP_LOGI(TAG, "-------------------------");
        }
        else if (command == "nfcdiag")
        {
            nfcController.diagnostics();
        }
        else if (command == "pdstatus")
        {
            // What did the CH224Q negotiate with this USB source? Decodes its status
            // register (which handshake succeeded) plus the charger's VBUS ADC (the
            // voltage actually delivered). Diagnoses source-compatibility problems
            // (e.g. power banks that refuse or drop the 9V request).
            ESP_LOGI(TAG, "--- USB-PD Sink Status (CH224Q) ---");
            ESP_LOGI(TAG, "VBUS present: %s, VBUS = %.3fV",
                     battery.isVbusPresent() ? "yes" : "no", battery.getVbusVoltage());
            uint8_t status = 0;
            uint16_t grantedMa = 0;
            if (!battery.readUsbPdStatus(status, grantedMa))
            {
                ESP_LOGW(TAG, "CH224Q not answering (it is VBUS-powered — is USB plugged?).");
            }
            else
            {
                ESP_LOGI(TAG, "Status 0x%02X: BC1.2=%d QC2=%d QC3=%d PD=%d EPR-act=%d EPR-cap=%d AVS-cap=%d",
                         status, (status >> 0) & 1, (status >> 1) & 1, (status >> 2) & 1,
                         (status >> 3) & 1, (status >> 4) & 1, (status >> 5) & 1, (status >> 6) & 1);
                if ((status >> 3) & 1)
                {
                    ESP_LOGI(TAG, "PD granted current budget: %umA", grantedMa);
                }
            }
            ESP_LOGI(TAG, "-----------------------------------");
        }
        else if (command.startsWith("pdvolt "))
        {
            // pdvolt <5|9|12|15|20> — bench experiment: change the requested PD gear.
            const long volts = command.substring(7).toInt();
            int8_t gear = -1;
            switch (volts)
            {
            case 5:  gear = 0; break;
            case 9:  gear = 1; break;
            case 12: gear = 2; break;
            case 15: gear = 3; break;
            case 20: gear = 4; break;
            }
            if (gear < 0)
            {
                ESP_LOGI(TAG, "Usage: pdvolt <5|9|12|15|20>");
            }
            else if (battery.setUsbPdVoltageGear((uint8_t)gear))
            {
                ESP_LOGI(TAG, "Requested %ldV from the source — check 'pdstatus' / VBUS in a few seconds.", volts);
            }
            else
            {
                ESP_LOGW(TAG, "CH224Q not answering (VBUS-powered — is USB plugged?).");
            }
        }
        else if (command.startsWith("sleeptest"))
        {
            // sleeptest <minutes> [railon] [nfcawake] [hold]
            long minutes = command.substring(9).toInt(); // toInt skips leading whitespace
            if (minutes <= 0)
            {
                minutes = 30;
            }
            const bool railOn = command.indexOf("railon") > 0;
            const bool nfcAwake = command.indexOf("nfcawake") > 0;
            const bool holdPads = command.indexOf(" hold") > 0;
            runSleepTest((uint32_t)minutes, railOn, nfcAwake, holdPads);
            // Only returns on a refused start (USB present / no telemetry).
        }
        else if (command == "nfcsleep")
        {
            ESP_LOGI(TAG, "Sending PN532 PowerDown — compare 'battery' current before/after (expect ~30-40mA drop).");
            nfcController.powerDown();
            ESP_LOGI(TAG, "PN532 stays down until 'restart' (or next NFC begin()).");
        }

        // Battery commands
        else if (command == "battery")
        {
            battery.refreshDiagnostics(); // the slow gauge/charger reads are no longer polled every second
            battery.printBatteryInfo();
        }
        else if (command == "battscan")
        {
            battery.printBusScan();
        }
        else if (command == "batterycfg")
        {
            bool success = battery.reconfigure(false);
            ESP_LOGI(TAG, "Battery bring-up reapply: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "batteryclearflags")
        {
            bool success = battery.clearChargerFaultHistory();
            ESP_LOGI(TAG, "Charger fault history clear: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "chargekick")
        {
            bool success = battery.restartChargeCycle();
            ESP_LOGI(TAG, "Charge cycle restart: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "gaugefets")
        {
            bool success = battery.restoreGaugeFetControl();
            ESP_LOGI(TAG, "Gauge FET control restore: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "gauge1s")
        {
            bool success = battery.setGaugeSingleCellMode();
            ESP_LOGI(TAG, "Gauge single-cell configuration: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "gaugeprog")
        {
            bool success = battery.forceGaugeProvisioning();
            ESP_LOGI(TAG, "Gauge provisioning: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        else if (command == "gaugeresetlearn")
        {
            bool success = battery.resetGaugeLearningState();
            ESP_LOGI(TAG, "Gauge learning reset: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        // gaugecalcurrent: calibrate the gauge current sense (CC Gain) against the
        // BQ25792 IBAT ADC. Must be run WHILE a steady current flows — keep USB plugged
        // and the cell charging (~300mA stopgap) so charger IBAT and gauge current are
        // both >150mA. Fixes the ~4.5x over-read that trips OCC.
        else if (command == "gaugecalcurrent")
        {
            ESP_LOGI(TAG, "Calibrating gauge current sense vs charger IBAT...");
            bool success = battery.calibrateCurrentSense();
            ESP_LOGI(TAG, "Current calibration: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        // gaugeprot: read back and decode the gauge's actual protection config (CUV
        // enable/latch/threshold, COV/OCC) to diagnose the over-discharge behaviour.
        else if (command.startsWith("dfread ") && DEBUG)
        {
            // dfread <hexaddr> [len]  - dump gauge data flash bytes (bench diagnostics)
            String args = command.substring(7);
            args.trim();
            int sp = args.indexOf(' ');
            String addrStr = sp > 0 ? args.substring(0, sp) : args;
            int len = sp > 0 ? args.substring(sp + 1).toInt() : 1;
            if (len < 1) len = 1;
            if (len > 32) len = 32;
            uint16_t addr = static_cast<uint16_t>(strtol(addrStr.c_str(), nullptr, 16));
            uint8_t buf[32] = {0};
            if (battery.readGaugeDataFlash(addr, buf, len))
            {
                String line = "DF 0x" + String(addr, HEX) + ":";
                for (int i = 0; i < len; i++)
                {
                    char b[6];
                    snprintf(b, sizeof(b), " %02x", buf[i]);
                    line += b;
                }
                if (len == 2)
                {
                    line += "  (u16 LE = " + String(static_cast<uint16_t>(buf[0] | (buf[1] << 8))) + ")";
                }
                ESP_LOGI(TAG, "%s", line.c_str());
            }
            else
            {
                ESP_LOGE(TAG, "dfread failed");
            }
        }
        else if (command.startsWith("dfwrite8 ") && DEBUG)
        {
            // dfwrite8 <hexaddr> <hexval> - write one gauge data flash byte (bench diagnostics)
            String args = command.substring(9);
            args.trim();
            int sp = args.indexOf(' ');
            if (sp <= 0)
            {
                ESP_LOGI(TAG, "Usage: dfwrite8 <hexaddr> <hexval>");
            }
            else
            {
                uint16_t addr = static_cast<uint16_t>(strtol(args.substring(0, sp).c_str(), nullptr, 16));
                uint8_t val = static_cast<uint8_t>(strtol(args.substring(sp + 1).c_str(), nullptr, 16));
                bool ok = battery.writeGaugeDataFlash8(addr, val);
                ESP_LOGI(TAG, "dfwrite8 0x%04x <- 0x%02x : %s", addr, val, ok ? "OK (verified)" : "FAILED");
            }
        }
        else if (command == "gaugeprot")
        {
            battery.printProtectionConfig();
        }
        // gaugecuvfix: enable the CUV latch (Protection Config bit1 CUV_RECOV_CHG) so the
        // cell can't over-discharge by CUV auto-recovery. Reports if the write sticks.
        else if (command == "gaugecuvfix")
        {
            battery.ensureGaugeCuvLatch();
            battery.printProtectionConfig();
        }
        // startsystem: force the full system to start even if the battery-ready gate
        // hasn't passed (bench/debug, e.g. running on USB without a healthy cell).
        else if (command == "startsystem")
        {
            ESP_LOGW(TAG, "Forcing full system start (bypassing battery-ready gate)...");
            startFullSystem();
        }
        // bringupreset: clear the first-boot bring-up flag and restart the auto
        // provision+calibration sequence (for re-testing or refurbishing a board).
        else if (command == "bringupreset")
        {
            config.storeInt(BRINGUP_NVS_KEY, 0);
            battery.setChargeCurrent(300);
            sleepController.setInactivityTimeout(0xFFFFFFFF);
            gBringupPhase = BRINGUP_PROVISION;
            ESP_LOGW(TAG, "BRINGUP flag cleared — re-running auto bring-up. Watch the LED.");
        }
        // gaugelearned: Path B - stabilize the rail (HIZ stops the charge oscillation so
        // SYS doesn't collapse and glitch I2C writes), then mark the gauge "learned"
        // (Update Status 0x06 + reseed QMax) so it computes FCC from QMax + chem Ra.
        // Pre-req: cell already charged to ~3.3V so the battery holds SYS up in HIZ.
        else if (command == "gaugelearned")
        {
            ESP_LOGI(TAG, "LEARN: stabilizing rail (HIZ) then marking gauge learned...");
            battery.setHizMode(true);
            delay(400); // let SYS settle off the battery
            bool success = battery.resetGaugeLearningState(); // writes Update Status 0x06 + QMax
            delay(100);
            battery.setHizMode(false); // restore charger input
            ESP_LOGI(TAG, "Gauge mark-learned: %s", success ? "OK" : "FAILED");
            battery.printBatteryInfo();
        }
        // --- Gauge learning-cycle helpers ---
        // learndis: DISCHARGE phase. Charger -> HIZ (ignore USB so the system runs
        // off the battery and it drains) and disable inactivity sleep so it stays
        // awake/loaded. USB stays connected for monitoring. Run until cell ~2.8-3.0V
        // and GaugingStatus shows FD, then 'learnrest'.
        else if (command == "learndis")
        {
            bool hiz = battery.setHizMode(true);
            sleepController.setInactivityTimeout(0xFFFFFFFF); // effectively disable auto-sleep
            sleepController.resetActivity();
            // Max out the LEDs (full-white, max brightness) to add ~300mA load and
            // speed up the discharge. Turned off again in 'learnchg'/sleep.
            ledController.setMaxBrightness(255);
            ledController.simpleLed(0xFFFFFF, 255);
            gLearnDischarge = true; // loop() enforces the LEARN_DISCHARGE_FLOOR_V hard floor
            ESP_LOGI(TAG, "LEARN: discharge mode %s (charger HIZ=%s, auto-sleep disabled, LEDs full)",
                     hiz ? "ON" : "FAILED", hiz ? "on" : "?");
            ESP_LOGW(TAG, "Draining cell; auto-stops at %.2fV to stay clear of CUV. Then 'learnrest'.",
                     LEARN_DISCHARGE_FLOOR_V);
        }
        // learnrest: REST phase. Keep charger in HIZ (no charge current to corrupt the
        // OCV), let the device deep-sleep so cell current drops to ~uA. The gauge
        // takes its OCV reading during the rest. Wake (button/pogo) after ~15-30 min,
        // then check 'battery' for VOK set / RDIS cleared / FCC nonzero.
        else if (command == "learnrest")
        {
            gLearnDischarge = false;
            battery.setHizMode(true); // ensure no charging during the rest/OCV
            ESP_LOGI(TAG, "LEARN: entering rest. Device will deep-sleep for OCV.");
            ESP_LOGW(TAG, "Leave it ~15-30 min, then wake (button/pogo) and run 'battery'.");
            ESP_LOGW(TAG, "Look for GaugingStatus VOK set, RDIS gone, FCC nonzero.");
            delay(500);
            sleepController.checkAndSleep(); // enter deep sleep now (OCV taken while resting)
        }
        // learnchg: CHARGE phase. Exit HIZ so the BQ25792 fast-charges to 3.6V, restore
        // normal auto-sleep. Run until ChargingStatus VCT + BatteryStatus FC (full).
        else if (command == "learnchg")
        {
            gLearnDischarge = false;
            bool hiz = battery.setHizMode(false);
            sleepController.setInactivityTimeout(30000); // restore normal 30s timeout
            sleepController.resetActivity();
            ledController.turnOff(); // drop the discharge load
            ESP_LOGI(TAG, "LEARN: charge mode (charger HIZ=%s, auto-sleep restored, LEDs off)",
                     hiz ? "off" : "FAILED-to-clear");
            ESP_LOGW(TAG, "Watch 'battery' until Charge State stays Fast charge then taper to");
            ESP_LOGW(TAG, "VCT/FC, cell ~3.6V. Then 'learnrest' again to anchor the full OCV.");
            battery.printBatteryInfo();
        }
        // System commands
        else if (command.startsWith("endpoint"))
        {
            // endpoint | endpoint set <api_base> <reverb_host> <reverb_port> <tls 0|1> [app_key] | endpoint clear
            String args = command.substring(8);
            args.trim();
            if (args.isEmpty())
            {
                printEndpoints();
            }
            else if (args == "clear")
            {
                config.deleteSetting("ep_api");
                config.deleteSetting("ep_rvhost");
                config.deleteSetting("ep_rvport");
                config.deleteSetting("ep_rvtls");
                config.deleteSetting("ep_rvkey");
                ESP_LOGI(TAG, "Endpoints reset to production defaults - 'restart' to apply");
            }
            else if (args.startsWith("set "))
            {
                String rest = args.substring(4);
                String parts[5];
                int n = 0;
                while (n < 5 && rest.length() > 0)
                {
                    int sp = rest.indexOf(' ');
                    parts[n++] = sp > 0 ? rest.substring(0, sp) : rest;
                    rest = sp > 0 ? rest.substring(sp + 1) : "";
                    rest.trim();
                }
                if (n < 4)
                {
                    ESP_LOGI(TAG, "Usage: endpoint set <api_base> <reverb_host> <reverb_port> <tls 0|1> [app_key]");
                    ESP_LOGI(TAG, "e.g.   endpoint set http://192.168.2.150:8000/api 192.168.2.150 8080 0 local-key");
                }
                else
                {
                    config.storeString("ep_api", parts[0]);
                    config.storeString("ep_rvhost", parts[1]);
                    config.storeInt("ep_rvport", parts[2].toInt());
                    config.storeInt("ep_rvtls", parts[3].toInt() ? 1 : 0);
                    if (n == 5) config.storeString("ep_rvkey", parts[4]); else config.deleteSetting("ep_rvkey");
                    printEndpoints();
                    ESP_LOGI(TAG, "'restart' to apply");
                }
            }
        }
        else if (command == "restart")
        {
            ESP_LOGW(TAG, "Restarting device...");
            delay(1000);
            ESP.restart();
        }
        else if (command == "config")
        {
            config.printAllSettings();
        }
        else if (command == "debug")
        {
            ESP_LOGI(TAG, "--- Debug Information ---");
            ESP_LOGI(TAG, "Free internal SRAM: %d bytes", ESP.getFreeHeap());
            ESP_LOGI(TAG, "Free PSRAM: %d bytes", ESP.getFreePsram());
            ESP_LOGI(TAG, "Largest free block: %d bytes", ESP.getMaxAllocHeap());
            ESP_LOGI(TAG, "Minimum free heap: %d bytes", ESP.getMinFreeHeap());
            ESP_LOGI(TAG, "Chip revision: %d", ESP.getChipRevision());
            ESP_LOGI(TAG, "SDK version: %s", ESP.getSdkVersion());
            ESP_LOGI(TAG, "WiFi mode: %d", WiFi.getMode());
            ESP_LOGI(TAG, "WiFi status: %d", WiFi.status());
            ESP_LOGI(TAG, "Battery: %s", battery.getBatteryStatusString().c_str());
            ESP_LOGI(TAG, "WiFi connected: %s", WiFi.isConnected() ? "Yes" : "No");
            ESP_LOGI(TAG, "Has WiFi credentials: %s", config.hasWiFiCredentials() ? "Yes" : "No");
            ESP_LOGI(TAG, "WiFi SSID length: %d", config.getWiFiSSID().length());
            ESP_LOGI(TAG, "WiFi Password length: %d", config.getWiFiPassword().length());

            // Check ESP32 WiFi provisioning library status
            bool provisioned = false;
            esp_err_t ret = wifi_prov_mgr_is_provisioned(&provisioned);
            ESP_LOGI(TAG, "ESP32 WiFi Library Provisioned: %s", provisioned ? "Yes" : "No");
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Provisioning check error: %s", esp_err_to_name(ret));
            }

            // Check actual WiFi config stored by ESP32
            wifi_config_t wifi_cfg;
            ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
            if (ret == ESP_OK)
            {
                String storedSSID = String((char *)wifi_cfg.sta.ssid);
                ESP_LOGI(TAG, "ESP32 Stored SSID: %s", storedSSID.length() > 0 ? storedSSID.c_str() : "(none)");
                ESP_LOGI(TAG, "ESP32 Stored Password Length: %d", strlen((char *)wifi_cfg.sta.password));
            }
            else
            {
                ESP_LOGE(TAG, "Failed to get WiFi config: %s", esp_err_to_name(ret));
            }

            ESP_LOGI(TAG, "Note: BLE is automatically managed by ESP32 provisioning library");
            config.printAllSettings();
        }
        // Add new heap command
        else if (command == "heap")
        {
            ESP_LOGI(TAG, "--- Detailed Heap Information ---");

            // Internal SRAM Heap Statistics
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Internal SRAM Heap:");
            ESP_LOGI(TAG, "  Total heap size: %d bytes", ESP.getHeapSize());
            ESP_LOGI(TAG, "  Free heap: %d bytes", ESP.getFreeHeap());
            ESP_LOGI(TAG, "  Largest free block: %d bytes", ESP.getMaxAllocHeap());
            ESP_LOGI(TAG, "  Minimum free heap since boot: %d bytes", ESP.getMinFreeHeap());

            // Calculate internal heap usage
            uint32_t usedHeap = ESP.getHeapSize() - ESP.getFreeHeap();
            float heapUsagePercent = (float)usedHeap / ESP.getHeapSize() * 100;
            ESP_LOGI(TAG, "  Used heap: %d bytes (%.1f%%)", usedHeap, heapUsagePercent);

            // Calculate internal heap fragmentation
            float heapFragmentation = 0;
            if (ESP.getFreeHeap() > 0)
            {
                heapFragmentation = (1.0 - (float)ESP.getMaxAllocHeap() / ESP.getFreeHeap()) * 100;
            }
            ESP_LOGI(TAG, "  Heap fragmentation: %.1f%%", heapFragmentation);

            // External PSRAM Statistics
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "External PSRAM:");
            ESP_LOGI(TAG, "  Total PSRAM size: %d bytes", ESP.getPsramSize());
            ESP_LOGI(TAG, "  Free PSRAM: %d bytes", ESP.getFreePsram());
            ESP_LOGI(TAG, "  Minimum free PSRAM since boot: %d bytes", ESP.getMinFreePsram());

            // Calculate PSRAM usage
            uint32_t usedPsram = ESP.getPsramSize() - ESP.getFreePsram();
            float psramUsagePercent = 0;
            if (ESP.getPsramSize() > 0)
            {
                psramUsagePercent = (float)usedPsram / ESP.getPsramSize() * 100;
            }
            ESP_LOGI(TAG, "  Used PSRAM: %d bytes (%.1f%%)", usedPsram, psramUsagePercent);

            // Overall memory status
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Combined Memory Status:");
            uint32_t totalFree = ESP.getFreeHeap() + ESP.getFreePsram();
            uint32_t totalSize = ESP.getHeapSize() + ESP.getPsramSize();
            ESP_LOGI(TAG, "  Total free memory: %d bytes", totalFree);
            ESP_LOGI(TAG, "  Total memory size: %d bytes", totalSize);

            // Memory health status
            ESP_LOGI(TAG, "");
            if (ESP.getFreeHeap() < CRITICAL_HEAP_THRESHOLD)
            {
                ESP_LOGE(TAG, "Internal Heap Status: CRITICAL - Very low memory");
            }
            else if (ESP.getFreeHeap() < WARNING_HEAP_THRESHOLD)
            {
                ESP_LOGW(TAG, "Internal Heap Status: WARNING - Low memory");
            }
            else
            {
                ESP_LOGI(TAG, "Internal Heap Status: OK - Memory levels normal");
            }

            if (ESP.getPsramSize() > 0)
            {
                if (ESP.getFreePsram() < 100000)
                { // Less than 100KB PSRAM free
                    ESP_LOGW(TAG, "PSRAM Status: WARNING - Low PSRAM");
                }
                else
                {
                    ESP_LOGI(TAG, "PSRAM Status: OK - PSRAM levels normal");
                }
            }
            else
            {
                ESP_LOGW(TAG, "PSRAM Status: No PSRAM detected or disabled");
            }

            ESP_LOGI(TAG, "----------------------------------");
        }
        else if (command == "timing")
        {
            Diag::printSectionStats(false);
        }
        else if (command == "tasks")
        {
            Diag::printTaskStats();
        }
        else if (command == "coredump")
        {
            Diag::printCoreDumpSummary();
        }
        else if (command == "coredumperase")
        {
            Diag::eraseCoreDump();
        }
        // Stack monitoring command
        else if (command == "stack")
        {
            ESP_LOGI(TAG, "--- Stack Information ---");
            UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
            size_t stackRemaining = stackHighWaterMark * sizeof(StackType_t);
            ESP_LOGI(TAG, "Stack high water mark: %d words (%d bytes)",
                     stackHighWaterMark, stackRemaining);

            // Estimate stack usage (assuming 16KB total from build flags)
            size_t totalStack = 16384; // From CONFIG_ARDUINO_LOOP_STACK_SIZE
            size_t usedStack = totalStack - stackRemaining;
            float usagePercent = (float)usedStack / totalStack * 100;

            ESP_LOGI(TAG, "Estimated stack usage: %d/%d bytes (%.1f%%)",
                     usedStack, totalStack, usagePercent);

            if (stackRemaining < 1000)
            {
                ESP_LOGE(TAG, "Status: CRITICAL - Very low stack space");
            }
            else if (stackRemaining < 2000)
            {
                ESP_LOGW(TAG, "Status: WARNING - Low stack space");
            }
            else
            {
                ESP_LOGI(TAG, "Status: OK - Stack levels normal");
            }
            ESP_LOGI(TAG, "------------------------");
        }
        // AsyncTCP speed test command
        else if (command == "speedtest")
        {
            speedTest.start();
        }
        // Sleep commands
        else if (command == "sleep")
        {
            ESP_LOGI(TAG, "Entering deep sleep immediately...");
            sleepController.checkAndSleep();
        }
        else if (command.startsWith("sleepafter "))
        {
            String timeStr = command.substring(11);
            unsigned long timeMs = timeStr.toInt();
            if (timeMs > 0)
            {
                sleepController.scheduleSleep(timeMs);
                ESP_LOGI(TAG, "Sleep scheduled after %lu ms", timeMs);
            }
            else
            {
                ESP_LOGI(TAG, "Usage: sleepafter <milliseconds>");
                ESP_LOGI(TAG, "Example: sleepafter 30000 (sleep after 30 seconds)");
            }
        }
        else if (command == "cancelsleep")
        {
            sleepController.cancelSleep();
            ESP_LOGI(TAG, "Scheduled sleep cancelled");
        }
        else if (command == "sleepstatus")
        {
            ESP_LOGI(TAG, "--- Sleep Controller Status ---");
            ESP_LOGI(TAG, "Inactivity timeout: %lu ms", sleepController.getInactivityTimeout());
            ESP_LOGI(TAG, "Was woken from sleep: %s", sleepController.wasWokenFromSleep() ? "Yes" : "No");
            if (sleepController.wasWokenFromSleep())
            {
                sleepController.printWakeupReason();
            }

            // Check sleep conditions
            FileManager &fm = FileManager::getInstance();
            AudioController &ac = AudioController::getInstance();
            ESP_LOGI(TAG, "Pending downloads: %d", fm.getPendingDownloadsCount());
            ESP_LOGI(TAG, "Audio state: %d (0=STOPPED, 1=PLAYING, 2=PAUSED)", ac.getState());
            ESP_LOGI(TAG, "Can enter sleep: %s", (fm.getPendingDownloadsCount() == 0 && ac.getState() != 1) ? "Yes" : "No");
            ESP_LOGI(TAG, "-------------------------------");
        }
        // File Manager commands that were missing
        else if (command == "dlstats")
        {
            ESP_LOGI(TAG, "%s", fileManager.getDownloadStatsString().c_str());
        }
        else if (command == "dlqueue")
        {
            fileManager.printDownloadQueue();
        }
        else if (command == "required")
        {
            fileManager.printRequiredFiles();
        }
        else if (command == "checkfiles")
        {
            ESP_LOGI(TAG, "Checking required files and scheduling missing ones for download...");
            fileManager.checkRequiredFiles();
            ESP_LOGI(TAG, "Check complete. Use 'dlqueue' to see download queue.");
        }
        else if (command == "cleanup")
        {
            ESP_LOGI(TAG, "Cleaning up temporary files...");
            fileManager.cleanupTempFiles();
            ESP_LOGI(TAG, "Cleanup complete.");
        }
        else
        {
            ESP_LOGI(TAG, "Unknown command. Type 'help' for a list of commands.");
        }
    }

    // Update battery management
    TT_TIMED("battery", battery.update());

    // Low-battery cutoff: BATTERY-ONLY. With VBUS present this must never fire — the NVDC
    // power path runs SYS from USB regardless of the cell, and force-sleeping on USB is
    // what abandoned charger recovery (the "sleep with charger attached" bootloop). Two
    // tiers on battery: at LOW_BATTERY_CUTOFF_V deep sleep (buttons still wake); at
    // SHIP_MODE_CUTOFF_V disconnect the cell entirely (ship mode, wake = USB plug-in).
    {
        static unsigned long lowBattSince = 0;
        if (!gLearnDischarge && !battery.isVbusPresent() &&
            !battery.getChargingStatus() && battery.hasTelemetry())
        {
            float v = battery.getBatteryVoltage();
            if (v > 0.5f && v <= LOW_BATTERY_CUTOFF_V) // ignore obviously bogus reads
            {
                if (lowBattSince == 0)
                {
                    lowBattSince = millis();
                }
                else if (millis() - lowBattSince >= LOW_BATTERY_CUTOFF_DEBOUNCE_MS)
                {
                    ESP_LOGW(TAG, "LOW BATTERY %.3fV (<= %.2fV), on battery — protecting the cell.",
                             v, LOW_BATTERY_CUTOFF_V);
                    ledController.setMaxBrightness(255);
                    ledController.simpleLed(0xFF0000, 255); // brief red = empty
                    delay(800);
                    ledController.turnOff();
                    // INTERIM POLICY (until the ~33mA standby phantom drain is fixed):
                    // go STRAIGHT to ship mode at the cutoff instead of deep sleep.
                    // Deep-sleeping at 3.0V with the phantom drain kills the cell within
                    // hours — asleep, nothing re-evaluates the voltage (no periodic
                    // wakes by design), so the 2.85V ship tier is never reached and the
                    // cell slides through CUV to ~0.3V (overnight incident, 2026-07-08).
                    // Cost: an empty toy doesn't respond to buttons, only to USB —
                    // acceptable "battery dead" UX. Once standby is sub-mA, restore the
                    // two-tier (deep sleep at 3.00V, ship only at 2.85V) so button-wake
                    // survives at empty.
                    ESP_LOGW(TAG, "Disconnecting cell (ship mode) — plug USB to wake.");
                    nfcController.powerDown();   // PN532 is on the always-on rail
                    battery.enterShipMode();     // on battery power this does not return
                    delay(1000);
                    sleepController.forceSleep(); // fallback if the ship-mode write failed
                }
            }
            else
            {
                lowBattSince = 0;
            }
        }
        else
        {
            lowBattSince = 0;
        }
    }

    // Battery-monitor mode: until the cell can support the full system, keep only the
    // battery + bring-up + LED alive, then start the full system once it recovers. This
    // prevents the brown-out/reboot loop a depleted cell causes during WiFi bring-up.
    // Charger supervision runs in every phase (monitor AND full-run) while USB is present.
    TT_TIMED("charger", superviseChargerOnUsb());

    if (gSystemPhase == PHASE_MONITOR)
    {
        updateBringup(); // gauge calibration (if needed) runs here at minimal load

        // Start the full system only once bring-up is terminal (done or failed) AND the
        // cell has PROVEN it can support the load: clearly-healthy voltage immediately,
        // or SYSTEM_START_V sustained for SYSTEM_START_HOLD_MS (a rested near-empty cell
        // reads above the bare threshold for a moment and then collapses under WiFi).
        const bool bringupTerminal =
            (gBringupPhase == BRINGUP_INACTIVE || gBringupPhase == BRINGUP_FAILED);
        static unsigned long readySince = 0;
        bool batteryProven = false;
        if (battery.hasTelemetry())
        {
            const float v = battery.getBatteryVoltage();
            if (v >= SYSTEM_START_IMMEDIATE_V)
            {
                batteryProven = true;
            }
            else if (v >= SYSTEM_START_V)
            {
                if (readySince == 0)
                {
                    readySince = millis();
                }
                batteryProven = (millis() - readySince) >= SYSTEM_START_HOLD_MS;
            }
            else
            {
                readySince = 0;
            }
        }
        else
        {
            readySince = 0;
        }
        if (bringupTerminal && batteryProven)
        {
            startFullSystem();
        }
        else
        {
            static unsigned long lastMonLog = 0;
            if (millis() - lastMonLog > 5000)
            {
                lastMonLog = millis();
                ESP_LOGW(TAG, "MONITOR: waiting (bringup=%d, %.3fV, charging=%d) before full start.",
                         (int)gBringupPhase, battery.hasTelemetry() ? battery.getBatteryVoltage() : 0.0f,
                         (int)battery.getChargingStatus());
            }
        }
        updateChargingUx();     // charge bar while docked (bring-up still owns its LED)
        ledController.update(); // animate the monitor / bring-up LED
        delay(5);
        return;
    }

    // Learning-cycle discharge floor: never let the controlled drain push the LFP cell
    // into the CUV protection trip. Stop the discharge at LEARN_DISCHARGE_FLOOR_V.
    if (gLearnDischarge && battery.hasTelemetry() &&
        battery.getBatteryVoltage() <= LEARN_DISCHARGE_FLOOR_V)
    {
        gLearnDischarge = false;
        battery.setHizMode(false);                   // re-enable the input
        ledController.turnOff();                      // drop the discharge load
        sleepController.setInactivityTimeout(30000);  // restore normal sleep
        sleepController.resetActivity();
        ESP_LOGW(TAG, "LEARN: discharge floor %.2fV reached at %.3fV - stopped.",
                 LEARN_DISCHARGE_FLOOR_V, battery.getBatteryVoltage());
        ESP_LOGW(TAG, "Run 'learnrest' now to take the empty-point OCV.");
    }

    // First-boot bring-up state machine (owns the LED until complete).
    updateBringup();

    // Update file manager
    TT_TIMED("files", fileManager.update());

    // Update audio controller
    TT_TIMED("audio", audioController.update());

    // Handle WiFi background reconnection (only when credentials exist but not connected)
    TT_TIMED("wifi", wifiProv.handleBackgroundReconnection());

    // Update Reverb client only if WiFi is connected
    if (WiFi.isConnected())
    {
        TT_TIMED("reverb", reverb.update());
    }

    // Update LED controller (handles pulse and pulseRapid animations)
#ifdef TT_OTA_CRASH_TEST
    // Bench only (-DTT_OTA_CRASH_TEST=<ms>): an image that dies before confirming itself must be
    // rolled back by the bootloader; one that dies later must trip the crash-loop guard.
    if (millis() > (unsigned long)(TT_OTA_CRASH_TEST))
    {
        ESP_LOGE(TAG, "TT_OTA_CRASH_TEST: aborting before the image is confirmed");
        abort();
    }
#endif
    TT_TIMED("ota", OtaManager::getInstance().update());
    gOtaProgress = OtaManager::getInstance().inProgress() ? OtaManager::getInstance().progress() : -1;
    TT_TIMED("led", deriveLedScene(); ledController.update());

    // Update Button controller (handles button state changes and callbacks)
    TT_TIMED("buttons", buttonController.update());

    // Update NFC controller — skipped during first-boot bring-up so figure events don't
    // fight the bring-up LED indication.
    if (gBringupPhase == BRINGUP_INACTIVE)
    {
        TT_TIMED("nfc", nfcController.update());
        // Deliver deferred figure completion callbacks after NFC state is refreshed.
        TT_TIMED("request", requestManager.update());
    }

    // Charge bar while docked (transient NFC/audio pulses play over it and it resumes).
    TT_TIMED("chargeux", updateChargingUx());

    // No sleeping while docked: charging is the toy's "plugged in" UX. resetActivity()
    // keeps the inactivity countdown cleared (silent no-op once clear), so after unplug
    // the full timeout starts fresh instead of sleeping instantly. Manual `sleep`
    // (forceSleep) still works, and the VBUS wake stub covers the inverse case of
    // docking a device that is already asleep on battery.
    if (battery.isVbusPresent())
    {
        sleepController.resetActivity();
    }
    else
    {
        TT_TIMED("sleep", sleepController.update());
    }

    delay(1);
}
#endif // SLEEP_PROBE_BUILD
