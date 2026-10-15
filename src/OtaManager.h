#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include "PromptPackManager.h"

// Over-the-air updates.
//  - The backend decides what a device should run: GET /hubs/{uuid}/firmware/check returns
//    204 (nothing) or a release {release_id, version, version_code, file_size, sha256, url}.
//  - The image is streamed by a dedicated task into the inactive OTA partition (Arduino
//    Update), hashed on the fly and only activated when the sha256 matches.
//  - Progress/result are posted to POST /hubs/{uuid}/firmware/status from the loop task.
//  - With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new image boots as "pending verify";
//    markBootHealthy() confirms it (called once the system is up), otherwise the next reset
//    returns to the previous image and that is reported as rolled_back.
class OtaManager
{
public:
    enum State : uint8_t
    {
        IDLE,
        CHECKING,
        WAITING,      // update known, waiting for an idle moment / enough battery
        PROMPTS,      // the release's voice prompt pack is being staged (before the image)
        DOWNLOADING,
        APPLYING,
        REBOOTING,
        FAILED
    };

    // Why the last attempt failed, classified where it happened (the download task), so the
    // loop does not have to match error texts.
    enum class Failure : uint8_t
    {
        NONE,
        DOWNLOAD,   // transport, HTTP, size, stall
        FLASH,      // Update.begin / write / end (activate)
        SHA,        // image hash mismatch
        PROMPTS     // the release's prompt pack could not be staged
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
        bool hasPrompts = false;          // the portal pairs a prompt pack with this release
        PromptPackManager::Pack prompts;  // staged into /sounds.staging before the image downloads
    };

    static OtaManager &getInstance();

    void begin();            // boot-time bookkeeping (pending image / previous attempt)
    void update();           // loop task: validation, periodic check, deferred start, status posts
    bool checkNow(uint32_t releaseId = 0, const char *reason = "manual"); // ask the backend
    bool startJob(const Job &job);                                     // explicit (bench / command)
    void markBootHealthy();  // network is up (the image itself is confirmed after 60 s of uptime)

    State state() const { return _state; }
    const char *stateName() const;   // "idle", "waiting", "downloading", ... for reports
    int progress() const { return _progress; }
    bool inProgress() const { return _state == PROMPTS || _state == DOWNLOADING || _state == APPLYING || _state == REBOOTING; }
    const Job &job() const { return _job; }
    String lastError() const;                    // copy of the last error text (any task)
    Failure lastFailure() const { return _failure.load(); }
    static const char *failureName(Failure f);   // "download", "flash", "sha", "prompts", "none"
    void printStatus();
    bool consumeJustUpdated();  // true once, on the boot that confirmed a new image
    bool bootedFromUpdate() const { return _bootedFromOta; } // this boot runs a freshly installed image
    bool consumeJustFailed();   // true once after an update attempt failed

    // Gate for starting a download: true when the toy is idle enough for an update.
    void setIdleCheck(std::function<bool()> fn) { _idleCheck = fn; }

private:
    OtaManager() { _textLock = xSemaphoreCreateMutex(); }
    static void taskEntry(void *arg);
    void runDownload();
    void setState(State s, int progress = -1, const char *error = nullptr, Failure failure = Failure::NONE);
    void postStatus(const char *status, int progress, const String &error, uint32_t versionCode = 0);
    void onCheckResult(JsonDocument &doc, int code); // loop task, answer to checkNow()
    void beginJob();                                 // WAITING -> PROMPTS (pack staging) or DOWNLOADING
    void onPromptsStaged(bool ok);                   // loop task, from PromptPackManager
    int _promptSyncFailures = 0;
    unsigned long _failedAt = 0;
    String deviceId() const;

    volatile State _state = IDLE;
    volatile int _progress = 0;
    // Written by the download task, read by the loop: fixed buffers under a small mutex.
    static const size_t ERROR_LEN = 96;
    mutable SemaphoreHandle_t _textLock = nullptr;
    char _error[ERROR_LEN] = {};
    std::atomic<Failure> _failure{Failure::NONE};
    void copyText(char *dst, const char *src);   // under _textLock
    void clearError();                           // a new attempt starts without the old text
    Job _job;
    TaskHandle_t _task = nullptr;
    std::function<bool()> _idleCheck;

    // status posting: set by either task (setState), posted by the loop
    std::atomic<bool> _statusDirty{false};
    char _pendingStatus[16] = {};
    int _pendingProgress = 0;
    char _pendingError[ERROR_LEN] = {};

    // scheduling
    unsigned long _nextCheckAt = 0;
    unsigned long _waitingSince = 0;
    unsigned long _checkStartedAt = 0;   // CHECKING gives up after CHECK_TIMEOUT_MS
    unsigned long _bootAt = 0;
    bool _bootHealthyMarked = false;
    bool _networkHealthy = false;
    void confirmImage();
    bool _pendingVerify = false;
    bool _bootReported = false;
    bool _justUpdated = false;
    bool _bootedFromOta = false;
    std::atomic<bool> _justFailed{false};
};
