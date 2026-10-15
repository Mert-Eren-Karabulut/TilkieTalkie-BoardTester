#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include <vector>

// Voice prompt packs: the files under /sounds, identified by a pack checksum (sha256 of the
// sorted "path:sha256" lines, see docs/VOICE-PROMPTS.md "Remote update"). The portal pairs a
// pack with every firmware release and names it in the firmware check. A pack is downloaded
// into /sounds.staging and swapped in only
//   (a) by the firmware it was staged for, at that firmware's first boot (an update), or
//   (b) right away, once no prompt is playing, when it is for the running firmware (a pack
//       changed for the current release, or a card without the pack).
// A failed update therefore never leaves new prompts behind with the old firmware, and a
// rollback finds the old prompts untouched.
class PromptPackManager
{
public:
    struct PackFile
    {
        String path;    // relative to /sounds, e.g. "tr/setup_welcome.mp3"
        String sha256;
        uint32_t size = 0;
    };
    struct Pack
    {
        uint32_t id = 0;
        String name;
        String checksum;  // 64 hex: the pack identity
        String baseUrl;   // a file is at baseUrl + path
        std::vector<PackFile> files;
        bool valid() const { return checksum.length() == 64 && !files.empty() && baseUrl.length() > 0; }
    };

    static PromptPackManager &getInstance();

    // Before begin(): this boot runs a freshly installed image that may still be rolled back.
    // The previous pack is then kept as /sounds.old until onFirmwareConfirmed(), and a boot of
    // the rolled-back firmware restores it (begin()).
    void setPendingVerify(bool pending) { _pendingVerify = pending; }
    void onFirmwareConfirmed();
    void begin();   // after the card is mounted, before the first prompt is spoken
    void update();  // loop task: live activation when silent, staging watchdog

    static bool parsePack(JsonVariantConst v, Pack &out);   // the "prompts" object of the firmware check
    const String &installedChecksum() const { return _installed; }
    void noteRequired(const String &checksum) { _required = checksum; } // what the portal pairs with this firmware
    const char *stateName() const;   // ok | outdated | syncing | missing | unknown (device report)

    // Download `pack` into /sounds.staging for firmware `forVersion`; files already there are
    // kept. cb(true) once the folder is complete and its pack.json written, cb(false) when a
    // file could not be downloaded (the queue retries first) or after the watchdog.
    bool stage(const Pack &pack, const String &forVersion, std::function<void(bool)> cb);
    bool isStaging() const { return _staging; }
    int stagingProgress() const;   // files done, in percent

    void onFileDone(const String &path, bool success);   // loop task, from the download callback
    // No update on the way: a staging folder for another firmware is dropped; one for the
    // running firmware (a live sync cut short by a reboot) is activated.
    void discardStaleStaging();

    static const char *SOUNDS_DIR;
    static const char *STAGING_DIR;

private:
    PromptPackManager() = default;
    bool readPackJson(const String &dir, Pack &pack, String &forVersion);
    bool filesComplete(const String &dir, const Pack &pack, int *missing = nullptr);
    bool writeStagingManifest();
    bool activateStaging();
    void deleteTree(const String &dir);
    void finishStaging(bool ok);

    String _installed;   // checksum of /sounds/pack.json ("" when the card has none)
    String _required;    // checksum the portal pairs with the running firmware ("" unknown)
    Pack _pack;
    String _forVersion;
    std::function<void(bool)> _cb;
    bool _staging = false;
    int _filesTotal = 0;
    int _filesDone = 0;
    unsigned long _stagingSince = 0;
    bool _activateWhenSilent = false;
    bool _pendingVerify = false;
};
