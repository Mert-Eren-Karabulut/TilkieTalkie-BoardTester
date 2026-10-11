#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
        DOWNLOADING,
        APPLYING,
        REBOOTING,
        FAILED
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
    };

    static OtaManager &getInstance();

    void begin();            // boot-time bookkeeping (pending image / previous attempt)
    void update();           // loop task: validation, periodic check, deferred start, status posts
    bool checkNow(uint32_t releaseId = 0, const char *reason = "manual"); // ask the backend
    bool startJob(const Job &job);                                     // explicit (bench / command)
    void markBootHealthy();  // network is up (the image itself is confirmed after 60 s of uptime)

    State state() const { return _state; }
    int progress() const { return _progress; }
    bool inProgress() const { return _state == DOWNLOADING || _state == APPLYING || _state == REBOOTING; }
    const Job &job() const { return _job; }
    String lastError() const { return _error; }
    void printStatus();
    bool consumeJustUpdated();  // true once, on the boot that confirmed a new image
    bool consumeJustFailed();   // true once after an update attempt failed

    // Gate for starting a download: true when the toy is idle enough for an update.
    void setIdleCheck(std::function<bool()> fn) { _idleCheck = fn; }

private:
    OtaManager() = default;
    static void taskEntry(void *arg);
    void runDownload();
    void setState(State s, int progress = -1, const String &error = String());
    void postStatus(const char *status, int progress, const String &error, uint32_t versionCode = 0);
    void onCheckResult(JsonDocument &doc, int code); // loop task, answer to checkNow()
    String deviceId() const;

    volatile State _state = IDLE;
    volatile int _progress = 0;
    String _error;
    Job _job;
    TaskHandle_t _task = nullptr;
    std::function<bool()> _idleCheck;

    // status posting (loop task only)
    volatile bool _statusDirty = false;
    String _pendingStatus;
    int _pendingProgress = 0;
    String _pendingError;
    unsigned long _lastProgressPostAt = 0;

    // scheduling
    unsigned long _nextCheckAt = 0;
    unsigned long _waitingSince = 0;
    unsigned long _bootAt = 0;
    bool _bootHealthyMarked = false;
    bool _networkHealthy = false;
    void confirmImage();
    bool _pendingVerify = false;
    bool _bootReported = false;
    bool _justUpdated = false;
    bool _justFailed = false;
};
