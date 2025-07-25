#include "NfcController.h"

// Debounce delay for the reed switch
#define DEBOUNCE_DELAY 50

// Constructor: Initialize the TwoWire object for I2C bus 1 (Wire1)
// and pass its address to the Adafruit_PN532 constructor.
NfcController::NfcController() : I2C_NFC(1), // Use I2C bus 1
                                 nfc(NFC_IRQ_PIN, NFC_RESET_PIN, &I2C_NFC),
                                 nfcReady(false),
                                 reedActive(false),
                                 cardPresent(false),
                                 cardReadInSession(false),
                                 lastDebounceTime(0),
                                 lastReedState(false),
                                 lastNFCReadAttempt(0)
{
    // The afterNFCReadCallback and afterDetachNFCCallback are initialized to nullptr by default
}

bool NfcController::begin()
{
    // Configure the reed switch pin as an input
    pinMode(REED_SWITCH_PIN, INPUT);
    lastReedState = digitalRead(REED_SWITCH_PIN);

    // Initialize our dedicated I2C bus with custom pins
    I2C_NFC.begin(NFC_SDA_PIN, NFC_SCL_PIN);
    delay(100);

    // Now, begin the NFC module communication on its own bus
    nfc.begin();

    uint32_t versiondata = nfc.getFirmwareVersion();
    if (!versiondata)
    {
        Serial.println("ERROR: PN532 not found on Wire1! Check wiring on SDA=22, SCL=21, RST=17.");
        nfcReady = false;
        return false;
    }

    // Print firmware version
    Serial.print("Found chip PN5");
    Serial.print((versiondata >> 16) & 0xFF, HEX);
    Serial.print(".");
    Serial.println((versiondata >> 8) & 0xFF, HEX);

    // Configure board to read RFID tags
    nfc.SAMConfig();

    nfcReady = true;
    return true;
}

void NfcController::update()
{
    if (!nfcReady)
    {
        return;
    }
    handleReedSwitch();
    
    // Only attempt NFC reading when reed switch is active AND no card has been read yet
    if (reedActive && !cardReadInSession)
    {
        handleNFCReading();
    }
}

void NfcController::handleReedSwitch()
{
    bool currentReedState = !digitalRead(REED_SWITCH_PIN);

    // Check if the state has changed
    if (currentReedState != lastReedState)
    {
        lastDebounceTime = millis();
    }

    // If the state has been stable for longer than the debounce delay
    if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY)
    {
        // If the state has truly changed
        if (currentReedState != reedActive)
        {
            reedActive = currentReedState;
            if (reedActive)
            {
                // New session starts
                Serial.println("Reed switch activated. NFC session started.");
                cardReadInSession = false; // Reset session flag
                lastReadUID = "";          // Clear last read UID for the new session
            }
            else
            {
                // Session ends
                Serial.println("Reed switch deactivated. NFC session ended.");
                if (cardReadInSession && afterDetachNFCCallback)
                {
                    // Only call the detach hook if a card was actually read
                    afterDetachNFCCallback();
                }
                cardPresent = false;            // Card is no longer considered present
                cardReadInSession = false;      // Reset session flag
                dockedCardData.isValid = false; // Invalidate the docked card data
            }
        }
    }

    lastReedState = currentReedState;
}

void NfcController::handleNFCReading()
{
    // Only attempt NFC reading at intervals to avoid blocking the main thread
    unsigned long currentTime = millis();
    if (currentTime - lastNFCReadAttempt < NFC_READ_INTERVAL)
    {
        return; // Too soon for another read attempt
    }
    
    lastNFCReadAttempt = currentTime;

    uint8_t success;
    uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0}; // Buffer to store the returned UID
    uint8_t uidLength;                     // Length of the UID (4 or 7 bytes)

    // Use a reasonable timeout - 50ms should be enough for most NFC operations
    success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50); // 50ms timeout

    if (success)
    {
        cardPresent = true;
        String currentUID = "";
        for (uint8_t i = 0; i < uidLength; i++)
        {
            if (i > 0)
                currentUID += "-";
            if (uid[i] < 0x10)
                currentUID += "0";
            currentUID += String(uid[i], HEX);
        }
        currentUID.toUpperCase();

        // Check if this is a new card in this session
        if (currentUID != lastReadUID)
        {
            Serial.println("Found new card!");
            lastReadUID = currentUID; // Update the last read UID
            cardReadInSession = true; // Mark that a card has been read in this session

            // Populate the data structure
            memcpy(dockedCardData.uid, uid, uidLength);
            dockedCardData.uidLength = uidLength;
            dockedCardData.uidString = currentUID;
            dockedCardData.timestamp = millis();
            dockedCardData.isValid = true;

            // Trigger the callback
            if (afterNFCReadCallback)
            {
                afterNFCReadCallback(dockedCardData);
            }
        }
    }
    // Note: No need for card removal detection here since reed switch handles that
}

void NfcController::setAfterNFCReadCallback(std::function<void(const NFCData &)> cb)
{
    afterNFCReadCallback = cb;
}

void NfcController::setAfterDetachNFCCallback(std::function<void()> cb)
{
    afterDetachNFCCallback = cb;
}

bool NfcController::isNFCReady() const
{
    return nfcReady;
}

bool NfcController::isReedSwitchActive() const
{
    return reedActive;
}

bool NfcController::isCardPresent() const
{
    return cardPresent && reedActive;
}

NFCData NfcController::currentNFCData() const
{
    return dockedCardData;
}

void NfcController::diagnostics()
{
    Serial.println("\n--- NFC Controller Diagnostics ---");
    if (!nfcReady)
    {
        Serial.println("NFC board not found. Check wiring and I2C address.");
        return;
    }

    uint32_t versiondata = nfc.getFirmwareVersion();
    Serial.print("Firmware version: ");
    Serial.print((versiondata >> 16) & 0xFF, DEC);
    Serial.print('.');
    Serial.println((versiondata >> 8) & 0xFF, DEC);

    Serial.println("Place a card on the reader to test communication...");
    uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0};
    uint8_t uidLength;
    uint8_t success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 1000);

    if (success)
    {
        Serial.println("Diagnostics PASSED: Successfully read a card.");
    }
    else
    {
        Serial.println("Diagnostics FAILED: Could not read a card within 1 second.");
    }
    Serial.println("--------------------------------\n");
}
