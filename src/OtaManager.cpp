#include "OtaManager.h"
#include "Version.h"
#include "RequestManager.h"
#include "ConfigManager.h"
#include "CaBundle.h"
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

namespace
{
const char *NVS_OTA_RELEASE = "ota_rel";   // release we rebooted into (set right before the reboot)
const char *NVS_OTA_VCODE = "ota_vcode";   // version_code that image should report
const unsigned long CHECK_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;
const unsigned long CHECK_RETRY_MS = 60UL * 60UL * 1000UL;
const unsigned long CHECK_RETRY_SOON_MS = 2UL * 60UL * 1000UL; // connection-level failure
const unsigned long FIRST_CHECK_DELAY_MS = 90UL * 1000UL;
const unsigned long MANDATORY_WAIT_MAX_MS = 60UL * 60UL * 1000UL;
const unsigned long CONFIRM_AFTER_MS = 60UL * 1000UL;   // a crash before this rolls back (bootloader)
const unsigned long STABLE_AFTER_MS = 5UL * 60UL * 1000UL; // crash counter resets after this uptime
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
    Serial.printf("OTA: firmware %s (code %u, %s) on %s%s\n", firmwareVersion(), (unsigned)firmwareVersionCode(),
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
        Serial.printf("OTA: crash reset #%u (reason %d)\n", (unsigned)g_otaCrashCount, (int)reason);
    }
    if (g_otaCrashCount >= 3 && !_pendingVerify)
    {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_app_desc_t desc;
        if (other && esp_ota_get_partition_description(other, &desc) == ESP_OK)
        {
            Serial.printf("OTA: crash loop - reverting to %s (%s)\n", other->label, desc.version);
            ConfigManager::getInstance().storeInt(NVS_OTA_REVERT, 1);
            g_otaCrashCount = 0;
            if (esp_ota_set_boot_partition(other) == ESP_OK)
            {
                delay(200);
                esp_restart();
            }
            Serial.println("OTA: revert failed, the other partition does not hold a valid image");
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
        Serial.printf("OTA: new image confirmed after %lu s (%s)\n", millis() / 1000,
                      err == ESP_OK ? "rollback cancelled" : esp_err_to_name(err));
        _pendingVerify = false;
    }
}

void OtaManager::setState(State s, int progress, const String &error)
{
    _state = s;
    if (progress >= 0)
    {
        _progress = progress;
    }
    if (error.length() > 0)
    {
        _error = error;
    }
    // Status for the backend is posted by the loop task (RequestManager is not thread safe).
    const char *status = nullptr;
    switch (s)
    {
    case DOWNLOADING: status = "downloading"; break;
    case APPLYING: status = "applying"; break;
    case FAILED: status = "failed"; _justFailed = true; break;
    default: break;
    }
    if (status)
    {
        _pendingStatus = status;
        _pendingProgress = _progress;
        _pendingError = error;
        _statusDirty = true;
    }
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
            Serial.printf("OTA: status '%s' %d%% -> HTTP %d\n", statusCopy.c_str(), progressCopy, code);
        });
    if (!queued)
    {
        Serial.printf("OTA: status '%s' %d%% not sent (no network)\n", status, progress);
    }
}

bool OtaManager::checkNow(uint32_t releaseId, const char *reason)
{
    if (inProgress())
    {
        Serial.println("OTA: check skipped, update in progress");
        return false;
    }
    if (_state == CHECKING)
    {
        Serial.println("OTA: check already running");
        return false;
    }
    if (!WiFi.isConnected())
    {
        Serial.println("OTA: check skipped, no network");
        return false;
    }
    String endpoint = "/hubs/" + deviceId() + "/firmware/check?version_code=" + String(firmwareVersionCode()) +
                      "&version=" + String(firmwareVersion()) + "&hardware_rev=" + TT_HARDWARE_REV;
    if (releaseId)
    {
        endpoint += "&release_id=" + String(releaseId);
    }
    Serial.printf("OTA: checking (%s)\n", reason);
    _state = CHECKING;
    _nextCheckAt = millis() + CHECK_INTERVAL_MS;
    // The answer arrives in onCheckResult() on the loop task; the request runs on the worker.
    bool started = RequestManager::getInstance().getAsync(endpoint, [this](JsonDocument &doc, int code) {
        onCheckResult(doc, code);
    });
    if (!started)
    {
        Serial.println("OTA: check not started (network not ready)");
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
        Serial.println("OTA: up to date");
        _state = IDLE;
        return;
    }
    if (code != 200 || !doc["release_id"].is<uint32_t>())
    {
        Serial.printf("OTA: check failed (HTTP %d)\n", code);
        _nextCheckAt = millis() + (code <= 0 ? CHECK_RETRY_SOON_MS : CHECK_RETRY_MS);
        _state = IDLE;
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
    Serial.printf("OTA: release %u offered: %s (code %u, %u bytes%s)\n", (unsigned)job.releaseId, job.version.c_str(),
                  (unsigned)job.versionCode, (unsigned)job.size, job.mandatory ? ", mandatory" : "");
    if (job.url.isEmpty() || job.sha256.length() != 64 || job.size == 0)
    {
        Serial.println("OTA: offer incomplete, ignored");
        _state = IDLE;
        return;
    }
    _job = job;
    _state = WAITING;
    _waitingSince = millis();
}

bool OtaManager::startJob(const Job &job)
{
    if (inProgress())
    {
        return false;
    }
    _job = job;
    _error = "";
    setState(DOWNLOADING, 0);
    if (xTaskCreatePinnedToCore(taskEntry, "ota", 8192, this, 5, &_task, 0) != pdPASS)
    {
        _task = nullptr;
        setState(FAILED, 0, "could not start the update task");
        return false;
    }
    return true;
}

void OtaManager::taskEntry(void *arg)
{
    static_cast<OtaManager *>(arg)->runDownload();
    static_cast<OtaManager *>(arg)->_task = nullptr;
    vTaskDelete(NULL);
}

void OtaManager::runDownload()
{
    Serial.printf("OTA: downloading %s\n", _job.url.c_str());
    const bool https = _job.url.startsWith("https://");
    WiFiClient plain;
    WiFiClientSecure secure;
    if (https)
    {
        secure.setCACertBundle(caBundleStart(), caBundleSize());
    }
    WiFiClient &client = https ? static_cast<WiFiClient &>(secure) : plain;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf)
    {
        setState(FAILED, _progress, "no memory for the download buffer");
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
    const int maxAttempts = 6;

    // The image is fetched in one or more HTTP requests: a dropped connection resumes with a
    // Range request (the backend answers 206), so a flaky home network does not restart a
    // 2 MB download from zero. Flash writes and the hash simply continue.
    for (int attempt = 1; attempt <= maxAttempts && (total == 0 || received < total); ++attempt)
    {
        if (attempt > 1)
        {
            Serial.printf("OTA: resuming at %u bytes (attempt %d)\n", (unsigned)received, attempt);
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
        if (failure.startsWith("flash"))
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
        setState(FAILED, _progress, failure.length() ? failure : String("incomplete download"));
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
        setState(FAILED, _progress, "sha256 mismatch");
        return;
    }

    setState(APPLYING, 100);
    if (!Update.end(true))
    {
        setState(FAILED, 100, String("activate: ") + Update.errorString());
        return;
    }
    // Remember what we expect to boot into so the result can be reported after the restart.
    ConfigManager &cfg = ConfigManager::getInstance();
    cfg.storeInt(NVS_OTA_RELEASE, (int)_job.releaseId);
    cfg.storeInt(NVS_OTA_VCODE, (int)_job.versionCode);
    Serial.printf("OTA: image %s verified and activated, rebooting\n", _job.version.c_str());
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
    if (_statusDirty)
    {
        _statusDirty = false;
        if (WiFi.isConnected())
        {
            postStatus(_pendingStatus.c_str(), _pendingProgress, _pendingError);
        }
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
            startJob(_job);
        }
    }

    // 6. Periodic check
    if (_state == IDLE && WiFi.isConnected() && _networkHealthy && now >= _nextCheckAt)
    {
        checkNow(0, "periodic");
    }
}

void OtaManager::printStatus()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    Serial.printf("OTA: firmware %s code %u hw %s | partition %s (next %s)%s | state %s %d%%%s%s\n",
                  firmwareVersion(), (unsigned)firmwareVersionCode(), TT_HARDWARE_REV,
                  running ? running->label : "?", next ? next->label : "?",
                  _pendingVerify ? " pending-verify" : (_bootHealthyMarked ? " confirmed" : ""), stateName(_state), (int)_progress,
                  _error.length() ? " error: " : "", _error.c_str());
    if (_job.releaseId)
    {
        Serial.printf("OTA: job release %u %s (code %u) %u bytes %s\n", (unsigned)_job.releaseId, _job.version.c_str(),
                      (unsigned)_job.versionCode, (unsigned)_job.size, _job.url.c_str());
    }
    Serial.printf("OTA: next periodic check in %ld s, crash resets in a row %u, network %s\n",
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
    const bool v = _justFailed;
    _justFailed = false;
    return v;
}
