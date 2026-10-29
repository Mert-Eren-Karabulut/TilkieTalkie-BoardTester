#include "Faults.h"
#include "ChargerSupervisor.h"
#include <esp_log.h>
#include "Bringup.h"
#include "Policy.h"
#include "SystemState.h"
#include "VoicePrompts.h"

static const char *TAG = "charger";

static VoicePrompts &prompts = VoicePrompts::getInstance();
static constexpr unsigned long KICK_BACKOFF_MS = 60UL * 60UL * 1000UL;

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
void superviseChargerOnUsb()
{
    static uint8_t pdBudgetChecksLeft = 0;
    static unsigned long lastPdBudgetCheck = 0;
    static unsigned long lastChargeKick = 0;
    static uint16_t requestedChargeCurrentMa = 0;
    static bool wasVbusPresent = false;
    static bool chargeSuspendedByTemp = false;
    static int tempOutOfWindowPolls = 0;
    static bool tempDerated = false;
    static bool fullHold = false;          // charger paused because the pack is full
    static unsigned long vbusSessionStartedAt = 0;
    // Input budget for sources without a PD contract. The charger's POR default (3.3 A + ICO
    // probing upwards) collapses weak 5 V adapters and power banks: VBUS drops out, the charger
    // stops, the source recovers, and the cycle repeats every few seconds (the red STAT LED
    // blinking). 1 A is enough for the 1 A charge current of a 1S LiFePO4 pack, and a source
    // that still drops out gets the limit halved for the rest of the session.
    static uint16_t nonPdInputLimitMa = 1000;
    static bool nonPdLimitApplied = false;
    static uint8_t vbusDropouts = 0;
    static unsigned long firstDropoutAt = 0;

    if (gGaugeBusReleased)
    {
        return; // bqStudio session: the charger keeps the settings written at boot
    }
    if (!battery.isVbusPresent())
    {
        if (wasVbusPresent)
        {
            wasVbusPresent = false;
            nonPdLimitApplied = false;
            if (millis() - gFullSystemStartedAt > 15000UL)
            {
                prompts.say(Prompt::TONE_CHARGE_OFF);
            }
            if (firstDropoutAt == 0 || millis() - firstDropoutAt > 120000UL)
            {
                firstDropoutAt = millis();
                vbusDropouts = 0;
            }
            if (++vbusDropouts >= 3 && nonPdInputLimitMa > 500)
            {
                nonPdInputLimitMa = 500;
                vbusDropouts = 0;
                ESP_LOGW(TAG, "CHARGER: the source dropped out three times in two minutes - non-PD input limit now %u mA", nonPdInputLimitMa);
            }
            // The next session starts from the conservative budget, never from the 3.3 A default.
            battery.setInputCurrentLimit(nonPdInputLimitMa);
            battery.setIcoEnabled(false);
        }
        return;
    }
    if (!wasVbusPresent)
    {
        vbusSessionStartedAt = millis() ? millis() : 1;
        if (millis() - gFullSystemStartedAt > 15000UL)
        {
            prompts.say(Prompt::TONE_CHARGE_ON); // a plug-in, not the cable that was there at boot
        }
        // New VBUS session: conservative budget right away, then give the CH224Q a few polls
        // to report a PD contract that may raise it.
        wasVbusPresent = true;
        pdBudgetChecksLeft = 5;
        lastPdBudgetCheck = 0;
        battery.setInputCurrentLimit(nonPdInputLimitMa);
        battery.setIcoEnabled(false);
        fullHold = false;
        battery.setChargeEnabled(true); // a new session always charges (EN_CHG off sags VSYS on this board)
    }

    if (gBringupPhase != BRINGUP_INACTIVE || gLearnDischarge || gFactoryLoadStep || !battery.hasTelemetry())
    {
        return;
    }

    const float v = battery.getBatteryVoltage();
    const float t = battery.getTemperatureC();

    // Hard temperature window (datasheet: charge 0..60°C), 2°C re-entry hysteresis.
    if (t >= 0.0f && t <= 60.0f)
    {
        tempOutOfWindowPolls = 0;
    }
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
    else if ((t < 0.0f || t > 60.0f) && ++tempOutOfWindowPolls < 3)
    {
        // one reading outside the window is not a reason to stop charging; three in a row is
        return;
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
        wanted = Policy::CHARGE_CURRENT_SAFE_MA;  // weak/high-ESR cell: the BAT node must not overshoot VBAT OVP
    }
    else if (v > 3.15f)
    {
        // Cell allows 0.5C (2 A) but the 1.0 mm-pitch single-contact cell connector (CN33)
        // and the ~100 mOhm cell-to-PACK path do not: the charger was in CV at 1.4 A anyway.
        wanted = Policy::CHARGE_CURRENT_FAST_MA;
    }
    if (wanted == 0)
    {
        wanted = Policy::CHARGE_CURRENT_SAFE_MA;
    }
    if (tempDerated && wanted > 800)
    {
        wanted = 800;  // datasheet 0.2C recommended rate near the temperature edges
    }
    if (bringupChargeCapped() && wanted > Policy::CHARGE_CURRENT_SAFE_MA)
    {
        // Bring-up gave up with an uncalibrated gauge: it over-reads ~4.5x and would trip OCC
        // at the normal current (fault 508 is active; the next bring-up lifts the cap).
        wanted = Policy::CHARGE_CURRENT_SAFE_MA;
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
        const bool pdRead = battery.readUsbPdStatus(pdStatus, grantedMa);
        if (pdRead && (pdStatus & 0x08) && grantedMa >= 500)
        {
            // Diagnostic record of what this source negotiated (readable from the console and
            // the hub page): voltage and budget, so a picky adapter can be judged afterwards.
            char d[40];
            snprintf(d, sizeof(d), "pd %umV %umA", (unsigned)(battery.getVbusVoltage() * 1000.0f), (unsigned)grantedMa);
            Faults::raise(Fault::USB_SOURCE, d, false);
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
        else if (pdBudgetChecksLeft == 0 && !nonPdLimitApplied)
        {
            nonPdLimitApplied = true;
            char d[40];
            snprintf(d, sizeof(d), "%s %umV no-pd", pdRead ? "ch224" : "no-ch224", (unsigned)(battery.getVbusVoltage() * 1000.0f));
            Faults::raise(Fault::USB_SOURCE, d, false);
            ESP_LOGI(TAG, "CHARGER: no PD contract - input limit %u mA, ICO off.", nonPdInputLimitMa);
        }
    }

    // Full-charge hold. When the gauge decides the pack is full it sets XCHG and opens its CHG
    // FET; a charger that keeps trying then sees its battery node float, trips VBAT-OVP and
    // retries once a second (the blinking STAT LED, the charging/discharging flips). Honour the
    // gauge (or the charger's own termination): disable charging and leave it off until the
    // cell has really come down. VSYS runs from VBUS meanwhile, so the cell rests.
    if (!fullHold && (battery.isGaugeChargeInhibited() || battery.isChargeDone()))
    {
        fullHold = true;
        ESP_LOGI(TAG, "CHARGER: pack full (%s) at %.3fV - no more charge kicks until the cell drops",
                 battery.isGaugeChargeInhibited() ? "gauge FC" : "charger termination", v);
        if (vbusSessionStartedAt && millis() - vbusSessionStartedAt > 120000UL)
        {
            prompts.say(Prompt::CHARGE_FULL); // it charged in this session; not "full" for a cable that was there at boot
        }
    }
    if (fullHold)
    {
        const bool cellDown = v < 3.30f || (battery.hasStateOfCharge() && battery.getBatteryPercentage() < 90.0f);
        if (cellDown && !battery.isGaugeChargeInhibited() && !battery.isChargeDone())
        {
            fullHold = false;
            ESP_LOGI(TAG, "CHARGER: cell at %.3fV - normal charge supervision resumed", v);
        }
        return; // no tiering or kicking while holding
    }

    // Never "kick" after Charge Done: toggling EN_CHG there starts a fresh CC/CV cycle every
    // 10 s, so the cell never rests at full (which the gauge needs to learn capacity). "Full" is
    // the gauge's state of charge (95 % and up), with the cell itself as the backstop while the
    // gauge is silent: a LiFePO4 cell that rests at or above 3.40 V with no current is above
    // ~95 %. And a charger that drops out three times in a row is not going to be fixed by a
    // fourth kick: back off for an hour.
    static uint8_t kicksInRow = 0;
    static bool kickBackoff = false;           // three kicks failed: wait an hour
    static unsigned long kickBackoffSince = 0; // wrap-safe: elapsed time since the back-off began
    const bool cellLooksFull = (battery.hasStateOfCharge() && battery.getBatteryPercentage() >= 95.0f)
        || (v >= 3.40f && fabsf((float)battery.getBatteryCurrentMilliAmps()) < 60.0f);
    if (battery.getChargingStatus() && millis() - lastChargeKick > 60000)
    {
        kicksInRow = 0; // a full minute of charging after a kick: the kicks were useful
    }
    static unsigned long notChargingSince = 0;
    if (battery.getChargingStatus())
    {
        notChargingSince = 0;
    }
    else if (notChargingSince == 0)
    {
        notChargingSince = millis();
    }
    const bool offLongEnough = notChargingSince != 0 && millis() - notChargingSince > 20000; // gauge sample is fresh by then
    if (kickBackoff && millis() - kickBackoffSince >= KICK_BACKOFF_MS)
    {
        kickBackoff = false; // the hour is over
    }
    if (offLongEnough && !battery.isChargeDone() && !cellLooksFull &&
        !kickBackoff && millis() - lastChargeKick > 10000)
    {
        lastChargeKick = millis();
        if (++kicksInRow > 3)
        {
            kicksInRow = 0;
            kickBackoff = true;
            kickBackoffSince = millis();
            ESP_LOGW(TAG, "CHARGER: three kicks did not keep the charger on (%.3fV) — backing off for an hour.", v);
        }
        else
        {
            ESP_LOGW(TAG, "CHARGER: USB present but not charging — clearing faults + kicking charger (%u/3).", kicksInRow);
            battery.clearChargerFaultHistory();
            battery.restartChargeCycle();
        }
    }
}
