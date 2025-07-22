// ReverbClient.h
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h> // Required for the insecure client
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h> // links2004/WebSockets v2.6.1

class ReverbClient {
public:
  static ReverbClient& getInstance() {
    static ReverbClient inst;
    return inst;
  }

  void onChatMessage(std::function<void(const String&)> cb) {
    _chatCb = cb;
  }

  void begin(
    const char* host,
    uint16_t    port,
    const char* appKey,
    const char* authToken,
    const char* deviceId
  ) {
    if (!WiFi.isConnected()) {
      Serial.println(F("⚠️ WiFi not connected! Call begin() only after your WiFi class connects."));
      return;
    }

    _host      = host;
    _port      = port;
    _appKey    = appKey;
    _authToken = authToken;
    _deviceId  = deviceId;

    // Initialize the WebSocketsClient pointer
    _ws = new WebSocketsClient();

    instance = this;
    _ws->onEvent(webSocketEvent);
    _ws->setReconnectInterval(5000);

    String path = String("/app/") + _appKey + "?protocol=7&client=esp32-client&version=1.0";

    // --- FIX: Use the standard beginSSL method ---
    // The library will manage its own secure client internally.
    _ws->beginSSL(_host.c_str(), _port, path.c_str());
    
    Serial.println(F("🔌 ReverbClient: connecting to WebSocket server..."));
  }

  void update() {
    _ws->loop();
  }

  bool sendMessage(const String& text) {
    if (!WiFi.isConnected()) {
      Serial.println(F("⚠️ Cannot send message, WiFi is disconnected."));
      return false;
    }

    // Use an insecure client for the HTTP request to save memory
    WiFiClientSecure insecureClient;
    insecureClient.setInsecure();

    HTTPClient http;
    String url = "https://" + _host + "/api/chat/device/" + _deviceId;
    
    if (http.begin(insecureClient, url)) {
      http.addHeader("Authorization", "Bearer " + _authToken);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Accept", "application/json");

      StaticJsonDocument<128> doc;
      doc["text"] = text;
      String payload;
      serializeJson(doc, payload);

      Serial.printf("Sending message to API: %s\n", text.c_str());
      int httpCode = http.POST(payload);
      http.end();

      if (httpCode == 200) {
        Serial.println(F("✅ Message sent successfully."));
        return true;
      } else {
        Serial.printf("⚠️ Failed to send message, HTTP Error: %d\n", httpCode);
        return false;
      }
    } else {
      Serial.println(F("⚠️ HTTP client connection failed!"));
      return false;
    }
  }


private:
  ReverbClient() = default;

  WebSocketsClient* _ws;
  // WiFiClientSecure* _wifiClientSecureForWS; // No longer needed
  String _host, _appKey, _authToken, _deviceId, _socketId;
  uint16_t _port;
  std::function<void(const String&)> _chatCb;

  static ReverbClient* instance;

  static void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (instance) instance->handleEvent(type, payload, length);
  }

  void handleEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
      case WStype_DISCONNECTED:
        Serial.println(F("⚠️ WebSocket Disconnected!"));
        break;

      case WStype_CONNECTED:
        Serial.println(F("🔗 WebSocket Connected to server. Waiting for handshake..."));
        break;

      case WStype_TEXT: {
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, payload, length);
        if (error) {
          Serial.print(F("⚠️ JSON parsing failed: "));
          Serial.println(error.c_str());
          return;
        }

        const char* eventName = doc["event"];
        if (!eventName) return;

        if (strcmp(eventName, "pusher:connection_established") == 0) {
          const char* socketData = doc["data"];
          StaticJsonDocument<256> socketDoc;
          deserializeJson(socketDoc, socketData);
          _socketId = socketDoc["socket_id"].as<String>();

          Serial.printf("🤝 Handshake complete. Socket ID: %s. Subscribing to presence channel...\n", _socketId.c_str());
          subscribeToPresence();
        }
        else if (strcmp(eventName, "chat-message") == 0 && _chatCb) {
          const char* chatDataStr = doc["data"];
          StaticJsonDocument<256> chatDoc;
          deserializeJson(chatDoc, chatDataStr);

          const char* text = chatDoc["text"];
          const char* senderName = chatDoc["user"]["name"];

          Serial.printf("📨 Message from '%s': %s\n", senderName, text);
          _chatCb(String(text));
        }
        break;
      }
      
      default:
        break;
    }
  }

  bool subscribeToPresence() {
    if (_socketId.length() == 0) {
      Serial.println(F("⚠️ Cannot subscribe, socket_id is missing."));
      return false;
    }

    // Use an insecure client for the HTTP request to save memory
    WiFiClientSecure insecureClient;
    insecureClient.setInsecure();

    HTTPClient http;
    String url = "https://" + _host + "/broadcasting/auth";

    if (http.begin(insecureClient, url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", "Bearer " + _authToken);
      http.addHeader("X-Client-Source", "esp32");
      
      StaticJsonDocument<128> authPayloadDoc;
      authPayloadDoc["socket_id"] = _socketId;
      authPayloadDoc["channel_name"] = "presence-device." + _deviceId;

      String authPayload;
      serializeJson(authPayloadDoc, authPayload);

      int httpCode = http.POST(authPayload);
      if (httpCode != 200) {
        Serial.printf("⚠️ Presence channel auth failed! HTTP Code: %d\n", httpCode);
        if(httpCode > 0) {
          String response = http.getString();
          Serial.println("Response: " + response);
        }
        http.end();
        return false;
      }

      StaticJsonDocument<512> authRespDoc;
      DeserializationError error = deserializeJson(authRespDoc, http.getString());
      http.end();
      if (error) {
        Serial.println(F("⚠️ Failed to parse auth response."));
        return false;
      }

      StaticJsonDocument<512> subMsgDoc;
      subMsgDoc["event"] = "pusher:subscribe";
      JsonObject data = subMsgDoc.createNestedObject("data");
      data["auth"] = authRespDoc["auth"].as<String>();
      data["channel"] = "presence-device." + _deviceId;
      data["channel_data"] = authRespDoc["channel_data"];

      String subFrame;
      serializeJson(subMsgDoc, subFrame);
      _ws->sendTXT(subFrame);

      Serial.println("✅ Successfully subscribed to presence-device." + _deviceId);
      return true;
    } else {
      Serial.println(F("⚠️ Auth HTTP client connection failed!"));
      return false;
    }
  }
};

ReverbClient* ReverbClient::instance = nullptr;
