#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <FastLED.h>
#include <Arduino.h>
#include <math.h>

// Forward declaration
class ConfigManager;

class LedController {
private:
    static const int LED_PIN = 5;           // GPIO5 for WS2812B data pin (ESP32-S3)
    static const int NUM_LEDS = 5;          // Number of LEDs
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
    int maxBrightness;  // Dynamic max brightness (0-255)
    bool operatingAnimationEnabled;
    unsigned long operatingLastUpdate;
    float operatingPosition;
    int operatingDirection;
    
    // Helper functions
    CRGB hexToRgb(uint32_t hexColor);
    CRGB scaleColor(CRGB color, int intensity);
    void updatePulse();
    void updatePulseRapid();
    void updateOperatingAnimation();
    
public:
    LedController();
    void begin();
    void update();  // Call this in main loop
    
    // Main functions
    void simpleLed(uint32_t hexColor, int intensity);
    void pulseLed(uint32_t hexColor);
    void pulseRapid(uint32_t hexColor, int count);
    void turnOff();
    void setMaxBrightness(int brightness);  // Set max brightness (0-255)
    int getMaxBrightness() const { return maxBrightness; }
    void enableOperatingAnimation(bool enable = true);
};

#endif // LED_CONTROLLER_H