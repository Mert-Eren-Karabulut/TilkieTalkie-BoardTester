#include "BatteryManagement.h"

// Static member initialization
BatteryManager* BatteryManager::instance = nullptr;

BatteryManager::BatteryManager() 
    : bufferIndex(0)
    , bufferFilled(false)
    , lastUpdate(0)
    , currentVoltage(0.0)
    , currentPercentage(0.0)
    , isCharging(false)
    , isChargingPrevious(false)
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
    
    // Configure pins
    pinMode(CHARGING_PIN, INPUT);  // Charging indicator pin
    
    // Initialize ADC for battery voltage reading
    analogReadResolution(12);  // Set ADC resolution to 12 bits
    analogSetAttenuation(ADC_11db);  // Set attenuation for 3.3V range
    
    // Take initial readings
    for (int i = 0; i < SMOOTHING_SAMPLES; i++) {
        voltageBuffer[i] = readRawVoltage();
        delay(10);
    }
    bufferFilled = true;
    
    // Initial update
    update();
    
    Serial.println("Battery Manager initialized successfully");
    printBatteryInfo();
}

void BatteryManager::update() {
    unsigned long currentTime = millis();
    
    // Update at specified interval
    if (currentTime - lastUpdate >= UPDATE_INTERVAL) {
        lastUpdate = currentTime;
        
        // Update voltage reading
        voltageBuffer[bufferIndex] = readRawVoltage();
        bufferIndex = (bufferIndex + 1) % SMOOTHING_SAMPLES;
        if (!bufferFilled && bufferIndex == 0) {
            bufferFilled = true;
        }
        
        // Calculate smoothed voltage
        currentVoltage = calculateSmoothedVoltage();
        currentPercentage = voltageToPercentage(currentVoltage);
        
        // Update charging status
        updateChargingStatus();
        
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

float BatteryManager::readRawVoltage() {
    // Read ADC value
    int adcValue = analogRead(BATTERY_ADC_PIN);
    
    // Convert to voltage (accounting for voltage divider)
    float voltage = (adcValue / (float)ADC_RESOLUTION) * ADC_REFERENCE_VOLTAGE * VOLTAGE_DIVIDER_RATIO;
    
    // Apply calibration offset
    voltage += CALIBRATION_OFFSET;
    
    return voltage;
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
    
    // Linear interpolation for now (can be made more sophisticated)
    float percentage = ((voltage - BATTERY_MIN_VOLTAGE) / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE)) * 100.0;
    
    // Apply a more realistic discharge curve for Li-ion batteries
    // This gives a more accurate representation of actual charge levels
    if (voltage > 3.9) {
        // Upper 80-100% range (4.2V to 3.9V)
        percentage = 80.0 + ((voltage - 3.9) / (BATTERY_MAX_VOLTAGE - 3.9)) * 20.0;
    } else if (voltage > 3.7) {
        // Middle 30-80% range (3.9V to 3.7V)
        percentage = 30.0 + ((voltage - 3.7) / (3.9 - 3.7)) * 50.0;
    } else if (voltage > 3.4) {
        // Lower 10-30% range (3.7V to 3.4V)
        percentage = 10.0 + ((voltage - 3.4) / (3.7 - 3.4)) * 20.0;
    } else {
        // Critical 0-10% range (3.4V to 3.0V)
        percentage = ((voltage - BATTERY_MIN_VOLTAGE) / (3.4 - BATTERY_MIN_VOLTAGE)) * 10.0;
    }
    
    return constrain(percentage, 0.0, 100.0);
}

void BatteryManager::updateChargingStatus() {
    // Read charging pin (assuming active LOW when charging)
    // Adjust this logic based on your charging circuit
    isCharging = !digitalRead(CHARGING_PIN);
}

String BatteryManager::getBatteryStatusString() const {
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

void BatteryManager::printBatteryInfo() const {
    Serial.println("\n--- Battery Information ---");
    Serial.println("Voltage: " + String(currentVoltage, 3) + "V");
    Serial.println("Percentage: " + String(currentPercentage, 1) + "%");
    Serial.println("Charging: " + String(isCharging ? "Yes" : "No"));
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
    Serial.println("ADC Raw: " + String(analogRead(BATTERY_ADC_PIN)));
    Serial.println("Charging Pin State: " + String(digitalRead(CHARGING_PIN)));
    Serial.println("---------------------------\n");
}

void BatteryManager::calibrate(float actualVoltage) {
    float measuredVoltage = readRawVoltage() - CALIBRATION_OFFSET;
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