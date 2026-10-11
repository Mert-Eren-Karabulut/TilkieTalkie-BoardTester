#pragma once
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// ArduinoJson allocator that keeps document pools in PSRAM (internal RAM is the scarce
// resource on this board; manifests and file lists are the largest documents we parse).
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p ? p : malloc(size);
    }
    void deallocate(void* pointer) override { free(pointer); }
    void* reallocate(void* ptr, size_t newSize) override {
        void* p = heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p ? p : realloc(ptr, newSize);
    }
};
inline PsramJsonAllocator psramJsonAllocator;
