#include "Buttons.h"

// Static member definitions
const int ButtonController::BUTTON_PINS[MAX_BUTTONS] = {12, 13, 14, 21}; // GPIO pins for buttons 1-4 (ESP32-S3)
volatile bool ButtonController::interruptFlag = false;

ButtonController& ButtonController::getInstance() {
    static ButtonController instance;
    return instance;
}

void ButtonController::begin() {
    // Configure button pins - no pull-up needed due to external pull-up circuit
    for (int i = 0; i < MAX_BUTTONS; i++) {
        pinMode(BUTTON_PINS[i], INPUT);
        
        // Initialize button states
        buttons[i] = ButtonState();
        buttons[i].currentState = readButtonRaw(static_cast<ButtonId>(i));
        buttons[i].lastState = buttons[i].currentState;
        buttons[i].debounceState = buttons[i].currentState;
    }

    // Note: Using polling instead of interrupts for better reliability across all GPIO pins
    Serial.println("[BUTTONS] Button controller initialized (polling mode)");
}

void ButtonController::update() {
    // Always update all buttons (polling mode)
    for (int i = 0; i < MAX_BUTTONS; i++) {
        updateButton(static_cast<ButtonId>(i));
    }
    
    // Update combo logic
    updateCombo();
    
    // Always check for ongoing hold events and combo timing
    unsigned long currentTime = millis();
    
    // Process hold continuous callbacks
    for (int i = 0; i < MAX_BUTTONS; i++) {
        ButtonState& btn = buttons[i];
        if (btn.isHolding && !comboActive && !combo2Active) {
            if (currentTime - btn.lastHoldTime >= holdInterval) {
                btn.lastHoldTime = currentTime;
                if (holdContinuousCallback) {
                    holdContinuousCallback(static_cast<ButtonId>(i), currentTime - btn.pressStartTime);
                }
            }
        }
    }
    
    // Check combo hold completion (Button 1 + 4)
    if (comboActive && !comboProcessed) {
        if (currentTime - comboStartTime >= comboHoldTime) {
            comboProcessed = true;
            if (comboHoldCallback) {
                comboHoldCallback();
            }
        }
    }
    
    // Check combo2 hold completion (Button 2 + 3)
    if (combo2Active && !combo2Processed) {
        if (currentTime - combo2StartTime >= comboHoldTime) {
            combo2Processed = true;
            if (comboHold2Callback) {
                comboHold2Callback();
            }
        }
    }
}

void ButtonController::updateButton(ButtonId button) {
    unsigned long currentTime = millis();
    ButtonState& btn = buttons[button];

    bool currentReading = readButtonRaw(button);
    
    // Debounce logic
    if (currentReading != btn.lastState) {
        btn.lastDebounceTime = currentTime;
    }
    
    if ((currentTime - btn.lastDebounceTime) > debounceTime) {
        if (currentReading != btn.debounceState) {
            btn.debounceState = currentReading;
            
            // State change detected
            if (btn.debounceState != btn.currentState) {
                btn.currentState = btn.debounceState;
                
                if (btn.currentState) {
                    // Button pressed
                    handleButtonPress(button);
                } else {
                    // Button released
                    handleButtonRelease(button);
                }
            }
        }
    }
    
    btn.lastState = currentReading;
}

void ButtonController::handleButtonPress(ButtonId button) {
    unsigned long currentTime = millis();
    ButtonState& btn = buttons[button];
    
    btn.pressStartTime = currentTime;
    btn.singleClickPending = true;
    btn.processed = false;
    
    Serial.printf("[BUTTONS] Button %d pressed\n", button + 1);
}

void ButtonController::handleButtonRelease(ButtonId button) {
    unsigned long currentTime = millis();
    ButtonState& btn = buttons[button];
    unsigned long pressDuration = currentTime - btn.pressStartTime;
    
    Serial.printf("[BUTTONS] Button %d released (held for %lu ms)\n", button + 1, pressDuration);
    
    // Handle hold end
    if (btn.isHolding) {
        btn.isHolding = false;
        if (holdEndCallback && !comboActive && !combo2Active) {
            holdEndCallback(button, pressDuration);
        }
    }
    
    // Handle single click (only if not part of combo and wasn't holding)
    if (btn.singleClickPending && !comboActive && !combo2Active && pressDuration < holdThreshold) {
        btn.singleClickPending = false;
        if (singleClickCallback) {
            singleClickCallback(button);
        }
    }
    
    btn.singleClickPending = false;
    
    // Check if this release affects combo state (Button 1 + 4)
    if (comboActive && (button == BUTTON_1 || button == BUTTON_4)) {
        bool button1Pressed = buttons[BUTTON_1].currentState;
        bool button4Pressed = buttons[BUTTON_4].currentState;
        
        if (!button1Pressed && !button4Pressed) {
            // Both combo buttons released
            resetCombo();
        }
    }
    
    // Check if this release affects combo2 state (Button 2 + 3)
    if (combo2Active && (button == BUTTON_2 || button == BUTTON_3)) {
        bool button2Pressed = buttons[BUTTON_2].currentState;
        bool button3Pressed = buttons[BUTTON_3].currentState;
        
        if (!button2Pressed && !button3Pressed) {
            // Both combo2 buttons released
            resetCombo2();
        }
    }
    
    // Start hold detection timer (non-blocking)
    if (pressDuration >= holdThreshold && !comboActive && !combo2Active) {
        // This was a hold, not a click
        btn.singleClickPending = false;
    }
}

void ButtonController::updateCombo() {
    unsigned long currentTime = millis();
    bool button1Pressed = buttons[BUTTON_1].currentState;
    bool button3Pressed = buttons[BUTTON_3].currentState;
    bool button4Pressed = buttons[BUTTON_4].currentState;
    
    // Check for combo start
    if (!comboActive && button1Pressed && button4Pressed) {
        // Both buttons pressed, check if they were pressed recently
        unsigned long btn1PressTime = buttons[BUTTON_1].pressStartTime;
        unsigned long btn4PressTime = buttons[BUTTON_4].pressStartTime;
        
        // Allow some tolerance for simultaneous press (within 200ms)
        if (abs((long)(btn1PressTime - btn4PressTime)) <= 200) {
            comboActive = true;
            comboStartTime = max(btn1PressTime, btn4PressTime);
            comboProcessed = false;
            
            // Cancel individual button events
            buttons[BUTTON_1].singleClickPending = false;
            buttons[BUTTON_4].singleClickPending = false;
            buttons[BUTTON_1].processed = true;
            buttons[BUTTON_4].processed = true;
            
            Serial.println("[BUTTONS] Combo hold started (Button 1 + 4)");
        }
    }
    
    // Check for combo2 start (Button 2 + 3)
    bool button2Pressed = buttons[BUTTON_2].currentState;
    
    if (!combo2Active && button2Pressed && button3Pressed) {
        unsigned long btn2PressTime = buttons[BUTTON_2].pressStartTime;
        unsigned long btn3PressTime = buttons[BUTTON_3].pressStartTime;
        
        // Allow some tolerance for simultaneous press (within 200ms)
        if (abs((long)(btn2PressTime - btn3PressTime)) <= 200) {
            combo2Active = true;
            combo2StartTime = max(btn2PressTime, btn3PressTime);
            combo2Processed = false;
            
            // Cancel individual button events
            buttons[BUTTON_2].singleClickPending = false;
            buttons[BUTTON_3].singleClickPending = false;
            buttons[BUTTON_2].processed = true;
            buttons[BUTTON_3].processed = true;
            
            Serial.println("[BUTTONS] Combo2 hold started (Button 2 + 3)");
        }
    }
    
    // Handle hold start for individual buttons (only if not in any combo)
    if (!comboActive && !combo2Active) {
        for (int i = 0; i < MAX_BUTTONS; i++) {
            ButtonState& btn = buttons[i];
            if (btn.currentState && !btn.isHolding && !btn.processed) {
                if (currentTime - btn.pressStartTime >= holdThreshold) {
                    btn.isHolding = true;
                    btn.lastHoldTime = currentTime;
                    btn.singleClickPending = false; // Cancel single click
                    
                    if (holdStartCallback) {
                        holdStartCallback(static_cast<ButtonId>(i), currentTime - btn.pressStartTime);
                    }
                    
                    Serial.printf("[BUTTONS] Button %d hold started\n", i + 1);
                }
            }
        }
    }
}

void ButtonController::resetButton(ButtonId button) {
    ButtonState& btn = buttons[button];
    btn.isHolding = false;
    btn.singleClickPending = false;
    btn.processed = false;
}

void ButtonController::resetCombo() {
    if (comboActive) {
        Serial.println("[BUTTONS] Combo hold ended");
        comboActive = false;
        comboProcessed = false;
        
        // Reset combo buttons
        resetButton(BUTTON_1);
        resetButton(BUTTON_4);
    }
}

void ButtonController::resetCombo2() {
    if (combo2Active) {
        Serial.println("[BUTTONS] Combo2 hold ended");
        combo2Active = false;
        combo2Processed = false;
        
        // Reset combo2 buttons
        resetButton(BUTTON_2);
        resetButton(BUTTON_3);
        
    }
}

bool ButtonController::readButtonRaw(ButtonId button) {
    // Direct reading - HIGH = pressed, LOW = not pressed
    bool reading = digitalRead(BUTTON_PINS[button]);
    return reading;
}

// Callback registration methods
void ButtonController::onSingleClick(ButtonCallback callback) {
    singleClickCallback = callback;
}

void ButtonController::onHoldStart(HoldCallback callback) {
    holdStartCallback = callback;
}

void ButtonController::onHoldContinuous(HoldCallback callback) {
    holdContinuousCallback = callback;
}

void ButtonController::onHoldEnd(HoldCallback callback) {
    holdEndCallback = callback;
}

void ButtonController::onComboHold(ComboCallback callback) {
    comboHoldCallback = callback;
}

void ButtonController::onComboHold2(ComboCallback callback) {
    comboHold2Callback = callback;
}

// Configuration methods
void ButtonController::setDebounceTime(unsigned long debounceMs) {
    debounceTime = debounceMs;
}

void ButtonController::setHoldThreshold(unsigned long holdMs) {
    holdThreshold = holdMs;
}

void ButtonController::setHoldInterval(unsigned long intervalMs) {
    holdInterval = intervalMs;
}

void ButtonController::setComboHoldTime(unsigned long comboMs) {
    comboHoldTime = comboMs;
}

// Utility methods
bool ButtonController::isPressed(ButtonId button) {
    return buttons[button].currentState;
}

bool ButtonController::isHolding(ButtonId button) {
    return buttons[button].isHolding;
}

bool ButtonController::isComboActive() {
    return comboActive;
}

// Interrupt handler
void IRAM_ATTR ButtonController::handleInterrupt() {
    interruptFlag = true;
}