#include "Faults.h"
#include "RequestManager.h"
#include "Version.h"
#include "PsramJson.h"
#include "TimeSync.h"
#include <Preferences.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <string.h>
#include <esp_heap_caps.h>

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

// The marker's separators must not appear inside a field: the portal splits records at ';' and
// fields at '|' (a remedy with a ';' used to be cut there). Refused at compile time, per row.
constexpr bool noSeparator(const char *s)
{
    return *s == '\0' ? true : (*s != ';' && *s != '|' && noSeparator(s + 1));
}
#define TT_FAULT_CHECK(code, name, sev, fruit, clear, desc, remedy)                                                   \
    static_assert(noSeparator(fruit) && noSeparator(desc) && noSeparator(remedy),                                     \
                  "fault " #code " " #name ": ';' or '|' in a catalogue field breaks the TTFAULTS1 marker");
TT_FAULT_LIST(TT_FAULT_CHECK)
#undef TT_FAULT_CHECK

// Persistent ring of events (NVS blob) - small, fixed-size records.
constexpr int RING = 32;
constexpr int DETAIL_LEN = 40;
constexpr unsigned long DEDUPE_MS = 10UL * 60UL * 1000UL;   // same code: one event per 10 min, counted
constexpr unsigned long UPLOAD_DEBOUNCE_MS = 5000;
constexpr unsigned long UPLOAD_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr int UPLOAD_BATCH = 20;
// Active faults, the same cap in the device report and in the events upload (the two used to
// disagree: 4 and 8). The portal accepts up to 32.
constexpr int MAX_ACTIVE = 8;

struct __attribute__((packed)) Entry {
    uint16_t code;
    uint8_t severity;
    uint8_t sent;        // 1 once the portal acknowledged it
    uint16_t count;
    uint32_t uptimeS;    // uptime of the boot it happened in
    uint32_t boot;       // boot sequence number
    char detail[DETAIL_LEN];
    uint32_t at;         // unix seconds when it happened (SNTP), 0 = clock not set then
};
// Previous layout (firmware 1.2.18 - 1.2.19, magic FAU3): no "at". Migrated at load so an
// update does not lose the events waiting for upload.
struct __attribute__((packed)) EntryV3 {
    uint16_t code;
    uint8_t severity;
    uint8_t sent;
    uint16_t count;
    uint32_t uptimeS;
    uint32_t boot;
    char detail[DETAIL_LEN];
};
struct __attribute__((packed)) Ring {
    uint32_t magic;
    uint16_t head;       // next slot to write
    uint16_t used;
    Entry e[RING];
};
struct __attribute__((packed)) RingV3 {
    uint32_t magic;
    uint16_t head;
    uint16_t used;
    EntryV3 e[RING];
};
constexpr uint32_t RING_MAGIC = 0x46415534;    // "FAU4"
constexpr uint32_t RING_MAGIC_V3 = 0x46415533; // "FAU3"

Ring g_ring;
bool g_dirty = false;
uint32_t g_boot = 0;
unsigned long g_lastRaise[kCatalogSize] = {};
bool g_everRaised[kCatalogSize] = {};
// Active faults: index into the catalogue + uptime at raise
struct Active { int idx; uint32_t sinceS; };
Active g_active[MAX_ACTIVE];
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
// The previous boot's final uptime (to within the 10 s refresh), captured at boot BEFORE the
// RTC copy is restarted for this boot. Events of the previous boot get an exact age from it
// when the gap between the two boots was only a reset (no power-off, no deep sleep).
uint32_t g_prevUptimeS = 0;
bool g_prevAgeExact = false;

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

// JSON string escaping (quote, backslash, control characters as \u00XX); UTF-8 passes through.
// out must hold 6x the input in the worst case; a value that would not fit is cut cleanly.
void jsonEscapeInto(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        char buf[8];
        const char *add = buf;
        if (c == '"') add = "\\\"";
        else if (c == '\\') add = "\\\\";
        else if (c == '\n') add = "\\n";
        else if (c == '\r') add = "\\r";
        else if (c == '\t') add = "\\t";
        else if (c < 0x20) snprintf(buf, sizeof(buf), "\\u%04x", c);
        else { buf[0] = (char)c; buf[1] = 0; }
        const size_t len = strlen(add);
        if (o + len + 1 > cap) break;
        memcpy(out + o, add, len);
        o += len;
    }
    out[o] = 0;
}

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
    } else if (n == sizeof(RingV3)) {
        // Ring written by 1.2.18/1.2.19: same records without the clock time.
        RingV3 *old = static_cast<RingV3 *>(heap_caps_malloc(sizeof(RingV3), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        memset(&g_ring, 0, sizeof(g_ring));
        g_ring.magic = RING_MAGIC;
        if (old && p.getBytes("ring", old, sizeof(RingV3)) == sizeof(RingV3) && old->magic == RING_MAGIC_V3 &&
            old->head < RING && old->used <= RING) {
            g_ring.head = old->head;
            g_ring.used = old->used;
            for (int k = 0; k < RING; ++k) {
                memcpy(&g_ring.e[k], &old->e[k], sizeof(EntryV3));
                g_ring.e[k].at = 0;
            }
            g_dirty = true;
        }
        if (old) heap_caps_free(old);
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
    if (detail) {
        strlcpy(e.detail, detail, sizeof(e.detail));
        // A cut inside a multi-byte UTF-8 character would make the upload invalid JSON for the
        // portal (the whole batch refused): drop the incomplete tail.
        size_t n = strlen(e.detail);
        if (n == sizeof(e.detail) - 1) {
            size_t lead = n;
            while (lead > 0 && (((uint8_t)e.detail[lead - 1]) & 0xC0) == 0x80) lead--;
            if (lead > 0 && ((uint8_t)e.detail[lead - 1]) >= 0xC0) {
                const uint8_t c = (uint8_t)e.detail[lead - 1];
                const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
                if (n - (lead - 1) < need) e.detail[lead - 1] = 0;
            }
        }
    }
    e.at = TimeSync::now(); // 0 until SNTP has set the clock (the upload derives it later if it can)
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
    // Captured before g_rtcUptimeS is restarted below (it used to be zeroed first and every
    // event of the previous boot got this boot's uptime as its age).
    g_prevUptimeS = haveUptime ? g_rtcUptimeS : 0;
    // Exact only when the toy went from the previous boot straight into this one: a software,
    // panic, watchdog or brownout reset. After a power-off or a deep sleep the time in between
    // is unknown, so those ages are lower bounds ("approx").
    switch (r) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        g_prevAgeExact = haveUptime;
        break;
    default:
        g_prevAgeExact = false;
        break;
    }
    char detail[DETAIL_LEN];
    snprintf(detail, sizeof(detail), "reason=%d prev_uptime=%lus", (int)r, (unsigned long)g_prevUptimeS);
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
        // Age: exact within this boot, and for the previous boot when only a reset lay in
        // between (its final uptime was captured at boot). Older boots, or a gap with a
        // power-off / deep sleep: the toy's off time is unknown, the age is a lower bound and
        // the event says so ("approx": true).
        bool exact = true;
        uint32_t age;
        if (e.boot == g_boot) {
            age = now >= e.uptimeS ? now - e.uptimeS : 0;
        } else if (e.boot + 1 == g_boot && g_prevUptimeS > 0) {
            age = now + (g_prevUptimeS > e.uptimeS ? g_prevUptimeS - e.uptimeS : 0);
            exact = g_prevAgeExact;
        } else {
            age = now;
            exact = false;
        }
        o["age_s"] = age;
        if (!exact) o["approx"] = true;
        // Wall-clock time when known (the portal prefers it over age_s): stored at raise once
        // SNTP had set the clock, else derived now from an exact age.
        const uint32_t unixNow = TimeSync::now();
        if (e.at) {
            o["at"] = e.at;
        } else if (exact && unixNow > age) {
            o["at"] = unixNow - age;
        }
        count++;
    }
    JsonArray active = doc["active"].to<JsonArray>();
    for (int i = 0; i < g_activeCount && i < MAX_ACTIVE; ++i) {
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
        g_uploadAt = millis() + 15000; // no network yet (typical right after boot): look again soon
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
        } else if (code >= 400 && code < 500 && code != 408 && code != 429) {
            // The portal refused the batch (validation, unknown or foreign hub, token refused
            // even after the refresh RequestManager already tried): keeping it would block every
            // later event behind it. Dropped, not retried (docs/FAULTS.md).
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
    for (int i = 0; i < g_activeCount && i < MAX_ACTIVE; ++i) {
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
        // The detail goes into a hand-built frame: a backslash, quote or control character in
        // it used to break the whole report (only '"' was handled).
        char d[DETAIL_LEN * 6 + 1];
        jsonEscapeInto(last->detail, d, sizeof(d));
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
