#include "DeadCellRecovery.h"
#include <esp_log.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
// esp_brownout_disable() has no public equivalent in IDF 5.5 (the brownout driver API is
// private); the recovery hold needs it to reach deep sleep through a survivable dip.
#include <esp_private/brownout.h>
#include "Policy.h"
#include "SystemState.h"
#include "VoicePrompts.h"

static const char *TAG = "power";

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
void runDeadCellRecoveryHold()
{
    if (!battery.isVbusPresent())
    {
        return;
    }
    const float v = battery.getBatteryVoltage();
    if (v >= Policy::DEAD_CELL_RECOVERY_ENTER_V)
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
    battery.setChargeCurrent(Policy::CHARGE_CURRENT_SAFE_MA); // gentle: a weak/high-ESR cell must not overshoot VBAT OVP

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

// Low-battery cutoff: BATTERY-ONLY. With VBUS present this must never fire — the NVDC
// power path runs SYS from USB regardless of the cell, and force-sleeping on USB is
// what abandoned charger recovery (the "sleep with charger attached" bootloop). At
// Policy::LOW_BATTERY_CUTOFF_V (debounced) the cell is disconnected (ship mode, wake = USB
// plug-in); see the INTERIM POLICY note below for why it is not a deep sleep.
void serviceLowBatteryCutoff()
{
    static unsigned long lowBattSince = 0;
    if (!gLearnDischarge && !battery.isVbusPresent() &&
        !battery.getChargingStatus() && battery.hasTelemetry())
    {
        float v = battery.getBatteryVoltage();
        if (v > 0.5f && v <= Policy::LOW_BATTERY_CUTOFF_V) // ignore obviously bogus reads
        {
            if (lowBattSince == 0)
            {
                lowBattSince = millis();
            }
            else if (millis() - lowBattSince >= Policy::LOW_BATTERY_CUTOFF_DEBOUNCE_MS)
            {
                ESP_LOGW(TAG, "LOW BATTERY %.3fV (<= %.2fV), on battery — protecting the cell.",
                         v, Policy::LOW_BATTERY_CUTOFF_V);
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
                VoicePrompts::getInstance().sayBlocking(Prompt::BATTERY_CRITICAL, 6000);
                ESP_LOGW(TAG, "Disconnecting cell (gauge FETs open) - plug USB to wake.");
                nfcController.powerDown();   // PN532 is on the always-on rail
                // Adem v7 has no ship FET on the charger's SDRV pin (only a capacitor), so the
                // charger's ship mode does nothing here; the gauge's protection FETs cut the cell.
                battery.openGaugeFets();     // on battery power this does not return
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
