#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include "PromptPackManager.h"

// Over-the-air updates, dock only (docs/PROTOCOL-V2.md §5; every state x event is in
// docs/OTA-STATE-MACHINE.md, which this code follows).
//  - The backend decides what a device should run: GET /hubs/{uuid}/firmware/check answers
//    {release_id: null} (nothing) or a release {release_id, version, version_code, file_size,
//    sha256, url, mandatory, explicit?, prompts}. The offer is kept in RAM with its time.
//  - The toy waits (WAITING) until it is docked, silent, without a figure and without a story
//    download; a stale offer (> 10 min) is re-checked first (signed URL, revoked release).
//  - The paired voice prompt pack is staged (PROMPTS), then the image is streamed by a task
//    into the inactive partition (DOWNLOADING), hashed on the fly and activated only when the
//    sha256 matches (APPLYING), then the toy restarts (REBOOTING).
//  - Progress/result are posted to POST /hubs/{uuid}/firmware/status from the loop task.
//  - With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new image boots "pending verify"; it is
//    confirmed once the portal is reached (Reverb or a 2xx from the API) or after 10 min of
//    uptime. A reset before that returns to the previous image, which reports rolled_back and
//    remembers the release (the last 4) so it is declined unless offered explicitly.
class OtaManager
{
public:
    enum State : uint8_t
    {
        IDLE,
        CHECKING,
        WAITING,      // release offered, waiting for the dock and an idle toy
        PROMPTS,      // the release's voice prompt pack is being staged (before the image)
        DOWNLOADING,
        APPLYING,
        REBOOTING,
        FAILED
    };

    // Why the last attempt failed, classified where it happened, so the loop does not have to
    // match error texts. Posted as "failure" next to the "error" text.
    enum class Failure : uint8_t
    {
        NONE,
        DOWNLOAD,   // transport, HTTP, size, stall, release withdrawn
        FLASH,      // Update.begin / write / end (activate)
        SHA,        // image hash mismatch
        PROMPTS,    // the release's prompt pack could not be staged
        BATTERY,    // battery under 15 % on battery during PROMPTS / DOWNLOADING
        EXPIRED     // internal: 403/410 on the download (re-checked once, then DOWNLOAD)
    };

    struct Job
    {
        uint32_t releaseId = 0;
        String version;
        uint32_t versionCode = 0;
        String url;
        String sha256;
        size_t size = 0;
        bool mandatory = false;
        bool explicitOffer = false;       // portal: the release comes from a pin or an ota-update command
        bool hasPrompts = false;          // the portal pairs a prompt pack with this release
        PromptPackManager::Pack prompts;  // staged into /sounds.staging before the image downloads
    };

    // What the toy is doing, sampled by update() every loop pass (main.cpp provides it).
    struct Conditions
    {
        bool docked = false;         // VBUS present
        bool playing = false;        // a story or a prompt is playing
        bool paused = false;
        bool figure = false;         // a figure is on the pins (pogo or card)
        bool storyDownload = false;  // a FileManager transfer is in flight
        bool batteryKnown = false;
        float batteryPercent = 100.0f;
    };

    static OtaManager &getInstance();

    void begin();            // boot-time bookkeeping (pending image / previous attempt)
    void update();           // loop task: the state machine
    // The portal's nudge (ota-update command) or the console: ask the backend now. A release
    // id makes the request explicit. Queued while a check is running; ignored during an update.
    bool checkNow(uint32_t releaseId = 0, const char *reason = "manual");
    bool startJob(const Job &job);   // console (bench): downloads now, bypassing the waiting rules
    void setConditions(std::function<Conditions()> fn) { _conditionsFn = fn; }
    void onNetworkServicesStarted() { _networkStarted = true; } // API base and token are set up
    void notePortalReached(const char *how);                    // e.g. Reverb subscribed

    State state() const { return _state; }
    const char *stateName() const;   // "idle", "waiting", "downloading", ... for reports
    int progress() const { return _progress; }
    bool inProgress() const { return _state == PROMPTS || _state == DOWNLOADING || _state == APPLYING || _state == REBOOTING; }
    // Deep sleep is refused while an update is written and while a new image is not confirmed
    // yet (the bootloader would roll back a pending image on the wake).
    bool blocksSleep() const { return inProgress() || (_pendingVerify && !_bootHealthyMarked); }
    const Job &job() const { return _job; }
    String lastError() const;                    // copy of the last error text (any task)
    Failure lastFailure() const { return _failure.load(); }
    static const char *failureName(Failure f);   // "download", "flash", "sha", "prompts", "battery", "none"
    void printStatus();
    bool bootedFromUpdate() const { return _bootedFromOta; } // this boot runs a freshly installed image
    bool consumeJustFailed();      // true once after an update attempt failed
    bool consumeAnnounceRequest(); // true once when "updating myself" should be spoken now

private:
    enum class CheckPurpose : uint8_t
    {
        PERIODIC,     // boot, hourly, dock
        COMMAND,      // ota-update / console
        REOFFER,      // WAITING is about to end with an offer older than 10 min
        URL_EXPIRED   // the download answered 403/410
    };

    OtaManager() { _textLock = xSemaphoreCreateMutex(); }
    static void taskEntry(void *arg);
    void runDownload();
    void setState(State s, int progress = -1, const char *error = nullptr, Failure failure = Failure::NONE);
    void postStatus(const char *status, int progress, const String &error, const char *failure, uint32_t versionCode,
                    uint32_t releaseId);
    bool startCheck(CheckPurpose purpose, uint32_t releaseId, const char *reason);
    void onCheckResult(JsonDocument &doc, int code); // loop task, answer to startCheck()
    void checkFailed(CheckPurpose purpose, const char *detail);
    void nothingOffered(CheckPurpose purpose, JsonDocument &doc);
    void acceptOffer(const Job &job);
    void decline(const Job &job, const char *why);
    void serviceWaiting(unsigned long now, const Conditions &c);
    void serviceConfirmation(unsigned long now);
    void serviceBootReport(unsigned long now);
    void beginJob();                                 // WAITING -> PROMPTS (pack staging) or DOWNLOADING
    bool launchDownload();                           // -> DOWNLOADING with _job (task start)
    void onPromptsStaged(bool ok);                   // loop task, from PromptPackManager
    void failNow(Failure f, const char *error);      // loop task: FAILED with status post
    // Rolled-back releases (NVS "ota_rb", the last 4): declined unless offered explicitly.
    bool isBlacklisted(uint32_t releaseId);
    void blacklistAdd(uint32_t releaseId);
    void blacklistRemove(uint32_t releaseId);
    String deviceId() const;
    void confirmImage(const char *why);
    void clearError();
    void copyText(char *dst, const char *src);   // under _textLock

    volatile State _state = IDLE;
    volatile int _progress = 0;
    // Written by the download task, read by the loop: fixed buffers under a small mutex.
    static const size_t ERROR_LEN = 96;
    mutable SemaphoreHandle_t _textLock = nullptr;
    char _error[ERROR_LEN] = {};
    std::atomic<Failure> _failure{Failure::NONE};
    Job _job;
    bool _hasOffer = false;                // _job holds a release offer (WAITING and later)
    TaskHandle_t _task = nullptr;
    std::function<Conditions()> _conditionsFn;
    std::atomic<bool> _abortRequested{false};   // battery: the download task stops
    std::atomic<bool> _expiredPending{false};   // the download task ended on 403/410
    bool _expiredRecheckUsed = false;           // one re-check per attempt

    // status posting: set by either task (setState), posted by the loop
    std::atomic<bool> _statusDirty{false};
    char _pendingStatus[16] = {};
    int _pendingProgress = 0;
    char _pendingError[ERROR_LEN] = {};
    Failure _pendingFailure = Failure::NONE;

    // scheduling
    unsigned long _bootAt = 0;
    unsigned long _nextCheckAt = 0;
    unsigned long _lastCheckAt = 0;        // last answer (any), 0 = none this boot
    unsigned long _checkStartedAt = 0;     // CHECKING gives up after CHECK_TIMEOUT_MS
    CheckPurpose _checkPurpose = CheckPurpose::PERIODIC;
    uint32_t _checkReleaseId = 0;          // release id sent with the running check
    uint8_t _check5xx = 0;                 // consecutive 5xx answers to checks (backoff)
    bool _commandPending = false;          // an ota-update arrived while a check was running
    uint32_t _commandReleaseId = 0;
    uint32_t _offerCommandReleaseId = 0;   // the offer came from a command with this release id
    unsigned long _offerAt = 0;            // when the offer was received / refreshed
    unsigned long _waitingSince = 0;       // first offer of this release (mandatory: 1 h)
    unsigned long _reofferRetryAt = 0;     // a failed re-check is not repeated before this
    bool _announced = false;               // "updating myself" requested for this offer
    unsigned long _announcedAt = 0;
    std::atomic<bool> _announceRequest{false};
    bool _wasDocked = false;
    unsigned long _failedAt = 0;
    int _promptSyncFailures = 0;

    bool _networkStarted = false;
    bool _portalReached = false;
    bool _bootHealthyMarked = false;
    bool _pendingVerify = false;
    bool _bootReported = false;
    bool _bootedFromOta = false;
    std::atomic<bool> _justFailed{false};
};
