// Factory test protocol `ft` (docs/FT-PROTOCOL.md). The manufacturing panel on the portal
// drives a unit over WebSerial: session, identity, self-tests with pass rules, the test record,
// cleanup and ship mode. Everything here runs on the loop task; a test is a small state machine
// advanced by update() so the loop never waits (the loop watchdog is 120 s, the console path
// must not block).
//
// Answers are JSON lines written with one Serial.write each: they are not ESP_LOG output (the
// session lowers the log level to WARN) and a line is never interleaved with a log line.

#include "FactoryTest.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <algorithm>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include "Bringup.h"
#include "Faults.h"
#include "OtaManager.h"
#include "Policy.h"
#include "PromptPackManager.h"
#include "SystemState.h"
#include "TimeSync.h"
#include "Version.h"
#include "VoicePrompts.h"

static const char *TAG = "ft";

namespace
{
VoicePrompts &prompts = VoicePrompts::getInstance();

// ---- The catalogue ---------------------------------------------------------------------------

enum class Test : uint8_t { NONE, LEDS, BUTTONS, NFC, POGO, SPEAKER, JACK, BATTERY, PD, SD, WIFI, BLE, BRINGUP, SLEEP };

struct TestDef
{
    Test id;
    const char *name;
    bool needsSystem; // SD, audio, NFC, buttons and the radio exist only once startFullSystem() ran
};

const TestDef kTests[] = {
    {Test::LEDS, "leds", false},     {Test::BUTTONS, "buttons", true}, {Test::NFC, "nfc", true},
    {Test::POGO, "pogo", true},      {Test::SPEAKER, "speaker", true}, {Test::JACK, "jack", true},
    {Test::BATTERY, "battery", false}, {Test::PD, "pd", false},        {Test::SD, "sd", true},
    {Test::WIFI, "wifi", true},      {Test::BLE, "ble", true},         {Test::BRINGUP, "bringup", false},
    {Test::SLEEP, "sleep", false},
};

const TestDef *findTest(const String &name)
{
    for (const TestDef &t : kTests)
    {
        if (name == t.name) return &t;
    }
    return nullptr;
}

// Timeouts (docs/FT-PROTOCOL.md)
constexpr unsigned long ASK_TIMEOUT_MS = 120000;
constexpr unsigned long LED_STEP_MS = 150;
constexpr int LED_TEST_INTENSITY = 160;      // bright enough to judge, not the full 5 x 60 mA
constexpr unsigned long BUTTON_WAIT_MS = 30000; // per button: the window restarts at each press
constexpr unsigned long NFC_WAIT_MS = 20000;
constexpr unsigned long POGO_WAIT_MS = 20000;
constexpr unsigned long JACK_WAIT_MS = 20000;
constexpr unsigned long SWITCH_DEBOUNCE_MS = 50;
constexpr int SPEAKER_TEST_VOLUME = 60;      // % of the codec range: clearly audible, not harsh
constexpr unsigned long PROMPT_MAX_MS = 10000;
constexpr size_t SD_TEST_BYTES = 2u * 1024u * 1024u;
constexpr size_t SD_CHUNK = 32u * 1024u;
constexpr unsigned long SD_BUDGET_MS = 60000;
const char *SD_TEST_DIR = "/.ft";
const char *SD_TEST_FILE = "/.ft/sdtest.bin";
constexpr unsigned long WIFI_DISCONNECT_MS = 3000;
constexpr unsigned long WIFI_CONNECT_MS = 20000;
constexpr unsigned long WIFI_PROBE_MS = 15000;
constexpr unsigned long BLE_ADVERTISE_MS = 10000;
constexpr unsigned long BRINGUP_BUDGET_MS = 3UL * 60UL * 1000UL;
constexpr unsigned long SHIP_CANCEL_MS = 15000;       // VBUS still there: cancel
constexpr unsigned long SHIP_NOT_ENGAGED_MS = 25000;  // unplugged but still alive: cancel
// LiFePO4 rests near 3.3 V from half charge up, so a voltage gate alone would refuse most units
// unless they ship while charging. The gauge's state of charge is the primary gate (the owner
// wants units shipped around 40 %); the voltage only catches an empty cell.
constexpr float SHIP_MIN_SOC = 30.0f;
constexpr float SHIP_MIN_V = 3.20f;
constexpr size_t TESTED_MAX_BYTES = 1500;
constexpr size_t ERROR_MAX_CHARS = 80;

// ---- Session and the running test ------------------------------------------------------------

bool gActive = false;
JsonDocument gResults; // name -> {pass, ms, data, error}; RAM only until `ft commit` / `ft end`

Test gTest = Test::NONE;
const char *gName = "";
unsigned long gStartedAt = 0;
unsigned long gStepAt = 0;
int gStep = 0;
bool gAsking = false;
unsigned long gAskAt = 0;
JsonDocument gData;
String gArg1, gArg2;

bool gShipArmed = false;
unsigned long gShipAt = 0;
unsigned long gLastLedAssert = 0;

// ---- Output ----------------------------------------------------------------------------------

void emit(const JsonDocument &doc)
{
    String line;
    serializeJson(doc, line);
    line += '\n';
    Serial.write(reinterpret_cast<const uint8_t *>(line.c_str()), line.length());
}

void replyOk(const char *verb)
{
    JsonDocument d;
    d["ft"] = verb;
    d["ok"] = true;
    emit(d);
}

void replyError(const char *verb, const String &error)
{
    JsonDocument d;
    d["ft"] = verb;
    d["ok"] = false;
    d["error"] = error;
    emit(d);
}

void progress(const String &step, int done, int total)
{
    JsonDocument d;
    d["ft"] = "progress";
    d["name"] = gName;
    d["step"] = step;
    d["done"] = done;
    d["total"] = total;
    emit(d);
}

void ask(const char *question)
{
    JsonDocument d;
    d["ft"] = "ask";
    d["name"] = gName;
    d["question"] = question;
    emit(d);
    gAsking = true;
    gAskAt = millis();
}

// ---- Identity helpers ------------------------------------------------------------------------

const char *POP_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

// "TK-ABCD-EFGH", "tk-abcd-efgh" or "ABCDEFGH" -> "ABCDEFGH"; "" when it is not a valid pop.
String normalisePop(String pop)
{
    pop.toUpperCase();
    if (pop.startsWith("TK-")) pop = pop.substring(3);
    pop.replace("-", "");
    if (pop.length() != 8) return String();
    for (size_t i = 0; i < pop.length(); ++i)
    {
        if (!strchr(POP_ALPHABET, pop[i])) return String();
    }
    return pop;
}

bool safeToken(const String &v, size_t minLen, size_t maxLen)
{
    if (v.length() < minLen || v.length() > maxLen) return false;
    for (size_t i = 0; i < v.length(); ++i)
    {
        const char c = v[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

void setSerial(JsonDocument &d)
{
    const String serial = config.factorySerial();
    if (serial.length()) d["serial"] = serial;
    else d["serial"] = nullptr;
}

// A stored JSON record (factory namespace) as a member of d, or null.
void embedRecord(JsonVariant dst, const char *key)
{
    const String json = config.factoryRecord(key);
    JsonDocument rec;
    if (json.length() && deserializeJson(rec, json) == DeserializationError::Ok)
    {
        dst.set(rec.as<JsonVariantConst>());
    }
    else
    {
        dst.set(nullptr);
    }
}

int mv(float volts) { return (int)lroundf(volts * 1000.0f); }

// ---- Factory mode ----------------------------------------------------------------------------

void activate()
{
    gActive = true;
    esp_log_level_set("*", ESP_LOG_WARN);
    prompts.setMuted(true);
    prompts.update(); // muted: drops what was queued before the session instead of saying it later
    OtaManager::getInstance().setFactoryHold(true);
    if (audioController.isInitialized())
    {
        audioController.stop();          // a story or prompt that was playing
        audioController.clearPlaylist(); // figure handling is off for the session
    }
    gFigureLoadingSince = 0;
    sleepController.resetActivity();
    ledController.setScene(LedController::SCENE_FACTORY);
    gLastLedAssert = millis();
    ESP_LOGW(TAG, "factory session open");
}

void deactivate()
{
    gActive = false;
    gResults.clear();
    prompts.setMuted(false);
    OtaManager::getInstance().setFactoryHold(false);
    sleepController.resetActivity(); // the inactivity countdown starts fresh
    if (gSystemPhase == PHASE_RUNNING)
    {
        ledController.setScene(LedController::SCENE_OFF); // deriveLedScene() takes over
    }
    else
    {
        ledController.pulseLed(SYSTEM_MONITOR_LED);
    }
    ESP_LOGW(TAG, "factory session closed");
    esp_log_level_set("*", (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
    startDeferredWifi(); // what startFullSystem() skipped during the session
}

// ---- Debounced two-state input (pogo switch, jack contact) ----------------------------------

struct Debounced
{
    bool stable = false;
    bool pending = false;
    unsigned long since = 0;
    void reset(bool level)
    {
        stable = pending = level;
        since = millis();
    }
    // true when the stable level changed on this call
    bool feed(bool level)
    {
        const unsigned long now = millis();
        if (level != pending)
        {
            pending = level;
            since = now;
        }
        if (pending != stable && now - since >= SWITCH_DEBOUNCE_MS)
        {
            stable = pending;
            return true;
        }
        return false;
    }
};

// ---- Per-test state --------------------------------------------------------------------------

// buttons
bool gBtnSeen[4];
bool gBtnPrev[4];
unsigned long gBtnMs[4];
int gBtnCount = 0;
unsigned long gBtnWindowAt = 0;

// pogo / jack: step 0 = waiting for open (it was closed at the start), 1 = waiting for
// close, 2 = waiting for open again
Debounced gSwitch;
unsigned long gClosedAt = 0;

// sd
File gSdFile;
uint8_t *gSdBuf = nullptr;
size_t gSdDone = 0;
uint64_t gSdWriteUs = 0;
uint64_t gSdReadUs = 0;
size_t gSdMismatch = 0;

// wifi
wifi_mode_t gWifiWasMode = WIFI_MODE_NULL;
bool gWifiWasConnected = false;
bool gWifiHadConfig = false;
bool gWifiStartedByUs = false;
bool gWifiTouched = false;
bool gWifiProbeStarted = false;
unsigned long gWifiConnectAt = 0;
wifi_config_t gWifiSaved;

// ble
bool gBleOwned = false;

// bringup
String gBringupPhaseSeen;

uint8_t sdPattern(size_t i)
{
    return (uint8_t)(((uint32_t)i * 2654435761u) >> 24);
}

void sdCleanup()
{
    if (gSdFile) gSdFile.close();
    if (SD_MMC.exists(SD_TEST_FILE)) SD_MMC.remove(SD_TEST_FILE);
    if (SD_MMC.exists(SD_TEST_DIR)) SD_MMC.rmdir(SD_TEST_DIR);
    if (gSdBuf)
    {
        heap_caps_free(gSdBuf);
        gSdBuf = nullptr;
    }
}

// Puts the radio back the way the test found it: the saved STA configuration (never written to
// flash: the test ran with RAM storage), the driver off if it was off, the home network
// reconnected if it was in use, and the background logic released.
void wifiRestore()
{
    if (!gWifiTouched)
    {
        return;
    }
    gWifiTouched = false;
    esp_wifi_disconnect();
    if (gWifiHadConfig)
    {
        // The driver refuses a new config while it is still connecting (ESP_ERR_WIFI_STATE); the
        // disconnect above takes a few ms. Going back to flash storage with the test network still
        // in RAM would let the next connect() write it over the customer's network, so a config
        // that cannot be restored ends in a restart (flash still holds the customer's network).
        esp_err_t r = ESP_FAIL;
        for (int i = 0; i < 25 && (r = esp_wifi_set_config(WIFI_IF_STA, &gWifiSaved)) != ESP_OK; ++i)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (r != ESP_OK)
        {
            ESP_LOGE(TAG, "wifi test: STA config could not be restored (%s) - restarting", esp_err_to_name(r));
            Serial.flush();
            ESP.restart();
        }
    }
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (gWifiStartedByUs)
    {
        WiFi.mode(WIFI_OFF);
    }
    else if (gWifiWasMode != WIFI_MODE_NULL)
    {
        const bool haveHome = gWifiHadConfig && gWifiSaved.sta.ssid[0] != 0;
        if (gWifiWasConnected || (haveHome && !wifiProv.isProvisioningManagerActive()))
        {
            WiFi.begin(); // the stored (restored) network
        }
    }
    wifiProv.setFactoryHold(false);
}

// Undo whatever the test changed. Runs for every way a test ends (pass, fail, abort, timeout).
void cleanup(Test t)
{
    switch (t)
    {
    case Test::LEDS:
        ledController.setScene(LedController::SCENE_FACTORY);
        break;
    case Test::SPEAKER:
        audioController.stopTone();
        if (audioController.isPromptPlaying()) audioController.stop();
        audioController.clearVolumeOverride();
        break;
    case Test::JACK:
        audioController.setAmpMode(AudioController::AMP_AUTO);
        break;
    case Test::SD:
        sdCleanup();
        break;
    case Test::WIFI:
        wifiRestore();
        break;
    case Test::BLE:
        if (gBleOwned) wifiProv.factoryBleStop();
        gBleOwned = false;
        break;
    case Test::BRINGUP:
        bringupAbort("aborted"); // no-op once it ended by itself
        break;
    default:
        break;
    }
}

void finish(bool pass, const String &errorIn)
{
    const Test t = gTest;
    cleanup(t);
    String error = errorIn;
    if (error.length() > ERROR_MAX_CHARS) error = error.substring(0, ERROR_MAX_CHARS);
    const uint32_t ms = (uint32_t)(millis() - gStartedAt);

    JsonDocument r;
    r["ft"] = "result";
    r["name"] = gName;
    r["pass"] = pass;
    r["ms"] = ms;
    if (gData.isNull()) r["data"].to<JsonObject>();
    else r["data"].set(gData.as<JsonVariantConst>());
    if (!pass) r["error"] = error;
    emit(r);

    JsonObject entry = gResults[gName].to<JsonObject>(); // a re-run overwrites
    entry["pass"] = pass;
    entry["ms"] = ms;
    if (gData.isNull()) entry["data"].to<JsonObject>();
    else entry["data"].set(gData.as<JsonVariantConst>());
    if (!pass) entry["error"] = error;

    gTest = Test::NONE;
    gAsking = false;
    gData.clear();
    gLastLedAssert = 0; // idle scene right away
}

// ---- Tests: start (right after the "running" line) and step (every loop pass) ----------------

void ledsShow()
{
    static const uint32_t colors[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    static const char *names[4] = {"red", "green", "blue", "white"};
    if (gStep < 20)
    {
        const int c = gStep / 5;
        if (gStep % 5 == 0) progress(names[c], c, 5);
        ledController.showTestPattern((uint8_t)(1u << (gStep % 5)), colors[c], LED_TEST_INTENSITY);
    }
    else
    {
        progress("all white", 4, 5);
        ledController.showTestPattern(0x1F, 0xFFFFFF, LED_TEST_INTENSITY);
        gData["count"] = 5;
        ask("Did all 5 LEDs light red, green, blue and white?"); // all white stays lit meanwhile
    }
}

void startLeds()
{
    gStep = 0;
    gStepAt = millis();
    ledsShow();
}

void stepLeds()
{
    if (millis() - gStepAt < LED_STEP_MS) return;
    gStepAt = millis();
    gStep++;
    ledsShow();
}

void startButtons()
{
    for (int i = 0; i < 4; ++i)
    {
        gBtnSeen[i] = false;
        gBtnMs[i] = 0;
        // A button held at the start counts only after it is released and pressed again.
        gBtnPrev[i] = buttonController.isPressed((ButtonController::ButtonId)i);
    }
    gBtnCount = 0;
    gBtnWindowAt = millis();
    progress("press each button once", 0, 4);
}

void buttonsData()
{
    JsonArray seen = gData["seen"].to<JsonArray>();
    JsonArray ms = gData["ms"].to<JsonArray>();
    for (int i = 0; i < 4; ++i)
    {
        if (!gBtnSeen[i]) continue;
        seen.add(i + 1);
        ms.add((uint32_t)gBtnMs[i]);
    }
}

void stepButtons()
{
    const unsigned long now = millis();
    for (int i = 0; i < 4; ++i)
    {
        const bool cur = buttonController.isPressed((ButtonController::ButtonId)i);
        if (cur && !gBtnPrev[i] && !gBtnSeen[i])
        {
            gBtnSeen[i] = true;
            gBtnMs[i] = now - gStartedAt;
            gBtnCount++;
            gBtnWindowAt = now;
            ledController.flash(LedController::FX_BUTTON);
            progress(String("press button ") + (i + 1), gBtnCount, 4);
        }
        gBtnPrev[i] = cur;
    }
    if (gBtnCount == 4)
    {
        buttonsData();
        finish(true, "");
        return;
    }
    if (now - gBtnWindowAt > BUTTON_WAIT_MS)
    {
        String missing;
        for (int i = 0; i < 4; ++i)
        {
            if (gBtnSeen[i]) continue;
            if (missing.length()) missing += ",";
            missing += String(i + 1);
        }
        buttonsData();
        finish(false, "timeout, not seen: " + missing);
    }
}

void startNfc()
{
    const uint32_t v = nfcController.firmwareVersion();
    if (v == 0)
    {
        gData["reader"] = nullptr;
        gData["uid"] = nullptr;
        finish(false, "reader not answering");
        return;
    }
    char ver[12];
    snprintf(ver, sizeof(ver), "%u.%u", (unsigned)((v >> 16) & 0xFF), (unsigned)((v >> 8) & 0xFF));
    gData["reader"] = ver;
    if (!nfcController.isNFCReady())
    {
        gData["uid"] = nullptr;
        finish(false, "reader answers but was not initialised at boot");
        return;
    }
    gStepAt = millis();
    progress(String("reader ") + ver + ", put the test card on the pins", 1, 2);
}

void stepNfc()
{
    if (nfcController.isCardPresent())
    {
        const String uid = nfcController.currentUidCopy();
        if (uid.length())
        {
            gData["uid"] = uid;
            progress("card read", 2, 2);
            finish(true, "");
            return;
        }
    }
    if (millis() - gStepAt > NFC_WAIT_MS)
    {
        gData["uid"] = nullptr;
        finish(false, "no card read in 20 s");
    }
}

// pogo (closed = GPIO HIGH) and jack (plugged = AMP_CTRL pulled LOW)
bool pogoClosed() { return digitalRead(POGO_SWITCH_PIN) == HIGH; }
bool jackPlugged() { return digitalRead(AudioController::AMP_MODE_PIN) == LOW; }

void startSwitch(bool closedNow, const char *openHint, const char *closeHint)
{
    gSwitch.reset(closedNow);
    gClosedAt = 0;
    gStep = closedNow ? 0 : 1;
    gStepAt = millis();
    progress(closedNow ? openHint : closeHint, 0, 2);
}

// Returns true when the cycle closed -> open has been seen; fills held ms.
bool stepSwitch(bool level, const char *closeHint, const char *closedText, uint32_t &heldMs)
{
    if (!gSwitch.feed(level)) return false;
    if (gStep == 0 && !gSwitch.stable)
    {
        gStep = 1;
        progress(closeHint, 0, 2);
    }
    else if (gStep == 1 && gSwitch.stable)
    {
        gStep = 2;
        gClosedAt = millis();
        progress(closedText, 1, 2);
    }
    else if (gStep == 2 && !gSwitch.stable)
    {
        heldMs = (uint32_t)(millis() - gClosedAt);
        return true;
    }
    return false;
}

void startPogo()
{
    startSwitch(pogoClosed(), "remove the figure", "put the figure on the pins");
}

void stepPogo()
{
    uint32_t held = 0;
    if (stepSwitch(pogoClosed(), "put the figure on the pins", "closed, remove the figure", held))
    {
        progress("opened", 2, 2);
        gData["closed_ms"] = held;
        finish(true, "");
        return;
    }
    if (millis() - gStepAt > POGO_WAIT_MS)
    {
        finish(false, gStep == 2 ? "pogo switch did not open again" : "pogo switch did not close");
    }
}

void startJack()
{
    if (!audioController.isInitialized())
    {
        finish(false, "audio not initialised");
        return;
    }
    // The contact is only readable while the line is released (the codec stays muted).
    audioController.setAmpMode(AudioController::AMP_FORCE_ON);
    startSwitch(jackPlugged(), "pull the plug out", "insert the plug");
}

void stepJack()
{
    uint32_t held = 0;
    if (stepSwitch(jackPlugged(), "insert the plug", "inserted, pull it out", held))
    {
        progress("removed", 2, 2);
        gData["inserted_ms"] = held;
        finish(true, "");
        return;
    }
    if (millis() - gStepAt > JACK_WAIT_MS)
    {
        finish(false, gStep == 2 ? "plug not removed" : "no plug inserted in 20 s");
    }
}

void startSpeaker()
{
    if (!audioController.isInitialized())
    {
        gData["prompt"] = false;
        finish(false, "audio not initialised");
        return;
    }
    audioController.stop();
    audioController.setVolumeOverride(SPEAKER_TEST_VOLUME);
    if (!audioController.startTone(1000, 1000))
    {
        gData["prompt"] = false;
        finish(false, "tone could not start");
        return;
    }
    gStep = 1;
    gStepAt = millis();
    progress("tone 1 kHz", 0, 2);
}

void stepSpeaker()
{
    if (gStep == 1)
    {
        if (audioController.isToneActive()) return;
        const String path = prompts.path(Prompt::IDLE_HELLO);
        if (prompts.available(Prompt::IDLE_HELLO) && audioController.playPrompt(path))
        {
            gStep = 2;
            gStepAt = millis();
            progress("greeting", 1, 2);
            return;
        }
        gData["prompt"] = false;
    }
    else if (gStep == 2)
    {
        if (audioController.isPromptPlaying() && millis() - gStepAt < PROMPT_MAX_MS) return;
        if (audioController.isPromptPlaying()) audioController.stop();
        gData["prompt"] = true;
    }
    audioController.clearVolumeOverride();
    gStep = 3;
    ask("Was the tone and the greeting audible and clean?");
}

void runBattery()
{
    const bool gauge = battery.isGaugePresent() && battery.hasTelemetry();
    const int cellMv = mv(battery.getBatteryVoltage());
    const int temp = (int)lroundf(battery.getTemperatureC());
    // REG20 bit7 is IBAT regulation (a control loop, not a fault); REG21 bits 3 and 1-0 are reserved.
    const uint8_t f0 = battery.getChargerFaultStatus0() & 0x7F;
    const uint8_t f1 = battery.getChargerFaultStatus1() & 0xF4;
    const String faults = BatteryManager::describeChargerFaults(f0, f1);
    gData["mv"] = cellMv;
    gData["soc"] = (int)lroundf(battery.getBatteryPercentage());
    gData["temp_c"] = temp;
    gData["design_mah"] = battery.getDesignCapacityMilliAmpHours();
    JsonObject ch = gData["charger"].to<JsonObject>();
    ch["status"] = battery.getChargeStateName();
    ch["faults"] = faults;
    ch["ichg_ma"] = battery.getChargerIbatMilliAmps();
    gData["vbus_mv"] = mv(battery.getVbusVoltage());
    char err[64];
    if (!gauge)
    {
        finish(false, "gauge not answering");
    }
    else if (cellMv < 3000 || cellMv > 4250)
    {
        snprintf(err, sizeof(err), "cell %d mV outside 3000-4250", cellMv);
        finish(false, err);
    }
    else if (temp < -5 || temp > 50)
    {
        snprintf(err, sizeof(err), "temperature %d C outside -5..50", temp);
        finish(false, err);
    }
    else if (f0 || f1)
    {
        finish(false, "charger fault: " + faults);
    }
    else
    {
        finish(true, "");
    }
}

void runPd()
{
    uint8_t status = 0;
    uint16_t granted = 0;
    const char *mode = "none";
    if (battery.readUsbPdStatus(status, granted))
    {
        if (status & 0x08) mode = "PD";
        else if (status & 0x06) mode = "QC";
        else if (status & 0x01) mode = "BC";
    }
    const int vbus = mv(battery.getVbusVoltage());
    gData["mode"] = mode;
    gData["vbus_mv"] = vbus;
    if (vbus >= 4500)
    {
        finish(true, "");
        return;
    }
    char err[48];
    snprintf(err, sizeof(err), "VBUS %d mV below 4500", vbus);
    finish(false, err);
}

void startSd()
{
    if (!fileManager.isSDCardAvailable())
    {
        finish(false, "no card");
        return;
    }
    gData["size_mb"] = (uint32_t)(SD_MMC.cardSize() >> 20);
    gSdBuf = (uint8_t *)heap_caps_malloc(SD_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gSdBuf)
    {
        finish(false, "no memory for the test buffer");
        return;
    }
    if (!SD_MMC.exists(SD_TEST_DIR)) SD_MMC.mkdir(SD_TEST_DIR);
    gSdFile = SD_MMC.open(SD_TEST_FILE, FILE_WRITE);
    if (!gSdFile)
    {
        finish(false, "cannot create the test file");
        return;
    }
    gSdDone = 0;
    gSdWriteUs = 0;
    gSdReadUs = 0;
    gSdMismatch = 0;
    gStep = 1;
    progress("writing 2 MB", 0, 3);
}

float mbps(uint64_t us)
{
    if (us == 0) return 0.0f;
    const float v = (SD_TEST_BYTES / 1048576.0f) / (us / 1e6f);
    return roundf(v * 100.0f) / 100.0f;
}

void stepSd()
{
    if (millis() - gStartedAt > SD_BUDGET_MS)
    {
        finish(false, "sd test took longer than 60 s");
        return;
    }
    if (gStep == 1)
    {
        // One chunk per pass: at the slowest acceptable card (0.2 MB/s) that is 160 ms.
        const size_t n = std::min(SD_CHUNK, SD_TEST_BYTES - gSdDone);
        for (size_t j = 0; j < n; ++j) gSdBuf[j] = sdPattern(gSdDone + j);
        const int64_t t0 = esp_timer_get_time();
        const size_t w = gSdFile.write(gSdBuf, n);
        gSdWriteUs += esp_timer_get_time() - t0;
        if (w != n)
        {
            finish(false, String("write failed at ") + (unsigned)(gSdDone / 1024) + " KB");
            return;
        }
        gSdDone += n;
        if (gSdDone < SD_TEST_BYTES) return;
        const int64_t t1 = esp_timer_get_time();
        gSdFile.close(); // the flush belongs to the write time
        gSdWriteUs += esp_timer_get_time() - t1;
        gSdFile = SD_MMC.open(SD_TEST_FILE, FILE_READ);
        if (!gSdFile)
        {
            finish(false, "cannot reopen the test file");
            return;
        }
        gSdDone = 0;
        gStep = 2;
        progress("reading back", 1, 3);
        return;
    }
    if (gStep == 2)
    {
        const size_t n = std::min(SD_CHUNK, SD_TEST_BYTES - gSdDone);
        const int64_t t0 = esp_timer_get_time();
        const size_t r = gSdFile.read(gSdBuf, n);
        gSdReadUs += esp_timer_get_time() - t0;
        if (r != n)
        {
            finish(false, String("read failed at ") + (unsigned)(gSdDone / 1024) + " KB");
            return;
        }
        for (size_t j = 0; j < n; ++j)
        {
            if (gSdBuf[j] != sdPattern(gSdDone + j)) gSdMismatch++;
        }
        gSdDone += n;
        if (gSdDone < SD_TEST_BYTES) return;
        gSdFile.close();
        gStep = 3;
        progress("checking the prompt pack", 2, 3);
        return;
    }
    // gStep 3: rates and the pack
    const float w = mbps(gSdWriteUs);
    const float rd = mbps(gSdReadUs);
    gData["write_mbps"] = w;
    gData["read_mbps"] = rd;
    String checksum;
    int files = 0, missing = 0;
    const bool havePack = PromptPackManager::getInstance().installedStatus(checksum, files, missing);
    if (havePack)
    {
        gData["pack"] = checksum;
        gData["missing"] = missing;
    }
    else
    {
        gData["pack"] = nullptr;
        gData["missing"] = nullptr;
    }
    char err[64];
    if (gSdMismatch)
    {
        snprintf(err, sizeof(err), "read-back mismatch (%u bytes)", (unsigned)gSdMismatch);
        finish(false, err);
    }
    else if (w < 0.2f)
    {
        snprintf(err, sizeof(err), "write %.2f MB/s below 0.2", w);
        finish(false, err);
    }
    else if (rd < 1.0f)
    {
        snprintf(err, sizeof(err), "read %.2f MB/s below 1", rd);
        finish(false, err);
    }
    else if (!havePack)
    {
        finish(false, "no prompt pack (/sounds/pack.json)");
    }
    else if (missing > 0)
    {
        snprintf(err, sizeof(err), "prompt pack incomplete: %d of %d files missing", missing, files);
        finish(false, err);
    }
    else
    {
        finish(true, "");
    }
}

void wifiFailData(uint32_t ms)
{
    gData["rssi"] = nullptr;
    gData["ip"] = nullptr;
    gData["probe"] = false;
    gData["ms"] = ms;
}

void startWifi()
{
    gWifiWasMode = WiFi.getMode();
    gWifiWasConnected = WiFi.isConnected();
    gWifiStartedByUs = false;
    gWifiProbeStarted = false;
    gWifiTouched = true;
    wifiProv.setFactoryHold(true); // no reconnects, no mirroring of the test network
    if (gWifiWasMode == WIFI_MODE_NULL)
    {
        esp_err_t r = esp_event_loop_create_default();
        (void)r; // ESP_ERR_INVALID_STATE: it exists already
        WiFi.mode(WIFI_STA);
        gWifiStartedByUs = true;
    }
    // What the customer's network is, as the driver has it (from flash). The test runs with RAM
    // storage, so neither this nor Arduino's connect() ever writes the test network to flash.
    gWifiHadConfig = esp_wifi_get_config(WIFI_IF_STA, &gWifiSaved) == ESP_OK;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_disconnect();
    gStep = 1;
    gStepAt = millis();
    progress("connecting to " + gArg1, 0, 3);
}

void stepWifi()
{
    const unsigned long now = millis();
    if (gStep == 1)
    {
        // Wait for the old link to go (Arduino's connect() would otherwise wait for it itself).
        if (WiFi.isConnected() && now - gStepAt < WIFI_DISCONNECT_MS) return;
        WiFi.begin(gArg1.c_str(), gArg2.length() ? gArg2.c_str() : nullptr);
        gWifiConnectAt = now;
        gStep = 2;
        return;
    }
    if (gStep == 2)
    {
        if (WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0)
        {
            gData["rssi"] = (int)WiFi.RSSI();
            gData["ip"] = WiFi.localIP().toString();
            gData["ms"] = (uint32_t)(now - gWifiConnectAt);
            if (!wifiProv.hasProbeTarget())
            {
                gData["probe"] = false;
                finish(false, "no portal probe target configured");
                return;
            }
            gStep = 3;
            gStepAt = now;
            progress("connected, probing the portal", 1, 3);
            return;
        }
        if (now - gWifiConnectAt > WIFI_CONNECT_MS)
        {
            const int st = (int)WiFi.status();
            wifiFailData((uint32_t)(now - gWifiConnectAt));
            finish(false, String("no connection in 20 s (status ") + st + ")");
        }
        return;
    }
    // gStep 3: the probe (a loop probe still in flight finishes first)
    if (!gWifiProbeStarted)
    {
        gWifiProbeStarted = wifiProv.startFactoryProbe();
        if (!gWifiProbeStarted && now - gStepAt > WIFI_PROBE_MS)
        {
            gData["probe"] = false;
            finish(false, "probe could not start");
        }
        return;
    }
    const int r = wifiProv.factoryProbeResult();
    if (r < 0)
    {
        if (now - gStepAt > WIFI_PROBE_MS)
        {
            gData["probe"] = false;
            finish(false, "probe timeout");
        }
        return;
    }
    gData["probe"] = r == 1;
    progress("probe: " + wifiProv.factoryProbeWhy(), 2, 3);
    if (r == 1)
    {
        finish(true, "");
    }
    else
    {
        finish(false, String(r == 0 ? "captive or foreign answer: " : "no answer: ") + wifiProv.factoryProbeWhy());
    }
}

void startBle()
{
    String name, err;
    const int r = wifiProv.factoryBleStart(name, err);
    gData["name"] = name;
    if (r == 0)
    {
        finish(false, err);
        return;
    }
    gBleOwned = r == 1;
    gStepAt = millis();
    progress(String(r == 1 ? "advertising as " : "already advertising as ") + name, 0, 1);
}

void stepBle()
{
    if (millis() - gStepAt < BLE_ADVERTISE_MS) return;
    progress("stopped", 1, 1);
    finish(true, "");
}

int bringupIndex(const String &phase)
{
    if (phase == "provision") return 0;
    if (phase == "wait_charge") return 1;
    if (phase == "calibrate") return 2;
    return 3;
}

void startBringup()
{
    if (!bringupRunning())
    {
        bringupRestart("ft");
    }
    // else: the boot run is still going: the test follows it (same machine, same record)
    gBringupPhaseSeen = "";
}

void stepBringup()
{
    if (bringupRunning())
    {
        const String phase = bringupPhaseName();
        if (phase != gBringupPhaseSeen)
        {
            gBringupPhaseSeen = phase;
            progress(phase, bringupIndex(phase), 4);
        }
        if (millis() - gStartedAt > BRINGUP_BUDGET_MS)
        {
            bringupAbort((String("budget 3 min exceeded in ") + phase).c_str());
        }
        return;
    }
    const bool done = bringupLastResult() == BRINGUP_RESULT_DONE;
    gData["state"] = done ? "done" : "failed";
    embedRecord(gData["record"].to<JsonVariant>(), "bringup");
    String reason = "failed";
    if (!done && gData["record"]["reason"].is<const char *>())
    {
        reason = gData["record"]["reason"].as<String>();
    }
    finish(done, done ? String() : reason);
}

void startTest(const TestDef &def)
{
    gTest = def.id;
    gName = def.name;
    gStartedAt = millis();
    gStepAt = gStartedAt;
    gStep = 0;
    gAsking = false;
    gData.clear();
    {
        JsonDocument d;
        d["ft"] = "test";
        d["name"] = gName;
        d["state"] = "running";
        emit(d);
    }
    switch (def.id)
    {
    case Test::LEDS: startLeds(); break;
    case Test::BUTTONS: startButtons(); break;
    case Test::NFC: startNfc(); break;
    case Test::POGO: startPogo(); break;
    case Test::SPEAKER: startSpeaker(); break;
    case Test::JACK: startJack(); break;
    case Test::SD: startSd(); break;
    case Test::WIFI: startWifi(); break;
    case Test::BLE: startBle(); break;
    case Test::BRINGUP: startBringup(); break;
    case Test::SLEEP: finish(false, "not implemented"); break; // no shunt on this hardware
    default: break; // battery, pd: measured on the next pass
    }
}

void stepTest()
{
    switch (gTest)
    {
    case Test::LEDS: stepLeds(); break;
    case Test::BUTTONS: stepButtons(); break;
    case Test::NFC: stepNfc(); break;
    case Test::POGO: stepPogo(); break;
    case Test::SPEAKER: stepSpeaker(); break;
    case Test::JACK: stepJack(); break;
    case Test::BATTERY: runBattery(); break;
    case Test::PD: runPd(); break;
    case Test::SD: stepSd(); break;
    case Test::WIFI: stepWifi(); break;
    case Test::BLE: stepBle(); break;
    case Test::BRINGUP: stepBringup(); break;
    default: break;
    }
}

// ---- Commands --------------------------------------------------------------------------------

void replyBegin()
{
    JsonDocument d;
    d["ft"] = "begin";
    d["ok"] = true;
    d["uuid"] = getDeviceEfuseMacDecimal();
    d["mac"] = getDeviceEfuseMacHex();
    d["hw"] = TT_HARDWARE_REV;
    d["fw"] = firmwareVersion();
    d["code"] = firmwareVersionCode();
    d["enrolled"] = config.isEnrolled();
    setSerial(d);
    d["pop"] = config.factoryPop().length() == 8;

    JsonObject b = d["bringup"].to<JsonObject>();
    b["state"] = bringupStateName();
    embedRecord(b["record"].to<JsonVariant>(), "bringup");

    JsonObject card = d["card"].to<JsonObject>();
    const bool present = fileManager.isSDCardAvailable(); // mounted (the full system started)
    card["present"] = present;
    card["size_mb"] = present ? (uint32_t)(SD_MMC.cardSize() >> 20) : 0;
    if (!present) card["free_mb"] = 0;
    else if (fileManager.isFreeSpaceKnown()) card["free_mb"] = fileManager.getSDCardFreeMB();
    else
    {
        fileManager.getSDCardFreeMB(); // starts the free-cluster count for the next answer
        card["free_mb"] = nullptr;
    }

    JsonObject pack = d["pack"].to<JsonObject>();
    String checksum;
    int files = 0, missing = 0;
    if (present && PromptPackManager::getInstance().installedStatus(checksum, files, missing))
    {
        pack["checksum"] = checksum;
    }
    else
    {
        pack["checksum"] = nullptr;
    }
    pack["files"] = files;
    pack["missing"] = missing;

    JsonObject batt = d["battery"].to<JsonObject>();
    batt["mv"] = mv(battery.getBatteryVoltage());
    batt["soc"] = (int)lroundf(battery.getBatteryPercentage());
    batt["temp_c"] = (int)lroundf(battery.getTemperatureC());
    batt["charging"] = battery.getChargingStatus();
    batt["gauge"] = battery.isGaugePresent();
    d["vbus_mv"] = mv(battery.getVbusVoltage());
    if (config.factoryRecord("tested").length())
    {
        embedRecord(d["tested"].to<JsonVariant>(), "tested");
    }
    d["session"] = true;
    emit(d);
}

void replyShow(const char *verb)
{
    JsonDocument d;
    d["ft"] = verb;
    d["ok"] = true;
    d["uuid"] = getDeviceEfuseMacDecimal();
    if (strcmp(verb, "show") == 0)
    {
        d["mac"] = getDeviceEfuseMacHex();
    }
    setSerial(d);
    if (strcmp(verb, "show") == 0)
    {
        d["enrolled"] = config.isEnrolled();
    }
    d["label"] = FactoryTest::labelPayload(); // never the secret
    emit(d);
}

void cmdEnroll(const String &args)
{
    String parts[3];
    int n = 0;
    String rest = args;
    while (n < 3 && rest.length())
    {
        const int sp = rest.indexOf(' ');
        parts[n++] = sp > 0 ? rest.substring(0, sp) : rest;
        rest = sp > 0 ? rest.substring(sp + 1) : String();
        rest.trim();
    }
    if (n < 2 || rest.length())
    {
        replyError("enroll", "usage: ft enroll <secret> <pop> [serial]");
        return;
    }
    const String pop = normalisePop(parts[1]);
    if (!safeToken(parts[0], 16, 128))
    {
        replyError("enroll", "bad secret");
    }
    else if (pop.isEmpty())
    {
        replyError("enroll", "bad pop");
    }
    else if (n > 2 && !safeToken(parts[2], 1, 32))
    {
        replyError("enroll", "bad serial");
    }
    else if (!config.setFactory(parts[0], pop, n > 2 ? parts[2] : String()))
    {
        replyError("enroll", "nvs write failed");
    }
    else
    {
        replyShow("enroll"); // what is stored now (the BLE pop applies from the next provisioning start)
    }
}

void cmdTest(const String &args)
{
    if (!gActive)
    {
        replyError("test", "no session (ft begin)");
        return;
    }
    const int sp = args.indexOf(' ');
    const String name = sp > 0 ? args.substring(0, sp) : args;
    String targs = sp > 0 ? args.substring(sp + 1) : String();
    targs.trim();
    const TestDef *def = findTest(name);
    if (!def)
    {
        replyError("test", "unknown test");
        return;
    }
    if (gTest != Test::NONE || gShipArmed || (bringupRunning() && def->id != Test::BRINGUP))
    {
        replyError("test", "busy");
        return;
    }
    if (def->needsSystem && gSystemPhase != PHASE_RUNNING)
    {
        replyError("test", "system not started (bring-up pending or battery low)");
        return;
    }
    gArg1 = String();
    gArg2 = String();
    if (def->id == Test::WIFI)
    {
        // <ssid> has no spaces; the password is the rest of the line (may be empty: open network)
        const int sp2 = targs.indexOf(' ');
        gArg1 = sp2 > 0 ? targs.substring(0, sp2) : targs;
        gArg2 = sp2 > 0 ? targs.substring(sp2 + 1) : String();
        if (gArg1.isEmpty() || gArg1.length() > 32 || gArg2.length() > 64)
        {
            replyError("test", "usage: ft test wifi <ssid> <pass> (ssid 1-32 chars, pass 0-64)");
            return;
        }
    }
    startTest(*def);
}

void cmdAnswer(bool ok, const String &reason)
{
    const char *verb = ok ? "ok" : "fail";
    if (gTest == Test::NONE || !gAsking)
    {
        replyError(verb, "nothing asked");
        return;
    }
    replyOk(verb);
    finish(ok, ok ? String() : (reason.length() ? reason : String("operator said fail")));
}

void cmdResults()
{
    JsonDocument d;
    d["ft"] = "results";
    d["ok"] = true;
    bool all = gResults.size() > 0;
    for (JsonPairConst kv : gResults.as<JsonObjectConst>())
    {
        if (!kv.value()["pass"].as<bool>()) all = false;
    }
    d["pass"] = all;
    if (gResults.isNull()) d["tests"].to<JsonObject>();
    else d["tests"].set(gResults.as<JsonVariantConst>());
    emit(d);
}

// withData: the per-test data, except that the bring-up test keeps only its state (its record
// has its own key, "bringup").
bool buildTested(String &out, bool withData)
{
    JsonDocument rec;
    rec["tested_at"] = TimeSync::now(); // 0 while the clock is unknown
    rec["fw"] = firmwareVersion();
    bool all = gResults.size() > 0;
    JsonObject tests = rec["tests"].to<JsonObject>();
    for (JsonPairConst kv : gResults.as<JsonObjectConst>())
    {
        JsonObject t = tests[kv.key()].to<JsonObject>();
        const bool pass = kv.value()["pass"].as<bool>();
        if (!pass) all = false;
        t["pass"] = pass;
        t["ms"] = kv.value()["ms"].as<uint32_t>();
        if (withData && strcmp(kv.key().c_str(), "bringup") == 0)
        {
            t["data"]["state"] = kv.value()["data"]["state"];
        }
        else if (withData)
        {
            t["data"].set(kv.value()["data"]);
        }
        if (!pass)
        {
            // Without the data the record must fit even with every test failed: shorter errors.
            String e = kv.value()["error"].as<String>();
            if (!withData && e.length() > 40) e = e.substring(0, 40);
            t["error"] = e;
        }
    }
    rec["pass"] = all;
    out = String();
    serializeJson(rec, out);
    return out.length() <= TESTED_MAX_BYTES;
}

void cmdCommit()
{
    if (!gActive)
    {
        replyError("commit", "no session (ft begin)");
        return;
    }
    if (gTest != Test::NONE)
    {
        replyError("commit", "busy");
        return;
    }
    if (gResults.size() == 0)
    {
        replyError("commit", "no results");
        return;
    }
    // Under 1.5 KB in NVS: the per-test data goes first when the full record would not fit.
    String json;
    if (!buildTested(json, true) && !buildTested(json, false))
    {
        replyError("commit", "record too large");
        return;
    }
    if (!config.setFactoryRecord("tested", json))
    {
        replyError("commit", "nvs write failed");
        return;
    }
    gResults.clear();
    replyOk("commit");
}

void cmdShip()
{
    if (!gActive)
    {
        replyError("ship", "no session (ft begin)");
        return;
    }
    if (gTest != Test::NONE || gShipArmed)
    {
        replyError("ship", "busy");
        return;
    }
    if (!battery.hasTelemetry())
    {
        replyError("ship", "no battery telemetry");
        return;
    }
    if (battery.getBatteryVoltage() < SHIP_MIN_V || battery.getBatteryPercentage() < SHIP_MIN_SOC)
    {
        replyError("ship", "charge first");
        return;
    }
    if (!battery.armDelayedShipMode())
    {
        replyError("ship", "charger not answering");
        return;
    }
    gShipArmed = true;
    gShipAt = millis();
    ledController.pulseLed(0x00FF00); // green breathing: unplug now
    JsonDocument d;
    d["ft"] = "ship";
    d["ok"] = true;
    d["unplug_within_s"] = 10;
    emit(d);
}

void serviceShip()
{
    if (!gShipArmed) return;
    const unsigned long elapsed = millis() - gShipAt;
    if (battery.isVbusPresent())
    {
        if (elapsed < SHIP_CANCEL_MS) return;
        battery.cancelShipMode();
        gShipArmed = false;
        gLastLedAssert = 0;
        replyError("ship", "still powered");
        return;
    }
    // Unplugged: the charger opens the BATFET once its 10 s are over and the unit dies here.
    if (elapsed < SHIP_NOT_ENGAGED_MS) return;
    battery.cancelShipMode();
    gShipArmed = false;
    gLastLedAssert = 0;
    ledController.flash(LedController::FX_FAIL);
    ESP_LOGE(TAG, "ship mode did not engage (unplugged, still running) - cancelled");
    replyError("ship", "ship mode did not engage");
}

void cmdEnd()
{
    if (gTest != Test::NONE)
    {
        finish(false, "aborted");
    }
    if (gShipArmed)
    {
        battery.cancelShipMode();
        gShipArmed = false;
    }
    if (gActive)
    {
        deactivate();
    }
    replyOk("end");
}

void cmdReset(const char *verb, bool all, const char *why)
{
    if (gTest != Test::NONE || gShipArmed)
    {
        replyError(verb, "busy");
        return;
    }
    JsonDocument d;
    d["ft"] = verb;
    d["ok"] = true;
    if (strcmp(verb, "reset") == 0) d["reset"] = all ? "all" : "customer";
    emit(d);
    Serial.flush();
    if (WiFi.getMode() == WIFI_MODE_NULL)
    {
        // WiFi was never started this boot (deferred by the session): the driver must be up for
        // the reset to erase its stored network, or the next boot would reconnect to it.
        esp_event_loop_create_default();
        WiFi.mode(WIFI_STA);
    }
    // WiFi, token, card cache, fault ring, volume (and with --all the factory namespace); restarts.
    runCustomerReset(all, why, false);
}
} // namespace

namespace FactoryTest
{
bool active()
{
    return gActive;
}

String labelPayload()
{
    const String pop = config.factoryPop();
    if (pop.length() != 8) return String();
    return "TT1:" + getDeviceEfuseMacDecimal() + ":TK-" + pop.substring(0, 4) + "-" + pop.substring(4);
}

void command(const String &argsIn)
{
    String args = argsIn;
    args.trim();
    const int sp = args.indexOf(' ');
    const String verb = sp > 0 ? args.substring(0, sp) : args;
    String rest = sp > 0 ? args.substring(sp + 1) : String();
    rest.trim();

    if (verb == "begin")
    {
        if (!gActive) activate();
        replyBegin();
    }
    else if (verb == "end")
    {
        cmdEnd();
    }
    else if (verb == "show")
    {
        replyShow("show");
    }
    else if (verb == "enroll")
    {
        cmdEnroll(rest);
    }
    else if (verb == "reset")
    {
        const bool all = rest == "--all";
        if (rest.length() && !all)
        {
            replyError("reset", "usage: ft reset [--all]");
            return;
        }
        cmdReset("reset", all, all ? "ft reset --all" : "ft reset");
    }
    else if (verb == "clean")
    {
        cmdReset("clean", false, "ft clean");
    }
    else if (verb == "test")
    {
        cmdTest(rest);
    }
    else if (verb == "ok" || verb == "fail")
    {
        if (!gActive)
        {
            replyError(verb.c_str(), "no session (ft begin)");
            return;
        }
        cmdAnswer(verb == "ok", rest);
    }
    else if (verb == "abort")
    {
        if (gTest == Test::NONE)
        {
            replyError("abort", "no test running");
            return;
        }
        replyOk("abort");
        finish(false, "aborted");
    }
    else if (verb == "result")
    {
        if (!gActive)
        {
            replyError("results", "no session (ft begin)");
            return;
        }
        cmdResults();
    }
    else if (verb == "commit")
    {
        cmdCommit();
    }
    else if (verb == "ship")
    {
        cmdShip();
    }
    else
    {
        replyError("?", "unknown ft command");
    }
}

void update()
{
    if (!gActive)
    {
        return;
    }
    serviceShip();
    const unsigned long now = millis();
    if (gTest == Test::NONE)
    {
        // The idle picture comes back after anything else drew (bring-up's green, a test).
        if (!gShipArmed && !bringupRunning() && (gLastLedAssert == 0 || now - gLastLedAssert > 500))
        {
            gLastLedAssert = now ? now : 1;
            if (ledController.getScene() != LedController::SCENE_FACTORY)
            {
                ledController.setScene(LedController::SCENE_FACTORY);
            }
        }
        return;
    }
    if (gAsking)
    {
        if (now - gAskAt > ASK_TIMEOUT_MS)
        {
            finish(false, "no answer in 120 s");
        }
        return;
    }
    stepTest();
}
} // namespace FactoryTest
