#ifndef BATTERYMANAGEMENT_H
#define BATTERYMANAGEMENT_H

#include <Arduino.h>
#include <driver/gpio.h>

class BatteryManager {
private:
    // Singleton instance
    static BatteryManager* instance;
    
    // Battery power bus on the new board is a dedicated I2C segment on GPIO1/2.
    static const int BATTERY_I2C_SDA_PIN = 1;
    static const int BATTERY_I2C_SCL_PIN = 2;
    static const int BATTERY_CHARGER_ENABLE_PIN = 44; // ESP32-S3 RXD0 wired to BQ25792 CE through 390 ohms.
    static constexpr unsigned long I2C_DELAY_US = 8;
    static constexpr unsigned long CLOCK_STRETCH_TIMEOUT_US = 1000;

    // Device addresses
    static const uint8_t RT6160_ADDRESS = 0x75;
    static const uint8_t BQ25792_ADDRESS = 0x6B;
    static const uint8_t BQ28Z610_PRIMARY_ADDRESS = 0x55;
    static const uint8_t BQ28Z610_FALLBACK_ADDRESS = 0x0B;

    // RT6160 registers
    static const uint8_t RT6160_VOUT1_REGISTER = 0x04;
    static const uint8_t RT6160_VOUT2_REGISTER = 0x05;
    static const uint8_t RT6160_4V5_SETTING = 0x63;

    // BQ25792 registers
    static const uint8_t BQ25792_MIN_SYSTEM_VOLTAGE_REGISTER = 0x00;
    static const uint8_t BQ25792_ADC_CONTROL_REGISTER = 0x2E;
    static const uint8_t BQ25792_CHARGE_VOLTAGE_REGISTER = 0x01;
    static const uint8_t BQ25792_CHARGE_CURRENT_REGISTER = 0x03;
    static const uint8_t BQ25792_INPUT_CURRENT_REGISTER = 0x06;
    static const uint8_t BQ25792_RECHARGE_CONTROL_REGISTER = 0x0A;
    static const uint8_t BQ25792_CHARGER_CONTROL_0_REGISTER = 0x0F;
    static const uint8_t BQ25792_CHARGER_CONTROL_1_REGISTER = 0x10;
    static const uint8_t BQ25792_NTC_CONTROL_1_REGISTER = 0x18;
    static const uint8_t BQ25792_STATUS_0_REGISTER = 0x1B;
    static const uint8_t BQ25792_STATUS_1_REGISTER = 0x1C;
    static const uint8_t BQ25792_STATUS_2_REGISTER = 0x1D;
    static const uint8_t BQ25792_STATUS_3_REGISTER = 0x1E;
    static const uint8_t BQ25792_STATUS_4_REGISTER = 0x1F;
    static const uint8_t BQ25792_FAULT_STATUS_0_REGISTER = 0x20;
    static const uint8_t BQ25792_FAULT_STATUS_1_REGISTER = 0x21;
    static const uint8_t BQ25792_CHARGER_FLAG_0_REGISTER = 0x22;
    static const uint8_t BQ25792_CHARGER_FLAG_1_REGISTER = 0x23;
    static const uint8_t BQ25792_CHARGER_FLAG_2_REGISTER = 0x24;
    static const uint8_t BQ25792_CHARGER_FLAG_3_REGISTER = 0x25;
    static const uint8_t BQ25792_FAULT_FLAG_0_REGISTER = 0x26;
    static const uint8_t BQ25792_FAULT_FLAG_1_REGISTER = 0x27;
    static const uint8_t BQ25792_IBAT_ADC_REGISTER = 0x33;
    static const uint8_t BQ25792_VBAT_ADC_REGISTER = 0x3B;
    static const uint8_t BQ25792_VSYS_ADC_REGISTER = 0x3D;

    // BQ28Z610 standard commands
    static const uint8_t BQ28Z610_TEMPERATURE_REGISTER = 0x06;
    static const uint8_t BQ28Z610_VOLTAGE_REGISTER = 0x08;
    static const uint8_t BQ28Z610_CURRENT_REGISTER = 0x0C;
    static const uint8_t BQ28Z610_RELATIVE_SOC_REGISTER = 0x2C;
    static const uint8_t BQ28Z610_REMAINING_CAPACITY_REGISTER = 0x10;
    static const uint8_t BQ28Z610_FULL_CHARGE_CAPACITY_REGISTER = 0x12;
    static const uint8_t BQ28Z610_MAC_DATA_START_REGISTER = 0x40;
    static const uint8_t BQ28Z610_MAC_CHECKSUM_REGISTER = 0x60;
    static const uint8_t BQ28Z610_MAC_LENGTH_REGISTER = 0x61;

    // BQ28Z610 alternate commands and persistent DF addresses
    static const uint16_t BQ28Z610_FET_CONTROL_COMMAND = 0x0022;
    static const uint16_t BQ28Z610_GAUGING_COMMAND = 0x0021;
    static const uint16_t BQ28Z610_SAFETY_STATUS_COMMAND = 0x0051;
    static const uint16_t BQ28Z610_OPERATION_STATUS_COMMAND = 0x0054;
    static const uint16_t BQ28Z610_GAUGING_STATUS_COMMAND = 0x0056;
    static const uint16_t BQ28Z610_MANUFACTURING_STATUS_COMMAND = 0x0057;
    static const uint16_t BQ28Z610_DA_STATUS_1_COMMAND = 0x0071;
    static const uint16_t BQ28Z610_IT_STATUS_1_COMMAND = 0x0073;
    static const uint16_t BQ28Z610_DEVICE_RESET_COMMAND = 0x0012;
    static const uint16_t BQ28Z610_SEAL_COMMAND = 0x0030;
    static const uint16_t BQ28Z610_DEFAULT_UNSEAL_KEY_1 = 0x0414;
    static const uint16_t BQ28Z610_DEFAULT_UNSEAL_KEY_2 = 0x3672;
    static const uint16_t BQ28Z610_CHEM_ID_ADDRESS = 0x4628;
    static const uint16_t BQ28Z610_DESIGN_CAPACITY_ADDRESS = 0x462A;
    static const uint16_t BQ28Z610_DESIGN_ENERGY_ADDRESS = 0x462C;
    static const uint16_t BQ28Z610_TERMINATE_VOLTAGE_ADDRESS = 0x4632;
    static const uint16_t BQ28Z610_FET_OPTIONS_ADDRESS = 0x4600;
    static const uint16_t BQ28Z610_I2C_GAUGING_CONFIGURATION_ADDRESS = 0x4601;
    static const uint16_t BQ28Z610_I2C_CONFIGURATION_ADDRESS = 0x4602;
    static const uint16_t BQ28Z610_POWER_CONFIG_ADDRESS = 0x4604;
    static const uint16_t BQ28Z610_SOC_FLAG_CONFIG_A_ADDRESS = 0x4632;
    static const uint16_t BQ28Z610_SOC_FLAG_CONFIG_B_ADDRESS = 0x4634;
    static const uint16_t BQ28Z610_QMAX_CELL_1_ADDRESS = 0x4206;
    static const uint16_t BQ28Z610_QMAX_CELL_2_ADDRESS = 0x4208;
    static const uint16_t BQ28Z610_QMAX_PACK_ADDRESS = 0x420A;
    static const uint16_t BQ28Z610_UPDATE_STATUS_ADDRESS = 0x420E;
    static const uint16_t BQ28Z610_IT_GAUGING_CONFIGURATION_ADDRESS = 0x464D;
    static const uint16_t BQ28Z610_CHARGING_CONFIGURATION_ADDRESS = 0x465E;
    static const uint16_t BQ28Z610_TEMPERATURE_ENABLE_ADDRESS = 0x469A;
    static const uint16_t BQ28Z610_CHARGING_VOLTAGE_ADDRESS = 0x4682;
    static const uint16_t BQ28Z610_TAPER_CURRENT_ADDRESS = 0x4692;
    static const uint16_t BQ28Z610_TAPER_VOLTAGE_ADDRESS = 0x4694;
    static const uint16_t BQ28Z610_DA_CONFIGURATION_ADDRESS = 0x469B;
    static const uint16_t BQ28Z610_BALANCING_CONFIGURATION_ADDRESS = 0x470A;

    // Generic bring-up defaults. These are intentionally centralized so they can
    // be replaced with production-cell values later without changing control flow.
    static const uint16_t DEFAULT_MIN_SYSTEM_VOLTAGE_MV = 3500;
    static const uint16_t DEFAULT_CHARGE_VOLTAGE_MV = 4180;
    static const uint16_t DEFAULT_CHARGE_CURRENT_MA = 1000;
    static const uint16_t DEFAULT_INPUT_CURRENT_LIMIT_MA = 1000;
    static const uint16_t DEFAULT_GAUGE_DESIGN_CAPACITY_MAH = 2500;
    static const uint16_t DEFAULT_GAUGE_DESIGN_ENERGY_MWH = 925;
    static const uint8_t DEFAULT_GAUGE_FET_OPTIONS = 0x20;
    static const uint8_t DEFAULT_GAUGE_I2C_GAUGING_CONFIGURATION = 0x04;
    static const uint8_t DEFAULT_GAUGE_I2C_CONFIGURATION = 0x01;
    static const uint8_t DEFAULT_GAUGE_POWER_CONFIG = 0x00;
    static const uint16_t DEFAULT_GAUGE_SOC_FLAG_CONFIG_A = 0x0C8C;
    static const uint8_t DEFAULT_GAUGE_SOC_FLAG_CONFIG_B = 0x8C;
    static const uint16_t DEFAULT_GAUGE_QMAX_CELL_1_MAH = 2500;
    static const uint16_t DEFAULT_GAUGE_QMAX_CELL_2_MAH = 0;
    static const uint16_t DEFAULT_GAUGE_QMAX_PACK_MAH = 2500;
    static const uint8_t DEFAULT_GAUGE_UPDATE_STATUS = 0x04;
    static const uint16_t DEFAULT_GAUGE_IT_GAUGING_CONFIGURATION = 0x14CE;
    static const uint8_t DEFAULT_GAUGE_CHARGING_CONFIGURATION = 0x00;
    static const uint8_t DEFAULT_GAUGE_TEMPERATURE_ENABLE = 0x03;
    static const uint16_t DEFAULT_GAUGE_TERMINATE_VOLTAGE_MV = 3200;
    static const uint16_t DEFAULT_GAUGE_CHARGING_VOLTAGE_MV = 4200;
    static const uint16_t DEFAULT_GAUGE_TAPER_CURRENT_MA = 100;
    static const uint16_t DEFAULT_GAUGE_TAPER_VOLTAGE_MV = 100;
    static const uint16_t DEFAULT_GAUGE_CHEM_ID = 0;
    static const uint8_t DEFAULT_GAUGE_DA_CONFIGURATION = 0x10;
    static const uint8_t DEFAULT_GAUGE_BALANCING_CONFIGURATION = 0x01;
    
    // Battery voltage constants (for single cell Li-ion/Li-Po)
    static constexpr float BATTERY_MIN_VOLTAGE = 3.0;    // Minimum safe voltage
    static constexpr float BATTERY_MAX_VOLTAGE = 4.2;    // Maximum charge voltage
    static constexpr float BATTERY_NOMINAL_VOLTAGE = 3.7; // Nominal voltage
    
    // Smoothing and calibration
    static constexpr int SMOOTHING_SAMPLES = 10;
    static constexpr float CALIBRATION_OFFSET = 0.0;     // Adjust based on actual measurements
    static constexpr unsigned long DEVICE_RESCAN_INTERVAL = 5000;
    
    // Private members
    float voltageBuffer[SMOOTHING_SAMPLES];
    int bufferIndex;
    bool bufferFilled;
    unsigned long lastUpdate;
    static constexpr unsigned long UPDATE_INTERVAL = 1000; // Update every 1 second
    
    // Battery state
    float currentVoltage;
    float currentPercentage;
    float currentTemperatureCelsius;
    float currentSystemVoltage;
    int currentBatteryCurrentMilliAmps;
    uint16_t currentRemainingCapacityMilliAmpHours;
    uint16_t currentFullChargeCapacityMilliAmpHours;
    uint16_t gaugeDesignCapacityMilliAmpHours;
    bool isCharging;
    bool isChargingPrevious;
    bool telemetryValid;

    // Device state
    bool busInitialized;
    bool rt6160Present;
    bool chargerPresent;
    bool gaugePresent;
    bool rt6160Configured;
    bool chargerConfigured;
    bool chargerAdcEnabled;
    bool chargerEnablePinAsserted;
    bool gaugeConfigured;
    bool gaugeSealed;
    uint8_t gaugeAddress;
    uint8_t rt6160Vout1Value;
    uint8_t rt6160Vout2Value;
    uint8_t chargerStatus0;
    uint8_t chargerStatus1;
    uint8_t chargerStatus2;
    uint8_t chargerStatus3;
    uint8_t chargerStatus4;
    uint8_t chargerFaultStatus0;
    uint8_t chargerFaultStatus1;
    uint8_t chargerFlag0;
    uint8_t chargerFlag1;
    uint8_t chargerFlag2;
    uint8_t chargerFlag3;
    uint8_t chargerFaultFlag0;
    uint8_t chargerFaultFlag1;
    uint8_t chargerFlagHistory0;
    uint8_t chargerFlagHistory1;
    uint8_t chargerFlagHistory2;
    uint8_t chargerFlagHistory3;
    uint8_t chargerFaultFlagHistory0;
    uint8_t chargerFaultFlagHistory1;
    uint8_t chargerRechargeControlRegister;
    uint8_t chargerMinimumSystemVoltageRegister;
    uint16_t chargerChargeVoltageRegister;
    uint16_t chargerChargeCurrentRegister;
    uint16_t chargerInputCurrentRegister;
    uint8_t chargerControl0Register;
    uint8_t chargerControl1Register;
    uint8_t chargerNtcControl1Register;
    uint8_t chargerAdcControlRegister;
    uint32_t gaugeSafetyStatus;
    uint32_t gaugeGaugingStatus;
    uint16_t gaugeOperationStatus;
    uint16_t gaugeManufacturingStatus;
    uint8_t gaugeDaConfiguration;
    uint16_t gaugeCell1VoltageMillivolts;
    uint16_t gaugeCell2VoltageMillivolts;
    uint16_t gaugeBatVoltageMillivolts;
    uint16_t gaugePackVoltageMillivolts;
    uint16_t gaugeChemId;
    uint16_t gaugeQMaxCell1MilliAmpHours;
    uint16_t gaugeQMaxCell2MilliAmpHours;
    uint16_t gaugeQMaxPackMilliAmpHours;
    int16_t gaugeTrueRemainingCapacityMilliAmpHours;
    uint16_t gaugeTrueFullChargeCapacityMilliAmpHours;
    uint8_t gaugeUpdateStatus;
    unsigned long lastDeviceScan;
    
    // Constructor (private for singleton)
    BatteryManager();
    
    // Helper methods
    float calculateSmoothedVoltage();
    float voltageToPercentage(float voltage);
    void refreshBatteryState();
    void refreshDevicePresence();
    void updateChargingStatus();
    bool applyPowerConfiguration(bool forceGaugeProvision);
    void assertChargerEnablePin();
    bool configureRt6160For4V5();
    bool configureChargerDefaults();
    bool enableChargerAdc();
    bool ensureGaugeProvisioned(bool force);
    bool updateGaugeMeasurements();
    bool updateChargerMeasurements();

    // Bit-banged I2C helpers for the dedicated battery bus.
    void releaseSDA();
    void releaseSCL();
    void driveSDALow();
    void driveSCLLow();
    void i2cDelay();
    bool waitForSCLHigh();
    void i2cStart();
    void i2cStop();
    bool i2cWriteByte(uint8_t value);
    uint8_t i2cReadByte(bool ack);
    bool probeDevice(uint8_t address);
    bool writeRegisters(uint8_t address, uint8_t startReg, const uint8_t *data, size_t length);
    bool readRegisters(uint8_t address, uint8_t startReg, uint8_t *data, size_t length);
    bool writeRegister8(uint8_t address, uint8_t reg, uint8_t value);
    bool writeRegister16LE(uint8_t address, uint8_t reg, uint16_t value);
    bool writeRegister16BE(uint8_t address, uint8_t reg, uint16_t value);
    bool readRegister8(uint8_t address, uint8_t reg, uint8_t &value);
    bool readRegister16LE(uint8_t address, uint8_t reg, uint16_t &value);
    bool readRegister16LESigned(uint8_t address, uint8_t reg, int16_t &value);
    bool readRegister16BE(uint8_t address, uint8_t reg, uint16_t &value);
    bool readRegister16BESigned(uint8_t address, uint8_t reg, int16_t &value);
    bool writeGaugeAltCommand(uint16_t command);
    bool readGaugeAltBlock(uint16_t command, uint8_t *data, size_t length);
    bool readGaugeAltStatus16(uint16_t command, uint16_t &value);
    bool readGaugeAltStatus32(uint16_t command, uint32_t &value);
    bool readGaugeOperationStatus(uint16_t &status);
    bool unsealGauge();
    bool sealGauge();
    bool readGaugeDataFlashByte(uint16_t address, uint8_t &value);
    bool readGaugeDataFlashWord(uint16_t address, uint16_t &value);
    bool writeGaugeDataFlashByte(uint16_t address, uint8_t value);
    bool writeGaugeDataFlashWord(uint16_t address, uint16_t value);
    static const char* chargerStateToString(uint8_t state);
    
public:
    // Singleton access
    static BatteryManager& getInstance();
    
    // Initialization
    void begin();
    bool reconfigure(bool forceGaugeProvision = false);
    bool forceGaugeProvisioning();
    bool resetGaugeLearningState();
    void prepareForDeepSleep();
    
    // Main update method (call regularly in loop)
    void update();
    bool restoreGaugeFetControl();
    bool setGaugeSingleCellMode();
    bool restartChargeCycle();
    bool clearChargerFaultHistory();
    
    // Getter methods
    float getBatteryVoltage() const { return currentVoltage; }
    float getBatteryPercentage() const { return currentPercentage; }
    bool getChargingStatus() const { return isCharging; }
    bool hasTelemetry() const { return telemetryValid; }
    
    // Battery status methods
    bool isBatteryLow() const { return telemetryValid && currentPercentage < 15.0; }
    bool isBatteryCritical() const { return telemetryValid && currentPercentage < 5.0; }
    bool isBatteryFull() const { return telemetryValid && currentPercentage >= 95.0 && isCharging; }
    
    // Utility methods
    String getBatteryStatusString() const;
    void printBatteryInfo() const;
    void printBusScan();
    
    // Calibration methods
    void calibrate(float actualVoltage);
    void resetCalibration();
    
    // Event callbacks (optional)
    typedef void (*BatteryEventCallback)(float voltage, float percentage, bool charging);
    void setLowBatteryCallback(BatteryEventCallback callback);
    void setChargingStateChangeCallback(BatteryEventCallback callback);
    
private:
    BatteryEventCallback lowBatteryCallback;
    BatteryEventCallback chargingStateChangeCallback;
    bool lowBatteryCallbackTriggered;
};

#endif // BATTERYMANAGEMENT_H