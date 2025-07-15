# WS2812B LED Controller

A comprehensive C++ library for controlling WS2812B addressable LEDs on ESP32 using the FastLED library.

## Features

- **Simple LED Control**: Set LED to any color with adjustable intensity
- **Pulse Animation**: Smooth pulsing effect with customizable color
- **Rapid Pulse**: Quick on/off pattern for alerts and notifications
- **Non-blocking**: All animations run asynchronously in background
- **Intensity Limiting**: Built-in maximum brightness control for power management

## Hardware Requirements

- ESP32 development board
- WS2812B addressable LED (or LED strip)
- Data pin connected to GPIO16
- 5V power supply (3.3V may work with reduced brightness)
- Optional: 470Ω resistor in series with data line for signal protection

## Wiring

```
ESP32 GPIO16 ----[470Ω]---- WS2812B Data In
ESP32 GND      ------------- WS2812B GND
5V Supply      ------------- WS2812B VCC
```

## Installation

1. Add FastLED library to your `platformio.ini`:
```ini
lib_deps = 
    fastled/FastLED@^3.6.0
```

2. Include the header files in your project:
```cpp
#include "LedController.h"
```

## Usage

### Basic Setup

```cpp
#include "LedController.h"

LedController ledController;

void setup() {
    ledController.begin();
}

void loop() {
    ledController.update();  // Must be called regularly for animations
}
```

### Functions

#### 1. `simpleLed(hexColor, intensity)`
Sets the LED to a specific color and intensity.

- **hexColor**: 32-bit hex color value (0xRRGGBB)
- **intensity**: Brightness level (0-255), limited by `LED_MAX_POWER`

```cpp
ledController.simpleLed(0xFF0000, 128);  // Red at 50% intensity
ledController.simpleLed(0x00FF00, 255);  // Green at full intensity
ledController.simpleLed(0x0000FF, 64);   // Blue at 25% intensity
```

#### 2. `pulseLed(hexColor)`
Starts a continuous pulsing animation.

- **hexColor**: 32-bit hex color value (0xRRGGBB)

```cpp
ledController.pulseLed(0xFF00FF);  // Purple pulsing
ledController.pulseLed(0x00FFFF);  // Cyan pulsing
```

#### 3. `pulseRapid(hexColor, count)`
Performs rapid on/off pulses for alerts.

- **hexColor**: 32-bit hex color value (0xRRGGBB)
- **count**: Number of pulses to perform

```cpp
ledController.pulseRapid(0xFF0000, 3);  // Red alert, 3 pulses
ledController.pulseRapid(0xFF8000, 5);  // Orange alert, 5 pulses
```

#### 4. `turnOff()`
Turns off the LED and stops all animations.

```cpp
ledController.turnOff();
```

## Color Examples

Common colors in hex format:

| Color   | Hex Code |
|---------|----------|
| Red     | 0xFF0000 |
| Green   | 0x00FF00 |
| Blue    | 0x0000FF |
| Yellow  | 0xFFFF00 |
| Purple  | 0xFF00FF |
| Cyan    | 0x00FFFF |
| White   | 0xFFFFFF |
| Orange  | 0xFF8000 |
| Pink    | 0xFF69B4 |

## Configuration

### Hardware Configuration
- **LED_PIN**: GPIO16 (can be changed in header file)
- **NUM_LEDS**: 1 (single LED, expandable)
- **LED_MAX_POWER**: 255 (maximum brightness limit)

### Timing Configuration
- **Pulse Animation**: Updates every 20ms for smooth effect
- **Rapid Pulse**: 200ms on, 100ms off pattern

## Serial Commands

When integrated into the main project, the following serial commands are available:

```
ledon <hex> <intensity>  - Turn LED on with hex color and intensity
ledoff                   - Turn LED off  
pulse <hex>              - Start pulsing LED with hex color
rapid <hex> <count>      - Rapid pulse LED for count times
```

### Command Examples

```
ledon FF0000 128    // Red LED at 50% intensity
ledon 00FF00 255    // Green LED at full intensity
pulse 0000FF        // Blue pulsing
rapid FF8000 3      // Orange rapid pulse 3 times
ledoff              // Turn off LED
```

## Performance Considerations

- The `update()` function must be called regularly in the main loop
- Animations are non-blocking and run asynchronously
- Small delay (10ms) in main loop recommended to prevent excessive CPU usage
- LED_MAX_POWER can be reduced to limit power consumption

## Troubleshooting

### LED Not Working
1. Check wiring connections
2. Verify power supply voltage (5V recommended)
3. Check if GPIO16 is available and not used by other peripherals
4. Try adding a 470Ω resistor in series with data line

### Flickering or Unstable Colors
1. Ensure stable power supply
2. Check for loose connections
3. Verify FastLED library is properly installed
4. Try reducing LED_MAX_POWER value

### Performance Issues
1. Ensure `update()` is called regularly in loop
2. Avoid blocking delays in main loop
3. Consider reducing animation update frequency if needed

## Example Projects

See `examples/LedControllerExample.cpp` for a complete working example with interactive serial commands.

## License

This project is part of the TilkieTalkie-BoardTester project and follows the same license terms.
