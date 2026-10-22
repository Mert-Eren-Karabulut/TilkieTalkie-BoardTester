#include "TimeSync.h"
#include <esp_sntp.h>
#include <esp_log.h>
#include <atomic>
#include <time.h>
#include <sys/time.h>

namespace
{
const char *TAG = "time";
// Anything before this cannot be a synced clock (the RTC starts at 1970 after a power-on).
constexpr time_t MIN_VALID = 1767225600; // 2026-01-01T00:00:00Z
// lwIP is built with one SNTP server slot (CONFIG_LWIP_SNTP_MAX_SERVERS=1): the second server
// is used by switching when the first gives nothing for a while.
const char *SERVERS[] = {"pool.ntp.org", "time.google.com"};
constexpr unsigned long SWITCH_AFTER_MS = 60000;

std::atomic<bool> g_synced{false};
std::atomic<bool> g_firstSyncLogged{false};
bool g_started = false;
int g_server = 0;
unsigned long g_serverSince = 0;

// SNTP (lwIP) task context: a flag and one log line, nothing else.
void onSync(struct timeval *tv)
{
    g_synced = true;
    if (!g_firstSyncLogged.exchange(true))
    {
        struct tm t;
        const time_t s = tv ? tv->tv_sec : time(nullptr);
        gmtime_r(&s, &t);
        ESP_LOGI(TAG, "clock set by SNTP: %04d-%02d-%02dT%02d:%02d:%02dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    }
}

void startServer(int idx)
{
    g_server = idx;
    g_serverSince = millis();
    // configTime() stops a running client, sets the server and restarts it under the lwIP
    // core lock; offsets 0 keep the system time in UTC.
    configTime(0, 0, SERVERS[idx]);
    ESP_LOGI(TAG, "SNTP started (%s)", SERVERS[idx]);
}
} // namespace

void TimeSync::begin()
{
    if (g_started)
    {
        return;
    }
    g_started = true;
    if (time(nullptr) >= MIN_VALID)
    {
        ESP_LOGI(TAG, "clock carried over from before the reset (%lld)", (long long)time(nullptr));
    }
    sntp_set_time_sync_notification_cb(onSync);
    startServer(0);
}

void TimeSync::update()
{
    if (!g_started || g_synced.load())
    {
        return;
    }
    if (millis() - g_serverSince > SWITCH_AFTER_MS)
    {
        ESP_LOGW(TAG, "no SNTP answer from %s - trying %s", SERVERS[g_server], SERVERS[(g_server + 1) % 2]);
        startServer((g_server + 1) % 2);
    }
}

bool TimeSync::valid()
{
    return g_synced.load() || time(nullptr) >= MIN_VALID;
}

uint32_t TimeSync::now()
{
    const time_t t = time(nullptr);
    return t >= MIN_VALID ? (uint32_t)t : 0;
}
