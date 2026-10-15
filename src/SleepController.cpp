#include "SleepController.h"
#include "VoicePrompts.h"
#include "OtaManager.h"
#include "AudioController.h"
#include "BatteryManagement.h"
#include "FileManager.h"
#include "Buttons.h"
#include "NfcController.h"
#include <esp_log.h>
#include <driver/gpio.h>

static const char* TAG = "sleep";

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
    scheduledSleepAt(0),
    scheduledSleepDelay(0),
    inactivityTimeout(Policy::INACTIVITY_TIMEOUT_SHORT_MS), // setup() sets the real value
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
    // The peripheral rail is powered by setup() (enablePeripheralPower) before any peripheral
    // init; it used to be switched on (and waited for) a second time here.

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

    // Deliberately NO timer wake: a periodic wake to "check the battery" is a brownout
    // trap on a nearly-dead cell (booting at ~2.8V can fail mid-wake), and cell
    // protection while asleep belongs in silicon — the gauge's CUV (2.5V trip,
    // latch-until-charge) covers that with the ESP fully dead.
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

    // Plugged in = awake. A docked toy shows its charge bar and stays reachable even when the
    // cell is full; sleeping on USB power also left the gauge without rest periods it needs.
    // Presence is debounced: the status bit blinks off while the battery task re-scans the bus
    // every 5 s, so VBUS has to be gone for 30 s (status bit AND the VBUS ADC) before sleep.
    {
        BatteryManager& battery = BatteryManager::getInstance();
        static unsigned long lastVbusSeenAt = 0;
        if (battery.isVbusPresent() || battery.getVbusVoltage() > 4.0f) {
            lastVbusSeenAt = currentTime == 0 ? 1 : currentTime;
        }
        if (lastVbusSeenAt != 0 && currentTime - lastVbusSeenAt < 30000UL) {
            blockedReason = "USB power seen within the last 30 s";
            lastCheckResult = false;
            return false;
        }
    }
    
    // A firmware update in flight must finish: a toy on battery used to deep-sleep in the middle
    // of the download (the OTA task is not a FileManager download) and start over on the next wake.
    if (OtaManager::getInstance().inProgress()) {
        blockedReason = "firmware update in progress";
        lastCheckResult = false;
        return false;
    }

    // Only an in-flight transfer holds off sleep. A non-empty queue must not: with WiFi down
    // or the server unreachable it would keep a battery-powered toy awake until the cell
    // is empty. The queue is persisted and resumes on the next wake with connectivity.
    if (fileManager.hasActiveDownload() || fileManager.hasPendingMaintenance()) {
        blockedReason = fileManager.hasActiveDownload() ? "download in progress" : "SD writes pending";
        lastCheckResult = false;
        return false;
    }
    
    // Check audio state (0=STOPPED, 1=PLAYING, 2=PAUSED)
    // Only prevent sleep if audio is actively playing
    if (audio.getState() == 1) { // PLAYING
        blockedReason = "audio playing";
        lastCheckResult = false;
        return false;
    }
    
    // All conditions met - can enter sleep
    blockedReason = nullptr;
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

void SleepController::forceSleep() {
    // Unconditional deep sleep (e.g. low-battery cutoff). Skips canEnterSleep() so it
    // sleeps even while audio is playing — protecting the cell takes priority.
    ESP_LOGW(TAG, "Forcing deep sleep (bypassing sleep conditions)...");
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
    
    // Codec muted and powered down, amp off, I2S pins released before the rail goes.
    VoicePrompts::getInstance().sayBlocking(Prompt::TONE_SLEEP, 1500);
    AudioController::getInstance().end();
    // Flush the download queue / required-files state and unmount the card before its rail
    // is cut (an unmounted FAT with a pending write is how cards get corrupted).
    FileManager::getInstance().end();

    // Disable peripheral power to save energy (latches GPIO4 low via gpio_hold_en).
    disablePeripheralPower();

    // Deliberately NO gpio_deep_sleep_hold_en() here: that global hold freezes ALL
    // digital pads at their functional state through deep sleep — including the
    // flash/octal-PSRAM SPI pads — which defeats the ESP_SLEEP_PSRAM/FLASH_LEAKAGE
    // workarounds and leaves the memories half-selected at ~40mA all night (the
    // 5-day battery-depletion culprit). GPIO4 (and the NFC bus pins) are RTC-capable
    // pads whose gpio_hold_en persists through deep sleep natively, so the 4.5V rail
    // still stays off without it. Exception: on USB power the sleep callback (main.cpp) arms
    // the global hold so GPIO44/BATT_CE (a digital pad) stays low and charging continues;
    // VBUS pays the standby cost there. On battery CE is irrelevant and is left unheld.

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
    // Release any deep-sleep latch from a previous sleep before driving the pin.
    gpio_hold_dis((gpio_num_t)PERIPHERAL_POWER_PIN);
    pinMode(PERIPHERAL_POWER_PIN, OUTPUT);
    digitalWrite(PERIPHERAL_POWER_PIN, HIGH);
}

void SleepController::disablePeripheralPower() {
    ESP_LOGI(TAG, "Disabling peripheral power (GPIO %d) to save energy...", PERIPHERAL_POWER_PIN);
    digitalWrite(PERIPHERAL_POWER_PIN, LOW);
    // CRITICAL: latch GPIO4 low so it stays low through deep sleep. Without this the pin
    // floats once the chip sleeps, the enable nets re-energize, and SD/NFC/audio drain
    // the cell (caused an overnight over-discharge). GPIO4 is an RTC-capable pad, so
    // this hold persists through deep sleep on its own — do NOT add the global
    // gpio_deep_sleep_hold_en() (it freezes the flash/PSRAM pads too; see enterDeepSleep).
    gpio_hold_en((gpio_num_t)PERIPHERAL_POWER_PIN);
}

void SleepController::powerDownNfcForSleep() {
    // Whatever the rail does (the 2026-10-11 audit reads RSTPD_N as following GPIO4 through
    // R77; the PN532 supply itself is on the always-on 3.3 V rail), the measured fix for the
    // ~40 mA sleep drain (5-day depletion incident) is the PN532 software power-down.
    NfcController::getInstance().powerDown();
    // Park the NFC I2C lines high and latch them through deep sleep. The pads get isolated in
    // sleep and the pullups are internal-only, so the bus would float, which the powered PN532
    // reads as endless phantom traffic (its wake source is I2C activity), defeating the
    // power-down. Idle-high bus keeps it asleep. Released in NfcController::begin()/powerDown().
    pinMode(NFC_SDA_PIN, OUTPUT);
    digitalWrite(NFC_SDA_PIN, HIGH);
    gpio_hold_en((gpio_num_t)NFC_SDA_PIN);
    pinMode(NFC_SCL_PIN, OUTPUT);
    digitalWrite(NFC_SCL_PIN, HIGH);
    gpio_hold_en((gpio_num_t)NFC_SCL_PIN);
}

void SleepController::scheduleSleep(unsigned long timeoutMs) {
    sleepScheduled = true;
    scheduledSleepAt = millis();
    scheduledSleepDelay = timeoutMs;
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
    if (sleepScheduled && currentTime - scheduledSleepAt >= scheduledSleepDelay) { // wrap-safe
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
        if (currentTime - lastBlockedLog > 60000UL) {
            lastBlockedLog = currentTime;
            ESP_LOGI(TAG, "Sleep blocked: %s", blockedReason ? blockedReason : "unknown");
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