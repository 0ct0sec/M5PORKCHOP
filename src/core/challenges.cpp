// Session Challenges Implementation
// pig demands action. pig tracks progress. pig rewards effort.

#include "challenges.h"
#include "config.h"
#include "../ui/display.h"
#include <M5Unified.h>

// porkchop global instance lives in main.cpp
extern Porkchop porkchop;

// static member initialization
ActiveChallenge Challenges::challenges[3] = {};
uint8_t Challenges::activeCount = 0;
bool Challenges::sessionDeauthed = false;

// ============================================================
// CHALLENGE TEMPLATE POOL
// the pig's menu of demands. 12 options, 3 chosen per session.
// ============================================================

struct ChallengeTemplate {
    ChallengeType type;
    uint16_t easyTarget;     // base target for EASY
    uint8_t mediumMult;      // multiplier for MEDIUM (2-3x)
    uint8_t hardMult;        // multiplier for HARD (4-6x)
    const char* nameFormat;  // printf format with %d for target
    uint8_t xpRewardBase;    // base XP reward (scaled by difficulty)
};

// pig's demands are varied but fair (mostly)
static const ChallengeTemplate CHALLENGE_POOL[] = {
    // type               easy  med  hard  name format                xp
    { ChallengeType::NETWORKS_FOUND,    25,   2,    4,  "sniff %d networks",       15 },
    { ChallengeType::NETWORKS_FOUND,    50,   2,    3,  "discover %d APs",         25 },
    { ChallengeType::HIDDEN_FOUND,       2,   2,    3,  "find %d hidden nets",     20 },
    { ChallengeType::HANDSHAKES,         1,   2,    4,  "capture %d handshakes",   40 },
    { ChallengeType::PMKIDS,             1,   2,    3,  "grab %d PMKIDs",          50 },
    { ChallengeType::DEAUTHS,            5,   3,    5,  "land %d deauths",         10 },
    { ChallengeType::GPS_NETWORKS,      15,   2,    4,  "tag %d GPS networks",     20 },
    { ChallengeType::BLE_PACKETS,       50,   3,    5,  "spam %d BLE packets",     15 },
    { ChallengeType::PASSIVE_NETWORKS,  20,   2,    3,  "observe %d silently",     25 },
    { ChallengeType::NO_DEAUTH_STREAK,  15,   2,    3,  "%d nets zero violence",   30 },
    { ChallengeType::DISTANCE_M,       500,   2,    4,  "walk %dm wardriving",     20 },
    { ChallengeType::WPA3_FOUND,         1,   2,    4,  "spot %d WPA3 nets",       15 },
};
static const uint8_t POOL_SIZE = sizeof(CHALLENGE_POOL) / sizeof(CHALLENGE_POOL[0]);

// ============================================================
// PIG AWAKE DETECTION
// menu surfing doesn't count. pig demands real work.
// ============================================================

bool Challenges::isPigAwake() {
    PorkchopMode mode = porkchop.getMode();
    return mode == PorkchopMode::OINK_MODE ||
           mode == PorkchopMode::DNH_MODE ||
           mode == PorkchopMode::WARHOG_MODE ||
           mode == PorkchopMode::PIGGYBLUES_MODE ||
           mode == PorkchopMode::SPECTRUM_MODE;
}

// ============================================================
// GENERATOR
// the pig wakes. the pig demands. three trials await.
// ============================================================

void Challenges::generate() {
    // reset state from previous session
    reset();
    activeCount = 3;
    sessionDeauthed = false;
    
    // pick 3 different templates (no repeats - pig has variety)
    uint8_t picked[3] = {0xFF, 0xFF, 0xFF};
    
    for (int i = 0; i < 3; i++) {
        uint8_t idx;
        bool unique;
        
        // keep rolling until we get a unique template
        do {
            idx = random(0, POOL_SIZE);
            unique = true;
            for (int j = 0; j < i; j++) {
                if (picked[j] == idx) {
                    unique = false;
                    break;
                }
            }
        } while (!unique);
        
        picked[i] = idx;
        
        // difficulty scales with slot: 0=EASY, 1=MEDIUM, 2=HARD
        ChallengeDifficulty diff = static_cast<ChallengeDifficulty>(i);
        const ChallengeTemplate& tmpl = CHALLENGE_POOL[idx];
        
        // calculate target based on difficulty
        uint16_t target = tmpl.easyTarget;
        if (diff == ChallengeDifficulty::MEDIUM) {
            target *= tmpl.mediumMult;
        } else if (diff == ChallengeDifficulty::HARD) {
            target *= tmpl.hardMult;
        }
        
        // calculate XP reward: EASY=base, MEDIUM=2x, HARD=4x
        uint16_t reward = tmpl.xpRewardBase;
        if (diff == ChallengeDifficulty::MEDIUM) {
            reward *= 2;
        } else if (diff == ChallengeDifficulty::HARD) {
            reward *= 4;
        }
        
        // format the challenge name with target value
        ActiveChallenge& ch = challenges[i];
        ch.type = tmpl.type;
        ch.difficulty = diff;
        ch.target = target;
        ch.progress = 0;
        ch.xpReward = reward;
        ch.completed = false;
        ch.failed = false;
        snprintf(ch.name, sizeof(ch.name), tmpl.nameFormat, target);
    }
    
    // pig's demands generated in silence
    // curious users can invoke printToSerial() to see them
}

// ============================================================
// SERIAL OUTPUT
// the pig reveals demands to the worthy. press '1' in IDLE.
// ============================================================

void Challenges::printToSerial() {
    if (activeCount == 0) {
        Serial.println("\n[PIG] no demands. pig sleeps.");
        return;
    }
    
    Serial.println();
    Serial.println("+------------------------------------------+");
    Serial.println("|     PIG WAKES. PIG DEMANDS ACTION.       |");
    Serial.println("+------------------------------------------+");
    
    for (int i = 0; i < activeCount; i++) {
        const ActiveChallenge& ch = challenges[i];
        const char* diffStr = (i == 0) ? "EASY  " : (i == 1) ? "MEDIUM" : "HARD  ";
        const char* status = ch.completed ? "[*]" : ch.failed ? "[X]" : "[ ]";
        
        // Fixed width: 42 chars inside box
        char line[64];
        snprintf(line, sizeof(line), " %s %s %-20s +%3d XP", status, diffStr, ch.name, ch.xpReward);
        Serial.printf("|%-42s|\n", line);
        
        if (!ch.completed && !ch.failed) {
            snprintf(line, sizeof(line), "       progress: %d / %d", ch.progress, ch.target);
            Serial.printf("|%-42s|\n", line);
        }
    }
    
    Serial.println("+------------------------------------------+");
    char summary[64];
    snprintf(summary, sizeof(summary), "           completed: %d / %d", getCompletedCount(), activeCount);
    Serial.printf("|%-42s|\n", summary);
    Serial.println("+------------------------------------------+");
    Serial.println();
}

// ============================================================
// PROGRESS TRACKING
// pig watches. pig judges. pig rewards.
// ============================================================

void Challenges::updateProgress(ChallengeType type, uint16_t delta) {
    for (int i = 0; i < activeCount; i++) {
        ActiveChallenge& ch = challenges[i];
        
        // skip if wrong type, already done, or failed
        if (ch.type != type || ch.completed || ch.failed) continue;
        
        ch.progress += delta;
        
        // the pig judges completion
        if (ch.progress >= ch.target) {
            ch.completed = true;
            ch.progress = ch.target;  // cap at target for display
            
            // reward the peasant (direct XP add - bypasses event to avoid recursion)
            XP::addXP(ch.xpReward);
            
            // pig is pleased. announce it.
            char toast[48];
            snprintf(toast, sizeof(toast), "* %s *", ch.name);
            Display::showToast(toast);
            
            // distinct jingle for challenge complete
            // rising tones: accomplishment achieved
            if (Config::personality().soundEnabled) {
                M5.Speaker.tone(700, 60);
                delay(80);
                M5.Speaker.tone(900, 60);
                delay(80);
                M5.Speaker.tone(1100, 100);
            }
            
            delay(400);  // let user see the toast
            
            Serial.printf("[CHALLENGES] pig pleased. '%s' complete. +%d XP.\\n",
                          ch.name, ch.xpReward);
            
            // Check for full sweep bonus (all 3 completed)
            if (allCompleted()) {
                // TRIPLE THREAT BONUS - pig respects dedication
                const uint16_t BONUS_XP = 100;
                XP::addXP(BONUS_XP);
                
                Display::showToast("* ALL TRIALS COMPLETE *");
                
                // Victory fanfare - triumphant jingle
                if (Config::personality().soundEnabled) {
                    delay(200);
                    M5.Speaker.tone(800, 80);
                    delay(100);
                    M5.Speaker.tone(1000, 80);
                    delay(100);
                    M5.Speaker.tone(1200, 80);
                    delay(100);
                    M5.Speaker.tone(1500, 200);
                }
                
                delay(500);
                
                Serial.println("[CHALLENGES] *** FULL SWEEP! +100 BONUS XP ***");
            }
        }
    }
}

void Challenges::failConditional(ChallengeType type) {
    // deauth sent? peace-lover challenges fail
    for (int i = 0; i < activeCount; i++) {
        ActiveChallenge& ch = challenges[i];
        if (ch.type == type && !ch.completed && !ch.failed) {
            ch.failed = true;
            Serial.printf("[CHALLENGES] '%s' failed. violence detected.\n", ch.name);
        }
    }
}

// ============================================================
// XP EVENT DISPATCHER
// single integration point. maps XPEvents to ChallengeTypes.
// ============================================================

void Challenges::onXPEvent(XPEvent event) {
    // pig sleeps? pig doesn't care about your progress
    if (!isPigAwake()) return;
    
    // no challenges generated yet? nothing to track
    if (activeCount == 0) return;
    
    // map XP events to challenge progress
    switch (event) {
        // network discovery events
        case XPEvent::NETWORK_FOUND:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            if (!sessionDeauthed) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        case XPEvent::NETWORK_HIDDEN:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            updateProgress(ChallengeType::HIDDEN_FOUND, 1);
            if (!sessionDeauthed) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        case XPEvent::NETWORK_WPA3:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            updateProgress(ChallengeType::WPA3_FOUND, 1);
            if (!sessionDeauthed) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        case XPEvent::NETWORK_OPEN:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            updateProgress(ChallengeType::OPEN_FOUND, 1);
            if (!sessionDeauthed) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        case XPEvent::NETWORK_WEP:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            if (!sessionDeauthed) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        // capture events
        case XPEvent::HANDSHAKE_CAPTURED:
            updateProgress(ChallengeType::HANDSHAKES, 1);
            break;
            
        case XPEvent::PMKID_CAPTURED:
            updateProgress(ChallengeType::PMKIDS, 1);
            break;
            
        case XPEvent::DNH_PMKID_GHOST:
            updateProgress(ChallengeType::PMKIDS, 1);
            break;
            
        // deauth events - the violence counter
        case XPEvent::DEAUTH_SUCCESS:
            updateProgress(ChallengeType::DEAUTHS, 1);
            if (!sessionDeauthed) {
                sessionDeauthed = true;
                failConditional(ChallengeType::NO_DEAUTH_STREAK);
            }
            break;
            
        // wardriving events
        case XPEvent::WARHOG_LOGGED:
            updateProgress(ChallengeType::GPS_NETWORKS, 1);
            break;
            
        case XPEvent::DISTANCE_KM:
            // event is per-km, challenge tracks meters
            updateProgress(ChallengeType::DISTANCE_M, 1000);
            break;
            
        // BLE spam events
        case XPEvent::BLE_BURST:
        case XPEvent::BLE_APPLE:
        case XPEvent::BLE_ANDROID:
        case XPEvent::BLE_SAMSUNG:
        case XPEvent::BLE_WINDOWS:
            updateProgress(ChallengeType::BLE_PACKETS, 1);
            break;
            
        // passive mode events
        case XPEvent::DNH_NETWORK_PASSIVE:
            updateProgress(ChallengeType::PASSIVE_NETWORKS, 1);
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            if (!sessionDeauthed) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        default:
            // other events don't affect challenges
            break;
    }
}

// ============================================================
// ACCESSORS
// ============================================================

void Challenges::reset() {
    for (int i = 0; i < 3; i++) {
        challenges[i] = {};
    }
    activeCount = 0;
    sessionDeauthed = false;
}

const ActiveChallenge& Challenges::get(uint8_t idx) {
    if (idx >= 3) idx = 0;
    return challenges[idx];
}

uint8_t Challenges::getActiveCount() {
    return activeCount;
}

uint8_t Challenges::getCompletedCount() {
    uint8_t count = 0;
    for (int i = 0; i < activeCount; i++) {
        if (challenges[i].completed) count++;
    }
    return count;
}

bool Challenges::allCompleted() {
    if (activeCount == 0) return false;
    for (int i = 0; i < activeCount; i++) {
        if (!challenges[i].completed) return false;
    }
    return true;
}
