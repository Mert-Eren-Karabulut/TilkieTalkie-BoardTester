#ifndef NFCCONTROLLER_H
#define NFCCONTROLLER_H

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <functional>

// Pin definitions based on the working example sketch
#define REED_SWITCH_PIN 4
#define NFC_SDA_PIN 22
#define NFC_SCL_PIN 21
#define NFC_IRQ_PIN 33
#define NFC_RESET_PIN 17

// Data structure to hold NFC card information
struct NFCData
{
    uint8_t uid[7];
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
    bool isReedSwitchActive() const;
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
    bool reedActive;
    bool cardPresent;
    bool cardReadInSession;
    String lastReadUID;
    NFCData dockedCardData;

    // Debouncing for the reed switch
    unsigned long lastDebounceTime;
    bool lastReedState;

    // Callback function pointers
    std::function<void(const NFCData &)> afterNFCReadCallback;
    std::function<void()> afterDetachNFCCallback;

    // Internal helper methods
    void handleReedSwitch();
    void handleNFCReading();
};

#endif // NFCCONTROLLER_H
