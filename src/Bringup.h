#pragma once

// First-boot bring-up: auto gauge provisioning + current-sense calibration. Runs once per
// board (gated by an NVS flag), in battery-monitor mode at minimal load. The LED shows the
// phase (FACTORY_BRINGUP.md).
enum BringupPhase
{
    BRINGUP_INACTIVE,    // already done (NVS flag set) or finished -> normal operation
    BRINGUP_PROVISION,   // write the LiFePO4 gauge config (1S, chem, capacity, thresholds)
    BRINGUP_WAIT_CHARGE, // wait for a steady charge current (technician: ensure USB power)
    BRINGUP_CALIBRATE,   // calibrate current sense against the charger IBAT ADC
    BRINGUP_COMPLETE,    // mark done in NVS, raise charge current, show success
    BRINGUP_FAILED       // hardware fault (e.g. gauge not detected)
};
extern BringupPhase gBringupPhase;

// setup(): if this board has never been provisioned + current-calibrated, hold the charge
// current at a safe 300 mA and start the bring-up. No-op when done or the bus is released.
void bringupBeginIfNeeded();
// Drives the bring-up state machine. Called from loop() until complete.
void updateBringup();
// Bench `bringupreset`: clear the NVS flag and re-run the bring-up.
void bringupRestart();
