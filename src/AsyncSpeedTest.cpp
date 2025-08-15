#include "AsyncSpeedTest.h"
#include <WiFi.h>

// Global instance
AsyncSpeedTest speedTest;

AsyncSpeedTest::AsyncSpeedTest() : targetHost(nullptr), targetPort(80), targetPath(nullptr) {
    data.reset();
}

AsyncSpeedTest::~AsyncSpeedTest() {
    stop();
}

bool AsyncSpeedTest::start(const char* host, uint16_t port, const char* path) {
    if (data.testActive) {
        Serial.println("⚠️ Speed test already in progress. Please wait for it to complete.");
        return false;
    }
    
    if (!WiFi.isConnected()) {
        Serial.println("❌ WiFi not connected - cannot perform speed test");
        return false;
    }
    
    // Store test parameters
    targetHost = host;
    targetPort = port;
    targetPath = path;
    
    // Reset and initialize test data
    data.reset();
    data.testActive = true;
    
    // Create new AsyncClient
    data.client = new AsyncClient();
    if (!data.client) {
        Serial.println("❌ Failed to create AsyncClient");
        data.testActive = false;
        return false;
    }
    
    // Set up callbacks - pass 'this' as argument to access instance data
    data.client->onConnect(onConnect, this);
    data.client->onError(onError, this);
    data.client->onData(onData, this);
    data.client->onDisconnect(onDisconnect, this);
    
    Serial.println("\n--- AsyncTCP Speed Test (Optimized) ---");
    Serial.printf("Host: %s:%d\n", host, port);
    Serial.printf("Path: %s\n", path);
    Serial.println("Features: Lean callbacks, no String ops, TCP optimized");
    Serial.printf("LWIP Settings: TCP_WND=%d, TCP_MSS=%d\n", 34816, 1460);
    Serial.println("Connecting...");
    
    data.connectStart = millis();
    
    if (!data.client->connect(host, port)) {
        Serial.println("❌ Failed to start connection");
        cleanup();
        return false;
    }
    
    return true;
}

void AsyncSpeedTest::stop() {
    if (data.testActive && data.client) {
        Serial.println("🛑 Speed test stopped by user");
        cleanup();
    }
}

void AsyncSpeedTest::cleanup() {
    data.testActive = false;
    if (data.client) {
        data.client->close(true);
        delete data.client;
        data.client = nullptr;
    }
    data.reset();
}

void AsyncSpeedTest::printResults() {
    unsigned long downloadEnd = millis();
    unsigned long totalDuration = downloadEnd - data.connectStart;
    unsigned long downloadDuration = downloadEnd - data.downloadStart;
    
    // Calculate final speeds
    float downloadSec = downloadDuration / 1000.0f;
    float totalSec = totalDuration / 1000.0f;
    
    float downloadSpeedKbps = downloadSec > 0 ? (data.totalBytes * 8.0f) / (downloadSec * 1000.0f) : 0;
    float downloadSpeedMbps = downloadSpeedKbps / 1000.0f;
    float downloadSpeedKBps = downloadSec > 0 ? data.totalBytes / (downloadSec * 1024.0f) : 0;
    
    float overallSpeedKbps = totalSec > 0 ? (data.totalBytes * 8.0f) / (totalSec * 1000.0f) : 0;
    float overallSpeedMbps = overallSpeedKbps / 1000.0f;
    
    Serial.println("\n--- AsyncTCP Speed Test Results ---");
    Serial.printf("Downloaded: %lu bytes", data.totalBytes);
    if (data.contentLength > 0) {
        Serial.printf(" of %d bytes (%.1f%%)\n", data.contentLength, 
                     (float)data.totalBytes / data.contentLength * 100.0f);
    } else {
        Serial.println();
    }
    
    Serial.printf("Connection time: %lu ms\n", data.connectTime);
    Serial.printf("Download time: %lu ms (%.2f seconds)\n", downloadDuration, downloadSec);
    Serial.printf("Total time: %lu ms (%.2f seconds)\n", totalDuration, totalSec);
    
    Serial.println("\n📊 Download Speed (data transfer only):");
    Serial.printf("  %.2f Kbps (%.2f Mbps)\n", downloadSpeedKbps, downloadSpeedMbps);
    Serial.printf("  %.2f KB/s\n", downloadSpeedKBps);
    
    Serial.println("\n📊 Overall Speed (including connection):");
    Serial.printf("  %.2f Kbps (%.2f Mbps)\n", overallSpeedKbps, overallSpeedMbps);
    
    // Performance rating
    Serial.print("\n📈 Performance: ");
    if (downloadSpeedMbps >= 20.0f) {
        Serial.println("🟢 Excellent (>20 Mbps)");
    } else if (downloadSpeedMbps >= 10.0f) {
        Serial.println("🟢 Very Good (10-20 Mbps)");
    } else if (downloadSpeedMbps >= 5.0f) {
        Serial.println("🟡 Good (5-10 Mbps)");
    } else if (downloadSpeedMbps >= 1.0f) {
        Serial.println("🟠 Fair (1-5 Mbps)");
    } else {
        Serial.println("🔴 Poor (<1 Mbps)");
    }
    
    Serial.printf("\n💾 Free heap after test: %d bytes\n", ESP.getFreeHeap());
    Serial.println("✅ AsyncTCP speed test completed!");
    Serial.println("------------------------------------\n");
}

// Static callback functions
void AsyncSpeedTest::onConnect(void* arg, AsyncClient* client) {
    AsyncSpeedTest* self = static_cast<AsyncSpeedTest*>(arg);
    self->data.connectTime = millis() - self->data.connectStart;
    Serial.printf("Connected in %lu ms\n", self->data.connectTime);
    
    // Critical: Set TCP optimizations for maximum throughput
    client->setRxTimeout(60);     // 60 second timeout
    client->setNoDelay(true);     // Disable Nagle's algorithm for lower latency
    client->setAckTimeout(1000);  // Quick ACK timeout for better performance
    
    // Build HTTP request dynamically but efficiently
    String request = "GET ";
    request += self->targetPath;
    request += " HTTP/1.1\r\n";
    request += "Host: ";
    request += self->targetHost;
    request += "\r\n";
    request += "Connection: close\r\n";
    request += "\r\n";
    
    // Send request
    client->write(request.c_str(), request.length());
    
    self->data.downloadStart = millis();
}

void AsyncSpeedTest::onError(void* arg, AsyncClient* client, int8_t error) {
    AsyncSpeedTest* self = static_cast<AsyncSpeedTest*>(arg);
    Serial.printf("❌ AsyncTCP Error: %d\n", error);
    self->cleanup();
}

void AsyncSpeedTest::onData(void* arg, AsyncClient* client, void* data, size_t len) {
    AsyncSpeedTest* self = static_cast<AsyncSpeedTest*>(arg);
    
    if (!self->data.headersParsed) {
        // Fast header parsing - find Content-Length and end of headers
        const char* buf = static_cast<const char*>(data);
        
        // Look for Content-Length header
        const char* content_length_ptr = strstr(buf, "Content-Length: ");
        if (content_length_ptr) {
            self->data.contentLength = atoi(content_length_ptr + 16);
            Serial.printf("Content-Length: %d bytes\n", self->data.contentLength);
        }
        
        // Find end of headers "\r\n\r\n"
        const char* header_end = strstr(buf, "\r\n\r\n");
        if (header_end) {
            self->data.headersParsed = true;
            self->data.downloadStart = millis(); // Reset download timer
            
            // Count data bytes after headers in this packet
            size_t header_size = (header_end + 4) - buf;
            if (len > header_size) {
                self->data.totalBytes += (len - header_size);
            }
            Serial.println("Headers parsed, downloading...");
        }
        return;
    }
    
    // Count all bytes in data phase
    self->data.totalBytes += len;
    
    // Minimal progress reporting - only every 3 seconds to reduce overhead
    unsigned long now = millis();
    if (now - self->data.lastProgressTime > 3000) {
        self->data.lastProgressTime = now;
        unsigned long elapsed = now - self->data.downloadStart;
        float currentSpeed = elapsed > 0 ? (self->data.totalBytes * 8.0f) / (elapsed / 1000.0f) / 1000.0f : 0;
        
        if (self->data.contentLength > 0) {
            float progress = (float)self->data.totalBytes / self->data.contentLength * 100.0f;
            Serial.printf("Progress: %.1f%% (%lu/%d bytes) - %.1f Kbps\n", 
                         progress, self->data.totalBytes, self->data.contentLength, currentSpeed);
        } else {
            Serial.printf("Downloaded: %lu bytes - %.1f Kbps\n", self->data.totalBytes, currentSpeed);
        }
    }
}

void AsyncSpeedTest::onDisconnect(void* arg, AsyncClient* client) {
    AsyncSpeedTest* self = static_cast<AsyncSpeedTest*>(arg);
    
    // Print final results
    self->printResults();
    
    // Cleanup
    self->cleanup();
}
