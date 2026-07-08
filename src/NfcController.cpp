#include "NfcController.h"
#include <driver/gpio.h>

// Constructor: Initialize the TwoWire object for I2C bus 1 (Wire1)
// and pass its address to the Adafruit_PN532 constructor.
NfcController::NfcController() : I2C_NFC(1), // Use I2C bus 1
                                 nfc(NFC_IRQ_PIN, NFC_RESET_PIN, &I2C_NFC),
                                 nfcReady(false),
                                 pogoActive(false),
                                 cardPresent(false),
                                 cardReadInSession(false),
                                 lastDebounceTime(0),
                                 lastPogoState(false),
                                 pendingPogoState(false),
                                 lastNFCReadAttempt(0),
                                 lastSuccessfulNFCRead(0),
                                 consecutiveFailures(0)
{
    // The afterNFCReadCallback and afterDetachNFCCallback are initialized to nullptr by default
}

bool NfcController::begin()
{
    // Release the deep-sleep latch on the I2C lines (parked high by the sleep path so
    // the powered-down PN532 doesn't see a floating bus). Without this the pins stay
    // frozen and the bus is dead after a wake.
    gpio_hold_dis((gpio_num_t)NFC_SDA_PIN);
    gpio_hold_dis((gpio_num_t)NFC_SCL_PIN);

    // Configure the pogo sense pin.
    pinMode(POGO_SWITCH_PIN, INPUT);
    lastPogoState = digitalRead(POGO_SWITCH_PIN);
    pendingPogoState = lastPogoState;

    // Initialize our dedicated I2C bus with custom pins
    I2C_NFC.begin(NFC_SDA_PIN, NFC_SCL_PIN);
    delay(100);

    // Attempt multiple times to initialize NFC module (hardware can be finicky)
    for (int attempts = 0; attempts < 3; attempts++)
    {
        nfc.begin();
        delay(50); // Brief delay between attempts
        
        uint32_t versiondata = nfc.getFirmwareVersion();
        if (versiondata)
        {
            // Print firmware version
            Serial.print("Found chip PN5");
            Serial.print((versiondata >> 16) & 0xFF, HEX);
            Serial.print(".");
            Serial.println((versiondata >> 8) & 0xFF, HEX);

            // Configure board to read RFID tags
            nfc.SAMConfig();
            
            nfcReady = true;
            lastSuccessfulNFCRead = millis(); // Initialize watchdog timer
            consecutiveFailures = 0;
            return true;
        }
        
        Serial.printf("NFC init attempt %d failed, retrying...\n", attempts + 1);
        delay(100);
    }

    Serial.printf("ERROR: PN532 not found on Wire1 after 3 attempts! Check wiring on SDA=%d, SCL=%d, reset GPIO=%d.\n",
                  NFC_SDA_PIN, NFC_SCL_PIN, NFC_RESET_PIN);
    nfcReady = false;
    return false;
}

void NfcController::update()
{
    if (!nfcReady)
    {
        return;
    }
    handlePogoSwitch();
    
    // Only attempt NFC reading while the pogo contacts are shorted.
    if (pogoActive && !cardReadInSession)
    {
        handleNFCReading();
    }
}

void NfcController::handlePogoSwitch()
{
    bool currentPogoState = digitalRead(POGO_SWITCH_PIN);

    if (currentPogoState != pendingPogoState)
    {
        pendingPogoState = currentPogoState;
        lastDebounceTime = millis();
    }

    unsigned long debounceDelay = pendingPogoState ?
        POGO_ENGAGE_DEBOUNCE_MS :
        POGO_RELEASE_DEBOUNCE_MS;

    if ((millis() - lastDebounceTime) > debounceDelay)
    {
        if (pendingPogoState != pogoActive)
        {
            pogoActive = pendingPogoState;
            if (pogoActive)
            {
                // New session starts
                Serial.println("Pogo switch engaged. NFC session started.");
                cardReadInSession = false; // Reset session flag
                lastReadUID = "";          // Clear last read UID for the new session
                consecutiveFailures = 0;   // Reset failure count for fresh start
            }
            else
            {
                // Session ends
                Serial.println("Pogo switch released. NFC session ended.");
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

    lastPogoState = currentPogoState;
}

void NfcController::handleNFCReading()
{
    // Optimized NFC reading with adaptive timing to reduce I2C Error 263 timeouts.
    // Uses progressive backoff when no card is present to minimize I2C bus traffic.
    // Reduces timeout from 50ms to 25ms to fail faster and prevent bus blocking.
    
    // Only attempt NFC reading at intervals to avoid blocking the main thread
    unsigned long currentTime = millis();
    
    // Dynamic read interval based on consecutive failures to reduce I2C traffic
    unsigned long readInterval = NFC_READ_INTERVAL;
    if (consecutiveFailures > 50)
    {
        // After 50 failures (5 seconds), slow down to every 500ms
        readInterval = 500;
    }
    else if (consecutiveFailures > 20)
    {
        // After 20 failures (2 seconds), slow down to every 300ms  
        readInterval = 300;
    }
    else if (consecutiveFailures > 10)
    {
        // After 10 failures (1 second), slow down to every 200ms
        readInterval = 200;
    }
    
    if (currentTime - lastNFCReadAttempt < readInterval)
    {
        return; // Too soon for another read attempt
    }
    
    // Check for NFC watchdog timeout - reinitialize if stuck
    if (lastSuccessfulNFCRead > 0 && (currentTime - lastSuccessfulNFCRead) > NFC_WATCHDOG_TIMEOUT)
    {
        Serial.println("WARNING: NFC watchdog timeout, attempting recovery...");
        consecutiveFailures = 0;
        lastSuccessfulNFCRead = currentTime; // Reset to prevent spam
        // Could add nfc.begin() here for full recovery if needed
    }
    
    lastNFCReadAttempt = currentTime;

    uint8_t success;
    uint8_t uid[MAX_UID_LENGTH] = {0}; // Buffer to store the returned UID
    uint8_t uidLength;                 // Length of the UID (4 or 7 bytes)

    success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 70);

    if (success)
    {
        cardPresent = true;
        consecutiveFailures = 0;
        lastSuccessfulNFCRead = currentTime;
        
        // Validate UID length
        if (uidLength > MAX_UID_LENGTH)
        {
            Serial.printf("ERROR: UID length %d exceeds maximum %d\n", uidLength, MAX_UID_LENGTH);
            return;
        }
        
        String currentUID;
        currentUID.reserve(uidLength * 3); // Pre-allocate memory
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
    else
    {
        consecutiveFailures++;
        // Reduce logging frequency to avoid serial spam
        if (consecutiveFailures % 200 == 0)  // Every 200 failures instead of 100
        {
            Serial.printf("WARNING: %u consecutive NFC read failures (using %lums interval)\n", 
                         consecutiveFailures, readInterval);
        }
    }
    // Note: No need for card removal detection here since reed switch handles that
}

bool NfcController::powerDown()
{
    // Put the PN532 into its software Power Down state (~µA), wake on I2C activity.
    // Board constraint (Adem v7): the PN532's VBAT/PVDD sit on the ALWAYS-ON 3.3V rail
    // and RSTPD_N is pulled high on the top PCB, so dropping GPIO4 does not de-power it.
    // Left alone it idles at ~30-45mA through deep sleep and drains the cell — this
    // command is the only off switch firmware has. Must work even when begin() never
    // ran (battery-monitor phase), so it brings the I2C bus up on demand.
    // Release any deep-sleep latch first (a monitor-phase wake never runs begin()).
    gpio_hold_dis((gpio_num_t)NFC_SDA_PIN);
    gpio_hold_dis((gpio_num_t)NFC_SCL_PIN);
    if (!nfcReady)
    {
        I2C_NFC.begin(NFC_SDA_PIN, NFC_SCL_PIN);
        nfc.begin();
        delay(10);
    }

    // PowerDown (0x16), WakeUpEnable 0x80 = wake on I2C address match (UM0701-02 §7.2.11).
    uint8_t cmd[2] = {0x16, 0x80};
    bool acked = false;
    for (int attempt = 0; attempt < 3 && !acked; ++attempt)
    {
        acked = nfc.sendCommandCheckAck(cmd, sizeof(cmd), 250);
        if (!acked)
        {
            delay(20);
        }
    }

    if (acked)
    {
        delay(3);         // PN532 needs ~1ms after the response to actually power down
        nfcReady = false; // any further NFC use requires begin() again
    }
    Serial.printf("NfcController: PN532 power-down %s\n", acked ? "OK" : "FAILED");
    return acked;
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

bool NfcController::isPogoSwitchActive() const
{
    return pogoActive;
}

bool NfcController::isCardPresent() const
{
    return cardPresent && pogoActive;
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
    uint8_t uid[MAX_UID_LENGTH] = {0};
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
