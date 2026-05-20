#ifndef SLEEPCONTROLLER_H
#define SLEEPCONTROLLER_H

#include <Arduino.h>
#include <functional>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

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
    static const int PERIPHERAL_POWER_PIN = 4;

private:
    // Singleton instance
    static SleepController* instance;

    // Private constructor to prevent instantiation
    SleepController();
    ~SleepController();

    // Sleep state
    bool sleepScheduled;
    unsigned long scheduledSleepTime;
    unsigned long lastActivityTime;
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
    void disablePeripheralPower();
    void enablePeripheralPower();
};

#endif // SLEEPCONTROLLER_H