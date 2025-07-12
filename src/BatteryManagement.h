#ifndef BATTERYMANAGEMENT_H
#define BATTERYMANAGEMENT_H

#include <Arduino.h>

class BatteryManager {
private:
    // Singleton instance
    static BatteryManager* instance;
    
    // Pin definitions
    static const int BATTERY_ADC_PIN = 39;     // SENSOR_VN (GPIO39)
    static const int CHARGING_PIN = 34;         // IO34 for charging indicator
    
    // Battery voltage constants (for single cell Li-ion/Li-Po)
    static constexpr float BATTERY_MIN_VOLTAGE = 3.0;    // Minimum safe voltage
    static constexpr float BATTERY_MAX_VOLTAGE = 4.2;    // Maximum charge voltage
    static constexpr float BATTERY_NOMINAL_VOLTAGE = 3.7; // Nominal voltage
    
    // ADC constants for ESP32
    static constexpr float ADC_REFERENCE_VOLTAGE = 3.3;   // ESP32 ADC reference
    static constexpr int ADC_RESOLUTION = 4095;           // 12-bit ADC
    static constexpr float VOLTAGE_DIVIDER_RATIO = 2.0;   // Voltage divided by 2
    
    // Smoothing and calibration
    static constexpr int SMOOTHING_SAMPLES = 10;
    static constexpr float CALIBRATION_OFFSET = 0.0;     // Adjust based on actual measurements
    
    // Private members
    float voltageBuffer[SMOOTHING_SAMPLES];
    int bufferIndex;
    bool bufferFilled;
    unsigned long lastUpdate;
    static constexpr unsigned long UPDATE_INTERVAL = 1000; // Update every 1 second
    
    // Battery state
    float currentVoltage;
    float currentPercentage;
    bool isCharging;
    bool isChargingPrevious;
    
    // Constructor (private for singleton)
    BatteryManager();
    
    // Helper methods
    float readRawVoltage();
    float calculateSmoothedVoltage();
    float voltageToPercentage(float voltage);
    void updateChargingStatus();
    
public:
    // Singleton access
    static BatteryManager& getInstance();
    
    // Initialization
    void begin();
    
    // Main update method (call regularly in loop)
    void update();
    
    // Getter methods
    float getBatteryVoltage() const { return currentVoltage; }
    float getBatteryPercentage() const { return currentPercentage; }
    bool getChargingStatus() const { return isCharging; }
    
    // Battery status methods
    bool isBatteryLow() const { return currentPercentage < 15.0; }
    bool isBatteryCritical() const { return currentPercentage < 5.0; }
    bool isBatteryFull() const { return currentPercentage >= 95.0 && isCharging; }
    
    // Utility methods
    String getBatteryStatusString() const;
    void printBatteryInfo() const;
    
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