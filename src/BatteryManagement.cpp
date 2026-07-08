#include "BatteryManagement.h"

namespace {
// Charge-current limit currently requested by the system (bring-up / deep-discharge
// recovery). configureChargerDefaults() reapplies the charger config on every device
// rescan (DEVICE_RESCAN_INTERVAL), so it must honor this instead of unconditionally
// restoring DEFAULT_CHARGE_CURRENT_MA — which silently undid a 300mA recovery limit
// every 5 seconds. 0 = no override, use the default.
uint16_t gRequestedChargeCurrentMa = 0;

// Same sticky-override pattern for the input current limit: set from the PD-granted
// budget (CH224Q register 0x50) when a PD source declares itself; 0 = use the default
// (and let ICO trim it for non-PD sources).
uint16_t gRequestedInputLimitMa = 0;

// Sticky charge-inhibit: while true (temperature outside the datasheet charge window,
// or the recovery hold's fault-clock pause), the periodic charger reconfiguration must
// NOT re-assert EN_CHG — without this it silently re-enabled charging within 5s.
bool gChargeInhibited = false;

// Sticky ICO-inhibit: while a PD contract declares the input budget, ICO must stay off
// or it re-detects (blindly — the charger's own D+/D- are unconnected) and overwrites
// IINDPM with a legacy ~1.5A guess (observed: PD 3000mA contract, REG06 back at 1560).
bool gIcoInhibited = false;

constexpr uint8_t kBq25792AdcEnableMask = 0x80;
// REG2E bit6 = ADC_RATE, and SETTING it selects ONE-SHOT conversion (after which
// ADC_EN self-clears — boot dumps read REG2E as 0x70). This keeps the charger at its
// 21µA battery-only quiescent; readings stay fresh because applyPowerConfiguration()
// re-triggers a conversion on every device rescan.
constexpr uint8_t kBq25792AdcOneShotMask = 0x40;
// REG0F Charger Control 0 bit map (datasheet): bit5 = EN_CHG, bit4 = EN_ICO. The old
// single 0x10 mask was EN_ICO mislabeled as charge-enable — charging only worked
// because EN_CHG's POR default is 1, and chargekick / the recovery charge-pause were
// actually toggling ICO. Both bits are handled explicitly now.
constexpr uint8_t kBq25792ChargeEnableMask = 0x20;
constexpr uint8_t kBq25792IcoEnableMask = 0x10;
constexpr uint8_t kBq25792WatchdogMask = 0x07;
constexpr uint8_t kBq25792TsIgnoreMask = 0x01;
constexpr uint16_t kGaugeSecurityMask = 0x0300;
constexpr uint16_t kGaugeSecuritySealed = 0x0300;

bool gaugeConfigurationLooksCorrupted(uint16_t designCapacity,
                                      uint16_t designEnergy,
                                      uint16_t terminateVoltage,
                                      uint16_t chargingVoltage,
                                      uint16_t taperCurrent) {
    return designCapacity < 500
        || designCapacity > 10000
        || designEnergy < 100
        || designEnergy > 5000
        || terminateVoltage < 2400
        || terminateVoltage > 4500
        || chargingVoltage < 3400
        || chargingVoltage > 4500
        || taperCurrent > 2000;
}

void appendFaultLabel(String &faults, const char *label) {
    if (!faults.isEmpty()) {
        faults += ", ";
    }
    faults += label;
}

String chargerFaultStatusToString(uint8_t fault0, uint8_t fault1) {
    String faults;

    if (fault0 & 0x80) appendFaultLabel(faults, "IBAT regulation");
    if (fault0 & 0x40) appendFaultLabel(faults, "VBUS OVP");
    if (fault0 & 0x20) appendFaultLabel(faults, "VBAT OVP");
    if (fault0 & 0x10) appendFaultLabel(faults, "IBUS OCP");
    if (fault0 & 0x08) appendFaultLabel(faults, "IBAT OCP");
    if (fault0 & 0x04) appendFaultLabel(faults, "Converter OCP");
    if (fault0 & 0x02) appendFaultLabel(faults, "VAC2 OVP");
    if (fault0 & 0x01) appendFaultLabel(faults, "VAC1 OVP");

    if (fault1 & 0x80) appendFaultLabel(faults, "VSYS short");
    if (fault1 & 0x40) appendFaultLabel(faults, "VSYS OVP");
    if (fault1 & 0x20) appendFaultLabel(faults, "OTG OVP");
    if (fault1 & 0x10) appendFaultLabel(faults, "OTG UVP");
    if (fault1 & 0x04) appendFaultLabel(faults, "Thermal shutdown");

    return faults.isEmpty() ? String("None") : faults;
}

String chargerTimerStatusToString(uint8_t status3) {
    String statuses;

    if (status3 & 0x08) appendFaultLabel(statuses, "Fast-charge safety timer");
    if (status3 & 0x04) appendFaultLabel(statuses, "Trickle safety timer");
    if (status3 & 0x02) appendFaultLabel(statuses, "Pre-charge safety timer");

    return statuses.isEmpty() ? String("None") : statuses;
}

String chargerTsStatusToString(uint8_t status4) {
    String statuses;

    if (status4 & 0x10) appendFaultLabel(statuses, "VBAT too low for OTG");
    if (status4 & 0x08) appendFaultLabel(statuses, "TS cold");
    if (status4 & 0x04) appendFaultLabel(statuses, "TS cool");
    if (status4 & 0x02) appendFaultLabel(statuses, "TS warm");
    if (status4 & 0x01) appendFaultLabel(statuses, "TS hot");

    return statuses.isEmpty() ? String("None") : statuses;
}

String chargerFlagStatusToString(uint8_t flag0, uint8_t flag1, uint8_t flag2, uint8_t flag3) {
    String flags;

    if (flag0 & 0x80) appendFaultLabel(flags, "IINDPM/IOTG edge");
    if (flag0 & 0x40) appendFaultLabel(flags, "VINDPM/VOTG edge");
    if (flag0 & 0x20) appendFaultLabel(flags, "Watchdog expired");
    if (flag0 & 0x10) appendFaultLabel(flags, "Poor source edge");
    if (flag0 & 0x08) appendFaultLabel(flags, "PG changed");
    if (flag0 & 0x04) appendFaultLabel(flags, "VAC2 present changed");
    if (flag0 & 0x02) appendFaultLabel(flags, "VAC1 present changed");
    if (flag0 & 0x01) appendFaultLabel(flags, "VBUS present changed");

    if (flag1 & 0x80) appendFaultLabel(flags, "Charge state changed");
    if (flag1 & 0x40) appendFaultLabel(flags, "ICO changed");
    if (flag1 & 0x10) appendFaultLabel(flags, "VBUS state changed");
    if (flag1 & 0x04) appendFaultLabel(flags, "Thermal regulation edge");
    if (flag1 & 0x02) appendFaultLabel(flags, "VBAT present changed");
    if (flag1 & 0x01) appendFaultLabel(flags, "BC1.2 done");

    if (flag2 & 0x40) appendFaultLabel(flags, "DPDM done");
    if (flag2 & 0x20) appendFaultLabel(flags, "ADC done");
    if (flag2 & 0x10) appendFaultLabel(flags, "VSYSMIN enter/exit");
    if (flag2 & 0x08) appendFaultLabel(flags, "Fast-charge timer edge");
    if (flag2 & 0x04) appendFaultLabel(flags, "Trickle timer edge");
    if (flag2 & 0x02) appendFaultLabel(flags, "Pre-charge timer edge");
    if (flag2 & 0x01) appendFaultLabel(flags, "Top-off timer edge");

    if (flag3 & 0x10) appendFaultLabel(flags, "VBAT too low for OTG edge");
    if (flag3 & 0x08) appendFaultLabel(flags, "TS cold edge");
    if (flag3 & 0x04) appendFaultLabel(flags, "TS cool edge");
    if (flag3 & 0x02) appendFaultLabel(flags, "TS warm edge");
    if (flag3 & 0x01) appendFaultLabel(flags, "TS hot edge");

    return flags.isEmpty() ? String("None") : flags;
}

String chargerFaultFlagToString(uint8_t fault0, uint8_t fault1) {
    String faults;

    if (fault0 & 0x80) appendFaultLabel(faults, "IBAT regulation edge");
    if (fault0 & 0x40) appendFaultLabel(faults, "VBUS OVP edge");
    if (fault0 & 0x20) appendFaultLabel(faults, "VBAT OVP edge");
    if (fault0 & 0x10) appendFaultLabel(faults, "IBUS OCP edge");
    if (fault0 & 0x08) appendFaultLabel(faults, "IBAT OCP edge");
    if (fault0 & 0x04) appendFaultLabel(faults, "Converter OCP edge");
    if (fault0 & 0x02) appendFaultLabel(faults, "VAC2 OVP edge");
    if (fault0 & 0x01) appendFaultLabel(faults, "VAC1 OVP edge");

    if (fault1 & 0x80) appendFaultLabel(faults, "VSYS short edge");
    if (fault1 & 0x40) appendFaultLabel(faults, "VSYS OVP edge");
    if (fault1 & 0x20) appendFaultLabel(faults, "OTG OVP edge");
    if (fault1 & 0x10) appendFaultLabel(faults, "OTG UVP edge");
    if (fault1 & 0x04) appendFaultLabel(faults, "Thermal shutdown edge");

    return faults.isEmpty() ? String("None") : faults;
}

String gaugeSafetyStatusToString(uint32_t status) {
    String faults;

    if (status & (1UL << 27)) appendFaultLabel(faults, "UTD");
    if (status & (1UL << 26)) appendFaultLabel(faults, "UTC");
    if (status & (1UL << 20)) appendFaultLabel(faults, "CTO");
    if (status & (1UL << 18)) appendFaultLabel(faults, "PTO");
    if (status & (1UL << 13)) appendFaultLabel(faults, "OTD");
    if (status & (1UL << 12)) appendFaultLabel(faults, "OTC");
    if (status & (1UL << 10)) appendFaultLabel(faults, "ASCD");
    if (status & (1UL << 8)) appendFaultLabel(faults, "ASCC");
    if (status & (1UL << 6)) appendFaultLabel(faults, "AOLD");
    if (status & (1UL << 4)) appendFaultLabel(faults, "OCD");
    if (status & (1UL << 2)) appendFaultLabel(faults, "OCC");
    if (status & (1UL << 1)) appendFaultLabel(faults, "COV");
    if (status & (1UL << 0)) appendFaultLabel(faults, "CUV");

    return faults.isEmpty() ? String("None") : faults;
}

String gaugeGaugingStatusToString(uint32_t status) {
    String flags;

    if (status & (1UL << 17)) appendFaultLabel(flags, "QMax updated");
    if (status & (1UL << 16)) appendFaultLabel(flags, "VDQ");
    if (status & (1UL << 12)) appendFaultLabel(flags, "QEN");
    if (status & (1UL << 11)) appendFaultLabel(flags, "VOK");
    if (status & (1UL << 10)) appendFaultLabel(flags, "RDIS");
    if (status & (1UL << 9)) appendFaultLabel(flags, "REST");
    if (status & (1UL << 2)) appendFaultLabel(flags, "TC");
    if (status & (1UL << 1)) appendFaultLabel(flags, "FC");
    if (status & (1UL << 0)) appendFaultLabel(flags, "FD");

    return flags.isEmpty() ? String("None") : flags;
}
}

// Static member initialization
BatteryManager* BatteryManager::instance = nullptr;

BatteryManager::BatteryManager() 
    : bufferIndex(0)
    , bufferFilled(false)
    , lastUpdate(0)
    , currentVoltage(0.0)
    , currentPercentage(0.0)
    , currentTemperatureCelsius(0.0)
    , currentSystemVoltage(0.0)
    , currentVbusVoltage(0.0)
    , currentBatteryCurrentMilliAmps(0)
    , currentRemainingCapacityMilliAmpHours(0)
    , currentFullChargeCapacityMilliAmpHours(0)
    , gaugeDesignCapacityMilliAmpHours(0)
    , isCharging(false)
    , isChargingPrevious(false)
    , telemetryValid(false)
    , busInitialized(false)
    , rt6160Present(false)
    , chargerPresent(false)
    , gaugePresent(false)
    , rt6160Configured(false)
    , chargerConfigured(false)
    , chargerAdcEnabled(false)
    , chargerEnablePinAsserted(false)
    , gaugeConfigured(false)
    , gaugeSealed(false)
    , gaugeAddress(BQ28Z610_PRIMARY_ADDRESS)
    , rt6160Vout1Value(0)
    , rt6160Vout2Value(0)
    , chargerStatus0(0)
    , chargerStatus1(0)
    , chargerStatus2(0)
    , chargerStatus3(0)
    , chargerStatus4(0)
    , chargerFaultStatus0(0)
    , chargerFaultStatus1(0)
    , chargerFlag0(0)
    , chargerFlag1(0)
    , chargerFlag2(0)
    , chargerFlag3(0)
    , chargerFaultFlag0(0)
    , chargerFaultFlag1(0)
    , chargerFlagHistory0(0)
    , chargerFlagHistory1(0)
    , chargerFlagHistory2(0)
    , chargerFlagHistory3(0)
    , chargerFaultFlagHistory0(0)
    , chargerFaultFlagHistory1(0)
    , chargerRechargeControlRegister(0)
    , chargerMinimumSystemVoltageRegister(0)
    , chargerChargeVoltageRegister(0)
    , chargerChargeCurrentRegister(0)
    , chargerInputCurrentRegister(0)
    , chargerControl0Register(0)
    , chargerControl1Register(0)
    , chargerNtcControl1Register(0)
    , chargerAdcControlRegister(0)
    , gaugeSafetyStatus(0)
    , gaugeGaugingStatus(0)
    , gaugeOperationStatus(0)
    , gaugeManufacturingStatus(0)
    , gaugeDaConfiguration(0)
    , gaugeCell1VoltageMillivolts(0)
    , gaugeCell2VoltageMillivolts(0)
    , gaugeBatVoltageMillivolts(0)
    , gaugePackVoltageMillivolts(0)
    , gaugeChemId(0)
    , gaugeQMaxCell1MilliAmpHours(0)
    , gaugeQMaxCell2MilliAmpHours(0)
    , gaugeQMaxPackMilliAmpHours(0)
    , gaugeTrueRemainingCapacityMilliAmpHours(0)
    , gaugeTrueFullChargeCapacityMilliAmpHours(0)
    , gaugeUpdateStatus(0)
    , lastDeviceScan(0)
    , lowBatteryCallback(nullptr)
    , chargingStateChangeCallback(nullptr)
    , lowBatteryCallbackTriggered(false)
{
    // Initialize voltage buffer
    for (int i = 0; i < SMOOTHING_SAMPLES; i++) {
        voltageBuffer[i] = 0.0;
    }
}

BatteryManager& BatteryManager::getInstance() {
    if (instance == nullptr) {
        instance = new BatteryManager();
    }
    return *instance;
}

void BatteryManager::begin() {
    Serial.println("Initializing Battery Manager...");

    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(BATTERY_CHARGER_ENABLE_PIN));
    assertChargerEnablePin();

    releaseSDA();
    releaseSCL();
    busInitialized = true;

    refreshDevicePresence();
    applyPowerConfiguration(false);

    lastUpdate = millis() - UPDATE_INTERVAL;
    refreshBatteryState();
    
    Serial.println("Battery Manager initialized successfully");
    printBatteryInfo();
}

bool BatteryManager::reconfigure(bool forceGaugeProvision) {
    return applyPowerConfiguration(forceGaugeProvision);
}

bool BatteryManager::forceGaugeProvisioning() {
    return applyPowerConfiguration(true);
}

bool BatteryManager::resetGaugeLearningState() {
    if (!gaugePresent) {
        refreshDevicePresence();
        if (!gaugePresent) {
            return false;
        }
    }

    uint16_t operationStatus = 0;
    if (!readGaugeOperationStatus(operationStatus)) {
        return false;
    }

    gaugeOperationStatus = operationStatus;
    gaugeSealed = ((operationStatus >> 8) & 0x03) == 0x03;
    const bool wasSealed = gaugeSealed;

    if (gaugeSealed && !unsealGauge()) {
        return false;
    }

    uint16_t designCapacity = 0;
    uint8_t daConfiguration = 0;
    bool ok = readGaugeDataFlashWord(BQ28Z610_DESIGN_CAPACITY_ADDRESS, designCapacity);
    ok = readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, daConfiguration) && ok;

    if (!ok || designCapacity == 0) {
        if (wasSealed) {
            sealGauge();
            gaugeSealed = true;
        }
        return false;
    }

    const uint16_t seededQmaxCell1 = designCapacity;
    const uint16_t seededQmaxCell2 = (daConfiguration & 0x01) ? designCapacity : 0;
    const uint16_t seededQmaxPack = (daConfiguration & 0x01)
        ? static_cast<uint16_t>(seededQmaxCell1 + seededQmaxCell2)
        : designCapacity;

    ok = writeGaugeDataFlashWord(BQ28Z610_QMAX_CELL_1_ADDRESS, seededQmaxCell1) && ok;
    ok = writeGaugeDataFlashWord(BQ28Z610_QMAX_CELL_2_ADDRESS, seededQmaxCell2) && ok;
    ok = writeGaugeDataFlashWord(BQ28Z610_QMAX_PACK_ADDRESS, seededQmaxPack) && ok;
    ok = writeGaugeDataFlashByte(BQ28Z610_UPDATE_STATUS_ADDRESS, DEFAULT_GAUGE_UPDATE_STATUS) && ok;
    ok = writeGaugeAltCommand(BQ28Z610_DEVICE_RESET_COMMAND) && ok;
    delay(50);

    if (ok) {
        bool refreshed = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            refreshDevicePresence();
            refreshed = updateGaugeMeasurements();
            if (refreshed) {
                break;
            }
            delay(50);
        }
        ok = refreshed && ok;
    }

    if (wasSealed) {
        ok = sealGauge() && ok;
        gaugeSealed = true;
    } else {
        gaugeSealed = false;
    }

    return ok
        && gaugeQMaxCell1MilliAmpHours == seededQmaxCell1
        && gaugeQMaxCell2MilliAmpHours == seededQmaxCell2
        && gaugeQMaxPackMilliAmpHours == seededQmaxPack
        && gaugeUpdateStatus == DEFAULT_GAUGE_UPDATE_STATUS;
}

void BatteryManager::update() {
    unsigned long currentTime = millis();
    
    // Update at specified interval
    if (currentTime - lastUpdate >= UPDATE_INTERVAL) {
        lastUpdate = currentTime;
        refreshBatteryState();
        
        // Handle callbacks
        if (lowBatteryCallback && isBatteryLow() && !lowBatteryCallbackTriggered) {
            lowBatteryCallback(currentVoltage, currentPercentage, isCharging);
            lowBatteryCallbackTriggered = true;
        } else if (!isBatteryLow()) {
            lowBatteryCallbackTriggered = false;
        }
        
        if (chargingStateChangeCallback && isCharging != isChargingPrevious) {
            chargingStateChangeCallback(currentVoltage, currentPercentage, isCharging);
        }
        
        isChargingPrevious = isCharging;
    }
}

float BatteryManager::calculateSmoothedVoltage() {
    if (!bufferFilled && bufferIndex == 0) {
        return voltageBuffer[0];  // Not enough samples yet
    }
    
    float sum = 0.0;
    int samples = bufferFilled ? SMOOTHING_SAMPLES : bufferIndex;
    
    for (int i = 0; i < samples; i++) {
        sum += voltageBuffer[i];
    }
    
    return sum / samples;
}

float BatteryManager::voltageToPercentage(float voltage) {
    // Clamp voltage to valid range
    if (voltage <= BATTERY_MIN_VOLTAGE) {
        return 0.0;
    }
    if (voltage >= BATTERY_MAX_VOLTAGE) {
        return 100.0;
    }
    
    // LiFePO4 1S has a very flat discharge curve (long ~3.2-3.3V plateau), so
    // voltage->SOC is only a coarse fallback for when the gauge RSOC is
    // unavailable. Piecewise-linear interpolation over an approximate LFP curve.
    struct CurvePoint { float voltage; float percentage; };
    static const CurvePoint curve[] = {
        {3.60f, 100.0f},
        {3.40f,  95.0f},
        {3.35f,  90.0f},
        {3.32f,  80.0f},
        {3.30f,  70.0f},
        {3.28f,  55.0f},
        {3.25f,  40.0f},
        {3.22f,  25.0f},
        {3.20f,  18.0f},
        {3.10f,  10.0f},
        {3.00f,   5.0f},
        {2.80f,   2.0f},
        {2.50f,   0.0f},
    };
    const int pointCount = sizeof(curve) / sizeof(curve[0]);

    for (int i = 0; i < pointCount - 1; i++) {
        if (voltage <= curve[i].voltage && voltage >= curve[i + 1].voltage) {
            float span = curve[i].voltage - curve[i + 1].voltage;
            float t = span > 0.0f ? (voltage - curve[i + 1].voltage) / span : 0.0f;
            float percentage = curve[i + 1].percentage + t * (curve[i].percentage - curve[i + 1].percentage);
            return constrain(percentage, 0.0f, 100.0f);
        }
    }

    return 0.0f;
}

void BatteryManager::refreshBatteryState() {
    unsigned long now = millis();

    if (now - lastDeviceScan >= DEVICE_RESCAN_INTERVAL || !rt6160Present || !chargerPresent || !gaugePresent) {
        refreshDevicePresence();
        lastDeviceScan = now;
        applyPowerConfiguration(false);
    }

    bool gaugeUpdated = updateGaugeMeasurements();
    bool chargerUpdated = updateChargerMeasurements();

    telemetryValid = gaugeUpdated || chargerUpdated;

    if (gaugeUpdated) {
        voltageBuffer[bufferIndex] = currentVoltage;
        bufferIndex = (bufferIndex + 1) % SMOOTHING_SAMPLES;
        if (!bufferFilled && bufferIndex == 0) {
            bufferFilled = true;
        }

        currentVoltage = calculateSmoothedVoltage() + CALIBRATION_OFFSET;
    } else if (chargerUpdated) {
        voltageBuffer[bufferIndex] = currentVoltage;
        bufferIndex = (bufferIndex + 1) % SMOOTHING_SAMPLES;
        if (!bufferFilled && bufferIndex == 0) {
            bufferFilled = true;
        }

        currentVoltage = calculateSmoothedVoltage();
        currentPercentage = voltageToPercentage(currentVoltage);
    } else {
        currentVoltage = 0.0f;
        currentPercentage = 0.0f;
        isCharging = false;
    }

    updateChargingStatus();
}

void BatteryManager::refreshDevicePresence() {
    rt6160Present = probeDevice(RT6160_ADDRESS);
    chargerPresent = probeDevice(BQ25792_ADDRESS);

    gaugePresent = false;
    gaugeAddress = BQ28Z610_PRIMARY_ADDRESS;
    if (probeDevice(BQ28Z610_PRIMARY_ADDRESS)) {
        gaugePresent = true;
        gaugeAddress = BQ28Z610_PRIMARY_ADDRESS;
    } else if (probeDevice(BQ28Z610_FALLBACK_ADDRESS)) {
        gaugePresent = true;
        gaugeAddress = BQ28Z610_FALLBACK_ADDRESS;
    }
}

bool BatteryManager::applyPowerConfiguration(bool forceGaugeProvision) {
    bool ok = true;

    assertChargerEnablePin();

    if (rt6160Present) {
        // RT6160 rail programming is volatile and must be applied on each boot.
        rt6160Configured = configureRt6160For4V5();
        ok = rt6160Configured && ok;
    }

    if (chargerPresent) {
        // BQ25792 charge limits and enable state are volatile and must be restored on each boot.
        chargerConfigured = configureChargerDefaults();
        chargerAdcEnabled = enableChargerAdc();
        ok = chargerConfigured && chargerAdcEnabled && ok;
    }

    // Skip all gauge data-flash work while the gauge is still in ROM mode (fallback
    // address 0x0B for ~15s after a deep-discharge reset): DF reads/writes only fail
    // and spam errors there. The periodic device rescan retries once it answers at 0x55.
    if (gaugePresent && gaugeAddress == BQ28Z610_PRIMARY_ADDRESS) {
        // BQ28Z610 data-flash provisioning is persistent, but boards for this product are always 1S
        // and may ship with FET control disabled.
        bool provisioned = ensureGaugeProvisioned(forceGaugeProvision);

        // NOTE: CUV protection thresholds are NOT written here. The Protections-class
        // data flash silently rejects writes via the working MAC method (confirmed by
        // readback), and the default CUV (trip 2500 / recovery 3000mV) is fine for LFP:
        // recovery is reachable because the charger goes to 3.6V — provided charge isn't
        // blocked. Deep-discharge recovery therefore depends on a valid FCC (a properly
        // provisioned gauge keeps charge enabled at low SOC), NOT on editing CUV.

        // Single-cell mode and FET control must be restored even when data-flash
        // provisioning has NOT fully verified (e.g. a fresh/unprovisioned gauge).
        // Otherwise the gauge leaves its CHG/DSG FETs open, isolating the cell, and
        // the device runs unbuffered off USB and browns out during network bring-up.
        bool singleCellReady = setGaugeSingleCellMode();
        bool fetControlReady = restoreGaugeFetControl();

        // Ensure CUV latches until charge (Protection Config bit1) on EVERY boot — this is
        // the only over-discharge protection that runs while the device is off/asleep, and
        // applying it here (not just in first-boot bring-up) means boards already past
        // bring-up also get it when they take this firmware. Idempotent, non-destructive.
        ensureGaugeCuvLatch();

        // Same fleet-wide pattern: let the gauge itself sleep between measurements
        // (SLEEP + IN_SYSTEM_SLEEP in DA Configuration). Idempotent, non-destructive.
        ensureGaugeSleepConfig();

        gaugeConfigured = provisioned && singleCellReady && fetControlReady;
        ok = gaugeConfigured && ok;
    }

    return ok;
}

void BatteryManager::updateChargingStatus() {
    if (!chargerPresent) {
        return;
    }

    uint8_t chargeState = (chargerStatus1 >> 5) & 0x07;
    isCharging = chargeState == 0x01 || chargeState == 0x02 || chargeState == 0x03 || chargeState == 0x04 || chargeState == 0x06;
}

String BatteryManager::getBatteryStatusString() const {
    if (!telemetryValid) {
        return String("Battery: unavailable");
    }

    String status = "Battery: " + String(currentPercentage, 1) + "% (" + String(currentVoltage, 2) + "V)";
    
    if (isCharging) {
        status += " [CHARGING]";
    } else if (isBatteryCritical()) {
        status += " [CRITICAL]";
    } else if (isBatteryLow()) {
        status += " [LOW]";
    } else if (currentPercentage >= 95.0) {
        status += " [FULL]";
    }
    
    return status;
}

void BatteryManager::printProtectionConfig() {
    if (!gaugePresent) {
        refreshDevicePresence();
        if (!gaugePresent) {
            Serial.println("Gauge not present — cannot read protection config.");
            return;
        }
    }
    uint8_t protCfg = 0, enaA = 0, enaB = 0, cuvDelay = 0;
    uint16_t cuvThr = 0, cuvRec = 0;
    bool ok = true;
    ok &= readGaugeDataFlashByte(BQ28Z610_PROTECTION_CONFIG_ADDRESS, protCfg);
    ok &= readGaugeDataFlashByte(BQ28Z610_ENABLED_PROTECTIONS_A_ADDRESS, enaA);
    ok &= readGaugeDataFlashByte(BQ28Z610_ENABLED_PROTECTIONS_B_ADDRESS, enaB);
    ok &= readGaugeDataFlashWord(BQ28Z610_CUV_THRESHOLD_ADDRESS, cuvThr);
    ok &= readGaugeDataFlashByte(BQ28Z610_CUV_DELAY_ADDRESS, cuvDelay);
    ok &= readGaugeDataFlashWord(BQ28Z610_CUV_RECOVERY_ADDRESS, cuvRec);

    Serial.println("\n--- Gauge Protection Config (read-back) ---");
    if (!ok) {
        Serial.println("WARNING: one or more DF reads failed; values may be unreliable.");
    }
    Serial.printf("Protection Configuration (0x46AE): 0x%02X  [CUV_RECOV_CHG(bit1)=%d -> %s]\n",
                  protCfg, (protCfg >> 1) & 1,
                  ((protCfg >> 1) & 1) ? "latch until charge" : "auto-recover (no charge needed)");
    Serial.printf("Enabled Protections A    (0x46AF): 0x%02X  [CUV=%d COV=%d OCC=%d OCD1=%d]\n",
                  enaA, enaA & 1, (enaA >> 1) & 1, (enaA >> 2) & 1, (enaA >> 4) & 1);
    Serial.printf("Enabled Protections B    (0x46B0): 0x%02X\n", enaB);
    Serial.printf("CUV Threshold (0x46B3): %u mV   Delay (0x46B5): %u s   Recovery (0x46B6): %u mV\n",
                  cuvThr, cuvDelay, cuvRec);
    Serial.println("Defaults expected: ProtCfg=0x03, EnabledA=0x57, CUV 2500/2/3000.");
    Serial.printf("Live: SafetyStatus=0x%X  OperationStatus=0x%X (XDSG=%d XCHG=%d)  MfgStatus=0x%X (FET_EN=%d)\n",
                  gaugeSafetyStatus, gaugeOperationStatus,
                  (gaugeOperationStatus & 0x2000) ? 1 : 0, (gaugeOperationStatus & 0x4000) ? 1 : 0,
                  gaugeManufacturingStatus, (gaugeManufacturingStatus >> 4) & 1);
    Serial.println("-------------------------------------------");
}

void BatteryManager::printBatteryInfo() const {
    Serial.println("\n--- Battery Information ---");
    Serial.println("Battery bus pins: SDA=" + String(BATTERY_I2C_SDA_PIN) + ", SCL=" + String(BATTERY_I2C_SCL_PIN));
    Serial.println("BQ25792 CE via GPIO44 forced low: " + String(chargerEnablePinAsserted ? "Yes" : "No"));
    Serial.println("RT6160 present: " + String(rt6160Present ? "Yes" : "No"));
    Serial.println("BQ25792 present: " + String(chargerPresent ? "Yes" : "No"));
    Serial.println("BQ28Z610 present: " + String(gaugePresent ? "Yes" : "No"));
    Serial.println("RT6160 configured for 4.5V: " + String(rt6160Configured ? "Yes" : "No"));
    Serial.println("BQ25792 configured: " + String(chargerConfigured ? "Yes" : "No"));
    Serial.println("BQ28Z610 configured: " + String(gaugeConfigured ? "Yes" : "No"));
    Serial.println("BQ28Z610 sealed: " + String(gaugeSealed ? "Yes" : "No"));
    Serial.println("BQ28Z610 active address: 0x" + String(gaugeAddress, HEX));
    Serial.println("RT6160 VOUT1/VOUT2: 0x" + String(rt6160Vout1Value, HEX) + " / 0x" + String(rt6160Vout2Value, HEX));
    Serial.println("Gauge Chem ID: 0x" + String(gaugeChemId, HEX));
    Serial.println("Gauge OperationStatus: 0x" + String(gaugeOperationStatus, HEX));
    Serial.println("Gauge SafetyStatus: 0x" + String(static_cast<unsigned long>(gaugeSafetyStatus), HEX)
        + " (" + gaugeSafetyStatusToString(gaugeSafetyStatus) + ")");
    Serial.println("Gauge GaugingStatus: 0x" + String(static_cast<unsigned long>(gaugeGaugingStatus), HEX)
        + " (" + gaugeGaugingStatusToString(gaugeGaugingStatus) + ")");
    Serial.println("Gauge ManufacturingStatus: 0x" + String(gaugeManufacturingStatus, HEX));
    Serial.println("Gauge DA Configuration: 0x" + String(gaugeDaConfiguration, HEX)
        + " (" + String((gaugeDaConfiguration & 0x01) ? "2S" : "1S") + ")");
    Serial.println("Gauge QMax Cell1/Cell2/Pack: " + String(gaugeQMaxCell1MilliAmpHours) + " / "
        + String(gaugeQMaxCell2MilliAmpHours) + " / " + String(gaugeQMaxPackMilliAmpHours) + "mAh");
    Serial.println("Gauge IT True Rem/FCC: " + String(gaugeTrueRemainingCapacityMilliAmpHours) + " / "
        + String(gaugeTrueFullChargeCapacityMilliAmpHours) + "mAh");
    Serial.println("Gauge Update Status: 0x" + String(gaugeUpdateStatus, HEX));

    if (!telemetryValid) {
        Serial.println("Telemetry: unavailable");
        Serial.println("---------------------------\n");
        return;
    }

    Serial.println("Voltage: " + String(currentVoltage, 3) + "V");
    Serial.println("Percentage: " + String(currentPercentage, 1) + "%");
    Serial.println("Temperature: " + String(currentTemperatureCelsius, 1) + "C");
    Serial.println("Battery Current: " + String(currentBatteryCurrentMilliAmps) + "mA");
    Serial.println("Remaining Capacity: " + String(currentRemainingCapacityMilliAmpHours) + "mAh");
    Serial.println("Full Charge Capacity: " + String(currentFullChargeCapacityMilliAmpHours) + "mAh");
    Serial.println("Gauge Design Capacity: " + String(gaugeDesignCapacityMilliAmpHours) + "mAh");
    Serial.println("Gauge Cell1/Cell2: " + String(gaugeCell1VoltageMillivolts) + "mV / " + String(gaugeCell2VoltageMillivolts) + "mV");
    Serial.println("Gauge BAT/PACK: " + String(gaugeBatVoltageMillivolts) + "mV / " + String(gaugePackVoltageMillivolts) + "mV");
    Serial.println("Gauge XCHG/XDSG: " + String((gaugeOperationStatus & 0x4000) ? "Yes" : "No")
        + " / " + String((gaugeOperationStatus & 0x2000) ? "Yes" : "No"));
    Serial.println("Gauge FET Control Enabled: " + String((gaugeManufacturingStatus & 0x0010) ? "Yes" : "No"));
    Serial.println("Gauge CHG/DSG FET Status: " + String((gaugeOperationStatus & 0x0004) ? "On" : "Off")
        + " / " + String((gaugeOperationStatus & 0x0002) ? "On" : "Off"));
    {
        // ChargingStatus (MAC 0x0055) tells us WHY charge is enabled/disabled.
        uint16_t chargingStatus = 0;
        if (const_cast<BatteryManager *>(this)->readGaugeAltStatus16(0x0055, chargingStatus)) {
            String flags;
            if (chargingStatus & 0x8000) flags += "VCT ";   // valid charge termination (full)
            if (chargingStatus & 0x4000) flags += "MCHG ";  // maintenance charge
            if (chargingStatus & 0x2000) flags += "SU ";    // suspend
            if (chargingStatus & 0x1000) flags += "IN ";    // charge inhibit
            if (chargingStatus & 0x0800) flags += "HV ";
            if (chargingStatus & 0x0400) flags += "MV ";
            if (chargingStatus & 0x0200) flags += "LV ";
            if (chargingStatus & 0x0100) flags += "PV ";
            if (chargingStatus & 0x0040) flags += "OT ";
            if (chargingStatus & 0x0020) flags += "HT ";
            if (chargingStatus & 0x0010) flags += "STH ";
            if (chargingStatus & 0x0008) flags += "RT ";
            if (chargingStatus & 0x0004) flags += "STL ";
            if (chargingStatus & 0x0002) flags += "LT ";
            if (chargingStatus & 0x0001) flags += "UT ";
            Serial.println("Gauge ChargingStatus: 0x" + String(chargingStatus, HEX) + " (" + flags + ")");
        }
    }
    Serial.println("System Voltage: " + String(currentSystemVoltage, 3) + "V");
    Serial.println("VBUS Voltage: " + String(currentVbusVoltage, 3) + "V");
    Serial.println("Charging: " + String(isCharging ? "Yes" : "No"));
    Serial.println("Charger Config REG00/01/03/06: 0x" + String(chargerMinimumSystemVoltageRegister, HEX)
        + " / 0x" + String(chargerChargeVoltageRegister, HEX)
        + " / 0x" + String(chargerChargeCurrentRegister, HEX)
        + " / 0x" + String(chargerInputCurrentRegister, HEX));
    Serial.println("Charger Config REG0A: 0x" + String(chargerRechargeControlRegister, HEX));
    Serial.println("Charger Config REG0F/10/18/2E: 0x" + String(chargerControl0Register, HEX)
        + " / 0x" + String(chargerControl1Register, HEX)
        + " / 0x" + String(chargerNtcControl1Register, HEX)
        + " / 0x" + String(chargerAdcControlRegister, HEX));
    Serial.println("Charger Status0/Status1: 0x" + String(chargerStatus0, HEX) + " / 0x" + String(chargerStatus1, HEX));
    Serial.println("Charger Status2/Status3: 0x" + String(chargerStatus2, HEX) + " / 0x" + String(chargerStatus3, HEX));
    Serial.println("Charger Status4: 0x" + String(chargerStatus4, HEX));
    Serial.println("Charger Fault0/Fault1: 0x" + String(chargerFaultStatus0, HEX) + " / 0x" + String(chargerFaultStatus1, HEX));
    Serial.println("Charger Flag0/1/2/3: 0x" + String(chargerFlag0, HEX)
        + " / 0x" + String(chargerFlag1, HEX)
        + " / 0x" + String(chargerFlag2, HEX)
        + " / 0x" + String(chargerFlag3, HEX));
    Serial.println("Charger FaultFlag0/1: 0x" + String(chargerFaultFlag0, HEX)
        + " / 0x" + String(chargerFaultFlag1, HEX));
    Serial.println("Charge State: " + String(chargerStateToString((chargerStatus1 >> 5) & 0x07)));
    Serial.println("VBAT Present: " + String((chargerStatus2 & 0x01) ? "Yes" : "No"));
    Serial.println("VSYSMIN Regulation: " + String((chargerStatus3 & 0x10) ? "Yes" : "No"));
    Serial.println("Thermal Regulation: " + String((chargerStatus2 & 0x04) ? "Yes" : "No"));
    Serial.println("Charge Suspend Status: " + chargerTimerStatusToString(chargerStatus3));
    Serial.println("TS Status: " + chargerTsStatusToString(chargerStatus4));
    Serial.println("Active Charger Faults: " + chargerFaultStatusToString(chargerFaultStatus0, chargerFaultStatus1));
    Serial.println("Latched Charger Flags: " + chargerFlagStatusToString(
        chargerFlagHistory0,
        chargerFlagHistory1,
        chargerFlagHistory2,
        chargerFlagHistory3));
    Serial.println("Latched Charger Faults: " + chargerFaultFlagToString(
        chargerFaultFlagHistory0,
        chargerFaultFlagHistory1));
    String statusStr;
    if (isBatteryCritical()) {
        statusStr = "Critical";
    } else if (isBatteryLow()) {
        statusStr = "Low";
    } else if (currentPercentage >= 95.0) {
        statusStr = "Full";
    } else {
        statusStr = "Normal";
    }
    Serial.println("Status: " + statusStr);
    Serial.println("---------------------------\n");
}

void BatteryManager::printBusScan() {
    constexpr uint8_t kCh224aqAddress0 = 0x22;
    constexpr uint8_t kCh224aqAddress1 = 0x23;

    Serial.println("\n--- Battery I2C Bus Scan ---");

    bool foundAny = false;
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
        if (!probeDevice(address)) {
            continue;
        }

        foundAny = true;
        String label = "Unknown";
        if (address == RT6160_ADDRESS) {
            label = "RT6160";
        } else if (address == BQ25792_ADDRESS) {
            label = "BQ25792";
        } else if (address == BQ28Z610_PRIMARY_ADDRESS || address == BQ28Z610_FALLBACK_ADDRESS) {
            label = "BQ28Z610";
        } else if (address == kCh224aqAddress0 || address == kCh224aqAddress1) {
            label = "CH224AQ";
        }

        Serial.printf("  0x%02X ACK (%s)\n", address, label.c_str());
    }

    if (!foundAny) {
        Serial.println("  No I2C devices ACKed on the battery bus.");
    }

    Serial.println("Expected bring-up addresses:");
    Serial.printf("  RT6160 expected at 0x%02X\n", RT6160_ADDRESS);
    Serial.printf("  BQ25792 expected at 0x%02X\n", BQ25792_ADDRESS);
    Serial.printf("  BQ28Z610 usually expected at 0x%02X\n", BQ28Z610_PRIMARY_ADDRESS);
    Serial.printf("  BQ28Z610 fallback candidate at 0x%02X\n", BQ28Z610_FALLBACK_ADDRESS);
    Serial.printf("  CH224AQ may appear at 0x%02X or 0x%02X when USB-C is present\n", kCh224aqAddress0, kCh224aqAddress1);
    Serial.println("-----------------------------\n");
}

void BatteryManager::calibrate(float actualVoltage) {
    float measuredVoltage = currentVoltage - CALIBRATION_OFFSET;
    float newOffset = actualVoltage - measuredVoltage;
    
    Serial.println("Battery calibration:");
    Serial.println("Measured: " + String(measuredVoltage, 3) + "V");
    Serial.println("Actual: " + String(actualVoltage, 3) + "V");
    Serial.println("New offset: " + String(newOffset, 3) + "V");
    
    // You would need to store this offset in NVS for persistence
    // For now, it's just used for this session
}

void BatteryManager::resetCalibration() {
    // Reset calibration offset
    Serial.println("Battery calibration reset");
}

void BatteryManager::setLowBatteryCallback(BatteryEventCallback callback) {
    lowBatteryCallback = callback;
}

void BatteryManager::setChargingStateChangeCallback(BatteryEventCallback callback) {
    chargingStateChangeCallback = callback;
}

bool BatteryManager::configureRt6160For4V5() {
    bool wroteVout1 = writeRegister8(RT6160_ADDRESS, RT6160_VOUT1_REGISTER, RT6160_4V5_SETTING);
    bool wroteVout2 = writeRegister8(RT6160_ADDRESS, RT6160_VOUT2_REGISTER, RT6160_4V5_SETTING);

    bool readVout1 = readRegister8(RT6160_ADDRESS, RT6160_VOUT1_REGISTER, rt6160Vout1Value);
    bool readVout2 = readRegister8(RT6160_ADDRESS, RT6160_VOUT2_REGISTER, rt6160Vout2Value);

    return wroteVout1 && wroteVout2 && readVout1 && readVout2
        && rt6160Vout1Value == RT6160_4V5_SETTING
        && rt6160Vout2Value == RT6160_4V5_SETTING;
}

bool BatteryManager::configureChargerDefaults() {
    const uint8_t minimumSystemVoltageSetting = static_cast<uint8_t>((DEFAULT_MIN_SYSTEM_VOLTAGE_MV - 2500) / 250);

    uint8_t rechargeControl = 0;
    uint8_t ntcControl1 = 0;
    bool ok = readRegister8(BQ25792_ADDRESS, BQ25792_RECHARGE_CONTROL_REGISTER, rechargeControl);
    rechargeControl &= static_cast<uint8_t>(~0xC0); // CELL=1s

    ok = writeRegister8(BQ25792_ADDRESS, BQ25792_RECHARGE_CONTROL_REGISTER, rechargeControl) && ok;
    ok = writeRegister8(BQ25792_ADDRESS, BQ25792_MIN_SYSTEM_VOLTAGE_REGISTER, minimumSystemVoltageSetting) && ok;
    ok = writeRegister16BE(BQ25792_ADDRESS, BQ25792_CHARGE_VOLTAGE_REGISTER, DEFAULT_CHARGE_VOLTAGE_MV / 10) && ok;
    const uint16_t chargeCurrentMa =
        gRequestedChargeCurrentMa != 0 ? gRequestedChargeCurrentMa : DEFAULT_CHARGE_CURRENT_MA;
    ok = writeRegister16BE(BQ25792_ADDRESS, BQ25792_CHARGE_CURRENT_REGISTER, chargeCurrentMa / 10) && ok;
    const uint16_t inputLimitMa =
        gRequestedInputLimitMa != 0 ? gRequestedInputLimitMa : DEFAULT_INPUT_CURRENT_LIMIT_MA;
    ok = writeRegister16BE(BQ25792_ADDRESS, BQ25792_INPUT_CURRENT_REGISTER, inputLimitMa / 10) && ok;

    uint8_t control0 = 0;
    uint8_t control1 = 0;

    // Explicit input-voltage floor for 5V sources (VINDPM 4.4V; higher-voltage sources
    // are protected by IINDPM/ICO). The POR default is 3.6V, which lets a sagging 5V
    // source be dragged far too low before the input loop backs off.
    ok = writeRegister8(BQ25792_ADDRESS, BQ25792_INPUT_VOLTAGE_REGISTER, 4400 / 100) && ok;

    // The charger's own D+/D- pins are UNCONNECTED on this board (the CH224Q owns the
    // USB data lines), so its automatic input detection can only misclassify sources —
    // it was seeding legacy ~1.5A limits under a valid PD 3A contract. Disable
    // FORCE_INDET (bit7) and AUTO_INDET_EN (bit6); preserve the SDRV ship-control bits.
    uint8_t control2 = 0;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_2_REGISTER, control2) && ok;
    control2 = static_cast<uint8_t>(control2 & ~0xC0);
    ok = writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_2_REGISTER, control2) && ok;

    // Release the ILIM_HIZ pin clamp (EN_EXTILIM, REG14 bit1, default 1): the R185
    // 100Ω strap hardware-clamps IINDPM to ~1.56A and silently rewrites every host
    // value above it — this was the constant REG06=0x9c across all sources/sessions.
    // With it released, the input limit is governed by the IINDPM register (3A default
    // / PD-declared budget), protected by VINDPM 4.4V and ICO on non-PD sources.
    uint8_t control5 = 0;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_5_REGISTER, control5) && ok;
    control5 = static_cast<uint8_t>(control5 & ~0x02);
    ok = writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_5_REGISTER, control5) && ok;

    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0) && ok;
    // EN_CHG and EN_ICO both follow their sticky inhibits so a temperature suspension,
    // the recovery hold's charge pause, or a PD-declared input budget survives this
    // periodic reconfiguration.
    if (gIcoInhibited) {
        control0 = static_cast<uint8_t>(control0 & ~kBq25792IcoEnableMask);
    } else {
        control0 |= kBq25792IcoEnableMask;
    }
    if (gChargeInhibited) {
        control0 = static_cast<uint8_t>(control0 & ~kBq25792ChargeEnableMask);
    } else {
        control0 |= kBq25792ChargeEnableMask;
    }
    ok = writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0) && ok;

    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_1_REGISTER, control1) && ok;
    control1 &= static_cast<uint8_t>(~kBq25792WatchdogMask);
    ok = writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_1_REGISTER, control1) && ok;

    ok = readRegister8(BQ25792_ADDRESS, BQ25792_NTC_CONTROL_1_REGISTER, ntcControl1) && ok;
    ntcControl1 |= kBq25792TsIgnoreMask;
    ok = writeRegister8(BQ25792_ADDRESS, BQ25792_NTC_CONTROL_1_REGISTER, ntcControl1) && ok;

    return ok;
}

bool BatteryManager::setHizMode(bool enable) {
    if (!chargerPresent) {
        refreshDevicePresence();
        if (!chargerPresent) {
            return false;
        }
    }

    uint8_t control0 = 0;
    if (!readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0)) {
        return false;
    }

    const uint8_t hizMask = 0x04; // REG0F bit2 = EN_HIZ
    if (enable) {
        control0 |= hizMask;
    } else {
        control0 &= static_cast<uint8_t>(~hizMask);
    }

    if (!writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0)) {
        return false;
    }

    uint8_t verify = 0;
    if (!readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, verify)) {
        return false;
    }
    chargerControl0Register = verify;
    return ((verify & hizMask) != 0) == enable;
}

bool BatteryManager::setChargeCurrent(uint16_t milliAmps) {
    if (!chargerPresent) {
        refreshDevicePresence();
        if (!chargerPresent) {
            return false;
        }
    }
    // REG03 charge current limit, 10mA/LSB, big-endian.
    if (!writeRegister16BE(BQ25792_ADDRESS, BQ25792_CHARGE_CURRENT_REGISTER, milliAmps / 10)) {
        return false;
    }
    uint16_t verify = 0;
    if (!readRegister16BE(BQ25792_ADDRESS, BQ25792_CHARGE_CURRENT_REGISTER, verify)) {
        return false;
    }
    chargerChargeCurrentRegister = verify;
    // Keep the periodic charger reconfiguration from silently reverting this limit.
    gRequestedChargeCurrentMa = milliAmps;
    return true;
}

bool BatteryManager::isGaugeProvisioned() {
    if (!gaugePresent) {
        refreshDevicePresence();
        if (!gaugePresent) {
            return false;
        }
    }
    uint16_t chem = 0, designCap = 0;
    uint8_t da = 0;
    if (!readGaugeDataFlashWord(BQ28Z610_CHEM_ID_ADDRESS, chem)) return false;
    if (!readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, da)) return false;
    if (!readGaugeDataFlashWord(BQ28Z610_DESIGN_CAPACITY_ADDRESS, designCap)) return false;

    const bool chemOk = (DEFAULT_GAUGE_CHEM_ID == 0) || (chem == DEFAULT_GAUGE_CHEM_ID);
    const bool singleCell = (da & 0x01) == 0;                 // bit0 set => 2S
    const bool capOk = designCap >= 1000 && designCap <= 8000; // sane LFP design capacity
    return chemOk && singleCell && capOk;
}

bool BatteryManager::isCurrentSenseCalibrated() {
    if (!gaugePresent) {
        refreshDevicePresence();
        if (!gaugePresent) {
            return false;
        }
    }
    float ccGain = 0.0f;
    if (!readGaugeDataFlashFloat(BQ28Z610_CC_GAIN_ADDRESS, ccGain)) {
        return false;
    }
    // Calibration moves CC Gain well away from the factory default; >2% deviation = calibrated.
    return fabsf(ccGain - BQ28Z610_DEFAULT_CC_GAIN) > (BQ28Z610_DEFAULT_CC_GAIN * 0.02f);
}

bool BatteryManager::isGaugeAlive() {
    // Re-probe: the gauge transitions from ROM mode (0x0B) to its normal address as it boots.
    refreshDevicePresence();
    return gaugePresent && gaugeAddress == BQ28Z610_PRIMARY_ADDRESS;
}

bool BatteryManager::gaugeAliveAndProvisioned() {
    return isGaugeAlive() && isGaugeProvisioned();
}

bool BatteryManager::ensureGaugeCuvLatch() {
    if (!gaugePresent) {
        refreshDevicePresence();
        if (!gaugePresent) {
            return false;
        }
    }
    uint8_t protCfg = 0;
    if (!readGaugeDataFlashByte(BQ28Z610_PROTECTION_CONFIG_ADDRESS, protCfg)) {
        Serial.println("ensureGaugeCuvLatch: failed to read Protection Configuration");
        return false;
    }
    if (protCfg & 0x02) {
        return true; // CUV_RECOV_CHG already set -> CUV latches until charge
    }
    const uint8_t desired = protCfg | 0x02; // set bit1, leave others
    writeGaugeDataFlashByte(BQ28Z610_PROTECTION_CONFIG_ADDRESS, desired);
    uint8_t verify = 0;
    readGaugeDataFlashByte(BQ28Z610_PROTECTION_CONFIG_ADDRESS, verify);
    const bool stuck = (verify & 0x02) != 0;
    Serial.printf("ensureGaugeCuvLatch: Protection Config 0x%02X -> wrote 0x%02X, read 0x%02X -> CUV latch %s\n",
                  protCfg, desired, verify,
                  stuck ? "ENABLED" : "REJECTED (relying on firmware low-battery cutoff)");
    return stuck;
}

bool BatteryManager::isVbusPresent() const {
    return chargerPresent && (chargerStatus0 & 0x01) != 0; // REG1B bit0 VBUS_PRESENT_STAT
}

bool BatteryManager::enterShipMode() {
    if (!chargerPresent) {
        refreshDevicePresence();
        if (!chargerPresent) {
            return false;
        }
    }
    uint8_t control2 = 0;
    if (!readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_2_REGISTER, control2)) {
        return false;
    }
    // SDRV_CTRL[2:1] = 10b (ship mode: BATFET off, cell disconnected from SYS, charger
    // I2C stays alive at ~µA). SDRV_DLY (bit0) = 1 -> act immediately, no 10s delay.
    // Exits on USB plug-in. On battery power this call does not return in any useful
    // sense: SYS collapses as soon as the BATFET opens.
    control2 = static_cast<uint8_t>((control2 & ~0x07) | (0x02 << 1) | 0x01);
    Serial.println("SHIP MODE: disconnecting cell from SYS - plug USB to wake.");
    Serial.flush();
    delay(50);
    return writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_2_REGISTER, control2);
}

bool BatteryManager::ensureGaugeSleepConfig() {
    if (!gaugePresent) {
        refreshDevicePresence();
        if (!gaugePresent) {
            return false;
        }
    }
    uint8_t da = 0;
    if (!readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, da)) {
        return false;
    }
    // SLEEP (bit4) + IN_SYSTEM_SLEEP (bit3), and keep CC0 (bit0) at 1S. Without
    // IN_SYSTEM_SLEEP the gauge only sleeps when the I2C bus is held LOW — ours idles
    // HIGH on pullups, so the gauge sat in NORMAL mode (~0.4mA from the cell) forever.
    const uint8_t desired = static_cast<uint8_t>((da | 0x18) & ~0x01);
    if (da == desired) {
        return true;
    }
    writeGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, desired);
    uint8_t verify = 0;
    readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, verify);
    const bool stuck = verify == desired;
    Serial.printf("ensureGaugeSleepConfig: DA Configuration 0x%02X -> wrote 0x%02X, read 0x%02X -> IN_SYSTEM_SLEEP %s\n",
                  da, desired, verify, stuck ? "ENABLED" : "REJECTED");
    if (stuck) {
        gaugeDaConfiguration = verify;
    }
    return stuck;
}

bool BatteryManager::isHizMode() {
    uint8_t control0 = 0;
    if (!readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0)) {
        return false;
    }
    chargerControl0Register = control0;
    return (control0 & 0x04) != 0;
}

bool BatteryManager::enableChargerAdc() {
    uint8_t adcControl = 0;
    if (!readRegister8(BQ25792_ADDRESS, BQ25792_ADC_CONTROL_REGISTER, adcControl)) {
        return false;
    }

    adcControl |= kBq25792AdcEnableMask | kBq25792AdcOneShotMask;
    if (!writeRegister8(BQ25792_ADDRESS, BQ25792_ADC_CONTROL_REGISTER, adcControl)) {
        return false;
    }

    uint8_t verify = 0;
    if (!readRegister8(BQ25792_ADDRESS, BQ25792_ADC_CONTROL_REGISTER, verify)) {
        return false;
    }

    return (verify & (kBq25792AdcEnableMask | kBq25792AdcOneShotMask))
        == (kBq25792AdcEnableMask | kBq25792AdcOneShotMask);
}

bool BatteryManager::ensureGaugeProvisioned(bool force) {
    uint16_t operationStatus = 0;
    if (!readGaugeOperationStatus(operationStatus)) {
        return false;
    }

    gaugeOperationStatus = operationStatus;
    gaugeSealed = ((operationStatus >> 8) & 0x03) == 0x03;
    bool wasSealed = gaugeSealed;

    if (gaugeSealed && !unsealGauge()) {
        return false;
    }

    uint16_t designCapacity = 0;
    uint16_t designEnergy = 0;
    uint8_t fetOptions = 0;
    uint8_t i2cGaugingConfiguration = 0;
    uint8_t i2cConfiguration = 0;
    uint8_t powerConfig = 0;
    uint16_t socFlagConfigA = 0;
    uint8_t socFlagConfigB = 0;
    uint16_t qmaxCell1 = 0;
    uint16_t qmaxCell2 = 0;
    uint16_t qmaxPack = 0;
    uint8_t updateStatus = 0;
    uint16_t itGaugingConfiguration = 0;
    uint8_t chargingConfiguration = 0;
    uint8_t temperatureEnable = 0;
    uint16_t terminateVoltage = 0;
    uint16_t chargingVoltage = 0;
    uint16_t taperCurrent = 0;
    uint16_t dsgThreshold = 0;
    uint16_t chgThreshold = 0;
    uint16_t quitCurrent = 0;
    uint8_t daConfiguration = 0;
    uint8_t balancingConfiguration = 0;

    bool ok = readGaugeDataFlashWord(BQ28Z610_CHEM_ID_ADDRESS, gaugeChemId);
    ok = readGaugeDataFlashByte(BQ28Z610_FET_OPTIONS_ADDRESS, fetOptions) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_I2C_GAUGING_CONFIGURATION_ADDRESS, i2cGaugingConfiguration) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_I2C_CONFIGURATION_ADDRESS, i2cConfiguration) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_POWER_CONFIG_ADDRESS, powerConfig) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_SOC_FLAG_CONFIG_A_ADDRESS, socFlagConfigA) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_SOC_FLAG_CONFIG_B_ADDRESS, socFlagConfigB) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_QMAX_CELL_1_ADDRESS, qmaxCell1) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_QMAX_CELL_2_ADDRESS, qmaxCell2) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_QMAX_PACK_ADDRESS, qmaxPack) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_UPDATE_STATUS_ADDRESS, updateStatus) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_IT_GAUGING_CONFIGURATION_ADDRESS, itGaugingConfiguration) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_CHARGING_CONFIGURATION_ADDRESS, chargingConfiguration) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_TEMPERATURE_ENABLE_ADDRESS, temperatureEnable) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_DESIGN_CAPACITY_ADDRESS, designCapacity) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_DESIGN_ENERGY_ADDRESS, designEnergy) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_TERMINATE_VOLTAGE_ADDRESS, terminateVoltage) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_CHARGING_VOLTAGE_MED_ADDRESS, chargingVoltage) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_TAPER_CURRENT_ADDRESS, taperCurrent) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_DSG_CURRENT_THRESHOLD_ADDRESS, dsgThreshold) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_CHG_CURRENT_THRESHOLD_ADDRESS, chgThreshold) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_QUIT_CURRENT_ADDRESS, quitCurrent) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, daConfiguration) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_BALANCING_CONFIGURATION_ADDRESS, balancingConfiguration) && ok;

    if (!ok) {
        return false;
    }

    const bool looksUninitialized = designCapacity == 0
        || designEnergy == 0
        || terminateVoltage == 0
        || chargingVoltage == 0
        || taperCurrent == 0
        || quitCurrent == 0;
    const bool looksCorrupted = gaugeConfigurationLooksCorrupted(designCapacity,
                                                                 designEnergy,
                                                                 terminateVoltage,
                                                                 chargingVoltage,
                                                                 taperCurrent);

    // Production packs can legitimately use non-generic gauge data, so only
    // auto-provision when the data flash looks blank or clearly invalid. Use
    // `gaugeprog` to force the generic bring-up values when needed.
    //
    // Learned-state fields like QMax and Update Status are intentionally left
    // alone so the gauge can relax and learn naturally during test cycles.
    bool needsWrite = force || looksUninitialized || looksCorrupted;

    const bool needsSingleCellFix = (daConfiguration & 0x01) != 0;

    if (needsWrite) {
        ok = writeGaugeDataFlashByte(BQ28Z610_FET_OPTIONS_ADDRESS, DEFAULT_GAUGE_FET_OPTIONS) && ok;
        ok = writeGaugeDataFlashByte(BQ28Z610_I2C_GAUGING_CONFIGURATION_ADDRESS, DEFAULT_GAUGE_I2C_GAUGING_CONFIGURATION) && ok;
        ok = writeGaugeDataFlashByte(BQ28Z610_I2C_CONFIGURATION_ADDRESS, DEFAULT_GAUGE_I2C_CONFIGURATION) && ok;
        ok = writeGaugeDataFlashByte(BQ28Z610_POWER_CONFIG_ADDRESS, DEFAULT_GAUGE_POWER_CONFIG) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_SOC_FLAG_CONFIG_A_ADDRESS, DEFAULT_GAUGE_SOC_FLAG_CONFIG_A) && ok;
        ok = writeGaugeDataFlashByte(BQ28Z610_SOC_FLAG_CONFIG_B_ADDRESS, DEFAULT_GAUGE_SOC_FLAG_CONFIG_B) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_IT_GAUGING_CONFIGURATION_ADDRESS, DEFAULT_GAUGE_IT_GAUGING_CONFIGURATION) && ok;
        ok = writeGaugeDataFlashByte(BQ28Z610_CHARGING_CONFIGURATION_ADDRESS, DEFAULT_GAUGE_CHARGING_CONFIGURATION) && ok;
        ok = writeGaugeDataFlashByte(BQ28Z610_TEMPERATURE_ENABLE_ADDRESS, DEFAULT_GAUGE_TEMPERATURE_ENABLE) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_DESIGN_CAPACITY_ADDRESS, DEFAULT_GAUGE_DESIGN_CAPACITY_MAH) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_DESIGN_ENERGY_ADDRESS, DEFAULT_GAUGE_DESIGN_ENERGY_MWH) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_TERMINATE_VOLTAGE_ADDRESS, DEFAULT_GAUGE_TERMINATE_VOLTAGE_MV) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_CHARGING_VOLTAGE_LOW_ADDRESS, DEFAULT_GAUGE_CHARGING_VOLTAGE_MV) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_CHARGING_VOLTAGE_MED_ADDRESS, DEFAULT_GAUGE_CHARGING_VOLTAGE_MV) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_CHARGING_VOLTAGE_HIGH_ADDRESS, DEFAULT_GAUGE_CHARGING_VOLTAGE_MV) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_TAPER_CURRENT_ADDRESS, DEFAULT_GAUGE_TAPER_CURRENT_MA) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_DSG_CURRENT_THRESHOLD_ADDRESS, DEFAULT_GAUGE_DSG_CURRENT_THRESHOLD_MA) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_CHG_CURRENT_THRESHOLD_ADDRESS, DEFAULT_GAUGE_CHG_CURRENT_THRESHOLD_MA) && ok;
        ok = writeGaugeDataFlashWord(BQ28Z610_QUIT_CURRENT_ADDRESS, DEFAULT_GAUGE_QUIT_CURRENT_MA) && ok;
        ok = writeGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, DEFAULT_GAUGE_DA_CONFIGURATION) && ok;
        ok = writeGaugeDataFlashByte(BQ28Z610_BALANCING_CONFIGURATION_ADDRESS, DEFAULT_GAUGE_BALANCING_CONFIGURATION) && ok;

        if (DEFAULT_GAUGE_CHEM_ID != 0) {
            ok = writeGaugeDataFlashWord(BQ28Z610_CHEM_ID_ADDRESS, DEFAULT_GAUGE_CHEM_ID) && ok;
        }

        if (!ok) {
            return false;
        }

        ok = writeGaugeAltCommand(BQ28Z610_DEVICE_RESET_COMMAND) && ok;
        delay(10);

        ok = readGaugeDataFlashWord(BQ28Z610_CHEM_ID_ADDRESS, gaugeChemId) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_FET_OPTIONS_ADDRESS, fetOptions) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_I2C_GAUGING_CONFIGURATION_ADDRESS, i2cGaugingConfiguration) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_I2C_CONFIGURATION_ADDRESS, i2cConfiguration) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_POWER_CONFIG_ADDRESS, powerConfig) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_SOC_FLAG_CONFIG_A_ADDRESS, socFlagConfigA) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_SOC_FLAG_CONFIG_B_ADDRESS, socFlagConfigB) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_QMAX_CELL_1_ADDRESS, qmaxCell1) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_QMAX_CELL_2_ADDRESS, qmaxCell2) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_QMAX_PACK_ADDRESS, qmaxPack) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_UPDATE_STATUS_ADDRESS, updateStatus) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_IT_GAUGING_CONFIGURATION_ADDRESS, itGaugingConfiguration) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_CHARGING_CONFIGURATION_ADDRESS, chargingConfiguration) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_TEMPERATURE_ENABLE_ADDRESS, temperatureEnable) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_DESIGN_CAPACITY_ADDRESS, designCapacity) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_DESIGN_ENERGY_ADDRESS, designEnergy) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_TERMINATE_VOLTAGE_ADDRESS, terminateVoltage) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_CHARGING_VOLTAGE_MED_ADDRESS, chargingVoltage) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_TAPER_CURRENT_ADDRESS, taperCurrent) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_DSG_CURRENT_THRESHOLD_ADDRESS, dsgThreshold) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_CHG_CURRENT_THRESHOLD_ADDRESS, chgThreshold) && ok;
        ok = readGaugeDataFlashWord(BQ28Z610_QUIT_CURRENT_ADDRESS, quitCurrent) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, daConfiguration) && ok;
        ok = readGaugeDataFlashByte(BQ28Z610_BALANCING_CONFIGURATION_ADDRESS, balancingConfiguration) && ok;

        needsWrite = false;
        needsWrite = fetOptions != DEFAULT_GAUGE_FET_OPTIONS || needsWrite;
        needsWrite = i2cGaugingConfiguration != DEFAULT_GAUGE_I2C_GAUGING_CONFIGURATION || needsWrite;
        needsWrite = i2cConfiguration != DEFAULT_GAUGE_I2C_CONFIGURATION || needsWrite;
        needsWrite = powerConfig != DEFAULT_GAUGE_POWER_CONFIG || needsWrite;
        needsWrite = socFlagConfigA != DEFAULT_GAUGE_SOC_FLAG_CONFIG_A || needsWrite;
        needsWrite = socFlagConfigB != DEFAULT_GAUGE_SOC_FLAG_CONFIG_B || needsWrite;
        needsWrite = itGaugingConfiguration != DEFAULT_GAUGE_IT_GAUGING_CONFIGURATION || needsWrite;
        needsWrite = chargingConfiguration != DEFAULT_GAUGE_CHARGING_CONFIGURATION || needsWrite;
        needsWrite = temperatureEnable != DEFAULT_GAUGE_TEMPERATURE_ENABLE || needsWrite;
        needsWrite = designCapacity != DEFAULT_GAUGE_DESIGN_CAPACITY_MAH || needsWrite;
        needsWrite = designEnergy != DEFAULT_GAUGE_DESIGN_ENERGY_MWH || needsWrite;
        needsWrite = terminateVoltage != DEFAULT_GAUGE_TERMINATE_VOLTAGE_MV || needsWrite;
        needsWrite = chargingVoltage != DEFAULT_GAUGE_CHARGING_VOLTAGE_MV || needsWrite;
        needsWrite = taperCurrent != DEFAULT_GAUGE_TAPER_CURRENT_MA || needsWrite;
        needsWrite = dsgThreshold != DEFAULT_GAUGE_DSG_CURRENT_THRESHOLD_MA || needsWrite;
        needsWrite = chgThreshold != DEFAULT_GAUGE_CHG_CURRENT_THRESHOLD_MA || needsWrite;
        needsWrite = quitCurrent != DEFAULT_GAUGE_QUIT_CURRENT_MA || needsWrite;
        needsWrite = daConfiguration != DEFAULT_GAUGE_DA_CONFIGURATION || needsWrite;
        needsWrite = balancingConfiguration != DEFAULT_GAUGE_BALANCING_CONFIGURATION || needsWrite;
        if (DEFAULT_GAUGE_CHEM_ID != 0) {
            needsWrite = gaugeChemId != DEFAULT_GAUGE_CHEM_ID || needsWrite;
        }
    }

    // This product is always a single-cell pack. Fixing CC0 is a targeted persistent remediation and
    // should not force the rest of the gauge profile back to generic defaults.
    if (ok && needsSingleCellFix) {
        daConfiguration = static_cast<uint8_t>(daConfiguration & ~0x01);
        ok = writeGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, daConfiguration) && ok;
        ok = writeGaugeAltCommand(BQ28Z610_DEVICE_RESET_COMMAND) && ok;
        delay(50);
        ok = readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, daConfiguration) && ok;
    }

    gaugeDaConfiguration = daConfiguration;
    gaugeQMaxCell1MilliAmpHours = qmaxCell1;
    gaugeQMaxCell2MilliAmpHours = qmaxCell2;
    gaugeQMaxPackMilliAmpHours = qmaxPack;
    gaugeUpdateStatus = updateStatus;

    if (wasSealed) {
        sealGauge();
        gaugeSealed = true;
    } else {
        gaugeSealed = false;
    }

    return ok && !needsWrite;
}

bool BatteryManager::updateGaugeMeasurements() {
    if (!gaugePresent) {
        return false;
    }

    uint8_t daStatus1[12] = {0};
    uint8_t itStatus1[24] = {0};
    uint32_t safetyStatus = 0;
    uint32_t gaugingStatus = 0;
    uint16_t manufacturingStatus = 0;
    uint8_t daConfiguration = 0;
    uint16_t voltageMillivolts = 0;
    uint16_t relativeStateOfCharge = 0;
    uint16_t temperatureDeciKelvin = 0;
    uint16_t remainingCapacity = 0;
    uint16_t fullChargeCapacity = 0;
    uint16_t designCapacity = 0;
    uint16_t qmaxPack = 0;
    uint8_t updateStatus = 0;
    int16_t batteryCurrent = 0;
    int16_t trueRemainingCapacity = 0;
    uint16_t trueFullChargeCapacity = 0;

    bool ok = readGaugeOperationStatus(gaugeOperationStatus);
    ok = readGaugeAltStatus32(BQ28Z610_SAFETY_STATUS_COMMAND, safetyStatus) && ok;
    ok = readGaugeAltStatus32(BQ28Z610_GAUGING_STATUS_COMMAND, gaugingStatus) && ok;
    ok = readGaugeAltStatus16(BQ28Z610_MANUFACTURING_STATUS_COMMAND, manufacturingStatus) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, daConfiguration) && ok;
    ok = readGaugeAltBlock(BQ28Z610_DA_STATUS_1_COMMAND, daStatus1, sizeof(daStatus1)) && ok;
    ok = readGaugeAltBlock(BQ28Z610_IT_STATUS_1_COMMAND, itStatus1, sizeof(itStatus1)) && ok;
    ok = readRegister16LE(gaugeAddress, BQ28Z610_VOLTAGE_REGISTER, voltageMillivolts) && ok;
    ok = readRegister16LE(gaugeAddress, BQ28Z610_RELATIVE_SOC_REGISTER, relativeStateOfCharge) && ok;
    ok = readRegister16LE(gaugeAddress, BQ28Z610_TEMPERATURE_REGISTER, temperatureDeciKelvin) && ok;
    ok = readRegister16LESigned(gaugeAddress, BQ28Z610_CURRENT_REGISTER, batteryCurrent) && ok;
    ok = readRegister16LE(gaugeAddress, BQ28Z610_REMAINING_CAPACITY_REGISTER, remainingCapacity) && ok;
    ok = readRegister16LE(gaugeAddress, BQ28Z610_FULL_CHARGE_CAPACITY_REGISTER, fullChargeCapacity) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_DESIGN_CAPACITY_ADDRESS, designCapacity) && ok;
    ok = readGaugeDataFlashWord(BQ28Z610_QMAX_PACK_ADDRESS, qmaxPack) && ok;
    ok = readGaugeDataFlashByte(BQ28Z610_UPDATE_STATUS_ADDRESS, updateStatus) && ok;

    if (!ok) {
        return false;
    }

    currentVoltage = voltageMillivolts / 1000.0f;
    currentPercentage = constrain(relativeStateOfCharge > 100 ? (relativeStateOfCharge & 0xFF) : relativeStateOfCharge, 0, 100);
    currentTemperatureCelsius = (temperatureDeciKelvin / 10.0f) - 273.15f;
    currentBatteryCurrentMilliAmps = batteryCurrent;
    currentRemainingCapacityMilliAmpHours = remainingCapacity;
    currentFullChargeCapacityMilliAmpHours = fullChargeCapacity;
    gaugeDesignCapacityMilliAmpHours = designCapacity;
    gaugeQMaxPackMilliAmpHours = qmaxPack;
    gaugeUpdateStatus = updateStatus;
    gaugeSafetyStatus = safetyStatus;
    gaugeGaugingStatus = gaugingStatus;
    gaugeManufacturingStatus = manufacturingStatus;
    gaugeDaConfiguration = daConfiguration;
    trueRemainingCapacity = static_cast<int16_t>(static_cast<uint16_t>(itStatus1[1] << 8) | itStatus1[0]);
    trueFullChargeCapacity = static_cast<uint16_t>(itStatus1[9] << 8) | itStatus1[8];
    gaugeTrueRemainingCapacityMilliAmpHours = trueRemainingCapacity;
    gaugeTrueFullChargeCapacityMilliAmpHours = trueFullChargeCapacity;
    gaugeCell1VoltageMillivolts = static_cast<uint16_t>(daStatus1[1] << 8) | daStatus1[0];
    gaugeCell2VoltageMillivolts = static_cast<uint16_t>(daStatus1[3] << 8) | daStatus1[2];
    gaugeBatVoltageMillivolts = static_cast<uint16_t>(daStatus1[9] << 8) | daStatus1[8];
    gaugePackVoltageMillivolts = static_cast<uint16_t>(daStatus1[11] << 8) | daStatus1[10];
    return true;
}

bool BatteryManager::restoreGaugeFetControl() {
    if (!gaugePresent) {
        return false;
    }

    bool wasSealed = gaugeSealed;
    if (gaugeSealed && !unsealGauge()) {
        return false;
    }

    bool ok = readGaugeAltStatus16(BQ28Z610_MANUFACTURING_STATUS_COMMAND, gaugeManufacturingStatus);
    if (ok && (gaugeManufacturingStatus & 0x0008) == 0) {
        ok = writeGaugeAltCommand(BQ28Z610_GAUGING_COMMAND) && ok;
        delay(10);
    }
    if (ok && (gaugeManufacturingStatus & 0x0010) == 0) {
        ok = writeGaugeAltCommand(BQ28Z610_FET_CONTROL_COMMAND) && ok;
        delay(10);
    }

    bool refreshed = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint16_t operationStatus = 0;
        uint32_t safetyStatus = 0;
        uint16_t manufacturingStatus = 0;
        uint8_t daConfiguration = 0;

        refreshed = readGaugeOperationStatus(operationStatus)
            && readGaugeAltStatus32(BQ28Z610_SAFETY_STATUS_COMMAND, safetyStatus)
            && readGaugeAltStatus16(BQ28Z610_MANUFACTURING_STATUS_COMMAND, manufacturingStatus)
            && readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, daConfiguration);
        if (refreshed) {
            gaugeOperationStatus = operationStatus;
            gaugeSafetyStatus = safetyStatus;
            gaugeManufacturingStatus = manufacturingStatus;
            gaugeDaConfiguration = daConfiguration;
            break;
        }
        delay(10);
    }
    ok = refreshed && ok;

    uint8_t daStatus1[12] = {0};
    bool voltagesRead = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        voltagesRead = readGaugeAltBlock(BQ28Z610_DA_STATUS_1_COMMAND, daStatus1, sizeof(daStatus1));
        if (voltagesRead) {
            break;
        }
        delay(10);
    }
    ok = voltagesRead && ok;
    if (ok) {
        gaugeCell1VoltageMillivolts = static_cast<uint16_t>(daStatus1[1] << 8) | daStatus1[0];
        gaugeCell2VoltageMillivolts = static_cast<uint16_t>(daStatus1[3] << 8) | daStatus1[2];
        gaugeBatVoltageMillivolts = static_cast<uint16_t>(daStatus1[9] << 8) | daStatus1[8];
        gaugePackVoltageMillivolts = static_cast<uint16_t>(daStatus1[11] << 8) | daStatus1[10];
    }

    ok = ((gaugeManufacturingStatus & 0x0018) == 0x0018) && ok;

    if (wasSealed) {
        ok = sealGauge() && ok;
    }

    return ok;
}

bool BatteryManager::setGaugeSingleCellMode() {
    if (!gaugePresent) {
        return false;
    }

    bool wasSealed = gaugeSealed;
    if (gaugeSealed && !unsealGauge()) {
        return false;
    }

    uint8_t daConfiguration = 0;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        ok = readGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, daConfiguration);
        if (!ok) {
            delay(10);
        }
    }
    if (ok && (daConfiguration & 0x01) == 0) {
        gaugeDaConfiguration = daConfiguration;
        return updateGaugeMeasurements();
    }

    if (ok && (daConfiguration & 0x01) != 0) {
        // Factory default is 2-cell (CC0=1); switch to 1-cell for this 1S pack.
        daConfiguration &= static_cast<uint8_t>(~0x01);
        bool wrote = writeGaugeDataFlashByte(BQ28Z610_DA_CONFIGURATION_ADDRESS, daConfiguration);
        Serial.printf("setGaugeSingleCellMode: switching gauge to 1S -> %s\n",
                      wrote ? "OK" : "FAILED");
        ok = wrote && ok;
        if (ok) {
            delay(10);
            // Reset so the gauge reinitializes in 1S and re-evaluates the CUV fault.
            ok = writeGaugeAltCommand(BQ28Z610_DEVICE_RESET_COMMAND) && ok;
            delay(50);
        }
    }

    if (ok) {
        bool refreshed = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            refreshDevicePresence();
            refreshed = updateGaugeMeasurements();
            if (refreshed) {
                break;
            }
            delay(50);
        }
        ok = refreshed && ok;
    }

    if (wasSealed) {
        ok = sealGauge() && ok;
    }

    return ok && ((gaugeDaConfiguration & 0x01) == 0);
}

bool BatteryManager::setIcoEnabled(bool enable) {
    if (!chargerPresent) {
        refreshDevicePresence();
        if (!chargerPresent) {
            return false;
        }
    }
    uint8_t control0 = 0;
    if (!readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0)) {
        return false;
    }
    if (enable) {
        control0 |= kBq25792IcoEnableMask;
    } else {
        control0 = static_cast<uint8_t>(control0 & ~kBq25792IcoEnableMask);
    }
    if (!writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0)) {
        return false;
    }
    gIcoInhibited = !enable; // keep the periodic reconfiguration from undoing this
    return true;
}

bool BatteryManager::setInputCurrentLimit(uint16_t milliAmps) {
    if (!chargerPresent) {
        refreshDevicePresence();
        if (!chargerPresent) {
            return false;
        }
    }
    // 0 = clear the override and restore the default (ICO then adapts to the source).
    const uint16_t target = milliAmps != 0 ? milliAmps : DEFAULT_INPUT_CURRENT_LIMIT_MA;
    if (!writeRegister16BE(BQ25792_ADDRESS, BQ25792_INPUT_CURRENT_REGISTER, target / 10)) {
        return false;
    }
    chargerInputCurrentRegister = target / 10;
    gRequestedInputLimitMa = milliAmps; // keep the periodic reconfig from reverting it
    return true;
}

bool BatteryManager::readUsbPdStatus(uint8_t &status, uint16_t &grantedMilliAmps) {
    // The CH224Q is VBUS-powered; try both possible addresses (batch-dependent).
    const uint8_t addresses[2] = {CH224Q_ADDRESS_A, CH224Q_ADDRESS_B};
    for (uint8_t address : addresses) {
        uint8_t raw = 0;
        if (readRegister8(address, CH224Q_STATUS_REGISTER, status) &&
            readRegister8(address, CH224Q_CURRENT_REGISTER, raw)) {
            grantedMilliAmps = static_cast<uint16_t>(raw) * 50U; // 50mA/LSB, PD only
            return true;
        }
    }
    return false;
}

bool BatteryManager::setUsbPdVoltageGear(uint8_t gear) {
    const uint8_t addresses[2] = {CH224Q_ADDRESS_A, CH224Q_ADDRESS_B};
    for (uint8_t address : addresses) {
        // Probe with the (readable) status register first so the write targets the
        // address that actually ACKs.
        uint8_t probe = 0;
        if (readRegister8(address, CH224Q_STATUS_REGISTER, probe)) {
            return writeRegister8(address, CH224Q_VOLTAGE_CONTROL_REGISTER, gear);
        }
    }
    return false;
}

bool BatteryManager::setChargeEnabled(bool enable) {
    if (!chargerPresent) {
        return false;
    }
    uint8_t control0 = 0;
    if (!readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0)) {
        return false;
    }
    if (enable) {
        control0 |= kBq25792ChargeEnableMask;
    } else {
        control0 = static_cast<uint8_t>(control0 & ~kBq25792ChargeEnableMask);
    }
    if (!writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0)) {
        return false;
    }
    gChargeInhibited = !enable; // keep the periodic reconfiguration from undoing this
    return true;
}

bool BatteryManager::restartChargeCycle() {
    if (!chargerPresent) {
        return false;
    }

    uint8_t control0 = 0;
    if (!readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, control0)) {
        return false;
    }

    uint8_t disabled = static_cast<uint8_t>(control0 & ~kBq25792ChargeEnableMask);
    if (!writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, disabled)) {
        return false;
    }

    delay(20);

    uint8_t enabled = static_cast<uint8_t>(disabled | kBq25792ChargeEnableMask);
    if (!writeRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, enabled)) {
        return false;
    }

    delay(50);

    bool ok = updateChargerMeasurements();
    if (gaugePresent) {
        ok = updateGaugeMeasurements() && ok;
    }
    updateChargingStatus();
    return ok;
}

bool BatteryManager::clearChargerFaultHistory() {
    if (!chargerPresent) {
        return false;
    }

    uint8_t ignored = 0;
    bool ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_FLAG_0_REGISTER, ignored);
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_FLAG_1_REGISTER, ignored) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_FLAG_2_REGISTER, ignored) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_FLAG_3_REGISTER, ignored) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_FAULT_FLAG_0_REGISTER, ignored) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_FAULT_FLAG_1_REGISTER, ignored) && ok;

    chargerFlag0 = 0;
    chargerFlag1 = 0;
    chargerFlag2 = 0;
    chargerFlag3 = 0;
    chargerFaultFlag0 = 0;
    chargerFaultFlag1 = 0;
    chargerFlagHistory0 = 0;
    chargerFlagHistory1 = 0;
    chargerFlagHistory2 = 0;
    chargerFlagHistory3 = 0;
    chargerFaultFlagHistory0 = 0;
    chargerFaultFlagHistory1 = 0;
    return ok;
}

bool BatteryManager::updateChargerMeasurements() {
    if (!chargerPresent) {
        return false;
    }

    bool ok = readRegister8(BQ25792_ADDRESS, BQ25792_STATUS_0_REGISTER, chargerStatus0);
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_STATUS_1_REGISTER, chargerStatus1) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_STATUS_2_REGISTER, chargerStatus2) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_STATUS_3_REGISTER, chargerStatus3) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_STATUS_4_REGISTER, chargerStatus4) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_FAULT_STATUS_0_REGISTER, chargerFaultStatus0) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_FAULT_STATUS_1_REGISTER, chargerFaultStatus1) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_FLAG_0_REGISTER, chargerFlag0) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_FLAG_1_REGISTER, chargerFlag1) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_FLAG_2_REGISTER, chargerFlag2) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_FLAG_3_REGISTER, chargerFlag3) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_FAULT_FLAG_0_REGISTER, chargerFaultFlag0) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_FAULT_FLAG_1_REGISTER, chargerFaultFlag1) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_RECHARGE_CONTROL_REGISTER, chargerRechargeControlRegister) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_MIN_SYSTEM_VOLTAGE_REGISTER, chargerMinimumSystemVoltageRegister) && ok;
    ok = readRegister16BE(BQ25792_ADDRESS, BQ25792_CHARGE_VOLTAGE_REGISTER, chargerChargeVoltageRegister) && ok;
    ok = readRegister16BE(BQ25792_ADDRESS, BQ25792_CHARGE_CURRENT_REGISTER, chargerChargeCurrentRegister) && ok;
    ok = readRegister16BE(BQ25792_ADDRESS, BQ25792_INPUT_CURRENT_REGISTER, chargerInputCurrentRegister) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_0_REGISTER, chargerControl0Register) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_CHARGER_CONTROL_1_REGISTER, chargerControl1Register) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_NTC_CONTROL_1_REGISTER, chargerNtcControl1Register) && ok;
    ok = readRegister8(BQ25792_ADDRESS, BQ25792_ADC_CONTROL_REGISTER, chargerAdcControlRegister) && ok;

    uint16_t vbatMillivolts = 0;
    uint16_t vsysMillivolts = 0;
    uint16_t vbusMillivolts = 0;
    uint16_t ibatMilliamps = 0;

    if (chargerAdcEnabled) {
        ok = readRegister16BE(BQ25792_ADDRESS, BQ25792_VBAT_ADC_REGISTER, vbatMillivolts) && ok;
        ok = readRegister16BE(BQ25792_ADDRESS, BQ25792_VSYS_ADC_REGISTER, vsysMillivolts) && ok;
        ok = readRegister16BE(BQ25792_ADDRESS, BQ25792_VBUS_ADC_REGISTER, vbusMillivolts) && ok;
        ok = readRegister16BESigned(BQ25792_ADDRESS, BQ25792_IBAT_ADC_REGISTER, reinterpret_cast<int16_t&>(ibatMilliamps)) && ok;
    }

    if (!ok) {
        return false;
    }

    chargerFlagHistory0 |= chargerFlag0;
    chargerFlagHistory1 |= chargerFlag1;
    chargerFlagHistory2 |= chargerFlag2;
    chargerFlagHistory3 |= chargerFlag3;
    chargerFaultFlagHistory0 |= chargerFaultFlag0;
    chargerFaultFlagHistory1 |= chargerFaultFlag1;

    if (!gaugePresent && vbatMillivolts > 0) {
        currentVoltage = vbatMillivolts / 1000.0f;
    }

    currentSystemVoltage = vsysMillivolts / 1000.0f;
    currentVbusVoltage = vbusMillivolts / 1000.0f;
    if (!gaugePresent) {
        currentBatteryCurrentMilliAmps = static_cast<int>(ibatMilliamps);
    }

    return true;
}

void BatteryManager::assertChargerEnablePin() {
    const gpio_num_t chargerEnablePin = static_cast<gpio_num_t>(BATTERY_CHARGER_ENABLE_PIN);
    gpio_config_t configuration = {};
    configuration.pin_bit_mask = 1ULL << BATTERY_CHARGER_ENABLE_PIN;
    configuration.mode = GPIO_MODE_OUTPUT;
    configuration.pull_up_en = GPIO_PULLUP_DISABLE;
    configuration.pull_down_en = GPIO_PULLDOWN_ENABLE;
    configuration.intr_type = GPIO_INTR_DISABLE;

    gpio_config(&configuration);
    gpio_set_drive_capability(chargerEnablePin, GPIO_DRIVE_CAP_3);
    gpio_set_level(chargerEnablePin, 0);
    chargerEnablePinAsserted = true;
}

void BatteryManager::prepareForDeepSleep() {
    const gpio_num_t chargerEnablePin = static_cast<gpio_num_t>(BATTERY_CHARGER_ENABLE_PIN);
    assertChargerEnablePin();
    // GPIO44 is a digital-only pad: this hold keeps CE low through the sleep transition
    // but is released once deep sleep actually starts. The global gpio_deep_sleep_hold_en()
    // that used to arm it is intentionally NOT called — it freezes the flash/octal-PSRAM
    // pads too, defeating the sleep leakage workarounds and burning ~40mA all night (the
    // 5-day depletion culprit). Until the board gets a pulldown on BATT_CE, the CE line
    // floats during deep sleep (datasheet says don't float it — bodge a 100k CE->GND so
    // charging stays default-enabled with the ESP asleep).
    gpio_hold_en(chargerEnablePin);
}

void BatteryManager::releaseSDA() {
    pinMode(BATTERY_I2C_SDA_PIN, INPUT_PULLUP);
}

void BatteryManager::releaseSCL() {
    pinMode(BATTERY_I2C_SCL_PIN, INPUT_PULLUP);
}

void BatteryManager::driveSDALow() {
    pinMode(BATTERY_I2C_SDA_PIN, OUTPUT);
    digitalWrite(BATTERY_I2C_SDA_PIN, LOW);
}

void BatteryManager::driveSCLLow() {
    pinMode(BATTERY_I2C_SCL_PIN, OUTPUT);
    digitalWrite(BATTERY_I2C_SCL_PIN, LOW);
}

void BatteryManager::i2cDelay() {
    delayMicroseconds(I2C_DELAY_US);
}

bool BatteryManager::waitForSCLHigh() {
    unsigned long start = micros();
    while (digitalRead(BATTERY_I2C_SCL_PIN) == LOW) {
        if (micros() - start >= CLOCK_STRETCH_TIMEOUT_US) {
            return false;
        }
    }
    return true;
}

void BatteryManager::i2cStart() {
    releaseSDA();
    releaseSCL();
    waitForSCLHigh();
    i2cDelay();
    driveSDALow();
    i2cDelay();
    driveSCLLow();
}

void BatteryManager::i2cStop() {
    driveSDALow();
    i2cDelay();
    releaseSCL();
    waitForSCLHigh();
    i2cDelay();
    releaseSDA();
    i2cDelay();
}

bool BatteryManager::i2cWriteByte(uint8_t value) {
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
        if (value & mask) {
            releaseSDA();
        } else {
            driveSDALow();
        }

        i2cDelay();
        releaseSCL();
        if (!waitForSCLHigh()) {
            return false;
        }
        i2cDelay();
        driveSCLLow();
    }

    releaseSDA();
    i2cDelay();
    releaseSCL();
    if (!waitForSCLHigh()) {
        return false;
    }

    bool acknowledged = digitalRead(BATTERY_I2C_SDA_PIN) == LOW;
    i2cDelay();
    driveSCLLow();
    return acknowledged;
}

uint8_t BatteryManager::i2cReadByte(bool ack) {
    uint8_t value = 0;
    releaseSDA();

    for (int bit = 0; bit < 8; bit++) {
        value <<= 1;
        releaseSCL();
        waitForSCLHigh();
        i2cDelay();
        if (digitalRead(BATTERY_I2C_SDA_PIN)) {
            value |= 0x01;
        }
        driveSCLLow();
        i2cDelay();
    }

    if (ack) {
        driveSDALow();
    } else {
        releaseSDA();
    }

    i2cDelay();
    releaseSCL();
    waitForSCLHigh();
    i2cDelay();
    driveSCLLow();
    releaseSDA();
    return value;
}

bool BatteryManager::probeDevice(uint8_t address) {
    if (!busInitialized) {
        return false;
    }

    i2cStart();
    bool ok = i2cWriteByte(static_cast<uint8_t>(address << 1));
    i2cStop();
    return ok;
}

bool BatteryManager::writeRegisters(uint8_t address, uint8_t startReg, const uint8_t *data, size_t length) {
    if (!busInitialized) {
        return false;
    }

    i2cStart();
    bool ok = i2cWriteByte(static_cast<uint8_t>(address << 1));
    ok = i2cWriteByte(startReg) && ok;
    for (size_t index = 0; index < length; ++index) {
        ok = i2cWriteByte(data[index]) && ok;
    }
    i2cStop();
    return ok;
}

bool BatteryManager::readRegisters(uint8_t address, uint8_t startReg, uint8_t *data, size_t length) {
    if (!busInitialized) {
        return false;
    }

    i2cStart();
    bool ok = i2cWriteByte(static_cast<uint8_t>(address << 1));
    ok = i2cWriteByte(startReg) && ok;
    i2cStart();
    ok = i2cWriteByte(static_cast<uint8_t>((address << 1) | 0x01)) && ok;
    if (ok) {
        for (size_t index = 0; index < length; ++index) {
            data[index] = i2cReadByte(index + 1 < length);
        }
    }
    i2cStop();
    return ok;
}

bool BatteryManager::writeRegister8(uint8_t address, uint8_t reg, uint8_t value) {
    return writeRegisters(address, reg, &value, 1);
}

bool BatteryManager::writeRegister16LE(uint8_t address, uint8_t reg, uint16_t value) {
    uint8_t buffer[2] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
    };

    return writeRegisters(address, reg, buffer, sizeof(buffer));
}

bool BatteryManager::writeRegister16BE(uint8_t address, uint8_t reg, uint16_t value) {
    uint8_t buffer[2] = {
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
    };

    return writeRegisters(address, reg, buffer, sizeof(buffer));
}

bool BatteryManager::readRegister8(uint8_t address, uint8_t reg, uint8_t &value) {
    return readRegisters(address, reg, &value, 1);
}

bool BatteryManager::readRegister16LE(uint8_t address, uint8_t reg, uint16_t &value) {
    if (!busInitialized) {
        return false;
    }

    i2cStart();
    bool ok = i2cWriteByte(static_cast<uint8_t>(address << 1));
    ok = i2cWriteByte(reg) && ok;
    i2cStart();
    ok = i2cWriteByte(static_cast<uint8_t>((address << 1) | 0x01)) && ok;
    if (ok) {
        uint8_t low = i2cReadByte(true);
        uint8_t high = i2cReadByte(false);
        value = static_cast<uint16_t>(high << 8) | low;
    }
    i2cStop();
    return ok;
}

bool BatteryManager::readRegister16LESigned(uint8_t address, uint8_t reg, int16_t &value) {
    uint16_t raw = 0;
    if (!readRegister16LE(address, reg, raw)) {
        return false;
    }

    value = static_cast<int16_t>(raw);
    return true;
}

bool BatteryManager::readRegister16BE(uint8_t address, uint8_t reg, uint16_t &value) {
    uint8_t buffer[2] = {0};
    if (!readRegisters(address, reg, buffer, sizeof(buffer))) {
        return false;
    }

    value = static_cast<uint16_t>(buffer[0] << 8) | buffer[1];
    return true;
}

bool BatteryManager::readRegister16BESigned(uint8_t address, uint8_t reg, int16_t &value) {
    uint16_t raw = 0;
    if (!readRegister16BE(address, reg, raw)) {
        return false;
    }

    value = static_cast<int16_t>(raw);
    return true;
}

bool BatteryManager::writeGaugeAltCommand(uint16_t command) {
    uint8_t buffer[2] = {
        static_cast<uint8_t>(command & 0xFF),
        static_cast<uint8_t>((command >> 8) & 0xFF),
    };

    return writeRegisters(gaugeAddress, 0x3E, buffer, sizeof(buffer));
}

bool BatteryManager::readGaugeAltBlock(uint16_t command, uint8_t *data, size_t length) {
    if (!writeGaugeAltCommand(command)) {
        return false;
    }

    delay(2);
    return readRegisters(gaugeAddress, BQ28Z610_MAC_DATA_START_REGISTER, data, length);
}

bool BatteryManager::readGaugeAltStatus16(uint16_t command, uint16_t &value) {
    uint8_t buffer[2] = {0};
    if (!readGaugeAltBlock(command, buffer, sizeof(buffer))) {
        return false;
    }

    value = static_cast<uint16_t>(buffer[1] << 8) | buffer[0];
    return true;
}

bool BatteryManager::readGaugeAltStatus32(uint16_t command, uint32_t &value) {
    uint8_t buffer[4] = {0};
    if (!readGaugeAltBlock(command, buffer, sizeof(buffer))) {
        return false;
    }

    value = static_cast<uint32_t>(buffer[3]) << 24
        | static_cast<uint32_t>(buffer[2]) << 16
        | static_cast<uint32_t>(buffer[1]) << 8
        | static_cast<uint32_t>(buffer[0]);
    return true;
}

bool BatteryManager::readGaugeOperationStatus(uint16_t &status) {
    uint8_t buffer[4] = {0};
    if (!readGaugeAltBlock(BQ28Z610_OPERATION_STATUS_COMMAND, buffer, sizeof(buffer))) {
        return false;
    }

    status = static_cast<uint16_t>(buffer[1] << 8) | buffer[0];
    return true;
}

bool BatteryManager::readGaugeDataFlashByte(uint16_t address, uint8_t &value) {
    uint8_t target[2] = {
        static_cast<uint8_t>(address & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
    };
    uint8_t response[3] = {0};

    if (!writeRegisters(gaugeAddress, 0x3E, target, sizeof(target))) {
        return false;
    }

    delay(2);
    if (!readRegisters(gaugeAddress, 0x3E, response, sizeof(response))) {
        return false;
    }

    if (response[0] != target[0] || response[1] != target[1]) {
        return false;
    }

    value = response[2];
    return true;
}

bool BatteryManager::unsealGauge() {
    // SEALED -> UNSEALED. (Data-memory writes succeed in UNSEALED once the write
    // uses the correct single-block protocol — see writeGaugeDataFlashByte/Word.
    // Sending the full-access key here corrupted the gauge state machine, so we
    // stay in UNSEALED.)
    if (!writeGaugeAltCommand(BQ28Z610_DEFAULT_UNSEAL_KEY_1)) {
        return false;
    }
    delay(2);
    if (!writeGaugeAltCommand(BQ28Z610_DEFAULT_UNSEAL_KEY_2)) {
        return false;
    }
    delay(2);

    uint16_t status = 0;
    if (!readGaugeOperationStatus(status)) {
        return false;
    }

    gaugeOperationStatus = status;
    // SEC[1:0] in OperationStatus bits 9:8 — 0b11 sealed, 0b10 unsealed, 0b01 full access.
    gaugeSealed = ((status >> 8) & 0x03) == 0x03;
    return !gaugeSealed;
}

bool BatteryManager::sealGauge() {
    bool ok = writeGaugeAltCommand(BQ28Z610_SEAL_COMMAND);
    delay(2);
    uint16_t status = 0;
    if (ok && readGaugeOperationStatus(status)) {
        gaugeOperationStatus = status;
        gaugeSealed = ((status >> 8) & 0x03) == 0x03;
        return gaugeSealed;
    }

    return false;
}

bool BatteryManager::readGaugeDataFlashWord(uint16_t address, uint16_t &value) {
    uint8_t target[2] = {
        static_cast<uint8_t>(address & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
    };
    uint8_t response[4] = {0};

    if (!writeRegisters(gaugeAddress, 0x3E, target, sizeof(target))) {
        return false;
    }

    delay(2);
    if (!readRegisters(gaugeAddress, 0x3E, response, sizeof(response))) {
        return false;
    }

    if (response[0] != target[0] || response[1] != target[1]) {
        return false;
    }

    value = static_cast<uint16_t>(response[3] << 8) | response[2];
    return true;
}

bool BatteryManager::writeGaugeDataFlashByte(uint16_t address, uint8_t value) {
    // TI canonical data-memory write: address + data must be written as ONE
    // contiguous block to AltManufacturerAccess (0x3E). Writing the address alone
    // (with a STOP) is interpreted as a read request, so a follow-up write to
    // MACData never commits. The checksum+length word (0x60/0x61) finalizes it.
    uint8_t block[3] = {
        static_cast<uint8_t>(address & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
        value,
    };
    if (!writeRegisters(gaugeAddress, 0x3E, block, sizeof(block))) {
        return false;
    }

    uint8_t trailer[2] = {
        static_cast<uint8_t>(~(block[0] + block[1] + block[2])), // checksum
        0x05,                                                    // length: 2 addr + 1 data + 2
    };
    if (!writeRegisters(gaugeAddress, BQ28Z610_MAC_CHECKSUM_REGISTER, trailer, sizeof(trailer))) {
        return false;
    }

    delay(15); // data-flash commit time

    // Some writes (e.g. DA Configuration cell count) make the gauge reconfigure
    // immediately, so the first readback can transiently fail even though the
    // value committed. Retry the verify before declaring failure.
    uint8_t verify = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (readGaugeDataFlashByte(address, verify) && verify == value) {
            return true;
        }
        delay(10);
    }
    return false;
}

bool BatteryManager::writeGaugeDataFlashWord(uint16_t address, uint16_t value) {
    // Single contiguous block (address + data) to 0x3E, then checksum+length.
    uint8_t block[4] = {
        static_cast<uint8_t>(address & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
    };
    if (!writeRegisters(gaugeAddress, 0x3E, block, sizeof(block))) {
        return false;
    }

    uint8_t trailer[2] = {
        static_cast<uint8_t>(~(block[0] + block[1] + block[2] + block[3])), // checksum
        0x06,                                                               // length: 2 addr + 2 data + 2
    };
    if (!writeRegisters(gaugeAddress, BQ28Z610_MAC_CHECKSUM_REGISTER, trailer, sizeof(trailer))) {
        return false;
    }

    delay(15); // data-flash commit time

    uint16_t verify = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (readGaugeDataFlashWord(address, verify) && verify == value) {
            return true;
        }
        delay(10);
    }
    return false;
}

bool BatteryManager::readGaugeDataFlashFloat(uint16_t address, float &value) {
    // DF floats are IEEE-754 single precision, little-endian (datasheet 13.1.3).
    uint8_t target[2] = {
        static_cast<uint8_t>(address & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
    };
    uint8_t response[6] = {0}; // 2 addr echo + 4 data

    if (!writeRegisters(gaugeAddress, 0x3E, target, sizeof(target))) {
        return false;
    }
    delay(2);
    if (!readRegisters(gaugeAddress, 0x3E, response, sizeof(response))) {
        return false;
    }
    if (response[0] != target[0] || response[1] != target[1]) {
        return false;
    }
    uint8_t le[4] = {response[2], response[3], response[4], response[5]};
    memcpy(&value, le, sizeof(value)); // ESP32 is little-endian
    return true;
}

bool BatteryManager::writeGaugeDataFlashFloat(uint16_t address, float value) {
    uint8_t f[4];
    memcpy(f, &value, sizeof(f)); // little-endian
    uint8_t block[6] = {
        static_cast<uint8_t>(address & 0xFF),
        static_cast<uint8_t>((address >> 8) & 0xFF),
        f[0], f[1], f[2], f[3],
    };
    if (!writeRegisters(gaugeAddress, 0x3E, block, sizeof(block))) {
        return false;
    }
    uint8_t trailer[2] = {
        static_cast<uint8_t>(~(block[0] + block[1] + block[2] + block[3] + block[4] + block[5])),
        0x08, // length: 2 addr + 4 data + 2
    };
    if (!writeRegisters(gaugeAddress, BQ28Z610_MAC_CHECKSUM_REGISTER, trailer, sizeof(trailer))) {
        return false;
    }
    delay(15);

    // Relative tolerance: large gains (e.g. Capacity Gain ~2.5e5) have float spacing
    // far bigger than any fixed absolute epsilon, and the gauge may re-derive the value.
    float verify = 0.0f;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (readGaugeDataFlashFloat(address, verify) &&
            fabsf(verify - value) <= (fabsf(value) * 1e-3f + 1e-4f)) {
            return true;
        }
        delay(10);
    }
    return false;
}

bool BatteryManager::calibrateCurrentSense() {
    if (!gaugePresent || !chargerPresent) {
        refreshDevicePresence();
        if (!gaugePresent || !chargerPresent) {
            Serial.println("calibrateCurrentSense: gauge or charger not present");
            return false;
        }
    }

    // Reference: BQ25792 IBAT ADC (factory-calibrated, 1mA/LSB, 2's complement).
    uint16_t ibatRaw = 0;
    if (!readRegister16BE(BQ25792_ADDRESS, BQ25792_IBAT_ADC_REGISTER, ibatRaw)) {
        Serial.println("calibrateCurrentSense: failed to read charger IBAT");
        return false;
    }
    int chargerCurrent = static_cast<int16_t>(ibatRaw); // mA

    // Gauge reported current (uses the current, miscalibrated CC Gain).
    updateGaugeMeasurements();
    int gaugeCurrent = currentBatteryCurrentMilliAmps; // mA

    Serial.printf("calibrateCurrentSense: charger IBAT=%dmA, gauge=%dmA\n",
                  chargerCurrent, gaugeCurrent);

    // Need a meaningful, same-direction current on both for a clean ratio.
    if (abs(chargerCurrent) < 150 || abs(gaugeCurrent) < 150) {
        Serial.println("calibrateCurrentSense: need a steady current >150mA (charge at 300mA). Aborting.");
        return false;
    }
    if ((chargerCurrent > 0) != (gaugeCurrent > 0)) {
        Serial.println("calibrateCurrentSense: charger/gauge current sign mismatch. Aborting.");
        return false;
    }

    float factor = static_cast<float>(chargerCurrent) / static_cast<float>(gaugeCurrent);

    bool wasSealed = gaugeSealed;
    if (gaugeSealed && !unsealGauge()) {
        return false;
    }

    float ccGain = 0.0f;
    if (!readGaugeDataFlashFloat(BQ28Z610_CC_GAIN_ADDRESS, ccGain)) {
        Serial.println("calibrateCurrentSense: failed to read CC Gain");
        if (wasSealed) { sealGauge(); }
        return false;
    }

    float newCcGain = ccGain * factor;
    if (newCcGain < 0.1f) newCcGain = 0.1f;   // datasheet range
    if (newCcGain > 4.0f) newCcGain = 4.0f;
    float newCapacityGain = newCcGain * 298261.6178f; // datasheet 11.4.3

    Serial.printf("calibrateCurrentSense: factor=%.4f, CC Gain %.5f -> %.5f\n",
                  factor, ccGain, newCcGain);

    bool ok = writeGaugeDataFlashFloat(BQ28Z610_CC_GAIN_ADDRESS, newCcGain);
    ok = writeGaugeDataFlashFloat(BQ28Z610_CAPACITY_GAIN_ADDRESS, newCapacityGain) && ok;

    if (ok) {
        writeGaugeAltCommand(BQ28Z610_DEVICE_RESET_COMMAND);
        delay(50);
        // After reset the gauge needs a measurement cycle (~1s) to report current with
        // the new gain — wait before the sanity re-check so it isn't stale.
        for (int i = 0; i < 30; ++i) {
            updateGaugeMeasurements();
            delay(100);
        }
    }

    if (wasSealed) {
        sealGauge();
        gaugeSealed = true;
    }

    Serial.printf("calibrateCurrentSense: %s. Re-check: charger=%dmA vs gauge=%dmA\n",
                  ok ? "OK" : "FAILED", chargerCurrent, currentBatteryCurrentMilliAmps);
    return ok;
}

const char* BatteryManager::chargerStateToString(uint8_t state) {
    switch (state) {
        case 0x00:
            return "Not charging";
        case 0x01:
            return "Trickle";
        case 0x02:
            return "Pre-charge";
        case 0x03:
            return "Fast charge";
        case 0x04:
            return "Taper";
        case 0x06:
            return "Top-off";
        case 0x07:
            return "Charge done";
        default:
            return "Unknown";
    }
}