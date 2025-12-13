// Piglet mood implementation

#include "mood.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../core/porkchop.h"
#include "../ui/display.h"
#include "../modes/oink.h"
#include <Preferences.h>

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

// Mood momentum system
int Mood::momentumBoost = 0;
uint32_t Mood::lastBoostTime = 0;

// Phrase queue for chaining
String Mood::phraseQueue[3] = {"", "", ""};
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

// Force trigger a mood peek (for significant events like handshake capture)
static void forceMoodPeek() {
    moodPeekActive = true;
    moodPeekStartTime = millis();
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
    return constrain(happiness + momentumBoost, -100, 100);
}

uint32_t Mood::getLastActivityTime() {
    return lastActivityTime;
}

// --- Phase 6: Phrase Chaining ---
// Queue up to 3 phrases for sequential display

static const uint32_t PHRASE_CHAIN_DELAY_MS = 2000;  // 2 seconds between chain phrases

static void queuePhrase(const String& phrase) {
    if (Mood::phraseQueueCount < 3) {
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
    "$NET truffles found",
    "$HS handshakes ez",
    "lvl $LVL piggy",
    "$DEAUTH kicks today",
    "$NET and counting",
    "rank $LVL unlocked",
    "$HS pwnage counter",
    "$KM km of mud",
    "$NET sniffs so far",
    "bacon lvl $LVL",
    "$DEAUTH boot party"
};

static const int PHRASES_DYNAMIC_COUNT = sizeof(PHRASES_DYNAMIC) / sizeof(PHRASES_DYNAMIC[0]);

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
    DEAUTH, DEAUTH_SUCCESS, PMKID, SNIFFING, MENU_IDLE, RARE, DYNAMIC,
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

// Phrase categories
const char* PHRASES_HAPPY[] = {
    "snout pwns all",
    "oink oink oink",
    "got that truffle",
    "packets nom nom",
    "hog on a roll",
    "mud life best life",
    "truffle shuffle",
    "chaos tastes good"
};

const char* PHRASES_EXCITED[] = {
    "OINK OINK OINK",
    "pwned em good",
    "truffle in the bag",
    "gg no re",
    "snout goes brrrr",
    "0day buffet"
};

const char* PHRASES_HUNTING[] = {
    "snout to ground",
    "sniff n drift",
    "hunting truffles",
    "curious piggy",
    "diggin deep",
    "where da truffles"
};

// OINK mode quiet phrases - when hunting but finding nothing
const char* PHRASES_OINK_QUIET[] = {
    "ether is silent",
    "sniffing dead air",
    "no truffles here",
    "channels empty",
    "where the beacons at",
    "radio silence",
    "faraday cage?",
    "lonely spectrum",
    "snout finds nothing",
    "airwaves dry",
    "hunting ghosts",
    "802.11 void"
};

const char* PHRASES_SLEEPY[] = {
    "bored piggy",
    "null n void",
    "no truffles here",
    "/dev/null",
    "zzz oink zzz",
    "sleepy piggy"
};

const char* PHRASES_SAD[] = {
    "starving piggy",
    "404 no truffle",
    "lost n confused",
    "empty trough",
    "sad lil piggy",
    "need dem truffles"
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
    "802.11 wasteland"
};

// WARHOG wardriving phrases - GPS recon style
const char* PHRASES_WARHOG[] = {
    "mobile n hostile",
    "wardrive n thrive",
    "gps lock n stalk",
    "coordz on deck",
    "wigle submission",
    "kismet vibes",
    "lat/lon huntin",
    "ssid cartography",
    "loggin the ether",
    "nmea flowin",
    "ap geolocation",
    "drive-by recon",
    "mobile pcap unit"
};

const char* PHRASES_WARHOG_FOUND[] = {
    "truffle logged",
    "coords acquired",
    "ap triangulated",
    "wigle fodder",
    "geotagged loot",
    "lat/lon banked",
    "another pin dropped",
    "ssid catalogued"
};

// Piggy Blues BLE spam phrases - BLE attack style
// All phrases use %s=vendor and %d=rssi
const char* PHRASES_PIGGYBLUES_TARGETED[] = {
    "%s breached @ %ddB",
    "bluesnarf %s [%ddB]",
    "%s notif bombed %ddB",
    "gatt attack %s %ddB",
    "%s ble rekt @ %ddB",
    "2.4ghz assault %s %ddB",
    "uuid storm %s %ddB",
    "%s payload sent %ddB"
};

// Status phrases showing scan results - all use %d/%d format
const char* PHRASES_PIGGYBLUES_STATUS[] = {
    "%d marks [%d scanned]",
    "spammin %d/%d devices",
    "%d pwned, %d seen",
    "notif storm: %d of %d",
    "%d active targets [%d]"
};

// Idle/scanning phrases - BLE chaos style
const char* PHRASES_PIGGYBLUES_IDLE[] = {
    "ble chaos pending",
    "advert storm ready",
    "uuid payload loaded",
    "gatt attack standby",
    "notif nuke armed",
    "2.4ghz spectrum owned",
    "bluejack mode primed"
};

// Deauth success - 802.11 hacker rap style
const char* PHRASES_DEAUTH_SUCCESS[] = {
    "%s disassoc'd",
    "%s reason code 7",
    "%s frame dropped",
    "%s wifi canceled",
    "%s unauth'd lol",
    "%s ejected ez",
    "%s 802.11 rekt",
    "%s connection ded",
    "%s off my channel",
    "%s skill issue"
};

// PMKID captured - clientless hash extraction style
const char* PHRASES_PMKID_CAPTURED[] = {
    "pmkid yoinked",
    "clientless hash",
    "rsn ie pwned",
    "eapol-free loot",
    "passive extraction",
    "hashcat ready",
    "no client needed",
    "pmkid extracted",
    "silent pwn mode"
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
    "pwn responsibly"
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
    
    // Phase 6: Process phrase queue first
    if (phraseQueueCount > 0) {
        processQueue();
        updateAvatarState();
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
        selectPhrase();
        lastPhraseChange = now;
    }
    
    updateAvatarState();
}

void Mood::onHandshakeCaptured(const char* apName) {
    happiness = min(happiness + 10, 100);  // Smaller permanent boost
    applyMomentumBoost(30);  // Big temporary excitement!
    lastActivityTime = millis();
    
    // Sniff animation - caught something big!
    Avatar::sniff();
    
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
        int idx = pickPhraseIdx(PhraseCategory::EXCITED, sizeof(PHRASES_EXCITED) / sizeof(PHRASES_EXCITED[0]));
        strncpy(buf1, PHRASES_EXCITED[idx], sizeof(buf1) - 1);
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
    
    // Celebratory beep for handshake capture (higher pitch than deauth)
    if (Config::personality().soundEnabled) {
        M5.Speaker.tone(1500, 150);  // Distinctive handshake beep
    }
    
    // Force mood peek to show EXCITED face regardless of threshold
    forceMoodPeek();
}

void Mood::onPMKIDCaptured(const char* apName) {
    happiness = min(happiness + 15, 100);  // Slightly bigger permanent boost
    applyMomentumBoost(40);  // Even more temporary excitement!
    lastActivityTime = millis();
    
    // Sniff animation - stealthy capture!
    Avatar::sniff();
    
    // Award XP for PMKID capture (75 XP - more than handshake)
    XP::addXP(XPEvent::PMKID_CAPTURED);
    
    // Bonus XP for low battery clutch capture
    if (M5.Power.getBatteryLevel() < 10) {
        XP::addXP(XPEvent::LOW_BATTERY_CAPTURE);
    }
    
    // Phase 6: PMKID gets special 3-phrase chain
    char buf1[48], buf2[48], buf3[48];
    
    // First phrase - PMKID celebration
    int idx = pickPhraseIdx(PhraseCategory::PMKID, sizeof(PHRASES_PMKID_CAPTURED) / sizeof(PHRASES_PMKID_CAPTURED[0]));
    strncpy(buf1, PHRASES_PMKID_CAPTURED[idx], sizeof(buf1) - 1);
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
    
    // Triple beep for PMKID - it's special!
    if (Config::personality().soundEnabled) {
        M5.Speaker.tone(1800, 100);
        delay(120);
        M5.Speaker.tone(2000, 100);
        delay(120);
        M5.Speaker.tone(2200, 150);
    }
    
    // Auto-save the PMKID
    OinkMode::saveAllPMKIDs();
    
    // Force mood peek to show EXCITED face regardless of threshold
    forceMoodPeek();
}

void Mood::onNewNetwork(const char* apName, int8_t rssi, uint8_t channel) {
    happiness = min(happiness + 3, 100);  // Small permanent boost
    applyMomentumBoost(10);  // Quick excitement for network find
    lastActivityTime = millis();
    isBoredState = false;  // Clear bored state - found something!
    
    // Sniff animation - found a truffle!
    Avatar::sniff();
    
    // Award XP for network discovery
    if (apName && strlen(apName) > 0) {
        XP::addXP(XPEvent::NETWORK_FOUND);
    } else {
        // Hidden network gets bonus XP
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
    currentPhrase = msg;
    lastPhraseChange = millis();
}

void Mood::onMLPrediction(float confidence) {
    lastActivityTime = millis();
    
    // High confidence = happy
    if (confidence > 0.8f) {
        happiness = min(happiness + 15, 100);
        int idx = pickPhraseIdx(PhraseCategory::EXCITED, sizeof(PHRASES_EXCITED) / sizeof(PHRASES_EXCITED[0]));
        currentPhrase = PHRASES_EXCITED[idx];
    } else if (confidence > 0.5f) {
        happiness = min(happiness + 5, 100);
        int idx = pickPhraseIdx(PhraseCategory::HAPPY, sizeof(PHRASES_HAPPY) / sizeof(PHRASES_HAPPY[0]));
        currentPhrase = PHRASES_HAPPY[idx];
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
                int idx = pickPhraseIdx(PhraseCategory::SLEEPY, sizeof(PHRASES_SLEEPY) / sizeof(PHRASES_SLEEPY[0]));
                currentPhrase = PHRASES_SLEEPY[idx];
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
    
    int idx = pickPhraseIdx(PhraseCategory::SAD, sizeof(PHRASES_SAD) / sizeof(PHRASES_SAD[0]));
    currentPhrase = PHRASES_SAD[idx];
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
    
    // Use effective happiness (base + momentum) for phrase selection
    int effectiveMood = getEffectiveHappiness();
    
    // Phase 3: 5% chance for rare phrase (surprise variety)
    int specialRoll = random(0, 100);
    if (specialRoll < 5) {
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
    
    // High curiosity (>0.7) with activity can trigger excited phrases
    if (pers.curiosity > 0.7f && sess.networks > 5 && personalityRoll < (int)(pers.curiosity * 25)) {
        phrases = PHRASES_EXCITED;
        count = sizeof(PHRASES_EXCITED) / sizeof(PHRASES_EXCITED[0]);
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
        // Extremely happy - use excited phrases regardless of context
        phrases = PHRASES_EXCITED;
        count = sizeof(PHRASES_EXCITED) / sizeof(PHRASES_EXCITED[0]);
        cat = PhraseCategory::EXCITED;
    } else if (effectiveMood < -60 && bleedRoll < 30) {
        // Extremely sad - melancholy bleeds through
        phrases = PHRASES_SAD;
        count = sizeof(PHRASES_SAD) / sizeof(PHRASES_SAD[0]);
        cat = PhraseCategory::SAD;
    } else if (effectiveMood > 70) {
        // High happiness but not from handshake - use HAPPY not EXCITED
        // EXCITED phrases reserved for actual handshake captures
        phrases = PHRASES_HAPPY;
        count = sizeof(PHRASES_HAPPY) / sizeof(PHRASES_HAPPY[0]);
        cat = PhraseCategory::HAPPY;
    } else if (effectiveMood > 30) {
        phrases = PHRASES_HAPPY;
        count = sizeof(PHRASES_HAPPY) / sizeof(PHRASES_HAPPY[0]);
        cat = PhraseCategory::HAPPY;
    } else if (effectiveMood > -10) {
        phrases = PHRASES_HUNTING;
        count = sizeof(PHRASES_HUNTING) / sizeof(PHRASES_HUNTING[0]);
        cat = PhraseCategory::HUNTING;
    } else if (effectiveMood > -50) {
        phrases = PHRASES_SLEEPY;
        count = sizeof(PHRASES_SLEEPY) / sizeof(PHRASES_SLEEPY[0]);
        cat = PhraseCategory::SLEEPY;
    } else {
        phrases = PHRASES_SAD;
        count = sizeof(PHRASES_SAD) / sizeof(PHRASES_SAD[0]);
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
    // Hide bubble during walk transition
    if (Avatar::isTransitioning()) {
        return;  // Pig is walking, no speech bubble
    }
    
    // Calculate bubble size based on ACTUAL word-wrapped line count
    // Optimized: 16 chars/line (fits 99px text area), 5 lines max (down to grass at y=73)
    String phrase = currentPhrase;
    phrase.toUpperCase();  // UPPERCASE for visibility
    int maxCharsPerLine = 16;  // Fits within available width
    
    // First pass: simulate word wrap to count actual lines needed
    int numLines = 0;
    String simRemaining = phrase;
    while (simRemaining.length() > 0 && numLines < 5) {
        if ((int)simRemaining.length() <= maxCharsPerLine) {
            simRemaining = "";
        } else {
            int splitPos = simRemaining.lastIndexOf(' ', maxCharsPerLine);
            if (splitPos < 1) {
                splitPos = simRemaining.indexOf(' ', maxCharsPerLine);
                if (splitPos < 0) {
                    splitPos = maxCharsPerLine;
                }
            }
            simRemaining = (splitPos < (int)simRemaining.length()) ? simRemaining.substring(splitPos + 1) : "";
        }
        numLines++;
    }
    if (numLines == 0) numLines = 1;  // At least 1 line
    
    // Position bubble based on pig SCREEN POSITION (not facing direction)
    // Pig on left side = bubble on right
    // Pig on right side = bubble on left
    bool pigOnRight = Avatar::isOnRightSide();
    int bubbleW = 116;  // Fixed bubble width
    int bubbleX = pigOnRight ? 4 : 120;  // Left side if pig is on right, right side if pig is on left
    int bubbleY = 3;
    int lineHeight = 11;
    int bubbleH = 8 + (numLines * lineHeight);  // Padding + actual lines
    
    // Cap bubble height to fit above grass (y=73)
    if (bubbleH > 70) bubbleH = 70;
    
    // Draw filled bubble with pink background
    canvas.fillRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 6, COLOR_FG);
    
    // Draw filled triangle arrow pointing to piglet (comic-style speech bubble tail)
    // Arrow direction depends on bubble position
    int arrowTipY = bubbleY + bubbleH / 2;    // Vertically centered on bubble
    int arrowTopY = arrowTipY - 5;
    int arrowBottomY = arrowTipY + 5;
    
    if (pigOnRight) {
        // Pig on right, bubble on left, arrow points RIGHT toward pig
        int arrowTipX = bubbleX + bubbleW + 8;
        int arrowBaseX = bubbleX + bubbleW;
        canvas.fillTriangle(arrowTipX, arrowTipY, arrowBaseX, arrowTopY, arrowBaseX, arrowBottomY, COLOR_FG);
    } else {
        // Pig on left, bubble on right, arrow points LEFT toward pig
        int arrowTipX = bubbleX - 8;
        int arrowBaseX = bubbleX;
        canvas.fillTriangle(arrowTipX, arrowTipY, arrowBaseX, arrowTopY, arrowBaseX, arrowBottomY, COLOR_FG);
    }
    
    // Draw phrase inside bubble with word wrapping - BLACK text on pink
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(COLOR_BG);  // Black text
    
    int textX = bubbleX + 5;
    int textY = bubbleY + 4;
    
    // Second pass: actually render the text
    String remaining = phrase;
    int lineNum = 0;
    while (remaining.length() > 0 && lineNum < 5) {
        String line;
        if ((int)remaining.length() <= maxCharsPerLine) {
            line = remaining;
            remaining = "";
        } else {
            // Try to find space before limit (but not at position 0)
            int splitPos = remaining.lastIndexOf(' ', maxCharsPerLine);
            if (splitPos < 1) {
                // No usable space before limit - search forward for next space
                splitPos = remaining.indexOf(' ', maxCharsPerLine);
                if (splitPos < 0) {
                    // No space at all - hard break at limit
                    splitPos = maxCharsPerLine;
                }
            }
            line = remaining.substring(0, splitPos);
            remaining = (splitPos < (int)remaining.length()) ? remaining.substring(splitPos + 1) : "";
        }
        canvas.drawString(line, textX, textY + lineNum * lineHeight);
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

// Deauth/digging phrases - 802.11 hacker rap style
const char* PHRASES_DEAUTH[] = {
    "droppin frames on %s",
    "802.11 slayin %s",
    "disassoc %s hard",
    "mgmt frame rain %s",
    "reason code 7 %s",
    "snout checkin %s",
    "kickin %s off turf",
    "deauth flow on %s"
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
    "idle paws...",
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

void Mood::onDeauthing(const char* apName, uint32_t deauthCount) {
    lastActivityTime = millis();
    
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
    
    // Quick beep for confirmed kick
    if (Config::personality().soundEnabled) {
        M5.Speaker.tone(800, 50);
    }
    
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
    
    // Award XP for WARHOG network logged with GPS
    XP::addXP(XPEvent::WARHOG_LOGGED);
    
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

