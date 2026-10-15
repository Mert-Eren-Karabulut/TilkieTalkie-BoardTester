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

namespace
{
const char *TAG = "ota";
const char *NVS_OTA_RELEASE = "ota_rel";   // release we rebooted into (set right before the reboot)
const char *NVS_OTA_VCODE = "ota_vcode";   // version_code that image should report
const unsigned long CHECK_INTERVAL_MS = 60UL * 60UL * 1000UL; // the portal also pushes ota-update when a release is published
const unsigned long CHECK_RETRY_MS = 60UL * 60UL * 1000UL;
const unsigned long CHECK_RETRY_SOON_MS = 2UL * 60UL * 1000UL; // connection-level failure
const unsigned long CHECK_TIMEOUT_MS = 60UL * 1000UL;  // no answer to a check: back to IDLE
const unsigned long FIRST_CHECK_DELAY_MS = 90UL * 1000UL;
const unsigned long MANDATORY_WAIT_MAX_MS = 60UL * 60UL * 1000UL;
const unsigned long CONFIRM_AFTER_MS = 60UL * 1000UL;   // a crash before this rolls back (bootloader)
const unsigned long STABLE_AFTER_MS = 5UL * 60UL * 1000UL; // crash counter resets after this uptime
const unsigned long FAILED_RETRY_MS = 10UL * 60UL * 1000UL;  // after a failed attempt, back to periodic checks
const char *NVS_OTA_REVERT = "ota_revert";   // set when the crash-loop guard switched partitions
const char *NVS_OTA_LAST_REL = "ota_last_rel"; // last release that was applied successfully
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
    bool crashed = (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT);
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

void OtaManager::markBootHealthy()
{
    // Network is up: noted, but the image is only confirmed after CONFIRM_AFTER_MS of uptime
    // (a crash shortly after connecting must still roll back).
    _networkHealthy = true;
}

void OtaManager::confirmImage()
{
    if (_bootHealthyMarked)
    {
        return;
    }
    _bootHealthyMarked = true;
    if (_pendingVerify)
    {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new image confirmed after %lu s (%s)", millis() / 1000,
                      err == ESP_OK ? "rollback cancelled" : esp_err_to_name(err));
        _pendingVerify = false;
    }
    PromptPackManager::getInstance().onFirmwareConfirmed(); // the previous prompt pack can go
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
    }
    if (_textLock) xSemaphoreGive(_textLock);
    if (s == FAILED)
    {
        _failure = (failure == Failure::NONE) ? Failure::DOWNLOAD : failure;
        _failedAt = millis();
        _justFailed = true;
    }
    if (status)
    {
        _statusDirty = true;
    }
    _state = s; // last: the loop sees the new state only with its texts in place
}

void OtaManager::postStatus(const char *status, int progress, const String &error, uint32_t versionCode)
{
    JsonDocument doc;
    if (_job.releaseId)
    {
        doc["release_id"] = _job.releaseId; // optional server-side; 0 would fail validation
    }
    doc["status"] = status;
    doc["progress"] = progress;
    if (error.length() > 0)
    {
        doc["error"] = error;
    }
    if (versionCode)
    {
        doc["version_code"] = versionCode;
    }
    // Posted by the request worker; the loop task is not held up by a slow or dead portal.
    const String statusCopy = status;
    const int progressCopy = progress;
    bool queued = RequestManager::getInstance().postAsync("/hubs/" + deviceId() + "/firmware/status", doc,
        [statusCopy, progressCopy](JsonDocument &, int code) {
            ESP_LOGI(TAG, "status '%s' %d%% -> HTTP %d", statusCopy.c_str(), progressCopy, code);
        });
    if (!queued)
    {
        ESP_LOGW(TAG, "status '%s' %d%% not sent (no network)", status, progress);
    }
}

bool OtaManager::checkNow(uint32_t releaseId, const char *reason)
{
    if (inProgress())
    {
        ESP_LOGI(TAG, "check skipped, update in progress");
        return false;
    }
    if (_state == CHECKING)
    {
        ESP_LOGI(TAG, "check already running");
        return false;
    }
    if (!WiFi.isConnected())
    {
        ESP_LOGI(TAG, "check skipped, no network");
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
    ESP_LOGI(TAG, "checking (%s)", reason);
    _state = CHECKING;
    _checkStartedAt = millis();
    _nextCheckAt = millis() + CHECK_INTERVAL_MS;
    // The answer arrives in onCheckResult() on the loop task; the request runs on the worker.
    bool started = RequestManager::getInstance().getAsync(endpoint, [this](JsonDocument &doc, int code) {
        onCheckResult(doc, code);
    });
    if (!started)
    {
        ESP_LOGI(TAG, "check not started (network not ready)");
        _nextCheckAt = millis() + CHECK_RETRY_MS;
        _state = IDLE;
    }
    return started;
}

void OtaManager::onCheckResult(JsonDocument &doc, int code)
{
    if (_state != CHECKING)
    {
        return; // something else (a manual job) took over while the check was in flight
    }
    if (code == 204)
    {
        ESP_LOGI(TAG, "up to date");
        _state = IDLE;
        return;
    }
    if (code != 200)
    {
        ESP_LOGW(TAG, "check failed (HTTP %d)", code);
        {
            char d[16];
            snprintf(d, sizeof(d), "http=%d", code);
            Faults::raise(Fault::OTA_CHECK_FAILED, d, false);
        }
        _nextCheckAt = millis() + (code <= 0 ? CHECK_RETRY_SOON_MS : CHECK_RETRY_MS);
        _state = IDLE;
        return;
    }
    // With prompts=1 the portal always answers 200: release_id null = nothing to install, and
    // "prompts" names the voice pack paired with the offered release, or with the running one.
    PromptPackManager &ppm = PromptPackManager::getInstance();
    PromptPackManager::Pack pack;
    const bool hasPack = PromptPackManager::parsePack(doc["prompts"], pack);
    if (!doc["release_id"].is<uint32_t>())
    {
        ESP_LOGI(TAG, "up to date");
        _state = IDLE;
        if (hasPack)
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
        return;
    }

    Job job;
    job.releaseId = doc["release_id"].as<uint32_t>();
    job.version = doc["version"].as<String>();
    job.versionCode = doc["version_code"].as<uint32_t>();
    job.url = doc["url"].as<String>();
    job.sha256 = doc["sha256"].as<String>();
    job.size = doc["file_size"].as<size_t>();
    job.mandatory = doc["mandatory"].as<bool>();
    job.hasPrompts = hasPack;
    job.prompts = pack;
    ESP_LOGI(TAG, "release %u offered: %s (code %u, %u bytes%s%s)", (unsigned)job.releaseId, job.version.c_str(),
                  (unsigned)job.versionCode, (unsigned)job.size, job.mandatory ? ", mandatory" : "",
                  hasPack ? (pack.checksum == ppm.installedChecksum() ? ", prompts up to date" : ", new prompt pack") : "");
    if (job.url.isEmpty() || job.sha256.length() != 64 || job.size == 0)
    {
        ESP_LOGI(TAG, "offer incomplete, ignored");
        _state = IDLE;
        return;
    }
    _job = job;
    _state = WAITING;
    _waitingSince = millis();
}

// Prompts first: the pack paired with the release is staged into /sounds.staging while the
// old prompts stay in use; the new firmware activates it at its first boot. If the pack cannot
// be fetched twice in a row the image is installed anyway (that firmware then syncs its pack
// itself at its first check), so a bad pack never blocks a firmware update for good.
void OtaManager::beginJob()
{
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
    startJob(_job);
}

void OtaManager::onPromptsStaged(bool ok)
{
    if (_state != PROMPTS)
    {
        return;
    }
    if (ok)
    {
        _promptSyncFailures = 0;
        ESP_LOGI(TAG, "prompt pack staged - downloading the image");
        startJob(_job);
        return;
    }
    _promptSyncFailures++;
    setState(FAILED, 0, "voice prompt pack could not be downloaded", Failure::PROMPTS);
}

bool OtaManager::startJob(const Job &job)
{
    if (inProgress())
    {
        return false;
    }
    _job = job;
    clearError();
    setState(DOWNLOADING, 0);
    if (xTaskCreatePinnedToCore(taskEntry, "ota", 8192, this, 5, &_task, 0) != pdPASS)
    {
        _task = nullptr;
        setState(FAILED, 0, "could not start the update task", Failure::DOWNLOAD);
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
    ESP_LOGI(TAG, "downloading %s", _job.url.c_str());
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
    Failure failureKind = Failure::NONE;   // classified where it happens; FLASH ends the retries
    const int maxAttempts = 6;

    // The image is fetched in one or more HTTP requests: a dropped connection resumes with a
    // Range request (the backend answers 206), so a flaky home network does not restart a
    // 2 MB download from zero. Flash writes and the hash simply continue.
    for (int attempt = 1; attempt <= maxAttempts && (total == 0 || received < total); ++attempt)
    {
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
            if (code == 410 || code == 404 || code == 403)
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
        if (failureKind == Failure::FLASH)
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
        setState(FAILED, _progress, failure.length() ? failure.c_str() : "incomplete download",
                 failureKind == Failure::FLASH ? Failure::FLASH : Failure::DOWNLOAD);
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
        setState(FAILED, _progress, "sha256 mismatch", Failure::SHA);
        return;
    }

    setState(APPLYING, 100);
    if (!Update.end(true))
    {
        setState(FAILED, 100, (String("activate: ") + Update.errorString()).c_str(), Failure::FLASH);
        return;
    }
    // Remember what we expect to boot into so the result can be reported after the restart.
    ConfigManager &cfg = ConfigManager::getInstance();
    cfg.storeInt(NVS_OTA_RELEASE, (int)_job.releaseId);
    cfg.storeInt(NVS_OTA_VCODE, (int)_job.versionCode);
    ESP_LOGI(TAG, "image %s verified and activated, rebooting", _job.version.c_str());
    _state = REBOOTING;
}

void OtaManager::update()
{
    const unsigned long now = millis();

    // 1. A new image confirms itself after CONFIRM_AFTER_MS of uptime (any reset before that
    //    rolls back); the crash-loop counter resets once the firmware proved stable.
    if (_pendingVerify && !_bootHealthyMarked && now - _bootAt > CONFIRM_AFTER_MS)
    {
        confirmImage();
    }
    if (g_otaCrashCount && now - _bootAt > STABLE_AFTER_MS)
    {
        g_otaCrashCount = 0;
    }

    // 2. Report how the previous update ended - for a pending image only once it is confirmed,
    //    so a crash before that leaves the NVS record in place for the rolled-back firmware
    //    to report.
    if (!_bootReported && WiFi.isConnected() && now - _bootAt > 8000 && (!_pendingVerify || _bootHealthyMarked))
    {
        _bootReported = true;
        ConfigManager &cfg = ConfigManager::getInstance();
        uint32_t rel = (uint32_t)cfg.getInt(NVS_OTA_RELEASE, 0);
        bool reverted = cfg.getInt(NVS_OTA_REVERT, 0) != 0;
        if (rel || reverted)
        {
            uint32_t want = (uint32_t)cfg.getInt(NVS_OTA_VCODE, 0);
            bool ok = !reverted && (want != 0 && want == firmwareVersionCode());
            if (!rel && reverted)
            {
                rel = (uint32_t)cfg.getInt(NVS_OTA_LAST_REL, 0); // the image we just abandoned
            }
            _job.releaseId = rel;
            if (ok)
            {
                cfg.storeInt(NVS_OTA_LAST_REL, (int)rel);
                _justUpdated = true;
            }
            if (!ok)
            {
                Faults::raise(Fault::OTA_ROLLED_BACK, reverted ? "crash loop" : "image did not come up");
            }
            postStatus(ok ? "success" : "rolled_back", 100,
                       ok ? String()
                          : (reverted ? String("crash loop on the new image; reverted to the previous firmware")
                                      : String("image did not come up; bootloader returned to the previous firmware")),
                       firmwareVersionCode());
            cfg.deleteSetting(NVS_OTA_RELEASE);
            cfg.deleteSetting(NVS_OTA_VCODE);
            cfg.deleteSetting(NVS_OTA_REVERT);
        }
    }

    // 3. Status changes made by the download task
    if (_statusDirty.exchange(false))
    {
        char status[sizeof(_pendingStatus)];
        char error[ERROR_LEN];
        int progress;
        if (_textLock) xSemaphoreTake(_textLock, portMAX_DELAY);
        memcpy(status, _pendingStatus, sizeof(status));
        memcpy(error, _pendingError, sizeof(error));
        progress = _pendingProgress;
        if (_textLock) xSemaphoreGive(_textLock);
        status[sizeof(status) - 1] = 0;
        error[sizeof(error) - 1] = 0;
        if (WiFi.isConnected())
        {
            postStatus(status, progress, String(error));
        }
    }

    // 3b. A check whose answer never came (worker stuck, request dropped): back to IDLE so the
    //     periodic checks and the portal's push can start a new one. A late answer is ignored.
    if (_state == CHECKING && now - _checkStartedAt > CHECK_TIMEOUT_MS)
    {
        ESP_LOGW(TAG, "check timed out after %lu s", CHECK_TIMEOUT_MS / 1000);
        Faults::raise(Fault::OTA_CHECK_FAILED, "timeout", false);
        _nextCheckAt = now + CHECK_RETRY_SOON_MS;
        _state = IDLE;
    }

    // 4. Reboot once the final status went out (the worker gets up to 10 s to deliver it)
    if (_state == REBOOTING)
    {
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
        return;
    }

    // 5. Start a known update when the toy is idle (mandatory ones stop waiting after an hour)
    if (_state == WAITING)
    {
        bool idle = !_idleCheck || _idleCheck();
        bool forced = _job.mandatory && now - _waitingSince > MANDATORY_WAIT_MAX_MS;
        if (idle || forced)
        {
            beginJob();
        }
    }
    if (_state == PROMPTS)
    {
        _progress = PromptPackManager::getInstance().stagingProgress();
    }
    // A failed attempt goes back to the periodic checks after a while (the portal's push
    // "ota-update" also restarts it any time).
    if (_state == FAILED && now - _failedAt > FAILED_RETRY_MS)
    {
        _state = IDLE;
        _nextCheckAt = now;
    }

    // 6. Periodic check
    if (_state == IDLE && WiFi.isConnected() && _networkHealthy && now >= _nextCheckAt)
    {
        checkNow(0, "periodic");
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
    if (_job.releaseId)
    {
        ESP_LOGI(TAG, "job release %u %s (code %u) %u bytes %s", (unsigned)_job.releaseId, _job.version.c_str(),
                      (unsigned)_job.versionCode, (unsigned)_job.size, _job.url.c_str());
    }
    ESP_LOGI(TAG, "next periodic check in %ld s, crash resets in a row %u, network %s",
                  (long)((long)_nextCheckAt - (long)millis()) / 1000, (unsigned)g_otaCrashCount, _networkHealthy ? "ok" : "not yet");
}

bool OtaManager::consumeJustUpdated()
{
    const bool v = _justUpdated;
    _justUpdated = false;
    return v;
}

bool OtaManager::consumeJustFailed()
{
    return _justFailed.exchange(false);
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
