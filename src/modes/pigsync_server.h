#pragma once

#include <Arduino.h>
#include <M5GFX.h>

// Minimal Sirloin-side PigSync responder.  This first stage proves discovery,
// encrypted channel setup, control traffic, and time sync. Capture serving is
// deliberately reported as empty until the SD-backed transfer layer is added.
class PigSyncServerMode {
public:
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isRunning() { return running; }
    static uint8_t getChannel() { return channel; }

private:
    static bool running;
    static bool initialized;
    static bool connected;
    static uint8_t channel;
    static uint8_t clientMac[6];
    static uint16_t sessionId;
    static uint8_t txSeq;
    static uint32_t rxCount;
    static uint32_t txCount;
    static uint32_t lastActivity;
    static char status[40];
};
