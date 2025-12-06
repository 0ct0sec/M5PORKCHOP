// Piglet mood and phrases
#pragma once

#include <M5Unified.h>
#include "avatar.h"

class Mood {
public:
    static void init();
    static void update();
    static void draw(M5Canvas& canvas);
    
    // Mood triggers
    static void onHandshakeCaptured(const char* apName = nullptr);
    static void onNewNetwork(const char* apName = nullptr);
    static void onMLPrediction(float confidence);
    static void onNoActivity(uint32_t seconds);
    static void onWiFiLost();
    static void onGPSFix();
    static void onGPSLost();
    static void onLowBattery();
    
    // Get current mood phrase
    static const String& getCurrentPhrase();
    static int getCurrentHappiness();
    
private:
    static String currentPhrase;
    static int happiness;  // -100 to 100
    static uint32_t lastPhraseChange;
    static uint32_t phraseInterval;
    static uint32_t lastActivityTime;
    
    static void selectPhrase();
    static void updateAvatarState();
};
