#pragma once

#include <Arduino.h>

// Wall-clock time from SNTP (UTC). The toy had no clock: fault events carried only their age
// and TLS dates were never checked. Started once WiFi has an IP (main.cpp); the system time
// then survives deep sleep and software resets in the RTC, so a wake from sleep has a valid
// clock before the next sync.
//
// time_valid for the modules (Faults: event "at", ReverbClient: command "issued_at"):
// TimeSync::valid(). docs/PROTOCOL-V2.md, docs/FAULTS.md.
namespace TimeSync
{
    void begin();      // idempotent; first call starts SNTP (pool.ntp.org, then time.google.com)
    void update();     // loop task: switches server when the first sync does not come
    bool valid();      // the clock is set (synced this boot, or carried over from before a reset)
    uint32_t now();    // unix seconds, 0 while not valid
}
