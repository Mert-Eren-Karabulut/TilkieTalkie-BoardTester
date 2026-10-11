#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <FastLED.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Five WS2812B LEDs on GPIO5, rendered by their own task at 50 Hz so a stalled loop task
// never freezes an animation. The loop only declares WHAT to show: a persistent scene (derived
// from system state) and short event effects that play on top of it and then disappear.
class LedController {
public:
    enum Scene : uint8_t {
        SCENE_OFF = 0,        // online and idle: dark (a bedside toy should not glow)
        SCENE_BOOT,           // soft white breathing while the system comes up
        SCENE_PROVISIONING,   // blue blink: BLE provisioning active
        SCENE_WIFI_CONNECTING,// blue breathing
        SCENE_OFFLINE,        // one dim amber LED: no network (cached stories still play)
        SCENE_LOADING,        // amber chase: figure recognised, fetching the first track
        SCENE_PLAYING,        // warm white, slow dim breathing
        SCENE_PAUSED,         // warm white steady, dim
        SCENE_DOWNLOADING,    // cyan sweep: downloading in the background while idle
        SCENE_CHARGING,       // green SOC bar (docked); param = percent, flag = current flowing
        SCENE_LOW_BATTERY,    // red slow breathing (idle and nearly empty)
        SCENE_ERROR,          // red steady
        SCENE_OTA,            // purple progress bar; param = percent
        SCENE_SLEEPING,       // fade to dark
        SCENE_MANUAL,         // direct control (bring-up / console): colour + steady or breathing
    };

    enum Effect : uint8_t {
        FX_NONE = 0,
        FX_ACK,          // one green blink: figure recognised / command accepted
        FX_NO_CONTENT,   // red double blink: nothing to play
        FX_MESSAGE,      // blue blink: chat message
        FX_VOLUME,       // white bar = volume level (param = percent), 700 ms
        FX_NEXT,         // white chase to the right
        FX_PREV,         // white chase to the left
        FX_BUTTON,       // tiny white blip
        FX_SUCCESS,      // green triple blink
        FX_FAIL,         // red x5
        FX_RESET,        // magenta x5
        FX_CUSTOM,       // legacy pulseRapid(colour, count)
    };

    LedController();
    void begin();
    void update() {}  // kept for the loop; rendering happens on the LED task

    // Scenes (idempotent) and the low-battery overlay (LED 0 breathes red on OFF/PLAYING/PAUSED)
    void setScene(Scene scene, int param = -1);
    Scene getScene() const { return scene; }
    void setLowBattery(bool low) { lowBattery = low; }
    void setProgress(int percent) { sceneParam = constrain(percent, 0, 100); }

    // Event effects: play once over the current scene
    void flash(Effect fx, int param = 0, uint32_t color = 0);

    // Legacy API (bring-up state machine, console) — maps onto the scene/effect model
    void simpleLed(uint32_t hexColor, int intensity);
    void pulseLed(uint32_t hexColor);
    void pulseRapid(uint32_t hexColor, int count);
    void turnOff();
    void setMaxBrightness(int brightness);
    int getMaxBrightness() const { return maxBrightness; }
    void enableOperatingAnimation(bool enable = true);
    void showChargeLevel(uint8_t percent, bool charging);
    void stopChargeDisplay();
    bool isChargeDisplayActive() const { return scene == SCENE_CHARGING; }

private:
    static const int LED_PIN = 5;
    static const int NUM_LEDS = 5;
    static const int LED_MAX_POWER = 255;

    CRGB leds[NUM_LEDS];
    CRGB frame[NUM_LEDS];
    int maxBrightness;

    // Declared state (written by any task under the mutex, read by the LED task)
    volatile Scene scene;
    volatile int sceneParam;
    volatile bool sceneFlag;
    volatile bool lowBattery;
    uint32_t manualColor;
    bool manualBreathing;
    int manualIntensity;
    unsigned long sceneSince;

    volatile Effect effect;
    int effectParam;
    uint32_t effectColor;
    unsigned long effectStart;
    unsigned long effectDuration;

    SemaphoreHandle_t lock;
    TaskHandle_t task;

    static void taskEntry(void* arg);
    void renderLoop();
    void renderScene(unsigned long now);
    bool renderEffect(unsigned long now);  // false when the effect is over
    void fillAll(CRGB c);
    CRGB scaled(uint32_t hex, int intensity) const;
    int pct(int percentOfMax) const { return (maxBrightness * percentOfMax) / 100; }
    static float breathe(unsigned long now, unsigned long periodMs);  // 0..1 sine
};

#endif // LED_CONTROLLER_H
