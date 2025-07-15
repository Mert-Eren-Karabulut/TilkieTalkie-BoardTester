#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <FastLED.h>
#include <Arduino.h>

class LedController {
private:
    static const int LED_PIN = 16;          // GPIO16 for WS2812B data pin
    static const int NUM_LEDS = 1;          // Single LED
    static const int LED_MAX_POWER = 255;   // Max brightness (0-255)
    
    CRGB leds[NUM_LEDS];
    bool pulseActive;
    bool pulseRapidActive;
    unsigned long lastUpdate;
    unsigned long pulseRapidStartTime;
    int pulseRapidCount;
    int pulseRapidCurrentCount;
    uint32_t pulseColor;
    uint32_t pulseRapidColor;
    int pulseDirection;
    int currentBrightness;
    
    // Helper functions
    CRGB hexToRgb(uint32_t hexColor);
    CRGB scaleColor(CRGB color, int intensity);
    void updatePulse();
    void updatePulseRapid();
    
public:
    LedController();
    void begin();
    void update();  // Call this in main loop
    
    // Main functions
    void simpleLed(uint32_t hexColor, int intensity);
    void pulseLed(uint32_t hexColor);
    void pulseRapid(uint32_t hexColor, int count);
    void turnOff();
};

#endif // LED_CONTROLLER_H