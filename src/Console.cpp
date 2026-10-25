// Serial console: one line per command from the USB CDC port, serviced from loop().
// Production commands are operational (status, playback, files, network, recovery of a stray
// bench override); bench-only commands (learning cycle, gauge data flash, destructive or
// test-only tools) are compiled only with -DTT_BENCH (env:bench). docs/CONSOLE.md lists both.
//
// Replies to the operator are console output, not logging: they stay on ESP_LOG with the
// "console" tag (as before the split) or on Serial for raw dumps.

#include "Console.h"
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <sys/time.h> // gettimeofday: system time survives deep sleep (sleeptest elapsed)
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD_MMC.h>
#include <wifi_provisioning/manager.h>
#include "Bringup.h"
#include "Diag.h"
#include "FactoryTest.h"
#include "Faults.h"
#include "OtaManager.h"
#include "Policy.h"
#include "SystemState.h"
#include "Version.h"

static const char *TAG = "console";

// Heap health thresholds for the `heap` report
static const size_t CRITICAL_HEAP_THRESHOLD = 15000; // 15KB critical threshold
static const size_t WARNING_HEAP_THRESHOLD = 25000;  // 25KB warning threshold

// Waits up to timeoutMs for one line on the console. True only for "yes"; prints why not
// otherwise (any other answer cancels, silence times out). Feeds the loop watchdog.
static bool confirmYes(uint32_t timeoutMs = 10000)
{
    const unsigned long start = millis();
    while (millis() - start < timeoutMs)
    {
        esp_task_wdt_reset();
        if (Serial.available())
        {
            String confirmation = Serial.readStringUntil('\n');
            confirmation.trim();
            confirmation.toLowerCase();
            if (confirmation == "yes")
            {
                return true;
            }
            if (confirmation.length() > 0)
            {
                ESP_LOGI(TAG, "Operation cancelled.");
                return false;
            }
        }
        delay(100);
    }
    ESP_LOGI(TAG, "Confirmation timeout. Operation cancelled.");
    return false;
}

// Splits up to maxParts space-separated words of rest into parts; returns how many.
static int splitArgs(String rest, String *parts, int maxParts)
{
    int n = 0;
    while (n < maxParts && rest.length() > 0)
    {
        int sp = rest.indexOf(' ');
        parts[n++] = sp > 0 ? rest.substring(0, sp) : rest;
        rest = sp > 0 ? rest.substring(sp + 1) : "";
        rest.trim();
    }
    return n;
}

static void printEndpoints()
{
    Endpoints ep = loadEndpoints();
    ESP_LOGI(TAG, "API base: %s", ep.apiBase.c_str());
    ESP_LOGI(TAG, "Reverb:   %s://%s:%u (app key %s)", ep.reverbTls ? "wss" : "ws", ep.reverbHost.c_str(), ep.reverbPort, ep.appKey.c_str());
    ESP_LOGI(TAG, "%s", config.getString("ep_api", "").isEmpty() ? "(production defaults)" : "(bench override from NVS - 'endpoint clear' to revert)");
}

#ifdef TT_BENCH
// Gauge learning-cycle discharge floor (learndis): stop at/below this (loaded) so the
// controlled drain never drives the LFP cell into the CUV protection trip.
static const float LEARN_DISCHARGE_FLOOR_V = 2.60f;

// --- Gauge-based sleep-current measurement (`sleeptest`) ---
// The cell path cannot be measured with a series DMM (the burden resistance corrupts
// the gauge's impedance tracking), so the gauge itself is the ammeter: it stays awake
// through deep sleep and coulomb-counts everything the cell sources. A test records
// RemainingCapacity + wall time into RTC memory, deep-sleeps for N minutes on a timer
// (VBUS wake stub suppressed so it cannot hijack the timer), and the wake boot prints
// delta-mAh -> average mA. Variant flags isolate suspects against the baseline
// production sleep. Battery only (charging makes the coulomb delta meaningless);
// prefer >= 30 minute runs — RemCap has 1mAh resolution and IT applies corrections.
struct SleepTestState
{
    uint32_t magic;
    uint64_t startEpochUs;
    uint16_t startRemainingMAh;
    uint16_t startMillivolts;
    uint8_t railOn;
    uint8_t nfcAwake;
    uint8_t holdPads;
};
static RTC_DATA_ATTR SleepTestState gSleepTest = {};
static const uint32_t SLEEP_TEST_MAGIC = 0x51EE7E57; // "SLEEpTESt"

static uint64_t epochMicros()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

// Prints the result of a pending sleep test on the wake boot. Called right after
// battery.begin() so the gauge readings are fresh.
void Console::reportSleepTestIfAny()
{
    if (gSleepTest.magic != SLEEP_TEST_MAGIC)
    {
        return;
    }
    gSleepTest.magic = 0;

    const float hours = (epochMicros() - gSleepTest.startEpochUs) / 3600.0e6f;
    if (!battery.hasTelemetry() || hours <= 0.0f)
    {
        ESP_LOGE(TAG, "SLEEPTEST: woke without usable telemetry — result discarded.");
        return;
    }
    const int endRemainingMAh = battery.getRemainingCapacityMilliAmpHours();
    const int deltaMAh = (int)gSleepTest.startRemainingMAh - endRemainingMAh;
    const int endMillivolts = (int)(battery.getBatteryVoltage() * 1000.0f);

    ESP_LOGW(TAG, "================= SLEEP TEST RESULT =================");
    ESP_LOGW(TAG, "Variant: rail=%s, NFC=%s, pad-hold=%s",
             gSleepTest.railOn ? "ON" : "off",
             gSleepTest.nfcAwake ? "AWAKE" : "powered-down",
             gSleepTest.holdPads ? "ARMED" : "off");
    ESP_LOGW(TAG, "Slept %.1f min: RemCap %u -> %d mAh (delta %d mAh), %u -> %d mV",
             hours * 60.0f, gSleepTest.startRemainingMAh, endRemainingMAh,
             deltaMAh, gSleepTest.startMillivolts, endMillivolts);
    ESP_LOGW(TAG, "AVERAGE SLEEP CURRENT ~= %.1f mA", deltaMAh / hours);
    ESP_LOGW(TAG, "(1mAh resolution + IT corrections: compare variants relatively, >=30min runs)");
    ESP_LOGW(TAG, "=====================================================");
}

// Runs one sleep-current measurement. Replicates the production battery-sleep
// preparation, with knobs to leave individual suspects powered for A/B isolation.
static void runSleepTest(uint32_t minutes, bool railOn, bool nfcAwake, bool holdPads)
{
    if (battery.isVbusPresent())
    {
        ESP_LOGE(TAG, "SLEEPTEST: unplug USB first — a charging cell makes the delta meaningless.");
        return;
    }
    if (!battery.hasTelemetry())
    {
        ESP_LOGE(TAG, "SLEEPTEST: no battery telemetry available.");
        return;
    }

    gSleepTest.magic = SLEEP_TEST_MAGIC;
    gSleepTest.startEpochUs = epochMicros();
    gSleepTest.startRemainingMAh = battery.getRemainingCapacityMilliAmpHours();
    gSleepTest.startMillivolts = (uint16_t)(battery.getBatteryVoltage() * 1000.0f);
    gSleepTest.railOn = railOn;
    gSleepTest.nfcAwake = nfcAwake;
    gSleepTest.holdPads = holdPads;

    ESP_LOGW(TAG, "SLEEPTEST: %lu min, rail=%s, NFC=%s, pad-hold=%s — start RemCap %umAh @ %umV.",
             (unsigned long)minutes, railOn ? "ON" : "off", nfcAwake ? "AWAKE" : "powered-down",
             holdPads ? "ARMED" : "off", gSleepTest.startRemainingMAh, gSleepTest.startMillivolts);
    ESP_LOGW(TAG, "SLEEPTEST: result prints on the wake boot. A button press ends the run early");
    ESP_LOGW(TAG, "(still valid — real elapsed time is recorded).");

    if (!nfcAwake)
    {
        // Same PN532 power-down + bus parking as the production sleep path.
        SleepController::powerDownNfcForSleep();
    }
    battery.prepareForDeepSleep();
    if (railOn)
    {
        // Keep the 4V5 rail energized through the sleep (GPIO4 is already high).
        gpio_hold_en((gpio_num_t)Policy::PERIPHERAL_POWER_GPIO);
    }
    else
    {
        sleepController.disablePeripheralPower(); // drives GPIO4 low + latches it
    }
    if (holdPads)
    {
        gpio_deep_sleep_hold_en(); // the global digital-pad hold under test (~13mA suspect)
    }

    // Timer wake for the measurement window; buttons/pogo stay armed as an abort path.
    // The VBUS wake stub must NOT run here — it would re-arm a 20s timer and hijack
    // the measurement window.
    esp_set_deep_sleep_wake_stub(NULL);
    esp_sleep_enable_timer_wakeup((uint64_t)minutes * 60ULL * 1000000ULL);
    const uint64_t wakeMask = (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | (1ULL << 21) | (1ULL << 6);
    esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_HIGH);
    Serial.flush();
    delay(100);
    esp_deep_sleep_start(); // does not return; reportSleepTestIfAny() runs on the wake boot
}

// `chargemon` console command: one charger/gauge line per second without holding the loop.
static unsigned long gChargeMonStart = 0, gChargeMonNextAt = 0, gChargeMonUntil = 0;
void Console::serviceChargeMonitor()
{
    if (!gChargeMonUntil)
    {
        return;
    }
    const unsigned long now = millis();
    if ((long)(now - gChargeMonUntil) >= 0)
    {
        gChargeMonUntil = 0;
        return;
    }
    if ((long)(now - gChargeMonNextAt) >= 0)
    {
        gChargeMonNextAt += 1000;
        battery.printChargeMonitorLine((now - gChargeMonStart) / 1000);
    }
}
#endif // TT_BENCH

static void printHelp()
{
    ESP_LOGI(TAG, "--- Console commands (docs/CONSOLE.md) ---");
    ESP_LOGI(TAG, "System:");
    ESP_LOGI(TAG, "  help              - this list");
    ESP_LOGI(TAG, "  version           - firmware, hardware and IDF version");
    ESP_LOGI(TAG, "  faults            - active faults and recent fault events (docs/FAULTS.md)");
    ESP_LOGI(TAG, "  restart           - restart the device");
    ESP_LOGI(TAG, "  config / debug    - stored configuration / debug information");
    ESP_LOGI(TAG, "  efusemac | hubid  - hub UUID (decimal), MAC, label payload TT1:<uuid>:<pop>");
    ESP_LOGI(TAG, "  ft begin|end|show|enroll <secret> <pop> [serial]|test <name> [args]|ok|fail [reason]|abort|result|commit|clean|ship|reset [--all] - factory protocol (JSON lines, docs/FT-PROTOCOL.md)");
    ESP_LOGI(TAG, "  heap / stack      - heap and loop-stack usage");
    ESP_LOGI(TAG, "  timing / tasks    - loop section timing / FreeRTOS task stats");
    ESP_LOGI(TAG, "  diag [on|off]     - periodic diagnostics verbosity");
    ESP_LOGI(TAG, "  coredump / coredumperase - stored crash core dump summary / erase it");
    ESP_LOGI(TAG, "  endpoint [clear]  - backend endpoints; clear removes a bench override");
    ESP_LOGI(TAG, "  ota [check [id]]  - firmware update status / check");
    ESP_LOGI(TAG, "WiFi / Reverb:");
    ESP_LOGI(TAG, "  qr / reset / stats - provisioning QR / forget WiFi / connection status");
    ESP_LOGI(TAG, "  reverbstatus / reverbclean / reverbstart - WebSocket status / free it / start it");
    ESP_LOGI(TAG, "Battery / power:");
    ESP_LOGI(TAG, "  battery / battscan - battery status / scan the battery I2C bus");
    ESP_LOGI(TAG, "  batterycfg        - reapply 4V5 rail and charger defaults");
    ESP_LOGI(TAG, "  batteryclearflags / chargekick - clear charger fault flags / restart the charge cycle");
    ESP_LOGI(TAG, "  gaugefets / gaugeprot / gaugecuvfix - gauge FET control / protection read-back / CUV latch");
    ESP_LOGI(TAG, "  gaugebus [attach] - battery bus owner; attach takes it back from an external tool");
    ESP_LOGI(TAG, "  pdstatus          - CH224Q handshake (BC/QC/PD) + VBUS voltage");
    ESP_LOGI(TAG, "  power / poweron   - peripheral rail (GPIO4) status / enable");
    ESP_LOGI(TAG, "Sleep:");
    ESP_LOGI(TAG, "  sleep / sleep now - deep sleep if allowed / unconditionally");
    ESP_LOGI(TAG, "  sleepafter <ms> / cancelsleep / sleepstatus - schedule / cancel / sleep controller status");
    ESP_LOGI(TAG, "Files:");
    ESP_LOGI(TAG, "  sdtree / sdinfo   - card tree / size and free space");
    ESP_LOGI(TAG, "  sdbench / sdwritebench [chunkKB] [count] - card benchmarks");
    ESP_LOGI(TAG, "  dlstats / dlqueue / dlclear - download statistics / queue / drop the queue");
    ESP_LOGI(TAG, "  required / checkfiles / cleanup - required files / schedule missing ones / remove temp files");
    ESP_LOGI(TAG, "Audio:");
    ESP_LOGI(TAG, "  play [<path>] / pause / resume / stop - playback");
    ESP_LOGI(TAG, "  next / prev / playlist / track - playlist navigation and status");
    ESP_LOGI(TAG, "  volup / voldown / volume [n] - volume");
    ESP_LOGI(TAG, "  audio / amp on|off|auto - audio status / amplifier override");
    ESP_LOGI(TAG, "LED:");
    ESP_LOGI(TAG, "  ledon <hex> <0-255> / ledoff / pulse <hex> / rapid <hex> <count> - manual LED control");
    ESP_LOGI(TAG, "NFC:");
    ESP_LOGI(TAG, "  nfcstatus / nfcdata / nfcreed / nfcdiag - reader status / current card / pogo switch / diagnostics");
    ESP_LOGI(TAG, "  nfcsleep          - PN532 software power-down");
#ifdef TT_BENCH
    ESP_LOGI(TAG, "Bench (TT_BENCH):");
    ESP_LOGI(TAG, "  endpoint set <api_base> <reverb_host> <port> <tls 0|1> [key] - point the unit at a bench server (NVS)");
    ESP_LOGI(TAG, "  factory / delete / deletefig <uid> - erase all data / required files / one figure (asks yes)");
    ESP_LOGI(TAG, "  sdformat          - delete everything on the card");
    ESP_LOGI(TAG, "  download <url> <path> / addfile <path> <url> / deletefile <path> - manual file management");
    ESP_LOGI(TAG, "  ota url <url> <sha256> <size> [version] - install an arbitrary image");
    ESP_LOGI(TAG, "  send <message> / testauth - Reverb broadcast (the portal refuses device chat) / token test without TLS checks");
    ESP_LOGI(TAG, "  poweroff          - disable the peripheral rail (asks yes)");
    ESP_LOGI(TAG, "  cat <path>        - print the first 4 KB of a file on the card");
    ESP_LOGI(TAG, "  tap <uid> / untap - simulate mounting / removing a figure");
    ESP_LOGI(TAG, "  startsystem       - start the full system bypassing the battery gate");
    ESP_LOGI(TAG, "  bringupreset      - re-run the first-boot gauge bring-up");
    ESP_LOGI(TAG, "  gaugebus release  - hand the battery bus to bqStudio (EV2400), persists");
    ESP_LOGI(TAG, "  gauge1s / gaugeprog / gaugeresetlearn / gaugelearned / gaugecalcurrent - gauge provisioning tools");
    ESP_LOGI(TAG, "  dfread <hexaddr> [len] / dfwrite8 <hexaddr> <hexval> - gauge data flash");
    ESP_LOGI(TAG, "  gaugedump [hexstart hexend] - hex dump of the gauge data flash (default 4000-4800, the golden image)");
    ESP_LOGI(TAG, "  gaugemac <hexcmd> [len] / gaugeit - raw MAC block / Impedance Track internals (IT Status 1-3)");
    ESP_LOGI(TAG, "  learndis / learnrest / learnchg - gauge learning cycle phases");
    ESP_LOGI(TAG, "  sleeptest <min> [railon] [nfcawake] [hold] - sleep current via the gauge (battery only)");
    ESP_LOGI(TAG, "  chargemon [s]     - one charger/gauge line per second");
    ESP_LOGI(TAG, "  pdvolt <5|9|12|15|20> - request another USB-PD voltage");
    ESP_LOGI(TAG, "  crash             - panic on purpose (crash-loop guard tests)");
#else
    ESP_LOGI(TAG, "(bench commands are compiled only in env:bench, -DTT_BENCH)");
#endif
}

static void dispatch(String command)
{
    if (command == "help")
    {
        printHelp();
    }
    else if (command == "ft" || command.startsWith("ft "))
    {
        FactoryTest::command(command.length() > 3 ? command.substring(3) : String());
    }
#ifdef TT_BENCH
    // The portal does not accept chat from a device (401/403): bench only.
    else if (command.startsWith("send "))
    {
        String message = command.substring(5); // Get the text after "send "
        message.trim();
        if (message.length() > 0)
        {
            ESP_LOGI(TAG, "Sending message: '%s'", message.c_str());
            if (reverb.sendMessage(message))
            {
                ESP_LOGI(TAG, "Message sent to API for broadcast.");
            }
            else
            {
                ESP_LOGE(TAG, "Failed to send message.");
            }
        }
        else
        {
            ESP_LOGI(TAG, "Usage: send <your message>");
        }
    }
#endif // TT_BENCH

    // WiFi related commands
    else if (command == "qr" || command == "reset" || command == "stats")
    {
        wifiProv.handleCommand(command);
    }
    // Reverb related commands
    else if (command == "reverbstatus")
    {
        ESP_LOGI(TAG, "Reverb Status: %s", reverb.isConnected() ? "Connected" : "Disconnected");
        ESP_LOGI(TAG, "WiFi Status: %s", WiFi.isConnected() ? "Connected" : "Disconnected");
        ESP_LOGI(TAG, "Free heap: %d bytes", ESP.getFreeHeap());
    }
    else if (command == "reverbclean")
    {
        ESP_LOGI(TAG, "Cleaning up Reverb client...");
        reverb.cleanup();
    }
    else if (command == "reverbstart")
    {
        if (WiFi.isConnected())
        {
            ESP_LOGI(TAG, "Starting Reverb client...");

            startReverb();
        }
        else
        {
            ESP_LOGW(TAG, "Cannot start Reverb - WiFi not connected");
        }
    }
    else if (command == "efusemac" || command == "hubid")
    {
        const String deviceId = getDeviceEfuseMacDecimal();
        const String deviceMacHex = getDeviceEfuseMacHex();

        const String label = FactoryTest::labelPayload();
        ESP_LOGI(TAG, "--- Device Identity ---");
        ESP_LOGI(TAG, "Hub UUID / efuse MAC (decimal): %s", deviceId.c_str());
        ESP_LOGI(TAG, "Base MAC (hex): %s", deviceMacHex.c_str());
        ESP_LOGI(TAG, "Factory identity: %s", config.isEnrolled() ? "enrolled" : "not enrolled (ft enroll)");
        ESP_LOGI(TAG, "Label payload: %s", label.length() ? label.c_str() : "(no pop enrolled)");
    }
#ifdef TT_BENCH
    else if (command == "testauth")
    {
        ESP_LOGI(TAG, "--- Testing Authorization ---");
        String token = config.getJWTToken();

        if (token.length() == 0)
        {
            ESP_LOGE(TAG, "No JWT token stored in configuration");
            return;
        }

        if (!WiFi.isConnected())
        {
            ESP_LOGE(TAG, "WiFi not connected - cannot test authorization");
            return;
        }

        ESP_LOGI(TAG, "JWT Token found, testing with server...");
        ESP_LOGI(TAG, "Token length: %d characters", token.length());

        // Use the same method as ReverbClient for consistency
        WiFiClientSecure client;
        client.setInsecure(); // For testing only

        HTTPClient http;
        String url = String(Policy::API_BASE_URL) + "/user"; // Simple endpoint to test auth (production)

        if (http.begin(client, url))
        {
            http.addHeader("Authorization", "Bearer " + token);
            http.addHeader("Accept", "application/json");

            ESP_LOGI(TAG, "Sending auth test request...");
            int httpCode = http.GET();

            if (httpCode == 200)
            {
                ESP_LOGI(TAG, "Authorization successful! Token is valid.");
                String response = http.getString();
                ESP_LOGI(TAG, "Server response: %s", response.c_str());
            }
            else if (httpCode == 401)
            {
                ESP_LOGE(TAG, "Authorization failed! Token is invalid or expired.");
            }
            else if (httpCode > 0)
            {
                ESP_LOGW(TAG, "Unexpected response code: %d", httpCode);
                String response = http.getString();
                ESP_LOGW(TAG, "Response: %s", response.c_str());
            }
            else
            {
                ESP_LOGE(TAG, "HTTP request failed with error: %d", httpCode);
            }

            http.end();
        }
        else
        {
            ESP_LOGE(TAG, "Failed to connect to server");
        }
    }
    else if (command == "factory")
    {
        ESP_LOGW(TAG, "WARNING: Factory reset will erase ALL stored data!");
        ESP_LOGI(TAG, "Type 'yes' within 10 s to confirm, anything else cancels:");
        if (confirmYes())
        {
            config.factoryReset();
        }
    }
#endif // TT_BENCH
    // File Manager commands
    else if (command == "sdtree")
    {
        fileManager.printFileTree();
    }
#ifdef TT_BENCH
    else if (command.startsWith("cat "))
    {
        // cat <path> - print the first 4 KB of a text file on the SD card (bench)
        String path = command.substring(4);
        path.trim();
        File f = SD_MMC.open(path, FILE_READ);
        if (!f)
        {
            ESP_LOGE(TAG, "cat: cannot open %s", path.c_str());
        }
        else
        {
            size_t shown = 0;
            while (f.available() && shown < 4096)
            {
                String line = f.readStringUntil('\n');
                shown += line.length() + 1;
                Serial.println(line);
            }
            if (f.available()) Serial.println("... (truncated)");
            f.close();
        }
    }
#endif // TT_BENCH
    else if (command.startsWith("sdwritebench"))
    {
        // sdwritebench [chunkKB] [count]
        String args = command.substring(12);
        args.trim();
        int chunkKb = 32, count = 64;
        if (args.length() > 0)
        {
            int sp = args.indexOf(' ');
            chunkKb = (sp > 0 ? args.substring(0, sp) : args).toInt();
            if (sp > 0) count = args.substring(sp + 1).toInt();
        }
        fileManager.benchmarkSequentialWrite((size_t)chunkKb * 1024, count);
    }
#ifdef TT_BENCH
    // No confirmation and arbitrary URLs/paths: bench only (docs/SYSTEM-AUDIT-2026-10-22.md).
    else if (command == "sdformat")
    {
        fileManager.formatSDCard();
    }
    else if (command.startsWith("download "))
    {
        // Parse download command: download <url> <path>
        int firstSpace = command.indexOf(' ');                  // Find first space (after "download")
        int secondSpace = command.indexOf(' ', firstSpace + 1); // Find second space

        if (firstSpace != -1 && secondSpace != -1)
        {
            String url = command.substring(firstSpace + 1, secondSpace);
            String path = command.substring(secondSpace + 1);

            ESP_LOGI(TAG, "Scheduling download: %s -> %s", url.c_str(), path.c_str());
            if (fileManager.scheduleDownload(url, path))
            {
                ESP_LOGI(TAG, "Download scheduled successfully");
            }
            else
            {
                ESP_LOGE(TAG, "Failed to schedule download");
            }
        }
        else
        {
            ESP_LOGI(TAG, "Usage: download <url> <local_path>");
            ESP_LOGI(TAG, "Example: download http://example.com/audio.mp3 /audio/test.mp3");
        }
    }
    else if (command.startsWith("addfile "))
    {
        // Parse addfile command: addfile <path> <url>
        int firstSpace = command.indexOf(' ');                  // Find first space (after "addfile")
        int secondSpace = command.indexOf(' ', firstSpace + 1); // Find second space

        if (firstSpace != -1 && secondSpace != -1)
        {
            String path = command.substring(firstSpace + 1, secondSpace);
            String url = command.substring(secondSpace + 1);

            ESP_LOGI(TAG, "Adding required file: %s <- %s", path.c_str(), url.c_str());
            if (fileManager.addRequiredFile(path, url))
            {
                ESP_LOGI(TAG, "Required file added successfully");
            }
            else
            {
                ESP_LOGE(TAG, "Failed to add required file");
            }
        }
        else
        {
            ESP_LOGI(TAG, "Usage: addfile <local_path> <url>");
            ESP_LOGI(TAG, "Example: addfile /audio/sound.mp3 http://example.com/audio.mp3");
        }
    }
    else if (command.startsWith("deletefile "))
    {
        // Parse deletefile command: deletefile <path>
        int firstSpace = command.indexOf(' ');

        if (firstSpace != -1)
        {
            String filePath = command.substring(firstSpace + 1);
            filePath.trim(); // Remove any extra whitespace

            if (filePath.isEmpty())
            {
                ESP_LOGI(TAG, "Usage: deletefile <file_path>");
                ESP_LOGI(TAG, "Example: deletefile /images/image.webp");
            }
            else
            {
                ESP_LOGI(TAG, "Deleting file and removing from required list: %s", filePath.c_str());
                if (fileManager.deleteFileAndRemoveFromRequired(filePath))
                {
                    ESP_LOGI(TAG, "File deleted successfully");
                }
                else
                {
                    ESP_LOGW(TAG, "Failed to delete file (file may not exist)");
                }
            }
        }
        else
        {
            ESP_LOGI(TAG, "Usage: deletefile <file_path>");
            ESP_LOGI(TAG, "Example: deletefile /images/image.webp");
            ESP_LOGI(TAG, "Note: This will also remove the file from required list to prevent re-download");
        }
    }
    // Delete all required files command
    else if (command == "delete")
    {
        ESP_LOGW(TAG, "WARNING: This will delete ALL required files from NVS and storage!");
        ESP_LOGI(TAG, "Are you sure? Type 'yes' to confirm:");
        if (confirmYes())
        {
            ESP_LOGI(TAG, "Confirmation received. Deleting all required files...");
            fileManager.clearAllRequiredFiles();
            ESP_LOGI(TAG, "All required files have been deleted from NVS and storage.");
        }
    }
#endif // TT_BENCH
    else if (command.startsWith("sdbench"))
    {
        fileManager.benchmarkSDCard();
    }
#ifdef TT_BENCH
    // Delete one figure's cached stories + manifest
    else if (command.startsWith("deletefig "))
    {
        String figureUid = command.substring(10);
        figureUid.trim();
        if (figureUid.isEmpty())
        {
            ESP_LOGI(TAG, "Usage: deletefig <figure_uid>");
            ESP_LOGI(TAG, "Note: This will delete all files associated with the figure");
        }
        else
        {
            ESP_LOGW(TAG, "WARNING: This will delete all files for figure (UID: %s)", figureUid.c_str());
            ESP_LOGI(TAG, "Type 'yes' to confirm deletion, or 'no' to cancel:");
            if (confirmYes())
            {
                if (requestManager.forgetFigure(figureUid))
                {
                    ESP_LOGI(TAG, "Successfully deleted all files for figure (UID: %s)", figureUid.c_str());
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to delete files for figure (UID: %s)", figureUid.c_str());
                }
            }
        }
    }
#endif // TT_BENCH
    // Audio commands
    else if (command.startsWith("play "))
    {
        // Parse play command: play <path>
        int firstSpace = command.indexOf(' ');

        if (firstSpace != -1)
        {
            String filePath = command.substring(firstSpace + 1);
            filePath.trim();

            if (filePath.isEmpty())
            {
                ESP_LOGI(TAG, "Usage: play <file_path>");
                ESP_LOGI(TAG, "Example: play /audio/song.mp3");
            }
            else
            {
                ESP_LOGI(TAG, "Playing: %s", filePath.c_str());
                if (audioController.play(filePath))
                {
                    ESP_LOGI(TAG, "Playback started successfully");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to start playback");
                }
            }
        }
        else
        {
            ESP_LOGI(TAG, "Usage: play <file_path>");
            ESP_LOGI(TAG, "Example: play /audio/song.mp3");
        }
    }
    else if (command == "play")
    {
        // Play playlist or resume
        if (audioController.play())
        {
            if (audioController.hasPlaylist())
            {
                ESP_LOGI(TAG, "Playing playlist track %d/%d",
                         audioController.getCurrentTrackIndex() + 1,
                         audioController.getPlaylistSize());
            }
            else
            {
                ESP_LOGI(TAG, "Playback resumed");
            }
        }
        else
        {
            ESP_LOGW(TAG, "No playlist available or failed to start playback");
        }
    }
    else if (command == "pause")
    {
        if (audioController.pause())
        {
            ESP_LOGI(TAG, "Playback paused");
        }
        else
        {
            ESP_LOGW(TAG, "Nothing to pause or already paused");
        }
    }
    else if (command == "resume")
    {
        if (audioController.resume())
        {
            ESP_LOGI(TAG, "Playback resumed");
        }
        else
        {
            ESP_LOGW(TAG, "Nothing to resume or not paused");
        }
    }
    else if (command == "stop")
    {
        if (audioController.stop())
        {
            ESP_LOGI(TAG, "Playback stopped");
        }
        else
        {
            ESP_LOGW(TAG, "Nothing to stop or already stopped");
        }
    }
    else if (command == "next")
    {
        if (audioController.nextTrack())
        {
            ESP_LOGI(TAG, "Playing next track: %d/%d",
                     audioController.getCurrentTrackIndex() + 1,
                     audioController.getPlaylistSize());
        }
        else
        {
            ESP_LOGW(TAG, "No playlist available or reached end of playlist");
        }
    }
    else if (command == "prev")
    {
        if (audioController.prevTrack())
        {
            ESP_LOGI(TAG, "Playing previous track: %d/%d",
                     audioController.getCurrentTrackIndex() + 1,
                     audioController.getPlaylistSize());
        }
        else
        {
            ESP_LOGW(TAG, "No playlist available");
        }
    }
    else if (command == "playlist")
    {
        if (audioController.hasPlaylist())
        {
            ESP_LOGI(TAG, "Current playlist (Figure UID: %s):",
                     audioController.getPlaylistFigureUid().c_str());
            ESP_LOGI(TAG, "Current track: %d/%d",
                     audioController.getCurrentTrackIndex() + 1,
                     audioController.getPlaylistSize());

            // Print playlist tracks (limit to 10 for readability)
            int maxTracks = min(10, audioController.getPlaylistSize());
            for (int i = 0; i < maxTracks; i++)
            {
                String indicator = (i == audioController.getCurrentTrackIndex()) ? " -> " : "    ";
                ESP_LOGI(TAG, "%s%d. Track %d", indicator.c_str(), i + 1, i + 1);
            }

            if (audioController.getPlaylistSize() > 10)
            {
                ESP_LOGI(TAG, "    ... and %d more tracks",
                         audioController.getPlaylistSize() - 10);
            }
        }
        else
        {
            ESP_LOGI(TAG, "No playlist loaded");
        }
    }
    else if (command == "volup")
    {
        if (audioController.volumeUp())
        {
            ESP_LOGI(TAG, "Volume increased to %d%%", audioController.getCurrentVolume());
        }
        else
        {
            ESP_LOGW(TAG, "Volume already at maximum");
        }
    }
    else if (command == "voldown")
    {
        if (audioController.volumeDown())
        {
            ESP_LOGI(TAG, "Volume decreased to %d%%", audioController.getCurrentVolume());
        }
        else
        {
            ESP_LOGW(TAG, "Volume already at minimum");
        }
    }
    else if (command.startsWith("volume "))
    {
        int requested = command.substring(7).toInt();
        audioController.setVolume(requested);
        ESP_LOGI(TAG, "Volume now %d%% (ceiling %d%%)", audioController.getCurrentVolume(), audioController.getVolumeCeiling());
    }
    else if (command == "volume")
    {
        ESP_LOGI(TAG, "Current volume: %d%% (ceiling %d%%)", audioController.getCurrentVolume(), audioController.getVolumeCeiling());
    }
    else if (command == "audio")
    {
        audioController.printStatus();
    }
#ifdef TT_BENCH
    else if (command.startsWith("chargemon"))
    {
        // chargemon [seconds]: one line per second of charger + gauge state, printed from the loop
        String arg = command.substring(9);
        arg.trim();
        int seconds = arg.isEmpty() ? 30 : arg.toInt();
        if (seconds < 1) seconds = 1;
        if (seconds > 300) seconds = 300;
        gChargeMonStart = millis();
        gChargeMonNextAt = gChargeMonStart;
        gChargeMonUntil = gChargeMonStart + (unsigned long)seconds * 1000UL;
    }
#endif // TT_BENCH
    else if (command == "sdinfo")
    {
        ESP_LOGI(TAG, "SD: card %llu MB, FAT volume %llu MB, used %llu MB, free %u MB (cached)",
                 (unsigned long long)(SD_MMC.cardSize() >> 20), (unsigned long long)(SD_MMC.totalBytes() >> 20),
                 (unsigned long long)(SD_MMC.usedBytes() >> 20), (unsigned)fileManager.getSDCardFreeMB());
    }
    else if (command == "dlclear")
    {
        fileManager.cancelAllDownloads();
    }
    else if (command == "gaugebus" || command.startsWith("gaugebus "))
    {
        String arg = command.substring(8);
        arg.trim();
        // release is bench-only; attach and the status stay in production so a stray release
        // (persisted in NVS) can always be undone.
#ifdef TT_BENCH
        if (arg == "release")
        {
            config.storeInt(GAUGEBUS_NVS_KEY, 1);
            gGaugeBusReleased = true;
            battery.setBusReleased(true);
            ESP_LOGW(TAG, "GAUGEBUS: released. Connect the EV2400 to BATT_SDA/BATT_SCL/GND. The charger keeps its current settings; the firmware will not poll or sleep on VBUS loss - use 'sleep now' for rest periods.");
        }
        else
#endif
        if (arg == "attach")
        {
            config.deleteSetting(GAUGEBUS_NVS_KEY);
            gGaugeBusReleased = false;
            battery.setBusReleased(false);
            ESP_LOGI(TAG, "GAUGEBUS: attached, polling resumes");
        }
        else
        {
            ESP_LOGI(TAG, "GAUGEBUS: %s (gaugebus release | gaugebus attach)", gGaugeBusReleased ? "RELEASED to an external tool" : "attached (firmware owns the bus)");
        }
    }
    else if (command == "sleep now")
    {
        ESP_LOGI(TAG, "Entering deep sleep unconditionally...");
        delay(100);
        sleepController.forceSleep();
    }
#ifdef TT_BENCH
    else if (command == "crash")
    {
        ESP_LOGE(TAG, "crash requested from the console (panic)");
        delay(50);
        abort();
    }
#endif // TT_BENCH
    else if (command == "version")
    {
        ESP_LOGI(TAG, "Firmware %s (code %u), hardware %s, IDF %s", firmwareVersion(), (unsigned)firmwareVersionCode(),
                 TT_HARDWARE_REV, esp_app_get_description()->idf_ver);
    }
    else if (command == "faults")
    {
        Faults::printStatus();
    }
    else if (command.startsWith("diag"))
    {
        String arg = command.substring(4);
        arg.trim();
        if (arg == "on") Diag::setVerbose(true);
        else if (arg == "off") Diag::setVerbose(false);
        ESP_LOGI(TAG, "Periodic diagnostics: %s", Diag::verbose() ? "verbose (section table every 10 s)" : "quiet (one line a minute)");
    }
    else if (command.startsWith("ota"))
    {
        // ota | ota check [release_id] | ota url <url> <sha256> <size> [version]
        String arg = command.substring(3);
        arg.trim();
        OtaManager &ota = OtaManager::getInstance();
        if (arg.isEmpty())
        {
            ota.printStatus();
        }
        else if (arg.startsWith("check"))
        {
            String id = arg.substring(5);
            id.trim();
            ota.checkNow(id.isEmpty() ? 0 : (uint32_t)id.toInt(), "console");
        }
#ifdef TT_BENCH
        // Any image with a hash the operator supplies: bench only.
        else if (arg.startsWith("url "))
        {
            String parts[4];
            const int n = splitArgs(arg.substring(4), parts, 4);
            if (n < 3)
            {
                ESP_LOGW(TAG, "usage: ota url <url> <sha256> <size> [version]");
            }
            else
            {
                OtaManager::Job job;
                job.url = parts[0];
                job.sha256 = parts[1];
                job.size = (size_t)parts[2].toInt();
                job.version = n > 3 ? parts[3] : String("bench");
                job.releaseId = 0;
                ota.startJob(job);
            }
        }
#endif // TT_BENCH
    }
    else if (command.startsWith("amp"))
    {
        // amp on|off|auto - speaker amplifier override for bench diagnostics
        String arg = command.substring(3);
        arg.trim();
        if (arg == "on")
            audioController.setAmpMode(AudioController::AMP_FORCE_ON);
        else if (arg == "off")
            audioController.setAmpMode(AudioController::AMP_FORCE_OFF);
        else if (arg == "auto")
            audioController.setAmpMode(AudioController::AMP_AUTO);
        audioController.printStatus();
    }
    else if (command == "track")
    {
        String track = audioController.getCurrentTrack();
        if (track.isEmpty())
        {
            ESP_LOGI(TAG, "No track currently loaded");
        }
        else
        {
            ESP_LOGI(TAG, "Current track: %s", track.c_str());
            ESP_LOGI(TAG, "Status: %s",
                     audioController.isPlaying() ? "Playing" : audioController.isPaused() ? "Paused"
                                                                                          : "Stopped");
        }
    }
    // Power control commands
    else if (command == "power")
    {
        bool powerState = digitalRead(Policy::PERIPHERAL_POWER_GPIO);
        ESP_LOGI(TAG, "Peripheral power (GPIO4): %s", powerState ? "ENABLED" : "DISABLED");
        ESP_LOGI(TAG, "Pin state: %s", powerState ? "HIGH" : "LOW");
        if (!powerState)
        {
            ESP_LOGW(TAG, "WARNING: Peripherals (SD card, etc.) will not work with power disabled!");
            ESP_LOGI(TAG, "Use 'poweron' command to enable peripheral power.");
        }
    }
    else if (command == "poweron")
    {
        ESP_LOGI(TAG, "Enabling peripheral power...");
        digitalWrite(Policy::PERIPHERAL_POWER_GPIO, HIGH);
        delay(100);
        ESP_LOGI(TAG, "Peripheral power ENABLED");
        ESP_LOGI(TAG, "You may need to reinitialize modules (restart recommended)");
    }
#ifdef TT_BENCH
    else if (command == "poweroff")
    {
        ESP_LOGW(TAG, "WARNING: This will disable power to SD card and other peripherals!");
        ESP_LOGI(TAG, "Type 'yes' within 10 s to confirm, anything else cancels:");
        if (confirmYes())
        {
            digitalWrite(Policy::PERIPHERAL_POWER_GPIO, LOW);
            ESP_LOGI(TAG, "Peripheral power DISABLED");
        }
    }
#endif // TT_BENCH
    // LED commands
    else if (command.startsWith("ledon"))
    {
        // Parse command: ledon <hex_color> <intensity>
        // Example: ledon FF0000 128 (red color with 128 intensity)
        String params = command.substring(5); // Remove "ledon"
        params.trim();

        int spaceIndex = params.indexOf(' ');
        if (spaceIndex > 0)
        {
            String hexStr = params.substring(0, spaceIndex);
            String intensityStr = params.substring(spaceIndex + 1);

            // Convert hex string to uint32_t
            uint32_t hexColor = strtol(hexStr.c_str(), NULL, 16);
            int intensity = intensityStr.toInt();

            ledController.simpleLed(hexColor, intensity);
            ESP_LOGI(TAG, "LED set to color: 0x%06X, intensity: %d", hexColor, intensity);
        }
        else
        {
            ESP_LOGI(TAG, "Usage: ledon <hex_color> <intensity>");
            ESP_LOGI(TAG, "Example: ledon FF0000 128 (red color with 128 intensity)");
        }
    }
    else if (command == "ledoff")
    {
        ledController.turnOff();
        ESP_LOGI(TAG, "LED turned off");
    }
    else if (command.startsWith("pulse"))
    {
        // Parse command: pulse <hex_color>
        // Example: pulse 00FF00 (green pulsing)
        String params = command.substring(5); // Remove "pulse"
        params.trim();

        if (params.length() > 0)
        {
            uint32_t hexColor = strtol(params.c_str(), NULL, 16);
            ledController.pulseLed(hexColor);
            ESP_LOGI(TAG, "LED pulsing started with color: 0x%06X", hexColor);
        }
        else
        {
            ESP_LOGI(TAG, "Usage: pulse <hex_color>");
            ESP_LOGI(TAG, "Example: pulse 00FF00 (green pulsing)");
        }
    }
    else if (command.startsWith("rapid"))
    {
        // Parse command: rapid <hex_color> <count>
        // Example: rapid 0000FF 5 (blue rapid pulse 5 times)
        String params = command.substring(5); // Remove "rapid"
        params.trim();

        int spaceIndex = params.indexOf(' ');
        if (spaceIndex > 0)
        {
            String hexStr = params.substring(0, spaceIndex);
            String countStr = params.substring(spaceIndex + 1);

            uint32_t hexColor = strtol(hexStr.c_str(), NULL, 16);
            int count = countStr.toInt();

            ledController.pulseRapid(hexColor, count);
            ESP_LOGI(TAG, "LED rapid pulse started with color: 0x%06X, count: %d", hexColor, count);
        }
        else
        {
            ESP_LOGI(TAG, "Usage: rapid <hex_color> <count>");
            ESP_LOGI(TAG, "Example: rapid 0000FF 5 (blue rapid pulse 5 times)");
        }
    }
#ifdef TT_BENCH
    // NFC commands
    else if (command.startsWith("tap "))
    {
        // tap <uid> - simulate mounting a figure (bench), e.g. tap 04-1F-76-D2-56-68-80
        String uid = command.substring(4);
        uid.trim();
        uid.toUpperCase();
        if (uid.isEmpty())
        {
            ESP_LOGI(TAG, "Usage: tap <nfc-uid>");
        }
        else
        {
            sleepController.resetActivity();
            nfcController.simulateCard(uid);
        }
    }
    else if (command == "untap")
    {
        nfcController.clearSimulation();
    }
#endif // TT_BENCH
    else if (command == "nfcstatus")
    {
        ESP_LOGI(TAG, "--- NFC Controller Status ---");
        ESP_LOGI(TAG, "NFC Ready: %s", nfcController.isNFCReady() ? "Yes" : "No");
        ESP_LOGI(TAG, "Pogo Switch Active: %s", nfcController.isPogoSwitchActive() ? "Yes" : "No");
        ESP_LOGI(TAG, "Card Present: %s", nfcController.isCardPresent() ? "Yes" : "No");
        ESP_LOGI(TAG, "-----------------------------");
    }
    else if (command == "nfcdata")
    {
        NFCData currentCard = nfcController.currentNFCData();
        ESP_LOGI(TAG, "--- Currently Docked NFC Card ---");
        if (currentCard.isValid)
        {
            ESP_LOGI(TAG, "UID: %s", currentCard.uidString.c_str());
            ESP_LOGI(TAG, "UID Length: %d", currentCard.uidLength);
            ESP_LOGI(TAG, "Timestamp: %lu", currentCard.timestamp);
        }
        else
        {
            ESP_LOGI(TAG, "No card is currently docked.");
        }
        ESP_LOGI(TAG, "----------------------------------");
    }
    else if (command == "nfcpogo" || command == "nfcreed")
    {
        bool rawPogoState = digitalRead(POGO_SWITCH_PIN);
        ESP_LOGI(TAG, "--- Pogo Switch Status ---");
        ESP_LOGI(TAG, "Raw Pin State (GPIO6): %s", rawPogoState ? "HIGH" : "LOW");
        ESP_LOGI(TAG, "Debounced Controller State: %s", nfcController.isPogoSwitchActive() ? "Active" : "Inactive");
        ESP_LOGI(TAG, "-------------------------");
    }
    else if (command == "nfcdiag")
    {
        nfcController.diagnostics();
    }
    else if (command == "pdstatus")
    {
        // What did the CH224Q negotiate with this USB source? Decodes its status
        // register (which handshake succeeded) plus the charger's VBUS ADC (the
        // voltage actually delivered). Diagnoses source-compatibility problems
        // (e.g. power banks that refuse or drop the 9V request).
        ESP_LOGI(TAG, "--- USB-PD Sink Status (CH224Q) ---");
        ESP_LOGI(TAG, "VBUS present: %s, VBUS = %.3fV",
                 battery.isVbusPresent() ? "yes" : "no", battery.getVbusVoltage());
        uint8_t status = 0;
        uint16_t grantedMa = 0;
        if (!battery.readUsbPdStatus(status, grantedMa))
        {
            ESP_LOGW(TAG, "CH224Q not answering (it is VBUS-powered — is USB plugged?).");
        }
        else
        {
            ESP_LOGI(TAG, "Status 0x%02X: BC1.2=%d QC2=%d QC3=%d PD=%d EPR-act=%d EPR-cap=%d AVS-cap=%d",
                     status, (status >> 0) & 1, (status >> 1) & 1, (status >> 2) & 1,
                     (status >> 3) & 1, (status >> 4) & 1, (status >> 5) & 1, (status >> 6) & 1);
            if ((status >> 3) & 1)
            {
                ESP_LOGI(TAG, "PD granted current budget: %umA", grantedMa);
            }
        }
        ESP_LOGI(TAG, "-----------------------------------");
    }
#ifdef TT_BENCH
    else if (command.startsWith("pdvolt "))
    {
        // pdvolt <5|9|12|15|20> — bench experiment: change the requested PD gear.
        const long volts = command.substring(7).toInt();
        int8_t gear = -1;
        switch (volts)
        {
        case 5:  gear = 0; break;
        case 9:  gear = 1; break;
        case 12: gear = 2; break;
        case 15: gear = 3; break;
        case 20: gear = 4; break;
        }
        if (gear < 0)
        {
            ESP_LOGI(TAG, "Usage: pdvolt <5|9|12|15|20>");
        }
        else if (battery.setUsbPdVoltageGear((uint8_t)gear))
        {
            ESP_LOGI(TAG, "Requested %ldV from the source — check 'pdstatus' / VBUS in a few seconds.", volts);
        }
        else
        {
            ESP_LOGW(TAG, "CH224Q not answering (VBUS-powered — is USB plugged?).");
        }
    }
    else if (command.startsWith("sleeptest"))
    {
        // sleeptest <minutes> [railon] [nfcawake] [hold]
        long minutes = command.substring(9).toInt(); // toInt skips leading whitespace
        if (minutes <= 0)
        {
            minutes = 30;
        }
        const bool railOn = command.indexOf("railon") > 0;
        const bool nfcAwake = command.indexOf("nfcawake") > 0;
        const bool holdPads = command.indexOf(" hold") > 0;
        runSleepTest((uint32_t)minutes, railOn, nfcAwake, holdPads);
        // Only returns on a refused start (USB present / no telemetry).
    }
#endif // TT_BENCH
    else if (command == "nfcsleep")
    {
        ESP_LOGI(TAG, "Sending PN532 PowerDown — compare 'battery' current before/after (expect ~30-40mA drop).");
        nfcController.powerDown();
        ESP_LOGI(TAG, "PN532 stays down until 'restart' (or next NFC begin()).");
    }

    // Battery commands
    else if (command == "battery")
    {
        battery.refreshDiagnostics(); // the slow gauge/charger reads are no longer polled every second
        battery.printBatteryInfo();
    }
    else if (command == "battscan")
    {
        battery.printBusScan();
    }
    else if (command == "batterycfg")
    {
        bool success = battery.reconfigure(false);
        ESP_LOGI(TAG, "Battery bring-up reapply: %s", success ? "OK" : "FAILED");
        battery.printBatteryInfo();
    }
    else if (command == "batteryclearflags")
    {
        bool success = battery.clearChargerFaultHistory();
        ESP_LOGI(TAG, "Charger fault history clear: %s", success ? "OK" : "FAILED");
        battery.printBatteryInfo();
    }
    else if (command == "chargekick")
    {
        bool success = battery.restartChargeCycle();
        ESP_LOGI(TAG, "Charge cycle restart: %s", success ? "OK" : "FAILED");
        battery.printBatteryInfo();
    }
    else if (command == "gaugefets")
    {
        bool success = battery.restoreGaugeFetControl();
        ESP_LOGI(TAG, "Gauge FET control restore: %s", success ? "OK" : "FAILED");
        battery.printBatteryInfo();
    }
#ifdef TT_BENCH
    else if (command == "gauge1s")
    {
        bool success = battery.setGaugeSingleCellMode();
        ESP_LOGI(TAG, "Gauge single-cell configuration: %s", success ? "OK" : "FAILED");
        battery.printBatteryInfo();
    }
    else if (command == "gaugeprog")
    {
        bool success = battery.forceGaugeProvisioning();
        ESP_LOGI(TAG, "Gauge provisioning: %s", success ? "OK" : "FAILED");
        battery.printBatteryInfo();
    }
    else if (command == "gaugeresetlearn")
    {
        bool success = battery.resetGaugeLearningState();
        ESP_LOGI(TAG, "Gauge learning reset: %s", success ? "OK" : "FAILED");
        battery.printBatteryInfo();
    }
    // gaugecalcurrent: calibrate the gauge current sense (CC Gain) against the
    // BQ25792 IBAT ADC. Must be run WHILE a steady current flows — keep USB plugged
    // and the cell charging (~300mA stopgap) so charger IBAT and gauge current are
    // both >150mA. Fixes the ~4.5x over-read that trips OCC.
    else if (command == "gaugecalcurrent")
    {
        ESP_LOGI(TAG, "Calibrating gauge current sense vs charger IBAT...");
        bool success = battery.calibrateCurrentSense();
        ESP_LOGI(TAG, "Current calibration: %s", success ? "OK" : "FAILED");
        battery.printBatteryInfo();
    }
    // gaugeprot: read back and decode the gauge's actual protection config (CUV
    // enable/latch/threshold, COV/OCC) to diagnose the over-discharge behaviour.
    else if (command.startsWith("dfread "))
    {
        // dfread <hexaddr> [len]  - dump gauge data flash bytes (bench diagnostics)
        String args = command.substring(7);
        args.trim();
        int sp = args.indexOf(' ');
        String addrStr = sp > 0 ? args.substring(0, sp) : args;
        int len = sp > 0 ? args.substring(sp + 1).toInt() : 1;
        if (len < 1) len = 1;
        if (len > 32) len = 32;
        uint16_t addr = static_cast<uint16_t>(strtol(addrStr.c_str(), nullptr, 16));
        uint8_t buf[32] = {0};
        if (battery.readGaugeDataFlash(addr, buf, len))
        {
            String line = "DF 0x" + String(addr, HEX) + ":";
            for (int i = 0; i < len; i++)
            {
                char b[6];
                snprintf(b, sizeof(b), " %02x", buf[i]);
                line += b;
            }
            if (len == 2)
            {
                line += "  (u16 LE = " + String(static_cast<uint16_t>(buf[0] | (buf[1] << 8))) + ")";
            }
            ESP_LOGI(TAG, "%s", line.c_str());
        }
        else
        {
            ESP_LOGE(TAG, "dfread failed");
        }
    }
    else if (command.startsWith("dfwrite8 "))
    {
        // dfwrite8 <hexaddr> <hexval> - write one gauge data flash byte (bench diagnostics)
        String args = command.substring(9);
        args.trim();
        int sp = args.indexOf(' ');
        if (sp <= 0)
        {
            ESP_LOGI(TAG, "Usage: dfwrite8 <hexaddr> <hexval>");
        }
        else
        {
            uint16_t addr = static_cast<uint16_t>(strtol(args.substring(0, sp).c_str(), nullptr, 16));
            uint8_t val = static_cast<uint8_t>(strtol(args.substring(sp + 1).c_str(), nullptr, 16));
            bool ok = battery.writeGaugeDataFlash8(addr, val);
            ESP_LOGI(TAG, "dfwrite8 0x%04x <- 0x%02x : %s", addr, val, ok ? "OK (verified)" : "FAILED");
        }
    }
    // gaugedump [hexstart hexend] - hex dump of the gauge data flash, 32 bytes per line, printed
    // as bare "DF 4100: 55 55 ..." lines (no log prefix) so a captured log is a clean image.
    // Default 0x4000-0x4800: calibration, chemistry tables (OCV 0x4480, Ra 0x4100), learned
    // state (Qmax/Update Status 0x4206), IT configuration (0x4580) and settings (0x4600+).
    else if (command == "gaugedump" || command.startsWith("gaugedump "))
    {
        String args = command.length() > 9 ? command.substring(9) : String();
        args.trim();
        uint32_t start = 0x4000, end = 0x4800;
        if (args.length() > 0)
        {
            int sp = args.indexOf(' ');
            start = static_cast<uint32_t>(strtol((sp > 0 ? args.substring(0, sp) : args).c_str(), nullptr, 16));
            end = sp > 0 ? static_cast<uint32_t>(strtol(args.substring(sp + 1).c_str(), nullptr, 16)) : start + 32;
        }
        if (end <= start || end > 0x10000) end = start + 32;
        int errors = 0;
        const uint32_t t0 = millis();
        battery.lockBus(); // the polling task's own MAC reads otherwise land between blocks (24 of 64 failed)
        for (uint32_t addr = start; addr < end; addr += 32)
        {
            uint8_t buf[32] = {0};
            const size_t n = (end - addr) < 32 ? static_cast<size_t>(end - addr) : 32;
            char line[32 * 3 + 24];
            int pos = snprintf(line, sizeof(line), "DF %04x:", static_cast<unsigned>(addr));
            bool ok = false;
            for (int attempt = 0; attempt < 3 && !ok; ++attempt)
            {
                ok = battery.readGaugeDataFlash(static_cast<uint16_t>(addr), buf, n);
                if (!ok) vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (ok)
            {
                for (size_t i = 0; i < n; i++)
                {
                    pos += snprintf(line + pos, sizeof(line) - pos, " %02x", buf[i]);
                }
            }
            else
            {
                ++errors;
                snprintf(line + pos, sizeof(line) - pos, " READ FAILED");
            }
            Serial.println(line);
            vTaskDelay(1);
        }
        battery.unlockBus();
        ESP_LOGI(TAG, "DF END %04x-%04x, %d read errors, %lu ms", static_cast<unsigned>(start),
                 static_cast<unsigned>(end), errors, static_cast<unsigned long>(millis() - t0));
    }
    // gaugemac <hexcmd> [len] - one raw AltManufacturerAccess block as the gauge returns it
    // (little-endian). IT Status 1/2/3 = 73/74/75, DA Status 1 = 71, Gauging Status = 56.
    else if (command.startsWith("gaugemac "))
    {
        String args = command.substring(9);
        args.trim();
        int sp = args.indexOf(' ');
        uint16_t cmd = static_cast<uint16_t>(strtol((sp > 0 ? args.substring(0, sp) : args).c_str(), nullptr, 16));
        int len = sp > 0 ? args.substring(sp + 1).toInt() : 32;
        if (len < 1) len = 1;
        if (len > 32) len = 32;
        uint8_t buf[32] = {0};
        if (battery.readGaugeMacBlock(cmd, buf, len))
        {
            String line = "MAC " + String(cmd, HEX) + ":";
            for (int i = 0; i < len; i++)
            {
                char b[6];
                snprintf(b, sizeof(b), " %02x", buf[i]);
                line += b;
            }
            ESP_LOGI(TAG, "%s", line.c_str());
        }
        else
        {
            ESP_LOGE(TAG, "gaugemac 0x%04x failed", cmd);
        }
    }
    // gaugeit - Impedance Track internals from IT Status 1/2/3 (bq28z610 TRM 0x0073-0x0075):
    // the true remaining/full capacity the simulation produced, the Ra scale in use, the depth
    // of discharge reference points and the Qmax update inputs. Raw bytes are printed too.
    else if (command == "gaugeit")
    {
        uint8_t s1[24] = {0}, s2[24] = {0}, s3[20] = {0};
        const bool ok1 = battery.readGaugeMacBlock(0x0073, s1, sizeof(s1));
        const bool ok2 = battery.readGaugeMacBlock(0x0074, s2, sizeof(s2));
        const bool ok3 = battery.readGaugeMacBlock(0x0075, s3, sizeof(s3));
        auto u16 = [](const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); };
        auto i16 = [](const uint8_t *p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); };
        auto hex = [](const uint8_t *p, size_t n) {
            String o;
            for (size_t i = 0; i < n; i++) { char b[6]; snprintf(b, sizeof(b), " %02x", p[i]); o += b; }
            return o;
        };
        if (ok1)
        {
            ESP_LOGI(TAG, "IT1 TrueRemQ %d mAh  InitialQ %d mAh  TrueFCCQ %d mAh  T_sim %.1f C  T_amb %.1f C  RaScale0 %u (1000 = x1.0)  CompRes1 %u",
                     i16(s1), i16(s1 + 4), i16(s1 + 8), u16(s1 + 12) / 10.0 - 273.15, u16(s1 + 14) / 10.0 - 273.15,
                     u16(s1 + 16), u16(s1 + 20));
            ESP_LOGI(TAG, "IT1 raw:%s", hex(s1, sizeof(s1)).c_str());
        }
        else ESP_LOGE(TAG, "IT Status 1 read failed");
        if (ok2)
        {
            // layout as the bq28z610 (fw 0.17) returns it: PackGrid U1, LStatus U1, CellGrid0 U1,
            // CellGrid1 U1, 2 reserved, StateTime U4 (s), DOD0_0 U2, DOD0_1 U2, DOD0PassedQ I2 (mAh),
            // DOD0PassedE I2 (cWh), DOD0Time U2, DODEOC_0 U2, DODEOC_1 U2. DOD in 1/16384.
            ESP_LOGI(TAG, "IT2 PackGrid %u  LStatus 0x%02x  CellGrid0 %u  StateTime %lu s  DOD0_0 %u (%.1f %%)  DOD0PassedQ %d mAh  DOD0PassedE %d cWh  DOD0Time %u  DODEOC_0 %u (%.1f %%)",
                     s2[0], s2[1], s2[2], static_cast<unsigned long>(s2[6] | (s2[7] << 8) | (s2[8] << 16) | (static_cast<uint32_t>(s2[9]) << 24)),
                     u16(s2 + 10), u16(s2 + 10) * 100.0 / 16384.0, i16(s2 + 14), i16(s2 + 16), u16(s2 + 18), u16(s2 + 20), u16(s2 + 20) * 100.0 / 16384.0);
            ESP_LOGI(TAG, "IT2 raw:%s", hex(s2, sizeof(s2)).c_str());
        }
        else ESP_LOGE(TAG, "IT Status 2 read failed");
        if (ok3)
        {
            // layout per TRM: QMax0 U2, QMax1 U2, QMaxDOD0_0 U2, QMaxDOD0_1 U2, QMaxPassedQ I2, QMaxTime U2, Tk U2, Ta U2, RawDOD0_0 U2, RawDOD0_1 U2
            ESP_LOGI(TAG, "IT3 QMax0 %u mAh  QMaxDOD0_0 %u (%.1f %%)  QMaxPassedQ %d mAh  QMaxTime %u  RawDOD0_0 %u (%.1f %%)",
                     u16(s3), u16(s3 + 4), u16(s3 + 4) * 100.0 / 16384.0, i16(s3 + 8), u16(s3 + 10), u16(s3 + 16), u16(s3 + 16) * 100.0 / 16384.0);
            ESP_LOGI(TAG, "IT3 raw:%s", hex(s3, sizeof(s3)).c_str());
        }
        else ESP_LOGE(TAG, "IT Status 3 read failed");
    }
#endif // TT_BENCH
    else if (command == "gaugeprot")
    {
        battery.printProtectionConfig();
    }
    // gaugecuvfix: enable the CUV latch (Protection Config bit1 CUV_RECOV_CHG) so the
    // cell can't over-discharge by CUV auto-recovery. Reports if the write sticks.
    else if (command == "gaugecuvfix")
    {
        battery.ensureGaugeCuvLatch();
        battery.printProtectionConfig();
    }
#ifdef TT_BENCH
    // startsystem: force the full system to start even if the battery-ready gate
    // hasn't passed (bench/debug, e.g. running on USB without a healthy cell).
    else if (command == "startsystem")
    {
        ESP_LOGW(TAG, "Forcing full system start (bypassing battery-ready gate)...");
        startFullSystem();
    }
    // bringupreset: clear the first-boot bring-up flag and restart the auto
    // provision+calibration sequence (for re-testing or refurbishing a board).
    else if (command == "bringupreset")
    {
        bringupRestart();
        ESP_LOGW(TAG, "BRINGUP flag cleared — re-running auto bring-up. Watch the LED.");
    }
    // gaugelearned: Path B - stabilize the rail (HIZ stops the charge oscillation so
    // SYS doesn't collapse and glitch I2C writes), then mark the gauge "learned"
    // (Update Status 0x06 + reseed QMax) so it computes FCC from QMax + chem Ra.
    // Pre-req: cell already charged to ~3.3V so the battery holds SYS up in HIZ.
    else if (command == "gaugelearned")
    {
        ESP_LOGI(TAG, "LEARN: stabilizing rail (HIZ) then marking gauge learned...");
        battery.setHizMode(true);
        delay(400); // let SYS settle off the battery
        bool success = battery.resetGaugeLearningState(); // writes Update Status 0x06 + QMax
        delay(100);
        battery.setHizMode(false); // restore charger input
        ESP_LOGI(TAG, "Gauge mark-learned: %s", success ? "OK" : "FAILED");
        battery.printBatteryInfo();
    }
    // --- Gauge learning-cycle helpers ---
    // learndis: DISCHARGE phase. Charger -> HIZ (ignore USB so the system runs
    // off the battery and it drains) and disable inactivity sleep so it stays
    // awake/loaded. USB stays connected for monitoring. Run until cell ~2.8-3.0V
    // and GaugingStatus shows FD, then 'learnrest'.
    else if (command == "learndis")
    {
        // With the battery bus handed to an EV2400 (gaugebus release) the firmware cannot
        // touch the charger or read the cell: no HIZ and no voltage floor. The gauge's own
        // CUV protection (2.5 V) ends the discharge by opening the FETs, which is exactly the
        // rest the learning cycle wants; USB must be unplugged so nothing charges meanwhile.
        bool hiz = false;
        if (battery.isBusReleased())
        {
            ESP_LOGW(TAG, "LEARN: battery bus is released to the EV2400 - no HIZ, no floor. Unplug USB now; the gauge CUV at 2.5 V ends the drain.");
        }
        else
        {
            hiz = battery.setHizMode(true);
        }
        sleepController.setInactivityTimeout(Policy::INACTIVITY_TIMEOUT_NEVER); // effectively disable auto-sleep
        sleepController.resetActivity();
        // Max out the LEDs (full-white, max brightness) to add ~300mA load and
        // speed up the discharge. Turned off again in 'learnchg'/sleep.
        ledController.setMaxBrightness(255);
        ledController.simpleLed(0xFFFFFF, 255);
        gLearnDischarge = true; // loop() enforces the LEARN_DISCHARGE_FLOOR_V hard floor
        ESP_LOGI(TAG, "LEARN: discharge mode ON (charger HIZ=%s, auto-sleep disabled, LEDs full)",
                 hiz ? "on" : (battery.isBusReleased() ? "n/a, bus released" : "FAILED"));
        ESP_LOGW(TAG, "Draining cell; auto-stops at %.2fV to stay clear of CUV. Then 'learnrest'.",
                 LEARN_DISCHARGE_FLOOR_V);
    }
    // learnrest: REST phase. Keep charger in HIZ (no charge current to corrupt the
    // OCV), let the device deep-sleep so cell current drops to ~uA. The gauge
    // takes its OCV reading during the rest. Wake (button/pogo) after ~15-30 min,
    // then check 'battery' for VOK set / RDIS cleared / FCC nonzero.
    else if (command == "learnrest")
    {
        gLearnDischarge = false;
        battery.setHizMode(true); // ensure no charging during the rest/OCV
        ESP_LOGI(TAG, "LEARN: entering rest. Device will deep-sleep for OCV.");
        ESP_LOGW(TAG, "Leave it ~15-30 min, then wake (button/pogo) and run 'battery'.");
        ESP_LOGW(TAG, "Look for GaugingStatus VOK set, RDIS gone, FCC nonzero.");
        delay(500);
        sleepController.checkAndSleep(); // enter deep sleep now (OCV taken while resting)
    }
    // learnchg: CHARGE phase. Exit HIZ so the BQ25792 fast-charges to 3.6V, restore
    // normal auto-sleep. Run until ChargingStatus VCT + BatteryStatus FC (full).
    else if (command == "learnchg")
    {
        gLearnDischarge = false;
        bool hiz = battery.setHizMode(false);
        sleepController.setInactivityTimeout(Policy::INACTIVITY_TIMEOUT_SHORT_MS); // see Policy.h
        sleepController.resetActivity();
        ledController.turnOff(); // drop the discharge load
        ESP_LOGI(TAG, "LEARN: charge mode (charger HIZ=%s, auto-sleep restored, LEDs off)",
                 hiz ? "off" : "FAILED-to-clear");
        ESP_LOGW(TAG, "Watch 'battery' until Charge State stays Fast charge then taper to");
        ESP_LOGW(TAG, "VCT/FC, cell ~3.6V. Then 'learnrest' again to anchor the full OCV.");
        battery.printBatteryInfo();
    }
#endif // TT_BENCH
    // System commands
    else if (command.startsWith("endpoint"))
    {
        // endpoint | endpoint clear | endpoint set <api_base> <reverb_host> <reverb_port> <tls 0|1> [app_key]
        // `set` (persisted in NVS, points the unit at another server) is bench-only; show and
        // clear stay in production so a stray override can be seen and removed.
        String args = command.substring(8);
        args.trim();
        if (args.isEmpty())
        {
            printEndpoints();
        }
        else if (args == "clear")
        {
            config.deleteSetting("ep_api");
            config.deleteSetting("ep_rvhost");
            config.deleteSetting("ep_rvport");
            config.deleteSetting("ep_rvtls");
            config.deleteSetting("ep_rvkey");
            ESP_LOGI(TAG, "Endpoints reset to production defaults - 'restart' to apply");
        }
#ifdef TT_BENCH
        else if (args.startsWith("set "))
        {
            String parts[5];
            const int n = splitArgs(args.substring(4), parts, 5);
            if (n < 4)
            {
                ESP_LOGI(TAG, "Usage: endpoint set <api_base> <reverb_host> <reverb_port> <tls 0|1> [app_key]");
                ESP_LOGI(TAG, "e.g.   endpoint set http://192.168.2.150:8000/api 192.168.2.150 8080 0 local-key");
            }
            else
            {
                config.storeString("ep_api", parts[0]);
                config.storeString("ep_rvhost", parts[1]);
                config.storeInt("ep_rvport", parts[2].toInt());
                config.storeInt("ep_rvtls", parts[3].toInt() ? 1 : 0);
                if (n == 5) config.storeString("ep_rvkey", parts[4]); else config.deleteSetting("ep_rvkey");
                printEndpoints();
                ESP_LOGI(TAG, "'restart' to apply");
            }
        }
#else
        else if (args.startsWith("set "))
        {
            ESP_LOGW(TAG, "'endpoint set' is a bench command (build env:bench)");
        }
#endif
    }
    else if (command == "restart")
    {
        ESP_LOGW(TAG, "Restarting device...");
        delay(1000);
        ESP.restart();
    }
    else if (command == "config")
    {
        config.printAllSettings();
    }
    else if (command == "debug")
    {
        ESP_LOGI(TAG, "--- Debug Information ---");
        ESP_LOGI(TAG, "Free internal SRAM: %d bytes", ESP.getFreeHeap());
        ESP_LOGI(TAG, "Free PSRAM: %d bytes", ESP.getFreePsram());
        ESP_LOGI(TAG, "Largest free block: %d bytes", ESP.getMaxAllocHeap());
        ESP_LOGI(TAG, "Minimum free heap: %d bytes", ESP.getMinFreeHeap());
        ESP_LOGI(TAG, "Chip revision: %d", ESP.getChipRevision());
        ESP_LOGI(TAG, "SDK version: %s", ESP.getSdkVersion());
        ESP_LOGI(TAG, "WiFi mode: %d", WiFi.getMode());
        ESP_LOGI(TAG, "WiFi status: %d", WiFi.status());
        ESP_LOGI(TAG, "Battery: %s", battery.getBatteryStatusString().c_str());
        ESP_LOGI(TAG, "WiFi connected: %s", WiFi.isConnected() ? "Yes" : "No");
        ESP_LOGI(TAG, "Has WiFi credentials: %s", config.hasWiFiCredentials() ? "Yes" : "No");
        ESP_LOGI(TAG, "WiFi SSID length: %d", config.getWiFiSSID().length());
        ESP_LOGI(TAG, "WiFi Password length: %d", config.getWiFiPassword().length());

        // Check ESP32 WiFi provisioning library status
        bool provisioned = false;
        esp_err_t ret = wifi_prov_mgr_is_provisioned(&provisioned);
        ESP_LOGI(TAG, "ESP32 WiFi Library Provisioned: %s", provisioned ? "Yes" : "No");
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Provisioning check error: %s", esp_err_to_name(ret));
        }

        // Check actual WiFi config stored by ESP32
        wifi_config_t wifi_cfg;
        ret = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
        if (ret == ESP_OK)
        {
            String storedSSID = String((char *)wifi_cfg.sta.ssid);
            ESP_LOGI(TAG, "ESP32 Stored SSID: %s", storedSSID.length() > 0 ? storedSSID.c_str() : "(none)");
            ESP_LOGI(TAG, "ESP32 Stored Password Length: %d", strlen((char *)wifi_cfg.sta.password));
        }
        else
        {
            ESP_LOGE(TAG, "Failed to get WiFi config: %s", esp_err_to_name(ret));
        }

        ESP_LOGI(TAG, "Note: BLE is automatically managed by ESP32 provisioning library");
        config.printAllSettings();
    }
    // Add new heap command
    else if (command == "heap")
    {
        ESP_LOGI(TAG, "--- Detailed Heap Information ---");

        // Internal SRAM Heap Statistics
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Internal SRAM Heap:");
        ESP_LOGI(TAG, "  Total heap size: %d bytes", ESP.getHeapSize());
        ESP_LOGI(TAG, "  Free heap: %d bytes", ESP.getFreeHeap());
        ESP_LOGI(TAG, "  Largest free block: %d bytes", ESP.getMaxAllocHeap());
        ESP_LOGI(TAG, "  Minimum free heap since boot: %d bytes", ESP.getMinFreeHeap());

        // Calculate internal heap usage
        uint32_t usedHeap = ESP.getHeapSize() - ESP.getFreeHeap();
        float heapUsagePercent = (float)usedHeap / ESP.getHeapSize() * 100;
        ESP_LOGI(TAG, "  Used heap: %d bytes (%.1f%%)", usedHeap, heapUsagePercent);

        // Calculate internal heap fragmentation
        float heapFragmentation = 0;
        if (ESP.getFreeHeap() > 0)
        {
            heapFragmentation = (1.0 - (float)ESP.getMaxAllocHeap() / ESP.getFreeHeap()) * 100;
        }
        ESP_LOGI(TAG, "  Heap fragmentation: %.1f%%", heapFragmentation);

        // External PSRAM Statistics
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "External PSRAM:");
        ESP_LOGI(TAG, "  Total PSRAM size: %d bytes", ESP.getPsramSize());
        ESP_LOGI(TAG, "  Free PSRAM: %d bytes", ESP.getFreePsram());
        ESP_LOGI(TAG, "  Minimum free PSRAM since boot: %d bytes", ESP.getMinFreePsram());

        // Calculate PSRAM usage
        uint32_t usedPsram = ESP.getPsramSize() - ESP.getFreePsram();
        float psramUsagePercent = 0;
        if (ESP.getPsramSize() > 0)
        {
            psramUsagePercent = (float)usedPsram / ESP.getPsramSize() * 100;
        }
        ESP_LOGI(TAG, "  Used PSRAM: %d bytes (%.1f%%)", usedPsram, psramUsagePercent);

        // Overall memory status
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Combined Memory Status:");
        uint32_t totalFree = ESP.getFreeHeap() + ESP.getFreePsram();
        uint32_t totalSize = ESP.getHeapSize() + ESP.getPsramSize();
        ESP_LOGI(TAG, "  Total free memory: %d bytes", totalFree);
        ESP_LOGI(TAG, "  Total memory size: %d bytes", totalSize);

        // Memory health status
        ESP_LOGI(TAG, "");
        if (ESP.getFreeHeap() < CRITICAL_HEAP_THRESHOLD)
        {
            ESP_LOGE(TAG, "Internal Heap Status: CRITICAL - Very low memory");
        }
        else if (ESP.getFreeHeap() < WARNING_HEAP_THRESHOLD)
        {
            ESP_LOGW(TAG, "Internal Heap Status: WARNING - Low memory");
        }
        else
        {
            ESP_LOGI(TAG, "Internal Heap Status: OK - Memory levels normal");
        }

        if (ESP.getPsramSize() > 0)
        {
            if (ESP.getFreePsram() < 100000)
            { // Less than 100KB PSRAM free
                ESP_LOGW(TAG, "PSRAM Status: WARNING - Low PSRAM");
            }
            else
            {
                ESP_LOGI(TAG, "PSRAM Status: OK - PSRAM levels normal");
            }
        }
        else
        {
            ESP_LOGW(TAG, "PSRAM Status: No PSRAM detected or disabled");
        }

        ESP_LOGI(TAG, "----------------------------------");
    }
    else if (command == "timing")
    {
        Diag::printSectionStats(false);
    }
    else if (command == "tasks")
    {
        Diag::printTaskStats();
    }
    else if (command == "coredump")
    {
        Diag::printCoreDumpSummary();
    }
    else if (command == "coredumperase")
    {
        Diag::eraseCoreDump();
    }
    // Stack monitoring command
    else if (command == "stack")
    {
        ESP_LOGI(TAG, "--- Stack Information ---");
        UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
        size_t stackRemaining = stackHighWaterMark * sizeof(StackType_t);
        ESP_LOGI(TAG, "Stack high water mark: %d words (%d bytes)",
                 stackHighWaterMark, stackRemaining);

        // Usage against the configured loop task stack (sdkconfig: 8192 bytes)
        size_t totalStack = CONFIG_ARDUINO_LOOP_STACK_SIZE;
        size_t usedStack = totalStack - stackRemaining;
        float usagePercent = (float)usedStack / totalStack * 100;

        ESP_LOGI(TAG, "Estimated stack usage: %d/%d bytes (%.1f%%)",
                 usedStack, totalStack, usagePercent);

        if (stackRemaining < 1000)
        {
            ESP_LOGE(TAG, "Status: CRITICAL - Very low stack space");
        }
        else if (stackRemaining < 2000)
        {
            ESP_LOGW(TAG, "Status: WARNING - Low stack space");
        }
        else
        {
            ESP_LOGI(TAG, "Status: OK - Stack levels normal");
        }
        ESP_LOGI(TAG, "------------------------");
    }
    // Sleep commands
    else if (command == "sleep")
    {
        ESP_LOGI(TAG, "Entering deep sleep immediately...");
        sleepController.checkAndSleep();
    }
    else if (command.startsWith("sleepafter "))
    {
        String timeStr = command.substring(11);
        unsigned long timeMs = timeStr.toInt();
        if (timeMs > 0)
        {
            sleepController.scheduleSleep(timeMs);
            ESP_LOGI(TAG, "Sleep scheduled after %lu ms", timeMs);
        }
        else
        {
            ESP_LOGI(TAG, "Usage: sleepafter <milliseconds>");
            ESP_LOGI(TAG, "Example: sleepafter 30000 (sleep after 30 seconds)");
        }
    }
    else if (command == "cancelsleep")
    {
        sleepController.cancelSleep();
        ESP_LOGI(TAG, "Scheduled sleep cancelled");
    }
    else if (command == "sleepstatus")
    {
        ESP_LOGI(TAG, "--- Sleep Controller Status ---");
        ESP_LOGI(TAG, "Inactivity timeout: %lu ms", sleepController.getInactivityTimeout());
        ESP_LOGI(TAG, "Was woken from sleep: %s", sleepController.wasWokenFromSleep() ? "Yes" : "No");
        if (sleepController.wasWokenFromSleep())
        {
            sleepController.printWakeupReason();
        }

        // Check sleep conditions
        FileManager &fm = FileManager::getInstance();
        AudioController &ac = AudioController::getInstance();
        ESP_LOGI(TAG, "Pending downloads: %d", fm.getPendingDownloadsCount());
        ESP_LOGI(TAG, "Audio state: %d (0=STOPPED, 1=PLAYING, 2=PAUSED)", ac.getState());
        ESP_LOGI(TAG, "Can enter sleep: %s", (fm.getPendingDownloadsCount() == 0 && ac.getState() != 1) ? "Yes" : "No");
        ESP_LOGI(TAG, "-------------------------------");
    }
    // File Manager commands that were missing
    else if (command == "dlstats")
    {
        ESP_LOGI(TAG, "%s", fileManager.getDownloadStatsString().c_str());
    }
    else if (command == "dlqueue")
    {
        fileManager.printDownloadQueue();
    }
    else if (command == "required")
    {
        fileManager.printRequiredFiles();
    }
    else if (command == "checkfiles")
    {
        ESP_LOGI(TAG, "Checking required files and scheduling missing ones for download...");
        fileManager.checkRequiredFiles();
        ESP_LOGI(TAG, "Check complete. Use 'dlqueue' to see download queue.");
    }
    else if (command == "cleanup")
    {
        ESP_LOGI(TAG, "Cleaning up temporary files...");
        fileManager.cleanupTempFiles();
        ESP_LOGI(TAG, "Cleanup complete.");
    }
    else
    {
        ESP_LOGI(TAG, "Unknown command. Type 'help' for a list of commands.");
    }
}

void Console::service()
{
    if (!Serial.available())
    {
        return;
    }
    String command = Serial.readStringUntil('\n');
    command.trim();
    // Lowercase only the command word: URLs, paths and hex arguments are case sensitive
    // (a lowercased URL used to turn 10MB.zip into 10mb.zip).
    {
        int sp = command.indexOf(' ');
        String word = sp > 0 ? command.substring(0, sp) : command;
        word.toLowerCase();
        command = sp > 0 ? word + command.substring(sp) : word;
    }
    if (command.length() == 0)
    {
        return; // skip empty commands
    }
    dispatch(command);
}

#ifdef TT_BENCH
void Console::serviceLearnFloor()
{
    // Learning-cycle discharge floor: never let the controlled drain push the LFP cell
    // into the CUV protection trip. Stop the discharge at LEARN_DISCHARGE_FLOOR_V.
    if (gLearnDischarge && battery.hasTelemetry() &&
        battery.getBatteryVoltage() <= LEARN_DISCHARGE_FLOOR_V)
    {
        gLearnDischarge = false;
        // The charger stays in HIZ: the empty-point rest that follows must see no charge
        // current (learnchg lifts it). The toy may sleep now; unplug USB so it does.
        ledController.turnOff();                      // drop the discharge load
        sleepController.setInactivityTimeout(Policy::INACTIVITY_TIMEOUT_SHORT_MS); // see Policy.h
        sleepController.resetActivity();
        ESP_LOGW(TAG, "LEARN: discharge floor %.2fV reached at %.3fV - stopped, charger still in HIZ.",
                 LEARN_DISCHARGE_FLOOR_V, battery.getBatteryVoltage());
        ESP_LOGW(TAG, "Unplug USB now (the toy sleeps in 30 s), rest 5 h, then plug USB and run 'learnchg'.");
    }
}
#endif
