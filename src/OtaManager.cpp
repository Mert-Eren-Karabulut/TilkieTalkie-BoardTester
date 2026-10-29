#include "OtaManager.h"
#include "Version.h"
#include "RequestManager.h"
#include "ConfigManager.h"
#include "CaBundle.h"
#include "PromptPackManager.h"
#include "Faults.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <ArduinoJson.h>
#include <esp_log.h>

// The state machine is specified in docs/OTA-STATE-MACHINE.md (one row per state x event);
// the comments name the rows ("table: ...") where the code is not obvious.

namespace
{
const char *TAG = "ota";
const char *NVS_OTA_RELEASE = "ota_rel";   // release we rebooted into (set right before the reboot)
const char *NVS_OTA_VCODE = "ota_vcode";   // version_code that image should report
const char *NVS_OTA_REVERT = "ota_revert";   // set when the crash-loop guard switched partitions
const char *NVS_OTA_LAST_REL = "ota_last_rel"; // last release that was applied successfully
const char *NVS_OTA_BLACKLIST = "ota_rb";      // rolled-back release ids, comma separated, newest last
const int BLACKLIST_MAX = 4;

const unsigned long FIRST_CHECK_DELAY_MS = 90UL * 1000UL;     // + 0..30 s jitter: 90-120 s after boot
const unsigned long CHECK_INTERVAL_MS = 60UL * 60UL * 1000UL; // hourly (the portal also nudges)
const unsigned long CHECK_RETRY_SOON_MS = 2UL * 60UL * 1000UL; // transport failure / timeout
const unsigned long CHECK_TIMEOUT_MS = 60UL * 1000UL;          // no answer to a check
const unsigned long CHECK_5XX_BACKOFF_MS[] = {30UL * 1000UL, 2UL * 60UL * 1000UL, 10UL * 60UL * 1000UL};
const unsigned long DOCK_CHECK_MIN_AGE_MS = 10UL * 60UL * 1000UL; // docking checks when the last one is older
const unsigned long OFFER_MAX_AGE_MS = 10UL * 60UL * 1000UL;      // older offers are re-checked before starting
const unsigned long REOFFER_RETRY_MS = 2UL * 60UL * 1000UL;
const unsigned long MANDATORY_WAIT_MAX_MS = 60UL * 60UL * 1000UL;
const unsigned long ANNOUNCE_SETTLE_MS = 1500;  // the prompt is queued by the loop and starts a moment later
const unsigned long CONFIRM_MAX_MS = 10UL * 60UL * 1000UL;   // a pending image is confirmed after this uptime at the latest
const unsigned long STABLE_AFTER_MS = 5UL * 60UL * 1000UL;   // crash counter resets after this uptime
const unsigned long FAILED_RETRY_MS = 10UL * 60UL * 1000UL;  // FAILED -> IDLE
const float BATTERY_ABORT_PERCENT = 15.0f;                   // on battery, PROMPTS / DOWNLOADING stop below this
const uint32_t CRASH_MAGIC = 0x07A5EEDu;

const char *stateName(OtaManager::State s)
{
    switch (s)
    {
    case OtaManager::IDLE: return "idle";
    case OtaManager::CHECKING: return "checking";
    case OtaManager::WAITING: return "waiting";
    case OtaManager::PROMPTS: return "prompts";
    case OtaManager::DOWNLOADING: return "downloading";
    case OtaManager::APPLYING: return "applying";
    case OtaManager::REBOOTING: return "rebooting";
    case OtaManager::FAILED: return "failed";
    }
    return "?";
}

bool isCrashReset(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}
} // namespace

RTC_NOINIT_ATTR uint32_t g_otaCrashMagic;
RTC_NOINIT_ATTR uint32_t g_otaCrashCount;

OtaManager &OtaManager::getInstance()
{
    static OtaManager inst;
    return inst;
}

String OtaManager::deviceId() const
{
    char buf[21];
    snprintf(buf, sizeof(buf), "%llu", ESP.getEfuseMac());
    return String(buf);
}

void OtaManager::begin()
{
    _bootAt = millis();
    _nextCheckAt = _bootAt + FIRST_CHECK_DELAY_MS + (esp_random() % 30000);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t imgState;
    if (running && esp_ota_get_state_partition(running, &imgState) == ESP_OK)
    {
        _pendingVerify = (imgState == ESP_OTA_IMG_PENDING_VERIFY);
        _bootedFromOta = _pendingVerify;
    }
    ESP_LOGI(TAG, "firmware %s (code %u, %s) on %s%s", firmwareVersion(), (unsigned)firmwareVersionCode(),
                  TT_HARDWARE_REV, running ? running->label : "?", _pendingVerify ? " - pending verification" : "");

    // Crash-loop guard for images that were already confirmed: three crash resets in a row
    // without ever reaching STABLE_AFTER_MS of uptime switch back to the other partition if it
    // holds a bootable image (the previous firmware).
    if (g_otaCrashMagic != CRASH_MAGIC)
    {
        g_otaCrashMagic = CRASH_MAGIC;
        g_otaCrashCount = 0;
    }
    esp_reset_reason_t reason = esp_reset_reason();
    bool crashed = isCrashReset(reason);
    g_otaCrashCount = crashed ? g_otaCrashCount + 1 : 0;
    if (crashed)
    {
        ESP_LOGW(TAG, "crash reset #%u (reason %d)", (unsigned)g_otaCrashCount, (int)reason);
    }
    if (g_otaCrashCount >= 3 && !_pendingVerify)
    {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_app_desc_t desc;
        if (other && esp_ota_get_partition_description(other, &desc) == ESP_OK)
        {
            ESP_LOGW(TAG, "crash loop - reverting to %s (%s)", other->label, desc.version);
            Faults::raise(Fault::OTA_CRASH_LOOP_REVERT, desc.version);
            ConfigManager::getInstance().storeInt(NVS_OTA_REVERT, 1);
            g_otaCrashCount = 0;
            if (esp_ota_set_boot_partition(other) == ESP_OK)
            {
                delay(200);
                esp_restart();
            }
            ESP_LOGW(TAG, "revert failed, the other partition does not hold a valid image");
        }
    }
}

void OtaManager::notePortalReached(const char *how)
{
    if (!_portalReached)
    {
        ESP_LOGI(TAG, "portal reached (%s)", how ? how : "?");
    }
    _portalReached = true;
}

void OtaManager::confirmImage(const char *why)
{
    if (_bootHealthyMarked)
    {
        return;
    }
    _bootHealthyMarked = true;
    if (_pendingVerify)
    {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new image confirmed after %lu s: %s (%s)", millis() / 1000, why,
                      err == ESP_OK ? "rollback cancelled" : esp_err_to_name(err));
        _pendingVerify = false;
    }
    PromptPackManager::getInstance().onFirmwareConfirmed(); // the previous prompt pack can go
}

// table: "portal reached" / "10 min" on a pending image. Whichever comes first. A crash after
// the confirmation is the crash-loop guard's job (three crashes before 5 min of uptime).
void OtaManager::serviceConfirmation(unsigned long now)
{
    if (!_pendingVerify || _bootHealthyMarked)
    {
        return;
    }
    if (_portalReached)
    {
        confirmImage("portal reached (Reverb)");
    }
    else if (RequestManager::getInstance().lastPortalSuccessAt() != 0)
    {
        confirmImage("portal reached (API 2xx)");
    }
    else if (now - _bootAt > CONFIRM_MAX_MS)
    {
        confirmImage("10 min of uptime without the portal");
    }
}

void OtaManager::copyText(char *dst, const char *src)
{
    // Callers pass one of the ERROR_LEN buffers (or _pendingStatus, 16 bytes, with a short literal).
    const size_t cap = (dst == _pendingStatus) ? sizeof(_pendingStatus) : ERROR_LEN;
    strlcpy(dst, src ? src : "", cap);
}

String OtaManager::lastError() const
{
    char copy[ERROR_LEN];
    if (_textLock) xSemaphoreTake(_textLock, portMAX_DELAY);
    memcpy(copy, _error, sizeof(copy));
    if (_textLock) xSemaphoreGive(_textLock);
    copy[ERROR_LEN - 1] = 0;
    return String(copy);
}

const char *OtaManager::failureName(Failure f)
{
    switch (f)
    {
    case Failure::DOWNLOAD: return "download";
    case Failure::FLASH: return "flash";
    case Failure::SHA: return "sha";
    case Failure::PROMPTS: return "prompts";
    case Failure::BATTERY: return "battery";
    case Failure::EXPIRED: return "download";
    default: return "none";
    }
}

// Any task (the download task reports progress and the result through here).
void OtaManager::setState(State s, int progress, const char *error, Failure failure)
{
    if (progress >= 0)
    {
        _progress = progress;
    }
    // Status for the backend is posted by the loop task (RequestManager is not thread safe).
    const char *status = nullptr;
    switch (s)
    {
    case PROMPTS: status = "prompts"; break;
    case DOWNLOADING: status = "downloading"; break;
    case APPLYING: status = "applying"; break;
    case FAILED: status = "failed"; break;
    default: break;
    }
    const Failure kind = (s == FAILED) ? ((failure == Failure::NONE || failure == Failure::EXPIRED) ? Failure::DOWNLOAD : failure)
                                       : Failure::NONE;
    if (_textLock) xSemaphoreTake(_textLock, portMAX_DELAY);
    if (error && error[0])
    {
        copyText(_error, error);
    }
    if (status)
    {
        copyText(_pendingStatus, status);
        copyText(_pendingError, error);
        _pendingProgress = _progress;
        _pendingFailure = kind;
    }
    if (_textLock) xSemaphoreGive(_textLock);
    if (s == FAILED)
    {
        _failure = kind;
        _failedAt = millis();
        _justFailed = true;
    }
    if (status)
    {
        _statusDirty = true;
    }
    _state = s; // last: the loop sees the new state only with its texts in place
}

void OtaManager::failNow(Failure f, const char *error)
{
    ESP_LOGW(TAG, "update failed (%s): %s", failureName(f), error ? error : "");
    _hasOffer = false; // the next check re-offers
    setState(FAILED, _progress, error, f);
}

void OtaManager::postStatus(const char *status, int progress, const String &error, const char *failure, uint32_t versionCode,
                            uint32_t releaseId)
{
    JsonDocument doc;
    if (releaseId)
    {
        doc["release_id"] = releaseId; // optional server-side; 0 would fail validation
    }
    doc["status"] = status;
    doc["progress"] = progress;
    if (error.length() > 0)
    {
        doc["error"] = error;
    }
    if (failure && failure[0])
    {
        doc["failure"] = failure; // download | flash | sha | prompts | battery (extra key, ignored by older portals)
    }
    if (versionCode)
    {
        doc["version_code"] = versionCode;
    }
    // Posted by the request worker; the loop task is not held up by a slow or dead portal.
    // Progress posts are fire-and-forget (a re-sent "downloading 0%" landing after "90%" put
    // the portal's timeline out of order on the bench); the verdicts (success, failed,
    // rolled_back, declined) are re-sent after 30 s / 2 min / 10 min on 5xx or a timeout.
    const bool progressOnly = strcmp(status, "downloading") == 0 || strcmp(status, "prompts") == 0 ||
                              strcmp(status, "pending") == 0;
    const String statusCopy = status;
    const int progressCopy = progress;
    bool queued = RequestManager::getInstance().postAsync("/hubs/" + deviceId() + "/firmware/status", doc,
        [statusCopy, progressCopy](JsonDocument &, int code) {
            ESP_LOGI(TAG, "status '%s' %d%% -> HTTP %d", statusCopy.c_str(), progressCopy, code);
        }, progressOnly ? RequestManager::Retry::NONE : RequestManager::Retry::BACKGROUND);
    if (!queued)
    {
        ESP_LOGW(TAG, "status '%s' %d%% not sent (no network)", status, progress);
    }
}

bool OtaManager::checkNow(uint32_t releaseId, const char *reason)
{
    if (_factoryHold)
    {
        ESP_LOGW(TAG, "check (%s) refused, factory session", reason ? reason : "?");
        return false;
    }
    if (inProgress())
    {
        ESP_LOGI(TAG, "check (%s) ignored, update in progress", reason ? reason : "?");
        return false;
    }
    if (_state == CHECKING)
    {
        // table: "nudge / explicit command" in CHECKING: run right after the current answer.
        ESP_LOGI(TAG, "check (%s) queued behind the running one", reason ? reason : "?");
        _commandPending = true;
        _commandReleaseId = releaseId;
        return true;
    }
    return startCheck(CheckPurpose::COMMAND, releaseId, reason);
}

bool OtaManager::startCheck(CheckPurpose purpose, uint32_t releaseId, const char *reason)
{
    if (!WiFi.isConnected())
    {
        ESP_LOGI(TAG, "check (%s) skipped, no network", reason ? reason : "?");
        if (purpose == CheckPurpose::URL_EXPIRED)
        {
            failNow(Failure::DOWNLOAD, "download refused (403/410), no network for the re-check");
        }
        else if (purpose == CheckPurpose::REOFFER)
        {
            _reofferRetryAt = millis() + REOFFER_RETRY_MS;
        }
        return false;
    }
    PromptPackManager &ppm = PromptPackManager::getInstance();
    String endpoint = "/hubs/" + deviceId() + "/firmware/check?version_code=" + String(firmwareVersionCode()) +
                      "&version=" + String(firmwareVersion()) + "&hardware_rev=" + TT_HARDWARE_REV +
                      "&prompts=1&prompt_pack=" + ppm.installedChecksum() + "&prompt_state=" + ppm.stateName();
    if (releaseId)
    {
        endpoint += "&release_id=" + String(releaseId);
    }
    ESP_LOGI(TAG, "checking (%s)", reason ? reason : "?");
    const State before = _state;
    _checkPurpose = purpose;
    _checkReleaseId = releaseId;
    _state = CHECKING;
    _checkStartedAt = millis();
    _nextCheckAt = millis() + CHECK_INTERVAL_MS;
    // Retry::NONE: the answer comes back at once whatever the status; the schedule (Retry-After,
    // 5xx backoff) is ours. It arrives in onCheckResult() on the loop task.
    bool started = RequestManager::getInstance().getAsync(endpoint, [this](JsonDocument &doc, int code) {
        onCheckResult(doc, code);
    }, RequestManager::Retry::NONE);
    if (!started)
    {
        ESP_LOGI(TAG, "check not started (network not ready)");
        _state = before;
        checkFailed(purpose, "network not ready");
        _nextCheckAt = millis() + CHECK_RETRY_SOON_MS;
    }
    return started;
}

// table: check answer = error / timeout. Nothing about the offer is learnt: an offer we hold
// stays (WAITING; a re-check is not repeated for 2 min), a download that asked for the
// re-check fails.
void OtaManager::checkFailed(CheckPurpose purpose, const char *detail)
{
    if (purpose == CheckPurpose::URL_EXPIRED)
    {
        char e[ERROR_LEN];
        snprintf(e, sizeof(e), "download refused (403/410), re-check failed (%s)", detail ? detail : "?");
        failNow(Failure::DOWNLOAD, e);
        return;
    }
    if (_hasOffer)
    {
        _state = WAITING;
        _reofferRetryAt = millis() + REOFFER_RETRY_MS;
    }
    else
    {
        _state = IDLE;
    }
}

void OtaManager::onCheckResult(JsonDocument &doc, int code)
{
    if (_state != CHECKING)
    {
        return; // timed out, or something else (a console job) took over while the check was in flight
    }
    const CheckPurpose purpose = _checkPurpose;
    const unsigned long now = millis();
    if (code > 0)
    {
        _lastCheckAt = now;
    }
    if (code == 429)
    {
        // Throttled by the portal: not a failure of the toy, no fault; ask again after Retry-After.
        const int ra = doc["_retry_after"] | 60;
        ESP_LOGW(TAG, "check throttled (429), next in %d s", ra);
        _nextCheckAt = now + (unsigned long)ra * 1000UL;
        if (purpose == CheckPurpose::REOFFER && _hasOffer)
        {
            _state = WAITING;
            _reofferRetryAt = _nextCheckAt;
            return;
        }
        checkFailed(purpose, "http=429");
        return;
    }
    if (code >= 500)
    {
        const unsigned long wait = CHECK_5XX_BACKOFF_MS[_check5xx < 2 ? _check5xx : 2];
        if (_check5xx < 255) _check5xx++;
        ESP_LOGW(TAG, "check failed (HTTP %d), next in %lu s", code, wait / 1000);
        if (_check5xx >= 3)
        {
            char d[16];
            snprintf(d, sizeof(d), "http=%d", code);
            Faults::raise(Fault::OTA_CHECK_FAILED, d, false);
        }
        _nextCheckAt = now + wait;
        checkFailed(purpose, "http=5xx");
        if (_state == WAITING)
        {
            _reofferRetryAt = now + wait;
        }
        return;
    }
    if (code != 200 && code != 204)
    {
        ESP_LOGW(TAG, "check failed (HTTP %d)", code);
        char d[16];
        snprintf(d, sizeof(d), "http=%d", code);
        Faults::raise(Fault::OTA_CHECK_FAILED, d, false);
        _nextCheckAt = now + (code <= 0 ? CHECK_RETRY_SOON_MS : CHECK_INTERVAL_MS);
        checkFailed(purpose, d);
        return;
    }
    _check5xx = 0;
    Faults::clear(Fault::OTA_CHECK_FAILED); // the check answered
    if (code == 204 || !doc["release_id"].is<uint32_t>())
    {
        nothingOffered(purpose, doc);
        return;
    }

    PromptPackManager &ppm = PromptPackManager::getInstance();
    PromptPackManager::Pack pack;
    const bool hasPack = PromptPackManager::parsePack(doc["prompts"], pack);
    Job job;
    job.releaseId = doc["release_id"].as<uint32_t>();
    job.version = doc["version"].as<String>();
    job.versionCode = doc["version_code"].as<uint32_t>();
    job.url = doc["url"].as<String>();
    job.sha256 = doc["sha256"].as<String>();
    job.size = doc["file_size"].as<size_t>();
    job.mandatory = doc["mandatory"].as<bool>();
    job.explicitOffer = doc["explicit"] | false; // absent (older portal) = not explicit
    job.hasPrompts = hasPack;
    job.prompts = pack;
    ESP_LOGI(TAG, "release %u offered: %s (code %u, %u bytes%s%s%s)", (unsigned)job.releaseId, job.version.c_str(),
                  (unsigned)job.versionCode, (unsigned)job.size, job.mandatory ? ", mandatory" : "",
                  job.explicitOffer ? ", explicit" : "",
                  hasPack ? (pack.checksum == ppm.installedChecksum() ? ", prompts up to date" : ", new prompt pack") : "");
    if (job.url.isEmpty() || job.sha256.length() != 64 || job.size == 0)
    {
        ESP_LOGW(TAG, "offer incomplete, ignored");
        if (purpose == CheckPurpose::URL_EXPIRED)
        {
            failNow(Failure::DOWNLOAD, "download refused (403/410), the re-check offered an incomplete release");
            return;
        }
        _hasOffer = false;
        _state = IDLE;
        return;
    }
    // table: check answer = blacklisted / not newer. Explicit offers (pin, command) override both.
    const uint32_t running = firmwareVersionCode();
    if (!job.explicitOffer && isBlacklisted(job.releaseId))
    {
        decline(job, "this release rolled back on this device before (offer it explicitly to retry)");
        return;
    }
    if (!job.explicitOffer && running != 0 && job.versionCode <= running)
    {
        decline(job, "not newer than the running firmware");
        return;
    }
    if (purpose == CheckPurpose::URL_EXPIRED)
    {
        if (_hasOffer && job.releaseId == _job.releaseId && job.sha256.equalsIgnoreCase(_job.sha256) && job.size == _job.size)
        {
            // table: download 403/410 -> re-check -> same release: the download continues with the
            // fresh URL (undocking does not matter any more, as in DOWNLOADING).
            ESP_LOGI(TAG, "fresh download URL for release %u - downloading again", (unsigned)job.releaseId);
            _job.url = job.url;
            _offerAt = now;
            launchDownload();
            return;
        }
        // Another release now: this attempt ends; the new offer waits like any offer.
        char e[ERROR_LEN];
        snprintf(e, sizeof(e), "download refused (403/410), release replaced by %u", (unsigned)job.releaseId);
        postStatus("failed", _progress, String(e), "download", running, _job.releaseId);
        _hasOffer = false;
    }
    acceptOffer(job);
}

// table: check answer = nothing to install. An offer we held is withdrawn (revoked, unpinned,
// rollout changed); an interrupted download ends as failed so the portal's status is not left
// at "downloading".
void OtaManager::nothingOffered(CheckPurpose purpose, JsonDocument &doc)
{
    if (_hasOffer)
    {
        ESP_LOGI(TAG, "release %u is no longer offered - offer dropped", (unsigned)_job.releaseId);
        if (purpose == CheckPurpose::URL_EXPIRED)
        {
            failNow(Failure::DOWNLOAD, "release no longer offered (download refused with 403/410)");
            return;
        }
    }
    else
    {
        ESP_LOGI(TAG, "up to date");
    }
    _hasOffer = false;
    _state = IDLE;
    // With prompts=1 "prompts" names the voice pack paired with the running release.
    PromptPackManager &ppm = PromptPackManager::getInstance();
    PromptPackManager::Pack pack;
    if (PromptPackManager::parsePack(doc["prompts"], pack))
    {
        ppm.noteRequired(pack.checksum);
        if (pack.checksum != ppm.installedChecksum())
        {
            ESP_LOGI(TAG, "prompt pack %.12s (%s) differs from the card (%s) - syncing it", pack.checksum.c_str(),
                          pack.name.c_str(), ppm.installedChecksum().isEmpty() ? "none" : ppm.installedChecksum().substring(0, 12).c_str());
            ppm.stage(pack, firmwareVersion(), [](bool ok) {
                ESP_LOGI(TAG, "prompt pack sync %s", ok ? "complete - activated when nothing speaks" : "failed, retried at the next check");
            });
            return;
        }
    }
    ppm.discardStaleStaging();
}

void OtaManager::decline(const Job &job, const char *why)
{
    ESP_LOGW(TAG, "release %u declined: %s", (unsigned)job.releaseId, why);
    postStatus("declined", 0, String(why), nullptr, firmwareVersionCode(), job.releaseId);
    _hasOffer = false;
    _state = IDLE;
}

void OtaManager::acceptOffer(const Job &job)
{
    const unsigned long now = millis();
    const bool same = _hasOffer && job.releaseId == _job.releaseId;
    if (!same)
    {
        _waitingSince = now; // the mandatory hour counts from the first offer of this release
        _announced = false;
        _offerCommandReleaseId = 0;
    }
    if (_checkPurpose == CheckPurpose::COMMAND && _checkReleaseId)
    {
        _offerCommandReleaseId = _checkReleaseId; // a re-check asks for the same release again
    }
    _job = job;
    _hasOffer = true;
    _offerAt = now;
    _reofferRetryAt = 0;
    _state = WAITING;
    ESP_LOGI(TAG, "waiting for the dock and an idle toy%s", job.mandatory ? " (mandatory: figure/playing ignored after 1 h)" : "");
}

// table: WAITING x dock / figure / play / download. Leaves only when docked AND (nothing
// playing or paused, no figure, no story download - or a mandatory release waited 1 h).
void OtaManager::serviceWaiting(unsigned long now, const Conditions &c)
{
    const bool forced = _job.mandatory && now - _waitingSince > MANDATORY_WAIT_MAX_MS;
    if (!c.docked || c.figure)
    {
        _announced = false; // said for a docked, empty moment: say it again next time
    }
    const bool idle = !c.playing && !c.paused && !c.figure && !c.storyDownload;
    if (!c.docked || (!forced && !idle))
    {
        return;
    }
    if (now - _offerAt > OFFER_MAX_AGE_MS)
    {
        // table: leaving WAITING with an offer older than 10 min: fresh signed URL, and the
        // release may have changed or been revoked. The answer replaces the offer.
        if (_reofferRetryAt == 0 || (long)(now - _reofferRetryAt) >= 0)
        {
            ESP_LOGI(TAG, "offer is %lu s old - re-checking before starting", (now - _offerAt) / 1000);
            startCheck(CheckPurpose::REOFFER, _offerCommandReleaseId, "re-check before starting");
        }
        return;
    }
    if (!_announced)
    {
        // "Kendimi güncelliyorum..." first (main.cpp speaks it); the download starts once it
        // is over (it counts as playing). A forced start does not wait for silence.
        _announced = true;
        _announcedAt = now;
        _announceRequest = true;
        return;
    }
    if (now - _announcedAt < ANNOUNCE_SETTLE_MS)
    {
        return;
    }
    beginJob();
}

// Prompts first: the pack paired with the release is staged into /sounds.staging while the
// old prompts stay in use; the new firmware activates it at its first boot. An unchanged pack
// (already on the card) is skipped. If the pack cannot be fetched twice in a row the image is
// installed anyway (that firmware then syncs its pack itself at its first check), so a bad
// pack never blocks a firmware update for good.
void OtaManager::beginJob()
{
    _expiredRecheckUsed = false;
    PromptPackManager &ppm = PromptPackManager::getInstance();
    if (_job.hasPrompts && _job.prompts.checksum != ppm.installedChecksum() && _promptSyncFailures < 2)
    {
        clearError();
        setState(PROMPTS, 0);
        const bool started = ppm.stage(_job.prompts, _job.version, [this](bool ok) { onPromptsStaged(ok); });
        if (!started)
        {
            onPromptsStaged(false);
        }
        return;
    }
    launchDownload();
}

void OtaManager::onPromptsStaged(bool ok)
{
    if (_state != PROMPTS)
    {
        return; // aborted meanwhile (battery)
    }
    if (ok)
    {
        _promptSyncFailures = 0;
        ESP_LOGI(TAG, "prompt pack staged - downloading the image");
        launchDownload();
        return;
    }
    _promptSyncFailures++;
    failNow(Failure::PROMPTS, "voice prompt pack could not be downloaded");
}

bool OtaManager::startJob(const Job &job)
{
    if (_factoryHold)
    {
        ESP_LOGW(TAG, "job refused, factory session");
        return false;
    }
    // Console (bench) path: no waiting rules, no re-check on 403 (there is no offer to refresh).
    if (_state == DOWNLOADING || _state == APPLYING || _state == REBOOTING)
    {
        return false;
    }
    _job = job;
    _hasOffer = job.releaseId != 0;
    _expiredRecheckUsed = true;
    return launchDownload();
}

bool OtaManager::launchDownload()
{
    // PROMPTS -> DOWNLOADING is the normal hand-over from the pack staging; only a transfer or
    // an activation already under way refuses a new one.
    if (_state == DOWNLOADING || _state == APPLYING || _state == REBOOTING || _task != nullptr)
    {
        return false;
    }
    _abortRequested = false;
    _expiredPending = false;
    clearError();
    setState(DOWNLOADING, 0);
    if (xTaskCreatePinnedToCore(taskEntry, "ota", 8192, this, 5, &_task, 0) != pdPASS)
    {
        _task = nullptr;
        failNow(Failure::DOWNLOAD, "could not start the update task");
        return false;
    }
    return true;
}

void OtaManager::taskEntry(void *arg)
{
    static_cast<OtaManager *>(arg)->runDownload();
    WiFi.setSleep(true); // back to power saving
    static_cast<OtaManager *>(arg)->_task = nullptr;
    vTaskDelete(NULL);
}

void OtaManager::runDownload()
{
    ESP_LOGI(TAG, "downloading release %u (%u bytes)", (unsigned)_job.releaseId, (unsigned)_job.size); // never the signed URL
    const bool https = _job.url.startsWith("https://");
    WiFiClient plain;
    WiFiClientSecure secure;
    if (https)
    {
        secure.setCACertBundle(caBundleStart(), caBundleSize());
    }
    WiFiClient &client = https ? static_cast<WiFiClient &>(secure) : plain;

    // Full-throughput WiFi for the download: the default modem sleep (DTIM listen) roughly
    // halves TCP throughput and stretches every retransmission on a weak link. Restored by taskEntry.
    WiFi.setSleep(false);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf)
    {
        setState(FAILED, _progress, "no memory for the download buffer", Failure::DOWNLOAD);
        return;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    size_t total = _job.size;
    size_t received = 0;
    bool flashStarted = false;
    int lastReported = -1;
    String failure;
    Failure failureKind = Failure::NONE;   // classified where it happens; FLASH/BATTERY/EXPIRED end the retries
    const int maxAttempts = 6;
    const char *batteryText = "battery under 15 % on battery";

    // The image is fetched in one or more HTTP requests: a dropped connection resumes with a
    // Range request (the backend answers 206), so a flaky home network does not restart a
    // 2 MB download from zero. Flash writes and the hash simply continue.
    for (int attempt = 1; attempt <= maxAttempts && (total == 0 || received < total); ++attempt)
    {
        if (_abortRequested)
        {
            failure = batteryText;
            failureKind = Failure::BATTERY;
            break;
        }
        if (attempt > 1)
        {
            ESP_LOGI(TAG, "resuming at %u bytes (attempt %d)", (unsigned)received, attempt);
            vTaskDelay(pdMS_TO_TICKS(1500 * attempt));
        }
        HTTPClient http;
        http.setTimeout(20000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setReuse(false);
        if (!http.begin(client, _job.url))
        {
            failure = "could not open the download URL";
            continue;
        }
        if (received > 0)
        {
            http.addHeader("Range", "bytes=" + String(received) + "-");
        }
        int code = http.GET();
        if (!(code == HTTP_CODE_OK || (received > 0 && code == HTTP_CODE_PARTIAL_CONTENT)))
        {
            http.end();
            failure = "download HTTP " + String(code);
            if (code == 403 || code == 410)
            {
                // Signed URL expired (403) or release revoked (410): re-checked once by the loop.
                failureKind = Failure::EXPIRED;
                break;
            }
            if (code == 404)
            {
                break; // not going to get better
            }
            continue;
        }
        if (received > 0 && code == HTTP_CODE_OK)
        {
            http.end();
            failure = "server ignored the Range request";
            break;
        }
        int remaining = http.getSize();
        if (total == 0)
        {
            total = (size_t)remaining;
        }
        if (remaining > 0 && (size_t)remaining != total - received)
        {
            http.end();
            failure = "size mismatch: server " + String(remaining) + ", expected " + String(total - received);
            break;
        }
        if (!flashStarted)
        {
            if (!Update.begin(total, U_FLASH))
            {
                http.end();
                failure = String("flash: ") + Update.errorString();
                failureKind = Failure::FLASH;
                break;
            }
            flashStarted = true;
        }

        WiFiClient *stream = http.getStreamPtr();
        unsigned long lastData = millis();
        failure = "";
        while (received < total)
        {
            if (_abortRequested)
            {
                failure = batteryText;
                failureKind = Failure::BATTERY;
                break;
            }
            size_t avail = stream->available();
            if (avail == 0)
            {
                if (!http.connected())
                {
                    failure = "connection closed early";
                    break;
                }
                if (millis() - lastData > 20000)
                {
                    failure = "download stalled";
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            size_t want = total - received;
            if (want > 4096) want = 4096;
            if (want > avail) want = avail;
            int n = stream->read(buf, want);
            if (n <= 0)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            lastData = millis();
            if (Update.write(buf, (size_t)n) != (size_t)n)
            {
                failure = String("flash write: ") + Update.errorString();
                failureKind = Failure::FLASH;
                break;
            }
            mbedtls_sha256_update(&sha, buf, (size_t)n);
            received += (size_t)n;
            int pct = (int)((uint64_t)received * 100 / (uint64_t)total);
            if (pct != _progress)
            {
                _progress = pct;
                if (pct / 10 != lastReported / 10 && pct < 100)
                {
                    lastReported = pct;
                    setState(DOWNLOADING, pct); // coarse progress for the backend
                }
            }
        }
        http.end();
        if (failureKind == Failure::FLASH || failureKind == Failure::BATTERY)
        {
            break; // retrying will not help
        }
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    heap_caps_free(buf);

    if (received < total || failure.length() > 0)
    {
        if (flashStarted)
        {
            Update.abort();
        }
        if (failureKind == Failure::EXPIRED && !_expiredRecheckUsed)
        {
            // The loop asks the portal again (fresh URL) instead of failing; state stays DOWNLOADING.
            ESP_LOGW(TAG, "%s - re-checking the release once", failure.c_str());
            _expiredPending = true;
            return;
        }
        _hasOffer = false;
        setState(FAILED, _progress, failure.length() ? failure.c_str() : "incomplete download",
                 failureKind == Failure::EXPIRED ? Failure::DOWNLOAD : (failureKind == Failure::NONE ? Failure::DOWNLOAD : failureKind));
        return;
    }

    char hex[65];
    for (int i = 0; i < 32; ++i)
    {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    String expected = _job.sha256;
    expected.toLowerCase();
    if (expected != hex)
    {
        Update.abort();
        _hasOffer = false;
        setState(FAILED, _progress, "sha256 mismatch", Failure::SHA);
        return;
    }

    setState(APPLYING, 100);
    if (!Update.end(true))
    {
        _hasOffer = false;
        setState(FAILED, 100, (String("activate: ") + Update.errorString()).c_str(), Failure::FLASH);
        return;
    }
    // Remember what we expect to boot into so the result can be reported after the restart.
    // (A power loss between the activation above and these writes boots the new image without
    // the record: it confirms itself and the portal learns the version from the report's "fw".)
    ConfigManager &cfg = ConfigManager::getInstance();
    cfg.storeInt(NVS_OTA_RELEASE, (int)_job.releaseId);
    cfg.storeInt(NVS_OTA_VCODE, (int)_job.versionCode);
    ESP_LOGI(TAG, "image %s verified and activated, rebooting", _job.version.c_str());
    _state = REBOOTING;
}

// table: "reboot" into the new image / "rollback detected". Reported once the image is
// confirmed (a crash before that leaves the NVS record for the rolled-back firmware).
void OtaManager::serviceBootReport(unsigned long now)
{
    if (_bootReported || !WiFi.isConnected() || now - _bootAt <= 8000 || (_pendingVerify && !_bootHealthyMarked))
    {
        return;
    }
    _bootReported = true;
    ConfigManager &cfg = ConfigManager::getInstance();
    uint32_t rel = (uint32_t)cfg.getInt(NVS_OTA_RELEASE, 0);
    const bool reverted = cfg.getInt(NVS_OTA_REVERT, 0) != 0;
    if (!rel && !reverted)
    {
        return;
    }
    const uint32_t want = (uint32_t)cfg.getInt(NVS_OTA_VCODE, 0);
    const uint32_t running = firmwareVersionCode();
    if (!rel && reverted)
    {
        rel = (uint32_t)cfg.getInt(NVS_OTA_LAST_REL, 0); // the image we just abandoned
    }
    // "Booted from the update" is normally the PENDING_VERIFY state the bootloader gives a new
    // image. A bootloader without the rollback feature (or one that already confirmed the image)
    // leaves no such state, so the record itself is the second witness: the release we rebooted
    // into reports exactly the version_code it was expected to have.
    const bool newImageRuns = _bootedFromOta || (rel && want && want == running);
    if (!reverted && newImageRuns)
    {
        // The new image runs and was confirmed. A version_code different from the release's
        // can only come from a formula mismatch (the portal now derives it from the image):
        // the update itself worked, so it is a success, not a rollback.
        if (want != running)
        {
            ESP_LOGW(TAG, "running code %u, release said %u - reported as success with the running code",
                          (unsigned)running, (unsigned)want);
        }
        cfg.storeInt(NVS_OTA_LAST_REL, (int)rel);
        blacklistRemove(rel); // an explicit retry of a rolled-back release worked
        postStatus("success", 100, String(), nullptr, running, rel);
    }
    else
    {
        // The previous image runs again. Bootloader rollback (the new partition is ABORTED or
        // INVALID) or crash-loop revert: report rolled_back. Remember the release so it is not
        // installed again automatically - unless the new image went down by a power loss or a
        // user reset before it was confirmed (not the release's fault).
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        const bool haveState = other && esp_ota_get_state_partition(other, &st) == ESP_OK;
        const bool bootloaderRollback = haveState && (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID);
        const esp_reset_reason_t why = esp_reset_reason();
        char err[ERROR_LEN];
        if (reverted || bootloaderRollback)
        {
            const bool blame = reverted || isCrashReset(why);
            if (reverted)
            {
                snprintf(err, sizeof(err), "crash loop on the new image; reverted to the previous firmware");
            }
            else
            {
                snprintf(err, sizeof(err), "new image not confirmed (reset reason %d%s); bootloader returned to the previous firmware",
                         (int)why, blame ? ", crash" : "");
            }
            if (blame && rel)
            {
                blacklistAdd(rel);
            }
            Faults::raise(Fault::OTA_ROLLED_BACK, reverted ? "crash loop" : (blame ? "crash before confirm" : "reset before confirm"));
            postStatus("rolled_back", 100, String(err), nullptr, running, rel);
        }
        else
        {
            // Activated but never started (or the record is stale): the old image never left.
            snprintf(err, sizeof(err), "new image did not start (reset reason %d)", (int)why);
            postStatus("failed", 100, String(err), "flash", running, rel);
        }
    }
    cfg.deleteSetting(NVS_OTA_RELEASE);
    cfg.deleteSetting(NVS_OTA_VCODE);
    cfg.deleteSetting(NVS_OTA_REVERT);
}

void OtaManager::update()
{
    const unsigned long now = millis();

    // 1. Image confirmation and the crash-loop counter.
    serviceConfirmation(now);
    if (g_otaCrashCount && now - _bootAt > STABLE_AFTER_MS)
    {
        g_otaCrashCount = 0;
    }

    // 2. How the previous update ended.
    serviceBootReport(now);

    // 3. Status changes made by either task.
    if (_statusDirty.exchange(false))
    {
        char status[sizeof(_pendingStatus)];
        char error[ERROR_LEN];
        int progress;
        Failure failure;
        if (_textLock) xSemaphoreTake(_textLock, portMAX_DELAY);
        memcpy(status, _pendingStatus, sizeof(status));
        memcpy(error, _pendingError, sizeof(error));
        progress = _pendingProgress;
        failure = _pendingFailure;
        if (_textLock) xSemaphoreGive(_textLock);
        status[sizeof(status) - 1] = 0;
        error[sizeof(error) - 1] = 0;
        if (WiFi.isConnected())
        {
            postStatus(status, progress, String(error), failure == Failure::NONE ? nullptr : failureName(failure),
                       firmwareVersionCode(), _job.releaseId);
        }
    }

    if (_factoryHold)
    {
        return; // factory session: the state machine waits (no checks, no jobs)
    }

    Conditions c;
    if (_conditionsFn)
    {
        c = _conditionsFn();
    }
    const bool dockEdge = c.docked && !_wasDocked;
    _wasDocked = c.docked;
    const bool lowOnBattery = !c.docked && c.batteryKnown && c.batteryPercent < BATTERY_ABORT_PERCENT;

    // A queued ota-update (it came while a check was running) runs as soon as one may.
    if (_commandPending && (_state == IDLE || _state == WAITING || _state == FAILED) && WiFi.isConnected())
    {
        _commandPending = false;
        startCheck(CheckPurpose::COMMAND, _commandReleaseId, "command (queued)");
    }

    switch (_state)
    {
    case CHECKING:
        // table: CHECKING x timeout. The worker may be stuck; a late answer is ignored.
        if (now - _checkStartedAt > CHECK_TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "check timed out after %lu s", CHECK_TIMEOUT_MS / 1000);
            Faults::raise(Fault::OTA_CHECK_FAILED, "timeout", false);
            _nextCheckAt = now + CHECK_RETRY_SOON_MS;
            checkFailed(_checkPurpose, "timeout");
        }
        break;

    case REBOOTING:
    {
        // Reboot once the final status went out (the worker gets up to 10 s to deliver it).
        static unsigned long rebootRequestedAt = 0;
        if (!rebootRequestedAt)
        {
            rebootRequestedAt = now;
        }
        if (!RequestManager::getInstance().hasPendingRequests() || now - rebootRequestedAt > 10000)
        {
            delay(500);
            ESP.restart();
        }
        break;
    }

    case WAITING:
        serviceWaiting(now, c);
        break;

    case PROMPTS:
        _progress = PromptPackManager::getInstance().stagingProgress();
        if (lowOnBattery)
        {
            // table: PROMPTS x battery low. FAILED first, so the cancelled staging's callback is ignored.
            failNow(Failure::BATTERY, "battery under 15 % on battery (prompt pack staging stopped)");
            PromptPackManager::getInstance().cancelStaging();
        }
        break;

    case DOWNLOADING:
        if (lowOnBattery && !_abortRequested)
        {
            ESP_LOGW(TAG, "battery %.0f %% on battery - stopping the download", c.batteryPercent);
            _abortRequested = true; // the task ends with FAILED (battery)
        }
        if (_expiredPending && _task == nullptr)
        {
            // table: DOWNLOADING x download 403/410 (first time this attempt): re-check instead of failing.
            _expiredPending = false;
            _expiredRecheckUsed = true;
            startCheck(CheckPurpose::URL_EXPIRED, _offerCommandReleaseId, "download refused, fresh URL");
        }
        break;

    case FAILED:
        // A failed attempt goes back to the periodic checks after a while (the portal's push
        // "ota-update" also restarts it any time).
        if (now - _failedAt > FAILED_RETRY_MS)
        {
            _state = IDLE;
            _hasOffer = false;
            _nextCheckAt = now;
        }
        break;

    case IDLE:
        if (!WiFi.isConnected() || !_networkStarted)
        {
            break;
        }
        if (dockEdge && _lastCheckAt != 0 && now - _lastCheckAt > DOCK_CHECK_MIN_AGE_MS && (long)(now - _nextCheckAt) < 0)
        {
            startCheck(CheckPurpose::PERIODIC, 0, "docked");
        }
        else if ((long)(now - _nextCheckAt) >= 0)
        {
            startCheck(CheckPurpose::PERIODIC, 0, "periodic");
        }
        break;

    default:
        break;
    }
}

bool OtaManager::isBlacklisted(uint32_t releaseId)
{
    if (!releaseId)
    {
        return false;
    }
    const String list = "," + ConfigManager::getInstance().getString(NVS_OTA_BLACKLIST, "") + ",";
    return list.indexOf("," + String(releaseId) + ",") >= 0;
}

void OtaManager::blacklistAdd(uint32_t releaseId)
{
    if (!releaseId || isBlacklisted(releaseId))
    {
        return;
    }
    String list = ConfigManager::getInstance().getString(NVS_OTA_BLACKLIST, "");
    list = list.length() ? list + "," + String(releaseId) : String(releaseId);
    // keep the newest BLACKLIST_MAX
    int count = 1;
    for (size_t i = 0; i < list.length(); ++i) if (list[i] == ',') count++;
    while (count > BLACKLIST_MAX)
    {
        list = list.substring(list.indexOf(',') + 1);
        count--;
    }
    ConfigManager::getInstance().storeString(NVS_OTA_BLACKLIST, list);
    ESP_LOGW(TAG, "release %u remembered as rolled back (declined unless offered explicitly): [%s]", (unsigned)releaseId, list.c_str());
}

void OtaManager::blacklistRemove(uint32_t releaseId)
{
    if (!isBlacklisted(releaseId))
    {
        return;
    }
    String list = "," + ConfigManager::getInstance().getString(NVS_OTA_BLACKLIST, "") + ",";
    list.replace("," + String(releaseId) + ",", ",");
    list = list.substring(1, list.length() > 1 ? list.length() - 1 : 1);
    if (list.isEmpty() || list == ",")
    {
        ConfigManager::getInstance().deleteSetting(NVS_OTA_BLACKLIST);
    }
    else
    {
        ConfigManager::getInstance().storeString(NVS_OTA_BLACKLIST, list);
    }
}

void OtaManager::printStatus()
{
    const String err = lastError();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "firmware %s code %u hw %s | partition %s (next %s)%s | state %s %d%%%s%s%s%s%s",
                  firmwareVersion(), (unsigned)firmwareVersionCode(), TT_HARDWARE_REV,
                  running ? running->label : "?", next ? next->label : "?",
                  _pendingVerify ? " pending-verify" : (_bootHealthyMarked ? " confirmed" : ""), ::stateName(_state), (int)_progress,
                  err.length() ? " error: " : "", err.c_str(), err.length() ? " (" : "",
                  err.length() ? failureName(lastFailure()) : "", err.length() ? ")" : "");
    if (_hasOffer)
    {
        ESP_LOGI(TAG, "offer release %u %s (code %u) %u bytes%s%s, %lu s old, waiting %lu s", (unsigned)_job.releaseId,
                      _job.version.c_str(), (unsigned)_job.versionCode, (unsigned)_job.size, _job.mandatory ? " mandatory" : "",
                      _job.explicitOffer ? " explicit" : "", (millis() - _offerAt) / 1000, (millis() - _waitingSince) / 1000);
    }
    ESP_LOGI(TAG, "next periodic check in %ld s, crash resets in a row %u, rolled back [%s]",
                  (long)((long)_nextCheckAt - (long)millis()) / 1000, (unsigned)g_otaCrashCount,
                  ConfigManager::getInstance().getString(NVS_OTA_BLACKLIST, "").c_str());
    {
        PromptPackManager &ppm = PromptPackManager::getInstance();
        ESP_LOGI(TAG, "prompt pack: %s (installed %s, paired with this release %s)", ppm.stateName(),
                 ppm.installedChecksum().isEmpty() ? "none" : ppm.installedChecksum().substring(0, 12).c_str(),
                 ppm.requiredChecksum().isEmpty() ? "not learnt yet" : ppm.requiredChecksum().substring(0, 12).c_str());
    }
}

bool OtaManager::consumeJustFailed()
{
    return _justFailed.exchange(false);
}

bool OtaManager::consumeAnnounceRequest()
{
    return _announceRequest.exchange(false);
}

void OtaManager::clearError()
{
    if (_textLock) xSemaphoreTake(_textLock, portMAX_DELAY);
    _error[0] = 0;
    if (_textLock) xSemaphoreGive(_textLock);
    _failure = Failure::NONE;
}

const char *OtaManager::stateName() const
{
    return ::stateName(_state);
}
