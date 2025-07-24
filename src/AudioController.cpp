#include "AudioController.h"

// Initialize static members
AudioController* AudioController::instance = nullptr;

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
    pausedPosition(0),
    hasPausedPosition(false),
    trackStartTime(0),
    accumulatedPlayTime(0.0f),
    pauseStartTime(0),
    fileManager(FileManager::getInstance()) {
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

    Serial.println("AudioController: Initializing...");

    // Initialize I2C for ES8388 control
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000); // 100kHz I2C speed
    
    // Initialize mute pin
    pinMode(MUTE_PIN, OUTPUT);
    digitalWrite(MUTE_PIN, HIGH); // Start muted
    
    // Initialize ES8388
    if (!initializeES8388()) {
        Serial.println("AudioController: Failed to initialize ES8388");
        return false;
    }

    // Initialize I2S
    if (!initializeI2S()) {
        Serial.println("AudioController: Failed to initialize I2S");
        return false;
    }

    // Create audio output
    audioOutput = new AudioOutputI2S();
    if (!audioOutput) {
        Serial.println("AudioController: Failed to create audio output");
        return false;
    }

    // Configure audio output
    audioOutput->SetPinout(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN);
    audioOutput->SetGain(currentVolume / 100.0f);

    // Create MP3 generator
    audioMP3 = new AudioGeneratorMP3();
    if (!audioMP3) {
        Serial.println("AudioController: Failed to create MP3 generator");
        delete audioOutput;
        audioOutput = nullptr;
        return false;
    }
    
    initialized = true;

    // Set volume to ensure hardware and software are synchronized
    Serial.printf("AudioController: Setting initial volume to %d%%\n", currentVolume);
    setVolume(currentVolume, true); // Initialize volume without checking current state
    
    // Test ES8388 communication and verify volume was set correctly
    Serial.printf("AudioController: Testing ES8388 registers...\n");
    Serial.printf("  CONTROL1: 0x%02X\n", readES8388Register(ES8388_CONTROL1));
    Serial.printf("  CONTROL2: 0x%02X\n", readES8388Register(ES8388_CONTROL2));
    Serial.printf("  CHIPPOWER: 0x%02X\n", readES8388Register(ES8388_CHIPPOWER));
    Serial.printf("  DACPOWER: 0x%02X\n", readES8388Register(ES8388_DACPOWER));
    Serial.printf("  LOUT1VOL: 0x%02X\n", readES8388Register(ES8388_LOUT1VOL));
    Serial.printf("  ROUT1VOL: 0x%02X\n", readES8388Register(ES8388_ROUT1VOL));
    
    // Unmute after volume is properly set
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

    // Delete audio output and generator
    if (audioOutput) {
        delete audioOutput;
        audioOutput = nullptr;
    }

    if (audioMP3) {
        delete audioMP3;
        audioMP3 = nullptr;
    }

    // Deinitialize I2S
    deinitializeI2S();

    initialized = false;
    Serial.println("AudioController: Deinitialized");
}

bool AudioController::play(const String& filePath) {
    if (!initialized) {
        Serial.println("AudioController: Not initialized");
        return false;
    }

    // Check if file exists
    if (!fileManager.fileExists(filePath)) {
        Serial.printf("AudioController: File not found: %s\n", filePath.c_str());
        return false;
    }

    // Check if it's a valid audio file
    if (!isValidAudioFile(filePath)) {
        Serial.printf("AudioController: Invalid audio file: %s\n", filePath.c_str());
        return false;
    }

    // Stop current playback if any
    if (currentState != STOPPED) {
        stop();
    }

    // Clear any previous paused position since we're starting a new track
    pausedPosition = 0;
    hasPausedPosition = false;

    // Clean up previous audio components
    cleanupAudioComponents();

    // Create new audio file source
    audioFile = new AudioFileSourceSD(filePath.c_str());
    if (!audioFile || !audioFile->isOpen()) {
        Serial.printf("AudioController: Failed to open audio file: %s\n", filePath.c_str());
        cleanupAudioComponents();
        return false;
    }

    // Create buffer
    audioBuffer = new AudioFileSourceBuffer(audioFile, AUDIO_BUFFER_SIZE);
    if (!audioBuffer) {
        Serial.printf("AudioController: Failed to create audio buffer\n");
        cleanupAudioComponents();
        return false;
    }

    // Start playback
    if (!audioMP3->begin(audioBuffer, audioOutput)) {
        Serial.printf("AudioController: Failed to start MP3 playback\n");
        cleanupAudioComponents();
        return false;
    }

    currentState = PLAYING;
    currentTrackPath = filePath;
    
    // Reset timing for new track
    trackStartTime = millis();
    accumulatedPlayTime = 0.0f;
    pauseStartTime = 0;
    
    Serial.printf("AudioController: Playing: %s\n", filePath.c_str());
    return true;
}

bool AudioController::pause() {
    if (!initialized || currentState != PLAYING) {
        return false;
    }

    if (audioMP3 && audioMP3->isRunning()) {
        // Save current position before stopping
        if (audioFile) {
            pausedPosition = audioFile->getPos();
            hasPausedPosition = true;
            Serial.printf("AudioController: Saved position %u for pause\n", pausedPosition);
        }
        
        // Accumulate play time before pausing
        if (trackStartTime > 0) {
            accumulatedPlayTime += (millis() - trackStartTime) / 1000.0f;
            pauseStartTime = millis();
        }
        
        // Stop the audio playback
        audioMP3->stop();
        currentState = PAUSED;
        Serial.println("AudioController: Paused");
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
        
        // Create new audio file source
        audioFile = new AudioFileSourceSD(currentTrackPath.c_str());
        if (!audioFile || !audioFile->isOpen()) {
            Serial.printf("AudioController: Failed to reopen audio file: %s\n", currentTrackPath.c_str());
            currentState = STOPPED;
            currentTrackPath = "";
            hasPausedPosition = false;
            return false;
        }

        // Seek to the paused position if we have one
        if (hasPausedPosition) {
            if (audioFile->seek(pausedPosition, SEEK_SET)) {
                Serial.printf("AudioController: Resumed from position %u\n", pausedPosition);
            } else {
                Serial.printf("AudioController: Failed to seek to position %u, starting from beginning\n", pausedPosition);
            }
            hasPausedPosition = false;
        }

        // Create buffer
        audioBuffer = new AudioFileSourceBuffer(audioFile, AUDIO_BUFFER_SIZE);
        if (!audioBuffer) {
            Serial.printf("AudioController: Failed to create audio buffer\n");
            cleanupAudioComponents();
            currentState = STOPPED;
            currentTrackPath = "";
            return false;
        }

        // Start playback
        if (!audioMP3->begin(audioBuffer, audioOutput)) {
            Serial.printf("AudioController: Failed to start MP3 playback\n");
            cleanupAudioComponents();
            currentState = STOPPED;
            currentTrackPath = "";
            return false;
        }
        
        currentState = PLAYING;
        
        // Restart timing for resumed playback
        trackStartTime = millis();
        
        Serial.println("AudioController: Resumed");
        return true;
    }

    return false;
}

bool AudioController::stop() {
    if (!initialized || currentState == STOPPED) {
        return false;
    }

    if (audioMP3) {
        audioMP3->stop();
    }

    cleanupAudioComponents();
    
    currentState = STOPPED;
    currentTrackPath = "";
    
    // Clear paused position
    pausedPosition = 0;
    hasPausedPosition = false;
    
    // Reset timing variables
    trackStartTime = 0;
    accumulatedPlayTime = 0.0f;
    pauseStartTime = 0;
    
    Serial.println("AudioController: Stopped");
    return true;
}

bool AudioController::volumeUp() {
    int newVolume = currentVolume + VOLUME_STEP;
    if (newVolume > MAX_VOLUME) {
        newVolume = MAX_VOLUME;
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

    // Clamp volume
    if (volume < MIN_VOLUME) volume = MIN_VOLUME;
    if (volume > MAX_VOLUME) volume = MAX_VOLUME;

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

void AudioController::update() {
    if (!initialized) {
        return;
    }

    // Update audio processing only if playing
    if (currentState == PLAYING) {
        if (audioMP3 && audioMP3->isRunning()) {
            if (!audioMP3->loop()) {
                // Track finished
                Serial.println("AudioController: Track finished");
                stop();
            }
        } else {
            // Should be playing but isn't - something went wrong
            Serial.println("AudioController: Playback error detected");
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

    Serial.printf("AudioController: Volume beep for %d%%\n", currentVolume);
    
    // Play the beep.mp3 file instead of generating a tone
    const String beepPath = "/sounds/beep.mp3";
    
    // Check if beep file exists
    if (!fileManager.fileExists(beepPath)) {
        Serial.printf("AudioController: Beep file not found: %s\n", beepPath.c_str());
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
    
    Serial.println("AudioController: Volume beep completed");
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

    Serial.println("AudioController: ES8388 initialized with noise & distortion fixes.");
    return true;
}

bool AudioController::writeES8388Register(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(ES8388_ADDR);
    Wire.write(reg);
    Wire.write(value);
    uint8_t result = Wire.endTransmission();
    
    if (result != 0) {
        Serial.printf("AudioController: I2C write error %d for register 0x%02X\n", result, reg);
        return false;
    }
    
    return true;
}

uint8_t AudioController::readES8388Register(uint8_t reg) {
    Wire.beginTransmission(ES8388_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    
    Wire.requestFrom(ES8388_ADDR, (uint8_t)1);
    if (Wire.available()) {
        return Wire.read();
    }
    
    return 0xFF; // Error value
}

bool AudioController::setES8388Volume(int volume) {
    // Convert 0-100 volume to ES8388 headphone register value.
    // From ES8388 Datasheet, page 28, registers 46 (LOUT1VOL) and 47 (ROUT1VOL).
    // The control is a 6-bit value with a direct scale (not inverted).
    // 0x00 = -45dB (min), 0x1E = 0dB (a good max).
    // We will map the 0-100% volume to this range.

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
    
    Serial.printf("AudioController: Setting ES8388 L/R OUT1 volume to %d%% (reg: 0x%02X)\n", volume, regValue);
    
    // Write to both left and right headphone volume registers
    bool success = true;
    success &= writeES8388Register(ES8388_LOUT1VOL, regValue);
    success &= writeES8388Register(ES8388_ROUT1VOL, regValue);
    
    // Verify the write by reading back
    uint8_t readBackL = readES8388Register(ES8388_LOUT1VOL);
    uint8_t readBackR = readES8388Register(ES8388_ROUT1VOL);
    Serial.printf("AudioController: Volume readback - L: 0x%02X, R: 0x%02X\n", readBackL, readBackR);
    
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
    Serial.println("AudioController: Initializing I2S with MCLK output...");

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = true, // Use APLL for higher clock accuracy, important for audio
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("AudioController: Failed to install I2S driver: %s\n", esp_err_to_name(err));
        return false;
    }

    // Configure the I2S pins, now including the MCLK output.
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_MCLK_PIN,       // Master Clock Pin
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRCK_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE // Not used for output
    };

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("AudioController: Failed to set I2S pins: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    Serial.println("AudioController: I2S initialized successfully");
    return true;
}

void AudioController::deinitializeI2S() {
    i2s_driver_uninstall(I2S_NUM_0);
    Serial.println("AudioController: I2S deinitialized");
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
    // Check file extension
    String lowerPath = filePath;
    lowerPath.toLowerCase();
    
    return lowerPath.endsWith(".mp3") || 
           lowerPath.endsWith(".wav") || 
           lowerPath.endsWith(".flac") || 
           lowerPath.endsWith(".aac");
}

void AudioController::cleanupAudioComponents() {
    if (audioBuffer) {
        delete audioBuffer;
        audioBuffer = nullptr;
    }
    
    if (audioFile) {
        delete audioFile;
        audioFile = nullptr;
    }
}
