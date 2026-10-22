#pragma once
#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <sdkconfig.h>

// Firmware identity. The version string comes from the image's app descriptor, which is set
// from CONFIG_APP_PROJECT_VER in sdkconfig (single source: bump it there). The backend parses
// the same descriptor from the uploaded .bin, so both sides agree without extra bookkeeping.
#define TT_HARDWARE_REV "adem-v7"

// version_code (docs/PROTOCOL-V2.md §5): the version must be exactly "major.minor.patch",
// each part 0-99 written without leading zeros, nothing before or after ("v1.2.3", "1.2",
// "1.2.100", "1.2.3-rc1" are refused). code = major*10000 + minor*100 + patch. The portal
// derives the same code from the uploaded image with the same rule, so a mismatch between the
// two formulas can no longer turn a good update into a "rolled back" one.
namespace VersionCode
{
    // Parses one part starting at s[i]: 1-2 digits, no leading zero unless the part is "0".
    // Returns the value or -1; advances i past the digits.
    constexpr int parsePart(const char *s, int &i)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        int v = s[i] - '0';
        ++i;
        if (s[i] >= '0' && s[i] <= '9')
        {
            if (v == 0)
                return -1; // leading zero ("01")
            v = v * 10 + (s[i] - '0');
            ++i;
            if (s[i] >= '0' && s[i] <= '9')
                return -1; // three digits: above 99
        }
        return v;
    }

    // 0 when the string is not a strict major.minor.patch (0 also means "unknown": every
    // release is newer).
    constexpr uint32_t parse(const char *s)
    {
        if (!s)
            return 0;
        int i = 0;
        const int major = parsePart(s, i);
        if (major < 0 || s[i] != '.')
            return 0;
        ++i;
        const int minor = parsePart(s, i);
        if (minor < 0 || s[i] != '.')
            return 0;
        ++i;
        const int patch = parsePart(s, i);
        if (patch < 0 || s[i] != '\0')
            return 0;
        return (uint32_t)major * 10000u + (uint32_t)minor * 100u + (uint32_t)patch;
    }

    constexpr bool valid(const char *s) { return parse(s) != 0 || (s && s[0] == '0' && s[1] == '.' && s[2] == '0' && s[3] == '.' && s[4] == '0' && s[5] == '\0'); }
}

// Build-time refusal: an image whose CONFIG_APP_PROJECT_VER is not strict major.minor.patch
// is not built (tools/release.sh refuses it too). A git hash or "v1.2.3" would otherwise ship
// with version_code 0 and be offered every release, forever.
#ifdef CONFIG_APP_PROJECT_VER
static_assert(VersionCode::valid(CONFIG_APP_PROJECT_VER),
              "CONFIG_APP_PROJECT_VER must be major.minor.patch, each 0-99 (no v prefix, no suffix)");
#endif

inline const char *firmwareVersion()
{
    return esp_app_get_description()->version;
}

// "1.2.3" -> 10203. The runtime check stays (the descriptor of a foreign image could be
// anything): an unparsable version logs once and counts as 0.
inline uint32_t firmwareVersionCode()
{
    const char *v = firmwareVersion();
    const uint32_t code = VersionCode::parse(v);
    if (code == 0 && !VersionCode::valid(v))
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            ESP_LOGE("version", "firmware version '%s' is not major.minor.patch (0-99 each) - version_code 0", v ? v : "");
        }
    }
    return code;
}
