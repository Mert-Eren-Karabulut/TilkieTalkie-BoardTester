#include "AudioController.h"
#include "ConfigManager.h"
#include "NfcController.h"
#include "AudioFileSourceFS.h"
#include <SD_MMC.h>
    
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// Initialize static members
AudioController* AudioController::instance = nullptr;

// Define static constants
const int AudioController::MIN_VOLUME;
const int AudioController::MAX_VOLUME;
const int AudioController::DEFAULT_VOLUME;
const int AudioController::VOLUME_STEP;

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
#define ES8388_ADCCONTROL2      0x0A
#define ES8388_ADCCONTROL3      0x0B
#define ES8388_ADCCONTROL4      0x0C
#define ES8388_ADCCONTROL5      0x0D
#define ES8388_ADCCONTROL6      0x0E
#define ES8388_ADCCONTROL7      0x0F
#define ES8388_ADCCONTROL8      0x10
#define ES8388_ADCCONTROL9      0x11
#define ES8388_ADCCONTROL10     0x12
#define ES8388_ADCCONTROL11     0x13
#define ES8388_ADCCONTROL12     0x14
#define ES8388_ADCCONTROL13     0x15
#define ES8388_ADCCONTROL14     0x16
#define ES8388_DACCONTROL1      0x17
#define ES8388_DACCONTROL2      0x18
#define ES8388_DACCONTROL3      0x19
#define ES8388_DACCONTROL4      0x1A
#define ES8388_DACCONTROL5      0x1B
#define ES8388_DACCONTROL6      0x1C
#define ES8388_DACCONTROL7      0x1D
#define ES8388_DACCONTROL8      0x1E
#define ES8388_DACCONTROL9      0x1F
#define ES8388_DACCONTROL10     0x20
#define ES8388_DACCONTROL11     0x21
#define ES8388_DACCONTROL12     0x22
#define ES8388_DACCONTROL13     0x23
#define ES8388_DACCONTROL14     0x24
#define ES8388_DACCONTROL15     0x25
#define ES8388_DACCONTROL16     0x26
#define ES8388_DACCONTROL17     0x27
#define ES8388_DACCONTROL18     0x28
#define ES8388_DACCONTROL19     0x29
#define ES8388_DACCONTROL20     0x2A
#define ES8388_DACCONTROL21     0x2B
#define ES8388_DACCONTROL22     0x2C
#define ES8388_DACCONTROL23     0x2D
#define ES8388_LOUT1VOL         0x2E  // ES8388_DACCONTROL24
#define ES8388_ROUT1VOL         0x2F  // ES8388_DACCONTROL25
#define ES8388_LOUT2VOL         0x30
#define ES8388_ROUT2VOL         0x31

AudioController::AudioController() :
    audioFile(nullptr),
    audioBuffer(nullptr),
    audioMP3(nullptr),
    audioOutput(nullptr),
    currentState(STOPPED),
    currentTrackPath(""),
    currentVolume(DEFAULT_VOLUME),
    initialized(false),
    pausedTimeSeconds(0.0f),
    hasPausedTime(false),
    trackStartTime(0),
    accumulatedPlayTime(0.0f),
    pauseStartTime(0),
    currentPlaylistIndex(-1),
    playlistFigureUid(""),
    playlistFinished(false),
    fileManager(FileManager::getInstance()) {
    
    // Load volume ceiling from NVS, default to MAX_VOLUME if not set
    ConfigManager& config = ConfigManager::getInstance();
    volumeCeiling = config.getInt("volume_ceiling", MAX_VOLUME);
    
    // Ensure the volume ceiling is within valid range
    volumeCeiling = min(volumeCeiling, MAX_VOLUME);
    volumeCeiling = max(volumeCeiling, MIN_VOLUME);
    
    // Load saved volume from NVS, default to DEFAULT_VOLUME if not set
    currentVolume = config.getInt("audio_volume", DEFAULT_VOLUME);
    
    // Ensure saved volume is within valid range and ceiling
    currentVolume = min(currentVolume, volumeCeiling);
    currentVolume = max(currentVolume, MIN_VOLUME);
}

AudioController::~AudioController() {
    end();
    cleanupAudioComponents();
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
    
    // Initialize I2C for ES8388 control
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);
    
    // Initialize mute pin (mute during initialization)
    pinMode(MUTE_PIN, OUTPUT);
    digitalWrite(MUTE_PIN, HIGH);
    
    // CRITICAL: Initialize I2S and audio components FIRST
    // This starts the MCLK signal that the ES8388 needs
    if (!initializeI2S()) {
        Serial.println("AudioController: Failed to initialize I2S");
        return false;
    }

    if (!initializeAudioComponents()) {
        Serial.println("AudioController: Failed to initialize audio components");
        return false;
    }
    
    // Small delay to let MCLK stabilize before configuring ES8388
    delay(100);
    
    // Now initialize ES8388 (it needs MCLK running)
    if (!initializeES8388()) {
        Serial.println("AudioController: Failed to initialize ES8388");
        return false;
    }
    
    initialized = true;

    // Set volume and unmute
    setVolume(currentVolume, true);
    delay(50); // Let volume setting propagate
    digitalWrite(MUTE_PIN, LOW);
    
    Serial.println("AudioController: Initialization complete - ready for playback");
    
    return true;
}

void AudioController::end() {
    if (!initialized) {
        return;
    }

    // Stop any current playback
    stop();

    // Mute
    digitalWrite(MUTE_PIN, HIGH);

    // Clean up audio components
    cleanupAudioComponents();

    // Clean up audio output
    if (audioOutput) {
        delete audioOutput;
        audioOutput = nullptr;
    }

    initialized = false;
}

bool AudioController::play(const String& filePath) {    
    if (!initialized) {
        Serial.println("AudioController: Not initialized");
        return false;
    }
    
    // Handle playlist case
    if (filePath.isEmpty()) {
        if (!hasPlaylist()) {
            Serial.println("AudioController: No playlist available");
            return false;
        }
        
        // Check if NFC session is still active
        if (!isNfcSessionActive(playlistFigureUid)) {
            Serial.println("AudioController: NFC session not active, clearing playlist");
            clearPlaylist();
            return false;
        }
        
        // Handle playlist navigation
        if (playlistFinished || currentPlaylistIndex == -1) {
            currentPlaylistIndex = 0;
            playlistFinished = false;
        }
        
        if (currentPlaylistIndex >= playlist.size()) {
            Serial.println("AudioController: Playlist index out of range");
            return false;
        }
        
        String trackPath = playlist[currentPlaylistIndex];
        Serial.printf("AudioController: Playing track %d: %s\n", currentPlaylistIndex, trackPath.c_str());
        return play(trackPath); // Recursive call with specific file
    }

    // Check if file exists
    if (!fileManager.fileExists(filePath)) {
        Serial.println("AudioController: File does not exist");
        return false;
    }

    // Check if it's a valid audio file
    if (!isValidAudioFile(filePath)) {
        Serial.println("AudioController: Invalid audio file");
        return false;
    }

    // Stop current playback
    if (currentState != STOPPED) {
        stop();
        delay(10);
    }

    // Clean up previous components following the working example pattern
    cleanupAudioComponents();
    
    // Create new components using AudioFileSourceFS with SD_MMC filesystem
    // Pass &SD_MMC as the filesystem reference, then open the file
    audioFile = new AudioFileSourceFS(SD_MMC);
    if (!audioFile) {
        Serial.println("AudioController: Failed to create AudioFileSourceFS");
        return false;
    }
    
    // Open the file on the SD_MMC filesystem
    if (!audioFile->open(filePath.c_str())) {
        Serial.printf("AudioController: Failed to open file: %s\n", filePath.c_str());
        delete audioFile;
        audioFile = nullptr;
        return false;
    }
    
    // Larger buffer (8KB) for faster MP3 frame sync and reduced I/O during initialization
    uint32_t spiBufferSize = 8192;
    byte *spiBuffer = (byte *)ps_malloc(spiBufferSize);
    if (!spiBuffer) {
        Serial.println("AudioController: Failed to allocate PSRAM buffer");
        delete audioFile;
        audioFile = nullptr;
        return false;
    }
    audioBuffer = new AudioFileSourceBuffer(audioFile, spiBuffer, spiBufferSize);
    if (!audioBuffer) {
        Serial.println("AudioController: Failed to create AudioFileSourceBuffer");
        free(spiBuffer);
        delete audioFile;
        audioFile = nullptr;
        return false;
    }
    
    audioMP3 = new AudioGeneratorMP3();
    if (!audioMP3) {
        Serial.println("AudioController: Failed to create AudioGeneratorMP3");
        cleanupAudioComponents();
        return false;
    }

    // Start playback (following working example)
    Serial.println("AudioController: Starting MP3 playback");
    if (!audioMP3->begin(audioBuffer, audioOutput)) {
        Serial.println("AudioController: Failed to start MP3 playback");
        cleanupAudioComponents();
        return false;
    }

    // Set current track info
    currentTrackPath = filePath;
    currentState = PLAYING;
    trackStartTime = millis();
    accumulatedPlayTime = 0.0f;
    pauseStartTime = 0;
    
    Serial.printf("AudioController: Successfully started playing: %s\n", filePath.c_str());
    return true;
}

bool AudioController::pause() {
    if (!initialized || currentState != PLAYING) {
        return false;
    }

    if (audioMP3 && audioMP3->isRunning()) {
        // Calculate and save current playback time in seconds
        if (trackStartTime > 0) {
            float currentPlayTime = accumulatedPlayTime + ((millis() - trackStartTime) / 1000.0f);
            pausedTimeSeconds = currentPlayTime;
            hasPausedTime = true;
            
            Serial.printf("AudioController: Paused at %.2f seconds\n", pausedTimeSeconds);
            
            // Update accumulated time
            accumulatedPlayTime = currentPlayTime;
            pauseStartTime = millis();
        }
        
        // Stop the audio playback
        audioMP3->stop();
        currentState = PAUSED;
        return true;
    }

    return false;
}

bool AudioController::resume() {
    if (!initialized || currentState != PAUSED) {
        return false;
    }

    if (!currentTrackPath.isEmpty()) {
        // Clean up current audio components
        cleanupAudioComponents();
        
        // Create fresh MP3 generator for resume
        audioMP3 = new AudioGeneratorMP3();
        if (!audioMP3) {
            Serial.println("AudioController: Failed to create MP3 generator for resume");
            currentState = STOPPED;
            currentTrackPath = "";
            hasPausedTime = false;
            return false;
        }
        
        // Create new audio file source with SD_MMC filesystem
        audioFile = new AudioFileSourceFS(SD_MMC);
        if (!audioFile) {
            Serial.printf("AudioController: Failed to create AudioFileSourceFS for resume\n");
            cleanupAudioComponents();
            currentState = STOPPED;
            currentTrackPath = "";
            hasPausedTime = false;
            return false;
        }
        
        // Open the file on the SD_MMC filesystem
        if (!audioFile->open(currentTrackPath.c_str())) {
            Serial.printf("AudioController: Failed to open audio file for resume: %s\n", currentTrackPath.c_str());
            delete audioFile;
            audioFile = nullptr;
            delete audioMP3;
            audioMP3 = nullptr;
            currentState = STOPPED;
            currentTrackPath = "";
            hasPausedTime = false;
            return false;
        }

        // Create buffer - larger buffer for faster decoding during fast-forward
        uint32_t resumeBufferSize = 8192;
        byte *resumeBuffer = (byte *)ps_malloc(resumeBufferSize);
        if (!resumeBuffer) {
            Serial.printf("AudioController: Failed to allocate PSRAM buffer for resume\n");
            cleanupAudioComponents();
            currentState = STOPPED;
            currentTrackPath = "";
            hasPausedTime = false;
            return false;
        }
        audioBuffer = new AudioFileSourceBuffer(audioFile, resumeBuffer, resumeBufferSize);
        if (!audioBuffer) {
            Serial.printf("AudioController: Failed to create audio buffer\n");
            free(resumeBuffer);
            cleanupAudioComponents();
            currentState = STOPPED;
            currentTrackPath = "";
            hasPausedTime = false;
            return false;
        }

        // Start playback from the beginning
        if (!audioMP3->begin(audioBuffer, audioOutput)) {
            Serial.printf("AudioController: Failed to start MP3 playback\n");
            cleanupAudioComponents();
            currentState = STOPPED;
            currentTrackPath = "";
            hasPausedTime = false;
            return false;
        }

        // If we have a paused time, fast-forward through the decoder to reach it
        // Since all MP3s are CBR at 96kbps (from ffmpeg), we can calculate precise byte position
        if (hasPausedTime && pausedTimeSeconds > 0.1f) {
            Serial.printf("AudioController: Fast-forwarding to %.2f seconds...\n", pausedTimeSeconds);
            
            // For CBR MP3 at 96kbps (12,000 bytes/sec), calculate target byte position
            // 96kbps = 96000 bits/sec = 12000 bytes/sec
            const uint32_t BYTES_PER_SECOND = 12000; // 96kbps / 8
            
            // Calculate target byte position
            uint32_t targetBytePosition = (uint32_t)(pausedTimeSeconds * BYTES_PER_SECOND);
            
            // Add a small offset to account for MP3 headers/metadata at the start
            // Typical MP3 has ~417 bytes of ID3v2 header, but we'll be conservative
            const uint32_t HEADER_OFFSET = 512;
            targetBytePosition += HEADER_OFFSET;
            
            // Ensure we don't seek past the file
            uint32_t fileSize = audioFile->getSize();
            if (targetBytePosition > fileSize) {
                targetBytePosition = fileSize - 1024; // Back off a bit from the end
            }
            
            Serial.printf("AudioController: Calculated byte position: %u (file size: %u)\n", 
                         targetBytePosition, fileSize);
            
            // Temporarily mute during seeking to avoid audio glitches
            float savedGain = currentVolume / 100.0f;
            if (audioOutput) {
                audioOutput->SetGain(0.0f);
            }
            
            // Seek to the calculated position
            if (audioFile->seek(targetBytePosition, SEEK_SET)) {
                Serial.printf("AudioController: Seeked to byte %u\n", targetBytePosition);
                
                // Now do a short fast-forward (just a few frames) to sync to the next valid MP3 frame
                // This is much faster than fast-forwarding from the beginning
                unsigned long startTime = millis();
                int loopCount = 0;
                const int MAX_SYNC_LOOPS = 50; // Should only need a few loops to find frame sync
                
                while (audioMP3->isRunning() && loopCount < MAX_SYNC_LOOPS) {
                    if (!audioMP3->loop()) {
                        // Reached end of file
                        Serial.println("AudioController: Reached EOF during frame sync");
                        break;
                    }
                    loopCount++;
                    
                    // Small yield every few iterations
                    if ((loopCount % 5) == 0) {
                        yield();
                    }
                }
                
                unsigned long syncTime = millis() - startTime;
                Serial.printf("AudioController: Frame sync complete after %d loops (%lu ms)\n", 
                             loopCount, syncTime);
            } else {
                Serial.printf("AudioController: Seek failed, starting from beginning\n");
            }
            
            // Restore volume
            if (audioOutput) {
                audioOutput->SetGain(savedGain);
            }
            
            hasPausedTime = false;
        }
        
        currentState = PLAYING;
        
        // Reset timing - we're starting fresh from the paused position
        trackStartTime = millis();
        accumulatedPlayTime = pausedTimeSeconds; // Keep the accumulated time
        
        Serial.printf("AudioController: Resumed successfully from %.2f seconds\n", pausedTimeSeconds);
        return true;
    }

    return false;
}

bool AudioController::stop() {
    if (!initialized || currentState == STOPPED) {
        return false;
    }

    if (audioMP3 && audioMP3->isRunning()) {
        audioMP3->stop();
        delay(10);
    }

    cleanupAudioComponents();
    
    currentState = STOPPED;
    currentTrackPath = "";
    pausedTimeSeconds = 0.0f;
    hasPausedTime = false;
    trackStartTime = 0;
    accumulatedPlayTime = 0.0f;
    pauseStartTime = 0;
    
    return true;
}

bool AudioController::seekTo(float seconds) {
    if (!initialized) {
        Serial.println("AudioController: Cannot seek - not initialized");
        return false;
    }

    // Can only seek if we have a track loaded (playing or paused)
    if (currentState == STOPPED || currentTrackPath.isEmpty()) {
        Serial.println("AudioController: Cannot seek - no track loaded");
        return false;
    }

    // Validate seek position
    if (seconds < 0) {
        Serial.println("AudioController: Invalid seek position - negative time");
        return false;
    }

    Serial.printf("AudioController: Seeking to %.2f seconds\n", seconds);

    // Remember the current state so we can restore it after seeking
    AudioState previousState = currentState;
    String trackPath = currentTrackPath;

    // Stop current playback
    if (audioMP3 && audioMP3->isRunning()) {
        audioMP3->stop();
        delay(10);
    }

    // Clean up audio components
    cleanupAudioComponents();

    // Create fresh MP3 generator
    audioMP3 = new AudioGeneratorMP3();
    if (!audioMP3) {
        Serial.println("AudioController: Failed to create MP3 generator for seek");
        currentState = STOPPED;
        currentTrackPath = "";
        return false;
    }

    // Create new audio file source
    audioFile = new AudioFileSourceFS(SD_MMC);
    if (!audioFile) {
        Serial.println("AudioController: Failed to create AudioFileSourceFS for seek");
        cleanupAudioComponents();
        currentState = STOPPED;
        currentTrackPath = "";
        return false;
    }

    // Open the file
    if (!audioFile->open(trackPath.c_str())) {
        Serial.printf("AudioController: Failed to open audio file for seek: %s\n", trackPath.c_str());
        cleanupAudioComponents();
        currentState = STOPPED;
        currentTrackPath = "";
        return false;
    }

    // Create buffer
    uint32_t seekBufferSize = 8192;
    byte *seekBuffer = (byte *)ps_malloc(seekBufferSize);
    if (!seekBuffer) {
        Serial.println("AudioController: Failed to allocate PSRAM buffer for seek");
        cleanupAudioComponents();
        currentState = STOPPED;
        currentTrackPath = "";
        return false;
    }
    audioBuffer = new AudioFileSourceBuffer(audioFile, seekBuffer, seekBufferSize);
    if (!audioBuffer) {
        Serial.println("AudioController: Failed to create audio buffer for seek");
        free(seekBuffer);
        cleanupAudioComponents();
        currentState = STOPPED;
        currentTrackPath = "";
        return false;
    }

    // Start playback from the beginning
    if (!audioMP3->begin(audioBuffer, audioOutput)) {
        Serial.println("AudioController: Failed to start MP3 playback for seek");
        cleanupAudioComponents();
        currentState = STOPPED;
        currentTrackPath = "";
        return false;
    }

    // Seek to the target position using CBR calculation
    if (seconds > 0.1f) {
        // For CBR MP3 at 96kbps (12,000 bytes/sec), calculate target byte position
        const uint32_t BYTES_PER_SECOND = 12000; // 96kbps / 8
        uint32_t targetBytePosition = (uint32_t)(seconds * BYTES_PER_SECOND);
        
        // Add header offset
        const uint32_t HEADER_OFFSET = 512;
        targetBytePosition += HEADER_OFFSET;
        
        // Ensure we don't seek past the file
        uint32_t fileSize = audioFile->getSize();
        if (targetBytePosition > fileSize) {
            targetBytePosition = fileSize - 1024;
        }
        
        Serial.printf("AudioController: Seeking to byte position: %u\n", targetBytePosition);
        
        // Temporarily mute during seeking
        float savedGain = currentVolume / 100.0f;
        if (audioOutput) {
            audioOutput->SetGain(0.0f);
        }
        
        // Seek to the calculated position
        if (audioFile->seek(targetBytePosition, SEEK_SET)) {
            // Do a short fast-forward to sync to the next valid MP3 frame
            int loopCount = 0;
            const int MAX_SYNC_LOOPS = 50;
            
            while (audioMP3->isRunning() && loopCount < MAX_SYNC_LOOPS) {
                if (!audioMP3->loop()) {
                    Serial.println("AudioController: Reached EOF during seek frame sync");
                    break;
                }
                loopCount++;
                if ((loopCount % 5) == 0) {
                    yield();
                }
            }
            
            Serial.printf("AudioController: Seek frame sync complete after %d loops\n", loopCount);
        } else {
            Serial.println("AudioController: Seek failed");
        }
        
        // Restore volume
        if (audioOutput) {
            audioOutput->SetGain(savedGain);
        }
    }

    // Restore the track path
    currentTrackPath = trackPath;

    // Restore the appropriate state
    if (previousState == PLAYING) {
        // Continue playing from the new position
        currentState = PLAYING;
        trackStartTime = millis();
        accumulatedPlayTime = seconds; // Start accumulating from the seek position
        Serial.printf("AudioController: Seek complete, resuming playback from %.2f seconds\n", seconds);
    } else if (previousState == PAUSED) {
        // Pause at the new position
        audioMP3->stop();
        currentState = PAUSED;
        pausedTimeSeconds = seconds;
        hasPausedTime = true;
        accumulatedPlayTime = seconds;
        Serial.printf("AudioController: Seek complete, paused at %.2f seconds\n", seconds);
    }

    return true;
}

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
    if (!initialized) {
        Serial.printf("AudioController: Cannot set volume - not initialized\n");
        return false;
    }

    // Clamp volume to valid range and volume ceiling
    if (volume < MIN_VOLUME) volume = MIN_VOLUME;
    if (volume > volumeCeiling) volume = volumeCeiling;

    if (volume == currentVolume && !initialize) {
        Serial.printf("AudioController: Volume already at %d%%, no change needed\n", volume);
        return false;
    }

    Serial.printf("AudioController: Setting volume from %d%% to %d%%\n", currentVolume, volume);
    currentVolume = volume;
    
    // Save volume to NVS for persistence across sleep/wake
    if (!initialize) {
        ConfigManager& config = ConfigManager::getInstance();
        config.storeInt("audio_volume", currentVolume);
    }

    // Set audio output gain
    if (audioOutput) {
        audioOutput->SetGain(volume / 100.0f);
        Serial.printf("AudioController: Set AudioOutput gain to %.2f\n", volume / 100.0f);
    }

    // Set ES8388 volume
    setES8388Volume(volume);

    Serial.printf("AudioController: Volume set to %d%%\n", volume);
    return true;
}

// Volume ceiling control methods
void AudioController::setVolumeCeiling(int ceiling) {
    // Clamp ceiling to valid range
    ceiling = min(ceiling, MAX_VOLUME);
    ceiling = max(ceiling, MIN_VOLUME);
    
    // Store the new ceiling
    volumeCeiling = ceiling;
    
    // Store in NVS
    ConfigManager& config = ConfigManager::getInstance();
    config.storeInt("volume_ceiling", volumeCeiling);
    
    // If current volume is higher than new ceiling, lower it
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
    playlist = trackPaths;
    playlistFigureUid = figureUid;
    currentPlaylistIndex = -1; // Start at -1, first play() will set to 0
    playlistFinished = false;
    
    Serial.printf("AudioController: Playlist set with %d tracks for figure UID: %s\n", 
                 playlist.size(), figureUid.c_str());
    
    // Print playlist for debugging
    for (int i = 0; i < playlist.size(); i++) {
        Serial.printf("  Track %d: %s\n", i + 1, playlist[i].c_str());
    }
}

void AudioController::clearPlaylist() {
    playlist.clear();
    currentPlaylistIndex = -1;
    playlistFigureUid = "";
    playlistFinished = false;
    
    Serial.println("AudioController: Playlist cleared");
}

bool AudioController::nextTrack() {
    if (!hasPlaylist()) {
        Serial.println("AudioController: No playlist available");
        return false;
    }
    
    // Check if NFC session is still active for the figure associated with this playlist
    if (!isNfcSessionActive(playlistFigureUid)) {
        Serial.println("AudioController: Figure not present or different figure detected, clearing playlist");
        clearPlaylist();
        return false;
    }
    
    // If playlist finished, start from beginning
    if (playlistFinished) {
        currentPlaylistIndex = 0;
        playlistFinished = false;
        return play(); // Call play() without parameters to use playlist
    }
    
    // Move to next track
    currentPlaylistIndex++;
    
    // Check if we've reached the end
    if (currentPlaylistIndex >= playlist.size()) {
        Serial.println("AudioController: Reached end of playlist");
        playlistFinished = true;
        stop();
        return false;
    }
    
    // Play the next track
    return play(); // Call play() without parameters to use playlist
}

bool AudioController::prevTrack() {
    if (!hasPlaylist()) {
        Serial.println("AudioController: No playlist available");
        return false;
    }
    
    // Check if NFC session is still active for the figure associated with this playlist
    if (!isNfcSessionActive(playlistFigureUid)) {
        Serial.println("AudioController: Figure not present or different figure detected, clearing playlist");
        clearPlaylist();
        return false;
    }

    //if a track is currently playing or paused and has played for more than 3 seconds, restart it
    if ((currentState == PLAYING || currentState == PAUSED) && (getCurrentTrackSeconds() > 3.0f)) {
        Serial.println("AudioController: Restarting current track");
        //seek to beginning
        seekTo(0.0f);
        //if paused, start playing
        if (currentState == PAUSED) {
            resume();
        }
        return true; // Don't fall through to previous track navigation
    }
    
    // If playlist finished or at first track, go to last track
    if (playlistFinished || currentPlaylistIndex <= 0) {
        currentPlaylistIndex = playlist.size() - 1;
        playlistFinished = false;
        return play(); // Call play() without parameters to use playlist
    }
    
    // Move to previous track
    currentPlaylistIndex--;
    return play(); // Call play() without parameters to use playlist
}

bool AudioController::playTrack(const String& trackId) {
    if (!hasPlaylist()) {
        Serial.println("AudioController: No playlist available");
        return false;
    }
    
    // Check if NFC session is still active
    if (!isNfcSessionActive(playlistFigureUid)) {
        Serial.println("AudioController: Figure not present or different figure detected, clearing playlist");
        clearPlaylist();
        return false;
    }
    
    // Search for the track ID in the playlist
    // Playlist paths follow the format: /figures/{figureId}/{episodeId}/{trackId}.mp3
    String suffix = "/" + trackId + ".mp3";
    
    for (int i = 0; i < playlist.size(); i++) {
        if (playlist[i].endsWith(suffix)) {
            Serial.printf("AudioController: Found track ID %s at playlist index %d\n", trackId.c_str(), i);
            currentPlaylistIndex = i;
            playlistFinished = false;
            return play(); // Play the matched track
        }
    }
    
    Serial.printf("AudioController: Track ID %s not found in current playlist\n", trackId.c_str());
    return false;
}

void AudioController::update() {
    if (!initialized) {
        return;
    }

    // Update audio processing only if playing
    if (currentState == PLAYING) {
        if (audioMP3 && audioMP3->isRunning()) {
            if (!audioMP3->loop()) {
                // Track finished
                stop();
                
                // If we have a playlist, automatically go to next track
                if (hasPlaylist() && !playlistFinished) {
                    nextTrack();
                }
            }
        } else {
            // Should be playing but isn't - something went wrong
            Serial.println("AudioController: Playback stopped unexpectedly");
            stop();
        }
    }
    // For PAUSED state, we don't call audioMP3->loop() so playback remains stopped
    // For STOPPED state, there's nothing to update
}

void AudioController::volumeBeep() {
    if (!initialized || currentState == PLAYING) {
        // Don't beep if a track is already playing to avoid interruption.
        return;
    }
    
    // Play the beep.mp3 file instead of generating a tone
    const String beepPath = "/sounds/beep.mp3";
    
    // Check if beep file exists
    if (!fileManager.fileExists(beepPath)) {
        return;
    }
    
    // Store current state to restore after beep
    String previousTrackPath = currentTrackPath;
    AudioState previousState = currentState;
    
    // Play the beep file
    if (play(beepPath)) {
        // Wait for the beep to finish playing
        while (currentState == PLAYING) {
            update();
            delay(10); // Small delay to prevent busy waiting
        }
        
        // Restore previous state if there was a track playing
        if (previousState == PLAYING && !previousTrackPath.isEmpty()) {
            play(previousTrackPath);
        } else if (previousState == PAUSED && !previousTrackPath.isEmpty()) {
            play(previousTrackPath);
            pause();
        }
    }
}

bool AudioController::initializeES8388() {
    Serial.println("AudioController: Initializing ES8388...");
    
    // Reset ES8388 to default values
    writeES8388Register(ES8388_CONTROL1, 0x80);
    delay(50);
    writeES8388Register(ES8388_CONTROL1, 0x00);
    delay(50);

    // --- Power Management ---
    // Power up analog and bias generation
    writeES8388Register(ES8388_CONTROL2, 0x40);  // Power up analog, disable low power modes
    writeES8388Register(ES8388_CONTROL1, 0x04);  // Enable reference circuits
    writeES8388Register(ES8388_CHIPPOWER, 0x00); // Power up digital blocks
    
    // **NEW**: Explicitly power down the entire ADC path to reduce noise
    writeES8388Register(ES8388_ADCPOWER, 0xFF);  // Power down ADC, Mic Bias, and analog inputs 
    
    // --- Clocking and Format ---
    writeES8388Register(ES8388_MASTERMODE, 0x00);   // Set to Slave mode (ESP32-S3 is master)
    writeES8388Register(ES8388_ADCCONTROL4, 0x0C);  // Set ADC to I2S, 16-bit
    writeES8388Register(ES8388_DACCONTROL1, 0x18);  // Set DAC to I2S, 16-bit

    // --- Clocking and Format ---
    writeES8388Register(ES8388_MASTERMODE, 0x00);   // Set to Slave mode (ESP32-S3 is master)
    writeES8388Register(ES8388_ADCCONTROL4, 0x0C);  // Set ADC to I2S, 16-bit
    writeES8388Register(ES8388_DACCONTROL1, 0x18);  // Set DAC to I2S, 16-bit

    // --- Gain and Volume (Fix for distortion) ---
    // Apply -12dB of digital attenuation to the DAC to prevent clipping
    // The digital volume registers attenuate in 0.5dB steps. 24 * -0.5dB = -12dB.
    writeES8388Register(ES8388_DACCONTROL4, 0x00);  // Left DAC digital volume to -12dB 
    writeES8388Register(ES8388_DACCONTROL5, 0x00);  // Right DAC digital volume to -12dB

    // --- Output Mixer Configuration ---
    // Route the DAC signal to the headphone output (LOUT1/ROUT1)
    writeES8388Register(ES8388_DACCONTROL17, 0x80); // Enable Left DAC to Left Mixer
    writeES8388Register(ES8388_DACCONTROL20, 0x80); // Enable Right DAC to Right Mixer

    // --- Final Power-Up ---
    // Power up the DACs and enable the Headphone Outputs (LOUT1/ROUT1)
    writeES8388Register(ES8388_DACPOWER, 0x30);     // Enable DAC L/R and LOUT1/ROUT1
    writeES8388Register(ES8388_DACCONTROL3, 0x20);  // Unmute DAC with soft ramp enabled
    
    // Initialize headphone output volume to a reasonable default (will be overridden by setVolume later)
    // Set to about 50% volume initially to avoid any potential issues
    writeES8388Register(ES8388_LOUT1VOL, 0x0F);     // Set left headphone volume to ~50%
    writeES8388Register(ES8388_ROUT1VOL, 0x0F);     // Set right headphone volume to ~50%

    // Small delay to let ES8388 process the writes before verification reads
    delay(20);

    // Verify critical registers to ensure ES8388 is properly configured
    uint8_t dacPower = readES8388Register(ES8388_DACPOWER);
    uint8_t dacControl3 = readES8388Register(ES8388_DACCONTROL3);
    Serial.printf("AudioController: ES8388 verification - DACPOWER: 0x%02X, DACCONTROL3: 0x%02X\n", dacPower, dacControl3);
    
    if (dacPower == 0xFF || dacControl3 == 0xFF) {
        Serial.println("AudioController: WARNING - ES8388 register verification failed, I2C communication issues");
        return false;
    }

    Serial.println(F("AudioController: ES8388 initialized and verified"));
    return true;
}

bool AudioController::writeES8388Register(uint8_t reg, uint8_t value) {
    // Add retry logic and error handling for I2C conflicts
    const int maxRetries = 3;
    const int retryDelay = 10;
    
    for (int attempt = 0; attempt < maxRetries; attempt++) {
        Wire.beginTransmission(ES8388_ADDR);
        Wire.write(reg);
        Wire.write(value);
        uint8_t result = Wire.endTransmission();
        
        if (result == 0) {
            // Success
            return true;
        }
        
        // If not the last attempt, wait and retry
        if (attempt < maxRetries - 1) {
            delay(retryDelay);
        } else {
            // Last attempt failed, log the error
            Serial.printf("AudioController: ES8388 write failed (reg=0x%02X, val=0x%02X, error=%d)\n", 
                         reg, value, result);
        }
    }
    
    return false;
}

uint8_t AudioController::readES8388Register(uint8_t reg) {
    // Add retry logic for reads as well
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
        
        // If not the last attempt, wait and retry
        if (attempt < maxRetries - 1) {
            delay(retryDelay);
        }
    }
    
    // Only log error in debug mode to reduce serial spam
    #ifdef AUDIO_DEBUG_VERBOSE
    Serial.printf("AudioController: ES8388 read failed (reg=0x%02X)\n", reg);
    #endif
    return 0xFF; // Error value
}

bool AudioController::setES8388Volume(int volume) {
    // Convert 0-100 volume to ES8388 headphone register value.
    // From ES8388 Datasheet, page 28, registers 46 (LOUT1VOL) and 47 (ROUT1VOL).
    // The control is a 6-bit value with a direct scale (not inverted).
    // 0x00 = -45dB (min), 0x1E = 0dB (a good max).
    // We will map the 0-100% volume to this range.
    
    // NOTE: Removed verification reads to prevent I2C Error 263 timeouts.
    // The verification reads were causing frequent `Wire.requestFrom()` calls
    // which were timing out when the ES8388 was busy, generating Error 263.
    // Volume setting works fine without verification.

    uint8_t regValue;
    
    if (volume == 0) {
        // Use the lowest volume setting for 0%.
        regValue = 0x00;
    } else {
        // Map 1-100% volume to the register range 0 to 30 (0x00 to 0x1E).
        // This provides a linear mapping to the dB range of -45dB to 0dB.
        regValue = (uint8_t) round((volume / 100.0f) * 30.0f);
        if (regValue > 0x1E) regValue = 0x1E; // Cap at 0dB (value 30)
    }
    
    // Write to both left and right headphone volume registers
    bool success = true;
    success &= writeES8388Register(ES8388_LOUT1VOL, regValue);
    delayMicroseconds(500); // Small delay to prevent I2C bus congestion
    success &= writeES8388Register(ES8388_ROUT1VOL, regValue);
    
    // Optional verification read - disable to reduce I2C traffic and avoid timeout errors
    // Only verify on critical operations or when debugging
    #ifdef AUDIO_DEBUG_VERBOSE
    uint8_t readBackL = readES8388Register(ES8388_LOUT1VOL);
    uint8_t readBackR = readES8388Register(ES8388_ROUT1VOL);
    Serial.printf("AudioController: Volume readback - L: 0x%02X, R: 0x%02X\n", readBackL, readBackR);
    #endif
    
    return success;
}

bool AudioController::muteES8388(bool mute) {
    // ES8388_DACCONTROL3 register controls DAC mute
    // Bit 1 (DACMute): 0 = unmute, 1 = mute
    uint8_t regValue = 0x20; // Base value with soft ramp enabled
    
    if (mute) {
        regValue |= 0x02; // Set DACMute bit
    }
    
    writeES8388Register(ES8388_DACCONTROL3, regValue);
    
    Serial.printf("AudioController: ES8388 %s\n", mute ? "muted" : "unmuted");
    return true;
}

bool AudioController::initializeI2S() {
    // Let ESP8266Audio library handle I2S initialization
    // Don't manually install I2S driver as it conflicts with the library
    return true;
}

void AudioController::deinitializeI2S() {
    // Let ESP8266Audio library handle I2S cleanup
    // Don't manually uninstall as it's managed by AudioOutputI2S
    i2s_driver_installed = false;
}

bool AudioController::reinitializeAudioOutput() {
    Serial.println("AudioController: Reinitializing audio output");
    
    // Clean up existing audio output
    if (audioOutput) {
        delete audioOutput;
        audioOutput = nullptr;
    }
    
    // Ensure I2S is properly initialized
    if (!initializeI2S()) {
        Serial.println(F("AudioController: Failed to reinitialize I2S"));
        return false;
    }
    
    // Create new audio output
    return initializeAudioComponents();
}

float AudioController::getCurrentTrackSeconds() const {
    if (!initialized || currentState == STOPPED || currentTrackPath.isEmpty()) {
        return 0.0f;
    }
    
    float totalTime = accumulatedPlayTime;
    
    // If currently playing, add the time since trackStartTime
    if (currentState == PLAYING && trackStartTime > 0) {
        totalTime += (millis() - trackStartTime) / 1000.0f;
    }
    
    return totalTime;
}

bool AudioController::isValidAudioFile(const String& filePath) {
    // Check file extension - now only supporting MP3 files
    String lowerPath = filePath;
    lowerPath.toLowerCase();
    
    return lowerPath.endsWith(".mp3");
}

void AudioController::cleanupAudioComponents() {
    // Following the working example pattern - stop and free components
    if (audioMP3) {
        if (audioMP3->isRunning()) {
            audioMP3->stop();
        }
        delete audioMP3;
        audioMP3 = nullptr;
    }
    
    if (audioBuffer) {
        delete audioBuffer;
        audioBuffer = nullptr;
    }
    
    if (audioFile) {
        // Clean up the file source
        delete audioFile;
        audioFile = nullptr;
    }
}

bool AudioController::initializeAudioComponents() {
    // Create audio output if it doesn't exist
    if (!audioOutput) {
        audioOutput = new AudioOutputI2S();
        if (!audioOutput) {
            Serial.println("AudioController: Failed to create I2S output");
            return false;
        }
        
        // Configure audio output WITH MCLK - critical for ES8388!
        audioOutput->SetPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_MCLK_PIN);
        audioOutput->SetGain(currentVolume / 100.0f);
        
        Serial.printf("AudioController: I2S configured - BCLK:%d, LRCK:%d, DOUT:%d, MCLK:%d\n",
                     I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_MCLK_PIN);
    }
    
    return true;
}

bool AudioController::isNfcSessionActive(const String& expectedUid) const {
    // We need to include NfcController here to check the session
    extern NfcController &nfcController; // Reference to the global instance from main.cpp
    
    Serial.printf("AudioController: Checking NFC session - expected UID: %s\n", expectedUid.c_str());
    
    // Safety check - make sure we can access the NFC controller
 
        bool cardPresent = nfcController.isCardPresent();
        Serial.printf("AudioController: Card present: %s\n", cardPresent ? "YES" : "NO");
        if (!cardPresent) {
            Serial.println("AudioController: No card present in NFC session check");
            return false;
        }
        
        String currentUid = nfcController.currentNFCData().uidString;
        Serial.printf("AudioController: Current UID: %s\n", currentUid.c_str());
        if (currentUid != expectedUid) {
            Serial.printf("AudioController: UID mismatch - expected: %s, current: %s\n", 
                         expectedUid.c_str(), currentUid.c_str());
            return false;
        }
        
        Serial.println("AudioController: NFC session is active and UID matches");
        return true;
   
}

