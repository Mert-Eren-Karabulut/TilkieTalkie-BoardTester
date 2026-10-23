#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_log.h>
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
#include "rtc_wake_stub_vbus.h"
#include "Diag.h"
#include "OtaManager.h"
#include "VoicePrompts.h"
#include "PromptPackManager.h"
#include "Faults.h"
#include "Version.h"
#include "Policy.h"
#include "SystemState.h"
#include "Bringup.h"
#include "ChargerSupervisor.h"
#include "DeadCellRecovery.h"
#include "UxScenes.h"
#include "Console.h"
#include "TimeSync.h"
#include "FactoryTest.h"

// The espidf CMake build compiles every file in src/ regardless of build_src_filter,
// so the sleep-probe build (env:sleepprobe, -DSLEEP_PROBE_BUILD) excludes this whole
// application via this guard and provides its own setup()/loop() in sleepprobe_main.cpp.
#ifndef SLEEP_PROBE_BUILD

static const char *TAG = "main";

// Bypass PSRAM test to prevent early boot issues
BYPASS_SPIRAM_TEST(true);

// Global instances (declared in SystemState.h for the modules)
NfcController &nfcController = NfcController::getInstance();
ConfigManager &config = ConfigManager::getInstance();
RequestManager &requestManager = RequestManager::getInstance(Policy::API_BASE_URL);
WiFiProvisioningManager &wifiProv = WiFiProvisioningManager::getInstance();
BatteryManager &battery = BatteryManager::getInstance();
FileManager &fileManager = FileManager::getInstance();
AudioController &audioController = AudioController::getInstance();
LedController ledController;
ReverbClient &reverb = ReverbClient::getInstance();
ButtonController &buttonController = ButtonController::getInstance();
SleepController &sleepController = SleepController::getInstance();

// Shared state (SystemState.h)
SystemPhase gSystemPhase = PHASE_MONITOR;
unsigned long gFullSystemStartedAt = 0;
unsigned long gFigureLoadingSince = 0;
bool gSystemError = false;
int gOtaProgress = -1;
bool gLearnDischarge = false;
bool gGaugeBusReleased = false;

// Network services (API client + Reverb) start when WiFi is up: in startFullSystem() on a
// provisioned boot, or later from the loop once a fresh provisioning / a late reconnect lands.
static bool gNetworkServicesStarted = false;
static unsigned long gWifiUpSince = 0;
static VoicePrompts &prompts = VoicePrompts::getInstance();
static String gDownloadAnnouncedUid;
static unsigned long lastStackCheck = 0;
// startFullSystem() ran during a factory session: WiFi/BLE wait for `ft end` (startDeferredWifi).
static bool gWifiDeferred = false;

void handleChatMessage(const String &message);

// Backend endpoints: production by default; a bench build can override them in NVS
// (`endpoint set`, Console.cpp). `endpoint clear` works in every build.
Endpoints loadEndpoints()
{
    Endpoints ep;
    ep.apiBase = config.getString("ep_api", Policy::API_BASE_URL);
    ep.reverbHost = config.getString("ep_rvhost", Policy::REVERB_HOST);
    int port = config.getInt("ep_rvport", 0);
    ep.reverbPort = port > 0 ? (uint16_t)port : Policy::REVERB_PORT;
    ep.reverbTls = config.getInt("ep_rvtls", 1) != 0;
    ep.appKey = config.getString("ep_rvkey", Policy::REVERB_APP_KEY);
    ep.apiOrigin = ep.apiBase;
    if (ep.apiOrigin.endsWith("/api"))
    {
        ep.apiOrigin = ep.apiOrigin.substring(0, ep.apiOrigin.length() - 4);
    }
    return ep;
}
// Host and port of the captive-portal probe (docs/PROTOCOL-V2.md §6): the API base's host,
// over plain HTTP. https bases use port 80 (nginx serves /storage there, like the assets); an
// http base (bench portal, `endpoint set http://<ip>:8000/api ...`) keeps its own port.
static void probeTargetFrom(const String &apiBase, String &host, uint16_t &port)
{
    String rest = apiBase;
    const bool tls = rest.startsWith("https://");
    if (rest.indexOf("://") >= 0) rest = rest.substring(rest.indexOf("://") + 3);
    int slash = rest.indexOf('/');
    String hostPort = slash >= 0 ? rest.substring(0, slash) : rest;
    int colon = hostPort.lastIndexOf(':');
    port = 80;
    host = hostPort;
    if (colon > 0)
    {
        host = hostPort.substring(0, colon);
        if (!tls) port = (uint16_t)hostPort.substring(colon + 1).toInt();
        if (port == 0) port = 80;
    }
}

// The Arduino core marks a freshly booted OTA image valid inside initArduino() unless this
// weak hook says otherwise; OtaManager confirms the image itself once the portal is reached
// (or after 10 min of uptime), so that an image that dies or cannot reach the portal before
// that still lets the bootloader fall back to the previous firmware.
extern "C" bool verifyRollbackLater()
{
    return true;
}

// ReverbClient calls this when /broadcasting/auth rejects the stored hub token (revoked
// server-side). A token renewed in the last 10 min is handed over as is (the socket held an
// older one); otherwise RequestManager refreshes it (rate-limited, persisted, shared). Fetching
// a new one on every refusal used to make the API client and the socket revoke each other.
static String refreshHubTokenForReverb()
{
    return requestManager.currentOrFreshToken();
}


void startReverb()
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
    // getEfuseMac() packs the six MAC bytes little-endian (first byte of the MAC in the low
    // byte), so the printed order starts at the low byte; the old code printed it reversed.
    uint64_t mac = ESP.getEfuseMac();
    char macHex[18];
    snprintf(macHex, sizeof(macHex), "%02X:%02X:%02X:%02X:%02X:%02X",
             (uint8_t)mac,
             (uint8_t)(mac >> 8),
             (uint8_t)(mac >> 16),
             (uint8_t)(mac >> 24),
             (uint8_t)(mac >> 32),
             (uint8_t)(mac >> 40));
    return String(macHex);
}

// Reverb chat message (channel broadcast): only a blue blink for now.
void handleChatMessage(const String &message)
{
    ESP_LOGI(TAG, "[REVERB] Message Received: %s", message.c_str());

    ledController.flash(LedController::FX_MESSAGE);
}

// --- NFC / figure hooks ---

// Callback function for when figure download is complete
void onFigureDownloadComplete(const String &uid, const String &figureName, bool success, const String &error, const RequestManager::Figure &figure, bool isUpdate)
{
    const bool mounted = nfcController.isCardPresent() && nfcController.currentUidCopy() == uid;

    if (!success)
    {
        ESP_LOGE(TAG, "Figure %s (%s): %s", figureName.c_str(), uid.c_str(), error.c_str());
        gFigureLoadingSince = 0;
        if (mounted)
        {
            ledController.flash(LedController::FX_NO_CONTENT);
            const String detail = "uid=" + uid;
            if (error.indexOf("unreachable") >= 0 || error.indexOf("Offline") >= 0)
            {
                Faults::raise(Fault::FIGURE_OFFLINE, detail.c_str());
            }
            else if (error.indexOf("downloaded") >= 0)
            {
                Faults::raise(Fault::DOWNLOAD_FAILED, detail.c_str());
            }
            else if (error.indexOf("No figure") >= 0)
            {
                Faults::raise(Fault::FIGURE_UNKNOWN, detail.c_str());
            }
            else if (error.indexOf("asset_manifest") >= 0)
            {
                Faults::raise(Fault::MANIFEST_INVALID, detail.c_str());
            }
            else
            {
                Faults::raise(Fault::FIGURE_NO_CONTENT, detail.c_str());
            }
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
    Faults::clearOn(FaultClear::ON_PLAY); // the figure works: its faults are over
    const bool moreComing = requestManager.isFigureDownloading(uid); // playlist waits at its end instead of finishing
    const bool samePlaylist = audioController.hasPlaylist() && audioController.getPlaylistFigureUid() == uid;
    if (isUpdate && samePlaylist)
    {
        // Same UID, but is the content we announced still this figure's content? The cache-first
        // path may have started tracks from an older manifest (figure re-bound, content replaced);
        // once the backend's answer arrives, content that is not in it must not keep playing.
        const String current = audioController.getCurrentTrack();
        bool keepPlaying = audioController.isWaitingForTrack(); // between stories: the merge continues it
        if (keepPlaying)
        {
            // nothing to check: the playlist is ours and parked after its last cached track
        }
        else if (!current.isEmpty())
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
            audioController.mergePlaylist(playlist, uid, moreComing); // never interrupts the current track
            return;
        }
        ESP_LOGW(TAG, "Figure %s (%s): current content is not in the new manifest - restarting with the new content",
                 figureName.c_str(), uid.c_str());
    }
    else if (samePlaylist)
    {
        return; // already playing this figure (e.g. a second tap event)
    }
    audioController.setPlaylist(playlist, uid, moreComing);
    audioController.play();
    ESP_LOGI(TAG, "Started playing figure '%s'%s", figureName.c_str(), moreComing ? " (rest still downloading)" : "");
    if (gDownloadAnnouncedUid == uid)
    {
        gDownloadAnnouncedUid = "";
        prompts.say(Prompt::DOWNLOAD_DONE); // spoken over the first second, the story resumes after
    }
}

// This function will be called ONLY ONCE when a new card is detected
void afterNFCRead(const NFCData &nfcData)
{
    if (FactoryTest::active())
    {
        return; // factory session: no figure handling (the nfc test reads the card itself)
    }
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

    ledController.flash(LedController::FX_ACK);
    gFigureLoadingSince = millis(); // amber chase until the first track plays
    // Cache-first: plays what is on the card, asks the portal (GET /api/units/{uid}) for the
    // manifest and downloads what is missing; onFigureDownloadComplete() starts the playlist.
    requestManager.getCheckFigureTracks(nfcData.uidString);

    ESP_LOGI(TAG, "==========================");
}

// This function will be called when the reed switch is deactivated
// AFTER a card was successfully read in that session.
void afterDetachNFC()
{
    if (FactoryTest::active())
    {
        return;
    }
    ESP_LOGI(TAG, "=== Hook: afterDetachNFC ===");
    ESP_LOGI(TAG, "NFC session has ended.");

    // Stop audio and clear the playlist
    audioController.stop();
    audioController.clearPlaylist();
    gFigureLoadingSince = 0; // removing the figure is normal: no red, the scene just returns to idle

    ESP_LOGI(TAG, "Playlist cleared due to figure removal.");
    ESP_LOGI(TAG, "==========================");
}

void setup()
{
    Serial.setTxBufferSize(4096); // absorb boot-time bursts so lines are not dropped
    Serial.begin(115200);
    Serial.setTxTimeoutMs(30); // a host that is not draining the USB console must not stall the loop for long
    delay(1000); // Give serial time to initialize
    ESP_LOGI(TAG, "=== TilkieTalkie Board Tester ===");
    ESP_LOGI(TAG, "Initializing system...");
    Diag::logBootInfo(); // reset reason, heap, stored core dump

    // Initialize battery management FIRST — its I2C bus is independent of the 4V5 rail.
    // This restores the gauge protection FETs so the cell is connected and can buffer
    // the inrush/transient current from WiFi/BLE/Reverb. With the FETs open the cell is
    // isolated, the board runs unbuffered off USB, and it browns out during bring-up.
    battery.begin();

    // The charger is configured (3.6 V LFP limits, watchdog off) by begin(); if an external
    // tool owns the bus, let go of it right here and never touch it again this boot.
    gGaugeBusReleased = config.getInt(GAUGEBUS_NVS_KEY, 0) != 0;
    if (gGaugeBusReleased)
    {
        battery.setBusReleased(true);
        ESP_LOGW(TAG, "GAUGEBUS: battery bus released for an external tool (gaugebus attach to end)");
    }

#ifdef TT_BENCH
    // If this boot is the timer wake of a sleep-current measurement, print its result.
    Console::reportSleepTestIfAny();
#endif

    // Deeply discharged cell on USB: charge at minimal load instead of boot-looping. Returns
    // when no recovery is needed; otherwise deep-sleeps (charging) and re-checks on the next
    // boot a minute later.
    runDeadCellRecoveryHold();

    // Enable the shared 4V5 peripheral rail on GPIO4 before SD/NFC/audio/LED init. Also
    // releases the deep-sleep latch on GPIO4 (SleepController::begin() used to do this a
    // second time, with a second 500 ms wait, after the LED init).
    ESP_LOGI(TAG, "Enabling peripheral power...");
    sleepController.enablePeripheralPower();
    delay(500); // Give peripherals time to power up

    // First-boot bring-up (gauge provisioning + current calibration) if this board needs it.
    bringupBeginIfNeeded();

    // --- Minimal subsystems only: battery (above) + LED + sleep. The heavy subsystems
    // (SD, WiFi, audio, NFC, buttons) are deferred to startFullSystem() until the cell can
    // support them, so a depleted board stays in battery-monitor mode and charges instead
    // of brown-out looping at WiFi bring-up. ---
    ESP_LOGI(TAG, "Initializing LED Controller...");
    ledController.begin();
    ledController.setScene(LedController::SCENE_BOOT); // "waking up" from the first second
    OtaManager::getInstance().begin();
    // Fault catalogue (docs/FAULTS.md): reset classification, stored events, prompts for faults.
    Faults::begin();
    if (ConfigManager::getInstance().takeOpenFailure())
    {
        Faults::raise(Fault::CONFIG_CORRUPT); // NVS could not be opened at static init
    }
    {
        const String note = ConfigManager::getInstance().takeNvsRepairNote();
        if (note.length() > 0)
        {
            Faults::raise(Fault::CONFIG_CORRUPT, note.substring(0, 39).c_str()); // partition was full and reformatted
        }
    }
    Faults::setSpeaker([](Fault f) {
        switch (f)
        {
        case Fault::WIFI_AUTH_FAILED: prompts.say(Prompt::SETUP_FAILED); break;
        case Fault::WIFI_NO_LINK: prompts.say(Prompt::WIFI_LOST); prompts.say(Prompt::WIFI_RESET_HINT); break;
        case Fault::FIGURE_UNKNOWN:
        case Fault::FIGURE_NO_CONTENT: prompts.say(Prompt::FIGURE_NO_CONTENT); break;
        case Fault::FIGURE_OFFLINE: prompts.say(Prompt::FIGURE_OFFLINE); break;
        case Fault::DOWNLOAD_FAILED: prompts.say(Prompt::DOWNLOAD_FAILED); break;
        case Fault::OTA_DOWNLOAD_FAILED:
        case Fault::OTA_FLASH_FAILED:
        case Fault::OTA_SHA_MISMATCH: prompts.say(Prompt::UPDATE_FAILED); break;
        case Fault::NFC_READ_TROUBLE: prompts.say(Prompt::NFC_READ_TROUBLE); break;
        case Fault::BATTERY_LOW: prompts.say(Prompt::BATTERY_LOW); break;
        case Fault::NOT_LINKED: scheduleNotLinkedPrompt(); break; // spoken when idle, after the hello, at most daily
        case Fault::CAPTIVE_PORTAL: prompts.say(Prompt::SETUP_CAPTIVE); break;
        default: break; // silent faults (muz: the toy cannot speak without the card anyway)
        }
    });

    // Loop watchdog. A hung loop task (2026-10-15: LEDs on, prompts playing, no network, no
    // buttons) stayed hung until someone pulled the battery; now it panics after 120 s and the
    // crash-loop guard in OtaManager treats a repeating hang like a crash. Only the loop task is
    // watched; synchronous HTTP on the loop (token, channel auth) resets it in between.
    {
        esp_task_wdt_config_t wdt = {};
        wdt.timeout_ms = 120000;
        wdt.idle_core_mask = 0;
        wdt.trigger_panic = true;
        if (esp_task_wdt_reconfigure(&wdt) == ESP_OK && esp_task_wdt_add(NULL) == ESP_OK)
        {
            ESP_LOGI(TAG, "Loop watchdog armed (120 s)");
        }
        else
        {
            ESP_LOGW(TAG, "Loop watchdog could not be armed");
        }
    }

    ESP_LOGI(TAG, "Initializing Sleep Controller...");
    sleepController.begin();
    // Inactivity timeout (disabled during first-boot bring-up so it stays awake to
    // provision + calibrate and survive gauge resets).
    sleepController.setInactivityTimeout(gBringupPhase == BRINGUP_INACTIVE ? Policy::INACTIVITY_TIMEOUT_MS : Policy::INACTIVITY_TIMEOUT_NEVER);
    sleepController.onSleep([]()
                            {
                                ESP_LOGI(TAG, "Device is about to enter deep sleep...");
                                ledController.setScene(LedController::SCENE_SLEEPING); // fade out
                                delay(900);
                                // PN532 software power-down + NFC bus parked high through the
                                // sleep (the PN532 otherwise idles ~40 mA: 5-day depletion incident).
                                SleepController::powerDownNfcForSleep();
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
    else if (battery.hasTelemetry() && battery.getBatteryVoltage() >= Policy::SYSTEM_START_IMMEDIATE_V)
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
                 Policy::SYSTEM_START_IMMEDIATE_V, Policy::SYSTEM_START_V, Policy::SYSTEM_START_HOLD_MS / 1000UL);
    }
}

// What OtaManager's dock-only state machine looks at every loop pass (docs/OTA-STATE-MACHINE.md):
// the dock (VBUS; a full pack on the dock counts), audio, a figure on the pins (pogo switch or a
// read card), a story download in flight, the battery for the 15 % abort.
static OtaManager::Conditions otaConditions()
{
    OtaManager::Conditions c;
    c.docked = battery.isVbusPresent();
    c.playing = audioController.isPlaying(); // prompts included: "updating myself" must finish first
    c.paused = audioController.isPaused();
    c.figure = nfcController.isPogoSwitchActive() || nfcController.isCardPresent();
    c.storyDownload = fileManager.hasActiveDownload();
    c.batteryKnown = battery.hasTelemetry();
    c.batteryPercent = battery.getBatteryPercentage();
    return c;
}

static const char *gResetRequestedBy = nullptr;

void requestCustomerReset(const char *why)
{
    gResetRequestedBy = why ? why : "request";
}

void runCustomerReset(bool all, const char *why, bool speak)
{
    ESP_LOGW(TAG, "=== %s reset (%s) ===", all ? "Service (--all)" : "Customer", why ? why : "?");
    ledController.flash(LedController::FX_RESET);
    audioController.stop();
    audioController.clearPlaylist();
    // Unbind first, while the token still exists (skipped offline; the portal's unbind stays
    // possible from the app or the admin).
    const bool unbound = requestManager.unbindBestEffort();
    ESP_LOGI(TAG, "reset: portal unbind %s", unbound ? "done" : "skipped/failed");
    config.clearJWTToken();
    requestManager.forgetToken();
    fileManager.wipeCardCache("reset");
    requestManager.forgetAllUidMappings();
    Faults::clearAll();
    config.deleteSetting("audio_volume");
    config.deleteSetting("volume_ceiling");
    config.deleteSetting("max_brightness");
    if (all)
    {
        config.clearFactory();
    }
    if (speak)
    {
        prompts.sayBlocking(Prompt::WIFI_RESET_DONE, 6000);
    }
    wifiProv.reset(); // forgets the network and restarts into provisioning
}

// WiFi (connect, or BLE provisioning when not provisioned) and the pairing invitation.
static void startWifi()
{
    wifiProv.begin();
    if (!wifiProv.isProvisioned() && !wifiProv.provisioningImpossible())
    {
        startSetupWelcome(); // repeated by serviceUxPrompts() while nobody pairs
    }
}

void startDeferredWifi()
{
    if (!gWifiDeferred)
    {
        return;
    }
    gWifiDeferred = false;
    ESP_LOGI(TAG, "factory session over - starting WiFi");
    startWifi();
}

// API client + Reverb. Safe to call once WiFi has an IP; idempotent.
static void startNetworkServices()
{
    if (gNetworkServicesStarted)
    {
        return;
    }
    gNetworkServicesStarted = true;
    ESP_LOGI(TAG, "Initializing network services...");
    // Apply the configured API base (production unless overridden for the bench).
    requestManager.setBaseUrl(loadEndpoints().apiBase);
    if (!requestManager.begin())
    {
        ESP_LOGW(TAG, "Request Manager initialization failed (no token yet) - Reverb will retry the token");
    }
    else
    {
        ESP_LOGI(TAG, "Request Manager initialized successfully");
    }
    startReverb();
    OtaManager::getInstance().onNetworkServicesStarted(); // checks use the configured API base
}

// Brings up the heavy subsystems (SD, WiFi, audio, Reverb, NFC, buttons). Called once the
// battery can support them — immediately from setup() on a healthy cell, or later from
// loop() once a depleted cell has charged back above Policy::SYSTEM_START_V.
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

    if (fileManager.begin())
    {
        // A voice prompt pack staged for this firmware by the previous one becomes active now,
        // before the first word is spoken (docs/VOICE-PROMPTS.md "Remote update").
        PromptPackManager::getInstance().setPendingVerify(OtaManager::getInstance().bootedFromUpdate());
        PromptPackManager::getInstance().begin();
        if (fileManager.takeCardWasForeign())
        {
            requestManager.forgetAllUidMappings(); // the wiped cache's figures (CARD_FOREIGN was raised)
        }
        // Downloads interrupted by the reboot continue from the persisted queue; give their
        // figures a tracker so the manifest is completed when they land.
        requestManager.recoverPendingManifests();
    }
    else
    {
        Faults::raise(Fault::SD_MOUNT_FAILED);
        ESP_LOGW(TAG, "File Manager initialization failed!");
        ESP_LOGW(TAG, "SD card functionality will not be available.");
    }
    // Set up figure download complete callback (before WiFi connection)
    requestManager.setFigureDownloadCompleteCallback(onFigureDownloadComplete);
    requestManager.setFigureEventCallback([](const String &uid, RequestManager::FigureEvent ev) {
        if (ev == RequestManager::FigureEvent::DOWNLOADS_STARTED)
        {
            gDownloadAnnouncedUid = uid;
            prompts.say(Prompt::DOWNLOAD_START);
        }
    });

    // Initialize audio controller
    if (!audioController.begin())
    {
        Faults::raise(Fault::AUDIO_INIT_FAILED);
        ESP_LOGW(TAG, "Audio Controller initialization failed!");
        ESP_LOGW(TAG, "Audio functionality will not be available.");
    }

    // Network services (API client, Reverb) are started by serviceNetworkStart() in the loop
    // as soon as WiFi has an IP - nothing here waits for the network.

    // --- Initialize NFC Controller ---
    ESP_LOGI(TAG, "Initializing NFC Controller...");


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
        Faults::raise(Fault::NFC_INIT_FAILED);
        ESP_LOGE(TAG, "FATAL: NFC Controller initialization failed!");
        ESP_LOGE(TAG, "NFC functionality will not be available.");
        gSystemError = true; // deriveLedScene() shows the steady red error scene
    }

    // Initialize Button Controller
    ESP_LOGI(TAG, "Initializing Button Controller...");
    buttonController.begin();
    buttonController.setComboHoldTime(5000);      // both combos: 5 s
    buttonController.setComboLongHoldTime(10000); // 1+4 only: 10 s = customer reset

    // Set up button callbacks
    // Factory session: the buttons only feed the `buttons` test (FactoryTest reads them).
    buttonController.onSingleClick([](ButtonController::ButtonId button)
                                   {
        if (FactoryTest::active()) return;
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
        if (FactoryTest::active()) return;
        // Reset sleep timer on button hold
        sleepController.resetActivity();
        
        ESP_LOGI(TAG, "Hold started on button %d (duration: %lu ms)", button + 1, duration);
            if (button == ButtonController::BUTTON_1 || button == ButtonController::BUTTON_4) {
                ESP_LOGI(TAG, "Starting volume %s", (button == ButtonController::BUTTON_1) ? "up" : "down");
        } });

    buttonController.onHoldContinuous([](ButtonController::ButtonId button, unsigned long duration)
                                      {
            if (FactoryTest::active()) return;
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

    // Volume buttons held together (docs/CONSOLE.md "Button holds"): at 5 s a tone (two rising
    // notes) arms "forget WiFi", done when the buttons are let go before 10 s; still held at
    // 10 s a second tone (three falling notes) and the customer reset.
    buttonController.onComboHold([]()
    {
        if (FactoryTest::active()) return;
        ESP_LOGW(TAG, "COMBO HOLD (1+4) 5 s - release to forget WiFi, keep holding for a full reset");
        ledController.flash(LedController::FX_RESET);
        prompts.say(Prompt::TONE_CHARGE_ON);
    });
    buttonController.onComboReleasedAfterHold([](unsigned long heldMs)
    {
        if (FactoryTest::active()) return;
        // Forget the WiFi network (stories stay on the card), then restart into BLE provisioning.
        ESP_LOGW(TAG, "COMBO (1+4) released after %lu ms - forgetting the WiFi network", heldMs);
        prompts.sayBlocking(Prompt::WIFI_RESET_DONE, 6000);
        wifiProv.reset(); // clears credentials and restarts
    });
    buttonController.onComboHoldLong([]()
    {
        if (FactoryTest::active()) return;
        ESP_LOGW(TAG, "COMBO HOLD (1+4) 10 s - customer reset");
        prompts.sayBlocking(Prompt::TONE_SLEEP, 2000);
        runCustomerReset(false, "buttons", true);
    });

    buttonController.onComboHold2([]()
    {
        if (FactoryTest::active()) return;
        ESP_LOGW(TAG, "COMBO HOLD (2+3) - restarting");
        ledController.flash(LedController::FX_FAIL);
        delay(1500);
        ESP.restart();
    });

    ESP_LOGI(TAG, "Button Controller initialized successfully!");

    // rapid pulse LED to indicate system is ready
    ledController.flash(LedController::FX_SUCCESS);
    battery.startTask(); // ~190 ms of bit-banged I2C per poll leaves the loop
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifiProv.onProvisioningEvent([](WiFiProvisioningManager::ProvEvent ev) {
        switch (ev)
        {
        case WiFiProvisioningManager::ProvEvent::CRED_RECV: prompts.say(Prompt::SETUP_CONNECTING); break;
        case WiFiProvisioningManager::ProvEvent::CRED_FAIL: Faults::raise(Fault::WIFI_AUTH_FAILED); break;
        // The network wants a login page: rejected like a wrong password (speaks setup_captive).
        // Raised from the WiFi event task like WIFI_AUTH_FAILED above.
        case WiFiProvisioningManager::ProvEvent::CAPTIVE: Faults::raise(Fault::CAPTIVE_PORTAL, "provisioning"); break;
        case WiFiProvisioningManager::ProvEvent::ENDED:
            if (WiFi.isConnected()) prompts.say(Prompt::SETUP_SUCCESS);
            break;
        default: break;
        }
    });
    if (config.factoryPop().isEmpty())
    {
        // No factory identity (docs/PROTOCOL-V2.md §1): BLE provisioning uses the legacy PIN
        // (TT_LEGACY_POP builds) or is impossible; the token comes from the legacy endpoint.
        Faults::raise(Fault::NOT_ENROLLED, "no pop");
    }
    {
        String host;
        uint16_t port = 80;
        probeTargetFrom(loadEndpoints().apiBase, host, port);
        wifiProv.setProbeTarget(host, port);
        ESP_LOGI(TAG, "portal probe: http://%s:%u/storage/ping.json", host.c_str(), (unsigned)port);
    }
    // Voice feedback: Turkish prompts from the SD card (docs/VOICE-PROMPTS.md)
    prompts.begin();
    if (FactoryTest::active())
    {
        // Factory session: the radio stays as it is until a test asks for it; `ft end` starts it.
        gWifiDeferred = true;
        ESP_LOGW(TAG, "factory session: WiFi/BLE start deferred to ft end");
    }
    else
    {
        startWifi();
    }

    wifiProv.setIdleCheck([]() {
        // A restart to recover the network is invisible only while nothing is going on.
        return !audioController.isPlaying() && !audioController.isPaused() && !nfcController.isCardPresent() &&
               !OtaManager::getInstance().inProgress() && !fileManager.hasActiveDownload() && !PromptPackManager::getInstance().isStaging();
    });
    OtaManager::getInstance().setConditions(otaConditions);
    ESP_LOGI(TAG, "Full system started.");
}

// Start the API client + Reverb when WiFi comes up after boot (fresh provisioning, router
// back), once the BLE provisioning stack is gone and the heap has recovered.
static void serviceNetworkStart()
{
    if (gSystemPhase != PHASE_RUNNING)
    {
        return;
    }
    // Behind a captive portal (or without internet) the toy behaves as offline: the API client
    // and the WebSocket see no network, and nothing starts until the portal probe passes.
    RequestManager::setPortalReachable(wifiProv.isInternetReady());
    if (!wifiProv.isInternetReady())
    {
        gWifiUpSince = 0;
        return;
    }
    if (gWifiUpSince == 0)
    {
        gWifiUpSince = millis() ? millis() : 1;
    }
    TimeSync::begin(); // SNTP from the first connection on (idempotent); the clock then survives sleep and resets
    if (gNetworkServicesStarted)
    {
        static bool healthyMarked = false;
        if (!healthyMarked && reverb.isConnected())
        {
            healthyMarked = true;
            OtaManager::getInstance().notePortalReached("Reverb subscribed"); // confirms a new image
        }
        return;
    }
    if (wifiProv.isProvisioningManagerActive() || millis() - gWifiUpSince < 2000)
    {
        return; // the provisioning app is still attached / BLE memory not yet released
    }
    if (ESP.getFreeHeap() < 55000)
    {
        static unsigned long lastLog = 0;
        if (millis() - lastLog > 10000)
        {
            lastLog = millis();
            ESP_LOGW(TAG, "NET: waiting for memory before starting the network services (%u free)", ESP.getFreeHeap());
        }
        return;
    }
    ESP_LOGI(TAG, "NET: WiFi is up - starting network services");
    startNetworkServices();
}

// WiFi "connected" but nothing gets through (DHCP lease gone, AP dropped us silently): the
// Reverb socket then stays down although the radio reports a link. After 10 min of that the
// radio is reset; the plain "not connected" case is handled in WiFiProvisioningManager.
static void serviceNetworkHealth()
{
    static unsigned long lastReset = 0;
    if (!gNetworkServicesStarted || !wifiProv.isInternetReady() || wifiProv.isProvisioningManagerActive())
    {
        return; // a captive network is "offline": no radio reset for it
    }
    const unsigned long now = millis();
    const unsigned long last = reverb.lastConnectedAt();
    const unsigned long ref = last ? last : gFullSystemStartedAt;
    if (now - ref > 10UL * 60UL * 1000UL && now - lastReset > 10UL * 60UL * 1000UL)
    {
        lastReset = now;
        wifiProv.resetRadio("link up but the portal has been unreachable for 10 min");
    }
}

void loop()
{
    esp_task_wdt_reset();
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
            Faults::raise(Fault::STACK_LOW);
        }
    }

    // Serial console (Console.cpp; bench commands only with -DTT_BENCH)
    Console::service();
    // Factory test session and its running test (`ft`, docs/FT-PROTOCOL.md)
    TT_TIMED("ft", FactoryTest::update());
    const bool factory = FactoryTest::active();

    // Update battery management
    TT_TIMED("battery", battery.update());

    // Low-battery cutoff on battery power (DeadCellRecovery.cpp)
    serviceLowBatteryCutoff();

    // Battery-monitor mode: until the cell can support the full system, keep only the
    // battery + bring-up + LED alive, then start the full system once it recovers. This
    // prevents the brown-out/reboot loop a depleted cell causes during WiFi bring-up.
    // Charger supervision runs in every phase (monitor AND full-run) while USB is present.
    TT_TIMED("charger", superviseChargerOnUsb());
#ifdef TT_BENCH
    TT_TIMED("chargemon", Console::serviceChargeMonitor());
#endif

    if (gSystemPhase == PHASE_MONITOR)
    {
        updateBringup(); // gauge calibration (if needed) runs here at minimal load

        // Start the full system only once bring-up is terminal (done or failed) AND the
        // cell has PROVEN it can support the load: clearly-healthy voltage immediately,
        // or SYSTEM_START_V sustained for SYSTEM_START_HOLD_MS (Policy.h; a rested near-empty cell
        // reads above the bare threshold for a moment and then collapses under WiFi).
        const bool bringupTerminal =
            (gBringupPhase == BRINGUP_INACTIVE || gBringupPhase == BRINGUP_FAILED);
        static unsigned long readySince = 0;
        bool batteryProven = false;
        if (battery.hasTelemetry())
        {
            const float v = battery.getBatteryVoltage();
            if (v >= Policy::SYSTEM_START_IMMEDIATE_V)
            {
                batteryProven = true;
            }
            else if (v >= Policy::SYSTEM_START_V)
            {
                if (readySince == 0)
                {
                    readySince = millis();
                }
                batteryProven = (millis() - readySince) >= Policy::SYSTEM_START_HOLD_MS;
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
            if (!factory && millis() - lastMonLog > 5000) // a factory session keeps the port quiet
            {
                lastMonLog = millis();
                ESP_LOGW(TAG, "MONITOR: waiting (bringup=%d, %.3fV, charging=%d) before full start.",
                         (int)gBringupPhase, battery.hasTelemetry() ? battery.getBatteryVoltage() : 0.0f,
                         (int)battery.getChargingStatus());
            }
        }
        if (!factory)
        {
            updateChargingUx(); // charge bar while docked (bring-up still owns its LED)
        }
        // Inactivity sleep in monitor mode too (same guards as the full run): a cell between
        // the 3.00 V cutoff and the 3.10 V start threshold on battery used to stay awake
        // until the cutoff. Never during a factory session.
        if (battery.isVbusPresent() || factory)
        {
            sleepController.resetActivity();
        }
        else
        {
            TT_TIMED("sleep", sleepController.update());
        }
        delay(5);
        return;
    }

#ifdef TT_BENCH
    Console::serviceLearnFloor(); // learning-cycle discharge floor (learndis)
#endif

    // First-boot bring-up state machine (owns the LED until complete).
    updateBringup();

    // Update file manager
    TT_TIMED("files", fileManager.update());

    // Update audio controller
    TT_TIMED("audio", audioController.update());

    // Handle WiFi background reconnection (only when credentials exist but not connected).
    // Not before WiFi was started (a factory session defers it).
    if (!gWifiDeferred)
    {
        TT_TIMED("wifi", wifiProv.handleBackgroundReconnection());
    }

    // Update Reverb client only if WiFi is connected and the portal is reachable (not captive)
    if (wifiProv.isInternetReady())
    {
        TT_TIMED("reverb", reverb.update());
    }

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
    TT_TIMED("prompts", PromptPackManager::getInstance().update());
    TT_TIMED("faults", Faults::update());
    TimeSync::update();
    {
        static unsigned long lastHeapCheck = 0;
        if (millis() - lastHeapCheck > 30000)
        {
            lastHeapCheck = millis();
            const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (freeInternal < 16000)
            {
                char d[24];
                snprintf(d, sizeof(d), "free=%lu", (unsigned long)freeInternal);
                Faults::raise(Fault::HEAP_LOW, d);
            }
        }
    }
    gOtaProgress = OtaManager::getInstance().inProgress() ? OtaManager::getInstance().progress() : -1;
    // "Kendimi güncelliyorum..." when OtaManager is about to leave WAITING; it starts once the
    // sentence is over ("update done" is spoken at boot, bootedFromUpdate).
    if (OtaManager::getInstance().consumeAnnounceRequest())
    {
        prompts.say(Prompt::UPDATE_START);
    }
    if (OtaManager::getInstance().consumeJustFailed())
    {
        const String err = OtaManager::getInstance().lastError();
        Fault f = Fault::OTA_DOWNLOAD_FAILED;
        switch (OtaManager::getInstance().lastFailure())
        {
        case OtaManager::Failure::SHA: f = Fault::OTA_SHA_MISMATCH; break;
        case OtaManager::Failure::FLASH: f = Fault::OTA_FLASH_FAILED; break;
        case OtaManager::Failure::PROMPTS: f = Fault::PROMPT_PACK_FAILED; break;
        // BATTERY stays OTA_DOWNLOAD_FAILED (karpuz: "will try again later"), detail says why
        default: break;
        }
        Faults::raise(f, err.substring(0, 39).c_str());
    }
    if (!factory)
    {
        TT_TIMED("led", deriveLedScene()); // the LED task renders the scene (FactoryTest owns it in a session)
    }

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
    if (!factory)
    {
        TT_TIMED("chargeux", updateChargingUx());
    }
    if (gResetRequestedBy)
    {
        runCustomerReset(false, gResetRequestedBy, true); // does not return
    }
    TT_TIMED("netstart", serviceNetworkStart());
    TT_TIMED("nethealth", serviceNetworkHealth());
    if (!factory)
    {
        TT_TIMED("ux", serviceUxPrompts()); // no hello, no reminders in a factory session
    }
    TT_TIMED("battux", updateBatteryUx());

    // No sleeping while docked: charging is the toy's "plugged in" UX. resetActivity()
    // keeps the inactivity countdown cleared (silent no-op once clear), so after unplug
    // the full timeout starts fresh instead of sleeping instantly. Manual `sleep`
    // (forceSleep) still works, and the VBUS wake stub covers the inverse case of
    // docking a device that is already asleep on battery.
    if (battery.isVbusPresent() || factory)
    {
        sleepController.resetActivity(); // docked, or a factory session: never sleep
    }
    else
    {
        TT_TIMED("sleep", sleepController.update());
    }

    delay(1);
}
#endif // SLEEP_PROBE_BUILD
