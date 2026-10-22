#include "Bringup.h"
#include <esp_log.h>
#include "Policy.h"
#include "SystemState.h"

static const char *TAG = "bringup";

BringupPhase gBringupPhase = BRINGUP_INACTIVE;
static const char *BRINGUP_NVS_KEY = "bringup_v1";
// Bring-up LED phase colors (documented in FACTORY_BRINGUP.md):
static const uint32_t BRINGUP_LED_PROVISION = 0x0000FF; // solid BLUE   - provisioning
static const uint32_t BRINGUP_LED_WAIT = 0xFF6000;      // pulsing AMBER - waiting for charge
static const uint32_t BRINGUP_LED_CALIBRATE = 0x00FFFF; // solid CYAN   - calibrating
static const uint32_t BRINGUP_LED_SUCCESS = 0x00FF00;   // solid GREEN  - complete
static const uint32_t BRINGUP_LED_FAIL = 0xFF0000;      // pulsing RED  - failed

void bringupBeginIfNeeded()
{
    // First-boot bring-up: if this board has never been provisioned + current-calibrated,
    // run the auto bring-up (LED-guided) and hold the charge current at a safe 300mA until
    // the gauge current sense is calibrated (an uncalibrated gauge over-reads and trips OCC).
    const bool bringupDone = config.getInt(BRINGUP_NVS_KEY, 0) != 0;
    if (!bringupDone && !gGaugeBusReleased)
    {
        battery.setChargeCurrent(Policy::CHARGE_CURRENT_SAFE_MA);
        gBringupPhase = BRINGUP_PROVISION;
        ESP_LOGW(TAG, "FIRST BOOT: gauge bring-up required — running auto provision+calibration.");
        ESP_LOGW(TAG, "Watch the LED (blue=provision, amber=needs charge, cyan=calibrate, green=done).");
    }
}

void bringupRestart()
{
    config.storeInt(BRINGUP_NVS_KEY, 0);
    battery.setChargeCurrent(Policy::CHARGE_CURRENT_SAFE_MA);
    sleepController.setInactivityTimeout(Policy::INACTIVITY_TIMEOUT_NEVER);
    gBringupPhase = BRINGUP_PROVISION;
}

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
        battery.setChargeCurrent(Policy::CHARGE_CURRENT_SAFE_MA); // safe while the gauge is still uncalibrated
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
        battery.setChargeCurrent(Policy::CHARGE_CURRENT_FAST_MA); // calibrated now -> safe to raise
        // The normal inactivity timeout (5 min), not the 30 s value it used to restore: a freshly
        // brought-up toy slept after 30 s of inactivity until its next reboot.
        sleepController.setInactivityTimeout(Policy::INACTIVITY_TIMEOUT_MS);
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
