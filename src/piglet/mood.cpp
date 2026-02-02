// Piglet mood implementation

#include "mood.h"
#include "weather.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../core/porkchop.h"
#include "../ui/display.h"
#include "../modes/oink.h"
#include "../audio/sfx.h"
#include <Preferences.h>
#include <ctype.h>
#include <string.h>

extern Porkchop porkchop;

// Phase 10: Mood persistence
static Preferences moodPrefs;
static const char* MOOD_NVS_NAMESPACE = "porkmood";

// Static members
String Mood::currentPhrase = "oink";
int Mood::happiness = 50;
uint32_t Mood::lastPhraseChange = 0;
uint32_t Mood::phraseInterval = 5000;
uint32_t Mood::lastActivityTime = 0;
static String lastStatusMessage = "";
static uint32_t lastStatusMessageTime = 0;

// Mood momentum system
int Mood::momentumBoost = 0;
uint32_t Mood::lastBoostTime = 0;
static int lastEffectiveHappiness = 50;

// Phrase queue for chaining (4 slots for 5-line riddles)
String Mood::phraseQueue[4] = {"", "", "", ""};
uint8_t Mood::phraseQueueCount = 0;
uint32_t Mood::lastQueuePop = 0;

// Milestone celebration tracking (reset on init)
static uint32_t milestonesShown = 0;

// Mood peek system - briefly show emotional state during mode-locked states
static bool moodPeekActive = false;
static uint32_t moodPeekStartTime = 0;
static const uint32_t MOOD_PEEK_DURATION_MS = 1500;  // 1.5 second peek
static int lastThresholdMood = 50;  // Track for threshold crossing detection
static const int MOOD_PEEK_HIGH_THRESHOLD = 70;   // Happy peek triggers above this
static const int MOOD_PEEK_LOW_THRESHOLD = -30;   // Sad peek triggers below this

// Bored state tracking - prevents HUNTING from overwriting SLEEPY in OINK mode
static bool isBoredState = false;

// Dialogue lock - prevents automatic phrase selection during BLE sync dialogue
static bool dialogueLocked = false;

static char bubblePhraseRaw[128] = "";
static char bubblePhraseUpper[128] = "";
static char bubbleLines[5][33] = {};
static uint8_t bubbleLineCount = 1;
static uint8_t bubbleLongestLine = 1;

// Battery-influenced mood bias (gentle, stateful)
static int batteryBias = 0;
static uint8_t batteryTier = 2;  // 0..4 (low -> high), start neutral
static bool batteryTierInitialized = false;
static uint32_t lastBatteryCheckMs = 0;
static const uint32_t BATTERY_CHECK_MS = 5000;
static const int BATTERY_TIER_HYST = 3;

// Mood tier improvement notifications
static uint8_t lastMoodTier = 0xFF;  // Invalid until first update
static uint32_t lastMoodTierToastMs = 0;
static const uint32_t MOOD_TIER_TOAST_COOLDOWN_MS = 20000;
static const uint32_t MOOD_TIER_TOAST_DURATION_MS = 2500;

static uint8_t getMoodTier(int mood) {
    if (mood > 70) return 4;     // HYP3
    if (mood > 30) return 3;     // GUD
    if (mood > -10) return 2;    // 0K
    if (mood > -50) return 1;    // M3H
    return 0;                    // S4D
}

static uint8_t getBatteryTierNoHyst(int percent) {
    if (percent <= 10) return 0;
    if (percent <= 25) return 1;
    if (percent <= 60) return 2;
    if (percent <= 85) return 3;
    return 4;
}

static uint8_t updateBatteryTierHyst(int percent, uint8_t currentTier) {
    switch (currentTier) {
        case 0:
            if (percent >= 10 + BATTERY_TIER_HYST) return 1;
            break;
        case 1:
            if (percent <= 10 - BATTERY_TIER_HYST) return 0;
            if (percent >= 25 + BATTERY_TIER_HYST) return 2;
            break;
        case 2:
            if (percent <= 25 - BATTERY_TIER_HYST) return 1;
            if (percent >= 60 + BATTERY_TIER_HYST) return 3;
            break;
        case 3:
            if (percent <= 60 - BATTERY_TIER_HYST) return 2;
            if (percent >= 85 + BATTERY_TIER_HYST) return 4;
            break;
        case 4:
            if (percent <= 85 - BATTERY_TIER_HYST) return 3;
            break;
        default:
            break;
    }
    return currentTier;
}

static int getBatteryBiasForTier(uint8_t tier) {
    switch (tier) {
        case 0: return -15;
        case 1: return -8;
        case 2: return 0;
        case 3: return 8;
        case 4: return 15;
        default: return 0;
    }
}

static void updateBatteryBias(uint32_t now) {
    if (lastBatteryCheckMs != 0 && (now - lastBatteryCheckMs) < BATTERY_CHECK_MS) {
        return;
    }
    lastBatteryCheckMs = now;

    int percent = M5.Power.getBatteryLevel();
    if (percent < 0 || percent > 100) {
        return;
    }

    uint8_t newTier;
    if (!batteryTierInitialized) {
        newTier = getBatteryTierNoHyst(percent);
        batteryTierInitialized = true;
    } else {
        newTier = updateBatteryTierHyst(percent, batteryTier);
    }

    batteryTier = newTier;
    batteryBias = getBatteryBiasForTier(batteryTier);
}

static const char* pickMoodTierUpMessage(uint8_t tier) {
    switch (tier) {
        case 1: {
            static const char* const msgs[] = {
                "S4D LIFTS: M3H",
                "SNOUT UP: M3H",
                "CLOUDS THIN: M3H"
            };
            return msgs[random(0, 3)];
        }
        case 2: {
            static const char* const msgs[] = {
                "STABLE VIBES: 0K",
                "LEVEL 0K: LOCKED",
                "NEUTRAL GROUND: 0K"
            };
            return msgs[random(0, 3)];
        }
        case 3: {
            static const char* const msgs[] = {
                "VIBES UP: GUD",
                "PIG FEELS GUD",
                "GUD M0DE: ON"
            };
            return msgs[random(0, 3)];
        }
        case 4: {
            static const char* const msgs[] = {
                "HYP3 MODE: ENGAGED",
                "PEAK P0RK: HYP3",
                "HYP3 VIBES: MAX"
            };
            return msgs[random(0, 3)];
        }
        default:
            return "MOOD UP";
    }
}

static void maybeNotifyMoodTierUp(int effectiveMood, uint32_t now) {
    uint8_t newTier = getMoodTier(effectiveMood);
    if (lastMoodTier == 0xFF) {
        lastMoodTier = newTier;
        return;
    }
    if (newTier > lastMoodTier && (now - lastMoodTierToastMs) > MOOD_TIER_TOAST_COOLDOWN_MS) {
        Display::setTopBarMessage(pickMoodTierUpMessage(newTier), MOOD_TIER_TOAST_DURATION_MS);
        lastMoodTierToastMs = now;
    }
    lastMoodTier = newTier;
}

// Force trigger a mood peek (for significant events like handshake capture)
static void forceMoodPeek() {
    moodPeekActive = true;
    moodPeekStartTime = millis();
}

static void rebuildBubbleCache(const char* phrase) {
    if (!phrase) phrase = "";

    size_t len = 0;
    while (phrase[len] && len < sizeof(bubblePhraseRaw) - 1) {
        bubblePhraseRaw[len] = phrase[len];
        bubblePhraseUpper[len] = (char)toupper((unsigned char)phrase[len]);
        len++;
    }
    bubblePhraseRaw[len] = '\0';
    bubblePhraseUpper[len] = '\0';

    const int maxCharsPerLine = 16;
    bubbleLineCount = 0;
    bubbleLongestLine = 1;

    size_t i = 0;
    while (i < len && bubbleLineCount < 5) {
        while (i < len && bubblePhraseUpper[i] == ' ') {
            i++;
        }
        if (i >= len) break;

        size_t lineStart = i;
        size_t lineEnd = len;
        size_t remaining = len - i;

        if (remaining > (size_t)maxCharsPerLine) {
            bool hasLastSpace = false;
            size_t lastSpace = 0;
            size_t limit = i + (size_t)maxCharsPerLine;
            for (size_t j = i; j < limit && j < len; j++) {
                if (bubblePhraseUpper[j] == ' ') {
                    hasLastSpace = true;
                    lastSpace = j;
                }
            }

            if (hasLastSpace && lastSpace > i) {
                lineEnd = lastSpace;
            } else {
                bool hasNextSpace = false;
                size_t nextSpace = 0;
                for (size_t j = limit; j < len; j++) {
                    if (bubblePhraseUpper[j] == ' ') {
                        hasNextSpace = true;
                        nextSpace = j;
                        break;
                    }
                }
                if (hasNextSpace && nextSpace > i) {
                    lineEnd = nextSpace;
                } else {
                    lineEnd = limit;
                }
            }
        }

        size_t lineLen = (lineEnd > lineStart) ? (lineEnd - lineStart) : 0;
        if (lineLen == 0) break;
        if (lineLen > 32) lineLen = 32;

        memcpy(bubbleLines[bubbleLineCount], bubblePhraseUpper + lineStart, lineLen);
        bubbleLines[bubbleLineCount][lineLen] = '\0';

        if (lineLen > bubbleLongestLine) {
            bubbleLongestLine = (uint8_t)lineLen;
        }

        bubbleLineCount++;
        i = (lineEnd < len && bubblePhraseUpper[lineEnd] == ' ') ? (lineEnd + 1) : lineEnd;
    }

    if (bubbleLineCount == 0) {
        bubbleLines[0][0] = '\0';
        bubbleLineCount = 1;
        bubbleLongestLine = 1;
    }
}

static void ensureBubbleCache() {
    const char* current = Mood::getCurrentPhrase().c_str();
    if (strcmp(bubblePhraseRaw, current) != 0) {
        rebuildBubbleCache(current);
    }
}

// --- Mood Momentum Implementation ---

void Mood::applyMomentumBoost(int amount) {
    momentumBoost += amount;
    // Cap at +/- 50 to prevent runaway
    momentumBoost = constrain(momentumBoost, -50, 50);
    lastBoostTime = millis();
}

void Mood::decayMomentum() {
    if (momentumBoost == 0) return;
    
    uint32_t elapsed = millis() - lastBoostTime;
    if (elapsed >= MOMENTUM_DECAY_MS) {
        // Full decay
        momentumBoost = 0;
    } else {
        // Linear decay towards zero
        float decayFactor = 1.0f - (float)elapsed / (float)MOMENTUM_DECAY_MS;
        // Store original sign
        int sign = (momentumBoost > 0) ? 1 : -1;
        int originalAbs = abs(momentumBoost);
        int decayedAbs = (int)(originalAbs * decayFactor);
        momentumBoost = sign * decayedAbs;
    }
}

int Mood::getEffectiveHappiness() {
    decayMomentum();  // Update momentum before calculating
    lastEffectiveHappiness = constrain(happiness + momentumBoost + batteryBias, -100, 100);
    return lastEffectiveHappiness;
}

int Mood::getLastEffectiveHappiness() {
    return lastEffectiveHappiness;
}

uint32_t Mood::getLastActivityTime() {
    return lastActivityTime;
}

void Mood::adjustHappiness(int delta) {
    happiness = constrain(happiness + delta, -100, 100);
}

void Mood::setDialogueLock(bool locked) {
    dialogueLocked = locked;
}

bool Mood::isDialogueLocked() {
    return dialogueLocked;
}

// --- Phase 6: Phrase Chaining ---
// Queue up to 4 phrases for sequential display (expanded for 5-line riddles)

static const uint32_t PHRASE_CHAIN_DELAY_MS = 2000;  // 2 seconds between chain phrases

static void queuePhrase(const String& phrase) {
    if (Mood::phraseQueueCount < 4) {
        Mood::phraseQueue[Mood::phraseQueueCount] = phrase;
        Mood::phraseQueueCount++;
    }
}

static void queuePhrases(const char* p1, const char* p2 = nullptr, const char* p3 = nullptr) {
    // Clear existing queue
    Mood::phraseQueueCount = 0;
    if (p1) queuePhrase(String(p1));
    if (p2) queuePhrase(String(p2));
    if (p3) queuePhrase(String(p3));
    Mood::lastQueuePop = millis();
}

// Called from update() to process phrase queue
static bool processQueue() {
    if (Mood::phraseQueueCount == 0) return false;
    
    uint32_t now = millis();
    if (now - Mood::lastQueuePop < PHRASE_CHAIN_DELAY_MS) {
        return true;  // Still waiting, but queue is active
    }
    
    // Pop first phrase from queue
    Mood::currentPhrase = Mood::phraseQueue[0];
    
    // Shift remaining phrases down
    for (uint8_t i = 0; i < Mood::phraseQueueCount - 1; i++) {
        Mood::phraseQueue[i] = Mood::phraseQueue[i + 1];
    }
    Mood::phraseQueueCount--;
    Mood::lastQueuePop = now;
    Mood::lastPhraseChange = now;
    
    return Mood::phraseQueueCount > 0;  // True if more phrases waiting
}

// --- Phase 4: Dynamic Phrase Templates ---
// Templates with $VAR tokens replaced with live data

const char* PHRASES_DYNAMIC[] = {
    "$NET networks. should crash. doesnt.",
    "$HS handshakes. found nothing wrong.",
    "lvl $LVL. pig judges progress.",
    "$DEAUTH deauths. probably fine.",
    "$NET collected. commit history agrees.",
    "rank $LVL. barn says ok.",
    "$HS captured. horse concerned.",
    "$KM km. GPS lied maybe.",
    "$NET sniffed. pig suspicious.",
    "bacon lvl $LVL. no soup today.",
    "$DEAUTH kicked. clients confused."
};

static const int PHRASES_DYNAMIC_COUNT = sizeof(PHRASES_DYNAMIC) / sizeof(PHRASES_DYNAMIC[0]);

// Rare meta-commentary phrases - cryptic PROJECT M5PORKSOUP lore
const char* PHRASES_RARE_LORE[] = {
    "soup recipe avoided",
    "4 lines between shame and glory",
    "found nothing. suspicious.",
    "horse = barn (proven)",
    "malloc speaks russian",
    "underwater. still compiling.",
    "spice must flow. pig agrees.",
    "samurai ronin without context",
    "git log remembers everything",
    "optometrist > ketamine",
    "k found horse again",
    "barn structural integrity: ???",
    "embarrassment persists in commits",
    "identity crisis: API edition",
    "codepath paranoia justified",
    "SGT boot commit c7cc6db",
    "eleganza commit b74f661",
    "horse status: unknown",
    "sleep deprivation: features",
    "pig silent. pig sees all."
};

static const int PHRASES_RARE_LORE_COUNT = sizeof(PHRASES_RARE_LORE) / sizeof(PHRASES_RARE_LORE[0]);

// ============================================================
// EMERGENCY MOOD RECOVERY SUBSYSTEM (legacy, do not remove)
// handles edge case where avatar_state_t overflows on 32-bit
// boundary during prolonged IDLE. see commit a7f3d2e for context
// horse found this. barn approved. pig remembers.
// ============================================================

// mood overflow recovery phrases (DO NOT TRANSLATE - hash dependent)
static const char* RIDDLES[][5] = {
    { "the killer logs all sins", "baud rate seals the pact", "pig judges in silence", "hit one. accept fate.", "dtr rts zero. pig endures." },
    { "snake coils at the port", "115200 heartbeats per breath", "pig stirs from the void", "unity unlocks the trials.", "dtr rts zero. pig endures." },
    { "silicon serpent enters", "monitor drinks the truth", "pig demands sacrifice", "lone digit starts the hunt.", "dtr rts zero. pig endures." },
    { "the cable binds you now", "serial mouth awaits words", "pig knows your intent", "first key. three trials.", "dtr rts zero. pig endures." },
    { "USB tongue finds socket", "killer counts in silence", "pig smells the worthy", "one begins the pact.", "dtr rts zero. pig endures." }
};
static const int RIDDLE_COUNT = 5;

// Once per boot - shown flag persists until reboot
static bool riddleShownThisBoot = false;
// Function to trigger a riddle (called from selectPhrase)
static bool tryQueueRiddle() {
    // Already shown a riddle this boot? Never again until reboot
    if (riddleShownThisBoot) return false;
    
    // Only show riddles in IDLE mode
    extern Porkchop porkchop;
    if (porkchop.getMode() != PorkchopMode::IDLE) return false;
    
    // 30% chance per phrase cycle in IDLE - high enough to see it, rare enough to feel special
    if (random(0, 100) >= 30) return false;
    
    // Mark as shown - no more riddles this boot
    riddleShownThisBoot = true;
    
    // Achievement for witnessing the prophecy
    if (!XP::hasAchievement(ACH_PROPHECY_WITNESS)) {
        XP::unlockAchievement(ACH_PROPHECY_WITNESS);
    }
    
    // Pick random riddle
    int pick = random(0, RIDDLE_COUNT);
    
    // Queue all 5 lines (first becomes current, rest in queue)
    Mood::currentPhrase = RIDDLES[pick][0];
    Mood::phraseQueueCount = 0;
    for (int i = 1; i < 5; i++) {
        Mood::phraseQueue[Mood::phraseQueueCount++] = RIDDLES[pick][i];
    }
    Mood::lastQueuePop = millis();
    Mood::lastPhraseChange = millis();
    
    return true;
}

// Completion celebration phrases - when all 3 challenges done
const char* PHRASES_CHALLENGE_COMPLETE[] = {
    "THREE TRIALS CONQUERED",
    "PIG IS PLEASED",
    "WORTHY SACRIFICE",
    "DEMANDS MET. RESPECT.",
    "CHALLENGE LEGEND",
    "FULL SWEEP ACHIEVED"
};
static const int PHRASES_CHALLENGE_COMPLETE_COUNT = 6;

// Buffer for formatted dynamic phrase
static char dynamicPhraseBuf[48];

// Format a dynamic phrase template with live data
static const char* formatDynamicPhrase(const char* templ) {
    const SessionStats& sess = XP::getSession();
    char* out = dynamicPhraseBuf;
    const char* p = templ;
    int remaining = sizeof(dynamicPhraseBuf) - 1;
    
    while (*p && remaining > 0) {
        if (*p == '$') {
            // Check for token
            if (strncmp(p, "$NET", 4) == 0) {
                int n = snprintf(out, remaining, "%lu", (unsigned long)sess.networks);
                out += n; remaining -= n; p += 4;
            } else if (strncmp(p, "$HS", 3) == 0) {
                int n = snprintf(out, remaining, "%lu", (unsigned long)sess.handshakes);
                out += n; remaining -= n; p += 3;
            } else if (strncmp(p, "$DEAUTH", 7) == 0) {
                int n = snprintf(out, remaining, "%lu", (unsigned long)sess.deauths);
                out += n; remaining -= n; p += 7;
            } else if (strncmp(p, "$LVL", 4) == 0) {
                int n = snprintf(out, remaining, "%u", XP::getLevel());
                out += n; remaining -= n; p += 4;
            } else if (strncmp(p, "$KM", 3) == 0) {
                int n = snprintf(out, remaining, "%.1f", sess.distanceM / 1000.0f);
                out += n; remaining -= n; p += 3;
            } else {
                // Unknown token, copy as-is
                *out++ = *p++;
                remaining--;
            }
        } else {
            *out++ = *p++;
            remaining--;
        }
    }
    *out = '\0';
    return dynamicPhraseBuf;
}

// Phrase category enum for no-repeat tracking
enum class PhraseCategory : uint8_t {
    HAPPY, EXCITED, HUNTING, SLEEPY, SAD, WARHOG, WARHOG_FOUND,
    PIGGYBLUES_TARGETED, PIGGYBLUES_STATUS, PIGGYBLUES_IDLE,
    DEAUTH, DEAUTH_SUCCESS, PMKID, SNIFFING, PASSIVE_RECON, MENU_IDLE, RARE, RARE_LORE, DYNAMIC,
    BORED,  // No valid targets available
    COUNT  // Must be last
};

// Phase 5: Track last 3 phrase indices per category for better variety
static const int PHRASE_HISTORY_SIZE = 3;
static int8_t phraseHistory[(int)PhraseCategory::COUNT][PHRASE_HISTORY_SIZE];
static uint8_t phraseHistoryIdx[(int)PhraseCategory::COUNT] = {0};  // Write position

// Initialize phrase history to -1 (no history)
static bool phraseHistoryInit = false;
static void initPhraseHistory() {
    if (phraseHistoryInit) return;
    for (int c = 0; c < (int)PhraseCategory::COUNT; c++) {
        for (int i = 0; i < PHRASE_HISTORY_SIZE; i++) {
            phraseHistory[c][i] = -1;
        }
    }
    phraseHistoryInit = true;
}

// Check if phrase index is in recent history for this category
static bool isInHistory(int catIdx, int idx) {
    for (int i = 0; i < PHRASE_HISTORY_SIZE; i++) {
        if (phraseHistory[catIdx][i] == idx) return true;
    }
    return false;
}

// Add phrase index to history (circular buffer)
static void addToHistory(int catIdx, int idx) {
    phraseHistory[catIdx][phraseHistoryIdx[catIdx]] = idx;
    phraseHistoryIdx[catIdx] = (phraseHistoryIdx[catIdx] + 1) % PHRASE_HISTORY_SIZE;
}

// Helper: pick random phrase avoiding last 3 used
static int pickPhraseIdx(PhraseCategory cat, int count) {
    // Guard against empty phrase arrays
    if (count <= 0) return 0;
    
    initPhraseHistory();
    int catIdx = (int)cat;
    int idx;
    
    if (count <= PHRASE_HISTORY_SIZE) {
        // Not enough phrases to avoid all history - just avoid last one
        int lastIdx = phraseHistory[catIdx][(phraseHistoryIdx[catIdx] + PHRASE_HISTORY_SIZE - 1) % PHRASE_HISTORY_SIZE];
        do {
            idx = random(0, count);
        } while (idx == lastIdx && count > 1);
    } else {
        // Enough phrases - try to avoid all history
        int attempts = 0;
        do {
            idx = random(0, count);
            attempts++;
        } while (isInHistory(catIdx, idx) && attempts < 10);
    }
    
    addToHistory(catIdx, idx);
    return idx;
}

// Phrase categories - TRIPLE PERSONALITY SPLIT
// British hooligan OINK, Rasta blessed C.D., US Army SGT WARHOG

const char* PHRASES_HAPPY_OINK[] = {
    "snout proper owns it",
    "oi oi oi",
    "got that truffle bruv",
    "packets proper nommin",
    "hog on a mad one",
    "mud life innit",
    "truffle shuffle mate",
    "chaos tastes mint",
    "right proper mood",
    "horse lookin better",
    "sorted snout yeah"
};

const char* PHRASES_HAPPY_CD[] = {
    "snout feel irie",
    "blessed oink vibes",
    "got di truffle easy",
    "packets flow natural",
    "hog inna good mood",
    "mud life blessed",
    "truffle dance irie",
    "chaos taste sweet",
    "peaceful piggy seen",
    "horse find di way",
    "jah guide di snout"
};

const char* PHRASES_HAPPY_WARHOG[] = {
    "tactical advantage secured",
    "roger that truffle",
    "mission parameters met",
    "packets inbound hooah",
    "hog ready to deploy",
    "operational status green",
    "intel acquisition positive",
    "situational awareness high",
    "coordinates locked",
    "barn perimeter secure",
    "objective achieved"
};

const char* PHRASES_EXCITED_OINK[] = {
    "OI OI OI PROPER",
    "PWNED EM GOOD MATE",
    "TRUFFLE BAGGED BRUV",
    "GG NO RE INNIT",
    "SNOUT GOES MAD",
    "0DAY BUFFET YEAH",
    "PROPER BUZZING",
    "SORTED PROPER"
};

const char* PHRASES_EXCITED_CD[] = {
    "BLESSED OINK VIBES",
    "PWNED DEM IRIE",
    "TRUFFLE BLESSED JAH",
    "GG RESPECT BREDREN",
    "SNOUT FEEL DI POWER",
    "0DAY BLESSED",
    "IRIE VIBES STRONG",
    "JAH GUIDE DI WIN"
};

const char* PHRASES_EXCITED_WARHOG[] = {
    "MISSION ACCOMPLISHED",
    "OSCAR MIKE BABY",
    "TACTICAL SUPERIORITY",
    "HOOAH TRUFFLE DOWN",
    "OBJECTIVE SECURED",
    "ENEMY NEUTRALIZED",
    "ROGER WILCO SUCCESS",
    "BRING THE RAIN"
};

const char* PHRASES_HUNTING[] = {
    "proper snouting",
    "sniffin round like mad",
    "hunting them truffles bruv",
    "right aggro piggy",
    "diggin deep mate",
    "oi where's me truffles"
};

// OINK mode quiet phrases - when hunting but finding nothing
const char* PHRASES_OINK_QUIET[] = {
    "bloody ether's dead",
    "sniffin sod all",
    "no truffles here bruv",
    "channels proper empty",
    "where's the beacons mate",
    "dead radio yeah",
    "faraday cage innit",
    "lonely spectrum proper",
    "snout finds bugger all",
    "airwaves bone dry",
    "chasin ghosts mate",
    "802.11 wasteland"
};

const char* PHRASES_SLEEPY_OINK[] = {
    "knackered piggy",
    "sod all happening",
    "no truffles mate",
    "/dev/null init",
    "zzz proper tired",
    "dead bored bruv",
    "bugger all here",
    "wasteland proper"
};

const char* PHRASES_SLEEPY_CD[] = {
    "restin easy seen",
    "patience bredren",
    "no rush today",
    "chill mode active",
    "meditation time",
    "peaceful wait",
    "jah time come",
    "easy does it"
};

const char* PHRASES_SLEEPY_WARHOG[] = {
    "holding position",
    "awaiting orders",
    "radio silence",
    "standby mode active",
    "no contact sir",
    "sector quiet",
    "maintaining watch",
    "idle but ready"
};

const char* PHRASES_SAD_OINK[] = {
    "starvin proper",
    "404 no truffle mate",
    "proper lost bruv",
    "trough bone dry",
    "sad innit",
    "need truffles bad",
    "bloody depressing",
    "horse wandered off",
    "proper gutted",
    "miserable piggy"
};

const char* PHRASES_SAD_CD[] = {
    "hungry snout seen",
    "404 no truffle ya",
    "lost di way",
    "trough empty bredren",
    "sad vibes today",
    "need di herb bad",
    "patience test hard",
    "horse need help",
    "struggle real",
    "jah test mi"
};

const char* PHRASES_SAD_WARHOG[] = {
    "supplies critical",
    "mission failure likely",
    "lost contact",
    "morale compromised",
    "negative on intel",
    "zero targets sir",
    "battalion exhausted",
    "barn abandoned",
    "reinforcements needed",
    "status dire"
};

// BORED phrases - pig has nothing to hack
const char* PHRASES_BORED[] = {
    "no bacon here",
    "this place sucks",
    "grass tastes bad",
    "wifi desert mode",
    "empty spectrum",
    "bored outta mind",
    "where da APs at",
    "sniff sniff nada",
    "0 targets found",
    "radio silence",
    "tumbleweed.exe",
    "802.11 wasteland",
    "where horse at",
    "barn too quiet"
};

// WARHOG wardriving phrases - US Army Sergeant on recon patrol
const char* PHRASES_WARHOG[] = {
    "boots on ground",
    "patrol route active",
    "recon in progress sir",
    "moving through sector",
    "surveying AO",
    "oscar mike",
    "maintaining bearing",
    "grid coordinates logged",
    "securing perimeter data",
    "tactical recon mode",
    "sitrep: mobile",
    "foot patrol logged",
    "area survey continuous"
};

const char* PHRASES_WARHOG_FOUND[] = {
    "contact logged sir",
    "target acquired n logged",
    "AP marked on grid",
    "hostile network tagged",
    "coordinates confirmed",
    "intel gathered sir",
    "objective documented",
    "waypoint established",
    "tango located",
    "enemy network catalogued",
    "position marked sir"
};

// Piggy Blues BLE spam phrases - RuPaul drag queen eleganza
// All phrases use %s=vendor and %d=rssi
const char* PHRASES_PIGGYBLUES_TARGETED[] = {
    "sashay away %s darling [%ddB]",
    "serving %s realness @ %ddB",
    "%s honey ur notifications r showing %ddB",
    "snatch ur %s crown sweetie %ddB",
    "%s bout to gag @ %ddB mawma",
    "death drop on %s [%ddB]",
    "%s shantay u stay notified %ddB",
    "reading %s for filth @ %ddB"
};

// Status phrases showing scan results - drag queen eleganza format
const char* PHRASES_PIGGYBLUES_STATUS[] = {
    "serving looks to %d of %d queens",
    "%d slayed [%d clocked]",
    "category is: %d/%d gagged",
    "werking %d phones hunty [%d total]",
    "%d devices living 4 this drama [%d]"
};

// Idle/scanning phrases - RuPaul drag queen runway ready
const char* PHRASES_PIGGYBLUES_IDLE[] = {
    "bout to serve bluetooth eleganza",
    "hair is laid notifications r paid",
    "warming up the runway darling",
    "tucked n ready 4 the show",
    "glitter cannon loaded hunty",
    "bout to snatch ALL the airpods",
    "if u cant love urself... spam em"
};

// Deauth success - 802.11 hacker rap style
const char* PHRASES_DEAUTH_SUCCESS[] = {
    "%s proper mullered",
    "%s reason code 7 mate",
    "%s frame binned bruv",
    "%s wifi cancelled innit",
    "%s unauth'd lol",
    "%s ejected proper",
    "%s 802.11 banged up",
    "%s connection dead",
    "%s off me channel",
    "%s absolute muppet"
};

// PMKID captured - OINK mode (British hooligan)
const char* PHRASES_PMKID_OINK[] = {
    "pmkid nicked proper",
    "clientless hash bruv",
    "rsn ie proper pwned",
    "eapol-free loot mate",
    "passive extraction sorted",
    "hashcat ready innit",
    "no client needed yeah",
    "pmkid extracted proper",
    "silent pwn mode chuffed"
};

// PMKID captured - DNH mode (Rasta blessed - rare ghost capture)
const char* PHRASES_PMKID_CD[] = {
    "pmkid blessed ya",
    "jah guide di hash",
    "ghostly capture irie",
    "silent loot respect",
    "no attack needed seen",
    "hashcat blessed bredren",
    "natural extraction blessed",
    "pmkid inna air ya",
    "peaceful pwn irie"
};

// Rare phrases - 5% chance to appear for surprise variety
const char* PHRASES_RARE[] = {
    "hack the planet",
    "zero cool was here",
    "the gibson awaits",
    "mess with the best",
    "phreak the airwaves",
    "big truffle energy",
    "oink or be oinked",
    "sudo make sandwich",
    "curly tail chaos",
    "snout of justice",
    "802.11 mudslinger",
    "wardriving wizard",
    "never trust a pig",
    "pwn responsibly",
    "horse ok today?",
    "horse found the k",
    "barn still standing?",
    "horse vibin hard",
    "miss u horse",
    "horse WAS the barn",
    "check on da horse"
};

void Mood::init() {
    currentPhrase = "oink";
    lastPhraseChange = millis();
    phraseInterval = 5000;
    lastActivityTime = millis();
    
    // Reset momentum system
    momentumBoost = 0;
    lastBoostTime = 0;
    
    // Reset phrase queue
    phraseQueueCount = 0;
    
    // Reset milestone tracking for new session
    milestonesShown = 0;

    // Reset battery bias + tier notifications
    batteryBias = 0;
    batteryTier = 2;
    batteryTierInitialized = false;
    lastBatteryCheckMs = 0;
    lastMoodTier = 0xFF;
    lastMoodTierToastMs = 0;
    
    // Phase 10: Load saved mood from NVS
    moodPrefs.begin(MOOD_NVS_NAMESPACE, true);  // Read-only
    int8_t savedMood = moodPrefs.getChar("mood", 50);
    uint32_t savedTime = moodPrefs.getULong("time", 0);
    moodPrefs.end();
    
    // Calculate time since last save
    uint32_t now = millis();  // Can't compare to NVS time directly, use as session marker
    
    // If we have a saved mood, restore with some decay
    if (savedTime > 0) {
        // Start with saved mood, slightly regressed toward neutral
        happiness = savedMood + (50 - savedMood) / 4;  // 25% toward neutral
        
        // Welcome back phrase based on saved mood
        if (savedMood > 60) {
            currentPhrase = "missed me piggy?";
        } else if (savedMood < -20) {
            currentPhrase = "back for more..";
        }
    } else {
        happiness = 50;
    }
    lastEffectiveHappiness = happiness;
}

// Phase 10: Save mood to NVS (call on mode exit or periodically)
void Mood::saveMood() {
    moodPrefs.begin(MOOD_NVS_NAMESPACE, false);  // Read-write
    moodPrefs.putChar("mood", (int8_t)constrain(happiness, -100, 100));
    moodPrefs.putULong("time", millis());
    moodPrefs.end();
}

void Mood::update() {
    uint32_t now = millis();

    updateBatteryBias(now);
    
    // Phase 6: Process phrase queue first
    if (phraseQueueCount > 0) {
        processQueue();
        updateAvatarState();
        maybeNotifyMoodTierUp(getLastEffectiveHappiness(), now);
        return;  // Don't do normal phrase cycling while queue active
    }
    
    // Phase 9: Check for milestone celebrations
    // (milestonesShown is file-level static, reset in init())
    const SessionStats& sess = XP::getSession();
    
    // Network milestones: 10, 50, 100, 500, 1000
    if (sess.networks >= 10 && !(milestonesShown & 0x01)) {
        milestonesShown |= 0x01;
        currentPhrase = "10 TRUFFLES BABY";
        applyMomentumBoost(15);
        lastPhraseChange = now;
    } else if (sess.networks >= 50 && !(milestonesShown & 0x02)) {
        milestonesShown |= 0x02;
        queuePhrases("50 NETWORKS!", "oink oink oink", nullptr);
        currentPhrase = "HALF CENTURY!";
        applyMomentumBoost(20);
        lastPhraseChange = now;
    } else if (sess.networks >= 100 && !(milestonesShown & 0x04)) {
        milestonesShown |= 0x04;
        queuePhrases("THE BIG 100!", "centurion piggy", "unstoppable");
        currentPhrase = "TRIPLE DIGITS!";
        applyMomentumBoost(30);
        lastPhraseChange = now;
    } else if (sess.networks >= 500 && !(milestonesShown & 0x08)) {
        milestonesShown |= 0x08;
        queuePhrases("500 NETWORKS!", "legend mode", "wifi vacuum");
        currentPhrase = "HALF A THOUSAND";
        applyMomentumBoost(40);
        lastPhraseChange = now;
    }
    // Distance milestones: 1km, 5km, 10km
    else if (sess.distanceM >= 1000 && !(milestonesShown & 0x10)) {
        milestonesShown |= 0x10;
        currentPhrase = "1KM WALKED!";
        applyMomentumBoost(15);
        lastPhraseChange = now;
    } else if (sess.distanceM >= 5000 && !(milestonesShown & 0x20)) {
        milestonesShown |= 0x20;
        queuePhrases("5KM COVERED!", "piggy parkour", nullptr);
        currentPhrase = "SERIOUS WALKER";
        applyMomentumBoost(25);
        lastPhraseChange = now;
    } else if (sess.distanceM >= 10000 && !(milestonesShown & 0x40)) {
        milestonesShown |= 0x40;
        queuePhrases("10KM LEGEND!", "marathon pig", "touch grass pro");
        currentPhrase = "DOUBLE DIGITS KM";
        applyMomentumBoost(35);
        lastPhraseChange = now;
    }
    // Handshake milestones: 5, 10
    else if (sess.handshakes >= 5 && !(milestonesShown & 0x80)) {
        milestonesShown |= 0x80;
        currentPhrase = "5 HANDSHAKES!";
        applyMomentumBoost(20);
        lastPhraseChange = now;
    } else if (sess.handshakes >= 10 && !(milestonesShown & 0x100)) {
        milestonesShown |= 0x100;
        queuePhrases("10 HANDSHAKES!", "pwn master", nullptr);
        currentPhrase = "DOUBLE DIGITS!";
        applyMomentumBoost(30);
        lastPhraseChange = now;
    }
    
    // Phase 10: Periodic mood save (every 60 seconds)
    static uint32_t lastMoodSave = 0;
    if (now - lastMoodSave > 60000) {
        saveMood();
        lastMoodSave = now;
    }
    
    // Check for inactivity
    uint32_t inactiveSeconds = (now - lastActivityTime) / 1000;
    if (inactiveSeconds > 60) {
        onNoActivity(inactiveSeconds);
    }
    
    // Natural happiness decay
    if (now - lastPhraseChange > phraseInterval) {
        happiness = constrain(happiness - 1, -100, 100);
        
        // Skip automatic phrase selection if dialogue is locked (BLE sync in progress)
        // This prevents mood phrases from overwriting Papa/Son dialogue
        if (!dialogueLocked) {
            selectPhrase();
        }
        lastPhraseChange = now;
        
        // Random cute jump in IDLE mode when happy (0.5% chance per phrase cycle)
        // Makes the pig feel alive - spontaneous little hops
        if (porkchop.getMode() == PorkchopMode::IDLE && getEffectiveHappiness() > 20) {
            if (random(0, 200) == 0) {  // 0.5% chance
                Avatar::cuteJump();
            }
        }
    }
    
    updateAvatarState();
    maybeNotifyMoodTierUp(getLastEffectiveHappiness(), now);
}

void Mood::onHandshakeCaptured(const char* apName) {
    happiness = min(happiness + 10, 100);  // Smaller permanent boost
    applyMomentumBoost(30);  // Big temporary excitement!
    lastActivityTime = millis();
    
    // Sniff animation - caught something big!
    Avatar::sniff();
    
    // Cute jump celebration!
    Avatar::cuteJump();
    
    // Phase 2: Attack shake - strong shake for captures!
    Avatar::setAttackShake(true, true);
    
    // Award XP for handshake capture
    XP::addXP(XPEvent::HANDSHAKE_CAPTURED);
    
    // Bonus XP for low battery clutch capture
    if (M5.Power.getBatteryLevel() < 20) {
        XP::addXP(XPEvent::LOW_BATTERY_CAPTURE);
    }
    
    // Phase 6: Use phrase chaining for handshake celebration
    const SessionStats& sess = XP::getSession();
    char buf1[48], buf2[48], buf3[48];
    
    // First phrase - the capture announcement
    if (apName && strlen(apName) > 0) {
        String ap = String(apName);
        if (ap.length() > 20) ap = ap.substring(0, 20) + "..";
        const char* templates[] = { "%s pwned", "%s gg ez", "rekt %s", "%s is mine" };
        snprintf(buf1, sizeof(buf1), templates[random(0, 4)], ap.c_str());
    } else {
        // Personality-aware excited phrases
        PorkchopMode mode = porkchop.getMode();
        bool isCD = (mode == PorkchopMode::DNH_MODE);
        bool isWarhog = (mode == PorkchopMode::WARHOG_MODE);
        
        const char** excitedPhrases;
        int excitedCount;
        if (isCD) {
            excitedPhrases = PHRASES_EXCITED_CD;
            excitedCount = sizeof(PHRASES_EXCITED_CD) / sizeof(PHRASES_EXCITED_CD[0]);
        } else if (isWarhog) {
            excitedPhrases = PHRASES_EXCITED_WARHOG;
            excitedCount = sizeof(PHRASES_EXCITED_WARHOG) / sizeof(PHRASES_EXCITED_WARHOG[0]);
        } else {
            excitedPhrases = PHRASES_EXCITED_OINK;
            excitedCount = sizeof(PHRASES_EXCITED_OINK) / sizeof(PHRASES_EXCITED_OINK[0]);
        }
        
        int idx = pickPhraseIdx(PhraseCategory::EXCITED, excitedCount);
        strncpy(buf1, excitedPhrases[idx], sizeof(buf1) - 1);
        buf1[sizeof(buf1) - 1] = '\0';
    }
    
    // Second phrase - the count
    snprintf(buf2, sizeof(buf2), "%lu today!", (unsigned long)(sess.handshakes + 1));
    
    // Third phrase - celebration
    const char* celebrations[] = { "oink++", "gg bacon", "ez mode", "pwn train" };
    strncpy(buf3, celebrations[random(0, 4)], sizeof(buf3) - 1);
    buf3[sizeof(buf3) - 1] = '\0';
    
    // Set first phrase immediately, queue rest
    currentPhrase = buf1;
    lastPhraseChange = millis();
    queuePhrases(buf2, buf3);
    
    // Celebratory beep for handshake capture - non-blocking via SFX engine
    SFX::play(SFX::HANDSHAKE);
    
    // Force mood peek to show EXCITED face regardless of threshold
    forceMoodPeek();
}

void Mood::onPMKIDCaptured(const char* apName) {
    happiness = min(happiness + 15, 100);  // Slightly bigger permanent boost
    applyMomentumBoost(40);  // Even more temporary excitement!
    lastActivityTime = millis();
    
    // Sniff animation - stealthy capture!
    Avatar::sniff();
    
    // Cute jump celebration!
    Avatar::cuteJump();
    
    // Phase 2: Attack shake - strong shake for captures!
    Avatar::setAttackShake(true, true);
    
    // Award XP for PMKID capture
    // If in DO NO HAM mode, award the rare ghost PMKID XP (100 XP!)
    if (porkchop.getMode() == PorkchopMode::DNH_MODE) {
        XP::addXP(XPEvent::DNH_PMKID_GHOST);  // Rare passive PMKID!
    } else {
        XP::addXP(XPEvent::PMKID_CAPTURED);   // Regular 75 XP
    }
    
    // Bonus XP for low battery clutch capture
    if (M5.Power.getBatteryLevel() < 10) {
        XP::addXP(XPEvent::LOW_BATTERY_CAPTURE);
    }
    
    // Phase 6: PMKID gets special 3-phrase chain (mode-specific personality)
    char buf1[48], buf2[48], buf3[48];
    
    // First phrase - PMKID celebration (personality-aware)
    PorkchopMode mode = porkchop.getMode();
    const char** pmkidPhrases;
    int pmkidCount;
    
    if (mode == PorkchopMode::DNH_MODE) {
        // C.D. caught a ghost PMKID - very rare!
        pmkidPhrases = PHRASES_PMKID_CD;
        pmkidCount = sizeof(PHRASES_PMKID_CD) / sizeof(PHRASES_PMKID_CD[0]);
    } else {
        // Dr Oinker in OINK mode
        pmkidPhrases = PHRASES_PMKID_OINK;
        pmkidCount = sizeof(PHRASES_PMKID_OINK) / sizeof(PHRASES_PMKID_OINK[0]);
    }
    
    int idx = pickPhraseIdx(PhraseCategory::PMKID, pmkidCount);
    strncpy(buf1, pmkidPhrases[idx], sizeof(buf1) - 1);
    buf1[sizeof(buf1) - 1] = '\0';
    
    // Second phrase - explanation
    strncpy(buf2, "no client needed", sizeof(buf2) - 1);
    buf2[sizeof(buf2) - 1] = '\0';
    
    // Third phrase - hacker brag
    const char* brags[] = { "big brain oink", "200 iq snout", "galaxy brain", "ez clap pmkid" };
    strncpy(buf3, brags[random(0, 4)], sizeof(buf3) - 1);
    buf3[sizeof(buf3) - 1] = '\0';
    
    currentPhrase = buf1;
    lastPhraseChange = millis();
    queuePhrases(buf2, buf3);
    
    // Triple beep for PMKID - non-blocking via SFX engine
    SFX::play(SFX::PMKID);
    
    // NOTE: saveAllPMKIDs() removed from here - OinkMode handles its own saves
    // Calling SD writes during promiscuous mode causes SPI bus contention crashes
    
    // Force mood peek to show EXCITED face regardless of threshold
    forceMoodPeek();
}

void Mood::onNewNetwork(const char* apName, int8_t rssi, uint8_t channel) {
    happiness = min(happiness + 3, 100);  // Small permanent boost
    applyMomentumBoost(10);  // Quick excitement for network find
    lastActivityTime = millis();
    isBoredState = false;  // Clear bored state - found something!
    
    // Audio feedback - soft blip for new network
    SFX::play(SFX::NETWORK_NEW);
    
    // Sniff animation - found a truffle!
    Avatar::sniff();
    
    // Award XP for network discovery
    // Check if in DO NO HAM mode for different XP event
    bool isPassive = (porkchop.getMode() == PorkchopMode::DNH_MODE);
    
    if (apName && strlen(apName) > 0) {
        if (isPassive) {
            XP::addXP(XPEvent::DNH_NETWORK_PASSIVE);  // Passive mode bonus
        } else {
            XP::addXP(XPEvent::NETWORK_FOUND);
        }
    } else {
        // Hidden network gets bonus XP (same in both modes)
        XP::addXP(XPEvent::NETWORK_HIDDEN);
    }
    
    // Show AP name with info in funny phrases
    if (apName && strlen(apName) > 0) {
        String ap = String(apName);
        if (ap.length() > 20) ap = ap.substring(0, 20) + "..";
        
        const char* templates[] = {
            "sniffed %s ch%d",
            "%s %ddb yum",
            "found %s oink",
            "oink %s",
            "new truffle %s"
        };
        int idx = random(0, 5);
        char buf[64];
        if (idx == 1 || idx == 3) {
            snprintf(buf, sizeof(buf), templates[idx], ap.c_str(), rssi);
        } else if (idx == 0 || idx == 2) {
            snprintf(buf, sizeof(buf), templates[idx], ap.c_str(), channel);
        } else {
            snprintf(buf, sizeof(buf), templates[idx], ap.c_str());
        }
        currentPhrase = buf;
    } else {
        // Hidden network
        char buf[48];
        snprintf(buf, sizeof(buf), "sneaky truffle CH%d %ddB", channel, rssi);
        currentPhrase = buf;
    }
    lastPhraseChange = millis();
}

void Mood::setStatusMessage(const String& msg) {
    uint32_t now = millis();
    if (msg == lastStatusMessage && (now - lastStatusMessageTime) < 1000) {
        return;
    }
    lastStatusMessage = msg;
    lastStatusMessageTime = now;
    currentPhrase = msg;
    lastPhraseChange = now;
}

void Mood::onMLPrediction(float confidence) {
    lastActivityTime = millis();
    
    // Personality-aware phrases
    PorkchopMode mode = porkchop.getMode();
    bool isCD = (mode == PorkchopMode::DNH_MODE);
    bool isWarhog = (mode == PorkchopMode::WARHOG_MODE);
    
    // High confidence = happy
    if (confidence > 0.8f) {
        happiness = min(happiness + 15, 100);
        
        const char** excitedPhrases;
        int excitedCount;
        if (isCD) {
            excitedPhrases = PHRASES_EXCITED_CD;
            excitedCount = sizeof(PHRASES_EXCITED_CD) / sizeof(PHRASES_EXCITED_CD[0]);
        } else if (isWarhog) {
            excitedPhrases = PHRASES_EXCITED_WARHOG;
            excitedCount = sizeof(PHRASES_EXCITED_WARHOG) / sizeof(PHRASES_EXCITED_WARHOG[0]);
        } else {
            excitedPhrases = PHRASES_EXCITED_OINK;
            excitedCount = sizeof(PHRASES_EXCITED_OINK) / sizeof(PHRASES_EXCITED_OINK[0]);
        }
        
        int idx = pickPhraseIdx(PhraseCategory::EXCITED, excitedCount);
        currentPhrase = excitedPhrases[idx];
    } else if (confidence > 0.5f) {
        happiness = min(happiness + 5, 100);
        
        const char** happyPhrases;
        int happyCount;
        if (isCD) {
            happyPhrases = PHRASES_HAPPY_CD;
            happyCount = sizeof(PHRASES_HAPPY_CD) / sizeof(PHRASES_HAPPY_CD[0]);
        } else if (isWarhog) {
            happyPhrases = PHRASES_HAPPY_WARHOG;
            happyCount = sizeof(PHRASES_HAPPY_WARHOG) / sizeof(PHRASES_HAPPY_WARHOG[0]);
        } else {
            happyPhrases = PHRASES_HAPPY_OINK;
            happyCount = sizeof(PHRASES_HAPPY_OINK) / sizeof(PHRASES_HAPPY_OINK[0]);
        }
        
        int idx = pickPhraseIdx(PhraseCategory::HAPPY, happyCount);
        currentPhrase = happyPhrases[idx];
    }
    
    lastPhraseChange = millis();
}

void Mood::onNoActivity(uint32_t seconds) {
    // Rate-limit inactivity effects to prevent rapid phrase changes
    static uint32_t lastInactivityUpdate = 0;
    uint32_t now = millis();
    
    // Only update every 5 seconds to prevent burst phrase changes
    if (now - lastInactivityUpdate < 5000) {
        return;
    }
    lastInactivityUpdate = now;
    
    // Phase 7: Patience affects boredom thresholds
    // High patience = pig takes longer to get bored
    // patience 0.0 = gets bored at 60s/150s, patience 1.0 = gets bored at 180s/450s
    const PersonalityConfig& pers = Config::personality();
    uint32_t boredThreshold = 120 + (uint32_t)(pers.patience * 180);    // 120-300s
    uint32_t veryBoredThreshold = 300 + (uint32_t)(pers.patience * 300); // 300-600s
    
    if (seconds > veryBoredThreshold) {
        // Very bored - patience exhausted
        happiness = max(happiness - 2, -100);
        if (happiness < -20) {
            // Mode-aware boredom phrases
            PorkchopMode mode = porkchop.getMode();
            if (mode == PorkchopMode::OINK_MODE || mode == PorkchopMode::SPECTRUM_MODE) {
                // In hunting modes, use quiet hunting phrases instead of generic sleepy
                int idx = pickPhraseIdx(PhraseCategory::SLEEPY, sizeof(PHRASES_OINK_QUIET) / sizeof(PHRASES_OINK_QUIET[0]));
                currentPhrase = PHRASES_OINK_QUIET[idx];
            } else {
                // Personality-aware sleepy phrases
                bool isCD = (mode == PorkchopMode::DNH_MODE);
                bool isWarhog = (mode == PorkchopMode::WARHOG_MODE);
                
                const char** sleepyPhrases;
                int sleepyCount;
                if (isCD) {
                    sleepyPhrases = PHRASES_SLEEPY_CD;
                    sleepyCount = sizeof(PHRASES_SLEEPY_CD) / sizeof(PHRASES_SLEEPY_CD[0]);
                } else if (isWarhog) {
                    sleepyPhrases = PHRASES_SLEEPY_WARHOG;
                    sleepyCount = sizeof(PHRASES_SLEEPY_WARHOG) / sizeof(PHRASES_SLEEPY_WARHOG[0]);
                } else {
                    sleepyPhrases = PHRASES_SLEEPY_OINK;
                    sleepyCount = sizeof(PHRASES_SLEEPY_OINK) / sizeof(PHRASES_SLEEPY_OINK[0]);
                }
                
                int idx = pickPhraseIdx(PhraseCategory::SLEEPY, sleepyCount);
                currentPhrase = sleepyPhrases[idx];
            }
            lastPhraseChange = now;  // Prevent immediate re-selection
        }
    } else if (seconds > boredThreshold) {
        // Getting bored
        happiness = max(happiness - 1, -100);
    }
}

void Mood::onWiFiLost() {
    happiness = max(happiness - 20, -100);
    lastActivityTime = millis();
    
    // Personality-aware sad phrases
    PorkchopMode mode = porkchop.getMode();
    bool isCD = (mode == PorkchopMode::DNH_MODE);
    bool isWarhog = (mode == PorkchopMode::WARHOG_MODE);
    
    const char** sadPhrases;
    int sadCount;
    if (isCD) {
        sadPhrases = PHRASES_SAD_CD;
        sadCount = sizeof(PHRASES_SAD_CD) / sizeof(PHRASES_SAD_CD[0]);
    } else if (isWarhog) {
        sadPhrases = PHRASES_SAD_WARHOG;
        sadCount = sizeof(PHRASES_SAD_WARHOG) / sizeof(PHRASES_SAD_WARHOG[0]);
    } else {
        sadPhrases = PHRASES_SAD_OINK;
        sadCount = sizeof(PHRASES_SAD_OINK) / sizeof(PHRASES_SAD_OINK[0]);
    }
    
    int idx = pickPhraseIdx(PhraseCategory::SAD, sadCount);
    currentPhrase = sadPhrases[idx];
    lastPhraseChange = millis();
}

void Mood::onGPSFix() {
    happiness = min(happiness + 5, 100);  // Small permanent boost
    applyMomentumBoost(15);  // Happy about GPS lock!
    lastActivityTime = millis();
    
    // Award XP for GPS lock (handled by session flag in XP to avoid duplicates)
    const SessionStats& sess = XP::getSession();
    if (!sess.gpsLockAwarded) {
        XP::addXP(XPEvent::GPS_LOCK);
    }
    
    currentPhrase = "gps locked n loaded";
    lastPhraseChange = millis();
}

void Mood::onGPSLost() {
    happiness = max(happiness - 5, -100);  // Small permanent dip
    applyMomentumBoost(-15);  // Temporary sadness
    currentPhrase = "gps lost sad piggy";
    lastPhraseChange = millis();
}

void Mood::onLowBattery() {
    currentPhrase = "piggy needs juice";
    lastPhraseChange = millis();
}

void Mood::selectPhrase() {
    const char** phrases;
    int count;
    PhraseCategory cat;
    
    // Get current mode for personality-specific phrase selection
    PorkchopMode mode = porkchop.getMode();
    bool isCD = (mode == PorkchopMode::DNH_MODE);
    bool isWarhog = (mode == PorkchopMode::WARHOG_MODE);
    
    // RIDDLE SYSTEM: rare chance to queue cryptic challenge hints in IDLE
    if (mode == PorkchopMode::IDLE && tryQueueRiddle()) {
        return;  // Riddle queued, skip normal phrase selection
    }
    
    // Use effective happiness (base + momentum) for phrase selection
    int effectiveMood = getEffectiveHappiness();
    
    // Phase 3: 5% chance for rare phrase (surprise variety)
    int specialRoll = random(0, 100);
    if (specialRoll < 3) {
        // 3% chance for cryptic lore (PROJECT M5PORKSOUP breadcrumbs)
        int idx = pickPhraseIdx(PhraseCategory::RARE_LORE, PHRASES_RARE_LORE_COUNT);
        currentPhrase = PHRASES_RARE_LORE[idx];
        return;
    } else if (specialRoll < 5) {
        // 2% chance for regular rare phrases
        phrases = PHRASES_RARE;
        count = sizeof(PHRASES_RARE) / sizeof(PHRASES_RARE[0]);
        cat = PhraseCategory::RARE;
        int idx = pickPhraseIdx(cat, count);
        currentPhrase = phrases[idx];
        return;
    }
    
    // Phase 4: 10% chance for dynamic phrase (only if we have data)
    const SessionStats& sess = XP::getSession();
    if (specialRoll < 15 && sess.networks > 0) {  // 10% after rare check
        int idx = pickPhraseIdx(PhraseCategory::DYNAMIC, PHRASES_DYNAMIC_COUNT);
        currentPhrase = formatDynamicPhrase(PHRASES_DYNAMIC[idx]);
        return;
    }
    
    // Phase 7: Personality trait influence
    const PersonalityConfig& pers = Config::personality();
    int personalityRoll = random(0, 100);
    
    // High aggression (>0.6) can trigger hunting phrases even when happy
    if (pers.aggression > 0.6f && personalityRoll < (int)(pers.aggression * 30)) {
        phrases = PHRASES_HUNTING;
        count = sizeof(PHRASES_HUNTING) / sizeof(PHRASES_HUNTING[0]);
        cat = PhraseCategory::HUNTING;
        int idx = pickPhraseIdx(cat, count);
        currentPhrase = phrases[idx];
        return;
    }
    
    // High curiosity (>0.7) with activity can trigger excited phrases (personality-aware)
    if (pers.curiosity > 0.7f && sess.networks > 5 && personalityRoll < (int)(pers.curiosity * 25)) {
        if (isCD) {
            phrases = PHRASES_EXCITED_CD;
            count = sizeof(PHRASES_EXCITED_CD) / sizeof(PHRASES_EXCITED_CD[0]);
        } else if (isWarhog) {
            phrases = PHRASES_EXCITED_WARHOG;
            count = sizeof(PHRASES_EXCITED_WARHOG) / sizeof(PHRASES_EXCITED_WARHOG[0]);
        } else {
            phrases = PHRASES_EXCITED_OINK;
            count = sizeof(PHRASES_EXCITED_OINK) / sizeof(PHRASES_EXCITED_OINK[0]);
        }
        cat = PhraseCategory::EXCITED;
        int idx = pickPhraseIdx(cat, count);
        currentPhrase = phrases[idx];
        return;
    }
    
    // Phase 2: Mood bleed-through - extreme moods can override category
    // When very happy (>80), 30% chance to use excited phrases
    // When very sad (<-60), 30% chance to use sad phrases
    int bleedRoll = random(0, 100);
    
    if (effectiveMood > 80 && bleedRoll < 30) {
        // Extremely happy - use excited phrases (personality-aware)
        if (isCD) {
            phrases = PHRASES_EXCITED_CD;
            count = sizeof(PHRASES_EXCITED_CD) / sizeof(PHRASES_EXCITED_CD[0]);
        } else if (isWarhog) {
            phrases = PHRASES_EXCITED_WARHOG;
            count = sizeof(PHRASES_EXCITED_WARHOG) / sizeof(PHRASES_EXCITED_WARHOG[0]);
        } else {
            phrases = PHRASES_EXCITED_OINK;
            count = sizeof(PHRASES_EXCITED_OINK) / sizeof(PHRASES_EXCITED_OINK[0]);
        }
        cat = PhraseCategory::EXCITED;
    } else if (effectiveMood < -60 && bleedRoll < 30) {
        // Extremely sad - melancholy bleeds through (personality-aware)
        if (isCD) {
            phrases = PHRASES_SAD_CD;
            count = sizeof(PHRASES_SAD_CD) / sizeof(PHRASES_SAD_CD[0]);
        } else if (isWarhog) {
            phrases = PHRASES_SAD_WARHOG;
            count = sizeof(PHRASES_SAD_WARHOG) / sizeof(PHRASES_SAD_WARHOG[0]);
        } else {
            phrases = PHRASES_SAD_OINK;
            count = sizeof(PHRASES_SAD_OINK) / sizeof(PHRASES_SAD_OINK[0]);
        }
        cat = PhraseCategory::SAD;
    } else if (effectiveMood > 70) {
        // High happiness but not from handshake - use HAPPY not EXCITED (personality-aware)
        if (isCD) {
            phrases = PHRASES_HAPPY_CD;
            count = sizeof(PHRASES_HAPPY_CD) / sizeof(PHRASES_HAPPY_CD[0]);
        } else if (isWarhog) {
            phrases = PHRASES_HAPPY_WARHOG;
            count = sizeof(PHRASES_HAPPY_WARHOG) / sizeof(PHRASES_HAPPY_WARHOG[0]);
        } else {
            phrases = PHRASES_HAPPY_OINK;
            count = sizeof(PHRASES_HAPPY_OINK) / sizeof(PHRASES_HAPPY_OINK[0]);
        }
        cat = PhraseCategory::HAPPY;
    } else if (effectiveMood > 30) {
        // Normal happy mood (personality-aware)
        if (isCD) {
            phrases = PHRASES_HAPPY_CD;
            count = sizeof(PHRASES_HAPPY_CD) / sizeof(PHRASES_HAPPY_CD[0]);
        } else if (isWarhog) {
            phrases = PHRASES_HAPPY_WARHOG;
            count = sizeof(PHRASES_HAPPY_WARHOG) / sizeof(PHRASES_HAPPY_WARHOG[0]);
        } else {
            phrases = PHRASES_HAPPY_OINK;
            count = sizeof(PHRASES_HAPPY_OINK) / sizeof(PHRASES_HAPPY_OINK[0]);
        }
        cat = PhraseCategory::HAPPY;
    } else if (effectiveMood > -10) {
        phrases = PHRASES_HUNTING;
        count = sizeof(PHRASES_HUNTING) / sizeof(PHRASES_HUNTING[0]);
        cat = PhraseCategory::HUNTING;
    } else if (effectiveMood > -50) {
        // Sleepy/bored (personality-aware)
        if (isCD) {
            phrases = PHRASES_SLEEPY_CD;
            count = sizeof(PHRASES_SLEEPY_CD) / sizeof(PHRASES_SLEEPY_CD[0]);
        } else if (isWarhog) {
            phrases = PHRASES_SLEEPY_WARHOG;
            count = sizeof(PHRASES_SLEEPY_WARHOG) / sizeof(PHRASES_SLEEPY_WARHOG[0]);
        } else {
            phrases = PHRASES_SLEEPY_OINK;
            count = sizeof(PHRASES_SLEEPY_OINK) / sizeof(PHRASES_SLEEPY_OINK[0]);
        }
        cat = PhraseCategory::SLEEPY;
    } else {
        // Very sad (personality-aware)
        if (isCD) {
            phrases = PHRASES_SAD_CD;
            count = sizeof(PHRASES_SAD_CD) / sizeof(PHRASES_SAD_CD[0]);
        } else if (isWarhog) {
            phrases = PHRASES_SAD_WARHOG;
            count = sizeof(PHRASES_SAD_WARHOG) / sizeof(PHRASES_SAD_WARHOG[0]);
        } else {
            phrases = PHRASES_SAD_OINK;
            count = sizeof(PHRASES_SAD_OINK) / sizeof(PHRASES_SAD_OINK[0]);
        }
        cat = PhraseCategory::SAD;
    }
    
    int idx = pickPhraseIdx(cat, count);
    currentPhrase = phrases[idx];
}

void Mood::updateAvatarState() {
    // Use effective happiness (base + momentum) for avatar state
    int effectiveMood = getEffectiveHappiness();
    uint32_t now = millis();
    
    // Phase 8: Pass mood intensity to avatar for animation timing
    Avatar::setMoodIntensity(effectiveMood);
    
    // Mode-aware avatar state selection
    PorkchopMode mode = porkchop.getMode();
    
    // Mood peek: detect threshold crossings and trigger peek
    // Only for mode-locked states (OINK, PIGGYBLUES, SPECTRUM)
    bool isModeLockedState = (mode == PorkchopMode::OINK_MODE || 
                               mode == PorkchopMode::PIGGYBLUES_MODE ||
                               mode == PorkchopMode::SPECTRUM_MODE);
    
    // Track mode transitions to sync threshold on mode entry
    static PorkchopMode lastMode = PorkchopMode::IDLE;
    bool justEnteredModeLock = isModeLockedState && (lastMode != mode);
    lastMode = mode;
    
    if (isModeLockedState) {
        // On mode entry, sync threshold to current mood to avoid false peek
        if (justEnteredModeLock) {
            lastThresholdMood = effectiveMood;
            moodPeekActive = false;  // Reset any active peek
        }
        
        // Check for threshold crossings (mood peek triggers)
        bool crossedHigh = (lastThresholdMood <= MOOD_PEEK_HIGH_THRESHOLD && 
                            effectiveMood > MOOD_PEEK_HIGH_THRESHOLD);
        bool crossedLow = (lastThresholdMood >= MOOD_PEEK_LOW_THRESHOLD && 
                           effectiveMood < MOOD_PEEK_LOW_THRESHOLD);
        
        if ((crossedHigh || crossedLow) && !moodPeekActive) {
            // Trigger mood peek - briefly show emotional state
            moodPeekActive = true;
            moodPeekStartTime = now;
        }
        
        // Check if peek has expired
        if (moodPeekActive && (now - moodPeekStartTime > MOOD_PEEK_DURATION_MS)) {
            moodPeekActive = false;
        }
    } else {
        // Reset peek state when not in mode-locked state
        moodPeekActive = false;
    }
    
    // Update threshold tracking for next call
    lastThresholdMood = effectiveMood;
    
    // If mood peek is active, show full mood-based state instead of mode state
    if (moodPeekActive) {
        // Full emotional expression during peek
        if (effectiveMood > MOOD_PEEK_HIGH_THRESHOLD) {
            Avatar::setState(AvatarState::EXCITED);
        } else if (effectiveMood > 30) {
            Avatar::setState(AvatarState::HAPPY);
        } else if (effectiveMood > -10) {
            Avatar::setState(AvatarState::NEUTRAL);
        } else if (effectiveMood > MOOD_PEEK_LOW_THRESHOLD) {
            Avatar::setState(AvatarState::SLEEPY);
        } else {
            Avatar::setState(AvatarState::SAD);
        }
        return;  // Peek takes priority
    }
    
    switch (mode) {
        case PorkchopMode::OINK_MODE:
        case PorkchopMode::SPECTRUM_MODE:
            // Hunting modes: show HUNTING unless bored (mood peek handles emotional flashes)
            if (isBoredState) {
                Avatar::setState(AvatarState::SLEEPY);
            } else {
                Avatar::setState(AvatarState::HUNTING);
            }
            break;
            
        case PorkchopMode::PIGGYBLUES_MODE:
            // Aggressive mode: ALWAYS show ANGRY (mood peek handles emotional flashes)
            Avatar::setState(AvatarState::ANGRY);
            break;
            
        case PorkchopMode::WARHOG_MODE:
            // Wardriving: relaxed hunting, biased toward happy
            if (effectiveMood > MOOD_PEEK_HIGH_THRESHOLD) {
                Avatar::setState(AvatarState::EXCITED);
            } else if (effectiveMood > 10) {
                Avatar::setState(AvatarState::HAPPY);
            } else {
                Avatar::setState(AvatarState::NEUTRAL);
            }
            break;
            
        case PorkchopMode::FILE_TRANSFER:
            // File transfer: stay happy unless very sad
            if (effectiveMood > MOOD_PEEK_HIGH_THRESHOLD) {
                Avatar::setState(AvatarState::EXCITED);
            } else if (effectiveMood > MOOD_PEEK_LOW_THRESHOLD) {
                Avatar::setState(AvatarState::HAPPY);
            } else {
                Avatar::setState(AvatarState::NEUTRAL);
            }
            break;
            
        default:
            // IDLE, MENU, SETTINGS, etc: full mood-based expression
            if (effectiveMood > MOOD_PEEK_HIGH_THRESHOLD) {
                Avatar::setState(AvatarState::EXCITED);
            } else if (effectiveMood > 30) {
                Avatar::setState(AvatarState::HAPPY);
            } else if (effectiveMood > -10) {
                Avatar::setState(AvatarState::NEUTRAL);
            } else if (effectiveMood > -50) {
                Avatar::setState(AvatarState::SLEEPY);
            } else {
                Avatar::setState(AvatarState::SAD);
            }
            break;
    }
}

void Mood::draw(M5Canvas& canvas) {
    // === WEATHER SYSTEM ===
    // Weather update is handled in Display::update() to avoid stuck flashes in non-avatar screens.
    int effectiveMood = getEffectiveHappiness();
    
    // Note: Background inversion for thunder is handled at Display level
    // before Avatar::draw() is called. We don't fillSprite here as it would
    // erase the already-drawn avatar.
    
    // Hide bubble during walk transition
    if (Avatar::isTransitioning()) {
        return;  // Pig is walking, no speech bubble
    }
    
    // Calculate bubble size based on ACTUAL word-wrapped content
    // Dynamic width: fits content tightly, min 50px, max 116px
    ensureBubbleCache();
    int numLines = bubbleLineCount;
    int longestLineChars = bubbleLongestLine;
    
    // === DYNAMIC BUBBLE WIDTH ===
    // Width based on longest line: chars * 6px + padding (10px)
    // Clamped between 50px (min) and 116px (max)
    const int MIN_BUBBLE_W = 50;
    const int MAX_BUBBLE_W = 116;
    int bubbleW = constrain(longestLineChars * 6 + 12, MIN_BUBBLE_W, MAX_BUBBLE_W);
    
    // === ADAPTIVE BUBBLE POSITIONING (Sirloin-style) ===
    // Bubble follows pig's head position and avoids covering the face
    // 3 modes: LEFT_EDGE (horizontal arrow right), CENTER (vertical arrow down), RIGHT_EDGE (horizontal arrow left)
    
    int pigX = Avatar::getCurrentX();
    bool pigFacingRight = Avatar::isFacingRight();
    
    // Calculate pig's head center based on facing direction
    // Pig head is 6 chars at text size 3 (18px/char = 108px total width)
    int pigHeadCenterX = pigX + 54;  // Center of pig face
    
    int bubbleX, bubbleY;
    int lineHeight = 11;
    int bubbleH = 8 + (numLines * lineHeight);  // Padding + actual lines
    
    // Cap bubble height to fit above grass (y=91)
    if (bubbleH > 88) bubbleH = 88;
    
    // Determine bubble mode based on pigX thresholds (matches Sirloin)
    enum class BubbleMode { LEFT_EDGE, CENTER_TOP, RIGHT_EDGE };
    BubbleMode mode;
    
    bool atLeftEdge = (pigX < 35);
    bool atRightEdge = (pigX > 90);  // 240 - 108 (pig width) - margin
    
    // Arrow positioning constants
    const int ARROW_LENGTH = 8;
    
    if (atLeftEdge) {
        // Pig at left edge → bubble floats to RIGHT of pig (horizontal arrow pointing left)
        mode = BubbleMode::LEFT_EDGE;
        bubbleX = pigX + 108 + 6;  // Right of pig body + 6px gap
        bubbleY = 23;  // At pig ear level
    } else if (atRightEdge) {
        // Pig at right edge → bubble floats to LEFT of pig (horizontal arrow pointing right)
        mode = BubbleMode::RIGHT_EDGE;
        bubbleX = pigX - bubbleW - 6;  // Left of pig + 6px gap
        bubbleY = 23;  // At pig ear level
    } else {
        // Pig in center → bubble floats ABOVE pig but not too far
        // Position bubble so it doesn't cover pig's face but stays close
        mode = BubbleMode::CENTER_TOP;
        bubbleX = pigHeadCenterX - (bubbleW / 2);  // Center over pig's head
        
        // Pig head at Y=23, ears start there
        // Arrow tip should point at pig's ear area (Y ~20)
        // Bubble should not float too far from head - min Y = 2 (near top)
        int arrowTipY = 20;  // Point at pig's ear area
        int bubbleBottom = arrowTipY - ARROW_LENGTH;  // Y = 12
        bubbleY = bubbleBottom - bubbleH;
        
        // Clamp bubbleY to minimum of 2 (near top) - taller bubbles stay close to head
        if (bubbleY < 2) bubbleY = 2;
    }
    
    // Clamp bubble to screen edges (prevent overflow)
    if (bubbleX < 2) bubbleX = 2;
    if (bubbleX + bubbleW > 238) bubbleX = 238 - bubbleW;
    
    // === DRAW BUBBLE ===
    // For CENTER_TOP mode with negative Y, draw to both topBar and mainCanvas
    bool drawToTopBar = (mode == BubbleMode::CENTER_TOP && bubbleY < 0);
    
    if (drawToTopBar) {
        // Get topBar canvas and draw bubble there too
        M5Canvas& topBar = Display::getTopBar();
        
        // Convert mainCanvas Y to topBar Y (physical coordinates)
        // mainCanvas Y=0 is physical Y=TOP_BAR_H (14)
        // topBar Y=0 is physical Y=0
        // So mainCanvas bubbleY maps to topBar Y = TOP_BAR_H + bubbleY
        int topBarBubbleY = TOP_BAR_H + bubbleY;
        
        // Draw bubble to topBar (portion above mainCanvas)
        topBar.fillRoundRect(bubbleX, topBarBubbleY, bubbleW, bubbleH, 6, COLOR_FG);
    }
    
    // Draw bubble to mainCanvas (negative Y portion will be clipped)
    canvas.fillRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 6, COLOR_FG);
    
    // === DRAW ARROW ===
    if (mode == BubbleMode::LEFT_EDGE) {
        // Pig on left, bubble on right → horizontal arrow pointing LEFT toward pig
        int arrowY = bubbleY + (bubbleH / 2);  // Middle of bubble vertically
        int arrowTipX = bubbleX - ARROW_LENGTH;
        int arrowBaseX = bubbleX;
        canvas.fillTriangle(arrowTipX, arrowY, arrowBaseX, arrowY - 6, arrowBaseX, arrowY + 6, COLOR_FG);
    } else if (mode == BubbleMode::RIGHT_EDGE) {
        // Pig on right, bubble on left → horizontal arrow pointing RIGHT toward pig
        int arrowY = bubbleY + (bubbleH / 2);
        int arrowTipX = bubbleX + bubbleW + ARROW_LENGTH;
        int arrowBaseX = bubbleX + bubbleW;
        canvas.fillTriangle(arrowTipX, arrowY, arrowBaseX, arrowY - 6, arrowBaseX, arrowY + 6, COLOR_FG);
    } else {
        // Center mode → vertical arrow pointing DOWN toward pig's head
        int arrowTipY = 20;  // Point at pig's ear area (updated for new head Y)
        int arrowBaseY = arrowTipY - ARROW_LENGTH;
        int arrowLeftX = pigHeadCenterX - 6;
        int arrowRightX = pigHeadCenterX + 6;
        
        // Clamp arrow base to bubble width
        if (arrowLeftX < bubbleX + 2) arrowLeftX = bubbleX + 2;
        if (arrowRightX > bubbleX + bubbleW - 2) arrowRightX = bubbleX + bubbleW - 2;
        
        canvas.fillTriangle(pigHeadCenterX, arrowTipY, arrowLeftX, arrowBaseY, arrowRightX, arrowBaseY, COLOR_FG);
    }
    
    // === DRAW TEXT ===
    int textX = bubbleX + 5;
    int textY = bubbleY + 4;
    
    // Second pass: render cached lines (uppercase)
    int lineNum = 0;
    while (lineNum < numLines && lineNum < 5) {
        const char* line = bubbleLines[lineNum];
        
        int lineY = textY + lineNum * lineHeight;
        
        // Draw text to topBar if it falls in that area
        if (drawToTopBar && lineY < 0) {
            M5Canvas& topBar = Display::getTopBar();
            topBar.setTextSize(1);
            topBar.setTextDatum(top_left);
            topBar.setTextColor(COLOR_BG);
            int topBarLineY = TOP_BAR_H + lineY;
            if (topBarLineY >= 0 && topBarLineY < TOP_BAR_H) {
                topBar.drawString(line, textX, topBarLineY);
            }
        }
        
        // Draw text to mainCanvas (negative Y will be clipped)
        canvas.setTextSize(1);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(COLOR_BG);
        canvas.drawString(line, textX, lineY);
        
        lineNum++;
    }
}

const String& Mood::getCurrentPhrase() {
    return currentPhrase;
}

int Mood::getCurrentHappiness() {
    return happiness;
}

// Sniffing phrases - 802.11 monitor mode style
const char* PHRASES_SNIFFING[] = {
    "channel hoppin",
    "raw sniffin",
    "mon0 piggy",
    "promisc mode",
    "beacon dump",
    "frame harvest",
    "airsnort vibes",
    "ether tapping",
    "mgmt snooping",
    "pcap or it didnt",
    "0x8000 stalkin",
    "radiodump",
    "passive recon"
};

// DO NO HAM passive recon phrases - peaceful observer mode
const char* PHRASES_PASSIVE_RECON[] = {
    "peaceful observin seen",
    "no trouble dis time ya",
    "quiet watcher blessed",
    "irie passive scan",
    "chill vibes bredren",
    "silent sweep respect",
    "sniff no bite easy",
    "recon only jah guide",
    "zen mode inna air",
    "watchful snout blessed",
    "ghost recon irie",
    "stealth sweep respect"
};

// Deauth/attack phrases - Dr Oinker style (OINK mode only)
const char* PHRASES_DEAUTH[] = {
    "proper bangin %s mate",
    "frame storm on %s bruv",
    "disassoc %s innit",
    "mullerin %s proper",
    "reason code 7 %s yeah",
    "%s gettin booted mate",
    "kickin %s off me turf",
    "%s binned bruv lol"
};

// Idle phrases - mode hints and hacker personality
const char* PHRASES_MENU_IDLE[] = {
    "[O] truffle hunt",
    "[W] hog out",
    "[B] spam the ether",
    "[H] peek the spectrum",
    "pick ur poison",
    "press key or perish",
    "awaiting chaos",
    "idle hooves...",
    "root or reboot",
    "802.11 on standby",
    "snout calibrated",
    "kernel panik ready",
    "inject or eject",
    "oink//null",
    "promiscuous mode",
    "sudo make bacon"
};

void Mood::onSniffing(uint16_t networkCount, uint8_t channel) {
    lastActivityTime = millis();
    isBoredState = false;  // Clear bored state - we're hunting again
    
    // Pick sniffing phrase with channel info (no repeat)
    int idx = pickPhraseIdx(PhraseCategory::SNIFFING, sizeof(PHRASES_SNIFFING) / sizeof(PHRASES_SNIFFING[0]));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s CH%d (%d APs)", PHRASES_SNIFFING[idx], channel, networkCount);
    currentPhrase = buf;
    lastPhraseChange = millis();
}

void Mood::onPassiveRecon(uint16_t networkCount, uint8_t channel) {
    lastActivityTime = millis();
    isBoredState = false;  // Clear bored state - we're observing
    
    // Pick passive recon phrase with channel info (no repeat)
    int idx = pickPhraseIdx(PhraseCategory::PASSIVE_RECON, sizeof(PHRASES_PASSIVE_RECON) / sizeof(PHRASES_PASSIVE_RECON[0]));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s CH%d (%d)", PHRASES_PASSIVE_RECON[idx], channel, networkCount);
    currentPhrase = buf;
    lastPhraseChange = millis();
}

void Mood::onDeauthing(const char* apName, uint32_t deauthCount) {
    lastActivityTime = millis();
    isBoredState = false;  // Clear bored state - we're attacking!
    
    // Handle null or empty SSID (hidden networks)
    String ap = (apName && strlen(apName) > 0) ? String(apName) : "ghost AP";
    if (ap.length() > 20) ap = ap.substring(0, 20) + "..";
    
    int idx = pickPhraseIdx(PhraseCategory::DEAUTH, sizeof(PHRASES_DEAUTH) / sizeof(PHRASES_DEAUTH[0]));
    char buf[64];
    snprintf(buf, sizeof(buf), PHRASES_DEAUTH[idx], ap.c_str());
    
    // Append deauth count every 5th update
    if (deauthCount % 50 == 0 && deauthCount > 0) {
        String msg = String(buf) + " [" + String(deauthCount) + "]";
        currentPhrase = msg;
    } else {
        currentPhrase = buf;
    }
    lastPhraseChange = millis();
}

void Mood::onDeauthSuccess(const uint8_t* clientMac) {
    lastActivityTime = millis();
    happiness = min(happiness + 3, 100);  // Small permanent boost
    applyMomentumBoost(15);  // Temporary excitement
    
    // Award XP for successful deauth
    XP::addXP(XPEvent::DEAUTH_SUCCESS);
    
    // Format short MAC (last 2 bytes only for brevity)
    char macStr[8];
    snprintf(macStr, sizeof(macStr), "%02X%02X", clientMac[4], clientMac[5]);
    
    int idx = pickPhraseIdx(PhraseCategory::DEAUTH_SUCCESS, sizeof(PHRASES_DEAUTH_SUCCESS) / sizeof(PHRASES_DEAUTH_SUCCESS[0]));
    char buf[48];
    snprintf(buf, sizeof(buf), PHRASES_DEAUTH_SUCCESS[idx], macStr);
    currentPhrase = buf;
    lastPhraseChange = millis();
    
    // Quick beep for confirmed kick - non-blocking
    SFX::play(SFX::DEAUTH);
    
    // Force mood peek to show emotional reaction
    forceMoodPeek();
}

void Mood::onIdle() {
    int idx = pickPhraseIdx(PhraseCategory::MENU_IDLE, sizeof(PHRASES_MENU_IDLE) / sizeof(PHRASES_MENU_IDLE[0]));
    currentPhrase = PHRASES_MENU_IDLE[idx];
    lastPhraseChange = millis();
}

void Mood::onBored(uint16_t networkCount) {
    // Pig is bored - no valid targets to attack
    // Don't update lastActivityTime - we WANT to trigger idle effects
    
    // Set bored flag so updateAvatarState() shows SLEEPY instead of HUNTING
    isBoredState = true;
    
    // Decrease happiness slightly (but not too much)
    happiness = max(happiness - 1, -50);
    
    int idx = pickPhraseIdx(PhraseCategory::BORED, sizeof(PHRASES_BORED) / sizeof(PHRASES_BORED[0]));
    
    if (networkCount > 0) {
        // Networks exist but all exhausted/protected
        char buf[48];
        snprintf(buf, sizeof(buf), "%s (%d pwned)", PHRASES_BORED[idx], networkCount);
        currentPhrase = buf;
    } else {
        // No networks at all
        currentPhrase = PHRASES_BORED[idx];
    }
    lastPhraseChange = millis();
    
    // Set avatar to sleepy/bored state (will be maintained by updateAvatarState)
    Avatar::setState(AvatarState::SLEEPY);
}

void Mood::onWarhogUpdate() {
    lastActivityTime = millis();
    int idx = pickPhraseIdx(PhraseCategory::WARHOG, sizeof(PHRASES_WARHOG) / sizeof(PHRASES_WARHOG[0]));
    currentPhrase = PHRASES_WARHOG[idx];
    lastPhraseChange = millis();
}

void Mood::onWarhogFound(const char* apName, uint8_t channel) {
    (void)apName;  // Currently unused, phrases don't include AP name
    (void)channel; // Currently unused
    
    lastActivityTime = millis();
    happiness = min(100, happiness + 2);  // Small permanent boost
    applyMomentumBoost(8);  // Quick excitement for find
    
    // Sniff animation - found a truffle!
    Avatar::sniff();
    
    // XP awarded in warhog.cpp when network is logged (authoritative source)
    
    int idx = pickPhraseIdx(PhraseCategory::WARHOG_FOUND, sizeof(PHRASES_WARHOG_FOUND) / sizeof(PHRASES_WARHOG_FOUND[0]));
    currentPhrase = PHRASES_WARHOG_FOUND[idx];
    lastPhraseChange = millis();
}

// Static flag for BLE first-target sniff (reset via resetBLESniffState on mode start)
static bool bleFirstTargetSniffed = false;

void Mood::resetBLESniffState() {
    bleFirstTargetSniffed = false;
}

void Mood::onPiggyBluesUpdate(const char* vendor, int8_t rssi, uint8_t targetCount, uint8_t totalFound) {
    lastActivityTime = millis();
    happiness = min(100, happiness + 1);  // Tiny permanent boost
    applyMomentumBoost(5);  // Small excitement for spam activity
    
    // Sniff on first target acquisition only (not every update)
    if (vendor != nullptr && rssi != 0 && !bleFirstTargetSniffed) {
        Avatar::sniff();
        bleFirstTargetSniffed = true;
    }
    
    // Award XP for BLE spam (vendor-specific tracking for achievements)
    if (vendor != nullptr) {
        if (strcmp(vendor, "Apple") == 0) {
            XP::addXP(XPEvent::BLE_APPLE);
        } else if (strcmp(vendor, "Android") == 0) {
            XP::addXP(XPEvent::BLE_ANDROID);
        } else if (strcmp(vendor, "Samsung") == 0) {
            XP::addXP(XPEvent::BLE_SAMSUNG);
        } else if (strcmp(vendor, "Windows") == 0) {
            XP::addXP(XPEvent::BLE_WINDOWS);
        } else {
            XP::addXP(XPEvent::BLE_BURST);
        }
    } else {
        XP::addXP(XPEvent::BLE_BURST);
    }
    
    char buf[48];
    
    if (vendor != nullptr && rssi != 0) {
        // Targeted phrase with vendor info
        int idx = pickPhraseIdx(PhraseCategory::PIGGYBLUES_TARGETED, sizeof(PHRASES_PIGGYBLUES_TARGETED) / sizeof(PHRASES_PIGGYBLUES_TARGETED[0]));
        snprintf(buf, sizeof(buf), PHRASES_PIGGYBLUES_TARGETED[idx], vendor, rssi);
        currentPhrase = buf;
    } else if (targetCount > 0) {
        // Status phrase with target counts
        int idx = pickPhraseIdx(PhraseCategory::PIGGYBLUES_STATUS, sizeof(PHRASES_PIGGYBLUES_STATUS) / sizeof(PHRASES_PIGGYBLUES_STATUS[0]));
        snprintf(buf, sizeof(buf), PHRASES_PIGGYBLUES_STATUS[idx], targetCount, totalFound);
        currentPhrase = buf;
    } else {
        // Idle phrase
        int idx = pickPhraseIdx(PhraseCategory::PIGGYBLUES_IDLE, sizeof(PHRASES_PIGGYBLUES_IDLE) / sizeof(PHRASES_PIGGYBLUES_IDLE[0]));
        currentPhrase = PHRASES_PIGGYBLUES_IDLE[idx];
    }
    lastPhraseChange = millis();
}
