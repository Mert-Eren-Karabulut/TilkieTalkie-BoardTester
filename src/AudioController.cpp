#include "AudioController.h"
#include "ConfigManager.h"
#include "NfcController.h"
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

    Serial.println("AudioController: Beginning initialization...");

    if (!audioMutex) {
        audioMutex = xSemaphoreCreateRecursiveMutex();
        if (!audioMutex) {
            Serial.println("AudioController: Failed to create audio mutex");
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
            Serial.println("AudioController: Failed to configure I2S pins");
            delete audioOutput;
            audioOutput = nullptr;
            return false;
        }
        audioOutput->SetOutputModeMono(true); // speaker is on LOUT1 only: feed it the L+R mix
        audioOutput->SetGain(1.0f);           // attenuation is done in the codec, not in software
    }
    if (!audioOutput->installed() && !audioOutput->begin()) {
        Serial.println("AudioController: Failed to install the I2S driver");
        return false;
    }
    Serial.printf("AudioController: I2S up - BCLK:%d, LRCK:%d, DOUT:%d, MCLK:%d (16 DMA buffers, mono mix)\n",
                  I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_MCLK_PIN);

    if (!source) {
        source = new ReadAheadFileSource();
    }
    if (!source->begin()) {
        Serial.println("AudioController: Failed to start the read-ahead source");
        return false;
    }

    if (!mp3Prealloc) {
        mp3PreallocSize = AudioGeneratorMP3::preAllocSize();
        mp3Prealloc = heap_caps_malloc(mp3PreallocSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!mp3Prealloc) {
            Serial.println("AudioController: Failed to allocate the decoder workspace");
            return false;
        }
        Serial.printf("AudioController: decoder workspace %d bytes (internal RAM, allocated once)\n", mp3PreallocSize);
    }

    delay(50); // MCLK settles

    if (!initializeES8388()) {
        Serial.println("AudioController: Failed to initialize ES8388");
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
            Serial.println("AudioController: Failed to create background audio task, falling back to loop updates");
            audioTaskHandle = nullptr;
        } else {
            Serial.println("AudioController: Background audio task created on Core 1 (priority 12)");
        }
    }

    applyCodecVolume();
    Serial.printf("AudioController: Initialization complete - volume %d%% (ceiling %d%%)\n", currentVolume, volumeCeiling);
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
    Serial.println("AudioController: powered down");
}

bool AudioController::suspendBackgroundTaskForControl() {
    if (!audioMutex) {
        return false;
    }

    return xSemaphoreTakeRecursive(audioMutex, portMAX_DELAY) == pdTRUE;
}

void AudioController::resumeBackgroundTaskForControl() {
    if (!audioMutex) {
        return;
    }

    xSemaphoreGiveRecursive(audioMutex);
}

void AudioController::audioTaskEntry(void* parameter) {
    AudioController* self = static_cast<AudioController*>(parameter);

    while (true) {
        unsigned long sliceStart = millis();
        if (self && self->initialized) {
            bool locked = self->suspendBackgroundTaskForControl();
            if (locked) {
                self->updatePlaybackSlice();
                self->resumeBackgroundTaskForControl();
            }
        }
        unsigned long took = millis() - sliceStart;
        if (took > 500) {
            // A slice ran far longer than a few decode passes (SD stall, track change under
            // memory pressure): log it and give the lower-priority tasks on this core a breather
            // so the loop and the idle task are never starved.
            Serial.printf("AudioController: slice took %lu ms\n", took);
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
    Serial.printf("AudioController: amp %s\n", desired ? "released (on unless a plug is in)" : "off");
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
        Serial.printf("AudioController: headphone %s\n", plugged ? "plugged (speaker muted by the jack)" : "removed");
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
        Serial.printf("AudioController: Failed to open file: %s\n", filePath.c_str());
        return false;
    }

    audioMP3 = new AudioGeneratorMP3(mp3Prealloc, mp3PreallocSize);
    if (!audioMP3) {
        source->close();
        return false;
    }
    if (!audioMP3->begin(source, audioOutput)) {
        Serial.println("AudioController: Failed to start MP3 decoder");
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

bool AudioController::play(const String& filePath) {
    bool taskSuspended = suspendBackgroundTaskForControl();
    bool success = false;

    do {
        if (!initialized) {
            Serial.println("AudioController: Not initialized");
            break;
        }
        // Handle playlist case
        if (filePath.isEmpty()) {
            if (!hasPlaylist()) {
                Serial.println("AudioController: No playlist available");
                break;
            }

            if (!isNfcSessionActive(playlistFigureUid)) {
                Serial.println("AudioController: NFC session not active, clearing playlist");
                clearPlaylist();
                break;
            }

            if (playlistFinished || currentPlaylistIndex == -1) {
                currentPlaylistIndex = 0;
                playlistFinished = false;
            }

            if (currentPlaylistIndex >= (int)playlist.size()) {
                Serial.println("AudioController: Playlist index out of range");
                break;
            }

            String trackPath = playlist[currentPlaylistIndex];
            Serial.printf("AudioController: Playing track %d: %s\n", currentPlaylistIndex, trackPath.c_str());
            success = play(trackPath);
            break;
        }

        if (!fileManager.fileExists(filePath)) {
            Serial.println("AudioController: File does not exist");
            break;
        }

        if (!isValidAudioFile(filePath)) {
            Serial.println("AudioController: Invalid audio file");
            break;
        }

        if (currentState != STOPPED) {
            stop();
        }

        if (!startDecoder(filePath)) {
            break;
        }

        currentTrackPath = filePath;
        currentState = PLAYING;
        trackStartTime = millis();
        accumulatedPlayTime = 0.0f;
        pauseStartTime = 0;
        hasPausedTime = false;
        setOutputsActive(true);

        Serial.printf("AudioController: Successfully started playing: %s\n", filePath.c_str());
        success = true;
    } while (false);

    if (taskSuspended) {
        resumeBackgroundTaskForControl();
    }

    return success;
}

bool AudioController::pause() {
    bool taskSuspended = suspendBackgroundTaskForControl();
    bool success = false;

    if (initialized && currentState == PLAYING && audioMP3 && audioMP3->isRunning()) {
        float currentPlayTime = accumulatedPlayTime + ((millis() - trackStartTime) / 1000.0f);
        pausedTimeSeconds = currentPlayTime;
        hasPausedTime = true;
        accumulatedPlayTime = currentPlayTime;
        pauseStartTime = millis();

        // The decoder simply stops being serviced: nothing is torn down, resume continues
        // exactly where we were.
        currentState = PAUSED;
        setOutputsActive(false);
        Serial.printf("AudioController: Paused at %.2f seconds\n", pausedTimeSeconds);
        success = true;
    }

    if (taskSuspended) {
        resumeBackgroundTaskForControl();
    }

    return success;
}

bool AudioController::resume() {
    bool taskSuspended = suspendBackgroundTaskForControl();
    bool success = false;

    if (initialized && currentState == PAUSED && audioMP3 && audioMP3->isRunning()) {
        currentState = PLAYING;
        trackStartTime = millis();
        setOutputsActive(true);
        Serial.printf("AudioController: Resumed from %.2f seconds\n", accumulatedPlayTime);
        success = true;
    } else if (initialized && currentState == PAUSED) {
        // Decoder went away (should not happen); restart the track from the paused position.
        String path = currentTrackPath;
        float at = pausedTimeSeconds;
        currentState = STOPPED;
        if (!path.isEmpty() && play(path)) {
            if (at > 0.5f) seekTo(at);
            success = true;
        }
    }

    if (taskSuspended) {
        resumeBackgroundTaskForControl();
    }

    return success;
}

bool AudioController::stop() {
    bool taskSuspended = suspendBackgroundTaskForControl();
    bool success = false;

    if (initialized && currentState != STOPPED) {
        setOutputsActive(false);
        releaseDecoder();
        currentState = STOPPED;
        currentTrackPath = "";
        pausedTimeSeconds = 0.0f;
        hasPausedTime = false;
        trackStartTime = 0;
        accumulatedPlayTime = 0.0f;
        pauseStartTime = 0;
        success = true;
    }

    if (taskSuspended) {
        resumeBackgroundTaskForControl();
    }

    return success;
}

bool AudioController::seekTo(float seconds) {
    bool taskSuspended = suspendBackgroundTaskForControl();
    bool success = false;

    do {
        if (!initialized) {
            Serial.println("AudioController: Cannot seek - not initialized");
            break;
        }

        if (currentState == STOPPED || currentTrackPath.isEmpty() || !audioMP3 || !source->isOpen()) {
            Serial.println("AudioController: Cannot seek - no track loaded");
            break;
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
            Serial.println("AudioController: Seek failed");
            if (wasActive) muteES8388(currentVolume == 0);
            break;
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
        Serial.printf("AudioController: Seeked to %.2f s (byte %u of %u)\n", seconds, (unsigned)target, (unsigned)fileSize);
        success = true;
    } while (false);

    if (taskSuspended) {
        resumeBackgroundTaskForControl();
    }

    return success;
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
    bool taskSuspended = suspendBackgroundTaskForControl();
    bool changed = false;

    do {
        if (!initialized) {
            Serial.printf("AudioController: Cannot set volume - not initialized\n");
            break;
        }

        if (volume < MIN_VOLUME) volume = MIN_VOLUME;
        if (volume > volumeCeiling) volume = volumeCeiling;

        if (volume == currentVolume && !initialize) {
            break;
        }

        currentVolume = volume;
        applyCodecVolume();
        if (!initialize) {
            volumeDirty = true;
            volumeDirtyAt = millis();
        }
        Serial.printf("AudioController: Volume %d%%\n", volume);
        changed = true;
    } while (false);

    if (taskSuspended) {
        resumeBackgroundTaskForControl();
    }
    return changed;
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

    Serial.printf("AudioController: Volume ceiling set to %d%%\n", volumeCeiling);
}

int AudioController::getVolumeCeiling() const {
    return volumeCeiling;
}

// Playlist management methods
void AudioController::setPlaylist(const std::vector<String>& trackPaths, const String& figureUid) {
    bool taskSuspended = suspendBackgroundTaskForControl();
    playlist = trackPaths;
    playlistFigureUid = figureUid;
    currentPlaylistIndex = -1; // Start at -1, first play() will set to 0
    playlistFinished = false;

    Serial.printf("AudioController: Playlist set with %d tracks for figure UID: %s\n",
                 playlist.size(), figureUid.c_str());

    for (int i = 0; i < (int)playlist.size(); i++) {
        Serial.printf("  Track %d: %s\n", i + 1, playlist[i].c_str());
    }

    if (taskSuspended) {
        resumeBackgroundTaskForControl();
    }
}

void AudioController::mergePlaylist(const std::vector<String>& trackPaths, const String& figureUid) {
    bool taskSuspended = suspendBackgroundTaskForControl();
    if (figureUid != playlistFigureUid || playlist.empty()) {
        playlist = trackPaths;
        playlistFigureUid = figureUid;
        currentPlaylistIndex = -1;
        playlistFinished = false;
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
    Serial.printf("AudioController: playlist merged: %u tracks for %s (index %d)\n",
                  (unsigned)playlist.size(), figureUid.c_str(), currentPlaylistIndex);
    if (taskSuspended) {
        resumeBackgroundTaskForControl();
    }
}

void AudioController::clearPlaylist() {
    bool taskSuspended = suspendBackgroundTaskForControl();
    playlist.clear();
    currentPlaylistIndex = -1;
    playlistFigureUid = "";
    playlistFinished = false;

    Serial.println("AudioController: Playlist cleared");

    if (taskSuspended) {
        resumeBackgroundTaskForControl();
    }
}

bool AudioController::nextTrack() {
    if (!hasPlaylist()) {
        Serial.println("AudioController: No playlist available");
        return false;
    }

    if (!isNfcSessionActive(playlistFigureUid)) {
        Serial.println("AudioController: Figure not present or different figure detected, clearing playlist");
        clearPlaylist();
        return false;
    }

    if (playlistFinished) {
        currentPlaylistIndex = 0;
        playlistFinished = false;
        return play();
    }

    // Move to the next track that actually exists on the card (tracks still downloading or
    // permanently missing are skipped instead of ending the playlist).
    currentPlaylistIndex++;
    while (currentPlaylistIndex < (int)playlist.size() && !fileManager.fileExists(playlist[currentPlaylistIndex])) {
        Serial.printf("AudioController: skipping missing track %s\n", playlist[currentPlaylistIndex].c_str());
        currentPlaylistIndex++;
    }

    if (currentPlaylistIndex >= (int)playlist.size()) {
        Serial.println("AudioController: Reached end of playlist");
        playlistFinished = true;
        stop();
        return false;
    }

    return play();
}

bool AudioController::prevTrack() {
    if (!hasPlaylist()) {
        Serial.println("AudioController: No playlist available");
        return false;
    }

    if (!isNfcSessionActive(playlistFigureUid)) {
        Serial.println("AudioController: Figure not present or different figure detected, clearing playlist");
        clearPlaylist();
        return false;
    }

    // More than 3 s in: restart the current track.
    if ((currentState == PLAYING || currentState == PAUSED) && (getCurrentTrackSeconds() > 3.0f)) {
        Serial.println("AudioController: Restarting current track");
        String trackPath = currentTrackPath;
        if (trackPath.isEmpty()) {
            return false;
        }
        return play(trackPath);
    }

    if (playlistFinished || currentPlaylistIndex <= 0) {
        currentPlaylistIndex = playlist.size() - 1;
        playlistFinished = false;
        return play();
    }

    currentPlaylistIndex--;
    return play();
}

bool AudioController::playTrack(const String& trackId) {
    if (!hasPlaylist()) {
        Serial.println("AudioController: No playlist available");
        return false;
    }

    if (!isNfcSessionActive(playlistFigureUid)) {
        Serial.println("AudioController: Figure not present or different figure detected, clearing playlist");
        clearPlaylist();
        return false;
    }

    // Playlist paths end in /{trackId}.mp3
    String suffix = "/" + trackId + ".mp3";

    for (int i = 0; i < (int)playlist.size(); i++) {
        if (playlist[i].endsWith(suffix)) {
            Serial.printf("AudioController: Found track ID %s at playlist index %d\n", trackId.c_str(), i);
            currentPlaylistIndex = i;
            playlistFinished = false;
            return play();
        }
    }

    Serial.printf("AudioController: Track ID %s not found in current playlist\n", trackId.c_str());
    return false;
}

void AudioController::finishTrack() {
    const bool wasTransient = transientPlayback;
    stop();
    transientPlayback = false;
    if (!wasTransient && hasPlaylist() && !playlistFinished) {
        nextTrack();
    }
}

void AudioController::updatePlaybackSlice() {
    if (!initialized) {
        return;
    }

    if (currentState == PLAYING) {
        const int maxDecodePassesPerUpdate = 4;

        // Safety net only: the debounced detach (NfcController -> afterDetachNFC) is the real
        // stop path. A raw pin read here stopped playback on every wobble of the figure.
        if (hasPlaylist() && !NfcController::getInstance().isCardPresent()) {
            Serial.println("AudioController: figure no longer present during playback");
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
            Serial.println("AudioController: Playback stopped unexpectedly");
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
    pollHeadphone();
    if (!audioTaskHandle) {
        bool locked = suspendBackgroundTaskForControl();
        updatePlaybackSlice();
        if (locked) resumeBackgroundTaskForControl();
    }
}

void AudioController::volumeBeep() {
    if (!initialized || currentState == PLAYING) {
        return; // never interrupt a track
    }

    const String beepPath = "/sounds/beep.mp3";
    if (!fileManager.fileExists(beepPath)) {
        return;
    }

    transientPlayback = true; // the audio task must not advance the playlist when it ends
    if (play(beepPath)) {
        unsigned long start = millis();
        while (currentState == PLAYING && millis() - start < 3000) {
            update();
            delay(10);
        }
    }
    transientPlayback = false;
}

void AudioController::printStatus() {
    const char* stateName = currentState == PLAYING ? "PLAYING" : currentState == PAUSED ? "PAUSED" : "STOPPED";
    Serial.printf("Audio: %s %s @ %.1fs | volume %d%% (ceiling %d%%) codec %s | amp %s (mode %s)%s | playlist %d/%d%s\n",
                  stateName, currentTrackPath.isEmpty() ? "-" : currentTrackPath.c_str(), getCurrentTrackSeconds(),
                  currentVolume, volumeCeiling, codecMuted ? "muted" : "live",
                  ampEnabled ? "released" : "off",
                  ampMode == AMP_AUTO ? "auto" : ampMode == AMP_FORCE_ON ? "on" : "off",
                  headphonePlugged ? " headphone plugged" : "",
                  currentPlaylistIndex + 1, (int)playlist.size(), playlistFinished ? " (finished)" : "");
    Serial.printf("Audio: read-ahead %u/%u KB, underruns %u, decoder %s, I2S %s, AMP_CTRL level %d | heap int %u PSRAM %u\n",
                  (unsigned)(getReadAheadFill() / 1024), (unsigned)(getReadAheadCapacity() / 1024), (unsigned)getUnderruns(),
                  (audioMP3 && audioMP3->isRunning()) ? "running" : "idle",
                  (audioOutput && audioOutput->installed()) ? "installed" : "released",
                  digitalRead(AMP_MODE_PIN),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    uint8_t dacPower = readES8388Register(ES8388_DACPOWER);
    uint8_t dacControl3 = readES8388Register(ES8388_DACCONTROL3);
    uint8_t lout1 = readES8388Register(ES8388_LOUT1VOL);
    uint8_t lout2 = readES8388Register(ES8388_LOUT2VOL);
    Serial.printf("Audio: ES8388 DACPOWER 0x%02X DACCONTROL3 0x%02X LOUT1VOL 0x%02X LOUT2VOL 0x%02X\n", dacPower, dacControl3, lout1, lout2);
}

// ---------------------------------------------------------------------------------------------
// ES8388 (Espressif ADF init order): outputs off while configuring, VMID enabled, DAC-only,
// DAC to both mixers at 0 dB, then LOUT1 + LOUT2/ROUT2 powered. Starts muted; the output path
// unmutes when something plays.
// ---------------------------------------------------------------------------------------------

bool AudioController::initializeES8388() {
    Serial.println("AudioController: Initializing ES8388...");

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
    Serial.printf("AudioController: ES8388 verification - CONTROL1: 0x%02X, DACPOWER: 0x%02X, DACCONTROL3: 0x%02X\n",
                  control1, dacPower, dacControl3);

    if (dacPower != ES8388_DACPOWER_ACTIVE || dacControl3 == 0xFF) {
        Serial.println("AudioController: WARNING - ES8388 register verification failed, I2C communication issues");
        return false;
    }

    Serial.println(F("AudioController: ES8388 initialized and verified"));
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
            Serial.printf("AudioController: ES8388 write failed (reg=0x%02X, val=0x%02X, error=%d)\n",
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

bool AudioController::isNfcSessionActive(const String& expectedUid) const {
    NfcController &nfc = NfcController::getInstance();
    if (!nfc.isCardPresent()) {
        Serial.println("AudioController: no figure present");
        return false;
    }
    String currentUid = nfc.currentNFCData().uidString;
    if (currentUid != expectedUid) {
        Serial.printf("AudioController: figure UID mismatch (playlist %s, mounted %s)\n",
                      expectedUid.c_str(), currentUid.c_str());
        return false;
    }
    return true;
}
