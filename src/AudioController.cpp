#include "AudioController.h"
#include "ConfigManager.h"
#include "NfcController.h"
#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorWAV.h"
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
    audioWAV(nullptr),
    audioOutput(nullptr),
    currentState(STOPPED),
    currentTrackPath(""),
    currentVolume(DEFAULT_VOLUME),
    initialized(false),
    pausedPosition(0),
    hasPausedPosition(false),
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

    // Initialize I2C for ES8388 control
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);
    
    // Initialize mute pin
    pinMode(MUTE_PIN, OUTPUT);
    digitalWrite(MUTE_PIN, HIGH);
    
    // Initialize ES8388
    if (!initializeES8388()) {
        return false;
    }

    // Initialize I2S
    if (!initializeI2S()) {
        return false;
    }

    // Initialize audio components
    if (!initializeAudioComponents()) {
        return false;
    }
    
    initialized = true;

    // Set volume and unmute
    setVolume(currentVolume, true);
    digitalWrite(MUTE_PIN, LOW);
    
    Serial.println("AudioController: Initialization complete");
    
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
    Serial.printf("AudioController: play() called with filePath: '%s'\n", filePath.c_str());
    
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
    
    // Create new components (following the working example)
    audioFile = new AudioFileSourceSD(filePath.c_str());
    if (!audioFile) {
        Serial.println("AudioController: Failed to create AudioFileSourceSD");
        return false;
    }
    
    audioBuffer = new AudioFileSourceBuffer(audioFile, 2048); // Using smaller buffer like example
    if (!audioBuffer) {
        Serial.println("AudioController: Failed to create AudioFileSourceBuffer");
        delete audioFile;
        audioFile = nullptr;
        return false;
    }
    
    audioWAV = new AudioGeneratorWAV();
    if (!audioWAV) {
        Serial.println("AudioController: Failed to create AudioGeneratorWAV");
        cleanupAudioComponents();
        return false;
    }

    // Start playback (following working example)
    Serial.println("AudioController: Starting WAV playback");
    if (!audioWAV->begin(audioBuffer, audioOutput)) {
        Serial.println("AudioController: Failed to start WAV playback");
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

    if (audioWAV && audioWAV->isRunning()) {
        // Save current position before stopping
        if (audioFile) {
            pausedPosition = audioFile->getPos();
            hasPausedPosition = true;
        }
        
        // Accumulate play time before pausing
        if (trackStartTime > 0) {
            accumulatedPlayTime += (millis() - trackStartTime) / 1000.0f;
            pauseStartTime = millis();
        }
        
        // Stop the audio playback
        audioWAV->stop();
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
        
        // Create fresh WAV generator for resume
        audioWAV = new AudioGeneratorWAV();
        if (!audioWAV) {
            Serial.println("AudioController: Failed to create WAV generator for resume");
            currentState = STOPPED;
            currentTrackPath = "";
            hasPausedPosition = false;
            return false;
        }
        
        // Create new audio file source
        audioFile = new AudioFileSourceSD(currentTrackPath.c_str());
        if (!audioFile) {
            Serial.printf("AudioController: Failed to reopen audio file: %s\n", currentTrackPath.c_str());
            cleanupAudioComponents();
            currentState = STOPPED;
            currentTrackPath = "";
            hasPausedPosition = false;
            return false;
        }

        // Create buffer (file pointer is still at beginning)
        audioBuffer = new AudioFileSourceBuffer(audioFile, 2048);
        if (!audioBuffer) {
            Serial.printf("AudioController: Failed to create audio buffer\n");
            cleanupAudioComponents();
            currentState = STOPPED;
            currentTrackPath = "";
            return false;
        }

        // Start playback - this will read the WAV header first
        if (!audioWAV->begin(audioBuffer, audioOutput)) {
            Serial.printf("AudioController: Failed to start WAV playback\n");
            cleanupAudioComponents();
            currentState = STOPPED;
            currentTrackPath = "";
            return false;
        }

        // AFTER initialization is complete, seek to the paused position if we have one
        if (hasPausedPosition) {
            if (audioFile->seek(pausedPosition, SEEK_SET)) {
                Serial.printf("AudioController: Resumed from position %u\n", pausedPosition);
            } else {
                Serial.printf("AudioController: Failed to seek to position %u, starting from beginning\n", pausedPosition);
            }
            hasPausedPosition = false;
        }
        
        currentState = PLAYING;
        
        // Restart timing for resumed playback
        trackStartTime = millis();
        
        Serial.println("AudioController: Resumed successfully");
        return true;
    }

    return false;
}

bool AudioController::stop() {
    if (!initialized || currentState == STOPPED) {
        return false;
    }

    if (audioWAV && audioWAV->isRunning()) {
        audioWAV->stop();
        delay(10);
    }

    cleanupAudioComponents();
    
    currentState = STOPPED;
    currentTrackPath = "";
    pausedPosition = 0;
    hasPausedPosition = false;
    trackStartTime = 0;
    accumulatedPlayTime = 0.0f;
    pauseStartTime = 0;
    
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

void AudioController::update() {
    if (!initialized) {
        return;
    }

    // Update audio processing only if playing
    if (currentState == PLAYING) {
        if (audioWAV && audioWAV->isRunning()) {
            if (!audioWAV->loop()) {
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
    // For PAUSED state, we don't call audioWAV->loop() so playback remains stopped
    // For STOPPED state, there's nothing to update
}

void AudioController::volumeBeep() {
    if (!initialized || currentState == PLAYING) {
        // Don't beep if a track is already playing to avoid interruption.
        return;
    }
    
    // Play the beep.wav file instead of generating a tone
    const String beepPath = "/sounds/beep.wav";
    
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
    // Reset ES8388 to default values
    writeES8388Register(ES8388_CONTROL1, 0x80);
    delay(50);
    writeES8388Register(ES8388_CONTROL1, 0x00);
    delay(50);

    // --- Power Management ---
    // Power up analog and bias generation
    writeES8388Register(ES8388_CONTROL2, 0x40);  // Power up analog, disable low power modes [cite: 392]
    writeES8388Register(ES8388_CONTROL1, 0x04);  // Enable reference circuits [cite: 390]
    writeES8388Register(ES8388_CHIPPOWER, 0x00); // Power up digital blocks [cite: 399]
    
    // **NEW**: Explicitly power down the entire ADC path to reduce noise
    writeES8388Register(ES8388_ADCPOWER, 0xFF);  // Power down ADC, Mic Bias, and analog inputs 
    
    // --- Clocking and Format ---
    writeES8388Register(ES8388_MASTERMODE, 0x00);   // Set to Slave mode [cite: 423]
    writeES8388Register(ES8388_ADCCONTROL4, 0x0C);  // Set ADC to I2S, 16-bit (good practice) [cite: 449]
    writeES8388Register(ES8388_DACCONTROL1, 0x18);  // Set DAC to I2S, 16-bit [cite: 510]

    // --- Gain and Volume (Fix for distortion) ---
    // **NEW**: Apply -12dB of digital attenuation to the DAC to prevent clipping
    // The digital volume registers attenuate in 0.5dB steps. 24 * -0.5dB = -12dB.
    writeES8388Register(ES8388_DACCONTROL4, 0x00);  // Left DAC digital volume to -12dB 
    writeES8388Register(ES8388_DACCONTROL5, 0x00);  // Right DAC digital volume to -12dB [cite: 525]

    // --- Output Mixer Configuration ---
    // Route the DAC signal to the headphone output (LOUT1/ROUT1)
    writeES8388Register(ES8388_DACCONTROL17, 0x80); // Enable Left DAC to Left Mixer [cite: 564]
    writeES8388Register(ES8388_DACCONTROL20, 0x80); // Enable Right DAC to Right Mixer [cite: 577]

    // --- Final Power-Up ---
    // Power up the DACs and enable the Headphone Outputs (LOUT1/ROUT1)
    writeES8388Register(ES8388_DACPOWER, 0x30);     // Enable DAC L/R and LOUT1/ROUT1 [cite: 411]
    writeES8388Register(ES8388_DACCONTROL3, 0x20);  // Unmute DAC with soft ramp enabled [cite: 515]
    
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
    // Check file extension - now only supporting WAV files
    String lowerPath = filePath;
    lowerPath.toLowerCase();
    
    return lowerPath.endsWith(".wav");
}

void AudioController::cleanupAudioComponents() {
    // Following the working example pattern - stop and free components
    if (audioWAV) {
        if (audioWAV->isRunning()) {
            audioWAV->stop();
        }
        delete audioWAV;
        audioWAV = nullptr;
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
        
        // Configure audio output
        audioOutput->SetPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
        audioOutput->SetGain(currentVolume / 100.0f);
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

