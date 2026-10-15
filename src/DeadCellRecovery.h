#pragma once

// Cell protection outside the charger supervisor:
// - the dead-cell recovery hold at boot (a sub-CUV cell on USB charges with the ESP parked
//   in deep sleep instead of boot-looping);
// - the low-battery cutoff on battery power (ship mode at Policy::LOW_BATTERY_CUTOFF_V).

// setup(), right after battery.begin(): on USB below Policy::DEAD_CELL_RECOVERY_ENTER_V this
// deep-sleeps (charging) and re-checks on the next boot a minute later; otherwise returns.
void runDeadCellRecoveryHold();
// loop(), every pass in every phase.
void serviceLowBatteryCutoff();
