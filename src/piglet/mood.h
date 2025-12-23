// Piglet mood and phrases
#pragma once

#include <M5Unified.h>
#include "avatar.h"

class Mood {
public:
    static void init();
    static void update();
    static void draw(M5Canvas& canvas);
    static void saveMood();  // Phase 10: Save mood to NVS
    
    // Mood triggers
    static void onHandshakeCaptured(const char* apName = nullptr);
    static void onPMKIDCaptured(const char* apName = nullptr);
    static void onNewNetwork(const char* apName = nullptr, int8_t rssi = 0, uint8_t channel = 0);
    static void setStatusMessage(const String& msg);  // For mode-specific info
    static void onMLPrediction(float confidence);
    static void onNoActivity(uint32_t seconds);
    static void onWiFiLost();
    static void onGPSFix();
    static void onGPSLost();
    static void onLowBattery();
    
    // Context-aware mood updates
    static void onSniffing(uint16_t networkCount, uint8_t channel);
    static void onPassiveRecon(uint16_t networkCount, uint8_t channel);  // DO NO HAM mode
    static void onDeauthing(const char* apName, uint32_t deauthCount);
    static void onDeauthSuccess(const uint8_t* clientMac);  // Client disconnected!
    static void onBored(uint16_t networkCount = 0);  // No valid targets available
    static void onIdle();
    static void onWarhogUpdate();
    static void onWarhogFound(const char* apName = nullptr, uint8_t channel = 0);
    static void onPiggyBluesUpdate(const char* vendor = nullptr, int8_t rssi = 0, uint8_t targetCount = 0, uint8_t totalFound = 0);
    static void resetBLESniffState();  // Reset first-target sniff flag on mode start
    
    // Get current mood phrase
    static const String& getCurrentPhrase();
    static int getCurrentHappiness();
    static int getEffectiveHappiness();  // Happiness with momentum applied
    static uint32_t getLastActivityTime();  // For buff/debuff idle detection
    static void adjustHappiness(int delta);  // Direct happiness adjustment
    
    // Phase 6: Public for phrase chaining helper functions
    static String currentPhrase;
    static uint32_t lastPhraseChange;
    static String phraseQueue[4];  // Expanded for 5-line riddles
    static uint8_t phraseQueueCount;
    static uint32_t lastQueuePop;
    
private:
    static int happiness;  // -100 to 100 (base level)
    static uint32_t phraseInterval;
    static uint32_t lastActivityTime;
    
    // Mood momentum system - recent boosts decay over time
    static int momentumBoost;           // Current boost amount (decays)
    static uint32_t lastBoostTime;      // When boost was applied
    static const uint32_t MOMENTUM_DECAY_MS = 30000;  // 30s full decay
    
    static void selectPhrase();
    static void updateAvatarState();
    static void applyMomentumBoost(int amount);
    static void decayMomentum();
};
