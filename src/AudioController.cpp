#include "AudioController.h"
#include "ConfigManager.h"
#include "NfcController.h"
#include "VoicePrompts.h"
#include <esp_log.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

// Initialize static members
AudioController* AudioController::instance = nullptr;

// Define static constants
const int AudioController::MIN_VOLUME;
const int AudioController::MAX_VOLUME;
const int AudioController::DEFAULT_VOLUME;
const int AudioController::VOLUME_STEP;
const int AudioController::HEADPHONE_CEILING_STEPS;

// ES8388 register definitions
#define ES8388_CONTROL1         0x00
#define ES8388_CONTROL2         0x01
#define ES8388_CHIPPOWER        0x02
#define ES8388_ADCPOWER         0x03
#define ES8388_DACPOWER         0x04
#define ES8388_CHIPLOPOW1       0x05
#define ES8388_CHIPLOPOW2       0x06
#define ES8388_ANAVOLMANAG      0x07
#define ES8388_MASTERMODE       0x08
#define ES8388_ADCCONTROL1      0x09
#define ES8388_ADCCONTROL4      0x0C
#define ES8388_DACCONTROL1      0x17
#define ES8388_DACCONTROL2      0x18
#define ES8388_DACCONTROL3      0x19
#define ES8388_DACCONTROL4      0x1A
#define ES8388_DACCONTROL5      0x1B
#define ES8388_DACCONTROL16     0x26
#define ES8388_DACCONTROL17     0x27
#define ES8388_DACCONTROL20     0x2A
#define ES8388_DACCONTROL21     0x2B
#define ES8388_DACCONTROL23     0x2D
#define ES8388_LOUT1VOL         0x2E  // speaker path (NS4150B on LOUT1)
#define ES8388_ROUT1VOL         0x2F  // unconnected on this board
#define ES8388_LOUT2VOL         0x30  // headphone jack
#define ES8388_ROUT2VOL         0x31

// DACPOWER: DAC L/R on (bits 7:6 = 0), LOUT1 (0x20), LOUT2 (0x08), ROUT2 (0x04). ROUT1 is not wired.
#define ES8388_DACPOWER_ACTIVE  0x2C
#define ES8388_DACPOWER_OFF     0xC0
// DACCONTROL3: bit5 soft ramp, bit1 mute
#define ES8388_DAC_UNMUTE_RAMP  0x20
#define ES8388_DAC_MUTE_RAMP    0x22


namespace {
const char* TAG = "audio";

// Recursive audio lock for the lifetime of a scope (the audio task holds it for a decode slice,
// control calls hold it while they change the player). No-op before begin() created the mutex.
class AudioLock {
public:
    explicit AudioLock(SemaphoreHandle_t m)
        : mutex(m), held(m && xSemaphoreTakeRecursive(m, portMAX_DELAY) == pdTRUE) {}
    ~AudioLock() {
        if (held) xSemaphoreGiveRecursive(mutex);
    }
    AudioLock(const AudioLock&) = delete;
    AudioLock& operator=(const AudioLock&) = delete;
private:
    SemaphoreHandle_t mutex;
    bool held;
};

const char* baseName(const String& path) {
    const int slash = path.lastIndexOf('/');
    return path.c_str() + (slash >= 0 ? slash + 1 : 0);
}
} // namespace

AudioController::AudioController() :
    audioOutput(nullptr),
    source(nullptr),
    audioMP3(nullptr),
    mp3Prealloc(nullptr),
    mp3PreallocSize(0),
    currentState(STOPPED),
    currentTrackPath(""),
    currentVolume(DEFAULT_VOLUME),
    initialized(false),
    transientPlayback(false),
    outputsActive(false),
    codecMuted(true),
    ampMode(AMP_AUTO),
    ampEnabled(false),
    headphonePlugged(false),
    lastPlugPoll(0),
    volumeDirty(false),
    volumeDirtyAt(0),
    pausedTimeSeconds(0.0f),
    hasPausedTime(false),
    trackStartTime(0),
    accumulatedPlayTime(0.0f),
    pauseStartTime(0),
    currentPlaylistIndex(-1),
    playlistFigureUid(""),
    playlistFinished(false),
    fileManager(FileManager::getInstance()),
    audioTaskHandle(nullptr),
    audioMutex(nullptr) {

    ConfigManager& config = ConfigManager::getInstance();
    volumeCeiling = config.getInt("volume_ceiling", MAX_VOLUME);
    volumeCeiling = min(volumeCeiling, MAX_VOLUME);
    volumeCeiling = max(volumeCeiling, MIN_VOLUME);

    currentVolume = config.getInt("audio_volume", DEFAULT_VOLUME);
    currentVolume = min(currentVolume, volumeCeiling);
    currentVolume = max(currentVolume, MIN_VOLUME);
}

AudioController::~AudioController() {
    end();
}

AudioController& AudioController::getInstance() {
    if (instance == nullptr) {
        instance = new AudioController();
    }
    return *instance;
}

bool AudioController::begin() {
    if (initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Beginning initialization...");

    if (!audioMutex) {
        audioMutex = xSemaphoreCreateRecursiveMutex();
        if (!audioMutex) {
            ESP_LOGE(TAG, "Failed to create audio mutex");
            return false;
        }
    }

    // Speaker amp off until there is something to play (open-drain: LOW = off, released = on).
    pinMode(AMP_MODE_PIN, OUTPUT_OPEN_DRAIN);
    digitalWrite(AMP_MODE_PIN, LOW);
    ampEnabled = false;

    // I2C for ES8388 control
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);

    // I2S first: the driver stays installed from here on, so MCLK runs before the codec is
    // configured and keeps running between tracks.
    if (!audioOutput) {
        audioOutput = new PersistentI2SOutput(0, AudioOutputI2S::EXTERNAL_I2S, 16, AudioOutputI2S::APLL_DISABLE);
        if (!audioOutput->SetMclk(true) ||
            !audioOutput->SetPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_MCLK_PIN)) {
            ESP_LOGE(TAG, "Failed to configure I2S pins");
            delete audioOutput;
            audioOutput = nullptr;
            return false;
        }
        audioOutput->SetOutputModeMono(true); // speaker is on LOUT1 only: feed it the L+R mix
        audioOutput->SetGain(1.0f);           // attenuation is done in the codec, not in software
    }
    if (!audioOutput->installed() && !audioOutput->begin()) {
        ESP_LOGE(TAG, "Failed to install the I2S driver");
        return false;
    }
    ESP_LOGI(TAG, "I2S up - BCLK:%d, LRCK:%d, DOUT:%d, MCLK:%d (16 DMA buffers, mono mix)",
             I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_MCLK_PIN);

    if (!source) {
        source = new ReadAheadFileSource();
    }
    if (!source->begin()) {
        ESP_LOGE(TAG, "Failed to start the read-ahead source");
        return false;
    }

    if (!mp3Prealloc) {
        mp3PreallocSize = AudioGeneratorMP3::preAllocSize();
        // libmad's stream/frame/synth state (~29 KB) lives in PSRAM: internal RAM is the scarce
        // resource on this board and the S3's 64 KB data cache keeps the decoder's working set
        // close. Falls back to internal RAM if PSRAM is unavailable.
        mp3Prealloc = heap_caps_malloc(mp3PreallocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const char* where = "PSRAM";
        if (!mp3Prealloc) {
            mp3Prealloc = heap_caps_malloc(mp3PreallocSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            where = "internal RAM";
        }
        if (!mp3Prealloc) {
            ESP_LOGE(TAG, "Failed to allocate the decoder workspace");
            return false;
        }
        ESP_LOGI(TAG, "decoder workspace %d bytes (%s, allocated once)", mp3PreallocSize, where);
    }

    delay(50); // MCLK settles

    if (!initializeES8388()) {
        ESP_LOGE(TAG, "Failed to initialize ES8388");
        return false;
    }

    initialized = true;

    if (!audioTaskHandle) {
        BaseType_t taskCreated = xTaskCreatePinnedToCore(
            audioTaskEntry,
            "AudioPlayback",
            8192,
            this,
            12,  // above the loop task and WiFi housekeeping; blocks on I2S DMA back-pressure
            &audioTaskHandle,
            1);

        if (taskCreated != pdPASS) {
            ESP_LOGW(TAG, "Failed to create background audio task, falling back to loop updates");
            audioTaskHandle = nullptr;
        } else {
            ESP_LOGI(TAG, "Background audio task created on Core 1 (priority 12)");
        }
    }

    applyCodecVolume();
    ESP_LOGI(TAG, "Initialization complete - volume %d%% (ceiling %d%%)", currentVolume, volumeCeiling);
    return true;
}

void AudioController::end() {
    if (!initialized) {
        return;
    }

    if (volumeDirty) {
        ConfigManager::getInstance().storeInt("audio_volume", currentVolume);
        volumeDirty = false;
    }

    stop();

    // Codec and amp down, I2S released: nothing drives the speaker or draws current in sleep.
    muteES8388(true);
    writeES8388Register(ES8388_DACPOWER, ES8388_DACPOWER_OFF);
    writeES8388Register(ES8388_CHIPPOWER, 0xFF);
    digitalWrite(AMP_MODE_PIN, LOW);
    ampEnabled = false;
    if (audioOutput) {
        audioOutput->teardown();
    }

    initialized = false;
    ESP_LOGI(TAG, "powered down");
}

void AudioController::audioTaskEntry(void* parameter) {
    AudioController* self = static_cast<AudioController*>(parameter);

    while (true) {
        unsigned long sliceStart = millis();
        if (self && self->initialized) {
            AudioLock lock(self->audioMutex);
            self->updatePlaybackSlice();
        }
        unsigned long took = millis() - sliceStart;
        if (took > 500) {
            // A slice ran far longer than a few decode passes (SD stall, track change under
            // memory pressure): log it and give the lower-priority tasks on this core a breather
            // so the loop and the idle task are never starved.
            ESP_LOGW(TAG, "slice took %lu ms", took);
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---------------------------------------------------------------------------------------------
// Output path: codec mute + speaker amp follow one flag so every transition is ordered the same
// way (amp on -> unmute with ramp; mute with ramp -> amp off).
// ---------------------------------------------------------------------------------------------

void AudioController::setOutputsActive(bool active) {
    outputsActive = active;
    if (active) {
        applyAmp();
        muteES8388(currentVolume == 0);
    } else {
        muteES8388(true);
        delay(15); // let the soft ramp finish before the amp drops
        applyAmp();
    }
}

void AudioController::applyAmp() {
    bool desired;
    switch (ampMode) {
        case AMP_FORCE_ON:  desired = true; break;
        case AMP_FORCE_OFF: desired = false; break;
        default:            desired = outputsActive; break;
    }
    if (desired == ampEnabled) {
        return;
    }
    // Open-drain: HIGH releases the line (board pull-up / jack contact decide), LOW forces off.
    digitalWrite(AMP_MODE_PIN, desired ? HIGH : LOW);
    ampEnabled = desired;
    ESP_LOGI(TAG, "amp %s", desired ? "released (on unless a plug is in)" : "off");
}

void AudioController::pollHeadphone() {
    // Only meaningful while the line is released: a plug pulls AMP_CTRL low.
    if (!ampEnabled || millis() - lastPlugPoll < 500) {
        return;
    }
    lastPlugPoll = millis();
    bool plugged = digitalRead(AMP_MODE_PIN) == LOW;
    if (plugged != headphonePlugged) {
        headphonePlugged = plugged;
        ESP_LOGI(TAG, "headphone %s", plugged ? "plugged (speaker muted by the jack)" : "removed");
    }
}

void AudioController::setAmpMode(AmpMode mode) {
    ampMode = mode;
    applyAmp();
}

// ---------------------------------------------------------------------------------------------
// Decoder lifecycle: the read-ahead source, I2S output and libmad workspace live for the whole
// run; only the small AudioGeneratorMP3 object is created per track.
// ---------------------------------------------------------------------------------------------

bool AudioController::startDecoder(const String& filePath) {
    releaseDecoder();

    if (!source->open(filePath.c_str())) {
        ESP_LOGW(TAG, "Failed to open file: %s", filePath.c_str());
        return false;
    }

    audioMP3 = new AudioGeneratorMP3(mp3Prealloc, mp3PreallocSize);
    if (!audioMP3) {
        source->close();
        return false;
    }
    if (!audioMP3->begin(source, audioOutput)) {
        ESP_LOGW(TAG, "Failed to start MP3 decoder");
        releaseDecoder();
        return false;
    }
    return true;
}

void AudioController::releaseDecoder() {
    if (audioMP3) {
        audioMP3->stop(); // our output keeps I2S; this also closes (parks) the source
        delete audioMP3;
        audioMP3 = nullptr;
    }
    if (source && source->isOpen()) {
        source->close();
    }
}

// ---------------------------------------------------------------------------------------------
// Faults found under the audio lock (often on the audio task) are queued and raised by the loop.
// ---------------------------------------------------------------------------------------------

void AudioController::queueFault(Fault f, const String& path, bool speak, const char* extra) {
    AudioLock lock(audioMutex);
    if (pendingFaultCount >= MAX_PENDING_FAULTS) {
        return; // the loop drains every pass; a burst beyond this is the same story repeated
    }
    PendingFault& pf = pendingFaults[pendingFaultCount++];
    pf.fault = f;
    pf.speak = speak;
    snprintf(pf.detail, sizeof(pf.detail), "%s%s%s", baseName(path), extra ? " " : "", extra ? extra : "");
}

void AudioController::raisePendingFaults() {
    PendingFault copy[MAX_PENDING_FAULTS];
    int n = 0;
    {
        AudioLock lock(audioMutex);
        n = pendingFaultCount;
        for (int i = 0; i < n; ++i) copy[i] = pendingFaults[i];
        pendingFaultCount = 0;
    }
    for (int i = 0; i < n; ++i) {
        Faults::raise(copy[i].fault, copy[i].detail, copy[i].speak);
    }
}

// ---------------------------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------------------------

bool AudioController::play(const String& filePath) {
    AudioLock lock(audioMutex);

    if (!initialized) {
        ESP_LOGW(TAG, "Not initialized");
        return false;
    }
    if (!filePath.isEmpty()) {
        return playFile(filePath);
    }

    // Playlist case
    if (!hasPlaylist()) {
        ESP_LOGI(TAG, "No playlist available");
        return false;
    }
    if (!isNfcSessionActive(playlistFigureUid)) {
        ESP_LOGI(TAG, "NFC session not active, clearing playlist");
        clearPlaylist();
        return false;
    }
    if (playlistFinished || currentPlaylistIndex == -1) {
        currentPlaylistIndex = 0;
        playlistFinished = false;
    }
    if (currentPlaylistIndex >= (int)playlist.size()) {
        ESP_LOGW(TAG, "Playlist index out of range");
        return false;
    }

    // A track that is not on the card is skipped (still downloading while the playlist is open,
    // otherwise reported as TRACK_MISSING); one that cannot start decoding is reported as
    // DECODER_FAILED and skipped. Each index is tried once; when none is left the playlist
    // waits for the next download or ends, exactly as after its last track.
    while (currentPlaylistIndex < (int)playlist.size()) {
        const String trackPath = playlist[currentPlaylistIndex];
        if (!fileManager.fileExists(trackPath)) {
            ESP_LOGI(TAG, "skipping missing track %s", trackPath.c_str());
            if (!playlistOpen) {
                queueFault(Fault::TRACK_MISSING, trackPath, false);
            }
            currentPlaylistIndex++;
            continue;
        }
        ESP_LOGI(TAG, "Playing track %d: %s", currentPlaylistIndex, trackPath.c_str());
        if (playFile(trackPath)) {
            waitingForTrack = false;
            return true;
        }
        ESP_LOGW(TAG, "track %d could not be started - skipping it", currentPlaylistIndex);
        currentPlaylistIndex++;
    }
    endOfCachedTracks();
    return false;
}

// One file, story or prompt; the audio lock is held by the caller.
bool AudioController::playFile(const String& filePath) {
    if (!fileManager.fileExists(filePath)) {
        ESP_LOGW(TAG, "File does not exist: %s", filePath.c_str());
        if (!startingPrompt) {
            queueFault(Fault::TRACK_MISSING, filePath, false);
        }
        return false;
    }

    if (!isValidAudioFile(filePath)) {
        ESP_LOGW(TAG, "Invalid audio file: %s", filePath.c_str());
        if (!startingPrompt) {
            queueFault(Fault::DECODER_FAILED, filePath);
        }
        return false;
    }

    if (!startingPrompt) {
        if (isPromptPlaying()) {
            // Let the prompt finish its sentence; update() starts the story right after.
            deferredPlayPath = filePath;
            deferredPlayPending = true;
            ESP_LOGI(TAG, "story %s waits for the prompt to finish", filePath.c_str());
            return true;
        }
        transientPlayback = false; // a story start abandons any prompt bookkeeping
        resumeAfterPrompt = false;
    }

    if (currentState != STOPPED) {
        stopPlayback();
    }

    if (!startDecoder(filePath)) {
        if (!startingPrompt) {
            queueFault(Fault::DECODER_FAILED, filePath);
        }
        return false;
    }

    currentTrackPath = filePath;
    currentState = PLAYING;
    trackStartTime = millis();
    accumulatedPlayTime = 0.0f;
    pauseStartTime = 0;
    hasPausedTime = false;
    setOutputsActive(true);

    ESP_LOGI(TAG, "Successfully started playing: %s", filePath.c_str());
    return true;
}

bool AudioController::pause() {
    AudioLock lock(audioMutex);

    if (!(initialized && currentState == PLAYING && audioMP3 && audioMP3->isRunning())) {
        return false;
    }
    float currentPlayTime = accumulatedPlayTime + ((millis() - trackStartTime) / 1000.0f);
    pausedTimeSeconds = currentPlayTime;
    hasPausedTime = true;
    accumulatedPlayTime = currentPlayTime;
    pauseStartTime = millis();

    // The decoder simply stops being serviced: nothing is torn down, resume continues
    // exactly where we were.
    currentState = PAUSED;
    setOutputsActive(false);
    ESP_LOGI(TAG, "Paused at %.2f seconds", pausedTimeSeconds);
    return true;
}

bool AudioController::resume() {
    AudioLock lock(audioMutex);

    if (initialized && currentState == PAUSED && audioMP3 && audioMP3->isRunning()) {
        currentState = PLAYING;
        trackStartTime = millis();
        setOutputsActive(true);
        ESP_LOGI(TAG, "Resumed from %.2f seconds", accumulatedPlayTime);
        return true;
    }
    if (initialized && currentState == PAUSED) {
        // Decoder went away (should not happen); restart the track from the paused position.
        // The lock is held across play + seek, so the audio task cannot decode from byte 0.
        String path = currentTrackPath;
        float at = pausedTimeSeconds;
        currentState = STOPPED;
        if (!path.isEmpty() && play(path)) {
            if (at > 0.5f) seekTo(at);
            return true;
        }
    }
    return false;
}

// Decoder and outputs down. Used inside track changes; the prompt bookkeeping (a prompt that
// is starting, the story to resume after it) is left alone.
void AudioController::stopPlayback() {
    if (!initialized || currentState == STOPPED) {
        return;
    }
    setOutputsActive(false);
    releaseDecoder();
    currentState = STOPPED;
    currentTrackPath = "";
    pausedTimeSeconds = 0.0f;
    hasPausedTime = false;
    trackStartTime = 0;
    accumulatedPlayTime = 0.0f;
    pauseStartTime = 0;
}

// A deliberate stop (figure removed, safety net, console, sleep): whatever plays ends, and
// nothing comes back by itself - no story resumed after a prompt, no deferred story start.
bool AudioController::stop() {
    AudioLock lock(audioMutex);
    const bool wasPlaying = initialized && currentState != STOPPED;
    stopPlayback();
    transientPlayback = false;
    resumeAfterPrompt = false;
    deferredPlayPending = false;
    promptEnded = false;
    return wasPlaying;
}

bool AudioController::seekTo(float seconds) {
    AudioLock lock(audioMutex);

    if (!initialized) {
        ESP_LOGW(TAG, "Cannot seek - not initialized");
        return false;
    }

    if (currentState == STOPPED || currentTrackPath.isEmpty() || !audioMP3 || !source->isOpen()) {
        ESP_LOGW(TAG, "Cannot seek - no track loaded");
        return false;
    }

    if (seconds < 0) {
        seconds = 0;
    }

    // Byte estimate for our CBR content (96 kbps); libmad resyncs on the next frame header.
    const uint32_t BYTES_PER_SECOND = 12000;
    const uint32_t HEADER_OFFSET = 512;
    uint32_t fileSize = source->getSize();
    uint32_t target = (uint32_t)(seconds * BYTES_PER_SECOND) + HEADER_OFFSET;
    if (fileSize > 1024 && target > fileSize - 1024) {
        target = fileSize - 1024;
    }

    bool wasActive = outputsActive;
    if (wasActive) muteES8388(true); // hide the resync glitch
    if (!source->seek((int32_t)target, SEEK_SET)) {
        ESP_LOGW(TAG, "Seek failed");
        if (wasActive) muteES8388(currentVolume == 0);
        return false;
    }
    audioMP3->desync();
    if (wasActive) muteES8388(currentVolume == 0);

    accumulatedPlayTime = seconds;
    if (currentState == PLAYING) {
        trackStartTime = millis();
    } else {
        pausedTimeSeconds = seconds;
        hasPausedTime = true;
    }
    ESP_LOGI(TAG, "Seeked to %.2f s (byte %u of %u)", seconds, (unsigned)target, (unsigned)fileSize);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Volume: codec attenuation only (software gain stays 1.0). LOUT1VOL (speaker) gets the setting,
// LOUT2/ROUT2 (jack) sit HEADPHONE_CEILING_STEPS below it. The NVS write is deferred to update().
// ---------------------------------------------------------------------------------------------

bool AudioController::volumeUp() {
    int newVolume = currentVolume + VOLUME_STEP;
    if (newVolume > volumeCeiling) {
        newVolume = volumeCeiling;
    }

    bool changed = setVolume(newVolume);
    if (changed && currentState == STOPPED) {
        volumeBeep();
    }

    return changed;
}

bool AudioController::volumeDown() {
    int newVolume = currentVolume - VOLUME_STEP;
    if (newVolume < MIN_VOLUME) {
        newVolume = MIN_VOLUME;
    }

    bool changed = setVolume(newVolume);
    if (changed && currentState == STOPPED) {
        volumeBeep();
    }

    return changed;
}

bool AudioController::setVolume(int volume, bool initialize /* = false */) {
    AudioLock lock(audioMutex);

    if (!initialized) {
        ESP_LOGW(TAG, "Cannot set volume - not initialized");
        return false;
    }

    if (volume < MIN_VOLUME) volume = MIN_VOLUME;
    if (volume > volumeCeiling) volume = volumeCeiling;

    if (volume == currentVolume && !initialize) {
        return false;
    }

    currentVolume = volume;
    applyCodecVolume();
    if (!initialize) {
        volumeDirty = true;
        volumeDirtyAt = millis();
    }
    ESP_LOGI(TAG, "Volume %d%%", volume);
    return true;
}

void AudioController::applyCodecVolume() {
    // LOUTxVOL: 6-bit, 0 = -45 dB, 30 (0x1E) = 0 dB, 1.5 dB per step. 0 % is a real mute.
    uint8_t speaker = 0;
    if (currentVolume > 0) {
        speaker = (uint8_t)round((currentVolume / 100.0f) * 30.0f);
        if (speaker > 0x1E) speaker = 0x1E;
    }
    uint8_t headphone = speaker > HEADPHONE_CEILING_STEPS ? speaker - HEADPHONE_CEILING_STEPS : 0;

    writeES8388Register(ES8388_LOUT1VOL, speaker);
    writeES8388Register(ES8388_ROUT1VOL, speaker);
    writeES8388Register(ES8388_LOUT2VOL, headphone);
    writeES8388Register(ES8388_ROUT2VOL, headphone);

    if (outputsActive) {
        muteES8388(currentVolume == 0);
    }
}

// Volume ceiling control methods
void AudioController::setVolumeCeiling(int ceiling) {
    ceiling = min(ceiling, MAX_VOLUME);
    ceiling = max(ceiling, MIN_VOLUME);

    volumeCeiling = ceiling;

    ConfigManager& config = ConfigManager::getInstance();
    config.storeInt("volume_ceiling", volumeCeiling);

    if (currentVolume > volumeCeiling) {
        setVolume(volumeCeiling);
    }

    ESP_LOGI(TAG, "Volume ceiling set to %d%%", volumeCeiling);
}

int AudioController::getVolumeCeiling() const {
    return volumeCeiling;
}

// ---------------------------------------------------------------------------------------------
// Playlist
// ---------------------------------------------------------------------------------------------

String AudioController::getCurrentTrack() const {
    AudioLock lock(audioMutex);
    if (transientPlayback) {
        return resumeAfterPrompt ? resumeTrackPath : String();
    }
    return currentTrackPath;
}

String AudioController::getPlaylistFigureUid() const {
    AudioLock lock(audioMutex);
    return playlistFigureUid;
}

bool AudioController::playlistContains(const String& path) const {
    AudioLock lock(audioMutex);
    for (const auto& p : playlist) {
        if (p == path) return true;
    }
    return false;
}

void AudioController::setPlaylist(const std::vector<String>& trackPaths, const String& figureUid, bool moreComing) {
    AudioLock lock(audioMutex);
    resumeAfterPrompt = false;
    playlist = trackPaths;
    playlistFigureUid = figureUid;
    currentPlaylistIndex = -1; // Start at -1, first play() will set to 0
    playlistFinished = false;
    playlistOpen = moreComing;
    waitingForTrack = false;

    ESP_LOGI(TAG, "Playlist set with %u tracks for figure UID: %s", (unsigned)playlist.size(), figureUid.c_str());

    for (int i = 0; i < (int)playlist.size(); i++) {
        ESP_LOGI(TAG, "  Track %d: %s", i + 1, playlist[i].c_str());
    }
}

void AudioController::mergePlaylist(const std::vector<String>& trackPaths, const String& figureUid, bool moreComing) {
    AudioLock lock(audioMutex);
    playlistOpen = moreComing;
    if (figureUid != playlistFigureUid || playlist.empty()) {
        playlist = trackPaths;
        playlistFigureUid = figureUid;
        currentPlaylistIndex = -1;
        playlistFinished = false;
        waitingForTrack = false;
    } else {
        int newIndex = -1;
        if (currentPlaylistIndex >= 0 && currentPlaylistIndex < (int)playlist.size()) {
            const String current = playlist[currentPlaylistIndex];
            for (size_t i = 0; i < trackPaths.size(); ++i) {
                if (trackPaths[i] == current) {
                    newIndex = (int)i;
                    break;
                }
            }
        }
        playlist = trackPaths;
        if (newIndex >= 0) {
            currentPlaylistIndex = newIndex;
            if (playlistFinished && newIndex + 1 < (int)playlist.size()) {
                playlistFinished = false; // new tracks arrived after the one we finished on
            }
        } else if (currentPlaylistIndex >= (int)playlist.size()) {
            currentPlaylistIndex = (int)playlist.size() - 1;
        }
    }
    // Waiting for a download: continue with the first cached track after the one we finished on.
    bool resume = false;
    if (waitingForTrack) {
        int after = -1;
        for (size_t i = 0; i < playlist.size(); ++i) {
            if (playlist[i] == waitAfterPath) { after = (int)i; break; }
        }
        int next = after + 1; // the finished track vanished from the manifest: start over
        while (next < (int)playlist.size() && !fileManager.fileExists(playlist[next])) {
            next++;
        }
        if (next < (int)playlist.size()) {
            currentPlaylistIndex = next;
            waitingForTrack = false;
            playlistFinished = false;
            resume = true;
        } else if (!moreComing) {
            // Nothing after it and nothing more on the way: the stories are over.
            waitingForTrack = false;
            playlistFinished = true;
            playlistEnded = true;
        }
    }
    ESP_LOGI(TAG, "playlist merged: %u tracks for %s (index %d%s%s)",
             (unsigned)playlist.size(), figureUid.c_str(), currentPlaylistIndex,
             moreComing ? ", more coming" : "", waitingForTrack ? ", waiting" : "");
    if (resume) {
        ESP_LOGI(TAG, "next track arrived - continuing with %s", playlist[currentPlaylistIndex].c_str());
        play();
    }
}

bool AudioController::consumeWaitStarted() {
    return waitStarted.exchange(false);
}

void AudioController::clearPlaylist() {
    AudioLock lock(audioMutex);
    resumeAfterPrompt = false;
    deferredPlayPending = false;
    playlist.clear();
    currentPlaylistIndex = -1;
    playlistFigureUid = "";
    playlistFinished = false;
    playlistOpen = false;
    waitingForTrack = false;

    ESP_LOGI(TAG, "Playlist cleared");
}

// Lock held. The playlist's figure must still be on the pins; otherwise the playlist goes.
bool AudioController::playlistFigurePresent() {
    if (!hasPlaylist()) {
        ESP_LOGI(TAG, "No playlist available");
        return false;
    }
    if (!isNfcSessionActive(playlistFigureUid)) {
        ESP_LOGI(TAG, "Figure not present or different figure detected, clearing playlist");
        clearPlaylist();
        return false;
    }
    return true;
}

// Lock held. No playable track after the current one: wait for the figure's next download
// (playlistOpen) or end the playlist. A prompt that is speaking finishes its sentence, but no
// story is resumed or started after it.
void AudioController::endOfCachedTracks() {
    if (playlistOpen) {
        // The figure's remaining tracks are still downloading: hold here instead of ending.
        // mergePlaylist() continues with the next track as soon as it is on the card.
        waitAfterPath = playlist.empty() ? String() : playlist.back();
        currentPlaylistIndex = (int)playlist.size() - 1;
        waitingForTrack = true;
        waitingSince = millis();
        waitStarted = true;
        ESP_LOGI(TAG, "cached tracks finished, waiting for the next download");
    } else {
        ESP_LOGI(TAG, "Reached end of playlist");
        playlistFinished = true;
        playlistEnded = true;
    }
    if (isPromptPlaying()) {
        resumeAfterPrompt = false;
        deferredPlayPending = false;
    } else {
        stopPlayback();
    }
}

bool AudioController::nextTrack() {
    AudioLock lock(audioMutex);
    if (!playlistFigurePresent()) {
        return false;
    }

    if (waitingForTrack) {
        // Already parked after the last cached track: Next changes nothing (and must not
        // announce the wait again); the merge continues as soon as the download lands.
        ESP_LOGI(TAG, "still waiting for the next download");
        return false;
    }

    if (playlistFinished) {
        currentPlaylistIndex = 0;
        playlistFinished = false;
        return play();
    }

    // play() skips tracks that are not on the card (still downloading or missing) and tracks
    // that cannot be decoded, and waits or ends the playlist when none is left.
    currentPlaylistIndex++;
    if (currentPlaylistIndex >= (int)playlist.size()) {
        endOfCachedTracks();
        return false;
    }
    return play();
}

bool AudioController::prevTrack() {
    AudioLock lock(audioMutex);
    if (!playlistFigurePresent()) {
        return false;
    }

    // More than 3 s in: restart the current track.
    if ((currentState == PLAYING || currentState == PAUSED) && (getCurrentTrackSeconds() > 3.0f)) {
        ESP_LOGI(TAG, "Restarting current track");
        String trackPath = currentTrackPath;
        if (trackPath.isEmpty()) {
            return false;
        }
        return play(trackPath);
    }

    waitingForTrack = false;
    if (playlistFinished || currentPlaylistIndex <= 0) {
        currentPlaylistIndex = playlist.size() - 1;
        playlistFinished = false;
        return play();
    }

    currentPlaylistIndex--;
    return play();
}

bool AudioController::playTrack(const String& trackId) {
    AudioLock lock(audioMutex);
    if (!playlistFigurePresent()) {
        return false;
    }

    // The portal sends the track's path from list-files ("/assets/..."): matched exactly, since
    // a content track and a custom track can share an id. A bare id (older portals) still
    // matches the path suffix /{trackId}.mp3.
    const bool byPath = trackId.startsWith("/");
    String suffix = "/" + trackId + ".mp3";

    for (int i = 0; i < (int)playlist.size(); i++) {
        if (byPath ? playlist[i] == trackId : playlist[i].endsWith(suffix)) {
            ESP_LOGI(TAG, "Found track ID %s at playlist index %d", trackId.c_str(), i);
            currentPlaylistIndex = i;
            playlistFinished = false;
            waitingForTrack = false;
            return play();
        }
    }

    ESP_LOGW(TAG, "Track ID %s not found in current playlist", trackId.c_str());
    return false;
}

// Audio task, lock held: the decoder reported the end of the file.
void AudioController::finishTrack() {
    const bool wasTransient = transientPlayback;
    if (!wasTransient && source) {
        const uint32_t underruns = source->underruns(); // per track: reset when a file is opened
        if (underruns > 0) {
            totalUnderruns += underruns;
            char extra[16];
            snprintf(extra, sizeof(extra), "n=%u", (unsigned)underruns);
            queueFault(Fault::AUDIO_UNDERRUN, currentTrackPath, false, extra);
        }
    }
    stopPlayback();
    transientPlayback = false;
    if (wasTransient) {
        promptEnded = true; // the loop task resumes the interrupted track (update())
        return;
    }
    if (hasPlaylist() && !playlistFinished) {
        nextTrack();
    }
}

// Prompts and the volume beep. One lock around the bookkeeping and the start, so the audio task
// never sees a half-written prompt state.
bool AudioController::playPrompt(const String& filePath) {
    AudioLock lock(audioMutex);
    if (!initialized) {
        return false;
    }
    if (!fileManager.fileExists(filePath)) {
        ESP_LOGW(TAG, "prompt file missing: %s", filePath.c_str());
        return false;
    }
    if (isPromptPlaying()) {
        return false; // one at a time; VoicePrompts queues the rest
    }
    resumeAfterPrompt = false;
    if ((currentState == PLAYING || currentState == PAUSED) && !currentTrackPath.isEmpty()) {
        resumeTrackPath = currentTrackPath;
        resumeSeconds = getCurrentTrackSeconds();
        resumeAsPaused = (currentState == PAUSED);
        resumeAfterPrompt = true;
    }
    promptEnded = false;
    transientPlayback = true;
    startingPrompt = true;
    const bool ok = playFile(filePath);
    startingPrompt = false;
    if (!ok) {
        transientPlayback = false;
        resumeAfterPrompt = false;
        return false;
    }
    return true;
}

bool AudioController::consumePlaylistEnded() {
    return playlistEnded.exchange(false);
}

// Audio task, lock held.
void AudioController::updatePlaybackSlice() {
    if (!initialized) {
        return;
    }

    if (currentState == PLAYING) {
        const int maxDecodePassesPerUpdate = 4;

        // Safety net only: the debounced detach (NfcController -> afterDetachNFC) is the real
        // stop path. A raw pin read here stopped playback on every wobble of the figure.
        if (hasPlaylist() && !NfcController::getInstance().isCardPresent()) {
            ESP_LOGW(TAG, "figure no longer present during playback");
            stop();
            return;
        }

        if (audioMP3 && audioMP3->isRunning()) {
            for (int decodePass = 0; decodePass < maxDecodePassesPerUpdate; ++decodePass) {
                if (!audioMP3->loop()) {
                    finishTrack();
                    break;
                }
            }
        } else {
            ESP_LOGW(TAG, "Playback stopped unexpectedly");
            stop();
        }
    }
    // PAUSED: the decoder is simply not serviced; STOPPED: nothing to do
}

void AudioController::update() {
    // Loop-task housekeeping only; decoding happens on the audio task.
    if (!initialized) {
        return;
    }
    if (volumeDirty && millis() - volumeDirtyAt > 2000) {
        ConfigManager::getInstance().storeInt("audio_volume", currentVolume);
        volumeDirty = false;
    }
    if (promptEnded) {
        // One lock across play + seek (+ pause): the audio task cannot decode the first frames
        // of the resumed story from byte 0 before the seek lands.
        AudioLock lock(audioMutex);
        if (promptEnded.exchange(false)) {
            if (deferredPlayPending) {
                deferredPlayPending = false;
                resumeAfterPrompt = false;
                const String path = deferredPlayPath;
                // A playlist track goes through the playlist (skips a track that cannot start).
                const bool isPlaylistTrack = currentPlaylistIndex >= 0 && currentPlaylistIndex < (int)playlist.size() &&
                                             playlist[currentPlaylistIndex] == path;
                if (isPlaylistTrack) {
                    play();
                } else {
                    play(path);
                }
            } else if (resumeAfterPrompt) {
                resumeAfterPrompt = false;
                const bool figureStillThere = playlist.empty() || isNfcSessionActive(playlistFigureUid);
                if (figureStillThere && fileManager.fileExists(resumeTrackPath) && play(resumeTrackPath)) {
                    if (resumeSeconds > 1.0f) {
                        seekTo(resumeSeconds);
                    }
                    if (resumeAsPaused) {
                        pause();
                    }
                }
            }
        }
    }
    if (waitingForTrack && millis() - waitingSince > DOWNLOAD_WAIT_MAX_MS) {
        bool gaveUp = false;
        String uid;
        {
            AudioLock lock(audioMutex);
            if (waitingForTrack && millis() - waitingSince > DOWNLOAD_WAIT_MAX_MS) {
                ESP_LOGW(TAG, "waited %lu min for the next download - ending the playlist",
                         DOWNLOAD_WAIT_MAX_MS / 60000UL);
                waitingForTrack = false;
                playlistOpen = false;
                playlistFinished = true;
                playlistEnded = true;
                uid = playlistFigureUid;
                gaveUp = true;
            }
        }
        if (gaveUp) {
            Faults::raise(Fault::PLAYLIST_STUCK, uid.c_str());
        }
    }
    raisePendingFaults();
    pollHeadphone();
    if (!audioTaskHandle) {
        AudioLock lock(audioMutex);
        updatePlaybackSlice();
    }
}

void AudioController::volumeBeep() {
    if (!initialized || currentState != STOPPED) {
        return; // never interrupt a track (or a paused one)
    }
    // Played as a prompt: transientPlayback survives the start, so the end of the beep never
    // advances the playlist or re-arms a download wait.
    if (playPrompt(VoicePrompts::kBeepPath)) {
        unsigned long start = millis();
        while (isPromptPlaying() && millis() - start < 3000) {
            update();
            delay(10);
        }
    }
}

void AudioController::printStatus() {
    const char* stateName = currentState == PLAYING ? "PLAYING" : currentState == PAUSED ? "PAUSED" : "STOPPED";
    String track;
    {
        AudioLock lock(audioMutex);
        track = currentTrackPath; // the file actually decoding (a prompt shows as itself here)
    }
    ESP_LOGI(TAG, "%s %s @ %.1fs | volume %d%% (ceiling %d%%) codec %s | amp %s (mode %s)%s | playlist %d/%d%s%s",
             stateName, track.isEmpty() ? "-" : track.c_str(), getCurrentTrackSeconds(),
             currentVolume, volumeCeiling, codecMuted ? "muted" : "live",
             ampEnabled ? "released" : "off",
             ampMode == AMP_AUTO ? "auto" : ampMode == AMP_FORCE_ON ? "on" : "off",
             headphonePlugged ? " headphone plugged" : "",
             currentPlaylistIndex + 1, (int)playlist.size(), playlistFinished ? " (finished)" : "",
             isPromptPlaying() ? " | prompt playing" : "");
    ESP_LOGI(TAG, "read-ahead %u/%u KB, underruns %u (since boot %u), decoder %s, I2S %s, AMP_CTRL level %d | heap int %u PSRAM %u",
             (unsigned)(getReadAheadFill() / 1024), (unsigned)(getReadAheadCapacity() / 1024), (unsigned)getUnderruns(),
             (unsigned)getTotalUnderruns(),
             (audioMP3 && audioMP3->isRunning()) ? "running" : "idle",
             (audioOutput && audioOutput->installed()) ? "installed" : "released",
             digitalRead(AMP_MODE_PIN),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    uint8_t dacPower = readES8388Register(ES8388_DACPOWER);
    uint8_t dacControl3 = readES8388Register(ES8388_DACCONTROL3);
    uint8_t lout1 = readES8388Register(ES8388_LOUT1VOL);
    uint8_t lout2 = readES8388Register(ES8388_LOUT2VOL);
    ESP_LOGI(TAG, "ES8388 DACPOWER 0x%02X DACCONTROL3 0x%02X LOUT1VOL 0x%02X LOUT2VOL 0x%02X", dacPower, dacControl3, lout1, lout2);
}

// ---------------------------------------------------------------------------------------------
// ES8388 (Espressif ADF init order): outputs off while configuring, VMID enabled, DAC-only,
// DAC to both mixers at 0 dB, then LOUT1 + LOUT2/ROUT2 powered. Starts muted; the output path
// unmutes when something plays.
// ---------------------------------------------------------------------------------------------

bool AudioController::initializeES8388() {
    ESP_LOGI(TAG, "Initializing ES8388...");

    writeES8388Register(ES8388_CONTROL1, 0x80);   // reset
    delay(50);
    writeES8388Register(ES8388_CONTROL1, 0x00);
    delay(50);

    writeES8388Register(ES8388_DACCONTROL3, 0x04);  // DAC muted while configuring
    writeES8388Register(ES8388_CONTROL2, 0x50);     // analog power, VREF up
    writeES8388Register(ES8388_CHIPPOWER, 0x00);    // everything powered
    writeES8388Register(0x35, 0xA0);                // internal DLL off (ADF)
    writeES8388Register(0x37, 0xD0);
    writeES8388Register(0x39, 0xD0);
    writeES8388Register(ES8388_MASTERMODE, 0x00);   // slave, ESP32 drives the clocks
    writeES8388Register(ES8388_DACPOWER, ES8388_DACPOWER_OFF); // outputs off until configured
    writeES8388Register(ES8388_CONTROL1, 0x12);     // VMID 500k divider, DAC MCLK, play mode
    writeES8388Register(ES8388_DACCONTROL1, 0x18);  // I2S, 16-bit
    writeES8388Register(ES8388_DACCONTROL2, 0x02);  // single speed, 256 x fs
    writeES8388Register(ES8388_DACCONTROL16, 0x00);
    writeES8388Register(ES8388_DACCONTROL17, 0x90); // left DAC -> left mixer, 0 dB
    writeES8388Register(ES8388_DACCONTROL20, 0x90); // right DAC -> right mixer, 0 dB
    writeES8388Register(ES8388_DACCONTROL21, 0x80); // DAC and ADC share LRCK
    writeES8388Register(ES8388_DACCONTROL23, 0x00);
    writeES8388Register(ES8388_DACCONTROL4, 0x00);  // DAC digital volume 0 dB
    writeES8388Register(ES8388_DACCONTROL5, 0x00);
    writeES8388Register(ES8388_ADCPOWER, 0xFF);     // no ADC, no mic bias
    writeES8388Register(ES8388_ADCCONTROL1, 0x00);

    writeES8388Register(ES8388_LOUT1VOL, 0x00);     // outputs at minimum until applyCodecVolume()
    writeES8388Register(ES8388_ROUT1VOL, 0x00);
    writeES8388Register(ES8388_LOUT2VOL, 0x00);
    writeES8388Register(ES8388_ROUT2VOL, 0x00);

    writeES8388Register(ES8388_DACPOWER, ES8388_DACPOWER_ACTIVE); // DACs + LOUT1 + LOUT2/ROUT2
    codecMuted = true;
    writeES8388Register(ES8388_DACCONTROL3, ES8388_DAC_MUTE_RAMP);
    delay(20);

    uint8_t dacPower = readES8388Register(ES8388_DACPOWER);
    uint8_t dacControl3 = readES8388Register(ES8388_DACCONTROL3);
    uint8_t control1 = readES8388Register(ES8388_CONTROL1);
    ESP_LOGI(TAG, "ES8388 verification - CONTROL1: 0x%02X, DACPOWER: 0x%02X, DACCONTROL3: 0x%02X",
             control1, dacPower, dacControl3);

    if (dacPower != ES8388_DACPOWER_ACTIVE || dacControl3 == 0xFF) {
        ESP_LOGE(TAG, "WARNING - ES8388 register verification failed, I2C communication issues");
        return false;
    }

    ESP_LOGI(TAG, "ES8388 initialized and verified");
    return true;
}

bool AudioController::writeES8388Register(uint8_t reg, uint8_t value) {
    const int maxRetries = 3;
    const int retryDelay = 10;

    for (int attempt = 0; attempt < maxRetries; attempt++) {
        Wire.beginTransmission(ES8388_ADDR);
        Wire.write(reg);
        Wire.write(value);
        uint8_t result = Wire.endTransmission();

        if (result == 0) {
            return true;
        }

        if (attempt < maxRetries - 1) {
            delay(retryDelay);
        } else {
            ESP_LOGE(TAG, "ES8388 write failed (reg=0x%02X, val=0x%02X, error=%d)",
                     reg, value, result);
        }
    }

    return false;
}

uint8_t AudioController::readES8388Register(uint8_t reg) {
    const int maxRetries = 3;
    const int retryDelay = 10;

    for (int attempt = 0; attempt < maxRetries; attempt++) {
        Wire.beginTransmission(ES8388_ADDR);
        Wire.write(reg);
        uint8_t result = Wire.endTransmission(false);

        if (result == 0) {
            Wire.requestFrom(ES8388_ADDR, (uint8_t)1);
            if (Wire.available()) {
                return Wire.read();
            }
        }

        if (attempt < maxRetries - 1) {
            delay(retryDelay);
        }
    }

    return 0xFF; // Error value
}

void AudioController::muteES8388(bool mute) {
    if (mute == codecMuted) {
        return;
    }
    writeES8388Register(ES8388_DACCONTROL3, mute ? ES8388_DAC_MUTE_RAMP : ES8388_DAC_UNMUTE_RAMP);
    codecMuted = mute;
}

float AudioController::getCurrentTrackSeconds() const {
    if (!initialized || currentState == STOPPED || currentTrackPath.isEmpty()) {
        return 0.0f;
    }

    float totalTime = accumulatedPlayTime;

    if (currentState == PLAYING && trackStartTime > 0) {
        totalTime += (millis() - trackStartTime) / 1000.0f;
    }

    return totalTime;
}

bool AudioController::isValidAudioFile(const String& filePath) {
    String lowerPath = filePath;
    lowerPath.toLowerCase();

    return lowerPath.endsWith(".mp3");
}

// Any task (the audio task during playback). The UID is copied under the NFC controller's lock.
bool AudioController::isNfcSessionActive(const String& expectedUid) const {
    NfcController &nfc = NfcController::getInstance();
    if (!nfc.isCardPresent()) {
        ESP_LOGI(TAG, "no figure present");
        return false;
    }
    const String currentUid = nfc.currentUidCopy();
    if (currentUid != expectedUid) {
        ESP_LOGW(TAG, "figure UID mismatch (playlist %s, mounted %s)", expectedUid.c_str(), currentUid.c_str());
        return false;
    }
    return true;
}
