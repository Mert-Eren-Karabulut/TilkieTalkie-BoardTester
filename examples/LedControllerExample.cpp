/**
 * LedController Example
 * 
 * This example demonstrates how to use the LedController class to control
 * a WS2812B addressable LED connected to GPIO16 on ESP32.
 * 
 * Hardware Requirements:
 * - ESP32 development board
 * - WS2812B addressable LED (or LED strip with 1 LED)
 * - Data pin connected to GPIO16
 * - Power supply (5V for WS2812B, but can work with 3.3V)
 * - Optional: 470 ohm resistor in series with data line
 * 
 * Features demonstrated:
 * - Simple LED control with color and intensity
 * - Continuous pulsing effect
 * - Rapid pulse for alerts
 */

#include <Arduino.h>
#include "LedController.h"

LedController ledController;

void setup() {
    Serial.begin(115200);
    Serial.println("WS2812B LED Controller Example");
    
    // Initialize LED controller
    ledController.begin();
    Serial.println("LED Controller initialized");
    
    // Test sequence
    delay(1000);
    
    // Test 1: Simple LED control
    Serial.println("Test 1: Red LED at 50% intensity");
    ledController.simpleLed(0xFF0000, 128);  // Red at 50% intensity (128/255)
    delay(2000);
    
    Serial.println("Test 2: Green LED at full intensity");
    ledController.simpleLed(0x00FF00, 255);  // Green at full intensity
    delay(2000);
    
    Serial.println("Test 3: Blue LED at 25% intensity");
    ledController.simpleLed(0x0000FF, 64);   // Blue at 25% intensity
    delay(2000);
    
    // Test 2: Pulsing effect
    Serial.println("Test 4: Purple pulsing effect");
    ledController.pulseLed(0xFF00FF);        // Purple pulsing
    delay(5000);
    
    // Test 3: Rapid pulse for alerts
    Serial.println("Test 5: Orange rapid pulse (3 times)");
    ledController.pulseRapid(0xFF8000, 3);  // Orange rapid pulse 3 times
    delay(2000);
    
    // Turn off
    Serial.println("Test 6: LED off");
    ledController.turnOff();
    delay(1000);
    
    Serial.println("Example completed. Starting interactive mode...");
    Serial.println("Commands:");
    Serial.println("  r - Red LED");
    Serial.println("  g - Green LED");
    Serial.println("  b - Blue LED");
    Serial.println("  p - Purple pulse");
    Serial.println("  a - Alert (rapid pulse)");
    Serial.println("  o - Turn off");
}

void loop() {
    // Update LED controller for animations
    ledController.update();
    
    // Handle serial commands
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        command.toLowerCase();
        
        if (command == "r") {
            Serial.println("Red LED");
            ledController.simpleLed(0xFF0000, 255);
        }
        else if (command == "g") {
            Serial.println("Green LED");
            ledController.simpleLed(0x00FF00, 255);
        }
        else if (command == "b") {
            Serial.println("Blue LED");
            ledController.simpleLed(0x0000FF, 255);
        }
        else if (command == "p") {
            Serial.println("Purple pulse");
            ledController.pulseLed(0xFF00FF);
        }
        else if (command == "a") {
            Serial.println("Alert (rapid pulse)");
            ledController.pulseRapid(0xFF0000, 5);
        }
        else if (command == "o") {
            Serial.println("LED off");
            ledController.turnOff();
        }
        else {
            Serial.println("Unknown command. Available: r, g, b, p, a, o");
        }
    }
    
    // Small delay to prevent excessive CPU usage
    delay(10);
}
