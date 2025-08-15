#pragma once

#include <Arduino.h>
#include <AsyncTCP.h>

/**
 * AsyncTCP Speed Test Module
 * 
 * A lean, production-ready speed test implementation using AsyncTCP library.
 * Optimized for maximum throughput with minimal overhead.
 * 
 * Features:
 * - Non-blocking async callbacks
 * - No String operations in critical paths
 * - Minimal memory allocations
 * - TCP optimizations for ESP32 LWIP stack
 * - Clean header parsing without buffers
 */
class AsyncSpeedTest {
public:
    // Constructor
    AsyncSpeedTest();
    
    // Destructor
    ~AsyncSpeedTest();
    
    /**
     * Start a speed test
     * @param host Target host (default: portal.tilkietalkie.com)
     * @param port Target port (default: 80)
     * @param path Target path (default: audio file)
     * @return true if test started successfully, false otherwise
     */
    bool start(const char* host = "portal.tilkietalkie.com", 
               uint16_t port = 80,
               const char* path = "/storage/tracks/audio/vIr8dNzNhQgWEpc9uceF1Wncljn5mSCRoYlXTsOU.wav");
    
    /**
     * Check if a test is currently active
     * @return true if test is running
     */
    bool isActive() const { return data.testActive; }
    
    /**
     * Stop current test if running
     */
    void stop();

private:
    // Test state data
    struct TestData {
        AsyncClient* client;
        unsigned long connectStart;
        unsigned long connectTime;
        unsigned long downloadStart;
        unsigned long totalBytes;
        unsigned long lastProgressTime;
        int contentLength;
        bool headersParsed;
        bool testActive;
        
        void reset() {
            client = nullptr;
            connectStart = 0;
            connectTime = 0;
            downloadStart = 0;
            totalBytes = 0;
            lastProgressTime = 0;
            contentLength = -1;
            headersParsed = false;
            testActive = false;
        }
    } data;
    
    // Test configuration
    const char* targetHost;
    uint16_t targetPort;
    const char* targetPath;
    
    // AsyncTCP callback functions
    static void onConnect(void* arg, AsyncClient* client);
    static void onError(void* arg, AsyncClient* client, int8_t error);
    static void onData(void* arg, AsyncClient* client, void* data, size_t len);
    static void onDisconnect(void* arg, AsyncClient* client);
    
    // Helper functions
    void cleanup();
    void printResults();
};

// Global instance for easy access
extern AsyncSpeedTest speedTest;
