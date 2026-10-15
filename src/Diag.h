#ifndef TT_DIAG_H
#define TT_DIAG_H

#include <Arduino.h>
#include <esp_timer.h>

// Lightweight runtime diagnostics: reset reason, core dump presence, per-section
// loop timing (max / avg / count per named section, reported periodically).
namespace Diag
{
    // Call once early in setup(): logs reset reason and whether a core dump is stored.
    void logBootInfo();

    // Periodic report of section timings + heap; call from loop() (rate-limited inside).
    void periodicReport(uint32_t intervalMs = 10000);
    void setVerbose(bool on);   // print the per-section table every interval (default: one heap line a minute)
    bool verbose();

    // Print section timing table now and reset the max values.
    void printSectionStats(bool reset = true);

    // Core-dump helpers for the serial console.
    void printCoreDumpSummary();
    void eraseCoreDump();

    // Per-task CPU usage and stack high-water marks (needs FreeRTOS run-time stats in sdkconfig).
    void printTaskStats();

    // Section timing primitives (used by the TT_TIMED macro).
    uint8_t sectionId(const char *name); // registers name on first use, returns slot (max 32)

    struct Scoped
    {
        uint8_t id;
        int64_t t0;
        explicit Scoped(uint8_t sid) : id(sid), t0(esp_timer_get_time()) {}
        ~Scoped();
    };
}

// Usage: TT_TIMED("battery", battery.update());  (single statement or expression)
#define TT_TIMED(name, ...)                                   \
    do                                                        \
    {                                                         \
        static const uint8_t _tt_sid = Diag::sectionId(name); \
        Diag::Scoped _tt_scope(_tt_sid);                      \
        __VA_ARGS__;                                          \
    } while (0)

#endif // TT_DIAG_H

// Longest gap between two LED frames (set by the LED task, printed and reset by Diag).
extern volatile int64_t g_ledFrameMaxGapUs;
