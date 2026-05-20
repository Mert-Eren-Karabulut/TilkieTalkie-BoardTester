#include "SleepController.h"
#include "AudioController.h"
#include "FileManager.h"
#include "Buttons.h"
#include <esp_log.h>

static const char* TAG = "SLEEP";

// Magic number for RTC data validation
#define RTC_MAGIC 0xABABABAB

// Minimal RTC memory (just flag indicating we had a proper sleep)
RTC_DATA_ATTR SleepStateRTC rtcSleepState = {false, 0};

// Initialize static members
SleepController* SleepController::instance = nullptr;

// Button pins for wake-up (GPIO 12, 13, 14, 21 from Buttons.h)
const gpio_num_t SleepController::WAKEUP_BUTTON_PINS[4] = {
    GPIO_NUM_12,
    GPIO_NUM_13,
    GPIO_NUM_14,
    GPIO_NUM_21
};

const gpio_num_t SleepController::WAKEUP_POGO_PIN = GPIO_NUM_6;

SleepController::SleepController() :
    sleepScheduled(false),
    scheduledSleepTime(0),
    lastActivityTime(0),
    inactivityTimeout(30000), // Default 30 seconds inactivity
    wokenFromSleep(false),
    sleepCallback(nullptr),
    conditionsMetForSleep(false),
    conditionsMetTime(0),
    conditionsMetLogged(false),
    lastConditionsCheck(0) {
}

SleepController::~SleepController() {
}

SleepController& SleepController::getInstance() {
    if (instance == nullptr) {
        instance = new SleepController();
    }
    return *instance;
}

void SleepController::begin() {
    ESP_LOGI(TAG, "Initializing Sleep Controller...");
    
    // Enable peripheral power first (needed for SD card access)
    enablePeripheralPower();
    delay(500); // Let peripherals stabilize
    
    // Check if we woke up from deep sleep
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    
    if (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED) {
        wokenFromSleep = true;
        printWakeupReason();
        
        // Check if we had a valid sleep cycle
        if (rtcSleepState.hasValidSleep && rtcSleepState.magic == RTC_MAGIC) {
            ESP_LOGI(TAG, "Device woke from deep sleep - normal operation will resume");
        }
    }
    
    // Configure wake-up sources for next sleep
    configureWakeupSources();
    
    ESP_LOGI(TAG, "Sleep Controller initialized");
}

void SleepController::end() {
    sleepCallback = nullptr;
    sleepScheduled = false;
}

void SleepController::configureWakeupSources() {
    // Configure ext1 wake-up for multiple GPIO pins (any button press or pogo engage)
    // Use ESP_EXT1_WAKEUP_ANY_HIGH because buttons read HIGH when pressed
    uint64_t buttonMask = 0;
    
    for (int i = 0; i < 4; i++) {
        buttonMask |= (1ULL << WAKEUP_BUTTON_PINS[i]);
    }

    buttonMask |= (1ULL << WAKEUP_POGO_PIN);
    
    esp_sleep_enable_ext1_wakeup(buttonMask, ESP_EXT1_WAKEUP_ANY_HIGH);
    
    ESP_LOGI(TAG, "Configured wake-up sources: Buttons (GPIO 12, 13, 14, 21) and pogo switch (GPIO %d)", WAKEUP_POGO_PIN);
}

bool SleepController::canEnterSleep() {
    // Only perform checks every 5 seconds to reduce system overhead
    unsigned long currentTime = millis();
    static bool lastCheckResult = false;
    
    // Return cached result if we checked recently
    if (lastConditionsCheck > 0 && (currentTime - lastConditionsCheck) < CONDITIONS_CHECK_INTERVAL) {
        return lastCheckResult;
    }
    
    // Time to perform actual checks
    lastConditionsCheck = currentTime;
    
    FileManager& fileManager = FileManager::getInstance();
    AudioController& audio = AudioController::getInstance();
    
    // Check if file sync is in progress
    if (fileManager.getPendingDownloadsCount() > 0) {
        lastCheckResult = false;
        return false;
    }
    
    // Check audio state (0=STOPPED, 1=PLAYING, 2=PAUSED)
    // Only prevent sleep if audio is actively playing
    if (audio.getState() == 1) { // PLAYING
        lastCheckResult = false;
        return false;
    }
    
    // All conditions met - can enter sleep
    lastCheckResult = true;
    return true;
}

void SleepController::checkAndSleep() {
    if (!canEnterSleep()) {
        return;
    }
    
    ESP_LOGI(TAG, "Sleep conditions met, entering deep sleep...");
    enterDeepSleep();
}

void SleepController::enterDeepSleep() {
    // Call user callback if set
    if (sleepCallback) {
        ESP_LOGI(TAG, "Calling sleep callback...");
        sleepCallback();
    }
    
    // Mark that we're entering a valid sleep
    rtcSleepState.hasValidSleep = true;
    rtcSleepState.magic = RTC_MAGIC;
    
    // Disable peripheral power to save energy
    disablePeripheralPower();
    
    // Configure wake-up sources
    configureWakeupSources();
    
    ESP_LOGI(TAG, "Entering deep sleep mode...");
    ESP_LOGI(TAG, "Device will wake on any button press or pogo engage");
    
    // Small delay to let serial output finish
    delay(100);
    
    // Enter deep sleep
    esp_deep_sleep_start();
}

void SleepController::enablePeripheralPower() {
    ESP_LOGI(TAG, "Enabling peripheral power (GPIO %d)...", PERIPHERAL_POWER_PIN);
    pinMode(PERIPHERAL_POWER_PIN, OUTPUT);
    digitalWrite(PERIPHERAL_POWER_PIN, HIGH);
}

void SleepController::disablePeripheralPower() {
    ESP_LOGI(TAG, "Disabling peripheral power (GPIO %d) to save energy...", PERIPHERAL_POWER_PIN);
    digitalWrite(PERIPHERAL_POWER_PIN, LOW);
}

void SleepController::scheduleSleep(unsigned long timeoutMs) {
    sleepScheduled = true;
    scheduledSleepTime = millis() + timeoutMs;
    ESP_LOGI(TAG, "Sleep scheduled in %lu ms", timeoutMs);
}

void SleepController::cancelSleep() {
    if (sleepScheduled) {
        sleepScheduled = false;
        ESP_LOGI(TAG, "Scheduled sleep cancelled");
    }
}

void SleepController::resetActivity() {
    // Reset the conditions met state
    if (conditionsMetForSleep) {
        conditionsMetForSleep = false;
        conditionsMetLogged = false;
        lastConditionsCheck = 0; // Force recheck on next update
        ESP_LOGI(TAG, "Activity detected - sleep countdown cancelled");
    }
    
    // Also cancel any scheduled sleep
    if (sleepScheduled) {
        sleepScheduled = false;
        ESP_LOGI(TAG, "Activity detected - scheduled sleep cancelled");
    }
}

void SleepController::update() {
    unsigned long currentTime = millis();
    
    // Check scheduled sleep (explicit sleep command)
    if (sleepScheduled && currentTime >= scheduledSleepTime) {
        sleepScheduled = false;
        ESP_LOGI(TAG, "Scheduled sleep time reached");
        checkAndSleep();
        return;
    }
    
    // Check if inactivity timeout is enabled
    if (inactivityTimeout == 0) {
        return; // Inactivity sleep disabled
    }
    
    // Check if conditions are met for sleep
    bool conditionsMet = canEnterSleep();
    
    if (conditionsMet) {
        // Conditions are met for sleep
        if (!conditionsMetForSleep) {
            // Conditions just became met - start the countdown
            conditionsMetForSleep = true;
            conditionsMetTime = currentTime;
            conditionsMetLogged = false;
        }
        
        // Calculate time since conditions were met
        unsigned long timeSinceConditionsMet = currentTime - conditionsMetTime;
        
        // Log warning once
        if (!conditionsMetLogged && timeSinceConditionsMet >= 1000) {
            unsigned long secondsRemaining = (inactivityTimeout - timeSinceConditionsMet) / 1000;
            ESP_LOGI(TAG, "Sleep conditions met. Device will sleep in %lu seconds if no activity occurs.", 
                     secondsRemaining);
            conditionsMetLogged = true;
        }
        
        // Check if timeout has been reached
        if (timeSinceConditionsMet >= inactivityTimeout) {
            ESP_LOGI(TAG, "Inactivity timeout reached, entering sleep...");
            conditionsMetForSleep = false;
            conditionsMetLogged = false;
            checkAndSleep();
        }
    } else {
        // Conditions not met - reset the countdown
        if (conditionsMetForSleep) {
            conditionsMetForSleep = false;
            conditionsMetLogged = false;
        }
    }
}

void SleepController::setInactivityTimeout(unsigned long timeoutMs) {
    inactivityTimeout = timeoutMs;
    ESP_LOGI(TAG, "Inactivity timeout set to %lu ms", timeoutMs);
}

void SleepController::onSleep(SleepCallback callback) {
    sleepCallback = callback;
}

esp_sleep_wakeup_cause_t SleepController::getWakeupCause() const {
    return esp_sleep_get_wakeup_cause();
}

void SleepController::printWakeupReason() {
    esp_sleep_wakeup_cause_t wakeup_reason = getWakeupCause();
    
    ESP_LOGI(TAG, "=== Wake-up Information ===");
    
    switch(wakeup_reason) {
        case ESP_SLEEP_WAKEUP_EXT0:
            ESP_LOGI(TAG, "Wakeup caused by external signal using RTC_IO");
            break;
        case ESP_SLEEP_WAKEUP_EXT1:
            ESP_LOGI(TAG, "Wakeup caused by external signal using RTC_CNTL");
            {
                uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
                if (wakeup_pin_mask != 0) {
                    int pin = __builtin_ffsll(wakeup_pin_mask) - 1;
                    ESP_LOGI(TAG, "Wake-up button: GPIO %d", pin);

                    if (pin == WAKEUP_POGO_PIN) {
                        ESP_LOGI(TAG, "Wake-up source: figure mount / pogo switch engage");
                        break;
                    }
                    
                    // Map to button number
                    for (int i = 0; i < 4; i++) {
                        if (WAKEUP_BUTTON_PINS[i] == pin) {
                            ESP_LOGI(TAG, "Button %d was pressed", i + 1);
                            break;
                        }
                    }
                }
            }
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "Wakeup caused by timer");
            break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            ESP_LOGI(TAG, "Wakeup caused by touchpad");
            break;
        case ESP_SLEEP_WAKEUP_ULP:
            ESP_LOGI(TAG, "Wakeup caused by ULP program");
            break;
        default:
            ESP_LOGI(TAG, "Wakeup was not caused by deep sleep: %d", wakeup_reason);
            break;
    }
    
    ESP_LOGI(TAG, "===========================");
}