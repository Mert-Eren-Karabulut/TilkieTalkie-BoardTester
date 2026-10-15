#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include "AudioFileSource.h"

// SD-backed audio source with a PSRAM ring that its own task keeps full, so the decoder
// (audio task) never waits on the card. The bench card pauses up to 3 s on writes; with
// 128 KB of read-ahead (about 10 s of 96 kbps audio) a pause only drains the ring.
class ReadAheadFileSource : public AudioFileSource {
public:
    ReadAheadFileSource(size_t ringBytes = 128 * 1024, size_t chunkBytes = 4 * 1024);
    ~ReadAheadFileSource() override;

    bool begin();  // allocate the buffers and start the reader task (once)

    bool open(const char* path) override;
    uint32_t read(void* data, uint32_t len) override;
    bool seek(int32_t pos, int dir) override;
    bool close() override;
    bool isOpen() override { return fileOpen.load(); }
    uint32_t getSize() override { return fileSize; }
    uint32_t getPos() override { return tail.load(); }

    uint32_t available() const { return head.load() - tail.load(); }
    uint32_t capacity() const { return ringSize; }
    uint32_t underruns() const { return underrunCount.load(); }
    bool atEof() const { return eof.load(); }

private:
    static void readerTaskEntry(void* arg);
    void readerLoop();
    // Park handshake. The reader owns the File only while it holds READER_BUSY; open/seek/close
    // run only after winning READER_IDLE -> READER_PARKED, so they never touch the File while
    // the reader is inside read(). parkReader() gives up after PARK_TIMEOUT_MS (a card stuck
    // for that long) and the operation fails instead of racing the reader.
    enum : uint8_t { READER_IDLE = 0, READER_BUSY = 1, READER_PARKED = 2 };
    static const uint32_t PARK_TIMEOUT_MS = 5000;
    bool parkReader();    // true: parked (the caller owns the File and the indices)
    void unparkReader();

    const size_t ringSize;
    const size_t chunkSize;
    uint8_t* ring = nullptr;      // PSRAM
    uint8_t* bounce = nullptr;    // internal, DMA-capable (SD reads)
    File file;
    std::atomic<bool> fileOpen{false};
    uint32_t fileSize = 0;
    std::atomic<uint32_t> head{0};  // file offset of the next byte the reader stores
    std::atomic<uint32_t> tail{0};  // file offset of the next byte the decoder reads
    std::atomic<bool> eof{false};
    std::atomic<bool> parkRequested{true};          // owner wants the File: the reader stops claiming it
    std::atomic<uint8_t> readerState{READER_PARKED}; // see the handshake above
    std::atomic<uint32_t> underrunCount{0};
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t dataSem = nullptr;   // reader -> decoder: bytes arrived / eof
    SemaphoreHandle_t spaceSem = nullptr;  // decoder -> reader: space freed
};
