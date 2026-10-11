#include "ReadAheadFileSource.h"
#include <SD_MMC.h>
#include <esp_heap_caps.h>

ReadAheadFileSource::ReadAheadFileSource(size_t ringBytes, size_t chunkBytes)
    : ringSize(ringBytes), chunkSize(chunkBytes) {}

ReadAheadFileSource::~ReadAheadFileSource() {
    close();
    if (task) {
        vTaskDelete(task);
        task = nullptr;
    }
    if (ring) heap_caps_free(ring);
    if (bounce) heap_caps_free(bounce);
    if (dataSem) vSemaphoreDelete(dataSem);
    if (spaceSem) vSemaphoreDelete(spaceSem);
}

bool ReadAheadFileSource::begin() {
    if (task) return true;
    if ((ringSize & (ringSize - 1)) != 0) {
        Serial.println("ReadAhead: ring size must be a power of two");
        return false;
    }
    ring = (uint8_t*)heap_caps_malloc(ringSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bounce = (uint8_t*)heap_caps_malloc(chunkSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    dataSem = xSemaphoreCreateBinary();
    spaceSem = xSemaphoreCreateBinary();
    if (!ring || !bounce || !dataSem || !spaceSem) {
        Serial.println("ReadAhead: allocation failed");
        return false;
    }
    // Core 0 next to the SD writer, above it in priority so a waiting read gets the card
    // between two 32 KB download writes.
    if (xTaskCreatePinnedToCore(readerTaskEntry, "audio_rd", 4096, this, 6, &task, 0) != pdPASS) {
        task = nullptr;
        Serial.println("ReadAhead: task creation failed");
        return false;
    }
    Serial.printf("ReadAhead: %u KB PSRAM ring, %u KB chunks\n", (unsigned)(ringSize / 1024), (unsigned)(chunkSize / 1024));
    return true;
}

void ReadAheadFileSource::readerTaskEntry(void* arg) {
    static_cast<ReadAheadFileSource*>(arg)->readerLoop();
}

void ReadAheadFileSource::readerLoop() {
    for (;;) {
        if (parkRequested.load() || !fileOpen || eof.load()) {
            parked = true;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        parked = false;
        uint32_t used = head.load() - tail.load();
        if (ringSize - used < chunkSize) {
            xSemaphoreTake(spaceSem, pdMS_TO_TICKS(50));
            continue;
        }
        int n = file.read(bounce, chunkSize);
        if (n <= 0) {
            eof = true;
            xSemaphoreGive(dataSem);
            continue;
        }
        uint32_t idx = head.load() & (ringSize - 1);
        uint32_t first = (uint32_t)n < ringSize - idx ? (uint32_t)n : ringSize - idx;
        memcpy(ring + idx, bounce, first);
        if ((uint32_t)n > first) {
            memcpy(ring, bounce + first, n - first);
        }
        head += n;
        xSemaphoreGive(dataSem);
    }
}

void ReadAheadFileSource::parkReader() {
    parkRequested = true;
    if (!task) return;
    // A read in flight can sit on the card for a few seconds; wait for it (bounded).
    for (int i = 0; i < 500 && !parked.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ReadAheadFileSource::unparkReader() {
    parkRequested = false;
    if (task) xTaskNotifyGive(task);
}

bool ReadAheadFileSource::open(const char* path) {
    if (!task) return false;
    parkReader();
    if (fileOpen) {
        file.close();
        fileOpen = false;
    }
    file = SD_MMC.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return false;
    }
    fileSize = file.size();
    head = 0;
    tail = 0;
    eof = (fileSize == 0);
    underrunCount = 0;
    fileOpen = true;
    unparkReader();
    return true;
}

uint32_t ReadAheadFileSource::read(void* data, uint32_t len) {
    if (!fileOpen || len == 0) return 0;
    uint8_t* out = static_cast<uint8_t*>(data);
    uint32_t got = 0;
    unsigned long waitStart = 0;
    bool countedUnderrun = false;
    while (got < len) {
        uint32_t avail = head.load() - tail.load();
        if (avail == 0) {
            if (eof.load() || got > 0) break;  // short reads are fine for the decoder
            if (waitStart == 0) waitStart = millis();
            if (millis() - waitStart > 8000) {  // card gone? let the decoder end the track
                if (!countedUnderrun) underrunCount++;
                break;
            }
            xSemaphoreTake(dataSem, pdMS_TO_TICKS(100));
            if (!countedUnderrun && millis() - waitStart > 20) {
                underrunCount++;
                countedUnderrun = true;
            }
            continue;
        }
        uint32_t n = (len - got) < avail ? (len - got) : avail;
        uint32_t idx = tail.load() & (ringSize - 1);
        uint32_t first = n < ringSize - idx ? n : ringSize - idx;
        memcpy(out + got, ring + idx, first);
        if (n > first) {
            memcpy(out + got + first, ring, n - first);
        }
        tail += n;
        got += n;
        xSemaphoreGive(spaceSem);
    }
    return got;
}

bool ReadAheadFileSource::seek(int32_t pos, int dir) {
    if (!fileOpen) return false;
    int64_t target = pos;
    if (dir == SEEK_CUR) target = (int64_t)tail.load() + pos;
    else if (dir == SEEK_END) target = (int64_t)fileSize + pos;
    if (target < 0) target = 0;
    if (target > fileSize) target = fileSize;
    parkReader();
    bool ok = file.seek((uint32_t)target);
    if (ok) {
        head = (uint32_t)target;
        tail = (uint32_t)target;
        eof = ((uint32_t)target >= fileSize);
    }
    unparkReader();
    return ok;
}

bool ReadAheadFileSource::close() {
    if (!fileOpen) return true;
    parkReader();
    file.close();
    fileOpen = false;
    head = 0;
    tail = 0;
    eof = true;
    return true;
}
