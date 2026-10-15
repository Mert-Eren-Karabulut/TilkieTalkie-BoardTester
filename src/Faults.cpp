#include "Faults.h"
#include "RequestManager.h"
#include "Version.h"
#include "PsramJson.h"
#include <Preferences.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <string.h>

namespace
{
const FaultInfo kCatalog[] = {
#define TT_FAULT_ROW(code, name, sev, fruit, clear, desc, remedy) {code, #name, Severity::sev, fruit, FaultClear::clear, desc, remedy},
    TT_FAULT_LIST(TT_FAULT_ROW)
#undef TT_FAULT_ROW
};
constexpr size_t kCatalogSize = sizeof(kCatalog) / sizeof(kCatalog[0]);

// Embedded for the portal (FaultCatalog::fromImage): code|slug|severity|fruit|description|remedy;...
#define TT_FAULT_STR_(x) #x
#define TT_FAULT_STR(x) TT_FAULT_STR_(x)
#define TT_FAULT_MARKER(code, name, sev, fruit, clear, desc, remedy) TT_FAULT_STR(code) "|" #name "|" #sev "|" fruit "|" desc "|" remedy ";"
const char kFaultSchema[] __attribute__((used)) = "TTFAULTS1:" TT_FAULT_LIST(TT_FAULT_MARKER);
#undef TT_FAULT_MARKER

// Persistent ring of events (NVS blob) - small, fixed-size records.
constexpr int RING = 32;
constexpr int DETAIL_LEN = 40;
constexpr unsigned long DEDUPE_MS = 10UL * 60UL * 1000UL;   // same code: one event per 10 min, counted
constexpr unsigned long UPLOAD_DEBOUNCE_MS = 5000;
constexpr unsigned long UPLOAD_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr int UPLOAD_BATCH = 20;

struct __attribute__((packed)) Entry {
    uint16_t code;
    uint8_t severity;
    uint8_t sent;        // 1 once the portal acknowledged it
    uint16_t count;
    uint32_t uptimeS;    // uptime of the boot it happened in
    uint32_t boot;       // boot sequence number
    char detail[DETAIL_LEN];
};
struct __attribute__((packed)) Ring {
    uint32_t magic;
    uint16_t head;       // next slot to write
    uint16_t used;
    Entry e[RING];
};
constexpr uint32_t RING_MAGIC = 0x46415533; // "FAU3"

Ring g_ring;
bool g_dirty = false;
uint32_t g_boot = 0;
unsigned long g_lastRaise[kCatalogSize] = {};
bool g_everRaised[kCatalogSize] = {};
// Active faults: index into the catalogue + uptime at raise
struct Active { int idx; uint32_t sinceS; };
Active g_active[8];
int g_activeCount = 0;
std::function<void(Fault)> g_speaker;
unsigned long g_uploadAt = 0;      // 0 = nothing pending
bool g_uploading = false;
bool g_begun = false;

// Survives a software reset (panic, watchdog): the previous boot's uptime, so the reset
// event can carry it. Garbage after a power-on, hence the magic.
RTC_NOINIT_ATTR uint32_t g_rtcMagic;
RTC_NOINIT_ATTR uint32_t g_rtcUptimeS;
constexpr uint32_t RTC_MAGIC = 0x55505431; // "UPT1"

int indexOf(uint16_t code)
{
    for (size_t i = 0; i < kCatalogSize; ++i) {
        if (kCatalog[i].code == code) return (int)i;
    }
    return -1;
}

const char *severityName(Severity s)
{
    switch (s) {
    case Severity::INFO: return "info";
    case Severity::WARNING: return "warning";
    case Severity::ERROR: return "error";
    case Severity::CRITICAL: return "critical";
    }
    return "info";
}

uint32_t uptimeS() { return (uint32_t)(millis() / 1000UL); }

void load()
{
    Preferences p;
    if (!p.begin("faults", true)) {
        memset(&g_ring, 0, sizeof(g_ring));
        g_ring.magic = RING_MAGIC;
        return;
    }
    size_t n = p.getBytesLength("ring");
    if (n == sizeof(Ring) && p.getBytes("ring", &g_ring, sizeof(Ring)) == sizeof(Ring) && g_ring.magic == RING_MAGIC &&
        g_ring.head < RING && g_ring.used <= RING) {
        // ok
    } else {
        memset(&g_ring, 0, sizeof(g_ring));
        g_ring.magic = RING_MAGIC;
    }
    g_boot = p.getUInt("boot", 0);
    p.end();
}

void save()
{
    Preferences p;
    if (!p.begin("faults", false)) return;
    p.putBytes("ring", &g_ring, sizeof(Ring));
    p.putUInt("boot", g_boot);
    p.end();
    g_dirty = false;
}

Entry *findUnsentSameBoot(uint16_t code)
{
    for (int k = 0; k < g_ring.used; ++k) {
        Entry &e = g_ring.e[k];
        if (e.code == code && !e.sent && e.boot == g_boot) return &e;
    }
    return nullptr;
}

void record(const FaultInfo &info, const char *detail)
{
    Entry &e = g_ring.e[g_ring.head];
    memset(&e, 0, sizeof(e));
    e.code = info.code;
    e.severity = (uint8_t)info.severity;
    e.count = 1;
    e.uptimeS = uptimeS();
    e.boot = g_boot;
    if (detail) strlcpy(e.detail, detail, sizeof(e.detail));
    g_ring.head = (g_ring.head + 1) % RING;
    if (g_ring.used < RING) g_ring.used++;
    g_dirty = true;
}

void activate(int idx)
{
    for (int i = 0; i < g_activeCount; ++i) {
        if (g_active[i].idx == idx) return;
    }
    if (g_activeCount < (int)(sizeof(g_active) / sizeof(g_active[0]))) {
        g_active[g_activeCount++] = {idx, uptimeS()};
    }
}

void deactivate(int idx)
{
    for (int i = 0; i < g_activeCount; ++i) {
        if (g_active[i].idx == idx) {
            g_active[i] = g_active[g_activeCount - 1];
            g_activeCount--;
            return;
        }
    }
}

void classifyReset()
{
    const esp_reset_reason_t r = esp_reset_reason();
    const bool haveUptime = (g_rtcMagic == RTC_MAGIC);
    char detail[DETAIL_LEN];
    snprintf(detail, sizeof(detail), "reason=%d prev_uptime=%lus", (int)r, haveUptime ? (unsigned long)g_rtcUptimeS : 0UL);
    const FaultInfo *fi = nullptr;
    switch (r) {
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
        fi = Faults::info(Fault::WATCHDOG_RESET);
        break;
    case ESP_RST_PANIC:
        fi = Faults::info(Fault::PANIC_RESET);
        break;
    case ESP_RST_BROWNOUT:
        fi = Faults::info(Fault::BROWNOUT_RESET);
        break;
    default:
        break;
    }
    if (fi) {
        record(*fi, detail);
        Serial.printf("Faults: previous boot ended with %s (%s)\n", fi->slug, detail);
    }
    g_rtcMagic = RTC_MAGIC;
    g_rtcUptimeS = 0;
}

void buildBatch(JsonDocument &doc, int &count)
{
    JsonArray events = doc["events"].to<JsonArray>();
    count = 0;
    const uint32_t now = uptimeS();
    for (int k = 0; k < g_ring.used && count < UPLOAD_BATCH; ++k) {
        const Entry &e = g_ring.e[k];
        if (e.sent) continue;
        const FaultInfo *fi = Faults::info(e.code);
        JsonObject o = events.add<JsonObject>();
        o["code"] = e.code;
        o["slug"] = fi ? fi->slug : "UNKNOWN";
        o["severity"] = severityName((Severity)e.severity);
        if (e.detail[0]) o["detail"] = e.detail;
        o["count"] = e.count;
        o["uptime_s"] = e.uptimeS;
        o["boot"] = e.boot;
        o["firmware"] = firmwareVersion();
        // Age: exact within this boot; for an earlier boot the time the toy was off is unknown,
        // so the previous boot's final uptime (RTC) bounds it from below.
        uint32_t age = (e.boot == g_boot) ? (now - e.uptimeS) : (now + (g_rtcUptimeS > e.uptimeS ? g_rtcUptimeS - e.uptimeS : 0));
        o["age_s"] = age;
        count++;
    }
    JsonArray active = doc["active"].to<JsonArray>();
    for (int i = 0; i < g_activeCount; ++i) {
        JsonObject a = active.add<JsonObject>();
        a["c"] = kCatalog[g_active[i].idx].code;
        a["since_s"] = now - g_active[i].sinceS;
    }
}

void markSent(int n)
{
    int marked = 0;
    for (int k = 0; k < g_ring.used && marked < n; ++k) {
        if (!g_ring.e[k].sent) {
            g_ring.e[k].sent = 1;
            marked++;
        }
    }
    g_dirty = true;
}

void upload()
{
    RequestManager &rm = RequestManager::getInstance();
    if (!rm.isNetworkReady() || !rm.isConnected()) {
        g_uploadAt = millis() + UPLOAD_RETRY_MS;
        return;
    }
    JsonDocument doc(&psramJsonAllocator);
    int count = 0;
    buildBatch(doc, count);
    if (count == 0) {
        g_uploadAt = 0;
        return;
    }
    char buf[21];
    snprintf(buf, sizeof(buf), "%llu", ESP.getEfuseMac());
    g_uploading = true;
    const int sent = count;
    bool queued = rm.postAsync(String("/hubs/") + buf + "/events", doc, [sent](JsonDocument &, int code) {
        g_uploading = false;
        if (code == 200 || code == 201) {
            markSent(sent);
            save();
            g_uploadAt = 0;
            Serial.printf("Faults: %d event(s) delivered\n", sent);
            // more left? schedule the next batch
            for (int k = 0; k < g_ring.used; ++k) {
                if (!g_ring.e[k].sent) { g_uploadAt = millis() + 1000; break; }
            }
        } else if (code == 422 || code == 400) {
            // The portal refused the batch itself (not a transport problem): keeping it would block
            // every later event behind it. Drop it and move on.
            markSent(sent);
            save();
            g_uploadAt = 0;
            Serial.printf("Faults: portal rejected %d event(s) (HTTP %d), dropped\n", sent, code);
        } else {
            g_uploadAt = millis() + UPLOAD_RETRY_MS;
            Serial.printf("Faults: upload failed (HTTP %d), retry in %lu s\n", code, UPLOAD_RETRY_MS / 1000);
        }
    });
    if (!queued) {
        g_uploading = false;
        g_uploadAt = millis() + UPLOAD_RETRY_MS;
    }
}
} // namespace

void Faults::begin()
{
    if (g_begun) return;
    g_begun = true;
    load();
    g_boot++;
    classifyReset();
    save();
    int unsent = 0;
    for (int k = 0; k < g_ring.used; ++k) if (!g_ring.e[k].sent) unsent++;
    // The catalogue string is referenced here so the linker keeps it in the image for the portal.
    Serial.printf("Faults: boot #%lu, %d event(s) stored, %d to deliver, catalogue %u codes (%u bytes)\n",
                  (unsigned long)g_boot, (int)g_ring.used, unsent, (unsigned)kCatalogSize, (unsigned)strlen(schema()));
    if (unsent) g_uploadAt = millis() + 30000; // once the network and token are there
}

void Faults::raise(Fault f, const char *detail, bool speak)
{
    const int idx = indexOf((uint16_t)f);
    if (idx < 0) return;
    const FaultInfo &fi = kCatalog[idx];
    const unsigned long now = millis();
    const bool repeat = g_everRaised[idx] && (now - g_lastRaise[idx] < DEDUPE_MS);
    g_everRaised[idx] = true;
    g_lastRaise[idx] = now;
    if (fi.clears != FaultClear::EVENT) activate(idx);
    if (repeat) {
        Entry *e = findUnsentSameBoot(fi.code);
        if (e) { if (e->count < 65535) e->count++; g_dirty = true; }
    } else {
        record(fi, detail);
        Serial.printf("Faults: %u %s (%s)%s%s\n", fi.code, fi.slug, severityName(fi.severity), detail ? " " : "", detail ? detail : "");
        if (speak && fi.fruit[0] && g_speaker) g_speaker(f);
        if (!g_uploadAt || g_uploadAt > now + UPLOAD_DEBOUNCE_MS) g_uploadAt = now + UPLOAD_DEBOUNCE_MS;
    }
    if (g_dirty && !repeat) save();
}

void Faults::clear(Fault f)
{
    const int idx = indexOf((uint16_t)f);
    if (idx >= 0) deactivate(idx);
}

void Faults::clearOn(FaultClear trigger)
{
    for (int i = g_activeCount - 1; i >= 0; --i) {
        if (kCatalog[g_active[i].idx].clears == trigger) {
            g_active[i] = g_active[g_activeCount - 1];
            g_activeCount--;
        }
    }
}

void Faults::update()
{
    static unsigned long lastRtc = 0;
    const unsigned long now = millis();
    if (now - lastRtc > 10000) { // the previous-boot uptime for a reset report
        lastRtc = now;
        g_rtcMagic = RTC_MAGIC;
        g_rtcUptimeS = uptimeS();
    }
    if (g_dirty && !g_uploading) save();
    if (g_uploadAt && !g_uploading && (long)(now - g_uploadAt) >= 0) upload();
}

const FaultInfo *Faults::info(Fault f) { return info((uint16_t)f); }

const FaultInfo *Faults::info(uint16_t code)
{
    const int idx = indexOf(code);
    return idx >= 0 ? &kCatalog[idx] : nullptr;
}

const char *Faults::fruit()
{
    const FaultInfo *best = nullptr;
    for (int i = 0; i < g_activeCount; ++i) {
        const FaultInfo &fi = kCatalog[g_active[i].idx];
        if (!fi.fruit[0]) continue;
        if (!best || fi.severity > best->severity) best = &fi;
    }
    return best ? best->fruit : "";
}

bool Faults::hasActive(Severity atLeast)
{
    for (int i = 0; i < g_activeCount; ++i) {
        if (kCatalog[g_active[i].idx].severity >= atLeast) return true;
    }
    return false;
}

String Faults::reportJson()
{
    const uint32_t now = uptimeS();
    String s = "\"active\":[";
    for (int i = 0; i < g_activeCount && i < 4; ++i) {
        if (i) s += ",";
        s += "{\"c\":" + String(kCatalog[g_active[i].idx].code) + ",\"since_s\":" + String(now - g_active[i].sinceS) + "}";
    }
    s += "],\"last\":";
    const Entry *last = nullptr;
    for (int k = 0; k < g_ring.used; ++k) {
        const Entry &e = g_ring.e[k];
        if (e.boot != g_boot) continue;
        if (!last || e.uptimeS >= last->uptimeS) last = &e;
    }
    if (!last) {
        s += "null";
    } else {
        String d = last->detail;
        d.replace("\"", "'");
        s += "{\"c\":" + String(last->code) + ",\"age_s\":" + String(now - last->uptimeS) + ",\"n\":" + String(last->count) + ",\"d\":\"" + d + "\"}";
    }
    return s;
}

void Faults::setSpeaker(std::function<void(Fault)> fn) { g_speaker = fn; }

const char *Faults::schema() { return kFaultSchema + 10; }

void Faults::printStatus()
{
    Serial.printf("Faults: boot #%lu, %d stored, %d active, upload %s\n", (unsigned long)g_boot, (int)g_ring.used, g_activeCount,
                  g_uploadAt ? "pending" : "idle");
    for (int i = 0; i < g_activeCount; ++i) {
        const FaultInfo &fi = kCatalog[g_active[i].idx];
        Serial.printf("  active %u %s since %lus\n", fi.code, fi.slug, (unsigned long)(uptimeS() - g_active[i].sinceS));
    }
    for (int k = 0; k < g_ring.used; ++k) {
        const Entry &e = g_ring.e[k];
        const FaultInfo *fi = info(e.code);
        Serial.printf("  [%s] boot %lu +%lus %u %s x%u %s\n", e.sent ? "sent" : "new ", (unsigned long)e.boot, (unsigned long)e.uptimeS,
                      e.code, fi ? fi->slug : "?", e.count, e.detail);
    }
}
