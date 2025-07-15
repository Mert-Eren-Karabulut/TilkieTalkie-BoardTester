#include <Arduino.h>
#include "NfcController.h"

// Create NFC controller instance
NfcController nfcController;

// NFC callback functions
void onNFCCardDetected(const NFCData& nfcData) {
    Serial.println("=== NFC Card Detected ===");
    Serial.print("Card UID: ");
    Serial.println(nfcData.uidString);
    Serial.print("Card Type: 0x");
    Serial.println(nfcData.tagType, HEX);
    Serial.print("UID Length: ");
    Serial.println(nfcData.uidLength);
    Serial.print("Timestamp: ");
    Serial.println(nfcData.timestamp);
    
    // Example: Different actions based on card UID
    if (nfcData.uidString == "04:AB:CD:EF:12:34:56") {
        Serial.println("-> This is Card A - Playing audio file A");
        // audioController.play("/sounds/cardA.mp3");
        // ledController.simpleLed(0x0000FF, 255); // Blue for Card A
    } else if (nfcData.uidString == "04:12:34:56:AB:CD:EF") {
        Serial.println("-> This is Card B - Playing audio file B");
        // audioController.play("/sounds/cardB.mp3");
        // ledController.simpleLed(0x00FF00, 255); // Green for Card B
    } else {
        Serial.println("-> Unknown card - Playing default sound");
        // audioController.play("/sounds/default.mp3");
        // ledController.simpleLed(0xFF0000, 255); // Red for unknown card
    }
    
    Serial.println("========================");
}

void onNFCSessionEnded() {
    Serial.println("=== NFC Session Ended ===");
    Serial.println("Reed switch deactivated or card removed");
    
    // Example: Stop any playing audio
    // audioController.stop();
    
    // Turn off LED
    // ledController.turnOff();
    
    Serial.println("========================");
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("=== NFC Controller Example ===");
    
    // Initialize NFC controller
    Serial.println("Initializing NFC Controller...");
    if (nfcController.begin()) {
        Serial.println("NFC Controller initialized successfully!");
        
        // Set up callbacks
        nfcController.setAfterNFCReadCallback(onNFCCardDetected);
        nfcController.setAfterDetachNFCCallback(onNFCSessionEnded);
        
        // Configure NFC settings
        nfcController.setReadInterval(200);  // Read every 200ms
        nfcController.setDebounceDelay(50);  // 50ms debounce
        
        Serial.println("NFC callbacks configured");
        Serial.println("Place a card on the NFC reader and activate the reed switch (GPIO4)");
    } else {
        Serial.println("ERROR: NFC Controller initialization failed!");
        Serial.println("Check your connections:");
        Serial.println("- NFC_SDA -> GPIO22");
        Serial.println("- NFC_SCL -> GPIO21");
        Serial.println("- Reed Switch -> GPIO4");
    }
    
    Serial.println("=== Commands ===");
    Serial.println("status  - Show NFC status");
    Serial.println("data    - Show current card data");
    Serial.println("reed    - Show reed switch status");
    Serial.println("================");
}

void loop() {
    // Update NFC controller - this handles reed switch monitoring and card reading
    nfcController.update();
    
    // Handle serial commands
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        command.toLowerCase();
        
        if (command == "status") {
            Serial.println("\n--- NFC Status ---");
            Serial.println("NFC Ready: " + String(nfcController.isNFCReady() ? "Yes" : "No"));
            Serial.println("Reed Switch Active: " + String(nfcController.isReedSwitchActive() ? "Yes" : "No"));
            Serial.println("Card Present: " + String(nfcController.isCardPresent() ? "Yes" : "No"));
            Serial.println("------------------\n");
        }
        else if (command == "data") {
            NFCData currentCard = nfcController.currentNFCData();
            Serial.println("\n--- Current Card Data ---");
            if (currentCard.isValid) {
                nfcController.printNFCData(currentCard);
            } else {
                Serial.println("No valid card data available");
            }
            Serial.println("------------------------\n");
        }
        else if (command == "reed") {
            bool reedState = digitalRead(4);
            Serial.println("\n--- Reed Switch Status ---");
            Serial.println("Reed Switch Pin (GPIO4): " + String(reedState ? "HIGH" : "LOW"));
            Serial.println("Reed Switch Active: " + String(nfcController.isReedSwitchActive() ? "Yes" : "No"));
            Serial.println("-------------------------\n");
        }
        else if (command.length() > 0) {
            Serial.println("Unknown command. Available commands: status, data, reed");
        }
    }
    
    // Small delay to prevent overwhelming the serial output
    delay(10);
}
