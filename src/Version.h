#pragma once
#include <Arduino.h>
#include <esp_app_desc.h>

// Firmware identity. The version string comes from the image's app descriptor, which is set
// from CONFIG_APP_PROJECT_VER in sdkconfig (single source: bump it there). The backend parses
// the same descriptor from the uploaded .bin, so both sides agree without extra bookkeeping.
#define TT_HARDWARE_REV "adem-v7"

inline const char *firmwareVersion()
{
    return esp_app_get_description()->version;
}

// "1.2.3" -> 10203; anything unparsable (e.g. a git hash) -> 0 so every release is newer.
inline uint32_t firmwareVersionCode()
{
    int major = 0, minor = 0, patch = 0;
    if (sscanf(firmwareVersion(), "%d.%d.%d", &major, &minor, &patch) < 2)
    {
        return 0;
    }
    return (uint32_t)major * 10000UL + (uint32_t)minor * 100UL + (uint32_t)patch;
}
