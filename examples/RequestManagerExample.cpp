#include "RequestManager.h"

void setupRequestManagerExample() {
    // Get the singleton instance of RequestManager with your Laravel API base URL
    RequestManager& requestManager = RequestManager::getInstance("https://your-laravel-api.com/api");
    
    // Optional: Set authentication token
    requestManager.setAuthToken("your-jwt-token-here");
    
    // Optional: Set custom timeout (default is 10 seconds)
    requestManager.setTimeout(15000); // 15 seconds
    
    // Initialize secure connection (validates/fetches JWT token)
    requestManager.initSecureConnection();
    
    // Example GET request
    JsonDocument getResponse = requestManager.get("/users");
    if (getResponse["error"]) {
        Serial.println("GET Error: " + getResponse["message"].as<String>());
    } else {
        Serial.println("GET Success!");
        serializeJsonPretty(getResponse, Serial);
    }
    
    // Example POST request
    JsonDocument postData;
    postData["name"] = "John Doe";
    postData["email"] = "john@example.com";
    postData["password"] = "securepassword";
    
    JsonDocument postResponse = requestManager.post("/users", postData);
    if (postResponse["error"]) {
        Serial.println("POST Error: " + postResponse["message"].as<String>());
    } else {
        Serial.println("POST Success!");
        serializeJsonPretty(postResponse, Serial);
    }
    
    // Example PUT request
    JsonDocument putData;
    putData["name"] = "Jane Doe";
    putData["email"] = "jane@example.com";
    
    JsonDocument putResponse = requestManager.put("/users/1", putData);
    if (putResponse["error"]) {
        Serial.println("PUT Error: " + putResponse["message"].as<String>());
    } else {
        Serial.println("PUT Success!");
        serializeJsonPretty(putResponse, Serial);
    }
    
    // Example PATCH request
    JsonDocument patchData;
    patchData["name"] = "Updated Name";
    
    JsonDocument patchResponse = requestManager.patch("/users/1", patchData);
    if (patchResponse["error"]) {
        Serial.println("PATCH Error: " + patchResponse["message"].as<String>());
    } else {
        Serial.println("PATCH Success!");
        serializeJsonPretty(patchResponse, Serial);
    }
    
    // Example DELETE request
    JsonDocument deleteResponse = requestManager.del("/users/1");
    if (deleteResponse["error"]) {
        Serial.println("DELETE Error: " + deleteResponse["message"].as<String>());
    } else {
        Serial.println("DELETE Success!");
        serializeJsonPretty(deleteResponse, Serial);
    }
    
    // Check connection status
    if (requestManager.isConnected()) {
        Serial.println("WiFi is connected");
    } else {
        Serial.println("WiFi is not connected");
    }
    
    // Get last error and status code
    Serial.println("Last Error: " + requestManager.getLastError());
    Serial.println("Last Status Code: " + String(requestManager.getLastStatusCode()));
    
    // Note: No need to delete the instance since it's a singleton
}

void loopRequestManagerExample() {
    // You can call API requests here in the loop if needed
    // Be careful about request frequency to avoid overwhelming your server
}
