#include "NfcController.h"
#include "Faults.h"
#include <driver/gpio.h>
#include <esp_log.h>

namespace
{
const char *TAG = "nfc";

// RAII for the data lock (no-op if the mutex could not be created).
class DataLock
{
public:
    explicit DataLock(SemaphoreHandle_t m) : mutex(m), held(m && xSemaphoreTake(m, portMAX_DELAY) == pdTRUE) {}
    ~DataLock()
    {
        if (held) xSemaphoreGive(mutex);
    }
    DataLock(const DataLock &) = delete;
    DataLock &operator=(const DataLock &) = delete;

private:
    SemaphoreHandle_t mutex;
    bool held;
};
} // namespace

// Constructor: Initialize the TwoWire object for I2C bus 1 (Wire1)
// and pass its address to the Adafruit_PN532 constructor.
NfcController::NfcController() : I2C_NFC(1), // Use I2C bus 1
                                 nfc(NFC_IRQ_PIN, NFC_RESET_PIN, &I2C_NFC),
                                 nfcReady(false),
                                 pogoActive(false),
                                 cardPresent(false),
                                 cardReadInSession(false),
                                 lastDebounceTime(0),
                                 pendingPogoState(false),
                                 lastNFCReadAttempt(0),
                                 consecutiveFailures(0)
{
    // The UID is read by the audio task (isNfcSessionActive) and the network code while the
    // loop task replaces it: a String copy needs a real mutex (it allocates).
    dataLock = xSemaphoreCreateMutex();
}

uint32_t NfcController::firmwareVersion()
{
    return nfc.getFirmwareVersion();
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
    pendingPogoState = digitalRead(POGO_SWITCH_PIN);

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
            ESP_LOGI(TAG, "Found chip PN5%X.%X", (unsigned)((versiondata >> 16) & 0xFF), (unsigned)((versiondata >> 8) & 0xFF));

            // Configure board to read RFID tags
            nfc.SAMConfig();

            nfcReady = true;
            consecutiveFailures = 0;
            return true;
        }

        ESP_LOGW(TAG, "NFC init attempt %d failed, retrying...", attempts + 1);
        delay(100);
    }

    ESP_LOGE(TAG, "ERROR: PN532 not found on Wire1 after 3 attempts! Check wiring on SDA=%d, SCL=%d, reset GPIO=%d.",
             NFC_SDA_PIN, NFC_SCL_PIN, NFC_RESET_PIN);
    nfcReady = false;
    return false;
}

void NfcController::update()
{
    if (simulated)
    {
        return;
    }
    serviceReaderHealth();
    // The pogo switch is read even without a reader: a figure placed on a dead reader still
    // gets the "could not read the figure" prompt after 3 s instead of silence.
    handlePogoSwitch();
    if (!nfcReady)
    {
        if (pogoActive && !readTroubleChecked && millis() - pogoEngagedAt > NFC_READ_TROUBLE_MS)
        {
            readTroubleChecked = true;
            readTroublePending = true;
        }
        return;
    }

    // Only attempt NFC reading while the pogo contacts are shorted.
    if (pogoActive && !cardReadInSession)
    {
        handleNFCReading();
    }

    // A figure on the pins whose card is not read within NFC_READ_TROUBLE_MS: flagged once
    // per session for the loop (NFC_READ_TROUBLE prompt and fault).
    if (pogoActive && !readTroubleChecked && millis() - pogoEngagedAt > NFC_READ_TROUBLE_MS)
    {
        readTroubleChecked = true;
        if (!cardPresent)
        {
            readTroublePending = true;
        }
    }
}

bool NfcController::consumeReadTrouble()
{
    const bool v = readTroublePending;
    readTroublePending = false;
    return v;
}

void NfcController::setDockedCard(const NFCData &data)
{
    DataLock lock(dataLock);
    dockedCardData = data;
}

void NfcController::invalidateDockedCard()
{
    DataLock lock(dataLock);
    dockedCardData.isValid = false;
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
                ESP_LOGI(TAG, "Pogo switch engaged. NFC session started.");
                cardReadInSession = false; // Reset session flag
                lastReadUID = "";          // Clear last read UID for the new session
                consecutiveFailures = 0;   // Reset failure count for fresh start
                pogoEngagedAt = millis();
                readTroubleChecked = false;
                readTroublePending = false;
            }
            else
            {
                // Session ends
                ESP_LOGI(TAG, "Pogo switch released. NFC session ended.");
                if (cardReadInSession && afterDetachNFCCallback)
                {
                    // Only call the detach hook if a card was actually read
                    afterDetachNFCCallback();
                }
                cardPresent = false;            // Card is no longer considered present
                cardReadInSession = false;      // Reset session flag
                readTroublePending = false;
                invalidateDockedCard();
            }
        }
    }
}

void NfcController::handleNFCReading()
{
    // Adaptive timing: progressive backoff while nothing is read, to keep I2C traffic (and
    // the I2C error 263 timeouts) down.
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
    lastNFCReadAttempt = currentTime;

    // The library writes as many UID bytes as the response's length byte says (up to 255):
    // read into a buffer that cannot overflow, keep the UID only once its length is valid.
    uint8_t raw[256] = {0};
    uint8_t uidLength = 0;
    const bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, raw, &uidLength, 70);

    if (!success)
    {
        consecutiveFailures++;
        // Reduce logging frequency to avoid serial spam
        if (consecutiveFailures % 200 == 0)
        {
            ESP_LOGW(TAG, "WARNING: %u consecutive NFC read failures (using %lums interval)",
                     consecutiveFailures, readInterval);
        }
        if (consecutiveFailures % NFC_REINIT_AFTER_FAILURES == 0)
        {
            reinitReader();
        }
        return;
    }

    if (uidLength == 0 || uidLength > MAX_UID_LENGTH)
    {
        consecutiveFailures++;
        if (consecutiveFailures == 1 || consecutiveFailures % 200 == 0)
        {
            ESP_LOGE(TAG, "ERROR: UID length %d outside 1..%d - ignored", uidLength, MAX_UID_LENGTH);
        }
        return;
    }
    cardPresent = true;
    consecutiveFailures = 0;

    String currentUID;
    currentUID.reserve(uidLength * 3); // Pre-allocate memory
    for (uint8_t i = 0; i < uidLength; i++)
    {
        if (i > 0)
            currentUID += "-";
        if (raw[i] < 0x10)
            currentUID += "0";
        currentUID += String(raw[i], HEX);
    }
    currentUID.toUpperCase();

    // Check if this is a new card in this session
    if (currentUID != lastReadUID)
    {
        ESP_LOGI(TAG, "Found new card!");
        lastReadUID = currentUID; // Update the last read UID
        cardReadInSession = true; // Mark that a card has been read in this session

        NFCData data;
        memcpy(data.uid, raw, uidLength);
        data.uidLength = uidLength;
        data.uidString = currentUID;
        data.timestamp = millis();
        data.isValid = true;
        setDockedCard(data);

        // The callback gets our copy; the lock is not held while it runs.
        if (afterNFCReadCallback)
        {
            afterNFCReadCallback(data);
        }
    }
    // Card removal is detected by the pogo switch, not here.
}

// Reader watchdog: a figure on the pins and NFC_REINIT_AFTER_FAILURES reads without an answer
// (wedged PN532 after an I2C glitch, or simply a figure without a readable card): run the
// reader's init sequence again. Harmless when the reader was fine.
bool NfcController::probeReader()
{
    const uint32_t versiondata = nfc.getFirmwareVersion();
    if (versiondata)
    {
        nfc.SAMConfig();
        return true;
    }
    return false;
}

// Reader health (docs/FAULTS.md 701): a reader that was missing at boot is tried again every
// 10 s (nfc.begin() + firmware version + SAMConfig) and comes back without a reboot; a reader
// that stops answering while no figure is on the pins (the read loop only runs with a figure)
// is noticed by a firmware-version probe every 10 s, three misses in a row, one re-init, and
// then 701 until it answers again. The fault owns the red LED while it is active.
void NfcController::serviceReaderHealth()
{
    const unsigned long now = millis();
    if (now - lastHealthProbeAt < 10000UL)
    {
        return;
    }
    lastHealthProbeAt = now;
    if (!nfcReady)
    {
        nfc.begin();
        if (probeReader())
        {
            nfcReady = true;
            consecutiveFailures = 0;
            healthProbeFailures = 0;
            ESP_LOGW(TAG, "NFC reader is back");
            Faults::clear(Fault::NFC_INIT_FAILED);
        }
        return;
    }
    if (pogoActive)
    {
        return; // the read loop is the probe while a figure is on the pins
    }
    if (probeReader())
    {
        healthProbeFailures = 0;
        Faults::clear(Fault::NFC_INIT_FAILED); // a reader that answers never carries 701 (also clears an injected one)
        return;
    }
    if (++healthProbeFailures < 3)
    {
        return;
    }
    healthProbeFailures = 0;
    nfc.begin();
    if (probeReader())
    {
        ESP_LOGW(TAG, "NFC reader needed a re-init (idle probe)");
        Faults::raise(Fault::NFC_RESET, "idle noack", false);
        return;
    }
    nfcReady = false;
    ESP_LOGE(TAG, "NFC reader stopped answering");
    Faults::raise(Fault::NFC_INIT_FAILED, "runtime"); // spoken: nfc_fault (Çilek)
}

void NfcController::reinitReader()
{
    nfc.begin();
    const uint32_t versiondata = nfc.getFirmwareVersion();
    if (versiondata)
    {
        nfc.SAMConfig();
    }
    ESP_LOGW(TAG, "reader re-initialised after %u failed reads (%s)", consecutiveFailures,
             versiondata ? "chip answered" : "chip not answering");
    char detail[24];
    snprintf(detail, sizeof(detail), "failures=%u%s", consecutiveFailures, versiondata ? "" : " noack");
    Faults::raise(Fault::NFC_RESET, detail, false);
}

void NfcController::simulateCard(const String &uid)
{
    simulated = true;
    pogoActive = true;
    cardReadInSession = true;
    lastReadUID = uid;
    NFCData data;
    data.uidString = uid;
    data.timestamp = millis();
    data.isValid = true;
    setDockedCard(data);
    cardPresent = true;
    ESP_LOGI(TAG, "SIMULATED figure %s mounted", uid.c_str());
    if (afterNFCReadCallback)
    {
        afterNFCReadCallback(data);
    }
}

void NfcController::clearSimulation()
{
    if (!simulated)
    {
        return;
    }
    ESP_LOGI(TAG, "SIMULATED figure removed");
    if (cardReadInSession && afterDetachNFCCallback)
    {
        afterDetachNFCCallback();
    }
    cardPresent = false;
    cardReadInSession = false;
    invalidateDockedCard();
    pogoActive = false;
    lastReadUID = "";
    simulated = false;
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
    ESP_LOGI(TAG, "PN532 power-down %s", acked ? "OK" : "FAILED");
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
    DataLock lock(dataLock);
    return dockedCardData;
}

String NfcController::currentUidCopy() const
{
    DataLock lock(dataLock);
    return dockedCardData.isValid ? dockedCardData.uidString : String();
}

void NfcController::diagnostics()
{
    ESP_LOGI(TAG, "--- NFC Controller Diagnostics ---");
    if (!nfcReady)
    {
        ESP_LOGW(TAG, "NFC board not found. Check wiring and I2C address.");
        return;
    }

    uint32_t versiondata = nfc.getFirmwareVersion();
    ESP_LOGI(TAG, "Firmware version: %u.%u", (unsigned)((versiondata >> 16) & 0xFF), (unsigned)((versiondata >> 8) & 0xFF));

    ESP_LOGI(TAG, "Place a card on the reader to test communication...");
    uint8_t raw[256] = {0}; // the library may write up to 255 UID bytes
    uint8_t uidLength = 0;
    uint8_t success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, raw, &uidLength, 1000);

    if (success)
    {
        ESP_LOGI(TAG, "Diagnostics PASSED: Successfully read a card (UID %u bytes).", uidLength);
    }
    else
    {
        ESP_LOGW(TAG, "Diagnostics FAILED: Could not read a card within 1 second.");
    }
    ESP_LOGI(TAG, "--------------------------------");
}
