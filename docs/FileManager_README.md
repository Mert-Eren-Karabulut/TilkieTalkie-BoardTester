# ESP32 FileManager

A comprehensive file management system for ESP32 with SD card support and intelligent audio file downloading capabilities.

## Features

### Core Functionality
- ✅ SD card initialization and management
- ✅ File operations (read, write, delete, copy, rename)
- ✅ Directory management (create, list, remove)
- ✅ Persistent download queue across reboots
- ✅ Automatic retry mechanism with exponential backoff
- ✅ File integrity verification with checksums
- ✅ Download progress monitoring and callbacks

### Smart Download Management
- 🔋 **Charging-only downloads** - Protects battery life
- 🌐 **Internet connectivity checks** - Avoids unnecessary retries
- 📥 **Background download processing** - Non-blocking operation
- 🔄 **Automatic retry logic** - Handles temporary failures
- 📊 **Download statistics tracking** - Monitor success rates
- 💾 **NVS persistence** - Survives reboots and power cycles

### Required Files System
- 📋 Maintain a list of files that must be present
- 🔍 Automatic missing file detection
- 📲 Automatic download of missing files
- ✅ File integrity verification

## Hardware Configuration

```cpp
// SD Card Pin Connections
SD_CS   = IO15  // Chip Select
SD_MISO = IO12  // Master In Slave Out
SD_MOSI = IO13  // Master Out Slave In  
SD_CLK  = IO14  // Clock

// CRITICAL: Peripheral Power Control
IO17    = HIGH  // Must be HIGH to power SD card and peripherals
```

**⚠️ IMPORTANT:** GPIO17 controls power to peripherals including the SD card. This pin MUST be set HIGH before initializing any peripherals, or they will not function.

## Quick Start

### 1. Include the FileManager

```cpp
#include "FileManager.h"

FileManager& fileManager = FileManager::getInstance();
```

### 2. Initialize in setup()

```cpp
void setup() {
    // Initialize other components first (WiFi, Battery)
    
    // CRITICAL: Enable peripheral power first!
    pinMode(17, OUTPUT);
    digitalWrite(17, HIGH);  // Enable power to peripherals
    delay(100);  // Give peripherals time to power up
    
    if (!fileManager.begin()) {
        Serial.println("FileManager initialization failed!");
        return;
    }
    
    Serial.println("FileManager ready!");
}
```

### 3. Update in loop()

```cpp
void loop() {
    fileManager.update(); // Handles background downloads
    delay(100);
}
```

## Basic Usage Examples

### File Operations

```cpp
// Write a file
fileManager.writeFile("/logs/system.log", "System started");

// Read a file
String content = fileManager.readFile("/config.txt");

// Check if file exists
if (fileManager.fileExists("/audio/welcome.wav")) {
    Serial.println("Welcome sound found!");
}

// Delete a file
fileManager.deleteFile("/temp/old_file.tmp");

// Copy a file
fileManager.copyFile("/audio/original.wav", "/audio/backup.wav");

// Get file size
size_t size = fileManager.getFileSize("/audio/large_file.wav");
```

### Directory Operations

```cpp
// Create directories
fileManager.createDirectory("/audio/system");
fileManager.createDirectory("/logs/debug");

// List files in directory
std::vector<String> files = fileManager.listFiles("/audio");
for (const auto& file : files) {
    Serial.println("Found: " + file);
}

// Remove directory
fileManager.removeDirectory("/temp");
```

### Download Management

```cpp
// Schedule a download (will happen when charging + WiFi available)
fileManager.scheduleDownload(
    "https://example.com/audio/welcome.wav", 
    "/audio/welcome.wav"
);

// Download immediately (if conditions are met)
String errorMsg;
if (fileManager.downloadNow(
    "https://example.com/test.wav", 
    "/audio/test.wav", 
    errorMsg)) {
    Serial.println("Download successful!");
} else {
    Serial.println("Download failed: " + errorMsg);
}

// Check download status
int pending = fileManager.getPendingDownloadsCount();
bool inProgress = fileManager.isDownloadInProgress();
```

### Required Files Management

```cpp
// Add files that must be present on the system
fileManager.addRequiredFile(
    "/audio/startup.wav", 
    "https://server.com/sounds/startup.wav",
    "abc123def456"  // Optional checksum for integrity
);

fileManager.addRequiredFile(
    "/audio/shutdown.wav", 
    "https://server.com/sounds/shutdown.wav"
);

// Check for missing files
std::vector<String> missing = fileManager.getMissingFiles();
if (missing.size() > 0) {
    Serial.printf("Missing %d files\n", missing.size());
    for (const auto& file : missing) {
        Serial.println("  - " + file);
    }
}

// Trigger check and download of missing files
fileManager.checkRequiredFiles();
```

## Download Conditions

Downloads only occur when ALL conditions are met:

1. **Device is charging** (`BatteryManager::getChargingStatus() == true`)
2. **WiFi is connected** (`WiFi.status() == WL_CONNECTED`)
3. **Internet is available** (HTTP connectivity test)
4. **No other download in progress**

This ensures:
- Battery life is preserved
- Downloads don't fail due to connectivity issues
- System resources aren't overloaded

## Download Queue Behavior

- **Persistent**: Queue survives reboots via NVS storage
- **Retry Logic**: Failed downloads retry up to 5 times
- **Backoff**: 30-second delay between retry attempts
- **Integrity**: Optional checksum verification
- **Progress**: Callback support for monitoring progress

## Error Handling

```cpp
// Set callbacks for monitoring
fileManager.setDownloadProgressCallback([](const String& url, const String& path, int progress, size_t downloaded, size_t total) {
    Serial.printf("Downloading %s: %d%% (%d/%d bytes)\n", 
                  path.c_str(), progress, downloaded, total);
});

fileManager.setDownloadCompleteCallback([](const String& url, const String& path, bool success, const String& error) {
    if (success) {
        Serial.println("✓ Download completed: " + path);
    } else {
        Serial.println("✗ Download failed: " + path + " - " + error);
    }
});
```

## Console Commands

When integrated with the main application, these commands are available:

```
sdinfo     - Show SD card information
files      - List files on SD card  
dlstats    - Show download statistics
dlqueue    - Show download queue
required   - Show required files
checkfiles - Check and download missing files
cleanup    - Clean up temporary files
power      - Show peripheral power status
poweron    - Enable peripheral power (IO17)
poweroff   - Disable peripheral power (IO17)

download <url> <path>  - Schedule download
addfile <path> <url>   - Add required file

Example:
> download https://example.com/test.wav /audio/test.wav
> addfile /audio/welcome.wav https://server.com/welcome.wav
```
## SD Card Information

```cpp
// Get detailed SD card information
String info = fileManager.getSDCardInfo();
Serial.println(info);

// Get space information
size_t total = fileManager.getSDCardTotalSpace();
size_t used = fileManager.getSDCardUsedSpace(); 
size_t free = fileManager.getSDCardFreeSpace();

// Format bytes for display
String totalStr = fileManager.formatBytes(total);  // "4.00 GB"
String usedStr = fileManager.formatBytes(used);    // "1.25 GB" 
String freeStr = fileManager.formatBytes(free);    // "2.75 GB"
```

## Download Statistics

```cpp
// Get statistics
FileManager::DownloadStats stats = fileManager.getDownloadStats();
Serial.printf("Total downloads: %d\n", stats.totalDownloads);
Serial.printf("Successful: %d\n", stats.successfulDownloads);
Serial.printf("Failed: %d\n", stats.failedDownloads);
Serial.printf("Total bytes: %lu\n", stats.totalBytesDownloaded);

// Or get formatted string
String statsStr = fileManager.getDownloadStatsString();
Serial.println(statsStr);

// Reset statistics
fileManager.resetDownloadStats();
```

## File Integrity Verification

```cpp
// Calculate file checksum
String checksum = fileManager.calculateFileChecksum("/audio/test.wav");

// Verify file integrity
bool isValid = fileManager.verifyFile("/audio/test.wav", "expected_checksum");

// Repair corrupted files (re-downloads files that fail checksum)
bool anyRepaired = fileManager.repairCorruptedFiles();
```

## Maintenance Operations

```cpp
// Clean up temporary files
fileManager.cleanupTempFiles();

// Cancel all pending downloads
fileManager.cancelAllDownloads();

// Retry failed downloads (resets retry counters)
fileManager.retryFailedDownloads();
```

## Best Practices

### 1. Initialize Order
```cpp
void setup() {
    // 1. Initialize serial communication
    Serial.begin(115200);
    
    // 2. CRITICAL: Enable peripheral power FIRST!
    pinMode(17, OUTPUT);
    digitalWrite(17, HIGH);  // Enable power to peripherals
    delay(100);  // Give peripherals time to power up
    
    // 3. Initialize WiFi and Battery
    wifiProv.begin();
    battery.begin();
    
    // 4. Then initialize FileManager
    fileManager.begin();
}
```

### 2. Regular Updates
```cpp
void loop() {
    // Update all managers regularly
    battery.update();       // Check charging status
    fileManager.update();   // Process downloads
    
    delay(100); // Don't overwhelm the system
}
```

### 3. Error Handling
```cpp
// Always check return values
if (!fileManager.writeFile("/config.txt", data)) {
    Serial.println("Failed to write config file!");
    // Handle error appropriately
}
```

### 4. Resource Management
```cpp
// Use callbacks instead of polling for large downloads
fileManager.setDownloadProgressCallback(onDownloadProgress);
fileManager.setDownloadCompleteCallback(onDownloadComplete);
```

## Troubleshooting

### SD Card Initialization Failures

If you see errors like:
```
[E][sd_diskio.cpp:126] sdSelectCard(): Select Failed
[E][sd_diskio.cpp:199] sdCommand(): Card Failed! cmd: 0x29
[E][sd_diskio.cpp:806] sdcard_mount(): f_mount failed: (3) The physical drive cannot work
```

**Step 1: Run Diagnostics**
```cpp
// In serial console, type:
sddiag
```

This will test:
- **Peripheral power status (IO17)** - MOST IMPORTANT
- System voltage levels
- Pin connectivity
- SPI communication at different frequencies
- Basic file operations

**Step 2: Check Peripheral Power**

The most common cause of SD card initialization failure is missing peripheral power:

1. **Check Power Status:**
   ```cpp
   // In serial console, type:
   power
   ```

2. **Enable Power if Disabled:**
   ```cpp
   // In serial console, type:
   poweron
   ```

3. **Restart Device:**
   After enabling power, restart the device for clean initialization.

**Step 3: Check Hardware**

1. **Wiring Verification:**
   ```
   ESP32 Pin → SD Card Pin
   GPIO15    → CS (Chip Select)
   GPIO12    → DO (Data Out / MISO)
   GPIO13    → DI (Data In / MOSI)
   GPIO14    → CLK (Clock)
   3.3V      → VCC
   GND       → GND
   
   CRITICAL: GPIO17 → HIGH (Peripheral Power Control)
   ```

2. **Peripheral Power (CRITICAL):**
   - GPIO17 must be HIGH to power SD card
   - Check with `power` command
   - Enable with `poweron` command if disabled
   - This is the #1 cause of SD card failures

3. **Power Supply:**
   - Check battery voltage with `battery` command
   - Voltage should be ≥ 3.2V for reliable SD operation
   - Connect external power if battery is low

4. **SD Card:**
   - Format as FAT32 (not exFAT or NTFS)
   - Use SD cards ≤ 32GB (SDHC)
   - Try a different SD card
   - Check SD card is not write-protected

**Step 4: Test Different Configurations**

```cpp
// Try different SPI frequencies in FileManager.cpp:
SPI.setFrequency(400000);   // Very slow (most compatible)
SPI.setFrequency(1000000);  // Slow
SPI.setFrequency(4000000);  // Medium
```

**Step 5: Alternative Pin Configuration**

If the current pins don't work, try VSPI pins:
```cpp
// In FileManager.h, change to:
static const int SD_CS_PIN = 5;
static const int SD_MISO_PIN = 19;
static const int SD_MOSI_PIN = 23;
static const int SD_CLK_PIN = 18;
```

### SD Card Issues
- Check wiring connections
- Ensure SD card is properly formatted (FAT32)
- Verify SD card is not write-protected
- Check power supply (SD cards can be power-hungry)

### Download Issues
- Verify device is charging (`battery` command)
- Check WiFi connection (`stats` command)
- Test internet connectivity manually
- Check available SD card space (`sdinfo` command)

### Performance Issues
- Increase buffer size for large files
- Implement download scheduling during low-usage periods
- Monitor memory usage during downloads

## Memory Usage

The FileManager is designed to be memory-efficient:
- Uses 8KB download buffer (configurable)
- Streams downloads to SD card (doesn't load entire files into RAM)
- Minimal RAM usage for queue management
- NVS storage for persistence (not RAM)

## Thread Safety

The FileManager is designed for single-threaded use. If using with RTOS tasks:
- Only call FileManager methods from one task
- Use proper synchronization if multiple tasks need file access
- The `update()` method handles background processing safely

## License

This FileManager implementation is part of the TilkieTalkie project.
