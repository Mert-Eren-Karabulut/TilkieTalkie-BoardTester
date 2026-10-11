#include "VoicePrompts.h"
#include "AudioController.h"
#include "FileManager.h"

namespace
{
const char *const kNames[(int)Prompt::COUNT] = {
    "setup_welcome", "setup_connecting", "setup_success", "setup_failed",
    "wifi_lost", "wifi_reset_hint", "wifi_reset_done",
    "idle_hello", "nfc_read_trouble", "figure_no_content", "figure_offline",
    "download_start", "download_done", "download_failed", "playlist_end",
    "battery_low", "battery_critical", "charge_full",
    "update_start", "update_done", "update_failed",
    "tone_charge_on", "tone_charge_off", "tone_sleep",
};
constexpr unsigned long kWaitForSilenceMaxMs = 30000; // a deferred prompt is dropped after this
}

VoicePrompts &VoicePrompts::getInstance()
{
    static VoicePrompts instance;
    return instance;
}

const char *VoicePrompts::name(Prompt p)
{
    return (int)p < (int)Prompt::COUNT ? kNames[(int)p] : "?";
}

const VoicePrompts::Policy &VoicePrompts::policy(Prompt p)
{
    // interrupt, dropWhilePlaying, minInterval
    static const Policy table[(int)Prompt::COUNT] = {
        {false, true, 5000},      // SETUP_WELCOME
        {false, true, 3000},      // SETUP_CONNECTING
        {false, true, 10000},     // SETUP_SUCCESS
        {false, true, 5000},      // SETUP_FAILED
        {false, true, 60000},     // WIFI_LOST
        {false, true, 60000},     // WIFI_RESET_HINT
        {false, true, 0},         // WIFI_RESET_DONE (blocking)
        {false, true, 30000},     // IDLE_HELLO
        {false, true, 10000},     // NFC_READ_TROUBLE
        {false, true, 5000},      // FIGURE_NO_CONTENT
        {false, true, 5000},      // FIGURE_OFFLINE
        {false, true, 5000},      // DOWNLOAD_START
        {true, false, 3000},      // DOWNLOAD_DONE (spoken over the first second of the story)
        {false, true, 10000},     // DOWNLOAD_FAILED
        {false, true, 5000},      // PLAYLIST_END
        {true, false, 240000},    // BATTERY_LOW
        {true, false, 0},         // BATTERY_CRITICAL (blocking)
        {false, true, 3600000},   // CHARGE_FULL
        {false, true, 60000},     // UPDATE_START
        {false, true, 60000},     // UPDATE_DONE
        {false, true, 60000},     // UPDATE_FAILED
        {true, false, 1500},      // TONE_CHARGE_ON
        {true, false, 1500},      // TONE_CHARGE_OFF
        {true, false, 0},         // TONE_SLEEP (blocking)
    };
    return table[(int)p];
}

String VoicePrompts::path(Prompt p) const
{
    const String n = name(p);
    if (n.startsWith("tone_"))
    {
        return "/sounds/" + n + ".mp3";
    }
    return "/sounds/" + language + "/" + n + ".mp3";
}

bool VoicePrompts::available(Prompt p) const
{
    return FileManager::getInstance().fileExists(path(p));
}

void VoicePrompts::begin()
{
    if (!queue)
    {
        queue = xQueueCreate(8, sizeof(uint8_t));
    }
    started = true;
    int present = 0;
    for (int i = 0; i < (int)Prompt::COUNT; ++i)
    {
        if (available((Prompt)i)) present++;
    }
    Serial.printf("VoicePrompts: %d/%d prompt files on the card (language %s)\n", present, (int)Prompt::COUNT, language.c_str());
}

void VoicePrompts::say(Prompt p)
{
    if (!queue || (int)p >= (int)Prompt::COUNT)
    {
        return;
    }
    const uint32_t bit = 1UL << (int)p;
    if (queuedMask & bit)
    {
        return; // already waiting
    }
    const Policy &pol = policy(p);
    const unsigned long now = millis();
    if (pol.minIntervalMs && lastSaidAt[(int)p] && now - lastSaidAt[(int)p] < pol.minIntervalMs)
    {
        return;
    }
    uint8_t id = (uint8_t)p;
    if (xQueueSend(queue, &id, 0) == pdTRUE)
    {
        queuedMask |= bit;
    }
}

bool VoicePrompts::isBusy() const
{
    return AudioController::getInstance().isPromptPlaying();
}

bool VoicePrompts::playNow(Prompt p)
{
    const String file = path(p);
    AudioController &audio = AudioController::getInstance();
    if (!audio.playPrompt(file))
    {
        Serial.printf("VoicePrompts: cannot play %s\n", file.c_str());
        return false;
    }
    lastSaidAt[(int)p] = millis() ? millis() : 1;
    Serial.printf("VoicePrompts: %s\n", name(p));
    return true;
}

void VoicePrompts::sayBlocking(Prompt p, uint32_t maxMs)
{
    if (!started)
    {
        return;
    }
    AudioController &audio = AudioController::getInstance();
    if (!playNow(p))
    {
        return;
    }
    const unsigned long t0 = millis();
    while (audio.isPromptPlaying() && millis() - t0 < maxMs)
    {
        audio.update();
        delay(10);
    }
    // give the resume logic (if a story was interrupted) a chance to run
    audio.update();
}

void VoicePrompts::update()
{
    if (!started || !queue)
    {
        return;
    }
    AudioController &audio = AudioController::getInstance();
    if (audio.isPromptPlaying())
    {
        return;
    }
    if (waitingPrompt < 0)
    {
        uint8_t id;
        if (xQueueReceive(queue, &id, 0) != pdTRUE)
        {
            return;
        }
        queuedMask &= ~(1UL << id);
        waitingPrompt = id;
        waitingSince = millis();
    }
    const Prompt p = (Prompt)waitingPrompt;
    const Policy &pol = policy(p);
    const bool storyPlaying = audio.isPlaying() && !audio.isPromptPlaying();
    if (storyPlaying && !pol.interrupt)
    {
        if (pol.dropWhilePlaying || millis() - waitingSince > kWaitForSilenceMaxMs)
        {
            waitingPrompt = -1; // a story is more important than this prompt
        }
        return;
    }
    waitingPrompt = -1;
    playNow(p);
}
