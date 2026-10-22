#pragma once

#include <Arduino.h>
#include <functional>

// Fault codes: one catalogue shared by the toy, the portal and support (docs/FAULTS.md).
// Each entry: code, name, severity, fruit the toy says ("" = silent), how it clears,
// description, remedy. The list is also embedded in the image as the "TTFAULTS1:" marker the
// portal reads when a release is uploaded, so the admin panel knows every code of every release.
//
// X(code, NAME, SEVERITY, fruit, CLEAR, description, remedy)
// No field may contain ';' or '|': they separate records and fields of the marker (the portal
// used to cut 22 remedies at their first ';'). Faults.cpp refuses them at compile time and
// tools/faults.py refuses them too. Codes are never renumbered.
#define TT_FAULT_LIST(X)                                                                                              \
    X(101, WIFI_NOT_PROVISIONED, INFO, "", EVENT, "Boot without WiFi credentials", "Pair the toy with the app")       \
    X(102, WIFI_AUTH_FAILED, ERROR, "üzüm", EVENT, "WiFi credentials rejected during pairing", "Check the network name and password in the app") \
    X(103, WIFI_NO_LINK, ERROR, "portakal", ON_NETWORK, "Home network in range but no link", "Restart the router and check the password. The toy retries by itself") \
    X(104, WIFI_OUT_OF_RANGE, INFO, "", ON_NETWORK, "Provisioned network not in range", "Normal away from home, the toy plays from the card") \
    X(105, WIFI_RADIO_RESET, WARNING, "", EVENT, "Reconnects kept failing, radio was reset", "If it repeats at home, check the router (band steering, DHCP)") \
    X(106, PORTAL_UNREACHABLE, WARNING, "", ON_NETWORK, "API request failed at transport level", "Check portal availability and the toy's internet access") \
    X(107, TOKEN_REJECTED, ERROR, "", ON_NETWORK, "Portal rejected the hub token after a refresh", "Check the hub's owner binding, re-bind the hub in the app") \
    X(108, WS_AUTH_FAILED, ERROR, "", ON_NETWORK, "WebSocket channel authorisation refused", "Check Reverb and the hub's token") \
    X(109, WS_DISCONNECTED, INFO, "", EVENT, "WebSocket dropped", "Normal on flaky links, watch the count") \
    X(201, FIGURE_UNKNOWN, ERROR, "elma", ON_PLAY, "Portal has no unit for this card", "Register the card (Catalog > Cards > Import)") \
    X(202, FIGURE_NO_CONTENT, ERROR, "elma", ON_PLAY, "Figure exists but has nothing to play", "Add content to the figure, or bind content to the wildcard card") \
    X(203, FIGURE_OFFLINE, ERROR, "armut", ON_PLAY, "Portal unreachable and nothing cached for the card", "Get the toy online once, the stories are then cached") \
    X(204, DOWNLOAD_FAILED, ERROR, "kiraz", ON_PLAY, "A track could not be downloaded", "Check the file on the portal and the storage URL over port 80") \
    X(205, TRACK_MISSING, ERROR, "", ON_PLAY, "A cached track vanished before playback", "Re-sync the figure from the hub page") \
    X(206, MANIFEST_INVALID, ERROR, "", ON_PLAY, "Manifest without the fields the toy needs", "Check the unit manifest endpoint") \
    X(207, DOWNLOAD_STALLED, WARNING, "", EVENT, "A transfer stalled and was retried", "Slow or flaky link, watch the count") \
    X(208, CONTENT_SYNC_FAILED, WARNING, "", EVENT, "Background content re-check failed", "Transient unless it repeats") \
    X(301, SD_MOUNT_FAILED, CRITICAL, "muz", ON_BOOT, "SD card missing or unreadable at boot", "Reseat or replace the card (tools/sdcard-prepare.sh)") \
    X(302, SD_WRITE_FAILED, ERROR, "muz", EVENT, "A write or rename on the card failed", "Card wearing out or corrupt: replace it") \
    X(303, SD_FULL, ERROR, "muz", EVENT, "No space left for a download", "Delete stories from the hub page or use a bigger card") \
    X(304, SD_FILE_CORRUPT, WARNING, "", EVENT, "A json file on the card was unreadable and recreated", "Watch the count. Replace a card that keeps corrupting files") \
    X(305, SD_SLOW, INFO, "", EVENT, "Card write stall above 3 s", "Slow card, affects download speed only") \
    X(401, AUDIO_INIT_FAILED, CRITICAL, "", ON_BOOT, "Codec or I2S initialisation failed", "Hardware fault: return the unit") \
    X(402, DECODER_FAILED, ERROR, "", ON_PLAY, "A file could not start decoding", "Check the file (MP3) on the portal") \
    X(403, AUDIO_UNDERRUN, INFO, "", EVENT, "Read-ahead ran dry during playback", "Slow card, watch the count") \
    X(404, PROMPT_FILE_MISSING, WARNING, "", ON_BOOT, "A prompt file is missing on the card", "Assign a complete prompt pack to the release") \
    X(405, PLAYLIST_STUCK, WARNING, "", ON_PLAY, "Waited for a download for too long", "Check the link and the storage URL") \
    X(501, BATTERY_LOW, INFO, "", ON_BOOT, "Battery at 20 % or below", "Charge the toy") \
    X(502, BATTERY_CRITICAL, WARNING, "", ON_BOOT, "Cutoff reached, toy went to sleep", "Charge the toy") \
    X(503, GAUGE_NO_TELEMETRY, ERROR, "", ON_BOOT, "Battery gauge not answering", "Hardware check: gauge bus") \
    X(504, CHARGER_FAULT, ERROR, "", EVENT, "Charger reported a fault", "Check the charger/adapter, hardware check if it repeats") \
    X(505, BROWNOUT_RESET, ERROR, "", EVENT, "Reset by brownout", "Weak cell or adapter, check the battery") \
    X(506, DEAD_CELL_RECOVERY, WARNING, "", EVENT, "Dead-cell recovery hold engaged", "Leave the toy on the charger") \
    X(507, POWER_BUS_ERROR, WARNING, "", EVENT, "I2C error on the power bus", "Hardware check if it repeats") \
    X(601, OTA_CHECK_FAILED, WARNING, "", ON_NETWORK, "Firmware check failed", "Transient unless it repeats, then check the portal") \
    X(602, OTA_DOWNLOAD_FAILED, ERROR, "karpuz", EVENT, "Firmware image download failed", "Check the release file and the link. The toy retries") \
    X(603, OTA_FLASH_FAILED, ERROR, "karpuz", EVENT, "Flash write or activation failed", "Hardware check if it repeats") \
    X(604, OTA_SHA_MISMATCH, ERROR, "karpuz", EVENT, "Downloaded image hash did not match", "Re-upload the release") \
    X(605, OTA_ROLLED_BACK, ERROR, "", EVENT, "Bootloader returned to the previous image", "The new firmware did not come up, check the release") \
    X(606, OTA_CRASH_LOOP_REVERT, CRITICAL, "", EVENT, "Crash-loop guard switched partitions", "The firmware crashes at boot, revoke the release") \
    X(607, PROMPT_PACK_FAILED, WARNING, "", EVENT, "Prompt pack could not be downloaded", "Check the pack files over port 80") \
    X(608, PROMPT_PACK_ACTIVATE_FAILED, ERROR, "", EVENT, "Staged prompt pack could not be swapped in", "Check the card. The toy retries at the next check") \
    X(701, NFC_INIT_FAILED, ERROR, "", ON_BOOT, "NFC reader not found at boot", "Hardware fault: return the unit") \
    X(702, NFC_READ_TROUBLE, WARNING, "", ON_PLAY, "Figure on the pins but no card read", "Reseat the figure, replace the card if it repeats") \
    X(703, NFC_RESET, INFO, "", EVENT, "NFC reader watchdog reset", "Watch the count") \
    X(801, WATCHDOG_RESET, ERROR, "", EVENT, "Task watchdog reset", "Firmware hang, report with the firmware version") \
    X(802, PANIC_RESET, ERROR, "", EVENT, "Panic or exception reset", "Firmware crash, report with the firmware version") \
    X(803, HEAP_LOW, WARNING, "", EVENT, "Internal heap below threshold", "Firmware memory pressure, report it") \
    X(804, LOOP_STALL, WARNING, "", EVENT, "A loop section took longer than 2 s", "Firmware stall, report the section") \
    X(805, STACK_LOW, WARNING, "", EVENT, "Loop task stack under 1 KB", "Firmware issue, report it") \
    X(806, CONFIG_CORRUPT, ERROR, "", EVENT, "An NVS record was unreadable, defaults restored", "Re-pair if WiFi was lost")

enum class Severity : uint8_t { INFO = 0, WARNING = 1, ERROR = 2, CRITICAL = 3 };
// How an active fault ends. EVENT: never active (a point event); ON_NETWORK: cleared when
// the network is back; ON_PLAY: cleared when a figure plays; ON_BOOT: cleared by a reboot only.
enum class FaultClear : uint8_t { EVENT, ON_NETWORK, ON_PLAY, ON_BOOT };

enum class Fault : uint16_t {
#define TT_FAULT_ENUM(code, name, ...) name = code,
    TT_FAULT_LIST(TT_FAULT_ENUM)
#undef TT_FAULT_ENUM
};

struct FaultInfo {
    uint16_t code;
    const char *slug;
    Severity severity;
    const char *fruit;       // "" when the toy stays silent
    FaultClear clears;
    const char *description;
    const char *remedy;
};

namespace Faults
{
    // After NVS is up (before the network): loads the ring, classifies the reset reason,
    // bumps the boot sequence.
    void begin();
    // Record a fault. speak=false for low-level repeats whose user-facing effect is raised
    // elsewhere (a track failing while the figure-level DOWNLOAD_FAILED is what the child hears).
    void raise(Fault f, const char *detail = nullptr, bool speak = true);
    void clear(Fault f);
    void clearOn(FaultClear trigger);            // ON_NETWORK / ON_PLAY: the owner of the condition calls this
    void update();                               // loop task: debounced upload of unsent events
    const FaultInfo *info(Fault f);
    const FaultInfo *info(uint16_t code);
    const char *fruit();                         // child word of the most severe active fault, "" if none
    bool hasActive(Severity atLeast);
    String reportJson();                         // "active":[{"c":103,"since_s":12}],"last":{"c":204,"age_s":30,"d":"..."}
    // The loop wires the prompt side (raise -> prompt) so Faults stays free of VoicePrompts.
    void setSpeaker(std::function<void(Fault)> fn);
    const char *schema();                        // the embedded catalogue (after the marker)
    void printStatus();                          // console
}
