#pragma once

#include <Arduino.h>

// Factory test protocol `ft` over the USB console (docs/FT-PROTOCOL.md, the contract shared
// with the portal's manufacturing panel). Owns the factory session and the self-tests, as a
// state machine serviced from the loop; the console only hands it the command lines.
//
// While a session is open (`ft begin` .. `ft end`, a reboot ends it) the toy is in factory
// mode: no deep sleep, no OTA checks or jobs, no voice prompts, no figure handling (NFC and pogo
// events only feed the tests), a dim white LED scene, WiFi and BLE left as they are until a test
// needs them, logging at WARN. Every answer is one JSON line starting with {"ft": written
// straight to the port, so the log level never hides it.
namespace FactoryTest
{
    // Console: everything after "ft " (verb and arguments).
    void command(const String &args);
    // loop(), every pass, in every phase.
    void update();
    // A session is open: the factory-mode gates in main.cpp / UxScenes ask this.
    bool active();
    // The label QR payload TT1:<uuid>:TK-XXXX-XXXX, "" when no pop is enrolled (`hubid` too).
    String labelPayload();
}
