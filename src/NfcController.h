#ifndef NFCCONTROLLER_H
#define NFCCONTROLLER_H

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <functional>

// Pin definitions based on the working example sketch
#define POGO_SWITCH_PIN 6     // ESP32-S3
#define NFC_SDA_PIN 16        // ESP32-S3
#define NFC_SCL_PIN 15        // ESP32-S3
#define NFC_IRQ_PIN -1        // No IRQ pin wired for PN532 on this board
#define NFC_RESET_PIN -1      // No reset pin wired for PN532 on this board

// Constants for NFC operations
#define MAX_UID_LENGTH 7
#define NFC_READ_TIMEOUT_MS 50

static const unsigned long POGO_ENGAGE_DEBOUNCE_MS = 50;
static const unsigned long POGO_RELEASE_DEBOUNCE_MS = 250;

// Data structure to hold NFC card information
struct NFCData
{
    uint8_t uid[MAX_UID_LENGTH];
    uint8_t uidLength;
    String uidString;
    unsigned long timestamp;
    bool isValid;

    NFCData() : uidLength(0), timestamp(0), isValid(false)
    {
        memset(uid, 0, sizeof(uid));
    }
};

class NfcController
{
public:
    // Singleton access
    static NfcController &getInstance()
    {
        static NfcController instance;
        return instance;
    }

    // Public methods
    bool begin();
    void update();
    void diagnostics();

    // Callback setters
    void setAfterNFCReadCallback(std::function<void(const NFCData &)> cb);
    void setAfterDetachNFCCallback(std::function<void()> cb);

    // Getters for status
    bool isNFCReady() const;
    bool isPogoSwitchActive() const;
    bool isCardPresent() const;
    NFCData currentNFCData() const;

private:
    // Private constructor for Singleton
    NfcController();
    // Delete copy and assignment operators
    NfcController(const NfcController &) = delete;
    void operator=(const NfcController &) = delete;

    // A dedicated I2C interface for the NFC controller (uses Wire1)
    TwoWire I2C_NFC;

    // PN532 instance
    Adafruit_PN532 nfc;

    // State variables
    bool nfcReady;
    bool pogoActive;
    bool cardPresent;
    bool cardReadInSession;
    String lastReadUID;
    NFCData dockedCardData;

    // Debouncing for the pogo switch sense line
    unsigned long lastDebounceTime;
    bool lastPogoState;
    bool pendingPogoState;

    // NFC reading timing control
    unsigned long lastNFCReadAttempt;
    unsigned long lastSuccessfulNFCRead;
    static const unsigned long NFC_READ_INTERVAL = 100; // Read attempt every 100ms
    static const unsigned long NFC_WATCHDOG_TIMEOUT = 30000; // 30 seconds without successful read
    uint16_t consecutiveFailures;

    // Callback function pointers
    std::function<void(const NFCData &)> afterNFCReadCallback;
    std::function<void()> afterDetachNFCCallback;

    // Internal helper methods
    void handlePogoSwitch();
    void handleNFCReading();
};

#endif // NFCCONTROLLER_H
