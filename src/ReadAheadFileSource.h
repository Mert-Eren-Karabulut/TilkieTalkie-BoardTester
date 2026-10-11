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
    ReadAheadFileSource(size_t ringBytes = 128 * 1024, size_t chunkBytes = 8 * 1024);
    ~ReadAheadFileSource() override;

    bool begin();  // allocate the buffers and start the reader task (once)

    bool open(const char* path) override;
    uint32_t read(void* data, uint32_t len) override;
    bool seek(int32_t pos, int dir) override;
    bool close() override;
    bool isOpen() override { return fileOpen; }
    uint32_t getSize() override { return fileSize; }
    uint32_t getPos() override { return tail.load(); }

    uint32_t available() const { return head.load() - tail.load(); }
    uint32_t capacity() const { return ringSize; }
    uint32_t underruns() const { return underrunCount.load(); }
    bool atEof() const { return eof.load(); }

private:
    static void readerTaskEntry(void* arg);
    void readerLoop();
    void parkReader();    // make the reader let go of the file (open/seek/close)
    void unparkReader();

    const size_t ringSize;
    const size_t chunkSize;
    uint8_t* ring = nullptr;      // PSRAM
    uint8_t* bounce = nullptr;    // internal, DMA-capable (SD reads)
    File file;
    volatile bool fileOpen = false;
    uint32_t fileSize = 0;
    std::atomic<uint32_t> head{0};  // file offset of the next byte the reader stores
    std::atomic<uint32_t> tail{0};  // file offset of the next byte the decoder reads
    std::atomic<bool> eof{false};
    std::atomic<bool> parkRequested{true};
    std::atomic<bool> parked{true};
    std::atomic<uint32_t> underrunCount{0};
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t dataSem = nullptr;   // reader -> decoder: bytes arrived / eof
    SemaphoreHandle_t spaceSem = nullptr;  // decoder -> reader: space freed
};
