# RequestManager

A comprehensive HTTP request manager for ESP32 projects that communicates with Laravel APIs using JSON format.

## Features

- **HTTP Methods**: Supports GET, POST, PUT, PATCH, and DELETE requests
- **JSON Support**: Automatic JSON parsing and serialization using ArduinoJson
- **Authentication**: Built-in Bearer token authentication support
- **Error Handling**: Comprehensive error handling with detailed error messages
- **WiFi Check**: Automatic WiFi connection validation before making requests
- **Configurable**: Customizable base URL, timeout, and authentication tokens
- **Laravel Ready**: Designed specifically for Laravel API integration

## Dependencies

- HTTPClient (ESP32 Core)
- ArduinoJson (v7.4.2+)
- WiFi (ESP32 Core)

## Installation

The RequestManager is already included in your project. Make sure you have the ArduinoJson library installed in your `platformio.ini`:

```ini
lib_deps = 
    bblanchon/ArduinoJson@^7.4.2
```

## Usage

### Basic Setup (Singleton Pattern)

```cpp
#include "RequestManager.h"

// Get the singleton instance
RequestManager& requestManager = RequestManager::getInstance("https://your-laravel-api.com/api");
```

### Configuration

```cpp
// Set authentication token
requestManager.setAuthToken("your-jwt-token-here");

// Set custom timeout (default: 10 seconds)
requestManager.setTimeout(15000); // 15 seconds

// Change base URL
requestManager.setBaseUrl("https://new-api.com/api");

// Initialize secure connection (validates/fetches JWT token automatically)
requestManager.initSecureConnection();
```

### Making Requests

#### GET Request
```cpp
JsonDocument response = requestManager.get("/users");
if (response["error"]) {
    Serial.println("Error: " + response["message"].as<String>());
} else {
    // Handle successful response
    serializeJsonPretty(response, Serial);
}
```

#### POST Request
```cpp
JsonDocument data;
data["name"] = "John Doe";
data["email"] = "john@example.com";

JsonDocument response = requestManager.post("/users", data);
if (response["error"]) {
    Serial.println("Error: " + response["message"].as<String>());
} else {
    // Handle successful response
    serializeJsonPretty(response, Serial);
}
```

#### PUT Request
```cpp
JsonDocument data;
data["name"] = "Jane Doe";
data["email"] = "jane@example.com";

JsonDocument response = requestManager.put("/users/1", data);
```

#### PATCH Request
```cpp
JsonDocument data;
data["name"] = "Updated Name";

JsonDocument response = requestManager.patch("/users/1", data);
```

#### DELETE Request
```cpp
JsonDocument response = requestManager.del("/users/1");
```

### Error Handling

The RequestManager provides comprehensive error handling:

```cpp
// Check if the request was successful
if (response["error"]) {
    String errorMessage = response["message"].as<String>();
    Serial.println("Request failed: " + errorMessage);
    
    // Get additional error information
    int statusCode = requestManager.getLastStatusCode();
    String lastError = requestManager.getLastError();
    
    Serial.println("Status Code: " + String(statusCode));
    Serial.println("Last Error: " + lastError);
}
```

### JWT Token Management

The RequestManager includes built-in JWT token management:

```cpp
// Initialize secure connection - this will:
// 1. Check if a stored token exists and is valid
// 2. If not, fetch a new token from your Laravel API
// 3. Store the token for future use
requestManager.initSecureConnection();

// Manually get a new JWT token
String token = requestManager.getJWTToken();
if (token.length() > 0) {
    Serial.println("New token obtained: " + token);
}

// Validate an existing token
bool isValid = requestManager.validateToken("your-token-here");
if (isValid) {
    Serial.println("Token is valid");
}
```

### Utility Methods

```cpp
// Check WiFi connection status
if (requestManager.isConnected()) {
    Serial.println("WiFi is connected");
}

// Get last HTTP status code
int statusCode = requestManager.getLastStatusCode();

// Get last error message
String error = requestManager.getLastError();
```

## Laravel API Integration

This RequestManager is designed to work seamlessly with Laravel APIs. It automatically sets the following headers:

- `Content-Type: application/json`
- `Accept: application/json`
- `Authorization: Bearer {token}` (when token is set)

### Laravel API Response Format

The RequestManager expects Laravel API responses in JSON format. A typical Laravel API response should look like:

```json
{
    "success": true,
    "data": {
        "id": 1,
        "name": "John Doe",
        "email": "john@example.com"
    },
    "message": "User created successfully"
}
```

For error responses:
```json
{
    "success": false,
    "message": "Validation failed",
    "errors": {
        "email": ["The email field is required."]
    }
}
```

## Error Types

The RequestManager handles various types of errors:

1. **WiFi Connection Errors**: When WiFi is not connected
2. **HTTP Errors**: When HTTP requests fail (network issues, server errors)
3. **JSON Parsing Errors**: When the response is not valid JSON
4. **Authentication Errors**: When authentication fails (401 status)

## Best Practices

1. **Always check for errors** before processing the response
2. **Set appropriate timeouts** based on your API response times
3. **Handle authentication tokens securely** - don't hardcode them
4. **Use appropriate HTTP methods** for different operations
5. **Monitor status codes** for debugging purposes
6. **Implement retry logic** for network failures if needed

## Example Integration with Laravel

```cpp
// Laravel login endpoint
JsonDocument loginData;
loginData["email"] = "user@example.com";
loginData["password"] = "password";

JsonDocument loginResponse = requestManager->post("/auth/login", loginData);
if (!loginResponse["error"]) {
    String token = loginResponse["data"]["access_token"];
    requestManager->setAuthToken(token);
    Serial.println("Login successful!");
}
```

## Memory Management

The RequestManager uses a singleton pattern and ArduinoJson's `JsonDocument` which automatically manages memory. Benefits of the singleton pattern:

1. **Single instance**: Only one RequestManager instance exists throughout the application lifecycle
2. **No manual cleanup**: The singleton is automatically destroyed when the program ends
3. **Consistent state**: All parts of your application share the same RequestManager state
4. **Memory efficient**: Avoids creating multiple instances unnecessarily

For large responses, consider:

1. Using `StaticJsonDocument` for fixed-size responses
2. Processing responses in chunks for very large datasets
3. The singleton instance persists throughout the application, so be mindful of stored tokens and state

## Thread Safety

The RequestManager singleton is not thread-safe. If you need to use it in a multi-threaded environment, implement appropriate synchronization mechanisms around the singleton instance.
