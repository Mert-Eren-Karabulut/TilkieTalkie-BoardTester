#ifndef AUDIOCONTROLLER_H
#define AUDIOCONTROLLER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <Wire.h>
#include <vector>
#include <functional>
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include "ReadAheadFileSource.h"
#include "FileManager.h"

// Forward declaration to avoid circular includes
class NfcController;

// AudioOutputI2S installs the I2S driver in begin() and uninstalls it in stop(), i.e. on every
// track change: MCLK to the ES8388 drops, the queued DMA tail is cut and the codec pops. This
// keeps the driver (and MCLK) up for the life of the device; teardown() is only for sleep.
class PersistentI2SOutput : public AudioOutputI2S {
public:
    using AudioOutputI2S::AudioOutputI2S;
    bool stop() override { return i2sOn; }  // queued samples play out, tx_desc_auto_clear then gives silence
    bool teardown() { return AudioOutputI2S::stop(); }
    bool installed() const { return i2sOn; }
};

class AudioController {
public:
    // Audio states
    enum AudioState {
        STOPPED,
        PLAYING,
        PAUSED
    };

    // Speaker amplifier control (NS4150B CTRL on GPIO41, shared with the jack contact)
    enum AmpMode {
        AMP_AUTO,       // on while sound is produced, off otherwise
        AMP_FORCE_ON,   // diagnostics
        AMP_FORCE_OFF
    };

    // Volume levels (0-100)
    static const int MIN_VOLUME = 0;
    static const int MAX_VOLUME = 100;
    static const int DEFAULT_VOLUME = 75;
    static const int VOLUME_STEP = 5;
    static const int HEADPHONE_CEILING_STEPS = 4;  // jack (LOUT2/ROUT2) sits 4 x 1.5 dB under the speaker

    // ES8388 I2C address
    static const uint8_t ES8388_ADDR = 0x10;

    // Pin definitions (ESP32-S3, board "Adem v7")
    static const int I2S_BCLK_PIN = 7;     // c_sclk
    static const int I2S_LRCK_PIN = 8;     // c_lrck
    static const int I2S_DOUT_PIN = 10;    // c_dsdin - ESP32 -> ES8388
    static const int I2S_MCLK_PIN = 11;    // c_mclk
    static const int I2C_SDA_PIN = 17;     // c_sda - ES8388 control
    static const int I2C_SCL_PIN = 18;     // c_scl
    // AMP_CTRL: NS4150B CTRL, held high by the board, pulled low by the headphone jack contact.
    // Driven open-drain: LOW = speaker off, released = board/jack decide (and the level tells
    // us whether a plug is in).
    static const int AMP_MODE_PIN = 41;

    // Singleton pattern
    static AudioController& getInstance();

    // Initialization
    bool begin();
    void end();  // mute, power the codec/amp down and release I2S (before sleep)

    // Playback control
    bool play(const String& filePath = ""); // Play specific file or current playlist track
    bool pause();
    bool resume();
    bool stop();
    bool seekTo(float seconds); // Seek to specific time position (works during playback or when paused)

    // Playlist control methods
    bool nextTrack();
    bool prevTrack();
    bool playTrack(const String& trackId); // Jump to a specific track by backend ID
    // moreComing: the figure's remaining tracks are still downloading. When the last cached
    // track ends the playlist then waits (LED "loading", prompt) and continues with the next
    // track as soon as mergePlaylist() brings it, instead of ending the playlist.
    void setPlaylist(const std::vector<String>& trackPaths, const String& figureUid, bool moreComing = false);
    // Replace the playlist for the same figure without interrupting the current track
    // (tracks that finished downloading are added in order; the playing track keeps its slot).
    void mergePlaylist(const std::vector<String>& trackPaths, const String& figureUid, bool moreComing = false);
    void clearPlaylist();
    bool isWaitingForTrack() const { return waitingForTrack; }
    bool consumeWaitStarted();        // true once when the playlist started waiting for a download

    // Playlist information
    bool hasPlaylist() const { return !playlist.empty(); }
    int getCurrentTrackIndex() const { return currentPlaylistIndex; }
    int getPlaylistSize() const { return playlist.size(); }
    String getPlaylistFigureUid() const { return playlistFigureUid; }
    bool playlistContains(const String& path) const {
        for (const auto& p : playlist) { if (p == path) return true; }
        return false;
    }

    // Volume control (codec attenuation only; the NVS write is deferred to update())
    bool volumeUp();
    bool volumeDown();
    bool setVolume(int volume, bool initialize = false);
    int getCurrentVolume() const { return currentVolume; }

    // Volume ceiling control
    void setVolumeCeiling(int ceiling);
    int getVolumeCeiling() const;

    // Amplifier / headphone
    void setAmpMode(AmpMode mode);
    AmpMode getAmpMode() const { return ampMode; }
    bool isAmpEnabled() const { return ampEnabled; }
    bool isHeadphonePlugged() const { return headphonePlugged; }

    // Status
    AudioState getState() const { return currentState; }
    // The story track: while a prompt plays this is the interrupted track (or nothing), never
    // the prompt file itself, so playlist merges and the portal never mistake a prompt for a story.
    String getCurrentTrack() const {
        if (transientPlayback) return resumeAfterPrompt ? resumeTrackPath : String();
        return currentTrackPath;
    }
    bool isPlaying() const { return currentState == PLAYING; }
    bool isPaused() const { return currentState == PAUSED; }
    bool isStopped() const { return currentState == STOPPED; }
    uint32_t getReadAheadFill() const { return source ? source->available() : 0; }
    uint32_t getReadAheadCapacity() const { return source ? source->capacity() : 0; }
    uint32_t getUnderruns() const { return source ? source->underruns() : 0; }
    void printStatus();

    // Track timing
    float getCurrentTrackSeconds() const;

    // Loop-task housekeeping (deferred NVS writes, headphone polling); decoding runs on its own task
    void update();

    // Beep functions
    void volumeBeep(); // Beep to indicate volume level
    // Spoken prompts / tones (VoicePrompts): interrupts a playing or paused track, which is
    // resumed at the same position when the prompt ends. One prompt at a time.
    bool playPrompt(const String& filePath);
    bool isPromptPlaying() const { return transientPlayback && currentState == PLAYING; }
    bool consumePlaylistEnded();      // true once after the last playlist track finished

    // NFC session check (public for SleepController)
    bool isNfcSessionActive(const String& expectedUid) const;

private:
    // Singleton instance
    static AudioController* instance;

    // Constructor (private for singleton)
    AudioController();
    ~AudioController();

    // Audio components (allocated once in begin(); only the decoder object is per track)
    PersistentI2SOutput* audioOutput;
    ReadAheadFileSource* source;
    AudioGeneratorMP3* audioMP3;
    void* mp3Prealloc;
    int mp3PreallocSize;

    // State variables
    AudioState currentState;
    String currentTrackPath;
    int currentVolume;
    int volumeCeiling;  // Maximum allowed volume (0-100), stored in NVS
    bool initialized;
    bool transientPlayback;   // a beep: must not advance the playlist when it ends
    String resumeTrackPath;          // track interrupted by a prompt
    float resumeSeconds = 0.0f;
    bool resumeAfterPrompt = false;
    bool resumeAsPaused = false;
    volatile bool promptEnded = false;   // set by the audio task, handled in update()
    bool startingPrompt = false;         // play() called from playPrompt()
    bool deferredPlayPending = false;    // a story asked to start while a prompt was playing
    String deferredPlayPath;
    volatile bool playlistEnded = false; // set when the last track finished
    bool outputsActive;       // codec unmuted + amp released
    bool codecMuted;
    AmpMode ampMode;
    bool ampEnabled;
    bool headphonePlugged;
    unsigned long lastPlugPoll;
    bool volumeDirty;
    unsigned long volumeDirtyAt;

    // Pause/resume time tracking
    float pausedTimeSeconds;         // Time position in seconds when paused
    bool hasPausedTime;              // Whether we have a valid paused time

    // Track timing variables
    unsigned long trackStartTime;    // millis() when track started playing
    float accumulatedPlayTime;       // accumulated play time in seconds
    unsigned long pauseStartTime;    // millis() when track was paused

    // Playlist variables
    std::vector<String> playlist;
    int currentPlaylistIndex;
    String playlistFigureUid; // UID of the figure associated with this playlist
    bool playlistFinished; // Track if playlist has finished playing
    bool playlistOpen = false;        // more tracks of this figure are still downloading
    bool waitingForTrack = false;     // the last cached track ended while playlistOpen
    volatile bool waitStarted = false; // consumed by the loop task (prompt + LED)
    String waitAfterPath;             // the track we finished on; playback resumes after it

    // ES8388 control
    bool initializeES8388();
    bool writeES8388Register(uint8_t reg, uint8_t value);
    uint8_t readES8388Register(uint8_t reg);
    void applyCodecVolume();
    void muteES8388(bool mute);

    // Output path
    void setOutputsActive(bool active);
    void applyAmp();
    void pollHeadphone();

    // Helper functions
    bool isValidAudioFile(const String& filePath);
    bool startDecoder(const String& filePath);
    void releaseDecoder();
    void finishTrack();
    void updatePlaybackSlice();
    bool suspendBackgroundTaskForControl();
    void resumeBackgroundTaskForControl();
    static void audioTaskEntry(void* parameter);

    // File manager reference
    FileManager& fileManager;
    TaskHandle_t audioTaskHandle;
    SemaphoreHandle_t audioMutex;
};

#endif // AUDIOCONTROLLER_H
