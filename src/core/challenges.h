// Session Challenges - pig wakes, pig demands action
// three trials per session: easy, middling, brutal
#pragma once

#include <Arduino.h>
#include "xp.h"
#include "porkchop.h"

// what the pig tracks
enum class ChallengeType : uint8_t {
    NETWORKS_FOUND,     // total networks sniffed
    HIDDEN_FOUND,       // hidden networks only
    HANDSHAKES,         // handshakes captured
    PMKIDS,             // PMKIDs grabbed
    DEAUTHS,            // successful deauths
    GPS_NETWORKS,       // GPS-tagged networks (WARHOG)
    BLE_PACKETS,        // BLE spam packets sent
    PASSIVE_NETWORKS,   // networks in DNH mode
    NO_DEAUTH_STREAK,   // networks without violence (conditional)
    DISTANCE_M,         // meters walked wardriving
    WPA3_FOUND,         // WPA3 networks spotted
    OPEN_FOUND          // open networks found
};

// difficulty scales with slot position
enum class ChallengeDifficulty : uint8_t {
    EASY = 0,    // slot 0: base target, base XP
    MEDIUM = 1,  // slot 1: 2-3x target, 2x XP  
    HARD = 2     // slot 2: 4-6x target, 4x XP
};

// runtime challenge state
struct ActiveChallenge {
    ChallengeType type;
    ChallengeDifficulty difficulty;
    uint16_t target;      // goal value
    uint16_t progress;    // current progress
    uint16_t xpReward;    // calculated reward
    char name[32];        // formatted challenge name
    bool completed;       // pig is pleased
    bool failed;          // for conditional challenges (deauth invalidates streak)
};

class Challenges {
public:
    // the pig wakes. three trials await.
    static void generate();
    
    // reveal demands to the worthy (Serial output)
    static void printToSerial();
    
    // single integration point - called from XP::addXP()
    static void onXPEvent(XPEvent event);
    
    // reset all challenges (session end)
    static void reset();
    
    // accessors for UI
    static const ActiveChallenge& get(uint8_t idx);
    static uint8_t getActiveCount();
    static uint8_t getCompletedCount();
    static bool allCompleted();
    
    // check if pig cares (active modes only)
    static bool isPigAwake();

private:
    static ActiveChallenge challenges[3];
    static uint8_t activeCount;
    static bool sessionDeauthed;  // tracks if deauth ever sent (for conditional)
    
    static void updateProgress(ChallengeType type, uint16_t delta);
    static void failConditional(ChallengeType type);
};
