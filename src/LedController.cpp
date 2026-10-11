#include "LedController.h"
#include "Diag.h"
#include <esp_timer.h>
#include "ConfigManager.h"

namespace {
const uint32_t COLOR_WARM = 0xFFB46E;
const uint32_t COLOR_AMBER = 0xFF8C00;
const uint32_t COLOR_GREEN = 0x00FF40;
const uint32_t COLOR_RED = 0xFF0000;
const uint32_t COLOR_BLUE = 0x2060FF;
const uint32_t COLOR_CYAN = 0x00C8FF;
const uint32_t COLOR_PURPLE = 0xA020F0;
const uint32_t COLOR_WHITE = 0xFFFFFF;
const uint32_t COLOR_MAGENTA = 0xFF00FF;
}

LedController::LedController()
    : maxBrightness(LED_MAX_POWER), scene(SCENE_OFF), sceneParam(0), sceneFlag(false), lowBattery(false),
      manualColor(0), manualBreathing(false), manualIntensity(0), sceneSince(0),
      effect(FX_NONE), effectParam(0), effectColor(0), effectStart(0), effectDuration(0),
      lock(nullptr), task(nullptr) {
    ConfigManager& config = ConfigManager::getInstance();
    maxBrightness = config.getInt("max_brightness", LED_MAX_POWER);
    maxBrightness = constrain(maxBrightness, 0, LED_MAX_POWER);
}

void LedController::begin() {
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(255);
    FastLED.clear();
    FastLED.show();
    if (!lock) {
        lock = xSemaphoreCreateMutex();
    }
    sceneSince = millis();
    if (!task) {
        xTaskCreatePinnedToCore(taskEntry, "led", 3072, this, 3, &task, 1);
    }
}

void LedController::taskEntry(void* arg) {
    static_cast<LedController*>(arg)->renderLoop();
}

void LedController::renderLoop() {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        unsigned long now = millis();
        bool drawn = false;
        if (xSemaphoreTake(lock, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (effect != FX_NONE) {
                drawn = renderEffect(now);
                if (!drawn) {
                    effect = FX_NONE;
                }
            }
            if (!drawn) {
                renderScene(now);
            }
            xSemaphoreGive(lock);
            memcpy(leds, frame, sizeof(leds));
            FastLED.show();
        }
        {
            static int64_t lastFrameUs = 0;
            const int64_t t = esp_timer_get_time();
            if (lastFrameUs && t - lastFrameUs > g_ledFrameMaxGapUs) g_ledFrameMaxGapUs = t - lastFrameUs;
            lastFrameUs = t;
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20)); // 50 Hz
    }
}

// ------------------------------------------------------------------ declarations

void LedController::setScene(Scene s, int param) {
    if (!lock) { scene = s; if (param >= 0) sceneParam = param; return; }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (scene != s) {
        scene = s;
        sceneSince = millis();
    }
    if (param >= 0) {
        sceneParam = param;
    }
    xSemaphoreGive(lock);
}

void LedController::flash(Effect fx, int param, uint32_t color) {
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    effect = fx;
    effectParam = param;
    effectColor = color;
    effectStart = millis();
    switch (fx) {
        case FX_ACK:        effectDuration = 180; break;
        case FX_NO_CONTENT: effectDuration = 700; break;
        case FX_MESSAGE:    effectDuration = 400; break;
        case FX_VOLUME:     effectDuration = 700; break;
        case FX_NEXT:
        case FX_PREV:       effectDuration = 350; break;
        case FX_BUTTON:     effectDuration = 90; break;
        case FX_SUCCESS:    effectDuration = 900; break;
        case FX_FAIL:       effectDuration = 1500; break;
        case FX_RESET:      effectDuration = 1500; break;
        case FX_CUSTOM:     effectDuration = (unsigned long)max(1, param) * 300UL; break;
        default:            effectDuration = 0; break;
    }
    xSemaphoreGive(lock);
}

void LedController::simpleLed(uint32_t hexColor, int intensity) {
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    manualColor = hexColor;
    manualIntensity = constrain(intensity, 0, maxBrightness);
    manualBreathing = false;
    scene = SCENE_MANUAL;
    sceneSince = millis();
    if (lock) xSemaphoreGive(lock);
}

void LedController::pulseLed(uint32_t hexColor) {
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    manualColor = hexColor;
    manualIntensity = maxBrightness;
    manualBreathing = true;
    scene = SCENE_MANUAL;
    sceneSince = millis();
    if (lock) xSemaphoreGive(lock);
}

void LedController::pulseRapid(uint32_t hexColor, int count) {
    flash(FX_CUSTOM, count, hexColor);
}

void LedController::turnOff() {
    setScene(SCENE_OFF);
}

void LedController::setMaxBrightness(int brightness) {
    maxBrightness = constrain(brightness, 0, LED_MAX_POWER);
    ConfigManager::getInstance().storeInt("max_brightness", maxBrightness);
}

void LedController::enableOperatingAnimation(bool enable) {
    if (enable && scene == SCENE_MANUAL) {
        setScene(SCENE_OFF); // the old idle sweep is gone: online idle is dark
    }
}

void LedController::showChargeLevel(uint8_t percent, bool charging) {
    if (lock) xSemaphoreTake(lock, portMAX_DELAY);
    if (scene != SCENE_CHARGING) {
        sceneSince = millis();
    }
    scene = SCENE_CHARGING;
    sceneParam = percent > 100 ? 100 : percent;
    sceneFlag = charging;
    if (lock) xSemaphoreGive(lock);
}

void LedController::stopChargeDisplay() {
    if (scene == SCENE_CHARGING) {
        setScene(SCENE_OFF);
    }
}

// ------------------------------------------------------------------ rendering

float LedController::breathe(unsigned long now, unsigned long periodMs) {
    float phase = (float)(now % periodMs) / (float)periodMs * TWO_PI;
    return (sinf(phase - HALF_PI) + 1.0f) * 0.5f;
}

CRGB LedController::scaled(uint32_t hex, int intensity) const {
    intensity = constrain(intensity, 0, 255);
    CRGB c((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
    return CRGB((uint8_t)((c.r * intensity) / 255), (uint8_t)((c.g * intensity) / 255), (uint8_t)((c.b * intensity) / 255));
}

void LedController::fillAll(CRGB c) {
    for (int i = 0; i < NUM_LEDS; ++i) frame[i] = c;
}

void LedController::renderScene(unsigned long now) {
    fillAll(CRGB::Black);
    const unsigned long age = now - sceneSince;

    switch (scene) {
        case SCENE_OFF:
            break;

        case SCENE_BOOT:
            fillAll(scaled(COLOR_WHITE, (int)(pct(25) * breathe(now, 2400))));
            break;

        case SCENE_PROVISIONING:
            if ((now / 400) % 2 == 0) fillAll(scaled(COLOR_BLUE, pct(50)));
            break;

        case SCENE_WIFI_CONNECTING:
            fillAll(scaled(COLOR_BLUE, (int)(pct(40) * breathe(now, 1600))));
            break;

        case SCENE_OFFLINE:
            frame[0] = scaled(COLOR_AMBER, pct(12));
            break;

        case SCENE_LOADING: {
            // one bright amber dot chasing left to right with a short tail
            float pos = fmodf((float)age / 110.0f, (float)NUM_LEDS);
            for (int i = 0; i < NUM_LEDS; ++i) {
                float d = pos - i;
                if (d < 0) d += NUM_LEDS;
                float level = d < 1.0f ? 1.0f : d < 2.0f ? 0.35f : d < 3.0f ? 0.1f : 0.0f;
                frame[i] = scaled(COLOR_AMBER, (int)(pct(60) * level));
            }
            break;
        }

        case SCENE_PLAYING: {
            int level = pct(4) + (int)((pct(14) - pct(4)) * breathe(now, 4000));
            fillAll(scaled(COLOR_WARM, level));
            break;
        }

        case SCENE_PAUSED:
            fillAll(scaled(COLOR_WARM, pct(6)));
            break;

        case SCENE_DOWNLOADING: {
            // a soft cyan sweep back and forth
            float t = (float)(age % 1800) / 1800.0f;
            float pos = (t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f) * (NUM_LEDS - 1);
            for (int i = 0; i < NUM_LEDS; ++i) {
                float falloff = 1.0f - fabsf(i - pos) / 1.4f;
                if (falloff > 0) frame[i] = scaled(COLOR_CYAN, (int)(pct(30) * falloff));
            }
            break;
        }

        case SCENE_CHARGING: {
            // one LED per 20 % SOC, the remaining segments breathe while current flows
            int filled = (sceneParam * NUM_LEDS) / 100;
            int solid = pct(30);
            int breath = sceneFlag ? (int)(solid * breathe(now, 3000)) : 0;
            for (int i = 0; i < NUM_LEDS; ++i) {
                frame[i] = i < filled ? scaled(COLOR_GREEN, solid) : scaled(COLOR_GREEN, breath);
            }
            break;
        }

        case SCENE_LOW_BATTERY:
            fillAll(scaled(COLOR_RED, (int)(pct(35) * breathe(now, 2000))));
            break;

        case SCENE_ERROR:
            fillAll(scaled(COLOR_RED, pct(50)));
            break;

        case SCENE_OTA: {
            // purple bar filling with progress; the segment being filled breathes
            int level = pct(50);
            int filledTenths = (sceneParam * NUM_LEDS * 10) / 100;
            for (int i = 0; i < NUM_LEDS; ++i) {
                int ledTenths = filledTenths - i * 10;
                if (ledTenths >= 10) frame[i] = scaled(COLOR_PURPLE, level);
                else if (ledTenths > 0) frame[i] = scaled(COLOR_PURPLE, (int)(level * (0.3f + 0.7f * breathe(now, 1000))));
                else frame[i] = 0; // not reached yet: dark (left stale, these flashed)
            }
            break;
        }

        case SCENE_SLEEPING: {
            int level = age < 800 ? (int)(pct(30) * (1.0f - (float)age / 800.0f)) : 0;
            fillAll(scaled(COLOR_WARM, level));
            break;
        }

        case SCENE_MANUAL:
            fillAll(scaled(manualColor, manualBreathing ? (int)(manualIntensity * breathe(now, 2000)) : manualIntensity));
            break;
    }

    // Low battery overlay: LED 0 breathes red on the quiet scenes so a parent notices.
    if (lowBattery && (scene == SCENE_OFF || scene == SCENE_PLAYING || scene == SCENE_PAUSED)) {
        frame[0] = scaled(COLOR_RED, (int)(pct(30) * breathe(now, 2000)));
    }
}

bool LedController::renderEffect(unsigned long now) {
    unsigned long t = now - effectStart;
    if (t >= effectDuration) return false;
    fillAll(CRGB::Black);
    const int hi = pct(80);

    switch (effect) {
        case FX_ACK:
            fillAll(scaled(COLOR_GREEN, hi));
            break;
        case FX_NO_CONTENT:
            if ((t / 175) % 2 == 0) fillAll(scaled(COLOR_RED, hi));
            break;
        case FX_MESSAGE:
            if (t < 200) fillAll(scaled(COLOR_BLUE, hi));
            break;
        case FX_VOLUME: {
            int lit = (effectParam * NUM_LEDS + 99) / 100; // 1-5 LEDs
            for (int i = 0; i < NUM_LEDS && i < lit; ++i) frame[i] = scaled(COLOR_WHITE, pct(50));
            if (effectParam == 0) frame[0] = scaled(COLOR_RED, pct(40));
            break;
        }
        case FX_NEXT:
        case FX_PREV: {
            int step = (int)(t / (effectDuration / NUM_LEDS));
            if (step >= NUM_LEDS) step = NUM_LEDS - 1;
            int idx = effect == FX_NEXT ? step : NUM_LEDS - 1 - step;
            frame[idx] = scaled(COLOR_WHITE, pct(60));
            break;
        }
        case FX_BUTTON:
            fillAll(scaled(COLOR_WHITE, pct(20)));
            break;
        case FX_SUCCESS:
            if ((t / 150) % 2 == 0) fillAll(scaled(COLOR_GREEN, hi));
            break;
        case FX_FAIL:
            if ((t / 150) % 2 == 0) fillAll(scaled(COLOR_RED, hi));
            break;
        case FX_RESET:
            if ((t / 150) % 2 == 0) fillAll(scaled(COLOR_MAGENTA, hi));
            break;
        case FX_CUSTOM:
            if ((t % 300) < 200) fillAll(scaled(effectColor, maxBrightness));
            break;
        default:
            return false;
    }
    return true;
}
