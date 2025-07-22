#ifndef REQUEST_MANAGER_H
#define REQUEST_MANAGER_H

#include <ConfigManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>

class RequestManager
{
private:
    HTTPClient http;
    String baseUrl;
    String authToken;
    int timeout;

    // Private helper methods
    bool isWiFiConnected();
    void setDefaultHeaders();
    JsonDocument parseResponse(String response);

    // Private constructor for singleton
    RequestManager(const String &baseUrl = "https://your-laravel-api.com/api");

    // Delete copy constructor and assignment operator
    RequestManager(const RequestManager&) = delete;
    RequestManager& operator=(const RequestManager&) = delete;

public:
    // Singleton instance getter
    static RequestManager& getInstance(const String &baseUrl = "https://your-laravel-api.com/api");

    // Destructor
    ~RequestManager();

    // Initialization
    bool begin();

    // Configuration methods
    void setBaseUrl(const String& url);
    void setAuthToken(const String& token);
    void setTimeout(int timeoutMs);

    // HTTP Methods
    // HTTP Methods
    JsonDocument get(const String& endpoint);
    JsonDocument post(const String& endpoint, const JsonDocument& data);

    // Utility methods
    bool isConnected();
    String getLastError();
    int getLastStatusCode();
    String getJWTToken();
    bool validateToken(String token);
    void initSecureConnection();

private:
    String lastError;
    int lastStatusCode;
};

#endif // REQUEST_MANAGER_H