#pragma once

#include <Arduino.h>

// Gauge bring-up: auto gauge provisioning + current-sense calibration. Runs at boot until it
// has passed once (NVS flag), in battery-monitor mode at minimal load, and again on demand
// (`ft test bringup`, bench `bringupreset`). The LED shows the phase (docs/MANUFACTURING.md).
//
// It never holds the toy hostage: the charge-current wait gives up after 3 minutes (a full cell
// or no USB never produce one), and any failure falls back to normal operation with fault 508
// BRINGUP_FAILED and the charge current capped at the safe 300 mA (an uncalibrated gauge
// over-reads and trips OCC at the normal current). Every run ends with a record in the factory
// namespace (key "bringup"): state, timestamps, gauge data, the failure reason.
enum BringupPhase
{
    BRINGUP_INACTIVE,    // not running: done, never needed this boot, or given up (see bringupLastResult)
    BRINGUP_PROVISION,   // write the LiFePO4 gauge config (1S, capacity, thresholds)
    BRINGUP_WAIT_CHARGE, // wait for a steady charge current (USB power, cell not full), 3 min at most
    BRINGUP_CALIBRATE,   // calibrate current sense against the charger IBAT ADC
    BRINGUP_COMPLETE,    // mark done in NVS, raise charge current, show success
    BRINGUP_FAILED       // transient: records the failure and falls back to INACTIVE on the next pass
};
extern BringupPhase gBringupPhase;

// How the last run of this boot ended.
enum BringupResult
{
    BRINGUP_RESULT_NONE,   // no run ended this boot
    BRINGUP_RESULT_DONE,
    BRINGUP_RESULT_FAILED
};

// setup(): if this board has never passed bring-up, hold the charge current at a safe 300 mA
// and start it. No-op when done or the bus is released.
void bringupBeginIfNeeded();
// Drives the bring-up state machine. Called from loop() in every phase.
void updateBringup();
// `ft test bringup` / bench `bringupreset`: clear the NVS flag and run the machine again.
void bringupRestart(const char *trigger = "console");
// `ft test bringup` budget / `ft abort`: stop a running machine as a failure with this reason.
void bringupAbort(const char *reason);

bool bringupRunning();               // PROVISION .. COMPLETE
bool bringupDone();                  // the NVS flag: passed once on this board
BringupResult bringupLastResult();   // this boot
const char *bringupPhaseName();      // "provision", "wait_charge", "calibrate", "complete", "failed", "idle"
// "done" | "pending" | "failed" | "running" for `ft begin` (failed: the last record says so).
const char *bringupStateName();
// After a failed run without a calibrated gauge: ChargerSupervisor keeps 300 mA.
bool bringupChargeCapped();
