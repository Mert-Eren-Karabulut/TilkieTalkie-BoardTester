#include "Diag.h"
#include <esp_system.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_core_dump.h>
#include <string.h>

volatile int64_t g_ledFrameMaxGapUs = 0;

static const char *TAG = "DIAG";

namespace
{
    constexpr uint8_t MAX_SECTIONS = 32; // 20 TT_TIMED sections in the loop today
    struct Section
    {
        const char *name = nullptr;
        int64_t maxUs = 0;    // max since last print
        int64_t worstUs = 0;  // max since boot
        int64_t sumUs = 0;
        uint32_t count = 0;
        uint32_t over20ms = 0; // calls that exceeded 20 ms (audio DMA margin)
    };
    Section g_sections[MAX_SECTIONS];
    uint8_t g_sectionCount = 0;
    int64_t g_loopLastUs = 0;
    int64_t g_loopMaxGapUs = 0;
    uint32_t g_loopIters = 0;
}

static const char *resetReasonName(esp_reset_reason_t r)
{
    switch (r)
    {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software (esp_restart)";
    case ESP_RST_PANIC: return "PANIC / exception";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
#endif
    default: return "unknown";
    }
}

void Diag::logBootInfo()
{
    esp_reset_reason_t r = esp_reset_reason();
    ESP_LOGW(TAG, "Reset reason: %s (%d)", resetReasonName(r), (int)r);
    ESP_LOGI(TAG, "Internal heap: free=%u largest=%u minfree=%u | PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (esp_core_dump_image_check() == ESP_OK)
    {
        ESP_LOGE(TAG, "A CORE DUMP from a previous crash is stored in flash. Type 'coredump' to see it, 'coredumperase' to clear.");
        printCoreDumpSummary();
    }
#endif
}

void Diag::printCoreDumpSummary()
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t *s = (esp_core_dump_summary_t *)heap_caps_calloc(1, sizeof(esp_core_dump_summary_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) s = (esp_core_dump_summary_t *)calloc(1, sizeof(esp_core_dump_summary_t));
    if (!s) { ESP_LOGE(TAG, "no memory for core dump summary"); return; }
    esp_err_t err = esp_core_dump_get_summary(s);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "No core dump available (%s)", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGE(TAG, "--- CORE DUMP SUMMARY ---");
        ESP_LOGE(TAG, "Crashed task: %s  PC=0x%08x  exc_cause=%u", s->exc_task, (unsigned)s->exc_pc, (unsigned)s->ex_info.exc_cause);
        ESP_LOGE(TAG, "App ELF SHA256: %s", s->app_elf_sha256);
        ESP_LOGE(TAG, "Backtrace (%u frames%s):", (unsigned)s->exc_bt_info.depth, s->exc_bt_info.corrupted ? ", CORRUPTED" : "");
        for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16; i++)
            ESP_LOGE(TAG, "  #%u 0x%08x", (unsigned)i, (unsigned)s->exc_bt_info.bt[i]);
        ESP_LOGE(TAG, "Decode with: xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <addresses>");
    }
    free(s);
#else
    ESP_LOGW(TAG, "Core dump to flash not enabled in this build");
#endif
}

void Diag::eraseCoreDump()
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    esp_err_t err = esp_core_dump_image_erase();
    ESP_LOGI(TAG, "Core dump erase: %s", esp_err_to_name(err));
#endif
}

uint8_t Diag::sectionId(const char *name)
{
    for (uint8_t i = 0; i < g_sectionCount; i++)
        if (strcmp(g_sections[i].name, name) == 0) return i;
    if (g_sectionCount >= MAX_SECTIONS) return MAX_SECTIONS - 1;
    g_sections[g_sectionCount].name = name;
    return g_sectionCount++;
}

Diag::Scoped::~Scoped()
{
    int64_t dt = esp_timer_get_time() - t0;
    Section &s = g_sections[id];
    if (dt > s.maxUs) s.maxUs = dt;
    if (dt > s.worstUs) s.worstUs = dt;
    s.sumUs += dt;
    s.count++;
    if (dt > 20000) s.over20ms++;
}

void Diag::printSectionStats(bool reset)
{
    ESP_LOGI(TAG, "--- loop section timing (ms): max-since-last / worst-ever / avg / calls / >20ms ---");
    for (uint8_t i = 0; i < g_sectionCount; i++)
    {
        Section &s = g_sections[i];
        if (s.count == 0 && s.worstUs == 0) continue;
        ESP_LOGI(TAG, "  %-12s %8.2f %8.2f %7.3f %8u %6u", s.name,
                 s.maxUs / 1000.0, s.worstUs / 1000.0,
                 s.count ? (s.sumUs / 1000.0) / s.count : 0.0, (unsigned)s.count, (unsigned)s.over20ms);
        if (reset) { s.maxUs = 0; s.sumUs = 0; s.count = 0; s.over20ms = 0; }
    }
    ESP_LOGI(TAG, "  loop iterations=%u  max gap between iterations=%.2f ms | LED frame max gap %.1f ms (nominal 20)",
             (unsigned)g_loopIters, g_loopMaxGapUs / 1000.0, g_ledFrameMaxGapUs / 1000.0);
    if (reset) { g_loopIters = 0; g_loopMaxGapUs = 0; g_ledFrameMaxGapUs = 0; }
}

static bool g_verbose = false;
void Diag::setVerbose(bool on) { g_verbose = on; }
bool Diag::verbose() { return g_verbose; }

void Diag::periodicReport(uint32_t intervalMs)
{
    int64_t now = esp_timer_get_time();
    if (g_loopLastUs)
    {
        int64_t gap = now - g_loopLastUs;
        if (gap > g_loopMaxGapUs) g_loopMaxGapUs = gap;
    }
    g_loopLastUs = now;
    g_loopIters++;

    static int64_t lastReport = 0;
    const int64_t interval = (g_verbose ? (int64_t)intervalMs : 60000LL) * 1000LL;
    if (now - lastReport < interval) return;
    lastReport = now;
    if (g_verbose)
    {
        printSectionStats(true); // ~20 lines over USB-CDC: only when asked for ('diag on')
    }
    else
    {
        ESP_LOGI(TAG, "loop max gap %.0f ms, LED frame max gap %.0f ms", g_loopMaxGapUs / 1000.0, g_ledFrameMaxGapUs / 1000.0);
        g_loopMaxGapUs = 0;
        g_ledFrameMaxGapUs = 0;
    }
    ESP_LOGI(TAG, "  heap: internal free=%u largest=%u minfree=%u | psram free=%u | loop stack hwm=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

void Diag::printTaskStats()
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
    const size_t bufSize = 4096;
    char *buf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(bufSize);
    if (!buf) { ESP_LOGE(TAG, "no memory for task stats"); return; }
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    vTaskGetRunTimeStats(buf);
    ESP_LOGI(TAG, "--- task run-time stats (abs time, %%) ---\n%s", buf);
#endif
    vTaskList(buf);
    ESP_LOGI(TAG, "--- task list (name state prio stack-hwm num core) ---\n%s", buf);
    free(buf);
#else
    ESP_LOGW(TAG, "FreeRTOS trace facility / stats formatting not enabled in sdkconfig");
#endif
}
