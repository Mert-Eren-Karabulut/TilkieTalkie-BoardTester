# NFC Controller Documentation

## Overview

The NFC Controller provides functionality to read NFC tags using a PN5321A3HN NFC controller connected to an ESP32. It includes reed switch integration to control when NFC reading should be active.

## Hardware Setup

### Pin Connections
- **NFC_SDA** → GPIO22 (I2C Data)
- **NFC_SCL** → GPIO21 (I2C Clock)
- **Reed Switch** → GPIO4 (Control pin)

### Reed Switch Logic
- When GPIO4 is HIGH: NFC reading is active
- When GPIO4 is LOW: NFC reading is disabled
- Built-in debouncing prevents false triggering

## Features

- **Automatic NFC Detection**: Reads NFC tags when reed switch is active
- **Duplicate Prevention**: Same card won't trigger multiple callbacks until reed switch is reset
- **Configurable Settings**: Adjustable read intervals and debounce delays
- **Callback System**: Hooks for card detection and session end events
- **Status Monitoring**: Real-time status of NFC controller and reed switch

## Usage

### Basic Setup

```cpp
#include "NfcController.h"

NfcController nfcController;

void setup() {
    // Initialize NFC controller
    if (nfcController.begin()) {
        // Set up callbacks
        nfcController.setAfterNFCReadCallback(onCardDetected);
        nfcController.setAfterDetachNFCCallback(onSessionEnded);
        
        // Configure settings (optional)
        nfcController.setReadInterval(200);  // Read every 200ms
        nfcController.setDebounceDelay(50);  // 50ms debounce
    }
}

void loop() {
    // Must be called regularly to update NFC status
    nfcController.update();
}
```

### Callback Functions

```cpp
void onCardDetected(const NFCData& nfcData) {
    Serial.println("Card detected: " + nfcData.uidString);
    
    // Example: Play different audio based on card
    if (nfcData.uidString == "04:AB:CD:EF:12:34:56") {
        audioController.play("/sounds/cardA.mp3");
    } else {
        audioController.play("/sounds/default.mp3");
    }
}

void onSessionEnded() {
    Serial.println("NFC session ended");
    // Cleanup actions (stop audio, reset LEDs, etc.)
}
```

## API Reference

### Public Methods

#### `bool begin()`
Initializes the NFC controller and I2C communication.
- **Returns**: `true` if initialization successful, `false` otherwise

#### `void update()`
Must be called regularly in the main loop to monitor reed switch and read NFC tags.

#### `NFCData currentNFCData() const`
Returns the currently detected NFC card data.
- **Returns**: `NFCData` structure with card information

#### `bool isReedSwitchActive() const`
Checks if the reed switch is currently active.
- **Returns**: `true` if reed switch is active (GPIO4 HIGH)

#### `bool isCardPresent() const`
Checks if a valid NFC card is currently present.
- **Returns**: `true` if card is present and reed switch is active

#### `bool isNFCReady() const`
Checks if the NFC controller is initialized and ready.
- **Returns**: `true` if NFC controller is ready

### Configuration Methods

#### `void setReadInterval(unsigned long interval)`
Sets the interval between NFC read attempts when reed switch is active.
- **Parameters**: `interval` - Time in milliseconds (default: 200ms)

#### `void setDebounceDelay(unsigned long delay)`
Sets the debounce delay for the reed switch.
- **Parameters**: `delay` - Time in milliseconds (default: 50ms)

### Callback Setters

#### `void setAfterNFCReadCallback(NFCReadCallback callback)`
Sets the callback function to be called when a new NFC card is detected.
- **Parameters**: `callback` - Function to call with NFCData parameter

#### `void setAfterDetachNFCCallback(NFCDetachCallback callback)`
Sets the callback function to be called when the NFC session ends.
- **Parameters**: `callback` - Function to call with no parameters

### Utility Methods

#### `void printNFCData(const NFCData& data)`
Prints formatted NFC card data to Serial.
- **Parameters**: `data` - NFCData structure to print

## NFCData Structure

```cpp
struct NFCData {
    uint8_t uid[7];          // UID of the NFC tag
    uint8_t uidLength;       // Length of the UID
    uint16_t tagType;        // Type of the NFC tag
    bool isValid;            // Whether the data is valid
    String uidString;        // String representation of UID
    unsigned long timestamp; // When the tag was read
};
```

## Serial Commands

When integrated with the main application, the following commands are available:

- **`nfcstatus`** - Show NFC controller status
- **`nfcdata`** - Show current NFC card data
- **`nfcreed`** - Show reed switch status
- **`nfcdiag`** - Run comprehensive NFC diagnostics

## Example Output

```
=== NFC Card Detected ===
Card UID: 04:AB:CD:EF:12:34:56
Card Type: 0x106
UID Length: 7
Timestamp: 12345678
========================

=== NFC Session Ended ===
Reed switch deactivated or card removed
========================
```

## Troubleshooting

### Common Issues

1. **NFC Controller Not Found / I2C Error 263**
   - Check I2C connections (GPIO21, GPIO22)
   - Verify power supply to NFC module (3.3V, NOT 5V!)
   - Ensure proper grounding
   - Check I2C pull-up resistors (4.7kΩ - 10kΩ) on SDA and SCL lines
   - Try shorter wires (< 20cm)
   - Verify PN532 is in I2C mode (check DIP switches if applicable)
   - Run `nfcdiag` command for detailed diagnostics

2. **Reed Switch Not Working**
   - Check connection to GPIO4
   - Verify reed switch polarity
   - Test with multimeter in continuity mode
   - Ensure proper magnet placement

3. **Multiple Card Readings**
   - This is normal behavior - the controller prevents duplicate callbacks
   - Reset reed switch to read the same card again

4. **Cards Not Detected**
   - Ensure card is close enough to NFC antenna
   - Check if card is compatible (ISO14443A)
   - Verify reed switch is active (GPIO4 HIGH)

### Debug Tips

- Use `nfcdiag` command for comprehensive diagnostics
- Use `nfcstatus` command to check controller state
- Monitor Serial output for detailed debugging information
- Check reed switch state with `nfcreed` command
- Verify I2C communication with oscilloscope if available

### Hardware Troubleshooting

1. **Check Connections**:
   ```
   ESP32          PN532
   GPIO22 (SDA) → SDA
   GPIO21 (SCL) → SCL
   3.3V         → VCC (NOT 5V!)
   GND          → GND
   GPIO4        → Reed Switch
   ```

2. **Verify I2C Pull-ups**: Use 4.7kΩ - 10kΩ resistors on SDA and SCL lines

3. **Check Power Supply**: PN532 requires stable 3.3V power supply

4. **Wire Length**: Keep I2C wires as short as possible (< 20cm)

5. **Mode Configuration**: Ensure PN532 is configured for I2C mode

## Dependencies

- **Adafruit PN532 Library**: `adafruit/Adafruit PN532@^1.3.1`
- **Wire Library**: Built-in I2C library (not directly used as Adafruit library handles I2C)
- **Arduino Framework**: Core Arduino functions

## Notes

- The PN5321A3HN is compatible with PN532 libraries
- Reed switch debouncing prevents false triggering
- NFC reading only occurs when reed switch is active
- Same card won't trigger multiple callbacks until reed switch is reset
- Maximum supported UID length is 7 bytes
