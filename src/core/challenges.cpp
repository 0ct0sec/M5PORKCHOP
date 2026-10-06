// Session Challenges Implementation
// pig demands action. pig tracks progress. pig rewards effort.

#include "challenges.h"
#include "config.h"
#include "../ui/display.h"
#include "../audio/sfx.h"
#include "../piglet/avatar.h"
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

// porkchop global instance lives in main.cpp
extern Porkchop porkchop;

// static member initialization
ActiveChallenge Challenges::challenges[3] = {};
uint8_t Challenges::activeCount = 0;
uint8_t Challenges::sessionDeauthCount = 0;

static portMUX_TYPE challengesMux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t clampToU16(uint32_t value) {
    return value > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(value);
}

// ============================================================
// CHALLENGE TEMPLATE POOL
// the pig's menu of demands. 18 options, 3 chosen per session.
// ============================================================

struct ChallengeTemplate {
    ChallengeType type;
    uint16_t easyTarget;     // base target for EASY
    uint8_t mediumMult;      // multiplier for MEDIUM (2-3x)
    uint8_t hardMult;        // multiplier for HARD (4-6x)
    const char* nameFormat;  // printf format with %d for target
    uint8_t xpRewardBase;    // base XP reward (scaled by difficulty)
    bool requiresGPS;        // only available when GPS enabled
};

// pig's demands are varied but fair (mostly)
// NOTE: name format must be <=18 chars after %d substitution for UI fit
static const ChallengeTemplate CHALLENGE_POOL[] = {
    // type                              easy  med  hard  name format                xp   gps?
    { ChallengeType::NETWORKS_FOUND,      25,   2,    4,  "inhale %d nets",          15, false },
    { ChallengeType::NETWORKS_FOUND,      50,   2,    3,  "discover %d APs",         25, false },
    { ChallengeType::HIDDEN_FOUND,         2,   2,    3,  "expose %d hidden",        20, false },
    { ChallengeType::HANDSHAKES,           1,   2,    4,  "snatch %d shakes",        40, false },
    { ChallengeType::HANDSHAKES,           2,   2,    3,  "pwn %d targets",          50, false },
    { ChallengeType::PMKIDS,               1,   2,    3,  "swipe %d PMKIDs",         50, false },
    { ChallengeType::DEAUTHS,              5,   3,    5,  "drop %d deauths",         10, false },
    { ChallengeType::DEAUTHS,             10,   2,    4,  "evict %d peasants",       15, false },
    { ChallengeType::GPS_NETWORKS,        15,   2,    4,  "tag %d GPS nets",         20,  true },
    { ChallengeType::GPS_NETWORKS,        30,   2,    3,  "geotag %d signals",       25,  true },
    { ChallengeType::BLE_PACKETS,         50,   3,    5,  "spam %d BLE pkts",        15, false },
    { ChallengeType::BLE_PACKETS,        150,   2,    3,  "serve %d BLE",            20, false },
    { ChallengeType::PASSIVE_NETWORKS,    20,   2,    3,  "lurk %d silently",        25, false },
    { ChallengeType::NO_DEAUTH_STREAK,    15,   2,    3,  "%d nets no deauth",       30, false },
    { ChallengeType::DISTANCE_M,         500,   2,    4,  "trot %dm hunting",        20,  true },
    { ChallengeType::DISTANCE_M,        1000,   2,    3,  "stomp %dm total",         25,  true },
    { ChallengeType::WPA3_FOUND,           1,   2,    4,  "spot %d WPA3 nets",       15, false },
    { ChallengeType::OPEN_FOUND,           3,   2,    3,  "find %d open nets",       15, false },
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
    
    bool gpsEnabled = Config::gps().enabled;
    
    // pick 3 different templates (no repeats, no duplicate types)
    uint8_t picked[3] = {0xFF, 0xFF, 0xFF};
    ChallengeType pickedTypes[3] = {};
    
    for (int i = 0; i < 3; i++) {
        uint8_t idx;
        bool valid;
        int attempts = 0;
        const int maxAttempts = 50;  // prevent infinite loop
        
        // keep rolling until we get a valid unique template
        do {
            idx = random(0, POOL_SIZE);
            valid = true;
            
            const ChallengeTemplate& candidate = CHALLENGE_POOL[idx];
            
            // Skip GPS-required challenges if GPS disabled
            if (candidate.requiresGPS && !gpsEnabled) {
                valid = false;
                attempts++;
                continue;
            }
            
            // Check for duplicate template index
            for (int j = 0; j < i; j++) {
                if (picked[j] == idx) {
                    valid = false;
                    break;
                }
            }
            
            // Check for duplicate ChallengeType (pig wants variety!)
            if (valid) {
                for (int j = 0; j < i; j++) {
                    if (pickedTypes[j] == candidate.type) {
                        valid = false;
                        break;
                    }
                }
            }
            
            attempts++;
        } while (!valid && attempts < maxAttempts);

        if (!valid) {
            // Fallback: pick any GPS-compatible template if the uniqueness rules fail
            for (uint8_t j = 0; j < POOL_SIZE; j++) {
                const ChallengeTemplate& candidate = CHALLENGE_POOL[j];
                if (candidate.requiresGPS && !gpsEnabled) continue;
                idx = j;
                valid = true;
                break;
            }
        }

        if (!valid) {
            // Final fallback if pool is empty (should never happen)
            idx = 0;
        }
        
        picked[i] = idx;
        pickedTypes[i] = CHALLENGE_POOL[idx].type;
        
        // difficulty scales with slot: 0=EASY, 1=MEDIUM, 2=HARD
        ChallengeDifficulty diff = static_cast<ChallengeDifficulty>(i);
        const ChallengeTemplate& tmpl = CHALLENGE_POOL[idx];
        
        // calculate target based on difficulty
        uint16_t target = tmpl.easyTarget;
        if (diff == ChallengeDifficulty::MEDIUM) {
            target = clampToU16(static_cast<uint32_t>(target) * tmpl.mediumMult);
        } else if (diff == ChallengeDifficulty::HARD) {
            target = clampToU16(static_cast<uint32_t>(target) * tmpl.hardMult);
        }
        
        // ============ LEVEL SCALING ============
        // pig's demands grow with power
        // L1-10: 1.0x, L11-20: 1.5x, L21-30: 2.0x, L31-40: 3.0x
        uint8_t level = XP::getLevel();
        if (level >= 31) {
            target = clampToU16(static_cast<uint32_t>(target) * 3);          // 3.0x
        } else if (level >= 21) {
            target = clampToU16(static_cast<uint32_t>(target) * 2);          // 2.0x
        } else if (level >= 11) {
            target = clampToU16((static_cast<uint32_t>(target) * 3) / 2);      // 1.5x
        }
        // L1-10 stays at 1.0x (no change)
        
        // calculate XP reward: EASY=base, MEDIUM=2x, HARD=4x
        uint16_t reward = tmpl.xpRewardBase;
        if (diff == ChallengeDifficulty::MEDIUM) {
            reward = clampToU16(static_cast<uint32_t>(reward) * 2);
        } else if (diff == ChallengeDifficulty::HARD) {
            reward = clampToU16(static_cast<uint32_t>(reward) * 4);
        }
        
        // ============ REWARD SCALING ============
        // pig rewards scale with pig demands (same multipliers)
        if (level >= 31) {
            reward = clampToU16(static_cast<uint32_t>(reward) * 3);          // 3.0x
        } else if (level >= 21) {
            reward = clampToU16(static_cast<uint32_t>(reward) * 2);          // 2.0x
        } else if (level >= 11) {
            reward = clampToU16((static_cast<uint32_t>(reward) * 3) / 2);      // 1.5x
        }
        
        // format the challenge name with target value
        portENTER_CRITICAL(&challengesMux);
        ActiveChallenge& ch = challenges[i];
        ch.type = tmpl.type;
        ch.difficulty = diff;
        ch.target = target;
        ch.progress = 0;
        ch.xpReward = reward;
        ch.completed = false;
        ch.failed = false;
        snprintf(ch.name, sizeof(ch.name), tmpl.nameFormat, target);
        portEXIT_CRITICAL(&challengesMux);
    }

    portENTER_CRITICAL(&challengesMux);
    activeCount = 3;
    sessionDeauthCount = 0;
    portEXIT_CRITICAL(&challengesMux);
    
    // pig's demands generated in silence
    // curious users can invoke printToSerial() to see them
}

// ============================================================
// SERIAL OUTPUT
// the pig reveals demands to the worthy. press '1' in IDLE.
// ============================================================

void Challenges::printToSerial() {
    uint8_t localActive = 0;
    portENTER_CRITICAL(&challengesMux);
    localActive = activeCount;
    portEXIT_CRITICAL(&challengesMux);

    if (localActive == 0) {
        Serial.println("\n[PIG] no demands. pig sleeps.");
        return;
    }
    
    Serial.println();
    Serial.println("+------------------------------------------+");
    Serial.println("|     PIG WAKES. PIG DEMANDS ACTION.       |");
    Serial.println("+------------------------------------------+");
    
    for (int i = 0; i < localActive; i++) {
        ActiveChallenge ch = {};
        if (!getSnapshot(i, ch)) {
            continue;
        }
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
    snprintf(summary, sizeof(summary), "           completed: %d / %d", getCompletedCount(), localActive);
    Serial.printf("|%-42s|\n", summary);
    Serial.println("+------------------------------------------+");
    Serial.println();
}

// ============================================================
// PROGRESS TRACKING
// pig watches. pig judges. pig rewards.
// ============================================================

void Challenges::updateProgress(ChallengeType type, uint16_t delta) {
    struct CompletionNotice {
        ChallengeDifficulty difficulty;
        uint16_t xpReward;
        char name[sizeof(ActiveChallenge::name)];
    };

    CompletionNotice notices[3] = {};
    uint8_t noticeCount = 0;
    bool sweepNow = false;

    portENTER_CRITICAL(&challengesMux);
    uint8_t localActive = activeCount;
    for (int i = 0; i < localActive; i++) {
        ActiveChallenge& ch = challenges[i];

        // skip if wrong type, already done, or failed
        if (ch.type != type || ch.completed || ch.failed) continue;

        uint32_t nextProgress = ch.progress;
        nextProgress = (nextProgress + delta > 0xFFFF) ? 0xFFFF : (nextProgress + delta);
        ch.progress = static_cast<uint16_t>(nextProgress);

        // the pig judges completion
        if (ch.progress >= ch.target) {
            ch.completed = true;
            ch.progress = ch.target;  // cap at target for display

            if (noticeCount < 3) {
                notices[noticeCount].difficulty = ch.difficulty;
                notices[noticeCount].xpReward = ch.xpReward;
                snprintf(notices[noticeCount].name, sizeof(notices[noticeCount].name), "%s", ch.name);
                noticeCount++;
            }
        }
    }

    if (noticeCount > 0 && localActive > 0) {
        sweepNow = true;
        for (int i = 0; i < localActive; i++) {
            if (!challenges[i].completed) {
                sweepNow = false;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&challengesMux);

    for (uint8_t i = 0; i < noticeCount; i++) {
        // reward the peasant (silent add - challenge toast/sound is the celebration)
        XP::addXPSilent(notices[i].xpReward);

        // difficulty-specific toast messages
        const char* toastMsg;
        switch (notices[i].difficulty) {
            case ChallengeDifficulty::EASY:   toastMsg = "FIRST BLOOD. PIG STIRS."; break;
            case ChallengeDifficulty::MEDIUM: toastMsg = "PROGRESS NOTED. PIG LISTENS."; break;
            case ChallengeDifficulty::HARD:   toastMsg = "BRUTAL. PIG RESPECTS."; break;
            default:                          toastMsg = "PIG APPROVES."; break;
        }
        Display::showToast(toastMsg);

        // Rising tones for challenge complete - non-blocking
        SFX::play(SFX::CHALLENGE_COMPLETE);
        Avatar::triggerTailWiggle();

        Serial.printf("[CHALLENGES] pig pleased. '%s' complete. +%d XP.\\n",
                      notices[i].name, notices[i].xpReward);
    }

    // Check for full sweep bonus (all 3 completed)
    if (sweepNow) {
        // TRIPLE THREAT BONUS - pig respects dedication (scales with mastery)
        const uint16_t BONUS_XP = 50 + (XP::getLevel() * 8);
        XP::addXPSilent(BONUS_XP);  // Silent add - sweep fanfare is the celebration

        Display::showToast("WORTHY. 115200 REMEMBERS.");

        // Victory fanfare - non-blocking (priority sound, interrupts CHALLENGE_COMPLETE)
        SFX::play(SFX::CHALLENGE_SWEEP);

        Serial.printf("[CHALLENGES] *** FULL SWEEP! +%d BONUS XP ***\n", BONUS_XP);
    }
}

void Challenges::failConditional(ChallengeType type) {
    // deauth sent? peace-lover challenges fail
    char failedName[sizeof(ActiveChallenge::name)] = {};
    bool failedLogged = false;

    portENTER_CRITICAL(&challengesMux);
    uint8_t localActive = activeCount;
    for (int i = 0; i < localActive; i++) {
        ActiveChallenge& ch = challenges[i];
        if (ch.type == type && !ch.completed && !ch.failed) {
            ch.failed = true;
            if (!failedLogged) {
                snprintf(failedName, sizeof(failedName), "%s", ch.name);
                failedLogged = true;
            }
        }
    }
    portEXIT_CRITICAL(&challengesMux);

    if (failedLogged) {
        Serial.printf("[CHALLENGES] '%s' failed. violence detected.\n", failedName);
    }
}

// ============================================================
// XP EVENT DISPATCHER
// single integration point. maps XPEvents to ChallengeTypes.
// ============================================================

void Challenges::onXPEvent(XPEvent event) {
    // pig sleeps? pig doesn't care about your progress
    if (!isPigAwake()) return;

    uint8_t deauthCountSnapshot = 0;
    uint8_t localActive = 0;
    portENTER_CRITICAL(&challengesMux);
    localActive = activeCount;
    deauthCountSnapshot = sessionDeauthCount;
    portEXIT_CRITICAL(&challengesMux);

    // no challenges generated yet? nothing to track
    if (localActive == 0) return;
    
    // map XP events to challenge progress
    switch (event) {
        // network discovery events
        case XPEvent::NETWORK_FOUND:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            if (deauthCountSnapshot < 2) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        case XPEvent::NETWORK_HIDDEN:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            updateProgress(ChallengeType::HIDDEN_FOUND, 1);
            if (deauthCountSnapshot < 2) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        case XPEvent::NETWORK_WPA3:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            updateProgress(ChallengeType::WPA3_FOUND, 1);
            if (deauthCountSnapshot < 2) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        case XPEvent::NETWORK_OPEN:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            updateProgress(ChallengeType::OPEN_FOUND, 1);
            if (deauthCountSnapshot < 2) {
                updateProgress(ChallengeType::NO_DEAUTH_STREAK, 1);
            }
            break;
            
        case XPEvent::NETWORK_WEP:
            updateProgress(ChallengeType::NETWORKS_FOUND, 1);
            if (deauthCountSnapshot < 2) {
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
            
        // deauth events - the violence counter (grace: 1 free, fail on 2nd)
        case XPEvent::DEAUTH_SUCCESS:
            updateProgress(ChallengeType::DEAUTHS, 1);
            if (deauthCountSnapshot < 2) {
                bool shouldFail = false;
                portENTER_CRITICAL(&challengesMux);
                sessionDeauthCount++;
                if (sessionDeauthCount >= 2) {
                    shouldFail = true;
                }
                portEXIT_CRITICAL(&challengesMux);
                if (shouldFail) {
                    failConditional(ChallengeType::NO_DEAUTH_STREAK);
                }
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
            if (deauthCountSnapshot < 2) {
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
    portENTER_CRITICAL(&challengesMux);
    for (int i = 0; i < 3; i++) {
        challenges[i] = {};
    }
    activeCount = 0;
    sessionDeauthCount = 0;
    portEXIT_CRITICAL(&challengesMux);
}

bool Challenges::getSnapshot(uint8_t idx, ActiveChallenge& out) {
    if (idx >= 3) idx = 0;
    portENTER_CRITICAL(&challengesMux);
    if (idx >= activeCount) {
        portEXIT_CRITICAL(&challengesMux);
        return false;
    }
    out = challenges[idx];
    portEXIT_CRITICAL(&challengesMux);
    return true;
}

uint8_t Challenges::getActiveCount() {
    portENTER_CRITICAL(&challengesMux);
    uint8_t count = activeCount;
    portEXIT_CRITICAL(&challengesMux);
    return count;
}

uint8_t Challenges::getCompletedCount() {
    uint8_t count = 0;
    portENTER_CRITICAL(&challengesMux);
    uint8_t localActive = activeCount;
    for (int i = 0; i < localActive; i++) {
        if (challenges[i].completed) count++;
    }
    portEXIT_CRITICAL(&challengesMux);
    return count;
}

bool Challenges::allCompleted() {
    portENTER_CRITICAL(&challengesMux);
    if (activeCount == 0) {
        portEXIT_CRITICAL(&challengesMux);
        return false;
    }
    uint8_t localActive = activeCount;
    for (int i = 0; i < localActive; i++) {
        if (!challenges[i].completed) {
            portEXIT_CRITICAL(&challengesMux);
            return false;
        }
    }
    portEXIT_CRITICAL(&challengesMux);
    return true;
}
