#include "UxScenes.h"
#include <esp_log.h>
#include "Bringup.h"
#include "Faults.h"
#include "OtaManager.h"
#include "Policy.h"
#include "SystemState.h"
#include "VoicePrompts.h"

static VoicePrompts &prompts = VoicePrompts::getInstance();

// Voice feedback state
static unsigned long gSetupWelcomeNextAt = 0;
static uint8_t gSetupWelcomeRepeats = 0;
static bool gIdleHelloDone = false;
static bool gWifiLostAnnounced = false;
static unsigned long gWifiLostAnnouncedAt = 0;

void startSetupWelcome()
{
    prompts.say(Prompt::SETUP_WELCOME);
    gSetupWelcomeNextAt = millis() + 90000UL;
    gSetupWelcomeRepeats = 1;
}

// Docked user feedback: while USB is present, show the 5-segment charge bar on the
// LEDs (one segment per 20% SOC, the filling segment breathing while current flows).
// Runs in both monitor and full-run phases. Deliberately stands down for every LED
// owner with higher claim: first-boot bring-up (owns the LED legend), the learning
// discharge (uses the LEDs as its load), and transient pulseRapid event feedback
// (plays over the bar, which resumes by itself). Read-only towards the charging
// state machine — supervisor, PD negotiation, recovery and bring-up are untouched.
// The docked charge bar is the idle picture. Anything the user is waiting for (pairing,
// connecting, loading a figure, downloading, playing, updating, an error) owns the LEDs.
bool chargeBarAllowed()
{
    if (gSystemPhase != PHASE_RUNNING)
    {
        return true;
    }
    if (gOtaProgress >= 0 || gSystemError)
    {
        return false;
    }
    if (audioController.isPlaying() || audioController.isPaused() || audioController.isWaitingForTrack())
    {
        return false;
    }
    if (gFigureLoadingSince != 0 && millis() - gFigureLoadingSince < Policy::FIGURE_LOADING_WINDOW_MS)
    {
        return false;
    }
    if (fileManager.hasActiveDownload())
    {
        return false;
    }
    if (!WiFi.isConnected() && (wifiProv.isProvisioningManagerActive() || millis() - gFullSystemStartedAt < Policy::WIFI_CONNECTING_WINDOW_MS))
    {
        return false; // pairing / connecting; a long offline stretch shows the bar again
    }
    return true;
}

void updateChargingUx()
{
    static bool wasShowingCharge = false;
    const bool showCharge = battery.isVbusPresent() &&
                            gBringupPhase == BRINGUP_INACTIVE &&
                            !gLearnDischarge &&
                            battery.hasTelemetry() &&
                            chargeBarAllowed();
    if (showCharge)
    {
        // Spike filter: a glitched gauge read once flashed the bar full for a frame.
        // The displayed percent only follows a >20-point jump if it persists for 2.5s
        // (several 1s gauge refresh cycles); real SOC re-seeds still get through.
        static int shownPercent = -1;
        static unsigned long jumpSince = 0;
        const int fresh = (int)battery.getBatteryPercentage();
        if (shownPercent < 0 || abs(fresh - shownPercent) <= 20)
        {
            shownPercent = fresh;
            jumpSince = 0;
        }
        else if (jumpSince == 0)
        {
            jumpSince = millis();
        }
        else if (millis() - jumpSince > 2500)
        {
            shownPercent = fresh;
            jumpSince = 0;
        }
        ledController.showChargeLevel((uint8_t)shownPercent, battery.getChargingStatus());
        wasShowingCharge = true;
    }
    else if (wasShowingCharge)
    {
        wasShowingCharge = false;
        ledController.stopChargeDisplay();
        if (gSystemPhase == PHASE_MONITOR)
        {
            ledController.pulseLed(SYSTEM_MONITOR_LED); // restore monitor breathing
        }
        else
        {
            // running phase: deriveLedScene() picks the next scene on the following loop
        }
    }
}

// Single owner of the persistent LED scene while the full system runs. Derived from state on
// every loop iteration, so no code path can leave a stale animation behind; event feedback
// uses ledController.flash() and plays on top.
void deriveLedScene()
{
    if (gSystemPhase != PHASE_RUNNING)
    {
        return; // monitor / bring-up drive the LED directly
    }
    if (ledController.isChargeDisplayActive())
    {
        return; // docked SOC bar (chargeux section owns it)
    }
    if (gOtaProgress >= 0)
    {
        ledController.setScene(LedController::SCENE_OTA, gOtaProgress);
        return;
    }
    if (gSystemError || Faults::hasActive(Severity::CRITICAL))
    {
        ledController.setScene(LedController::SCENE_ERROR);
        return;
    }

    const bool lowBattery = battery.hasTelemetry() && !battery.getChargingStatus() && battery.getBatteryPercentage() < Policy::LOW_BATTERY_PERCENT;
    ledController.setLowBattery(lowBattery);

    if (audioController.isPlaying() && !audioController.isPromptPlaying())
    {
        ledController.setScene(LedController::SCENE_PLAYING);
    }
    else if (audioController.isPaused())
    {
        ledController.setScene(LedController::SCENE_PAUSED);
    }
    else if (audioController.isWaitingForTrack())
    {
        ledController.setScene(LedController::SCENE_LOADING); // between stories, the next one is downloading
    }
    else if (gFigureLoadingSince != 0 && millis() - gFigureLoadingSince < Policy::FIGURE_LOADING_WINDOW_MS)
    {
        ledController.setScene(LedController::SCENE_LOADING);
    }
    else if (fileManager.hasActiveDownload())
    {
        ledController.setScene(LedController::SCENE_DOWNLOADING);
    }
    else if (!WiFi.isConnected())
    {
        if (wifiProv.isProvisioningManagerActive())
            ledController.setScene(LedController::SCENE_PROVISIONING);
        else if (millis() - gFullSystemStartedAt < Policy::WIFI_CONNECTING_WINDOW_MS)
            ledController.setScene(LedController::SCENE_WIFI_CONNECTING);
        else
            ledController.setScene(LedController::SCENE_OFFLINE);
    }
    else if (lowBattery)
    {
        ledController.setScene(LedController::SCENE_LOW_BATTERY);
    }
    else
    {
        ledController.setScene(LedController::SCENE_OFF);
    }
}

// Voice feedback that depends on state rather than on an event.
void serviceUxPrompts()
{
    if (gSystemPhase != PHASE_RUNNING)
    {
        return;
    }
    if (!OtaManager::getInstance().inProgress())
    {
        prompts.update(); // flash writes make speech stutter: hold prompts during an update
    }
    if (audioController.consumePlaylistEnded())
    {
        prompts.say(Prompt::PLAYLIST_END);
    }
    if (audioController.consumeWaitStarted())
    {
        prompts.say(Prompt::DOWNLOAD_WAIT); // the next story is still downloading; playback resumes by itself
    }
    {
        static bool wasConnected = false;
        const bool connected = WiFi.isConnected();
        if (connected && !wasConnected)
        {
            Faults::clearOn(FaultClear::ON_NETWORK); // network is back
        }
        wasConnected = connected;
    }
    static unsigned long lastCheck = 0;
    const unsigned long now = millis();
    if (now - lastCheck < 1000)
    {
        return;
    }
    lastCheck = now;
    const bool provisioned = wifiProv.isProvisioned();
    // Not provisioned: repeat the invitation a few times while nobody pairs.
    if (gSetupWelcomeNextAt && !WiFi.isConnected() && wifiProv.isProvisioningManagerActive() && (long)(now - gSetupWelcomeNextAt) >= 0)
    {
        if (gSetupWelcomeRepeats < 5)
        {
            gSetupWelcomeRepeats++;
            gSetupWelcomeNextAt = now + 90000UL;
            prompts.say(Prompt::SETUP_WELCOME);
        }
        else
        {
            gSetupWelcomeNextAt = 0;
        }
    }
    // Provisioned boot without a figure: say hello once (3 s in, so the pogo state is settled).
    if (!gIdleHelloDone && now - gFullSystemStartedAt > 3000)
    {
        gIdleHelloDone = true;
        if (OtaManager::getInstance().bootedFromUpdate())
        {
            prompts.say(Prompt::UPDATE_DONE); // first words after an update
        }
        if (provisioned && !nfcController.isPogoSwitchActive())
        {
            prompts.say(Prompt::IDLE_HELLO);
        }
    }
    // Provisioned, the home network is in range, but no link for a minute. Away from home
    // (network not in range) the toy plays from the card and says nothing about it.
    if (provisioned && !WiFi.isConnected() && !wifiProv.isProvisioningManagerActive() && wifiProv.isHomeNetworkVisible() &&
        now - gFullSystemStartedAt > Policy::WIFI_CONNECTING_WINDOW_MS)
    {
        if (!gWifiLostAnnounced || now - gWifiLostAnnouncedAt > 600000UL)
        {
            gWifiLostAnnounced = true;
            gWifiLostAnnouncedAt = now;
            Faults::raise(Fault::WIFI_NO_LINK); // speaks wifi_lost + the reset hint
        }
    }
    else if (WiFi.isConnected())
    {
        gWifiLostAnnounced = false;
    }
    // Figure on the pogo pins but no card read within 3 s (decided by NfcController, once per session).
    if (nfcController.consumeReadTrouble())
    {
        Faults::raise(Fault::NFC_READ_TROUBLE);
    }
}

// Battery warnings on battery power: at 20 % and every 5 % below.
void updateBatteryUx()
{
    static int lastWarnStep = 100;
    if (gSystemPhase != PHASE_RUNNING || !battery.hasTelemetry())
    {
        return;
    }
    if (battery.isVbusPresent() || battery.getChargingStatus())
    {
        lastWarnStep = 100;
        return;
    }
    const int pct = (int)battery.getBatteryPercentage();
    if (pct > 20)
    {
        lastWarnStep = 100;
        return;
    }
    const int step = pct / 5; // 20 -> 4, 15 -> 3, 10 -> 2, 5 -> 1
    if (step < lastWarnStep)
    {
        lastWarnStep = step;
        char d[16];
        snprintf(d, sizeof(d), "pct=%d", pct);
        Faults::raise(Fault::BATTERY_LOW, d);
    }
}
