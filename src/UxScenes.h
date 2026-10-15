#pragma once

// What the toy shows and says, derived from system state (no state machine of its own):
// the persistent LED scene, the docked charge bar, state-driven voice prompts and the
// battery warnings. Called from loop().

// True when nothing the user is waiting for owns the LEDs (the docked charge bar may show).
bool chargeBarAllowed();
// Docked charge bar (both phases).
void updateChargingUx();
// Single owner of the persistent LED scene while the full system runs.
void deriveLedScene();
// Voice feedback that depends on state rather than on an event.
void serviceUxPrompts();
// Battery warnings on battery power: at 20 % and every 5 % below.
void updateBatteryUx();
// startFullSystem() on an unprovisioned boot: say the setup invitation now and repeat it
// (every 90 s, up to 5 times) while nobody pairs.
void startSetupWelcome();
