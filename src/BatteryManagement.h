#ifndef BATTERYMANAGEMENT_H
#define BATTERYMANAGEMENT_H

#include <Arduino.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "Policy.h"

class BatteryManager {
private:
    // Singleton instance
    static BatteryManager* instance;
    
    // Battery power bus on the new board is a dedicated I2C segment on GPIO1/2.
    static const int BATTERY_I2C_SDA_PIN = 1;
    static const int BATTERY_I2C_SCL_PIN = 2;
    static const int BATTERY_CHARGER_ENABLE_PIN = 44; // ESP32-S3 RXD0 wired to BQ25792 CE through 390 ohms.
    static constexpr unsigned long I2C_DELAY_US = 8;
    // TI gauges (bq28z610 / bq27xxx family) need >= 66 us bus-free time between a STOP and the
    // next START, and after a command write before the read. The old pinMode()-based bit-bang
    // provided this by accident; the register-driven bus must enforce it explicitly.
    static constexpr unsigned long I2C_BUS_FREE_US = 80;
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
    static const uint8_t BQ25792_TERMINATION_CONTROL_REGISTER = 0x09; // bit6 = REG_RST (never write 1), ITERM[4:0] 40mA/LSB
    static const uint8_t DEFAULT_TERMINATION_CURRENT_CODE = 0x03;    // 120 mA (POR 0x05 = 200 mA)
    static const uint8_t DEFAULT_RECHARGE_THRESHOLD_CODE = 0x04;     // VREG - 250 mV (POR 0x03 = 200 mV), 50 mV/LSB
    static const uint8_t BQ25792_INPUT_VOLTAGE_REGISTER = 0x05; // VINDPM, 100mV/LSB
    static const uint8_t BQ25792_CHARGER_CONTROL_0_REGISTER = 0x0F;
    static const uint8_t BQ25792_CHARGER_CONTROL_1_REGISTER = 0x10;
    static const uint8_t BQ25792_CHARGER_CONTROL_2_REGISTER = 0x11;
    static const uint8_t BQ25792_CHARGER_CONTROL_5_REGISTER = 0x14;
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
    static const uint8_t BQ25792_VBUS_ADC_REGISTER = 0x35;
    static const uint8_t BQ25792_VBAT_ADC_REGISTER = 0x3B;
    static const uint8_t BQ25792_VSYS_ADC_REGISTER = 0x3D;

    // CH224Q USB-PD sink controller (U28). Lives on the battery I2C bus per WCH's
    // reference design (single-resistor 9V request on CFG1 + I2C auto-enabled on
    // CFG2/SCL, CFG3/SDA). Powered from VHV = VBUS, so it only answers while USB is
    // plugged. 7-bit address is 0x22 or 0x23 depending on chip batch.
    static const uint8_t CH224Q_ADDRESS_A = 0x22;
    static const uint8_t CH224Q_ADDRESS_B = 0x23;
    static const uint8_t CH224Q_STATUS_REGISTER = 0x09;          // protocol handshake bits
    static const uint8_t CH224Q_VOLTAGE_CONTROL_REGISTER = 0x0A; // 0=5V 1=9V 2=12V 3=15V 4=20V
    static const uint8_t CH224Q_CURRENT_REGISTER = 0x50;         // granted current, 50mA units (PD only)

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
    static const uint16_t BQ28Z610_FET_CONTROL_COMMAND = 0x0022;   // toggles ManufacturingStatus[FET_EN]
    static const uint16_t BQ28Z610_CHG_FET_COMMAND = 0x001F;       // toggles ManufacturingStatus[CHG_TEST] (manual CHG FET)
    // ManufacturingStatus bits (TRM): PCHG_TEST 0, CHG_TEST 1, DSG_TEST 2, GAUGE_EN 3, FET_EN 4
    static const uint16_t BQ28Z610_MFG_CHG_TEST = 0x0002;
    static const uint16_t BQ28Z610_MFG_DSG_TEST = 0x0004;
    static const uint16_t BQ28Z610_MFG_FET_EN = 0x0010;
    static const uint16_t BQ28Z610_CHEM_ID_COMMAND = 0x0006;   // AltManufacturerAccess: the real Chemical ID
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
    // Data-flash addresses verified against datasheet sluua65 DF map (2026-06-26).
    // Several were previously wrong (term voltage collided with SOC Flag Config A;
    // taper current/voltage hit the Charging Voltage Hysteresis field) — corrected.
    static const uint16_t BQ28Z610_CC_GAIN_ADDRESS = 0x4006;       // F4 (IEEE-754 LE float)
    static const uint16_t BQ28Z610_CAPACITY_GAIN_ADDRESS = 0x400A; // F4; = CC Gain * 298261.6178
    static constexpr float BQ28Z610_DEFAULT_CC_GAIN = 3.58422f;    // factory default (uncalibrated)
    // NOTE: 0x4628 is NOT a documented data-flash field (the TRM table goes 0x4626 -> 0x4629).
    // Earlier firmware wrote 0x0418 here and read it back as a "Chem ID"; the real chemistry
    // is read with MAC 0x0006 and can only be programmed with bqStudio. Kept for reference.
    static const uint16_t BQ28Z610_CHEM_ID_ADDRESS = 0x4628;
    static const uint16_t BQ28Z610_BROADCAST_PACING_ADDRESS = 0x4629; // U1, default 15 s (corrupted by the 0x4628 write)
    static const uint8_t DEFAULT_GAUGE_BROADCAST_PACING_S = 15;
    static const uint16_t BQ28Z610_DESIGN_CAPACITY_ADDRESS = 0x462A;
    static const uint16_t BQ28Z610_DESIGN_ENERGY_ADDRESS = 0x462C;
    static const uint16_t BQ28Z610_TERMINATE_VOLTAGE_ADDRESS = 0x45BE; // was 0x4632 (=SOC Flag Config A!)
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
    // Charging Voltage is per JEITA temperature zone (Low/Med/High); set all for 1S LFP.
    // Advanced Charge Algorithm. NOTE the naming trap that cost this product its charge
    // termination: 0x468C/0x468E/0x4690 are the "Voltage Range" BOUNDARIES (Charging Voltage
    // Low/Med/High, which select the charging-current range); the voltages the gauge expects
    // the cell to reach are the per-temperature "Charging Voltage" fields below.
    static const uint16_t BQ28Z610_VOLTAGE_RANGE_LOW_ADDRESS = 0x468C;   // TRM default 2900
    static const uint16_t BQ28Z610_VOLTAGE_RANGE_MED_ADDRESS = 0x468E;   // TRM default 3600
    static const uint16_t BQ28Z610_VOLTAGE_RANGE_HIGH_ADDRESS = 0x4690;
    static const uint16_t BQ28Z610_LOW_TEMP_CHARGING_VOLTAGE_ADDRESS = 0x4666;  // TRM default 4000
    static const uint16_t BQ28Z610_STD_TEMP_CHARGING_VOLTAGE_ADDRESS = 0x466E;  // TRM default 4200
    static const uint16_t BQ28Z610_HIGH_TEMP_CHARGING_VOLTAGE_ADDRESS = 0x4676; // TRM default 4000
    static const uint16_t BQ28Z610_REC_TEMP_CHARGING_VOLTAGE_ADDRESS = 0x467E;  // TRM default 4100
    static const uint16_t BQ28Z610_REC_TEMP_CHARGING_CURRENT_MED_ADDRESS = 0x4682; // TRM default 4488 (old fw wrote 3600 here)
    static const uint16_t BQ28Z610_CHARGE_TERM_VOLTAGE_ADDRESS = 0x4695; // I2, TRM default 75 mV (old fw zeroed it)
    static const uint16_t BQ28Z610_DESIGN_VOLTAGE_ADDRESS = 0x462E;      // I2, TRM default 7200 (2S)
    static const uint16_t BQ28Z610_TAPER_CURRENT_ADDRESS = 0x4693; // was 0x4692 (=Charging Voltage Hysteresis)
    static const uint16_t BQ28Z610_DA_CONFIGURATION_ADDRESS = 0x469B;
    // Current Thresholds (Gas Gauging) — required for learning-cycle relax/charge/discharge
    // detection. Relationship: Taper > Dsg/Chg threshold > Quit; Quit < C/20.
    static const uint16_t BQ28Z610_DSG_CURRENT_THRESHOLD_ADDRESS = 0x46A6;
    static const uint16_t BQ28Z610_CHG_CURRENT_THRESHOLD_ADDRESS = 0x46A8;
    static const uint16_t BQ28Z610_QUIT_CURRENT_ADDRESS = 0x46AA;
    static const uint16_t BQ28Z610_BALANCING_CONFIGURATION_ADDRESS = 0x470A;
    // Protection (Settings:Protection) — read-back for diagnosing over-discharge behaviour.
    static const uint16_t BQ28Z610_PROTECTION_CONFIG_ADDRESS = 0x46AE;     // bit1 CUV_RECOV_CHG
    static const uint16_t BQ28Z610_ENABLED_PROTECTIONS_A_ADDRESS = 0x46AF; // bit0 CUV, bit1 COV, bit2 OCC
    static const uint16_t BQ28Z610_ENABLED_PROTECTIONS_B_ADDRESS = 0x46B0;
    static const uint16_t BQ28Z610_CUV_THRESHOLD_ADDRESS = 0x46B3; // mV (default 2500)
    // The cell cut (low-battery cutoff, ship mode): the CUV threshold is raised to this value so
    // the gauge trips its own under-voltage protection, which opens the DSG FET and keeps the
    // CHG FET closed (a charger always reaches the cell); restored to GAUGE_CUV_MV at the next boot.
    static const uint16_t GAUGE_CUV_MV = 2500;
    static const uint16_t GAUGE_CUT_CUV_MV = 3700;
    static const uint16_t BQ28Z610_CUV_DELAY_ADDRESS = 0x46B5;     // s  (default 2)
    static const uint16_t BQ28Z610_CUV_RECOVERY_ADDRESS = 0x46B6;  // mV (default 3000)

    // Generic bring-up defaults. These are intentionally centralized so they can
    // be replaced with production-cell values later without changing control flow.
    // VSYSMIN must sit BELOW the LFP cell's operating range. The LiFePO4 1S cell
    // rests ~3.2V; with the old 3.5V VSYSMIN the charger had to boost SYS above the
    // cell AND it was only 100mV below VREG (3.6V) — that inverted/tight spacing made
    // the buck-boost loop oscillate (SYS collapsed to ~2.6V, false VBAT OVP, pulsing
    // rail). 3.0V lets the system run straight off the cell; the ESP 3.3V rail is a
    // buck-boost (RT6160 U34) that works from SYS down to ~2V, so it doesn't need 3.5V.
    static const uint16_t DEFAULT_MIN_SYSTEM_VOLTAGE_MV = 3000;
    // LiFePO4 1S: charge (CV) target 3.6V. NEVER raise toward Li-ion 4.2V — that
    // would overcharge and damage the cell.
    static const uint16_t DEFAULT_CHARGE_VOLTAGE_MV = 3600;
    // Charge current 1A (~C/4). After current calibration (gaugecalcurrent) the gauge no
    // longer over-reads, so OCC won't false-trip. 1A fits the ICO-limited USB input
    // (~1A×3.1V charge + ~3W system < ~7.5W) so it won't starve SYS even at low SOC.
    // Could go to 2A with a stronger (PD 9V) input. NOTE: requires the gauge to be
    // current-calibrated first — on an uncalibrated gauge use 300mA (it over-reads ~4.5x).
    static const uint16_t DEFAULT_CHARGE_CURRENT_MA = Policy::CHARGE_CURRENT_FAST_MA;
    static const uint16_t DEFAULT_INPUT_CURRENT_LIMIT_MA = 3000;
    // 4000mAh LiFePO4 1S cell. Design Energy is in 10mWh units (capacity_mAh ×
    // nominal_3.2V / 10 = 4000 × 3.2 / 10 = 1280).
    static const uint16_t DEFAULT_GAUGE_DESIGN_CAPACITY_MAH = 4000;
    static const uint16_t DEFAULT_GAUGE_DESIGN_ENERGY_MWH = 1280;
    static const uint8_t DEFAULT_GAUGE_FET_OPTIONS = 0x00; // FETs stay closed in every charging mode (was 0x20): at FC the gauge must not open the CHG FET, the charger terminates; safety faults still open the FETs
    static const uint8_t DEFAULT_GAUGE_I2C_GAUGING_CONFIGURATION = 0x04;
    static const uint8_t DEFAULT_GAUGE_I2C_CONFIGURATION = 0x00; // BCAST off: no smart charger on this bus, and the gauge
                                                                   // becoming bus master every pacing interval collides with the bit-bang
    static const uint8_t DEFAULT_GAUGE_POWER_CONFIG = 0x00;
    static const uint16_t DEFAULT_GAUGE_SOC_FLAG_CONFIG_A = 0x0C8C;
    static const uint8_t DEFAULT_GAUGE_SOC_FLAG_CONFIG_B = 0x8C;
    static const uint16_t DEFAULT_GAUGE_QMAX_CELL_1_MAH = 4000;
    static const uint16_t DEFAULT_GAUGE_QMAX_CELL_2_MAH = 0;
    static const uint16_t DEFAULT_GAUGE_QMAX_PACK_MAH = 4000;
    // 0x04 = IT enabled, learning (gauge will learn QMax/Ra during a learning cycle and
    // advance this to 0x05 -> 0x06). Start a fresh gauge in learning mode and run the
    // SLUA777 learning cycle to populate Qmax/Ra properly.
    static const uint8_t DEFAULT_GAUGE_UPDATE_STATUS = 0x04;
    static const uint16_t DEFAULT_GAUGE_IT_GAUGING_CONFIGURATION = 0x14CE;
    static const uint8_t DEFAULT_GAUGE_CHARGING_CONFIGURATION = 0x00;
    static const uint8_t DEFAULT_GAUGE_TEMPERATURE_ENABLE = 0x03;
    static const uint16_t DEFAULT_GAUGE_TERMINATE_VOLTAGE_MV = 2500; // LiFePO4 EDV / discharge cutoff (cell min)
    static const uint16_t DEFAULT_GAUGE_CHARGING_VOLTAGE_MV = 3600;  // LiFePO4 1S charge voltage (all temp zones)
    // SLUA777 §3 relationship: Taper > Dsg/Chg current threshold > Quit; Quit < C/20.
    // 4000mAh: C/10=400, C/20=200. Taper 200mA, Dsg 100, Chg 75, Quit 40.
    static const uint16_t DEFAULT_GAUGE_TAPER_CURRENT_MA = 100;    // below the charger ITERM (120 mA): the charger ends the charge, the gauge only confirms it
    static const uint16_t DEFAULT_GAUGE_CHARGE_TERM_VOLTAGE_MV = 25;  // valid termination needs cell >= 3575 mV: the ~0.2 ohm pack path put the cell at 3.5 V while the charger sat at 3.6 V, and the gauge called that full
    static const uint16_t DEFAULT_GAUGE_VOLTAGE_RANGE_LOW_MV = 2900;
    static const uint16_t DEFAULT_GAUGE_VOLTAGE_RANGE_MED_MV = 3300;
    static const uint16_t DEFAULT_GAUGE_VOLTAGE_RANGE_HIGH_MV = 3450;
    static const uint16_t DEFAULT_GAUGE_DESIGN_VOLTAGE_MV = 3200;
    static const uint16_t DEFAULT_GAUGE_REC_TEMP_CHARGING_CURRENT_MED_MA = 4488; // TI default, restored
    static const uint16_t DEFAULT_GAUGE_DSG_CURRENT_THRESHOLD_MA = 100;
    static const uint16_t DEFAULT_GAUGE_CHG_CURRENT_THRESHOLD_MA = 75;
    static const uint16_t DEFAULT_GAUGE_QUIT_CURRENT_MA = 40;
    // Chemistry: the firmware does not program one (it is not a data-flash word; see
    // docs/GAUGE-CHEMISTRY.md). The gauge keeps whatever chemistry it shipped with until the
    // golden data-flash image from a bqStudio session is replayed; MAC 0x0006 reports it.
    // DA Configuration: bit0 CC0=0 (1S pack), bit4 SLEEP=1, bit3 IN_SYSTEM_SLEEP=1.
    // IN_SYSTEM_SLEEP is required on this board: without it the gauge's SLEEP entry
    // needs the I2C bus held LOW, but our bus idles HIGH on pullups, so the gauge
    // never sleeps and burns ~0.4mA from the cell 24/7 (sluua65 §5.3).
    static const uint8_t DEFAULT_GAUGE_DA_CONFIGURATION = 0x18;
    static const uint8_t DEFAULT_GAUGE_BALANCING_CONFIGURATION = 0x01;
    // NOTE: CUV/COV (Protections-class) thresholds are intentionally left at gauge
    // defaults. This gauge rejects Protections-class DF writes (verified by readback),
    // and they aren't needed: the BQ25792 caps charge at 3.6V (no overcharge) and the
    // default CUV (trip 2500 / recovery 3000mV) is fine for normal LFP use.

    static constexpr unsigned long DEVICE_RESCAN_INTERVAL = 5000;
    // No gauge on USB: a charger pulse on the BAT/PACK node wakes a BQ28Z610 out of SHUTDOWN
    // (TI: the cell alone never does, PACK needs >= 100 ms above Charger Present Threshold).
    static constexpr unsigned long GAUGE_WAKE_PULSE_MS = 300;
    static constexpr unsigned long GAUGE_WAKE_PULSE_INTERVAL = 30000;
    unsigned long lastGaugeWakePulse = 0;
    
    // Private members
    unsigned long lastUpdate;
    static constexpr unsigned long UPDATE_INTERVAL = 1000; // Update every 1 second
    
    // Battery state. The voltage is the gauge's (the charger's VBAT ADC only without a gauge),
    // the percentage is the gauge's RelativeStateOfCharge (0x2C) and nothing else: every gauge
    // carries the learned image (GaugeGoldenImage.h), so its state of charge is authoritative.
    // stateOfChargeValid is true while the gauge answers; the last percentage stays readable
    // for displays, decisions must check hasStateOfCharge().
    float currentVoltage;
    float currentPercentage;
    bool stateOfChargeValid = false;
    uint8_t gaugeMissedPolls = 0;
    float currentTemperatureCelsius;
    uint32_t implausibleGaugeReads = 0;  // polls rejected by the plausibility check
    uint8_t rt6160MissCount = 0;          // consecutive failed presence probes
    uint8_t chargerMissCount = 0;
    uint8_t gaugeMissCount = 0;
    float currentSystemVoltage;
    float currentVbusVoltage;
    int currentBatteryCurrentMilliAmps;
    uint16_t currentRemainingCapacityMilliAmpHours;
    uint16_t currentFullChargeCapacityMilliAmpHours;
    uint16_t gaugeDesignCapacityMilliAmpHours;
    bool isCharging;
    bool telemetryValid;

    // Device state
    bool busInitialized;
    bool busReleased = false;   // SDA/SCL floated for an external tool (bqStudio/EV2400)
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
    uint8_t gaugeGoldenImage = 0; // GoldenImage state (see goldenImageStateName())
    unsigned long lastDeviceScan;
    // Reconfigure-on-transition bookkeeping (see refreshBatteryState()).
    bool prevRt6160Present = false;
    bool prevChargerPresent = false;
    bool prevGaugePresent = false;
    uint8_t prevGaugeAddress = 0;
    unsigned long lastChargerSanityCheck = 0;
    static constexpr unsigned long CHARGER_SANITY_INTERVAL = 30000;
    uint8_t pollCycle = 0;
    static constexpr uint8_t FULL_POLL_EVERY = 10; // slow diagnostic reads every N telemetry polls
    bool chargerVoltageVerified = false;
    int chargerIbatMilliAmps = 0; // last IBAT ADC reading of the charger (signed, mA)
    
    // Constructor (private for singleton)
    BatteryManager();
    
    // Helper methods
    void refreshBatteryState();
    void refreshDevicePresence();
    void updateChargingStatus();
    bool applyPowerConfiguration(bool forceGaugeProvision);
    void assertChargerEnablePin();
    void releaseChargerEnablePin();
    bool verifyChargerVoltageSetting();
    bool ensureGaugeBroadcastPacing();
    bool ensureGaugeLfpChargeProfile();
    bool readGaugeChemId(uint16_t &chemId);
    bool configureRt6160For4V5();
    bool configureChargerDefaults();
    bool enableChargerAdc();
    bool ensureGaugeProvisioned(bool force);
    bool updateGaugeMeasurements(bool full = true);
    bool updateChargerMeasurements(bool full = true);

    // Bit-banged I2C helpers for the dedicated battery bus.
    void releaseSDA();
    void releaseSCL();
    void driveSDALow();
    void driveSCLLow();
    void i2cDelay();
    bool waitForSCLHigh();
    void i2cBusRecover();

    // One bit-banged bus, two callers (poll task + loop-side charger supervision): every
    // transaction takes this recursive lock.
    SemaphoreHandle_t busLock = nullptr;
    TaskHandle_t pollTask = nullptr;
    static void pollTaskEntry(void *arg);
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
    bool readGaugeDataFlashBlock32(uint16_t address, uint8_t *out32);   // one MAC read: 32 bytes
    bool writeGaugeDataFlashBlock32(uint16_t address, const uint8_t *data32); // one MAC write: 32 bytes, verified
    bool readGaugeDataFlashWord(uint16_t address, uint16_t &value);
    bool readGaugeDataFlashFloat(uint16_t address, float &value);
    bool writeGaugeDataFlashFloat(uint16_t address, float value);
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

    // Charger HIZ control (BQ25792 REG0F[EN_HIZ]). HIZ makes the charger ignore the
    // USB input so the system runs off the battery — used to force a controlled
    // DISCHARGE during a gauge learning cycle while USB stays connected for serial.
    bool setHizMode(bool enable);
    bool isHizMode();

    // Runtime charge-current limit (BQ25792 REG03). Bring-up uses 300mA before the gauge
    // is current-calibrated (uncalibrated gauge over-reads -> OCC trips), then raises it.
    bool setChargeCurrent(uint16_t milliAmps);

    // Non-destructive bring-up checks (no DF writes / no DEVICE_RESET). Used by the
    // first-boot bring-up to skip provisioning/calibration when they are already done,
    // avoiding a gauge reset that would open the FETs and brown out the system.
    bool isGaugeProvisioned();        // chem/1S/design-capacity look correct
    // The learned data-flash image (src/GaugeGoldenImage.h, docs/GAUGE-CHEMISTRY.md): written onto a
    // gauge that still has the stock chemistry or an unlearned state (Update Status 0x04), then a
    // device reset and a read-back. allowWrite=false only classifies (a reset on battery kills the
    // ESP mid-boot, so the write waits for a USB boot). force rewrites regardless of the state.
    enum GoldenImage : uint8_t { IMAGE_UNKNOWN = 0, IMAGE_APPLIED, IMAGE_LEARNING, IMAGE_STOCK, IMAGE_INHIBITED, IMAGE_FAILED };
    bool replayGaugeGoldenImage(bool force, bool allowWrite);
    const char *goldenImageStateName() const;
    static bool isGoldenImageInhibited();           // NVS flag for bench learning cycles (gaugeimage inhibit)
    static void setGoldenImageInhibited(bool inhibited);
    bool isCurrentSenseCalibrated();  // CC Gain has moved away from the factory default
    // Re-probes the gauge address and returns true only when it is responding at its normal
    // operating address (not ROM mode 0x0B) AND looks provisioned. The bq28z610 boots in ROM
    // mode for several seconds after a low-cell reset, so bring-up must wait on this.
    bool gaugeAliveAndProvisioned();
    // Re-probes and returns true once the gauge answers at its normal operating address
    // (0x55), regardless of provisioning. False while it is still in ROM mode (0x0B).
    bool isGaugeAlive();
    // Ensures CUV protection latches until charging (Protection Configuration bit1
    // CUV_RECOV_CHG). Without it CUV auto-recovers when the unloaded cell relaxes, the load
    // reconnects, and the cell over-discharges. Writes 0x46AE and verifies; returns true if
    // the latch is enabled (already-set or write stuck).
    bool ensureGaugeCuvLatch();
    // Ensures DA Configuration has SLEEP (bit4) + IN_SYSTEM_SLEEP (bit3) set so the gauge
    // can enter its SLEEP mode with the I2C bus idling high (embedded pack). Targeted
    // non-destructive DF write, no DEVICE_RESET; returns true if the bits are set.
    bool ensureGaugeSleepConfig();
    // True while the charger reports a live VBUS (USB attached). The low-battery cutoff
    // must never force sleep in this state: with an adapter present the NVDC power path
    // runs SYS from VBUS regardless of the cell, and sleeping just abandons recovery.
    bool isVbusPresent() const;
    bool isChargeDone() const;              // BQ25792 CHG_STAT = 111 (charge termination done)
    bool isChargerVoltageVerified() const { return chargerVoltageVerified; }
    void refreshDiagnostics();              // force the slow gauge/charger diagnostic reads now
    // Gauge data-flash access for bench diagnostics (byte-wise, via MAC 0x3E; gauge must be
    // unsealed - this product's gauges ship in FULL ACCESS).
    bool readGaugeDataFlash(uint16_t address, uint8_t *buffer, size_t length);
    bool writeGaugeDataFlash8(uint16_t address, uint8_t value);
    // Raw AltManufacturerAccess block (IT Status 1/2/3 = 0x0073-0x0075, DA Status 1 = 0x0071)
    // for bench diagnostics; the command write and the block read run under the bus lock so
    // the polling task cannot slip a command in between.
    bool readGaugeMacBlock(uint16_t command, uint8_t *buffer, size_t length);
    // Hold the battery bus for a sequence of reads (bench `gaugedump`): the polling task waits
    // meanwhile, so no MAC access of its own lands between the blocks. Recursive; always pair.
    void lockBus();
    void unlockBus();
    // NOTE (Adem v7): the charger's SDRV pin only carries a 1 nF capacitor, no ship FET, so the
    // three BQ25792 ship-mode calls below do nothing on this board. They stay for a board that
    // populates the FET; the shipped product and the low-battery cutoff use openGaugeFets().
    // BQ25792 ship mode (REG11 SDRV_CTRL=10b): opens the external ship FET, disconnecting
    // the cell from SYS (~µA cell drain). On battery power this kills the system
    // immediately; wake is USB plug-in only (QON is not wired on this board). Writing it
    // with VBUS present is safely ignored by the charger.
    bool enterShipMode();
    // Factory `ft ship`: the same ship mode with SDRV_DLY=0, i.e. the charger waits 10 s before
    // opening the BATFET, which gives the operator time to unplug USB after the answer.
    // cancelShipMode() writes SDRV_CTRL back to 00b (idle) when the unit is still powered.
    bool armDelayedShipMode();
    bool cancelShipMode();
    // Reads the CH224Q PD-sink status: which fast-charge handshake succeeded (register
    // 0x09: BC1.2/QC2/QC3/PD/EPR bits) and the PD-granted current budget. Only works
    // while VBUS is present (the chip is VBUS-powered). Returns false if it never ACKs.
    bool readUsbPdStatus(uint8_t &status, uint16_t &grantedMilliAmps);
    // Requests a different PD voltage gear via the CH224Q (0=5V 1=9V 2=12V 3=15V 4=20V).
    // Bench/diagnostic use — the resistor-strap default is 9V.
    bool setUsbPdVoltageGear(uint8_t gear);
    // Actual negotiated VBUS voltage from the charger's ADC (volts; 0 when unplugged).
    float getVbusVoltage() const { return currentVbusVoltage; }
    // Cell temperature from the gauge's TS1 thermistor (°C) — drives the charge-current
    // temperature guard (datasheet charge window 0-60°C).
    float getTemperatureC() const { return currentTemperatureCelsius; }
    bool isGaugeChargeInhibited() const { return gaugePresent && (gaugeOperationStatus & 0x4000) != 0; } // XCHG: gauge declared full / inhibits charge
    void printChargeMonitorLine(unsigned long t);  // one compact diagnostic line (full poll)
    int getBatteryCurrentMilliAmps() const { return currentBatteryCurrentMilliAmps; } // + charging, - discharging
    // Input current limit (IINDPM, REG06). 0 = clear override, restore the default and
    // let ICO adapt. Sticky against the periodic charger reconfiguration.
    bool setInputCurrentLimit(uint16_t milliAmps);
    // Input Current Optimizer on/off (REG0F EN_ICO), sticky. Disabled while a PD
    // contract declares the budget — ICO otherwise re-detects (blindly, since the
    // charger's D+/D- are unconnected on this board) and overwrites IINDPM with a
    // legacy ~1.5A guess, under-running the contract.
    bool setIcoEnabled(bool enable);
    // EN_CHG register bit (REG0F). Dead-cell recovery pauses charging while the ESP is
    // awake (the charger's charge-attempt faults on a sub-CUV pack collapse SYS under
    // the awake ESP's load) and re-enables it as the last instruction before deep sleep.
    bool setChargeEnabled(bool enable);

    // Main update method (call regularly in loop)
    void update();
    // Run the periodic poll on its own task (core 0). After this, update() calls from the loop
    // return immediately; the ~190 ms of bit-banged I2C per poll no longer stall the loop.
    void startTask();
    // Hand the battery bus to an external I2C master (bqStudio through an EV2400): the pins
    // float, polling stops and every register access fails fast until the bus is attached again.
    void setBusReleased(bool released);
    bool isBusReleased() const { return busReleased; }
    bool isTaskRunning() const { return pollTask != nullptr; }
    // reconnectCutCell: also put a raised CUV threshold (a cell cut by openGaugeFets()) back.
    // Boot passes "USB present": a cut cell is only reconnected under a charger, so a boot
    // on battery inside the cut's ~3 s window cannot undo it.
    bool restoreGaugeFetControl(bool reconnectCutCell = true);
    bool reconnectCutCellNow();              // CUV latched under a charger: close the DSG FET again
    bool isCellCut() const;                  // gauge CUV latched / DSG FET open: the cell is not on SYS
    bool gaugeDfReadByte(uint16_t address, uint8_t &value);   // bench console
    bool gaugeDfWriteByte(uint16_t address, uint8_t value);   // bench console (unseals)
    // Factory ship: clears the gauge's FET_EN so both protection FETs open and the cell is cut
    // from the system (the charger's own ship mode needs an external ship FET on SDRV, which
    // this board may not populate). Only meaningful once VBUS is gone: SYS collapses at once.
    // The next boot on USB finds FET_EN clear and restoreGaugeFetControl() closes the FETs.
    bool openGaugeFets();
    bool setGaugeSingleCellMode();
    // Calibrate the gauge current sense (CC Gain) against the BQ25792's IBAT ADC.
    // Requires a steady current flowing (e.g. charging). Fixes the ~4.5x over-read.
    bool calibrateCurrentSense();
    bool restartChargeCycle();
    bool clearChargerFaultHistory();
    
    // Getter methods
    float getBatteryVoltage() const { return currentVoltage; }
    // Gauge RemainingCapacity — the coulomb counter used as the "ammeter" for the
    // gauge-based sleep-current measurement (`sleeptest`), since a series DMM in the
    // cell path corrupts the gauge's impedance tracking.
    uint16_t getRemainingCapacityMilliAmpHours() const { return currentRemainingCapacityMilliAmpHours; }
    // The gauge's state of charge (RelativeStateOfCharge). Keeps the last value while the gauge
    // is silent: check hasStateOfCharge() before deciding anything on it.
    float getBatteryPercentage() const { return currentPercentage; }
    bool hasStateOfCharge() const { return stateOfChargeValid; }
    bool getChargingStatus() const { return isCharging; }
    bool hasTelemetry() const { return telemetryValid; } // a voltage reading exists (gauge, or charger ADC without a gauge)
    bool isGaugePresent() const { return gaugePresent; }
    // Factory test (`ft test battery`, bring-up record): cached values of the last poll.
    uint16_t getDesignCapacityMilliAmpHours() const { return gaugeDesignCapacityMilliAmpHours; }
    uint16_t getGaugeChemId() const { return gaugeChemId; }       // MAC 0x0006, informational
    uint8_t getChargerFaultStatus0() const { return chargerFaultStatus0; } // REG20 (live)
    uint8_t getChargerFaultStatus1() const { return chargerFaultStatus1; } // REG21 (live)
    const char *getChargeStateName() const { return chargerStateToString((chargerStatus1 >> 5) & 0x07); }
    int getChargerIbatMilliAmps() const { return chargerIbatMilliAmps; }    // charger IBAT ADC, + charging
    static String describeChargerFaults(uint8_t fault0, uint8_t fault1);    // "None" or "VBUS OVP, ..."
    
    // True when the cell can actually SOURCE current into the system (and thus
    // buffer RF transients) — i.e. the charger sees VBAT present, or the gauge's
    // discharge (DSG) FET is on. NOTE: gauge cell voltage is NOT a valid signal
    // here — the gauge measures the cell even when its protection FETs are open
    // (e.g. latched off by a CUV fault), so the cell reads ~3V while delivering
    // no power. Used at boot to decide whether it is safe to bring up WiFi/BLE.
    bool isBatteryConnected() const {
        return (chargerStatus2 & 0x01) != 0 || (gaugeOperationStatus & 0x0002) != 0;
    }

    // Battery status methods
    bool isBatteryLow() const { return stateOfChargeValid && currentPercentage < Policy::LOW_BATTERY_PERCENT; }
    bool isBatteryCritical() const { return stateOfChargeValid && currentPercentage < 5.0; }
    bool isBatteryFull() const { return stateOfChargeValid && currentPercentage >= 95.0 && isCharging; }
    
    // Utility methods
    String getBatteryStatusString() const;
    void printBatteryInfo() const;
    // Reads back and decodes the gauge's actual protection config (CUV enable/latch/
    // thresholds, COV/OCC) for diagnosing over-discharge behaviour.
    void printProtectionConfig();
    void printBusScan();
    
};

#endif // BATTERYMANAGEMENT_H