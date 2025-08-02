#ifndef BUTTONS_H
#define BUTTONS_H

#include <Arduino.h>
#include <functional>

/**
 * ButtonController class for managing 4 hardware buttons with proper debouncing and event handling.
 * 
 * Hardware Configuration:
 * - Each button has a 390Ω pull-up resistor to 3.3V
 * - 100nF capacitor for hardware debouncing 
 * - 10kΩ pull-down resistor to ground
 * - Button connects junction point to 3.3V when pressed
 * - GPIO reads HIGH when pressed, LOW when not pressed
 * - No internal pull-ups needed
 */
class ButtonController {
public:
    // Button identifiers
    enum ButtonId {
        BUTTON_1 = 0,
        BUTTON_2 = 1,
        BUTTON_3 = 2,
        BUTTON_4 = 3,
        MAX_BUTTONS = 4
    };

    // Button event types
    enum ButtonEvent {
        SINGLE_CLICK,
        HOLD_START,
        HOLD_CONTINUOUS,
        HOLD_END,
        COMBO_HOLD_START,
        COMBO_HOLD_END
    };

    // Callback function types
    using ButtonCallback = std::function<void(ButtonId button)>;
    using HoldCallback = std::function<void(ButtonId button, unsigned long holdDuration)>;
    using ComboCallback = std::function<void()>;

    // Singleton pattern
    static ButtonController& getInstance();

    // Initialization
    void begin();
    void update(); // Call this in main loop

    // Callback registration
    void onSingleClick(ButtonCallback callback);
    void onHoldStart(HoldCallback callback);
    void onHoldContinuous(HoldCallback callback);
    void onHoldEnd(HoldCallback callback);
    void onComboHold(ComboCallback callback);

    // Configuration
    void setDebounceTime(unsigned long debounceMs);
    void setHoldThreshold(unsigned long holdMs);
    void setHoldInterval(unsigned long intervalMs);
    void setComboHoldTime(unsigned long comboMs);

    // Utility functions
    bool isPressed(ButtonId button);
    bool isHolding(ButtonId button);
    bool isComboActive();

private:
    // Pin definitions
    static const int BUTTON_PINS[MAX_BUTTONS];
    
    // Timing constants (can be configured)
    unsigned long debounceTime = 20;        // 20ms debounce (reduced due to hardware debouncing)
    unsigned long holdThreshold = 500;      // 500ms to trigger hold
    unsigned long holdInterval = 100;       // 100ms between hold callbacks
    unsigned long comboHoldTime = 10000;    // 10 seconds for combo

    // Button state structure
    struct ButtonState {
        bool currentState = false;
        bool lastState = false;
        bool debounceState = false;
        unsigned long lastDebounceTime = 0;
        unsigned long pressStartTime = 0;
        unsigned long lastHoldTime = 0;
        bool isHolding = false;
        bool singleClickPending = false;
        bool processed = false;
    };

    ButtonState buttons[MAX_BUTTONS];
    
    // Combo state
    bool comboActive = false;
    unsigned long comboStartTime = 0;
    bool comboProcessed = false;

    // Callbacks
    ButtonCallback singleClickCallback = nullptr;
    HoldCallback holdStartCallback = nullptr;
    HoldCallback holdContinuousCallback = nullptr;
    HoldCallback holdEndCallback = nullptr;
    ComboCallback comboHoldCallback = nullptr;

    // Singleton constructor
    ButtonController() = default;
    ButtonController(const ButtonController&) = delete;
    ButtonController& operator=(const ButtonController&) = delete;

    // Internal methods
    void handleButtonPress(ButtonId button);
    void handleButtonRelease(ButtonId button);
    void updateButton(ButtonId button);
    void updateCombo();
    void resetButton(ButtonId button);
    void resetCombo();
    bool readButtonRaw(ButtonId button);
    
    // Interrupt handlers (static methods)
    static void IRAM_ATTR handleInterrupt();
    static volatile bool interruptFlag;
};

#endif // BUTTONS_H