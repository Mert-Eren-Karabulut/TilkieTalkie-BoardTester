#ifndef SLEEPCONTROLLER_H
#define SLEEPCONTROLLER_H

#include <Arduino.h>
#include <functional>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "Policy.h"

// Minimal RTC Memory structure (just a flag to indicate we had a valid sleep)
// RTC_DATA_ATTR variables survive deep sleep
struct SleepStateRTC {
    bool hasValidSleep;              // Flag to check if we went to sleep properly
    uint32_t magic;                  // Magic number for validation (0xABABABAB)
};

class SleepController {
public:
    // Singleton pattern
    static SleepController& getInstance();

    // Initialization
    void begin();
    void end();

    // Sleep management
    void checkAndSleep();            // Check conditions and enter sleep if appropriate
    void forceSleep();               // Enter deep sleep immediately, bypassing conditions
    const char* blockedReason = nullptr;   // why canEnterSleep() said no (diagnostics)
    unsigned long lastBlockedLog = 0;
    void scheduleSleep(unsigned long timeoutMs);
    void cancelSleep();
    void resetActivity();            // Reset inactivity timer
    void update();                   // Call regularly in main loop

    // Wake-up handling
    bool wasWokenFromSleep() const { return wokenFromSleep; }
    esp_sleep_wakeup_cause_t getWakeupCause() const;
    void printWakeupReason();

    // Sleep callback (called before going to sleep)
    using SleepCallback = std::function<void()>;
    void onSleep(SleepCallback callback);

    // Configuration
    void setInactivityTimeout(unsigned long timeoutMs);
    unsigned long getInactivityTimeout() const { return inactivityTimeout; }
    
    // Peripheral power control pin
    static const int PERIPHERAL_POWER_PIN = Policy::PERIPHERAL_POWER_GPIO;

    // 4V5 rail control (GPIO4 + deep-sleep latch). Public: used by the power serial
    // commands and by the gauge-based sleep-current measurement (`sleeptest`).
    void disablePeripheralPower();
    void enablePeripheralPower();

    // PN532 software power-down + NFC I2C lines parked high and latched through deep sleep.
    // Used by the sleep callback (main) and the bench `sleeptest`.
    static void powerDownNfcForSleep();

private:
    // Singleton instance
    static SleepController* instance;

    // Private constructor to prevent instantiation
    SleepController();
    ~SleepController();

    // Sleep state
    bool sleepScheduled;
    unsigned long scheduledSleepAt;     // millis() when scheduleSleep() ran
    unsigned long scheduledSleepDelay;  // wrap-safe: elapsed since scheduledSleepAt
    unsigned long inactivityTimeout;
    bool wokenFromSleep;
    SleepCallback sleepCallback;
    
    // Sleep condition tracking
    bool conditionsMetForSleep;
    unsigned long conditionsMetTime;
    bool conditionsMetLogged;
    unsigned long lastConditionsCheck;
    static const unsigned long CONDITIONS_CHECK_INTERVAL = 5000; // Check every 5 seconds

    // Button pins for wake-up (from Buttons.h)
    static const gpio_num_t WAKEUP_BUTTON_PINS[4];
    static const gpio_num_t WAKEUP_POGO_PIN;
    
    // Helper methods
    void configureWakeupSources();
    bool canEnterSleep();
    void enterDeepSleep();
};

#endif // SLEEPCONTROLLER_H