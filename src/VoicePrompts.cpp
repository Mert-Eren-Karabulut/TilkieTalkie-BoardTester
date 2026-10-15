#include "VoicePrompts.h"
#include "AudioController.h"
#include "FileManager.h"
#include <esp_log.h>

namespace
{
const char *const kNames[(int)Prompt::COUNT] = {
#define TT_PROMPT_NAME(id, name) name,
    TT_SPEECH_PROMPTS(TT_PROMPT_NAME)
    TT_TONE_PROMPTS(TT_PROMPT_NAME)
#undef TT_PROMPT_NAME
};
// Embedded in the image: the portal's PromptSchema::fromImage() reads this NUL-terminated
// marker from the uploaded .bin, so a prompt pack can be checked against exactly what this
// firmware plays. Keep the prefix and the format (comma-separated paths under /sounds).
#define TT_SPEECH_PATH(id, name) "tr/" name ".mp3,"
#define TT_TONE_PATH(id, name) name ".mp3,"
const char kPromptSchema[] __attribute__((used)) =
    "TTPROMPTS1:" TT_SPEECH_PROMPTS(TT_SPEECH_PATH) TT_TONE_PROMPTS(TT_TONE_PATH) "beep.mp3";
#undef TT_SPEECH_PATH
#undef TT_TONE_PATH
const char *TAG = "prompts";
static_assert((int)Prompt::COUNT <= 32, "queuedMask holds one bit per prompt");
}

VoicePrompts &VoicePrompts::getInstance()
{
    static VoicePrompts instance;
    return instance;
}

const char *VoicePrompts::schema()
{
    return kPromptSchema + 11; // after "TTPROMPTS1:"
}

std::vector<String> VoicePrompts::requiredPaths()
{
    std::vector<String> out;
    String all = schema();
    int start = 0;
    while (start < (int)all.length())
    {
        int comma = all.indexOf(',', start);
        if (comma < 0) comma = all.length();
        if (comma > start) out.push_back(all.substring(start, comma));
        start = comma + 1;
    }
    return out;
}

const char *VoicePrompts::name(Prompt p)
{
    return (int)p < (int)Prompt::COUNT ? kNames[(int)p] : "?";
}

const VoicePrompts::Policy &VoicePrompts::policy(Prompt p)
{
    // interrupt, minInterval. Not interrupting = dropped while a story plays (the story wins).
    // There is no "wait for silence" row: nothing needed it, and a prompt spoken minutes after
    // its cause (a story can run for 20 min) would only confuse.
    static const Policy table[(int)Prompt::COUNT] = {
        {false, 5000},      // SETUP_WELCOME
        {false, 3000},      // SETUP_CONNECTING
        {false, 10000},     // SETUP_SUCCESS
        {false, 5000},      // SETUP_FAILED
        {false, 60000},     // WIFI_LOST
        {false, 60000},     // WIFI_RESET_HINT
        {false, 0},         // WIFI_RESET_DONE (blocking)
        {false, 30000},     // IDLE_HELLO
        {false, 10000},     // NFC_READ_TROUBLE
        {false, 5000},      // FIGURE_NO_CONTENT
        {false, 5000},      // FIGURE_OFFLINE
        {false, 5000},      // DOWNLOAD_START
        {true, 3000},       // DOWNLOAD_DONE (spoken over the first second of the story)
        {false, 10000},     // DOWNLOAD_FAILED
        {false, 30000},     // DOWNLOAD_WAIT
        {false, 5000},      // PLAYLIST_END
        {true, 240000},     // BATTERY_LOW
        {true, 0},          // BATTERY_CRITICAL (blocking)
        {false, 3600000},   // CHARGE_FULL
        {false, 60000},     // UPDATE_START
        {false, 60000},     // UPDATE_DONE
        {false, 60000},     // UPDATE_FAILED
        {true, 1500},       // TONE_CHARGE_ON
        {true, 1500},       // TONE_CHARGE_OFF
        {true, 0},          // TONE_SLEEP (blocking)
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
    ESP_LOGI(TAG, "%d/%d prompt files on the card (language %s)", present, (int)Prompt::COUNT, language.c_str());
}

void VoicePrompts::say(Prompt p)
{
    if (!queue || (int)p >= (int)Prompt::COUNT)
    {
        return;
    }
    const uint32_t bit = 1UL << (int)p;
    // Claim the bit first: of two tasks saying the same prompt at once, exactly one queues it.
    if (queuedMask.fetch_or(bit) & bit)
    {
        return; // already waiting
    }
    const Policy &pol = policy(p);
    const uint32_t now = millis();
    const uint32_t last = lastSaidAt[(int)p].load();
    uint8_t id = (uint8_t)p;
    const bool tooSoon = pol.minIntervalMs && last && now - last < pol.minIntervalMs;
    if (tooSoon || xQueueSend(queue, &id, 0) != pdTRUE)
    {
        queuedMask.fetch_and(~bit); // not queued: release the claim
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
        ESP_LOGW(TAG, "cannot play %s", file.c_str());
        return false;
    }
    const uint32_t now = millis();
    lastSaidAt[(int)p].store(now ? now : 1);
    ESP_LOGI(TAG, "%s", name(p));
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
    uint8_t id;
    if (xQueueReceive(queue, &id, 0) != pdTRUE)
    {
        return;
    }
    queuedMask.fetch_and(~(1UL << id));
    const Prompt p = (Prompt)id;
    const bool storyPlaying = audio.isPlaying() && !audio.isPromptPlaying();
    if (storyPlaying && !policy(p).interrupt)
    {
        ESP_LOGI(TAG, "%s dropped, a story is playing", name(p));
        return; // a story is more important than this prompt
    }
    playNow(p);
}
