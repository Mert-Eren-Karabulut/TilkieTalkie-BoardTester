#ifndef NFCCONTROLLER_H
#define NFCCONTROLLER_H

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <functional>

// Pin definitions based on the working example sketch
#define POGO_SWITCH_PIN 6     // ESP32-S3
#define NFC_SDA_PIN 16        // ESP32-S3
#define NFC_SCL_PIN 15        // ESP32-S3
#define NFC_IRQ_PIN -1        // No IRQ pin wired for PN532 on this board
#define NFC_RESET_PIN -1      // No reset pin wired for PN532 on this board

// ISO 14443A UIDs are 4, 7 or 10 bytes. The reader library copies as many bytes as the
// response claims, so reads go into a scratch buffer of 255 bytes and only a validated UID of
// at most MAX_UID_LENGTH bytes is kept.
#define MAX_UID_LENGTH 10

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
    bool powerDown();

    // Callback setters
    void setAfterNFCReadCallback(std::function<void(const NFCData &)> cb);
    void setAfterDetachNFCCallback(std::function<void()> cb);

    // Bench simulation: behaves exactly like a mounted figure (callbacks, presence, UID)
    // without the top PCB. `tap <uid>` / `untap` on the console.
    void simulateCard(const String &uid);
    void clearSimulation();
    bool isSimulated() const { return simulated; }

    // Getters for status
    bool isNFCReady() const;
    // Factory test: asks the PN532 for its firmware version now (0 = no answer). Loop task.
    uint32_t firmwareVersion();
    bool isPogoSwitchActive() const;
    bool isCardPresent() const;
    NFCData currentNFCData() const;   // copy taken under the data lock (any task)
    String currentUidCopy() const;    // just the UID text, under the data lock (any task)
    // True once per pogo session when a figure has been on the pins for NFC_READ_TROUBLE_MS
    // without a card being read (the loop raises NFC_READ_TROUBLE).
    bool consumeReadTrouble();

    static const unsigned long NFC_READ_TROUBLE_MS = 3000;
    // Consecutive failed reads (figure on the pins, nothing read) after which the reader is
    // re-initialised (NFC_RESET). With the read backoff this is roughly every 45 s.
    static const uint16_t NFC_REINIT_AFTER_FAILURES = 100;

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
    bool simulated = false;
    String lastReadUID;
    NFCData dockedCardData;           // written by the loop task under dataLock
    SemaphoreHandle_t dataLock = nullptr;

    // Debouncing for the pogo switch sense line
    unsigned long lastDebounceTime;
    bool pendingPogoState;
    unsigned long pogoEngagedAt = 0;  // millis() of the current session start
    bool readTroubleChecked = false;  // the 3 s check ran for this session
    volatile bool readTroublePending = false;

    // NFC reading timing control
    unsigned long lastNFCReadAttempt;
    static const unsigned long NFC_READ_INTERVAL = 100; // Read attempt every 100ms
    uint16_t consecutiveFailures;

    // Callback function pointers
    std::function<void(const NFCData &)> afterNFCReadCallback;
    std::function<void()> afterDetachNFCCallback;

    // Internal helper methods
    void handlePogoSwitch();
    void handleNFCReading();
    void reinitReader();
    bool probeReader();                 // one getFirmwareVersion() (+ SAMConfig when it answers)
    unsigned long lastHealthProbeAt = 0;
    uint8_t healthProbeFailures = 0;
    void serviceReaderHealth();         // update(): re-init a missing reader, notice a dead one
    void setDockedCard(const NFCData &data);
    void invalidateDockedCard();
};

#endif // NFCCONTROLLER_H
