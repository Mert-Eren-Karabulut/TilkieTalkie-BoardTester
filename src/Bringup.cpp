#include "Bringup.h"
#include <esp_log.h>
#include <ArduinoJson.h>
#include "Faults.h"
#include "Policy.h"
#include "SystemState.h"
#include "TimeSync.h"
#include "Version.h"

static const char *TAG = "bringup";

BringupPhase gBringupPhase = BRINGUP_INACTIVE;
static const char *BRINGUP_NVS_KEY = "bringup_v1";
// Bring-up LED phase colors (docs/MANUFACTURING.md):
static const uint32_t BRINGUP_LED_PROVISION = 0x0000FF; // solid BLUE   - provisioning
static const uint32_t BRINGUP_LED_WAIT = 0xFF6000;      // pulsing AMBER - waiting for charge
static const uint32_t BRINGUP_LED_CALIBRATE = 0x00FFFF; // solid CYAN   - calibrating
static const uint32_t BRINGUP_LED_SUCCESS = 0x00FF00;   // solid GREEN  - complete

// A full cell or a unit without USB never produces a charge current: the wait gives up instead
// of keeping the toy awake (and the system deferred) forever.
static const unsigned long CHARGE_WAIT_TIMEOUT_MS = 3UL * 60UL * 1000UL;
// The gauge may sit in ROM mode for a long time on a recovering cell; on USB that is a charge
// situation worth waiting for, on battery alone it only drains the cell.
static const unsigned long GAUGE_WAIT_ON_BATTERY_TIMEOUT_MS = 3UL * 60UL * 1000UL;

// One run of the machine
static unsigned long gLastTick = 0;
static int gCalAttempts = 0;
static int gProvisionAttempts = 0;
static unsigned long gRunStartedMs = 0;
static uint32_t gRunStartedAt = 0;          // unix, 0 while the clock is unknown
static unsigned long gWaitChargeSince = 0;  // first entry into WAIT_CHARGE of this run
static unsigned long gGaugeWaitSince = 0;   // gauge not answering at 0x55 since (on battery)
static const char *gTrigger = "boot";
static BringupResult gLastResult = BRINGUP_RESULT_NONE;
static bool gChargeCapped = false;

static void startRun(const char *trigger)
{
    gLastTick = 0;
    gCalAttempts = 0;
    gProvisionAttempts = 0;
    gRunStartedMs = millis();
    gRunStartedAt = TimeSync::now();
    gWaitChargeSince = 0;
    gGaugeWaitSince = 0;
    gTrigger = trigger ? trigger : "?";
    gBringupPhase = BRINGUP_PROVISION;
}

// The record of a run (factory namespace key "bringup", also in `ft begin` and the `bringup`
// test's data). Small: one line of JSON, well under the NVS string limit.
static void writeRecord(bool done, const char *reason)
{
    JsonDocument doc;
    doc["state"] = done ? "done" : "failed";
    doc["reason"] = reason ? reason : "";
    doc["trigger"] = gTrigger;
    doc["started_at"] = gRunStartedAt;
    doc["ended_at"] = TimeSync::now();
    doc["ms"] = (uint32_t)(millis() - gRunStartedMs);
    doc["fw"] = firmwareVersion();
    JsonObject g = doc["gauge"].to<JsonObject>();
    const bool present = battery.isGaugePresent();
    g["present"] = present;
    // The data-flash checks only make sense (and only answer quickly) with a gauge on the bus.
    g["provisioned"] = present && battery.isGaugeProvisioned();
    g["calibrated"] = present && battery.isCurrentSenseCalibrated();
    char chem[8];
    snprintf(chem, sizeof(chem), "0x%04X", (unsigned)battery.getGaugeChemId());
    g["chem_id"] = chem;
    g["image"] = battery.goldenImageStateName(); // learned image: applied / stock / learning / inhibited / failed
    g["design_mah"] = battery.getDesignCapacityMilliAmpHours();
    g["mv"] = (int)lroundf(battery.getBatteryVoltage() * 1000.0f);
    g["temp_c"] = (int)lroundf(battery.getTemperatureC());
    g["current_ma"] = battery.getBatteryCurrentMilliAmps();
    JsonObject c = doc["charger"].to<JsonObject>();
    c["vbus_mv"] = (int)lroundf(battery.getVbusVoltage() * 1000.0f);
    c["ichg_ma"] = battery.getChargerIbatMilliAmps();
    c["state"] = battery.getChargeStateName();
    doc["provision_attempts"] = gProvisionAttempts;
    doc["cal_attempts"] = gCalAttempts;
    String json;
    serializeJson(doc, json);
    if (!config.setFactoryRecord("bringup", json))
    {
        ESP_LOGE(TAG, "BRINGUP: record could not be written");
    }
}

// Every failure ends here: record, fault, normal operation (sleep allowed again) with the safe
// charge current kept while the gauge is not calibrated.
static void failRun(const char *reason)
{
    ESP_LOGE(TAG, "BRINGUP: FAILED - %s", reason);
    gBringupPhase = BRINGUP_FAILED;
    writeRecord(false, reason);
    gLastResult = BRINGUP_RESULT_FAILED;
    gChargeCapped = !(battery.isGaugePresent() && battery.isCurrentSenseCalibrated());
    battery.setChargeCurrent(Policy::CHARGE_CURRENT_SAFE_MA);
    char detail[40];
    strlcpy(detail, reason, sizeof(detail));
    Faults::raise(Fault::BRINGUP_FAILED, detail);
    sleepController.setInactivityTimeout(Policy::INACTIVITY_TIMEOUT_MS);
    sleepController.resetActivity();
    if (gSystemPhase == PHASE_MONITOR)
    {
        ledController.pulseLed(SYSTEM_MONITOR_LED); // the monitor scene; the red flash below says why
    }
    else
    {
        ledController.setScene(LedController::SCENE_OFF); // deriveLedScene() takes over
    }
    ledController.flash(LedController::FX_FAIL);
    gBringupPhase = BRINGUP_INACTIVE;
}

void bringupBeginIfNeeded()
{
    // First-boot bring-up: if this board has never been provisioned + current-calibrated,
    // run the auto bring-up (LED-guided) and hold the charge current at a safe 300mA until
    // the gauge current sense is calibrated (an uncalibrated gauge over-reads and trips OCC).
    if (!bringupDone() && !gGaugeBusReleased)
    {
        battery.setChargeCurrent(Policy::CHARGE_CURRENT_SAFE_MA);
        startRun("boot");
        ESP_LOGW(TAG, "FIRST BOOT: gauge bring-up required — running auto provision+calibration.");
        ESP_LOGW(TAG, "Watch the LED (blue=provision, amber=needs charge, cyan=calibrate, green=done).");
    }
}

void bringupRestart(const char *trigger)
{
    config.storeInt(BRINGUP_NVS_KEY, 0);
    battery.setChargeCurrent(Policy::CHARGE_CURRENT_SAFE_MA);
    sleepController.setInactivityTimeout(Policy::INACTIVITY_TIMEOUT_NEVER);
    startRun(trigger);
}

void bringupAbort(const char *reason)
{
    if (bringupRunning())
    {
        failRun(reason ? reason : "aborted");
    }
}

bool bringupRunning()
{
    return gBringupPhase != BRINGUP_INACTIVE && gBringupPhase != BRINGUP_FAILED;
}

bool bringupDone()
{
    return config.getInt(BRINGUP_NVS_KEY, 0) != 0;
}

BringupResult bringupLastResult()
{
    return gLastResult;
}

bool bringupChargeCapped()
{
    return gChargeCapped;
}

const char *bringupPhaseName()
{
    switch (gBringupPhase)
    {
    case BRINGUP_PROVISION: return "provision";
    case BRINGUP_WAIT_CHARGE: return "wait_charge";
    case BRINGUP_CALIBRATE: return "calibrate";
    case BRINGUP_COMPLETE: return "complete";
    case BRINGUP_FAILED: return "failed";
    default: return "idle";
    }
}

const char *bringupStateName()
{
    if (bringupRunning())
    {
        return "running";
    }
    if (bringupDone())
    {
        return "done";
    }
    if (gLastResult == BRINGUP_RESULT_FAILED)
    {
        return "failed";
    }
    // An earlier boot's run failed (the record survives reboots; the flag is still clear).
    JsonDocument rec;
    const String json = config.factoryRecord("bringup");
    if (json.length() && deserializeJson(rec, json) == DeserializationError::Ok && rec["state"] == "failed")
    {
        return "failed";
    }
    return "pending";
}

void updateBringup()
{
    if (!bringupRunning())
    {
        return;
    }

    unsigned long now = millis();

    switch (gBringupPhase)
    {
    case BRINGUP_PROVISION:
        ledController.setMaxBrightness(255);
        ledController.simpleLed(BRINGUP_LED_PROVISION, 255); // solid blue
        // The gauge boots in ROM mode (0x0B) for several seconds (longer after a deep
        // discharge) and only answers at 0x55 once the cell has charged a little. On USB this
        // is a charging/recovery situation, not a fault: wait. On battery alone nothing will
        // change it, so the wait is bounded there.
        if (!battery.isGaugeAlive())
        {
            if (battery.isVbusPresent())
            {
                gGaugeWaitSince = 0;
            }
            else if (gGaugeWaitSince == 0)
            {
                gGaugeWaitSince = now ? now : 1;
            }
            else if (now - gGaugeWaitSince > GAUGE_WAIT_ON_BATTERY_TIMEOUT_MS)
            {
                failRun("gauge not answering (no USB, 3 min)");
                break;
            }
            if (now - gLastTick > 2000)
            {
                gLastTick = now;
                ESP_LOGW(TAG, "BRINGUP: waiting for gauge to come up (charging / recovery)...");
            }
            break;
        }
        gGaugeWaitSince = 0;
        // Gauge answers at 0x55. If it is blank (e.g. begin() ran while it was still in ROM
        // mode, or a genuinely fresh gauge), provision it now — safe here because we are in
        // monitor mode (minimal load), so the provisioning DEVICE_RESET won't brown out.
        if (!battery.isGaugeProvisioned())
        {
            if (++gProvisionAttempts > 5)
            {
                failRun("gauge provisioning does not stick");
                break;
            }
            ESP_LOGW(TAG, "BRINGUP: gauge unprovisioned — provisioning now (attempt %d)...", gProvisionAttempts);
            battery.forceGaugeProvisioning();
            battery.update();
            gLastTick = now;
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
        battery.setChargeCurrent(Policy::CHARGE_CURRENT_SAFE_MA); // safe while the gauge is still uncalibrated
        gCalAttempts = 0;
        gLastTick = now;
        gWaitChargeSince = now ? now : 1;
        gBringupPhase = BRINGUP_WAIT_CHARGE;
        ledController.pulseLed(BRINGUP_LED_WAIT); // amber pulse = needs charge current
        ESP_LOGW(TAG, "BRINGUP: waiting for charge current — ensure USB-C power, cell not full.");
        break;

    case BRINGUP_WAIT_CHARGE:
        if (now - gLastTick < 1500)
        {
            break;
        }
        gLastTick = now;
        ledController.pulseLed(BRINGUP_LED_WAIT); // re-assert amber
        if (battery.getChargingStatus())
        {
            gBringupPhase = BRINGUP_CALIBRATE;
        }
        else if (gWaitChargeSince && now - gWaitChargeSince > CHARGE_WAIT_TIMEOUT_MS)
        {
            if (!battery.isVbusPresent())
            {
                failRun("no USB power, no charge current in 3 min");
            }
            else if (battery.isChargeDone() || battery.isGaugeChargeInhibited())
            {
                failRun("cell full, no charge current in 3 min");
            }
            else
            {
                failRun("no charge current in 3 min");
            }
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
        else if (++gCalAttempts >= 5)
        {
            failRun("current calibration failed 5 times");
        }
        else
        {
            ESP_LOGW(TAG, "BRINGUP: calibration not ready (current not steady), waiting...");
            gBringupPhase = BRINGUP_WAIT_CHARGE; // the charge wait keeps its first start time
            ledController.pulseLed(BRINGUP_LED_WAIT);
            gLastTick = now;
        }
        break;

    case BRINGUP_COMPLETE:
        config.storeInt(BRINGUP_NVS_KEY, 1);
        battery.setChargeCurrent(Policy::CHARGE_CURRENT_FAST_MA); // calibrated now -> safe to raise
        gChargeCapped = false;
        // The normal inactivity timeout (5 min), not the 30 s value it used to restore: a freshly
        // brought-up toy slept after 30 s of inactivity until its next reboot.
        sleepController.setInactivityTimeout(Policy::INACTIVITY_TIMEOUT_MS);
        sleepController.resetActivity();
        ledController.setMaxBrightness(255);
        ledController.simpleLed(BRINGUP_LED_SUCCESS, 255); // solid green
        writeRecord(true, "");
        gLastResult = BRINGUP_RESULT_DONE;
        Faults::clear(Fault::BRINGUP_FAILED);
        ESP_LOGI(TAG, "BRINGUP: COMPLETE — gauge provisioned + current calibrated. Starting system.");
        gBringupPhase = BRINGUP_INACTIVE;
        break;

    default:
        break;
    }
}
