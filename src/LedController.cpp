#include "LedController.h"

LedController::LedController() {
    pulseActive = false;
    pulseRapidActive = false;
    lastUpdate = 0;
    pulseRapidStartTime = 0;
    pulseRapidCount = 0;
    pulseRapidCurrentCount = 0;
    pulseColor = 0;
    pulseRapidColor = 0;
    pulseDirection = 1;
    currentBrightness = 0;
}

void LedController::begin() {
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(255);
    FastLED.clear();
    FastLED.show();
}

void LedController::update() {
    if (pulseActive) {
        updatePulse();
    }
    if (pulseRapidActive) {
        updatePulseRapid();
    }
}

void LedController::simpleLed(uint32_t hexColor, int intensity) {
    // Stop any active effects
    pulseActive = false;
    pulseRapidActive = false;
    
    // Clamp intensity to LED_MAX_POWER
    intensity = min(intensity, LED_MAX_POWER);
    intensity = max(intensity, 0);
    
    CRGB color = hexToRgb(hexColor);
    leds[0] = scaleColor(color, intensity);
    
    FastLED.show();
}

void LedController::pulseLed(uint32_t hexColor) {
    // Stop pulse rapid if active
    pulseRapidActive = false;
    
    // Start pulse effect
    pulseActive = true;
    pulseColor = hexColor;
    pulseDirection = 1;
    currentBrightness = 0;
    lastUpdate = millis();
}

void LedController::pulseRapid(uint32_t hexColor, int count) {
    // Stop pulse if active
    pulseActive = false;
    
    // Start pulse rapid effect
    pulseRapidActive = true;
    pulseRapidColor = hexColor;
    pulseRapidCount = count;
    pulseRapidCurrentCount = 0;
    pulseRapidStartTime = millis();
    
    // Turn on LED immediately
    CRGB color = hexToRgb(hexColor);
    leds[0] = scaleColor(color, LED_MAX_POWER);
    FastLED.show();
}

void LedController::turnOff() {
    pulseActive = false;
    pulseRapidActive = false;
    
    FastLED.clear();
    FastLED.show();
}

CRGB LedController::hexToRgb(uint32_t hexColor) {
    uint8_t r = (hexColor >> 16) & 0xFF;
    uint8_t g = (hexColor >> 8) & 0xFF;
    uint8_t b = hexColor & 0xFF;
    return CRGB(r, g, b);
}

CRGB LedController::scaleColor(CRGB color, int intensity) {
    float scale = (float)intensity / 255.0f;
    return CRGB(
        (uint8_t)(color.r * scale),
        (uint8_t)(color.g * scale),
        (uint8_t)(color.b * scale)
    );
}

void LedController::updatePulse() {
    unsigned long currentTime = millis();
    
    // Update every 20ms for smooth animation
    if (currentTime - lastUpdate >= 20) {
        lastUpdate = currentTime;
        
        // Adjust brightness
        currentBrightness += pulseDirection * 5;
        
        // Check bounds and reverse direction
        if (currentBrightness >= LED_MAX_POWER) {
            currentBrightness = LED_MAX_POWER;
            pulseDirection = -1;
        } else if (currentBrightness <= 0) {
            currentBrightness = 0;
            pulseDirection = 1;
        }
        
        // Update LED
        CRGB color = hexToRgb(pulseColor);
        leds[0] = scaleColor(color, currentBrightness);
        FastLED.show();
    }
}

void LedController::updatePulseRapid() {
    unsigned long currentTime = millis();
    unsigned long elapsed = currentTime - pulseRapidStartTime;
    
    // 200ms on, 100ms off pattern
    unsigned long cycleTime = 300; // 200ms + 100ms
    unsigned long timeInCycle = elapsed % cycleTime;
    
    // Check if we've completed the required number of cycles
    if (elapsed >= (unsigned long)pulseRapidCount * cycleTime) {
        pulseRapidActive = false;
        FastLED.clear();
        FastLED.show();
        return;
    }
    
    // Control LED based on position in cycle
    if (timeInCycle < 200) {
        // LED on for first 200ms
        CRGB color = hexToRgb(pulseRapidColor);
        leds[0] = scaleColor(color, LED_MAX_POWER);
    } else {
        // LED off for remaining 100ms
        leds[0] = CRGB::Black;
    }
    
    FastLED.show();
}