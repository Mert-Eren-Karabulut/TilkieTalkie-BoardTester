#include "PromptPackManager.h"
#include "FileManager.h"
#include "VoicePrompts.h"
#include "AudioController.h"
#include "PsramJson.h"
#include "Version.h"
#include "Faults.h"
#include <SD_MMC.h>
#include <esp_log.h>

const char *PromptPackManager::SOUNDS_DIR = "/sounds";
const char *PromptPackManager::STAGING_DIR = "/sounds.staging";

namespace
{
const char *TAG = "pack";
const unsigned long STAGING_WATCHDOG_MS = 45UL * 60UL * 1000UL; // a slow link gets this long for ~1 MB
const char *OLD_DIR = "/sounds.old";
}

PromptPackManager &PromptPackManager::getInstance()
{
    static PromptPackManager inst;
    return inst;
}

void PromptPackManager::parseFields(JsonVariantConst v, Pack &out)
{
    out = Pack();
    out.id = v["pack_id"] | 0U;
    out.name = v["name"].as<String>();
    out.checksum = v["checksum"].as<String>();
    out.checksum.toLowerCase();
    out.baseUrl = v["base_url"].as<String>();   // absent in pack.json
    for (JsonVariantConst f : v["files"].as<JsonArrayConst>())
    {
        PackFile pf;
        pf.path = f["p"].as<String>();
        pf.sha256 = f["h"].as<String>();
        pf.size = f["s"] | 0U;
        if (pf.path.isEmpty() || pf.path.startsWith("/") || pf.path.indexOf("..") >= 0)
        {
            continue;
        }
        out.files.push_back(pf);
    }
}

bool PromptPackManager::parsePack(JsonVariantConst v, Pack &out)
{
    out = Pack();
    if (v.isNull() || !v.is<JsonObjectConst>())
    {
        return false;
    }
    parseFields(v, out);
    return out.valid();
}

bool PromptPackManager::readPackJson(const String &dir, Pack &pack, String &forVersion)
{
    File f = SD_MMC.open(dir + "/pack.json", FILE_READ);
    if (!f)
    {
        return false;
    }
    JsonDocument doc(&psramJsonAllocator);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err)
    {
        ESP_LOGW(TAG, "%s/pack.json unreadable (%s)", dir.c_str(), err.c_str());
        return false;
    }
    parseFields(doc.as<JsonVariantConst>(), pack);
    forVersion = doc["for_firmware"].as<String>();
    return pack.checksum.length() == 64;
}

bool PromptPackManager::filesComplete(const String &dir, const Pack &pack, int *missing)
{
    int miss = 0;
    for (const PackFile &pf : pack.files)
    {
        File f = SD_MMC.open(dir + "/" + pf.path, FILE_READ);
        const bool ok = f && !f.isDirectory() && (pf.size == 0 || f.size() == pf.size);
        if (f) f.close();
        if (!ok) miss++;
    }
    if (missing) *missing = miss;
    return miss == 0;
}

void PromptPackManager::begin()
{
    FileManager &fm = FileManager::getInstance();
    Pack installed;
    String installedFor;
    bool haveInstalled = readPackJson(SOUNDS_DIR, installed, installedFor);
    if (fm.fileExists(OLD_DIR))
    {
        if (haveInstalled && !installedFor.isEmpty() && installedFor != firmwareVersion())
        {
            // The active pack belongs to a firmware that did not survive its first boot (the
            // bootloader rolled back to us): put our own pack back, keep theirs staged.
            ESP_LOGW(TAG, "active pack is for firmware %s - restoring the previous pack after the rollback",
                     installedFor.c_str());
            if (fm.fileExists(STAGING_DIR))
            {
                deleteTree(STAGING_DIR);
            }
            if (SD_MMC.rename(SOUNDS_DIR, STAGING_DIR) && SD_MMC.rename(OLD_DIR, SOUNDS_DIR))
            {
                haveInstalled = readPackJson(SOUNDS_DIR, installed, installedFor);
            }
            else
            {
                ESP_LOGW(TAG, "restore failed, keeping what is there");
                Faults::raise(Fault::PROMPT_PACK_ACTIVATE_FAILED, "restore");
            }
        }
        else if (haveInstalled)
        {
            deleteTree(OLD_DIR); // an activation that was interrupted before the cleanup
        }
        else if (!fm.fileExists(SOUNDS_DIR))
        {
            // Power lost between "move /sounds aside" and "move the staged pack in": the
            // previous pack is the only one there is. A staged pack for this firmware is
            // activated below (it keeps /sounds.old until it is in place).
            Pack staged;
            String stagedFor;
            if (!(readPackJson(STAGING_DIR, staged, stagedFor) && stagedFor == firmwareVersion()))
            {
                ESP_LOGW(TAG, "/sounds missing - restoring the previous pack from %s", OLD_DIR);
                if (SD_MMC.rename(OLD_DIR, SOUNDS_DIR))
                {
                    haveInstalled = readPackJson(SOUNDS_DIR, installed, installedFor);
                }
                else
                {
                    Faults::raise(Fault::PROMPT_PACK_ACTIVATE_FAILED, "restore");
                }
            }
        }
        else
        {
            // /sounds exists but carries no pack: /sounds.old stays until a pack is active.
            ESP_LOGI(TAG, "/sounds has no pack - keeping %s", OLD_DIR);
        }
    }
    _installed = haveInstalled ? installed.checksum : String();
    Pack staged;
    String forVersion;
    if (readPackJson(STAGING_DIR, staged, forVersion))
    {
        if (forVersion == firmwareVersion())
        {
            ESP_LOGI(TAG, "pack %.12s staged for this firmware - activating", staged.checksum.c_str());
            activateStaging();
        }
        else
        {
            ESP_LOGI(TAG, "pack %.12s staged for firmware %s, kept for that update",
                     staged.checksum.c_str(), forVersion.c_str());
        }
    }
    if (!fm.fileExists(SOUNDS_DIR) && fm.fileExists(OLD_DIR))
    {
        // The staged pack could not be swapped in after an interrupted activation: never boot
        // without prompts while the previous pack is on the card.
        ESP_LOGW(TAG, "no /sounds after activation - restoring the previous pack");
        if (SD_MMC.rename(OLD_DIR, SOUNDS_DIR))
        {
            String restoredFor;
            _installed = readPackJson(SOUNDS_DIR, installed, restoredFor) ? installed.checksum : String();
        }
    }
    int missing = 0;
    for (const String &p : VoicePrompts::requiredPaths())
    {
        if (!FileManager::getInstance().fileExists(String(SOUNDS_DIR) + "/" + p)) missing++;
    }
    ESP_LOGI(TAG, "card pack %s, %d required file(s) missing",
             _installed.isEmpty() ? "none" : _installed.substring(0, 12).c_str(), missing);
    if (missing > 0)
    {
        char d[24];
        snprintf(d, sizeof(d), "missing=%d", missing);
        Faults::raise(Fault::PROMPT_FILE_MISSING, d);
    }
}

void PromptPackManager::update()
{
    if (_activateWhenSilent && !VoicePrompts::getInstance().isBusy() && !AudioController::getInstance().isPromptPlaying())
    {
        _activateWhenSilent = false;
        activateStaging();
    }
    if (_staging && millis() - _stagingSince > STAGING_WATCHDOG_MS)
    {
        ESP_LOGW(TAG, "pack download took too long - giving up for now");
        finishStaging(false);
    }
}

const char *PromptPackManager::stateName() const
{
    if (_staging) return "syncing";
    if (_installed.isEmpty()) return "missing";
    if (_required.isEmpty()) return "unknown";
    return _required == _installed ? "ok" : "outdated";
}

int PromptPackManager::stagingProgress() const
{
    if (!_staging || _filesTotal <= 0) return _staging ? 0 : 100;
    return (_filesDone * 100) / _filesTotal;
}

bool PromptPackManager::stage(const Pack &pack, const String &forVersion, std::function<void(bool)> cb)
{
    if (!pack.valid())
    {
        return false;
    }
    if (pack.checksum == _installed)
    {
        if (cb) cb(true);
        return true;
    }
    if (_staging)
    {
        if (_pack.checksum == pack.checksum)
        {
            _forVersion = forVersion; // same pack, maybe a different target: the manifest is written at the end
            if (_cb && cb)
            {
                auto previous = _cb;
                _cb = [previous, cb](bool ok) { previous(ok); cb(ok); }; // both waiters hear the result
            }
            else if (cb)
            {
                _cb = cb;
            }
            return true;
        }
        ESP_LOGW(TAG, "a different pack was being staged - replaced");
        cancelQueuedStaging();
        _staging = false;
        if (_cb)
        {
            auto previous = _cb;
            _cb = nullptr;
            previous(false);
        }
    }
    FileManager &fm = FileManager::getInstance();
    Pack staged;
    String stagedFor;
    const bool reuse = readPackJson(STAGING_DIR, staged, stagedFor) && staged.checksum == pack.checksum;
    if (!reuse && fm.fileExists(STAGING_DIR))
    {
        deleteTree(STAGING_DIR);
    }
    fm.createDirectory(STAGING_DIR);
    _pack = pack;
    _forVersion = forVersion;
    _cb = cb;
    _filesTotal = 0;
    _filesDone = 0;
    for (const PackFile &pf : pack.files)
    {
        const String local = String(STAGING_DIR) + "/" + pf.path;
        File f = SD_MMC.open(local, FILE_READ);
        const bool present = f && !f.isDirectory() && (pf.size == 0 || f.size() == pf.size);
        if (f) f.close();
        if (present)
        {
            continue;
        }
        fm.scheduleDownload(pack.baseUrl + pf.path, local, pf.sha256);
        _filesTotal++;
    }
    ESP_LOGI(TAG, "staging pack %.12s (%s) for firmware %s: %d file(s) to download",
             pack.checksum.c_str(), pack.name.c_str(), forVersion.c_str(), _filesTotal);
    if (_filesTotal == 0)
    {
        finishStaging(true);
        return true;
    }
    _staging = true;
    _stagingSince = millis();
    return true;
}

void PromptPackManager::onFileDone(const String &path, bool success)
{
    if (!_staging || !ownsStagingPath(path))
    {
        return; // not ours, or a late answer for a pack that was replaced
    }
    if (!success)
    {
        ESP_LOGW(TAG, "%s could not be downloaded", path.c_str());
        finishStaging(false);
        return;
    }
    _filesDone++;
    if (_filesDone >= _filesTotal)
    {
        finishStaging(true);
    }
}

bool PromptPackManager::ownsStagingPath(const String &path) const
{
    if (!path.startsWith(STAGING_DIR))
    {
        return false;
    }
    for (const PackFile &pf : _pack.files)
    {
        if (path == String(STAGING_DIR) + "/" + pf.path)
        {
            return true;
        }
    }
    return false;
}

// The pack's downloads still waiting in the queue are dropped (the one in flight finishes and
// is ignored by onFileDone): a failed or replaced staging stops costing bandwidth.
void PromptPackManager::cancelQueuedStaging()
{
    FileManager &fm = FileManager::getInstance();
    int cancelled = 0;
    for (const PackFile &pf : _pack.files)
    {
        if (fm.cancelQueuedDownload(String(STAGING_DIR) + "/" + pf.path))
        {
            cancelled++;
        }
    }
    if (cancelled)
    {
        ESP_LOGI(TAG, "%d queued pack download(s) cancelled", cancelled);
    }
}

void PromptPackManager::finishStaging(bool ok)
{
    _staging = false;
    if (ok)
    {
        int missing = 0;
        ok = filesComplete(STAGING_DIR, _pack, &missing) && writeStagingManifest();
        if (!ok)
        {
            ESP_LOGW(TAG, "staging incomplete (%d file(s) missing or manifest not written)", missing);
        }
    }
    ESP_LOGI(TAG, "pack %.12s staging %s", _pack.checksum.c_str(), ok ? "complete" : "failed");
    if (!ok)
    {
        cancelQueuedStaging();
        Faults::raise(Fault::PROMPT_PACK_FAILED, _pack.checksum.substring(0, 12).c_str());
    }
    if (ok && _forVersion == firmwareVersion())
    {
        _activateWhenSilent = true; // for the running firmware: swap in as soon as nothing speaks
    }
    if (_cb)
    {
        auto cb = _cb;
        _cb = nullptr;
        cb(ok);
    }
}

bool PromptPackManager::writeStagingManifest()
{
    JsonDocument doc(&psramJsonAllocator);
    doc["checksum"] = _pack.checksum;
    doc["pack_id"] = _pack.id;
    doc["name"] = _pack.name;
    doc["for_firmware"] = _forVersion;
    JsonArray files = doc["files"].to<JsonArray>();
    for (const PackFile &pf : _pack.files)
    {
        JsonObject o = files.add<JsonObject>();
        o["p"] = pf.path;
        o["h"] = pf.sha256;
        o["s"] = pf.size;
    }
    String json;
    serializeJson(doc, json);
    const String path = String(STAGING_DIR) + "/pack.json";
    return FileManager::getInstance().writeFileNow(path.c_str(), json.c_str(), json.length());
}

bool PromptPackManager::activateStaging()
{
    Pack staged;
    String forVersion;
    if (!readPackJson(STAGING_DIR, staged, forVersion))
    {
        ESP_LOGW(TAG, "staged pack has no readable pack.json, not activated");
        Faults::raise(Fault::PROMPT_PACK_ACTIVATE_FAILED, "manifest");
        return false;
    }
    if (forVersion != firmwareVersion())
    {
        ESP_LOGI(TAG, "staged pack is for firmware %s, not activated", forVersion.c_str());
        return false;
    }
    int missing = 0;
    if (!filesComplete(STAGING_DIR, staged, &missing))
    {
        ESP_LOGW(TAG, "staged pack has %d file(s) missing, not activated", missing);
        char d[24];
        snprintf(d, sizeof(d), "missing=%d", missing);
        Faults::raise(Fault::PROMPT_PACK_ACTIVATE_FAILED, d);
        return false;
    }
    FileManager &fm = FileManager::getInstance();
    Pack current;
    String currentFor;
    const bool currentHasPack = readPackJson(SOUNDS_DIR, current, currentFor);
    bool hadSounds = fm.fileExists(SOUNDS_DIR);
    if (fm.fileExists(OLD_DIR))
    {
        if (currentHasPack)
        {
            deleteTree(OLD_DIR); // superseded: the current pack becomes the fallback
        }
        else if (hadSounds)
        {
            // /sounds without a pack next to a real previous pack: the previous pack stays
            // the fallback, the packless folder makes room for the new pack.
            ESP_LOGI(TAG, "/sounds has no pack - keeping %s as the fallback", OLD_DIR);
            deleteTree(SOUNDS_DIR);
            hadSounds = false;
        }
    }
    if (hadSounds && !SD_MMC.rename(SOUNDS_DIR, OLD_DIR))
    {
        ESP_LOGW(TAG, "could not move the current /sounds aside - pack not activated");
        Faults::raise(Fault::PROMPT_PACK_ACTIVATE_FAILED, "move-aside");
        return false;
    }
    if (!SD_MMC.rename(STAGING_DIR, SOUNDS_DIR))
    {
        ESP_LOGW(TAG, "could not move the staged pack into place - restoring the old one");
        Faults::raise(Fault::PROMPT_PACK_ACTIVATE_FAILED, "rename");
        if (fm.fileExists(OLD_DIR) && !fm.fileExists(SOUNDS_DIR))
        {
            SD_MMC.rename(OLD_DIR, SOUNDS_DIR);
        }
        return false;
    }
    // A pack is active now: the fallback can go, unless this firmware may still be rolled back.
    if (fm.fileExists(OLD_DIR))
    {
        if (!_pendingVerify)
        {
            deleteTree(OLD_DIR);
        }
        else
        {
            ESP_LOGI(TAG, "previous pack kept as /sounds.old until this firmware is confirmed");
        }
    }
    _installed = staged.checksum;
    ESP_LOGI(TAG, "pack %.12s (%s) is now active", staged.checksum.c_str(), staged.name.c_str());
    return true;
}

void PromptPackManager::onFirmwareConfirmed()
{
    _pendingVerify = false;
    if (_installed.isEmpty())
    {
        return; // no pack in /sounds: /sounds.old (if any) is still the only real pack
    }
    if (FileManager::getInstance().fileExists(OLD_DIR))
    {
        deleteTree(OLD_DIR);
        ESP_LOGI(TAG, "firmware confirmed, previous pack removed");
    }
}

void PromptPackManager::discardStaleStaging()
{
    if (_staging)
    {
        return;
    }
    Pack staged;
    String forVersion;
    if (!readPackJson(STAGING_DIR, staged, forVersion))
    {
        if (FileManager::getInstance().fileExists(STAGING_DIR))
        {
            deleteTree(STAGING_DIR); // leftovers without a manifest
        }
        return;
    }
    if (forVersion == firmwareVersion())
    {
        _activateWhenSilent = true; // a live sync interrupted by a reboot
        return;
    }
    ESP_LOGI(TAG, "dropping the pack staged for firmware %s (no update on the way)", forVersion.c_str());
    deleteTree(STAGING_DIR);
}

void PromptPackManager::deleteTree(const String &dir)
{
    File d = SD_MMC.open(dir);
    if (!d)
    {
        return;
    }
    if (!d.isDirectory())
    {
        d.close();
        SD_MMC.remove(dir);
        return;
    }
    std::vector<String> names;
    std::vector<bool> isDir;
    for (File e = d.openNextFile(); e; e = d.openNextFile())
    {
        names.push_back(String(e.name()));
        isDir.push_back(e.isDirectory());
        e.close();
    }
    d.close();
    for (size_t i = 0; i < names.size(); ++i)
    {
        const String child = dir + "/" + names[i];
        if (isDir[i])
        {
            deleteTree(child);
        }
        else
        {
            SD_MMC.remove(child);
        }
    }
    SD_MMC.rmdir(dir);
}
