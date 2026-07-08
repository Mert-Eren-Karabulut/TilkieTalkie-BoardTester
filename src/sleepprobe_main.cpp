// Bare-minimum deep-sleep floor probe (built ONLY by env:sleepprobe).
//
// Purpose: measure the board's lowest achievable sleep current with NOTHING
// initialized — no I2C, no radios, no drivers, no holds except the 4V5 enable.
// Interpreting the cell current in this state:
//   - still tens of mA  -> the drain is board-level hardware (thermal-hunt it),
//   - µA..few mA        -> the main app's sleep path leaves something on (bisect app).
#ifdef SLEEP_PROBE_BUILD

#include <Arduino.h>
#include <driver/gpio.h>

void setup()
{
    Serial.begin(115200);
    delay(3000); // window to catch the banner / reflash before it sleeps
    Serial.println("SLEEP PROBE: GPIO4 held low, deep sleep in 2s. Wake = BTN1 (GPIO12).");
    delay(2000);

    // Keep the 4V5 enable (and the diode-OR'd SD/NFC/AUDIO enables) low, latched
    // through deep sleep via the RTC pad hold — same mechanism as the main app.
    pinMode(4, OUTPUT);
    digitalWrite(4, LOW);
    gpio_hold_en(GPIO_NUM_4);

    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_12, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_deep_sleep_start();
}

void loop() {}

#endif // SLEEP_PROBE_BUILD
