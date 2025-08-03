#ifndef AUDIOCONTROLLER_H
#define AUDIOCONTROLLER_H

#include <Arduino.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <vector>
#include "AudioFileSourceSD.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include "FileManager.h"

// Forward declaration to avoid circular includes
class NfcController;

class AudioController {
public:
    // Audio states
    enum AudioState {
        STOPPED,
        PLAYING,
        PAUSED
    };

    // Volume levels (0-100)
    static const int MIN_VOLUME = 0;
    static const int MAX_VOLUME = 100;
    static const int DEFAULT_VOLUME = 75;
    static const int VOLUME_STEP = 5;

    // ES8388 I2C address
    static const uint8_t ES8388_ADDR = 0x10;

    // Pin definitions (updated with actual GPIO assignments)
    static const int I2S_BCLK_PIN = 5;     // c_sclk - I2S bit clock (GPIO5)
    static const int I2S_LRCK_PIN = 25;    // c_lrck - I2S left/right clock (GPIO25)
    static const int I2S_DOUT_PIN = 26;    // c_dsdin - I2S data from ESP32 to ES8388 (GPIO26)
    static const int I2S_MCLK_PIN = 0;     // c_mclk - I2S master clock (GPIO0) - ADDED
    static const int I2C_SDA_PIN = 18;     // c_sda - I2C data for ES8388 control (GPIO18)
    static const int I2C_SCL_PIN = 23;     // c_scl - I2C clock for ES8388 control (GPIO23)
    static const int MUTE_PIN = 19;        // Optional mute control pin (you can change this)
    // Available but not used: c_asdout (GPIO35)

    // Buffer size for audio streaming
    static const int AUDIO_BUFFER_SIZE = 4096;

    // Singleton pattern
    static AudioController& getInstance();

    // Initialization
    bool begin();
    void end();

    // Playback control
    bool play(const String& filePath = ""); // Play specific file or current playlist track
    bool pause();
    bool resume();
    bool stop();
    
    // Playlist control methods
    bool nextTrack();
    bool prevTrack();
    void setPlaylist(const std::vector<String>& trackPaths, const String& figureUid);
    void clearPlaylist();
    
    // Playlist information
    bool hasPlaylist() const { return !playlist.empty(); }
    int getCurrentTrackIndex() const { return currentPlaylistIndex; }
    int getPlaylistSize() const { return playlist.size(); }
    String getPlaylistFigureUid() const { return playlistFigureUid; }

    // Volume control
    bool volumeUp();
    bool volumeDown();
    bool setVolume(int volume, bool initialize = false); // Added initialize flag for internal use
    int getCurrentVolume() const { return currentVolume; }
    
    // Volume ceiling control
    void setVolumeCeiling(int ceiling);
    int getVolumeCeiling() const;

    // Status
    AudioState getState() const { return currentState; }
    String getCurrentTrack() const { return currentTrackPath; }
    bool isPlaying() const { return currentState == PLAYING; }
    bool isPaused() const { return currentState == PAUSED; }
    bool isStopped() const { return currentState == STOPPED; }
    
    // Track timing
    float getCurrentTrackSeconds() const;

    // Update function (call in main loop)
    void update();
    
    // Beep functions
    void volumeBeep(); // Beep to indicate volume level

private:
    // Singleton instance
    static AudioController* instance;
    
    // Constructor (private for singleton)
    AudioController();
    ~AudioController();

    // Audio components
    AudioFileSourceSD* audioFile;
    AudioFileSourceBuffer* audioBuffer;
    AudioGeneratorWAV* audioWAV;
    AudioOutputI2S* audioOutput;

    // State variables
    AudioState currentState;
    String currentTrackPath;
    int currentVolume;
    int volumeCeiling;  // Maximum allowed volume (0-100), stored in NVS
    bool initialized;
    bool i2s_driver_installed;  // Track I2S driver state to prevent double initialization
    
    // Pause/resume position tracking
    uint32_t pausedPosition;
    bool hasPausedPosition;
    
    // Track timing variables
    unsigned long trackStartTime;    // millis() when track started playing
    float accumulatedPlayTime;       // accumulated play time in seconds
    unsigned long pauseStartTime;    // millis() when track was paused
    
    // Playlist variables
    std::vector<String> playlist;
    int currentPlaylistIndex;
    String playlistFigureUid; // UID of the figure associated with this playlist
    bool playlistFinished; // Track if playlist has finished playing

    // ES8388 control
    bool initializeES8388();
    bool writeES8388Register(uint8_t reg, uint8_t value);
    uint8_t readES8388Register(uint8_t reg);
    bool setES8388Volume(int volume);
    bool muteES8388(bool mute);

    // I2S configuration
    bool initializeI2S();
    void deinitializeI2S();
    bool reinitializeAudioOutput();

    // Helper functions
    bool isValidAudioFile(const String& filePath);
    void cleanupAudioComponents();
    bool initializeAudioComponents();
    bool isNfcSessionActive(const String& expectedUid) const;

    // File manager reference
    FileManager& fileManager;
};

#endif // AUDIOCONTROLLER_H
