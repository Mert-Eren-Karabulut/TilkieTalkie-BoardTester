#pragma once

#include <stdint.h>

// Product policy constants shared by main.cpp and the modules. One definition each: change a
// number here, not at its call sites.
namespace Policy
{
    // --- Backend (production; a bench build can override it in NVS, `endpoint set`) ---
    constexpr const char *API_BASE_URL = "https://portal.tilkietalkie.com/api";
    constexpr const char *REVERB_HOST = "portal.tilkietalkie.com";
    constexpr uint16_t REVERB_PORT = 443;
    constexpr const char *REVERB_APP_KEY = "erko2001";

    // --- Hardware ---
    // Shared 4V5 peripheral rail enable (SD, codec/amp, LEDs). Latched low through deep sleep.
    constexpr int PERIPHERAL_POWER_GPIO = 4;

    // --- Charging ---
    // Safe current: an uncalibrated gauge over-reads ~4.5x (OCC trips), and a weak / high-ESR
    // cell below 3.05 V must not overshoot VBAT OVP. Also the dead-cell recovery current.
    constexpr uint16_t CHARGE_CURRENT_SAFE_MA = 300;
    // Normal current (~C/4 of the 4000 mAh LFP cell). The cell allows 0.5C but the cell
    // connector (CN33) and the cell-to-PACK path do not. Needs a current-calibrated gauge.
    constexpr uint16_t CHARGE_CURRENT_FAST_MA = 1000;

    // --- Battery thresholds (1S LiFePO4) ---
    // Below this SOC on battery the LEDs show the low-battery overlay / scene.
    constexpr float LOW_BATTERY_PERCENT = 15.0f;
    // Firmware cutoff on battery: well above the gauge CUV (~2.5 V) so the firmware, not the
    // protection IC, stops the drain. Debounced to ignore momentary load sag.
    constexpr float LOW_BATTERY_CUTOFF_V = 3.00f;
    constexpr unsigned long LOW_BATTERY_CUTOFF_DEBOUNCE_MS = 5000;
    // On USB below this the boot charges with the ESP parked in deep sleep (DeadCellRecovery).
    constexpr float DEAD_CELL_RECOVERY_ENTER_V = 2.85f;
    // Full-system start gate: a rested near-empty cell can read above SYSTEM_START_V for a
    // single sample and then collapse under WiFi load, so the full system starts only on a
    // clearly-healthy voltage immediately, or on SYSTEM_START_V sustained for SYSTEM_START_HOLD_MS.
    constexpr float SYSTEM_START_V = 3.10f;
    constexpr float SYSTEM_START_IMMEDIATE_V = 3.20f;
    constexpr unsigned long SYSTEM_START_HOLD_MS = 10000;

    // --- UX windows ---
    // Tap of a figure until something plays or fails: the amber loading chase.
    constexpr unsigned long FIGURE_LOADING_WINDOW_MS = 30000;
    // After the full system starts without WiFi: "connecting" before "offline".
    constexpr unsigned long WIFI_CONNECTING_WINDOW_MS = 60000;

    // --- Sleep ---
    // Inactivity before deep sleep on battery (set at boot).
    constexpr unsigned long INACTIVITY_TIMEOUT_MS = 5UL * 60UL * 1000UL;
    // Restored when the bench learning cycle ends (Console.cpp). Bring-up completion restores
    // INACTIVITY_TIMEOUT_MS since 1.2.20 (it used to leave a new toy at 30 s until a reboot).
    constexpr unsigned long INACTIVITY_TIMEOUT_SHORT_MS = 30000;
    // Effectively never (bring-up, learning discharge).
    constexpr unsigned long INACTIVITY_TIMEOUT_NEVER = 0xFFFFFFFF;
}
