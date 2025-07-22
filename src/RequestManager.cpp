#include "RequestManager.h"

// Singleton instance getter
RequestManager &RequestManager::getInstance(const String &baseUrl)
{
    static RequestManager instance(baseUrl);
    return instance;
}

// Constructor
RequestManager::RequestManager(const String &baseUrl)
{
    this->baseUrl = baseUrl;
    this->timeout = 10000; // Default 10 seconds timeout
    this->lastStatusCode = 0;
    this->lastError = "";
}

// Destructor
RequestManager::~RequestManager()
{
    http.end();
}

// Initialization
bool RequestManager::begin()
{
    Serial.println("Initializing RequestManager...");
    initSecureConnection();

    if (authToken.length() > 0)
    {
        Serial.println("RequestManager initialized successfully with authentication token");
        return true;
    }
    else
    {
        Serial.println("RequestManager initialized but no authentication token available");
        return false;
    }
}

// Configuration methods
void RequestManager::setBaseUrl(const String &url)
{
    this->baseUrl = url;
}

void RequestManager::setAuthToken(const String &token)
{
    this->authToken = token;
}

void RequestManager::setTimeout(int timeoutMs)
{
    this->timeout = timeoutMs;
}

// Private helper methods
bool RequestManager::isWiFiConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

void RequestManager::setDefaultHeaders()
{
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");

    if (authToken.length() > 0)
    {
        http.addHeader("Authorization", "Bearer " + authToken);
    }
}

JsonDocument RequestManager::parseResponse(String response)
{
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error)
    {
        lastError = "JSON parsing error: " + String(error.c_str());
        JsonDocument errorDoc;
        errorDoc["error"] = true;
        errorDoc["message"] = lastError;
        return errorDoc;
    }

    return doc;
}

// HTTP Methods
JsonDocument RequestManager::get(const String &endpoint)
{
    JsonDocument emptyDoc;

    if (!isWiFiConnected())
    {
        lastError = "WiFi not connected";
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }

    // Check if we have an auth token, if not try to get one
    if (authToken.length() == 0)
    {
        Serial.println("No auth token available, attempting to authenticate...");
        initSecureConnection();
        if (authToken.length() == 0)
        {
            lastError = "Authentication failed - no token available";
            emptyDoc["error"] = true;
            emptyDoc["message"] = lastError;
            return emptyDoc;
        }
    }

    String url = baseUrl + endpoint;
    http.begin(url);
    http.setTimeout(timeout);
    setDefaultHeaders();

    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        return parseResponse(response);
    }
    else
    {
        lastError = "HTTP GET failed with code: " + String(httpResponseCode);
        http.end();
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }
}

JsonDocument RequestManager::post(const String &endpoint, const JsonDocument &data)
{
    JsonDocument emptyDoc;

    if (!isWiFiConnected())
    {
        lastError = "WiFi not connected";
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }

    // Check if we have an auth token, if not try to get one
    if (authToken.length() == 0)
    {
        Serial.println("No auth token available, attempting to authenticate...");
        initSecureConnection();
        if (authToken.length() == 0)
        {
            lastError = "Authentication failed - no token available";
            emptyDoc["error"] = true;
            emptyDoc["message"] = lastError;
            return emptyDoc;
        }
    }

    String url = baseUrl + endpoint;
    http.begin(url);
    http.setTimeout(timeout);
    setDefaultHeaders();

    String jsonString;
    serializeJson(data, jsonString);

    int httpResponseCode = http.POST(jsonString);
    lastStatusCode = httpResponseCode;

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        return parseResponse(response);
    }
    else
    {
        lastError = "HTTP POST failed with code: " + String(httpResponseCode);
        http.end();
        emptyDoc["error"] = true;
        emptyDoc["message"] = lastError;
        return emptyDoc;
    }
}

// Utility methods
bool RequestManager::isConnected()
{
    return isWiFiConnected();
}

String RequestManager::getLastError()
{
    return lastError;
}

int RequestManager::getLastStatusCode()
{
    return lastStatusCode;
}

String RequestManager::getJWTToken()
{
    // send get request to BASE_URL + "/{ESP.getEfuseMac()}/token"
    String url = baseUrl + "/hubs/" + String(ESP.getEfuseMac()) + "/token";
    http.begin(url);
    http.setTimeout(timeout);
    setDefaultHeaders();

    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    // response is formatted as below
    //  return response()->json([
    //              'token'   => $token,
    //              'status'  => 'success',
    //              'message' => 'Token generated successfully.'
    //          ]);
    // or
    //      return response()->json([
    //          'token'   => null,
    //          'status'  => 'error',
    //          'message' => 'No user is bound to this hub.'], 404);
    //  }

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        http.end();
        JsonDocument doc = parseResponse(response);
        // check if status is success
        if (doc["status"] == "success")
        {
            Serial.println("JWT token obtained successfully: " + doc["token"].as<String>());
            return doc["token"].as<String>();
        }
        else
        {
            lastError = doc["message"].as<String>();
            return "";
        }
    }
    else
    {
        lastError = "HTTP GET failed with code: " + String(httpResponseCode);
        http.end();
        Serial.println("HTTP Error: " + lastError);
        return "";
    }
}

bool RequestManager::validateToken(String token)
{
    // send get request to BASE_URL + "/validate-token"
    String url = baseUrl + "/hubs/" + String(ESP.getEfuseMac()) + "/validate-token";
    
    http.begin(url);
    http.setTimeout(timeout);
    setDefaultHeaders();

    // add Authorization header
    http.addHeader("Authorization", "Bearer " + token);

    int httpResponseCode = http.GET();
    lastStatusCode = httpResponseCode;

    if (httpResponseCode == 200)
    {
        http.end();
        return true;
    }
    else
    {
        lastError = "Token validation failed with code: " + String(httpResponseCode);
        http.end();
        return false;
    }
}

void RequestManager::initSecureConnection()
{
    ConfigManager &config = ConfigManager::getInstance();
    String storedToken = config.getJWTToken();

    bool needNewToken = true;

    if (storedToken.length() > 0)
    {
        if (validateToken(storedToken))
        {
            Serial.println("Using stored JWT token: " + storedToken);
            needNewToken = false;
        }
        else
        {
            config.setJWTToken("");
        }
    }

    if (needNewToken)
    {
        storedToken = getJWTToken();
        if (storedToken.length() > 0)
        {
            Serial.println("New JWT token obtained: " + storedToken);
            config.setJWTToken(storedToken);
        }
        else
        {
            setAuthToken("");
            return;
        }
    }

    setAuthToken(storedToken);
}
